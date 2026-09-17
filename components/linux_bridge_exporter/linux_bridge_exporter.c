/*
 * See linux_bridge_exporter.h for the full design summary. Implementation
 * notes:
 *
 * Event flow (correction pass): TOPOLOGY events (observer_on_topology_applied/
 * observer_on_node_offline) still go through a small bounded FreeRTOS FIFO
 * queue (s_topology_queue) -- topology changes are comparatively rare, and
 * the Linux ingest side's own generation/sequence ordering check
 * (linux/matter_bridge/ingest's Registry::apply_topology()) makes a
 * FIFO-drained, possibly-briefly-stale intermediate state harmless (a stale
 * snapshot is rejected there, never applied out of order). SAMPLE events
 * are DIFFERENT: they previously went through the same FIFO, which meant a
 * burst of samples for the same module_uuid could pile up multiple stale
 * copies ahead of the newest one, and a full queue would drop whichever one
 * happened to arrive last -- sometimes the newest. SAMPLE events now go
 * through s_pending_samples, a small bounded table keyed by module_uuid
 * that ALWAYS COALESCES to the single latest sample per uuid (an upsert,
 * never an append) -- there is structurally no way for an older sample to
 * be sent after (or instead of) a newer one for the same uuid, and no
 * "replay a backlog of stale samples" scenario is possible at all.
 *
 * Both power_base_transport's bridge-observer callbacks
 * (observer_on_sample_stored/observer_on_topology_applied/
 * observer_on_node_offline below) run from within the OpenThread task's own
 * context (same as every otCoapAddResource handler in this codebase) and do
 * ONLY a strictly non-blocking enqueue/upsert -- never a CoAP send, and
 * every xQueueSend()/xSemaphoreTake() call site they reach uses a 0-tick
 * wait (correction pass: upsert_pending_sample()'s mutex take used to
 * block up to 20 ticks, which this pass removed -- see that function's own
 * comment). A 0-tick miss (the exporter task happens to hold the mutex, or
 * the topology queue is momentarily full) simply drops that one event with
 * a rate-limited warning; it is never retried in a loop and never handed
 * to a new task/state machine. exporter_task() (this component's own,
 * dedicated task) is the ONLY thing that ever actually sends network
 * traffic, drains both the topology queue and the pending-sample table, and
 * runs the periodic timers: a full TOPOLOGY resync, a full latest-SAMPLE
 * resync, and a HEARTBEAT, all currently sharing
 * CONFIG_LINUX_BRIDGE_EXPORTER_FULL_RESYNC_INTERVAL_MS /
 * CONFIG_LINUX_BRIDGE_EXPORTER_HEARTBEAT_INTERVAL_MS. The periodic full
 * resyncs read straight from the REAL sources of truth
 * (power_base_transport_get_node()/sensor_root_aggregator_get_node()), so a
 * Linux OTBR host that was down or just restarted converges to the CURRENT
 * state within one resync interval regardless of anything dropped earlier --
 * this component never waits for "the next sensor measurement" to recover.
 *
 * Overflow/drop policy (this follow-up's own explicit requirement: "queue
 * overflow 시 시스템을 멈추지 말고 최신 상태가 이후 snapshot으로 복구되게 할
 * 것"): a dropped/coalesced-away SAMPLE is never a source-of-truth loss --
 * the Power Base's own sensor_root_aggregator already has the latest value
 * regardless, and the periodic full sample resync re-sends it. A dropped
 * TOPOLOGY event is likewise recovered by the periodic full topology
 * resync. Either way, dropping never blocks or crashes anything -- just one
 * rate-limited WARN log line.
 *
 * CoAP client send state (token/staleness guard): identical pattern to
 * sensor_transport.c's/topology_reporter.c's own s_send_done_sem/
 * s_send_token -- ONE semaphore for this task's whole lifetime (this task
 * is also the only caller of transmit(), so there is no cross-task race on
 * the token the way sensor_transport.c has against its OWN periodic caller,
 * but the same stale-callback guard is kept anyway: a late callback for an
 * attempt this task's own bounded wait already gave up on must still never
 * corrupt a LATER attempt's result).
 *
 * Real-hardware finding (this follow-up's correction pass, stale TOPOLOGY
 * backlog): while Linux ingest was down, real Power Base topology events
 * kept piling up in s_topology_queue; on restart, the periodic full
 * TOPOLOGY resync applied the CURRENT (newest) state, but the FIFO then
 * went on to drain its stale backlog of OLDER snapshots for the same
 * uuid(s) right after -- each one rejected by Linux ingest's own
 * generation/sequence check as CoAP 4.12 stale_session, which `transmit()`
 * used to retry 3 times like any other failure, producing pointless
 * retries/log noise for a rejection that was never going to succeed no
 * matter how many times the same stale payload was resent. Fixed two ways:
 *
 *   1. The queue itself no longer carries a topology SNAPSHOT at all --
 *      only a module_uuid (topo_evt_payload_t is now just that). Whatever
 *      state a uuid was in when its event was enqueued is irrelevant by
 *      the time exporter_task() actually drains it; handle_topology_event()
 *      always re-reads that uuid's CURRENT entry via
 *      power_base_transport_get_node() right before sending, so the wire
 *      payload is always the current generation/last_seq/online/depth/
 *      downstream state, never a backlog snapshot. A uuid no longer present
 *      is simply skipped (ESP_LOGD) -- the periodic resync wouldn't find it
 *      either. Duplicate-uuid backlog entries are additionally coalesced
 *      away before ever reaching the queue (see s_topology_pending_* below)
 *      so a burst of changes for one uuid can't crowd out a different
 *      uuid's event out of the bounded queue.
 *   2. transmit()/send_one_attempt() now return an explicit
 *      export_send_result_t instead of a bare bool: a 4.xx response (Linux
 *      explicitly parsed and rejected the request) is EXPORT_SEND_REJECTED
 *      and returned immediately, never retried -- retrying an unchanged
 *      payload against an explicit rejection can only ever get rejected
 *      again. Only EXPORT_SEND_TRANSIENT_FAILURE (timeout/no ACK/OT send
 *      error/5.xx) still gets the existing bounded retry+backoff. This also
 *      stops a normal 4.xx rejection from being mis-logged as "delivery
 *      unconfirmed" (that phrase is now reserved for an actually-exhausted
 *      transient-failure retry budget).
 *
 * Per-uuid SAMPLE export rate limit (this pass -- transport-load reduction,
 * Power Base -> Linux -> Matter controller data path): drain_pending_samples()
 * now additionally gates each slot on its own next_send_allowed_tick (default
 * CONFIG_LINUX_BRIDGE_EXPORTER_SAMPLE_MIN_INTERVAL_MS = 1000ms between two
 * actual SAMPLE sends for the SAME module_uuid). This is a pure send-frequency
 * cap layered on top of the pre-existing coalescing table -- sensor
 * measurement period, POGO POLL_CONFIG, and Power Base aggregator storage
 * rate are completely unchanged; s_pending_samples already always coalesces
 * to the single latest (seq, value) per uuid (upsert, never append), so a
 * rate-limited slot simply waits, still holding only ever the latest sample,
 * until its window reopens -- never a FIFO replay of older values. TOPOLOGY
 * events (this file's own s_topology_queue/s_topology_pending_*, ATTACHED/
 * UPDATED/DETACHED/OFFLINE and parent-state changes) are completely
 * unaffected by this gate -- they are still sent as soon as
 * exporter_task()'s own queue drain reaches them, same as before this pass.
 * The periodic full TOPOLOGY+SAMPLE resync (do_full_topology_resync()/
 * do_full_sample_resync(), CONFIG_LINUX_BRIDGE_EXPORTER_FULL_RESYNC_INTERVAL_MS,
 * far longer than this gate by default) also bypasses this per-uuid gate
 * entirely and unconditionally re-sends every node's current state -- it is
 * a distinct, already-existing recovery mechanism, not something this pass
 * needed to change. Existing CoAP retry policy (2.xx success, 4.xx never
 * retried, timeout/OT-error/5.xx bounded retry) is completely unchanged by
 * this gate -- rate limiting only decides WHEN a send is dispatched, never
 * how a dispatched send's own outcome is handled.
 */
#include "linux_bridge_exporter/linux_bridge_exporter.h"

#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "openthread/coap.h"
#include "openthread/error.h"

#include "bridge_export_proto/bridge_export_proto.h"
#include "sensor_channel/sensor_channel.h"
#include "sensor_root_aggregator/sensor_root_aggregator.h"
#include "coap_response_timing/coap_response_timing.h"

static const char *TAG = "linux_bridge_exporter";

#define OT_LOCK_WAIT pdMS_TO_TICKS(1000)
#define URI_SAMPLE "pogo/v1/sample"
#define URI_TOPOLOGY "pogo/v1/topology"
#define URI_HEARTBEAT "pogo/v1/heartbeat"

/* 1 initial send + 2 retries -- bounded, lightweight (a single fixed Linux
 * destination, not a multi-hop mesh needing the UUID-hash backoff spread
 * sensor_transport.c/topology_reporter.c use for radio contention). */
#define EXPORTER_MAX_RETRIES 2
#define EXPORTER_MAX_ATTEMPTS (1 + EXPORTER_MAX_RETRIES)
static const uint32_t s_retry_backoff_ms[EXPORTER_MAX_ATTEMPTS] = {0, 200, 500};

/* Shared with sensor_transport.c/topology_reporter.c -- ONE Kconfig source
 * of truth, per this follow-up's own requirement ("CoAP timing은 기존
 * coap_response_timing을 재사용"). */
#define EXPORTER_ACK_TIMEOUT_MS CONFIG_SENSOR_COAP_ACK_TIMEOUT_MS

/*
 * Explicit per-attempt outcome (correction pass, replacing a bare bool --
 * see this file's own top comment): a 4.xx response is semantically
 * DIFFERENT from a timeout/OT-send-error/5.xx -- Linux ingest received and
 * explicitly parsed the request and rejected it, so resending the exact
 * same payload can only ever be rejected again, whereas the other failure
 * kinds are genuinely worth a bounded retry.
 */
typedef enum {
    EXPORT_SEND_DELIVERED_2XX,      /* 2.xx -- success, stop immediately. */
    EXPORT_SEND_REJECTED_4XX,       /* 4.xx -- explicit rejection, stop immediately, never retried. */
    EXPORT_SEND_TRANSIENT_FAILURE,  /* timeout/no ACK/OT send error/5.xx -- bounded retry applies. */
} export_send_result_t;

#define EXPORTER_POLL_TICK_MS 200
/* A send failure is logged at most this often, regardless of how many
 * individual sample/topology/heartbeat sends fail in between -- never a
 * per-event flood while the Linux host is simply down for a while. */
#define SEND_FAIL_WARN_INTERVAL_MS 30000

/*
 * Correction pass (stale TOPOLOGY backlog -- see this file's own top
 * comment): carries ONLY a module_uuid, never a topology_registry_entry_t
 * snapshot -- exporter_task() always resolves the CURRENT entry for this
 * uuid via power_base_transport_get_node() at drain time (handle_topology_event()),
 * so a backlog of stale queued events can never cause an outdated
 * generation/last_seq/online/depth/downstream state to be sent, regardless
 * of how long it sat in the queue.
 */
typedef struct {
    uint8_t uuid[TOPOLOGY_UUID_LEN];
} topo_evt_payload_t;

/*
 * Bounded set of module_uuids currently enqueued in s_topology_queue, used
 * only to COALESCE duplicate-uuid TOPOLOGY events before they ever reach
 * xQueueSend() -- since handle_topology_event() always re-reads the CURRENT
 * registry entry for a uuid at drain time regardless of which of that
 * uuid's queued events is being drained, a second event for a uuid already
 * pending would just cause the same current state to be sent twice; skipping
 * the duplicate instead saves a queue slot for some OTHER uuid's event
 * during a burst (e.g. a flapping link) -- see this file's own top comment.
 * Sized to TOPOLOGY_REGISTRY_MAX_NODES, the true upper bound on how many
 * distinct module_uuids the registry (and therefore this set) can ever need
 * to track at once, so this can never need to reject a legitimate new uuid
 * the registry itself would still have room for (same sizing rationale as
 * s_pending_samples/PENDING_SAMPLES_MAX above).
 *
 * Touched from BOTH the OT task (producer, via observer_on_topology_applied/
 * observer_on_node_offline) and the exporter task (consumer, draining
 * s_topology_queue) -- protected by a short critical section
 * (s_topology_pending_lock), the same cheap-spinlock pattern this file
 * already uses for s_send_lock, never a blocking wait: the OT-task callback
 * must never block (see this file's own top comment).
 */
#define TOPOLOGY_PENDING_MAX TOPOLOGY_REGISTRY_MAX_NODES
static portMUX_TYPE s_topology_pending_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_topology_pending_valid[TOPOLOGY_PENDING_MAX];
static uint8_t s_topology_pending_uuids[TOPOLOGY_PENDING_MAX][TOPOLOGY_UUID_LEN];

/*
 * Bounded, uuid-keyed, always-coalescing table of samples still waiting to
 * be (re-)exported -- see this file's own top comment for why SAMPLE events
 * no longer share the FIFO topology queue. Sized to
 * SENSOR_ROOT_AGGREGATOR_MAX_NODES: that is the true upper bound on how
 * many distinct module_uuids the aggregator (this table's own data source)
 * can ever hold, so this can never need to reject a legitimate uuid the
 * aggregator itself would still have room for.
 */
#define PENDING_SAMPLES_MAX SENSOR_ROOT_AGGREGATOR_MAX_NODES

typedef struct {
    bool valid;  /* this slot has ever held a (module_uuid, sample) pair */
    bool dirty;  /* has NOT been exported since the last upsert -- needs (re-)sending */
    uint8_t module_uuid[SENSOR_UUID_LEN];
    sensor_sample_t sample;
    /*
     * Per-uuid SAMPLE export rate limit (this pass -- transport-load
     * reduction, Power Base -> Linux): the earliest tick at which the NEXT
     * SAMPLE export for this uuid may actually be sent. Zero-initialized,
     * meaning "no prior send -- allowed immediately" (see
     * drain_pending_samples()'s wrap-safe comparison below; tick 0 is
     * indistinguishable from "never sent" only at the very first instant
     * after boot, which is itself still correctly "allowed"). Stamped to
     * `now + CONFIG_LINUX_BRIDGE_EXPORTER_SAMPLE_MIN_INTERVAL_MS` the moment
     * a send for this uuid is actually DISPATCHED (not when it completes),
     * so a slow bounded-retry send cannot itself be immediately followed by
     * another one. This caps SEND FREQUENCY only -- sensor measurement,
     * Power Base aggregator storage, and registry/dirty tracking above are
     * all still driven at their existing rates; only how often the
     * already-current latest value is mirrored to Linux is limited.
     */
    TickType_t next_send_allowed_tick;
} pending_sample_slot_t;

static uint8_t s_power_base_uuid[TOPOLOGY_UUID_LEN];
static uint8_t s_host_addr[POWER_BASE_BRIDGE_CONFIG_ADDR_LEN];
static uint16_t s_host_port;
static uint32_t s_start_tick_ms;
static QueueHandle_t s_topology_queue;
static SemaphoreHandle_t s_pending_samples_mutex;
static pending_sample_slot_t s_pending_samples[PENDING_SAMPLES_MAX];
static TickType_t s_last_send_fail_warn_tick;

/* See this file's own top comment: same token-staleness guard pattern as
 * sensor_transport.c/topology_reporter.c. */
static SemaphoreHandle_t s_send_done_sem;
static portMUX_TYPE s_send_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_send_token;
static otError s_send_result;
static otCoapCode s_send_response_code;
static bool s_send_got_message;

static void warn_rate_limited(const char *msg)
{
    TickType_t now = xTaskGetTickCount();
    if (s_last_send_fail_warn_tick == 0 || (now - s_last_send_fail_warn_tick) > pdMS_TO_TICKS(SEND_FAIL_WARN_INTERVAL_MS)) {
        s_last_send_fail_warn_tick = now;
        ESP_LOGW(TAG, "%s", msg);
    }
}

static void coap_response_handler(void *ctx_v, otMessage *message, const otMessageInfo *message_info, otError result)
{
    (void)message_info;
    uint32_t my_token = (uint32_t)(uintptr_t)ctx_v;
    bool stale;

    taskENTER_CRITICAL(&s_send_lock);
    stale = (my_token != s_send_token);
    if (!stale) {
        s_send_result = result;
        s_send_got_message = false;
        if (result == OT_ERROR_NONE && message != NULL) {
            s_send_response_code = otCoapMessageGetCode(message);
            s_send_got_message = true;
        }
    }
    taskEXIT_CRITICAL(&s_send_lock);

    if (!stale) {
        xSemaphoreGive(s_send_done_sem);
    }
}

/* One Confirmable POST attempt to the configured Linux host/port. Mirrors
 * sensor_transport.c's send_one_attempt() exactly, generalized over
 * uri_path/payload and a fixed (not collector_endpoint-derived) destination,
 * and (this follow-up's correction pass) returning an explicit
 * export_send_result_t instead of a bare bool -- see this file's own top
 * comment and export_send_result_t's own doc comment. */
static export_send_result_t send_one_attempt(const char *uri_path, const uint8_t *payload, size_t len)
{
    otIp6Address dest_addr;
    memcpy(dest_addr.mFields.m8, s_host_addr, sizeof(dest_addr.mFields.m8));

    uint32_t my_token;
    taskENTER_CRITICAL(&s_send_lock);
    my_token = ++s_send_token;
    s_send_got_message = false;
    taskEXIT_CRITICAL(&s_send_lock);
    xSemaphoreTake(s_send_done_sem, 0); /* drain any stale "give" -- see coap_response_handler()'s own comment */

    otInstance *instance = esp_openthread_get_instance();
    otError err = OT_ERROR_NO_BUFS;
    if (esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        otMessage *message = otCoapNewMessage(instance, NULL);
        if (message != NULL) {
            otCoapMessageInit(message, OT_COAP_TYPE_CONFIRMABLE, OT_COAP_CODE_POST);
            err = otCoapMessageAppendUriPathOptions(message, uri_path);
            if (err == OT_ERROR_NONE) {
                err = otCoapMessageSetPayloadMarker(message);
            }
            if (err == OT_ERROR_NONE) {
                err = otMessageAppend(message, payload, (uint16_t)len);
            }
            if (err == OT_ERROR_NONE) {
                otMessageInfo info = {0};
                info.mPeerAddr = dest_addr;
                info.mPeerPort = s_host_port;
                otCoapTxParameters tx = {
                    .mAckTimeout = EXPORTER_ACK_TIMEOUT_MS,
                    .mAckRandomFactorNumerator = COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_NUM,
                    .mAckRandomFactorDenominator = COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_DEN,
                    .mMaxRetransmit = 0, /* this component's own retry loop below owns retries */
                };
                err = otCoapSendRequestWithParameters(instance, message, &info, coap_response_handler,
                                                       (void *)(uintptr_t)my_token, &tx);
            }
            if (err != OT_ERROR_NONE) {
                otMessageFree(message);
            }
        }
        esp_openthread_lock_release();
    } else {
        err = OT_ERROR_BUSY;
    }

    if (err != OT_ERROR_NONE) {
        ESP_LOGD(TAG, "failed to send bridge-export CoAP request to '%s': %s", uri_path, otThreadErrorToString(err));
        return EXPORT_SEND_TRANSIENT_FAILURE;
    }

    uint32_t wait_ms = coap_response_timing_wait_ms(EXPORTER_ACK_TIMEOUT_MS, COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_NUM,
                                                      COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_DEN, EXPORTER_ACK_TIMEOUT_MS);
    if (xSemaphoreTake(s_send_done_sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        return EXPORT_SEND_TRANSIENT_FAILURE; /* no ACK within the derived wait -- timeout */
    }
    if (s_send_result != OT_ERROR_NONE || !s_send_got_message) {
        return EXPORT_SEND_TRANSIENT_FAILURE; /* OT-layer send/ack error */
    }
    uint8_t code_class = (uint8_t)(s_send_response_code >> 5);
    if (code_class == 2) {
        return EXPORT_SEND_DELIVERED_2XX;
    }
    if (code_class == 4) {
        return EXPORT_SEND_REJECTED_4XX;
    }
    return EXPORT_SEND_TRANSIENT_FAILURE; /* 5.xx (or any other unexpected class) */
}

/* Bounded 1+EXPORTER_MAX_RETRIES attempts with a small fixed backoff -- see
 * this file's own top comment for why a per-module hash schedule (the
 * mesh-radio-contention design sensor_transport.c/topology_reporter.c use)
 * is not needed for a single fixed Linux destination.
 *
 * Retry policy (correction pass -- see this file's own top comment):
 * EXPORT_SEND_DELIVERED_2XX and EXPORT_SEND_REJECTED_4XX both stop
 * immediately (a 4.xx is a final, explicit outcome from Linux ingest, never
 * worth retrying the same payload against); only EXPORT_SEND_TRANSIENT_FAILURE
 * consumes the bounded retry budget below. */
static export_send_result_t transmit(const char *uri_path, const uint8_t *payload, size_t len)
{
    export_send_result_t result = EXPORT_SEND_TRANSIENT_FAILURE;
    for (int attempt = 0; attempt < EXPORTER_MAX_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(s_retry_backoff_ms[attempt]));
        }
        result = send_one_attempt(uri_path, payload, len);
        if (result == EXPORT_SEND_DELIVERED_2XX) {
            ESP_LOGD(TAG, "bridge export sent OK ('%s', attempt=%d/%d)", uri_path, attempt + 1,
                      EXPORTER_MAX_ATTEMPTS);
            return result;
        }
        if (result == EXPORT_SEND_REJECTED_4XX) {
            /* Final, explicit rejection -- see this function's own doc
             * comment. Never falls through to the retry backoff below. */
            return result;
        }
        /* EXPORT_SEND_TRANSIENT_FAILURE: fall through and retry (bounded). */
    }
    return result;
}

/*
 * Logs the outcome of a transmit() call appropriately for `what` (e.g.
 * "topology"/"sample"/"heartbeat") -- shared by send_topology_entry()/
 * handle_sample_event()/send_heartbeat() so all three follow the identical
 * policy from this file's own top comment: EXPORT_SEND_DELIVERED_2XX is
 * silent (routine success, already logged at DEBUG by transmit() itself);
 * EXPORT_SEND_REJECTED_4XX -- an explicit, FINAL outcome from Linux ingest,
 * e.g. a 4.12 stale_session rejecting a backlog snapshot -- is logged at
 * DEBUG, never the same "delivery unconfirmed" WARN a genuine transient
 * failure gets (this follow-up's own explicit requirement: a normal 4.xx
 * must never be mis-reported as unconfirmed); only an exhausted
 * EXPORT_SEND_TRANSIENT_FAILURE retry budget gets the rate-limited WARN.
 */
static void report_export_result(export_send_result_t result, const char *what, const char *unconfirmed_msg)
{
    switch (result) {
    case EXPORT_SEND_DELIVERED_2XX:
        break;
    case EXPORT_SEND_REJECTED_4XX:
        ESP_LOGD(TAG, "%s export rejected by Linux host (4.xx) -- not retried (the periodic full resync will "
                      "re-derive and resend the CURRENT state)", what);
        break;
    case EXPORT_SEND_TRANSIENT_FAILURE:
    default:
        warn_rate_limited(unconfirmed_msg);
        break;
    }
}

static void send_topology_entry(const topology_registry_entry_t *e)
{
    bridge_export_topology_node_t node = {0};
    memcpy(node.power_base_uuid, s_power_base_uuid, BRIDGE_EXPORT_UUID_LEN);
    memcpy(node.module_uuid, e->module_uuid, BRIDGE_EXPORT_UUID_LEN);
    memcpy(node.parent_uuid, e->parent_uuid, BRIDGE_EXPORT_UUID_LEN);
    node.online = e->online;
    node.depth = e->depth;
    node.role = e->role;
    node.sensor_bitmap = e->sensor_bitmap;
    node.downstream_connected = e->downstream_connected;
    node.boot_session_generation = e->boot_session_generation;
    node.last_seq = e->last_seq;

    uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
    size_t wlen = bridge_export_pack_topology_node(&node, wire, sizeof(wire));
    if (wlen == 0) {
        ESP_LOGW(TAG, "failed to pack a topology export message -- dropped");
        return;
    }
    report_export_result(transmit(URI_TOPOLOGY, wire, wlen), "topology",
                          "topology export delivery unconfirmed (Linux OTBR host unreachable? -- the topology "
                          "registry itself is completely unaffected, and the next periodic full resync will "
                          "retry every node's current state)");
}

static void handle_sample_event(const sensor_sample_t *sample)
{
    bridge_export_sample_t exp = {0};
    memcpy(exp.power_base_uuid, s_power_base_uuid, BRIDGE_EXPORT_UUID_LEN);
    uint8_t sc_buf[SENSOR_CHANNEL_MAX_WIRE_LEN];
    size_t sc_len = sensor_channel_pack(sample, sc_buf, sizeof(sc_buf));
    if (sc_len == 0) {
        ESP_LOGW(TAG, "failed to re-pack a sample for export (channel_count=%u) -- dropped", sample->channel_count);
        return;
    }
    memcpy(exp.sensor_channel_payload, sc_buf, sc_len);
    exp.sensor_channel_len = (uint16_t)sc_len;

    uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
    size_t wlen = bridge_export_pack_sample(&exp, wire, sizeof(wire));
    if (wlen == 0) {
        ESP_LOGW(TAG, "failed to pack a sample export message -- dropped");
        return;
    }
    report_export_result(transmit(URI_SAMPLE, wire, wlen), "sample",
                          "sample export delivery unconfirmed (Linux OTBR host unreachable? -- the sensor sample "
                          "itself is already stored in the Power Base aggregator regardless)");
}

/* Linear scan over the registry for `uuid`'s CURRENT entry -- the only
 * lookup power_base_transport.h exposes (index-based get_node()/
 * get_node_count(), no lookup-by-uuid accessor). Bounded by
 * TOPOLOGY_REGISTRY_MAX_NODES (32) either way. */
static bool find_current_topology_entry(const uint8_t uuid[TOPOLOGY_UUID_LEN], topology_registry_entry_t *out)
{
    size_t count = power_base_transport_get_node_count();
    for (size_t i = 0; i < count; i++) {
        topology_registry_entry_t e;
        if (power_base_transport_get_node(i, &e) && memcmp(e.module_uuid, uuid, TOPOLOGY_UUID_LEN) == 0) {
            *out = e;
            return true;
        }
    }
    return false;
}

/* Returns true (and marks `uuid` pending) iff a fresh xQueueSend() for this
 * uuid should proceed -- false means `uuid` is already pending (a queued
 * event for it will still be drained, always against its CURRENT registry
 * state, so this duplicate is redundant -- see s_topology_pending_* 's own
 * doc comment). A short, bounded critical section, never a blocking wait --
 * called only from the OT-task's own observer callbacks below. */
static bool topology_pending_try_mark(const uint8_t uuid[TOPOLOGY_UUID_LEN])
{
    bool should_enqueue = true;
    int free_slot = -1;
    taskENTER_CRITICAL(&s_topology_pending_lock);
    for (int i = 0; i < TOPOLOGY_PENDING_MAX; i++) {
        if (s_topology_pending_valid[i] && memcmp(s_topology_pending_uuids[i], uuid, TOPOLOGY_UUID_LEN) == 0) {
            should_enqueue = false;
            break;
        }
        if (!s_topology_pending_valid[i] && free_slot < 0) {
            free_slot = i;
        }
    }
    /* free_slot < 0 here (the set is full) cannot happen in practice -- see
     * TOPOLOGY_PENDING_MAX's own sizing comment -- but if it ever did, fail
     * OPEN (still enqueue) rather than silently and permanently refusing to
     * ever mark this uuid pending again. */
    if (should_enqueue && free_slot >= 0) {
        s_topology_pending_valid[free_slot] = true;
        memcpy(s_topology_pending_uuids[free_slot], uuid, TOPOLOGY_UUID_LEN);
    }
    taskEXIT_CRITICAL(&s_topology_pending_lock);
    return should_enqueue;
}

/* Clears `uuid`'s pending mark, if any -- called once a queued event for it
 * has actually been dequeued (exporter_task()) or once an enqueue attempt
 * for it failed (enqueue_topology_event() below), either way freeing it to
 * be marked pending again by a future event. */
static void topology_pending_unmark(const uint8_t uuid[TOPOLOGY_UUID_LEN])
{
    taskENTER_CRITICAL(&s_topology_pending_lock);
    for (int i = 0; i < TOPOLOGY_PENDING_MAX; i++) {
        if (s_topology_pending_valid[i] && memcmp(s_topology_pending_uuids[i], uuid, TOPOLOGY_UUID_LEN) == 0) {
            s_topology_pending_valid[i] = false;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_topology_pending_lock);
}

/* Shared by observer_on_topology_applied()/observer_on_node_offline() --
 * both enqueue nothing but a uuid now (see topo_evt_payload_t's own doc
 * comment), coalesced via s_topology_pending_* before ever reaching
 * xQueueSend(). Still strictly non-blocking (xQueueSend(..., 0)) -- never
 * anything that could block the calling OT task (this file's own top
 * comment). */
static void enqueue_topology_event(const uint8_t uuid[TOPOLOGY_UUID_LEN], const char *drop_log_msg)
{
    if (!topology_pending_try_mark(uuid)) {
        return; /* a fresh event for this uuid is already queued -- coalesced away */
    }
    topo_evt_payload_t evt;
    memcpy(evt.uuid, uuid, TOPOLOGY_UUID_LEN);
    if (xQueueSend(s_topology_queue, &evt, 0) != pdTRUE) {
        topology_pending_unmark(uuid); /* enqueue failed -- let a future event for this uuid try again */
        warn_rate_limited(drop_log_msg);
    }
}

/* Correction pass (stale TOPOLOGY backlog -- see this file's own top
 * comment): `p` only ever carries a uuid now, regardless of whether it came
 * from on_topology_applied or on_node_offline -- the CURRENT registry entry
 * is always re-resolved right here, in this task's own context (never
 * inside the OT callback), so a queue event that sat behind other work for
 * a while can never cause a stale generation/last_seq/online/depth/
 * downstream snapshot to be sent. A uuid no longer found (should not happen
 * -- registry slots are never reused for a different uuid) is simply
 * skipped; the periodic full resync would not find it either. */
static void handle_topology_event(const topo_evt_payload_t *p)
{
    topology_registry_entry_t e;
    if (find_current_topology_entry(p->uuid, &e)) {
        send_topology_entry(&e);
        return;
    }
    ESP_LOGD(TAG, "topology export event for a uuid no longer present in the registry -- skipped");
}

static void do_full_topology_resync(void)
{
    size_t count = power_base_transport_get_node_count();
    for (size_t i = 0; i < count; i++) {
        topology_registry_entry_t e;
        if (power_base_transport_get_node(i, &e)) {
            send_topology_entry(&e);
        }
    }
}

/*
 * Re-sends every module's CURRENT last-known sample straight from
 * sensor_root_aggregator (the real source of truth), regardless of the
 * pending-sample table's own state -- this is what lets a Linux OTBR host
 * that just (re)started converge on the latest value within one resync
 * interval instead of waiting for that module's next sensor measurement
 * (this follow-up's own explicit requirement).
 */
static void do_full_sample_resync(void)
{
    size_t count = sensor_root_aggregator_get_node_count();
    for (size_t i = 0; i < count; i++) {
        sensor_root_aggregator_node_t node;
        if (sensor_root_aggregator_get_node(i, &node) && node.has_sample) {
            handle_sample_event(&node.last_sample);
        }
    }
}

/*
 * Upserts `sample` into s_pending_samples, coalescing by module_uuid: a
 * newer sample for a uuid already pending simply overwrites it (dirty stays
 * true) -- there is never more than one pending export per uuid, so this
 * table can never accumulate or later replay a backlog of stale samples
 * (see this file's own top comment). Called from the OT task's own context
 * (an observer callback), which must NEVER block (correction pass): the
 * mutex take below uses a 0-tick (non-blocking) wait -- if the exporter
 * task happens to be holding s_pending_samples_mutex at that exact instant
 * (drain_pending_samples()/linux_bridge_exporter_init()'s own brief
 * critical sections), this reactive upsert is simply dropped with a
 * rate-limited warning, never retried in a busy loop and never handed off
 * to a new task/state machine -- the periodic full sample resync
 * (do_full_sample_resync(), reading sensor_root_aggregator directly) is the
 * sole recovery path for a dropped upsert, same as every other
 * best-effort drop in this component (see this file's own top comment on
 * the overall overflow/drop policy).
 */
static void upsert_pending_sample(const sensor_sample_t *sample)
{
    if (xSemaphoreTake(s_pending_samples_mutex, 0) != pdTRUE) {
        warn_rate_limited("pending-sample table busy -- dropped one reactive SAMPLE export event (the periodic "
                           "full sample resync still delivers the current value)");
        return;
    }
    int slot = -1;
    int free_slot = -1;
    for (int i = 0; i < PENDING_SAMPLES_MAX; i++) {
        if (s_pending_samples[i].valid &&
            memcmp(s_pending_samples[i].module_uuid, sample->module_uuid, SENSOR_UUID_LEN) == 0) {
            slot = i;
            break;
        }
        if (!s_pending_samples[i].valid && free_slot < 0) {
            free_slot = i;
        }
    }
    if (slot < 0) {
        if (free_slot < 0) {
            xSemaphoreGive(s_pending_samples_mutex);
            warn_rate_limited("pending-sample table full -- dropped one reactive SAMPLE export event for a new "
                               "uuid (the periodic full sample resync still delivers it)");
            return;
        }
        slot = free_slot;
        s_pending_samples[slot].valid = true;
        memcpy(s_pending_samples[slot].module_uuid, sample->module_uuid, SENSOR_UUID_LEN);
    }
    s_pending_samples[slot].sample = *sample; /* coalesce: always the latest wins */
    s_pending_samples[slot].dirty = true;
    xSemaphoreGive(s_pending_samples_mutex);
}

/*
 * Sends every currently-dirty pending sample that is ALSO past its own
 * per-uuid rate-limit window (clearing dirty and re-stamping
 * next_send_allowed_tick as it goes), and returns. Called once per
 * exporter_task() loop iteration -- bounded (PENDING_SAMPLES_MAX slots),
 * never blocks on anything but its own short mutex per slot.
 *
 * A dirty slot still inside its rate-limit window is left exactly as-is
 * (dirty stays true, sample untouched) -- it is neither sent now nor
 * dropped; the NEXT loop iteration that finds it both dirty and past its
 * window sends it, always with whatever is the CURRENT (possibly further
 * updated by upsert_pending_sample() in the meantime) sample content at
 * that moment. This is the same "latest wins, no FIFO replay" coalescing
 * behavior this table already had, now also gated on time -- a rate-limited
 * slot is never a source-of-truth loss (the aggregator already has the
 * value, and the periodic full sample resync -- itself independent of this
 * per-uuid gate -- converges Linux regardless).
 *
 * Wrap-safe comparison (this pass): `(int32_t)(now - deadline) >= 0`, the
 * same idiom already used elsewhere in this codebase (e.g.
 * components/pogo_port/pogo_port.c) for a TickType_t (unsigned, wraps) that
 * must still compare correctly across a wraparound boundary.
 */
static void drain_pending_samples(void)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t interval_ticks = pdMS_TO_TICKS(CONFIG_LINUX_BRIDGE_EXPORTER_SAMPLE_MIN_INTERVAL_MS);
    for (int i = 0; i < PENDING_SAMPLES_MAX; i++) {
        sensor_sample_t sample;
        bool need_send = false;
        if (xSemaphoreTake(s_pending_samples_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (s_pending_samples[i].valid && s_pending_samples[i].dirty &&
                (int32_t)(now - s_pending_samples[i].next_send_allowed_tick) >= 0) {
                sample = s_pending_samples[i].sample;
                s_pending_samples[i].dirty = false;
                s_pending_samples[i].next_send_allowed_tick = now + interval_ticks;
                need_send = true;
            }
            xSemaphoreGive(s_pending_samples_mutex);
        }
        if (need_send) {
            handle_sample_event(&sample);
        }
    }
}

static void send_heartbeat(void)
{
    bridge_export_heartbeat_t hb = {0};
    memcpy(hb.power_base_uuid, s_power_base_uuid, BRIDGE_EXPORT_UUID_LEN);
    hb.uptime_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - s_start_tick_ms;
    hb.node_count = (uint16_t)power_base_transport_get_node_count();

    uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
    size_t wlen = bridge_export_pack_heartbeat(&hb, wire, sizeof(wire));
    if (wlen == 0) {
        return;
    }
    export_send_result_t result = transmit(URI_HEARTBEAT, wire, wlen);
    if (result == EXPORT_SEND_DELIVERED_2XX) {
        /* Routine success -- DEBUG only, never floods the log (this
         * follow-up's own requirement: "정상 heartbeat 성공 로그는 DEBUG
         * 이하"). */
        ESP_LOGD(TAG, "heartbeat sent OK (node_count=%u)", (unsigned)hb.node_count);
        return;
    }
    report_export_result(result, "heartbeat", "heartbeat export delivery unconfirmed (Linux OTBR host unreachable?)");
}

static void exporter_task(void *arg)
{
    (void)arg;
    TickType_t last_resync = xTaskGetTickCount();
    TickType_t last_heartbeat = xTaskGetTickCount();

    while (1) {
        topo_evt_payload_t evt;
        if (xQueueReceive(s_topology_queue, &evt, pdMS_TO_TICKS(EXPORTER_POLL_TICK_MS)) == pdTRUE) {
            /* This event is no longer "pending" from the coalescing set's
             * point of view as of right now -- a NEW on_topology_applied/
             * on_node_offline for this same uuid arriving while
             * handle_topology_event() below is still in flight (e.g.
             * retrying a transient failure) is correctly treated as a fresh
             * event to enqueue, not folded into the one already being
             * processed. */
            topology_pending_unmark(evt.uuid);
            handle_topology_event(&evt);
        }
        /* Runs every loop iteration regardless of whether a topology event
         * arrived -- bounded (PENDING_SAMPLES_MAX slots), so this never
         * meaningfully lengthens the loop when nothing is dirty. */
        drain_pending_samples();

        TickType_t now = xTaskGetTickCount();
        if ((now - last_resync) >= pdMS_TO_TICKS(CONFIG_LINUX_BRIDGE_EXPORTER_FULL_RESYNC_INTERVAL_MS)) {
            do_full_topology_resync();
            do_full_sample_resync();
            last_resync = now;
        }
        if ((now - last_heartbeat) >= pdMS_TO_TICKS(CONFIG_LINUX_BRIDGE_EXPORTER_HEARTBEAT_INTERVAL_MS)) {
            send_heartbeat();
            last_heartbeat = now;
        }
    }
}

/* ---- power_base_transport bridge-observer callbacks: OT-task context,
 * non-blocking enqueue/upsert only -- see this file's own top comment. ---- */

static void observer_on_sample_stored(const sensor_sample_t *sample, void *ctx)
{
    (void)ctx;
    upsert_pending_sample(sample);
}

static void observer_on_topology_applied(const topology_registry_entry_t *entry, void *ctx)
{
    (void)ctx;
    enqueue_topology_event(entry->module_uuid,
                            "topology queue full -- dropped one TOPOLOGY export event (the next periodic full "
                            "resync recovers it)");
}

static void observer_on_node_offline(const uint8_t uuid[TOPOLOGY_UUID_LEN], void *ctx)
{
    (void)ctx;
    enqueue_topology_event(uuid, "topology queue full -- dropped one TOPOLOGY (offline) export event (the next "
                                   "periodic full resync recovers it)");
}

esp_err_t linux_bridge_exporter_init(const uint8_t power_base_uuid[TOPOLOGY_UUID_LEN],
                                      const power_base_bridge_config_t *cfg)
{
    if (power_base_uuid == NULL || cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cfg->enabled) {
        ESP_LOGI(TAG, "bridge exporter config is disabled -- not starting (defensive no-op; the caller should "
                      "prefer not calling this at all in that case)");
        return ESP_OK;
    }

    memcpy(s_power_base_uuid, power_base_uuid, TOPOLOGY_UUID_LEN);
    memcpy(s_host_addr, cfg->host_addr, POWER_BASE_BRIDGE_CONFIG_ADDR_LEN);
    s_host_port = cfg->port;
    s_start_tick_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_send_token = 0;
    s_last_send_fail_warn_tick = 0;
    memset(s_pending_samples, 0, sizeof(s_pending_samples));
    memset(s_topology_pending_valid, 0, sizeof(s_topology_pending_valid));

    s_send_done_sem = xSemaphoreCreateBinary();
    if (s_send_done_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_pending_samples_mutex = xSemaphoreCreateMutex();
    if (s_pending_samples_mutex == NULL) {
        vSemaphoreDelete(s_send_done_sem);
        s_send_done_sem = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_topology_queue = xQueueCreate(CONFIG_LINUX_BRIDGE_EXPORTER_QUEUE_LEN, sizeof(topo_evt_payload_t));
    if (s_topology_queue == NULL) {
        vSemaphoreDelete(s_pending_samples_mutex);
        s_pending_samples_mutex = NULL;
        vSemaphoreDelete(s_send_done_sem);
        s_send_done_sem = NULL;
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreate(exporter_task, "linux_bridge_exporter", 4096, NULL, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed -- the bridge exporter will not run this boot (every other subsystem is "
                      "unaffected)");
        vQueueDelete(s_topology_queue);
        s_topology_queue = NULL;
        vSemaphoreDelete(s_pending_samples_mutex);
        s_pending_samples_mutex = NULL;
        vSemaphoreDelete(s_send_done_sem);
        s_send_done_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    power_base_transport_bridge_observer_t observer = {
        .ctx = NULL,
        .on_sample_stored = observer_on_sample_stored,
        .on_topology_applied = observer_on_topology_applied,
        .on_node_offline = observer_on_node_offline,
    };
    power_base_transport_set_bridge_observer(&observer);

    ESP_LOGI(TAG, "linux bridge exporter ready: resources='/%s','/%s','/%s' port=%u topology_queue_len=%d "
                  "pending_samples_max=%d resync_interval_ms=%u heartbeat_interval_ms=%u "
                  "sample_min_interval_ms=%u (host address never logged)",
             URI_SAMPLE, URI_TOPOLOGY, URI_HEARTBEAT, (unsigned)s_host_port, CONFIG_LINUX_BRIDGE_EXPORTER_QUEUE_LEN,
             PENDING_SAMPLES_MAX, (unsigned)CONFIG_LINUX_BRIDGE_EXPORTER_FULL_RESYNC_INTERVAL_MS,
             (unsigned)CONFIG_LINUX_BRIDGE_EXPORTER_HEARTBEAT_INTERVAL_MS,
             (unsigned)CONFIG_LINUX_BRIDGE_EXPORTER_SAMPLE_MIN_INTERVAL_MS);
    return ESP_OK;
}
