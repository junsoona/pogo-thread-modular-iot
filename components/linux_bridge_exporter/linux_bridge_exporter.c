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
 * ONLY a strictly non-blocking enqueue/mark-pending -- never a CoAP send,
 * and every xQueueSend()/xSemaphoreTake() call site they reach uses a
 * 0-tick wait when called from the OT task (a prior correction pass's own
 * fix -- see mark_sample_pending()'s own doc comment for exactly which
 * callers get a 0-tick wait vs. a small bounded one). A 0-tick miss (the
 * exporter task happens to hold the mutex, or
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
 * state -- TOPOLOGY within one resync interval, SAMPLE within one resync
 * interval plus at most one more SAMPLE_MIN_INTERVAL_MS window (see the
 * "Unified SAMPLE scheduler" paragraph further down) -- regardless of
 * anything dropped earlier. This component never waits for "the next
 * sensor measurement" to recover.
 *
 * Overflow/drop policy (this follow-up's own explicit requirement: "queue
 * overflow 시 시스템을 멈추지 말고 최신 상태가 이후 snapshot으로 복구되게 할
 * 것"): a dropped/coalesced-away SAMPLE is never a source-of-truth loss --
 * the Power Base's own sensor_root_aggregator already has the latest value
 * regardless, and the periodic full sample resync marks it pending again
 * (see below). A dropped TOPOLOGY event is likewise recovered by the
 * periodic full topology resync. Either way, dropping never blocks or
 * crashes anything -- just one rate-limited WARN log line.
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
 * gates each slot on its own next_send_allowed_tick (default
 * CONFIG_LINUX_BRIDGE_EXPORTER_SAMPLE_MIN_INTERVAL_MS = 1000ms between two
 * SAMPLE PUBLISHES for the SAME module_uuid -- see that Kconfig option's own
 * help text for the precise unit: one LOGICAL PUBLISH, i.e. one whole bounded
 * transmit_sample() attempt sequence, never one individual CoAP attempt).
 * This is a pure publish-frequency cap -- sensor measurement period, POGO
 * POLL_CONFIG, and Power Base aggregator storage rate are completely
 * unchanged. TOPOLOGY events (this file's own s_topology_queue/
 * s_topology_pending_*, ATTACHED/UPDATED/DETACHED/OFFLINE and parent-state
 * changes) are a completely separate path and are NOT subject to this gate
 * at all -- they are still sent as soon as exporter_task()'s own queue drain
 * reaches them, including the periodic full TOPOLOGY resync.
 *
 * Unified SAMPLE scheduler (correction pass, fixing two real-hardware-
 * relevant bugs found in code review of the rate-limit pass above -- see
 * the two paragraphs below for each one): EVERY SAMPLE publish path --
 * the reactive OT-task callback (observer_on_sample_stored()) AND the
 * periodic full sample resync (do_full_sample_resync()) -- now goes through
 * the exact SAME s_pending_samples table and the exact same
 * next_send_allowed_tick gate in drain_pending_samples(). Neither path ever
 * dispatches a SAMPLE directly anymore; both only ever call
 * mark_sample_pending() to mark a uuid's slot dirty (do_full_sample_resync()
 * marks every currently-known aggregator uuid dirty every resync tick,
 * exactly like the reactive callback does for one uuid on a real sample).
 * This is what makes "at most one SAMPLE publish per uuid per interval"
 * actually true end to end: a reactive publish immediately followed by a
 * resync tick can never itself produce a second publish inside the same
 * window, because the resync tick only marks pending -- it is
 * drain_pending_samples() alone that ever decides WHEN a publish actually
 * goes out, for either origin. s_pending_samples slots no longer cache a
 * sample VALUE at all (see pending_sample_slot_t's own comment) -- only
 * "this uuid has a pending change", because the value actually published is
 * always re-read fresh from sensor_root_aggregator at publish time (see
 * transmit_sample() below), never a value captured back when it was marked
 * dirty.
 *
 * Per-attempt fresh payload (correction pass, fixes real-hardware-adjacent
 * bug found in code review: transmit()'s old fixed-payload retry loop could
 * resend a STALE sample_seq on attempt 2/3 if a newer sample for the same
 * uuid arrived while attempt 1 was still in flight): transmit_sample() below
 * re-reads sensor_root_aggregator's CURRENT sample for the target uuid and
 * re-packs the wire payload immediately before EVERY attempt, including the
 * very first one -- never a cached/precomputed buffer reused across retries.
 * If all EXPORTER_MAX_ATTEMPTS attempts hit only transient failures,
 * handle_sample_event() marks the uuid pending again (mark_sample_pending())
 * so the NEXT time its rate-limit window allows a publish, whatever is
 * CURRENT in the aggregator at THAT later moment is sent -- bounded by the
 * gate, never a busy retry loop, never a replay of the failed attempt's
 * stale value. A 4.xx rejection is NOT re-marked pending here (unchanged
 * existing policy -- see EXPORT_SEND_REJECTED_4XX's own doc comment): the
 * next periodic resync still re-marks it regardless, same as before.
 *
 * Existing CoAP retry policy (2.xx success, 4.xx never retried,
 * timeout/OT-error/5.xx bounded retry) is completely unchanged by any of
 * this -- rate limiting and the unified scheduler only decide WHEN and WITH
 * WHAT PAYLOAD a send is dispatched, never how a dispatched send's own
 * outcome is handled.
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
#include "removed_journal.h"

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
 *
 * `kind` (this follow-up: POGO auto-registration/removal lifecycle)
 * distinguishes an ordinary topology re-sync (SNAPSHOT -- re-resolved
 * against the CURRENT registry entry at drain time, exactly as above) from
 * a confirmed-detach tombstone (REMOVED -- see
 * power_base_transport_bridge_observer_t::on_node_removed and
 * BRIDGE_EXPORT_MSG_TOPOLOGY_REMOVED's own doc comment). A REMOVED event
 * carries no registry entry to re-resolve at all -- the removal is the
 * whole fact being conveyed, independent of whatever nodes_/registry state
 * this uuid is (or is not) in by drain time -- so it must never be
 * reinterpreted as a SNAPSHOT re-sync.
 */
typedef enum {
    TOPOLOGY_EVT_SNAPSHOT = 0,
    TOPOLOGY_EVT_REMOVED = 1,
} topology_evt_kind_t;

typedef struct {
    uint8_t uuid[TOPOLOGY_UUID_LEN];
    topology_evt_kind_t kind;
} topo_evt_payload_t;

/*
 * Bounded set of (module_uuid, kind) pairs currently enqueued in
 * s_topology_queue, used only to COALESCE duplicate (uuid, kind) TOPOLOGY
 * events before they ever reach xQueueSend() -- since handle_topology_event()
 * always re-reads the CURRENT registry entry for a SNAPSHOT uuid at drain
 * time regardless of which of that uuid's queued SNAPSHOT events is being
 * drained, a second SNAPSHOT event for a uuid already pending would just
 * cause the same current state to be sent twice; skipping the duplicate
 * instead saves a queue slot for some OTHER uuid's event during a burst
 * (e.g. a flapping link) -- see this file's own top comment. Keyed by
 * (uuid, kind) rather than uuid alone (this follow-up) so a pending REMOVED
 * tombstone for a uuid can never coalesce away (or be coalesced away by) an
 * unrelated, independently-meaningful SNAPSHOT event for that SAME uuid --
 * e.g. a removal immediately followed by a genuine re-attach SNAPSHOT for
 * the same module_uuid must both still reach the queue, in that same
 * chronological order (a single FIFO queue preserves that order; see
 * exporter_task()'s own single-consumer drain loop). Sized to
 * TOPOLOGY_REGISTRY_MAX_NODES, the true upper bound on how many distinct
 * module_uuids the registry (and therefore this set) can ever need to track
 * at once per kind, so this can never need to reject a legitimate new uuid
 * the registry itself would still have room for (same sizing rationale as
 * s_pending_samples/PENDING_SAMPLES_MAX above).
 *
 * Touched from BOTH the OT task (producer, via observer_on_topology_applied/
 * observer_on_node_offline/observer_on_node_removed) and the exporter task
 * (consumer, draining s_topology_queue) -- protected by a short critical
 * section (s_topology_pending_lock), the same cheap-spinlock pattern this
 * file already uses for s_send_lock, never a blocking wait: the OT-task
 * callback must never block (see this file's own top comment).
 */
#define TOPOLOGY_PENDING_MAX TOPOLOGY_REGISTRY_MAX_NODES
static portMUX_TYPE s_topology_pending_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_topology_pending_valid[TOPOLOGY_PENDING_MAX];
static uint8_t s_topology_pending_uuids[TOPOLOGY_PENDING_MAX][TOPOLOGY_UUID_LEN];
static topology_evt_kind_t s_topology_pending_kind[TOPOLOGY_PENDING_MAX];

/*
 * Bounded, uuid-keyed set of REMOVED tombstones not yet CONFIRMED delivered
 * (a 2.xx response) -- the retry counterpart to s_topology_pending_*
 * above (this follow-up). Unlike an ordinary SNAPSHOT (whose "current
 * state" to resend always still exists, for free, in
 * power_base_transport's own topology_registry -- see
 * do_full_topology_resync()), a REMOVED tombstone conveys a one-shot fact:
 * the confirmed detach already ERASED the uuid from topology_registry, so
 * nothing else remembers that Linux still has to be told. This table is
 * that fact, and it is mirrored to flash (removed_journal.c, written by
 * persist_removed_journal_if_dirty() from the exporter task whenever
 * s_pending_removed_dirty is set) and reloaded by
 * linux_bridge_exporter_init() -- so a removal that never reached Linux
 * survives a Power Base reboot instead of leaving the module on Linux
 * forever (where its own stale timeout could only ever mark it offline).
 * do_full_removed_resync() below
 * re-enqueues every entry still marked here on every periodic resync tick,
 * so a removal that failed to deliver (Linux host unreachable, exhausted
 * retry budget) keeps being retried indefinitely -- not just the initial
 * 1+EXPORTER_MAX_RETRIES burst -- until Linux actually confirms it (see
 * handle_removed_event()'s own doc comment), giving the SAME "periodic
 * resync converges regardless of any single delivery failure" guarantee
 * every other export path in this file already has. Sized to
 * TOPOLOGY_REGISTRY_MAX_NODES for the same reason as every other bounded
 * table in this file.
 */
#define PENDING_REMOVED_MAX TOPOLOGY_REGISTRY_MAX_NODES
static portMUX_TYPE s_pending_removed_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_pending_removed_valid[PENDING_REMOVED_MAX];
static uint8_t s_pending_removed_uuids[PENDING_REMOVED_MAX][TOPOLOGY_UUID_LEN];
/* Outcome of the last delivery attempt per slot, only so a failure is
 * logged when it CHANGES rather than on every resync retry: 0 = not
 * attempted yet, REMOVED_OUTCOME_NO_RESPONSE, else the CoAP response code. */
#define REMOVED_OUTCOME_NO_RESPONSE 0xFFu
static uint8_t s_pending_removed_last_outcome[PENDING_REMOVED_MAX];
/* The set above differs from what removed_journal last committed. */
static bool s_pending_removed_dirty;

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

/*
 * Correction pass: no longer caches a sensor_sample_t value at all -- this
 * slot only ever tracks "this uuid has a pending SAMPLE publish", never a
 * captured value. The value actually published is always re-read fresh
 * from sensor_root_aggregator at the moment transmit_sample() dispatches
 * (and again before every retry) -- see this file's own top comment on why
 * caching a value here was exactly the bug this pass fixes (a stale
 * sample_seq could otherwise be resent on a later retry attempt).
 */
typedef struct {
    bool valid;  /* this slot has ever been marked pending for this module_uuid */
    bool dirty;  /* a SAMPLE publish for this uuid has NOT happened since the last mark -- needs (re-)publishing */
    uint8_t module_uuid[SENSOR_UUID_LEN];
    /*
     * Per-uuid SAMPLE publish rate limit (this pass -- transport-load
     * reduction, Power Base -> Linux): the earliest tick at which the NEXT
     * SAMPLE publish for this uuid may actually be dispatched. Zero-
     * initialized, meaning "no prior publish -- allowed immediately" (see
     * drain_pending_samples()'s wrap-safe comparison below; tick 0 is
     * indistinguishable from "never published" only at the very first
     * instant after boot, which is itself still correctly "allowed").
     * Stamped to `now + CONFIG_LINUX_BRIDGE_EXPORTER_SAMPLE_MIN_INTERVAL_MS`
     * the moment a publish for this uuid is actually DISPATCHED (not when
     * it completes), so one whole bounded transmit_sample() attempt
     * sequence -- 1 initial attempt + up to 2 transient-failure retries --
     * counts as exactly ONE publish against this gate, never one per
     * individual CoAP attempt (see the Kconfig option's own help text).
     * This caps PUBLISH FREQUENCY only -- sensor measurement, Power Base
     * aggregator storage, and dirty-marking above are all still driven at
     * their existing rates/immediately; only how often the already-current
     * latest value is mirrored to Linux is limited.
     */
    TickType_t next_send_allowed_tick;
} pending_sample_slot_t;

/* Set once by linux_bridge_exporter_suspend() and never cleared: the board
 * is being unenrolled, nothing more is exported in this boot. */
static volatile bool s_suspended;

static uint8_t s_power_base_uuid[TOPOLOGY_UUID_LEN];
/*
 * Where requests go. Written at init and by
 * linux_bridge_exporter_set_destination() (another task), read by the
 * exporter task for every attempt -- all three under s_dest_lock.
 * s_dest_generation goes up with every change: an attempt remembers the
 * number it was sent under, and an answer that comes back under another
 * number is not that destination's (see send_one_attempt()).
 * s_resync_requested asks the exporter task to give the (new) destination
 * everything again on its next pass.
 */
static portMUX_TYPE s_dest_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_host_addr[POWER_BASE_BRIDGE_CONFIG_ADDR_LEN];
static uint16_t s_host_port;
static uint32_t s_dest_generation;
static bool s_resync_requested;
static volatile bool s_started;
static uint32_t s_start_tick_ms;
static QueueHandle_t s_topology_queue;
static SemaphoreHandle_t s_pending_samples_mutex;
static pending_sample_slot_t s_pending_samples[PENDING_SAMPLES_MAX];
static TickType_t s_last_send_fail_warn_tick;

/* Correction pass (this follow-up, item 6): TEST-ONLY clock override -- see
 * linux_bridge_exporter_test_set_clock()'s own doc comment. NULL (the
 * default) means "use the real xTaskGetTickCount()"; production code never
 * calls the setter, so exporter_now_ms() is byte-for-byte
 * xTaskGetTickCount() in every real build. Used ONLY for exporter_task()'s
 * own full-resync/heartbeat interval comparisons -- deliberately NOT for
 * drain_pending_samples()'s per-uuid SAMPLE_MIN_INTERVAL_MS rate-limit gate
 * (a separate, much shorter timer existing tests exercise opening and
 * closing over genuinely elapsed real time by design -- see that
 * function's own comment), and never for send timeouts or retry backoff
 * (real blocking waits, not simple elapsed-time comparisons). */
static TickType_t (*s_test_clock_fn)(void) = NULL;

static TickType_t exporter_now_ms(void)
{
    return (s_test_clock_fn != NULL) ? s_test_clock_fn() : xTaskGetTickCount();
}

void linux_bridge_exporter_test_set_clock(TickType_t (*now_ms_fn)(void))
{
    s_test_clock_fn = now_ms_fn;
}

esp_err_t linux_bridge_exporter_set_destination(const uint8_t host_addr[POWER_BASE_BRIDGE_CONFIG_ADDR_LEN],
                                                 uint16_t port)
{
    if (host_addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    /* What power_base_bridge_config refuses for an enabled exporter:
     * unspecified, multicast (ff00::/8), link-local (fe80::/10), port 0. */
    static const uint8_t unspecified[POWER_BASE_BRIDGE_CONFIG_ADDR_LEN] = {0};
    if (port == 0 || memcmp(host_addr, unspecified, sizeof(unspecified)) == 0 || host_addr[0] == 0xff ||
        (host_addr[0] == 0xfe && (host_addr[1] & 0xc0) == 0x80)) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_dest_lock);
    const bool changed = s_host_port != port || memcmp(s_host_addr, host_addr, POWER_BASE_BRIDGE_CONFIG_ADDR_LEN) != 0;
    if (changed) {
        memcpy(s_host_addr, host_addr, POWER_BASE_BRIDGE_CONFIG_ADDR_LEN);
        s_host_port = port;
        s_dest_generation++;
        s_resync_requested = true;
    }
    taskEXIT_CRITICAL(&s_dest_lock);
    if (changed) {
        ESP_LOGW(TAG, "destination changed: port=%u (host address never logged) -- full resync requested",
                 (unsigned)port);
    }
    return ESP_OK;
}

void linux_bridge_exporter_suspend(void)
{
    s_suspended = true;
}

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
static void persist_removed_journal_if_dirty(void);

static export_send_result_t send_one_attempt(const char *uri_path, const uint8_t *payload, size_t len)
{
    /* Every CoAP attempt, not only once per exporter_task() iteration: with
     * Linux unreachable one iteration (e.g. a full resync of every node,
     * each exhausting its timeouts/backoff) can block for many seconds, and
     * a detach confirmed meanwhile must not stay RAM-only that long. This
     * bounds it to one attempt plus one backoff. */
    persist_removed_journal_if_dirty();

    otIp6Address dest_addr;
    uint16_t dest_port;
    uint32_t dest_generation;
    taskENTER_CRITICAL(&s_dest_lock);
    memcpy(dest_addr.mFields.m8, s_host_addr, sizeof(dest_addr.mFields.m8));
    dest_port = s_host_port;
    dest_generation = s_dest_generation;
    taskEXIT_CRITICAL(&s_dest_lock);

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
                info.mPeerPort = dest_port;
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
    taskENTER_CRITICAL(&s_dest_lock);
    const bool destination_changed = dest_generation != s_dest_generation;
    taskEXIT_CRITICAL(&s_dest_lock);
    if (destination_changed) {
        /* Whatever was answered, it was answered by the endpoint this
         * exporter has left since: not a delivery to the hub's current one.
         * The caller's retry goes to the new endpoint. */
        return EXPORT_SEND_TRANSIENT_FAILURE;
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

/* Forward declarations -- defined further down, but handle_removed_event()
 * right below needs both first. */
static bool clear_removed_pending(const uint8_t uuid[TOPOLOGY_UUID_LEN]);
static uint8_t swap_removed_outcome(const uint8_t uuid[TOPOLOGY_UUID_LEN], uint8_t outcome);
static bool find_current_topology_entry(const uint8_t uuid[TOPOLOGY_UUID_LEN], topology_registry_entry_t *out);

/* Sends one BRIDGE_EXPORT_MSG_TOPOLOGY_REMOVED tombstone for module_uuid (see
 * that message type's own wire-format doc comment) to the SAME
 * URI_TOPOLOGY resource an ordinary SNAPSHOT uses -- msg_type inside the
 * envelope, not the URI, is what Linux ingest's own handle_request()
 * dispatches on (see linux/matter_bridge/ingest/src/session.cpp). Clears
 * this uuid's s_pending_removed mark ONLY on a confirmed 2.xx delivery --
 * see s_pending_removed_*'s own doc comment: any other outcome leaves it
 * pending for do_full_removed_resync() to retry, since resending an
 * already-applied removal is always a harmless, idempotent no-op on the
 * Linux side (registry.hpp's own apply_removal() doc comment) -- unlike an
 * ordinary SNAPSHOT's 4.12 stale_session, there is no "this payload can
 * only ever be rejected again" case to short-circuit here. */
static void handle_removed_event(const uint8_t module_uuid[TOPOLOGY_UUID_LEN])
{
    /* Correction pass (this follow-up, item 2: rapid detach/reattach race)
     * -- the AUTHORITATIVE backstop, re-checked at the exact moment of
     * drain/send, regardless of how this event reached here (the reactive
     * fast path, or a later do_full_removed_resync() retry) and regardless
     * of whether observer_on_topology_applied()'s own proactive cancel
     * (above) already fired for it. If module_uuid is CURRENTLY findable
     * in the Power Base's own topology_registry, a genuine re-attach has
     * happened since this REMOVED event was produced -- sending the
     * tombstone now would be actively WRONG (it would delete the
     * just-reattached module on Linux), not merely redundant. Skipped, and
     * the (by now definitely stale) pending mark is cleared too so
     * do_full_removed_resync() never retries it again either. */
    topology_registry_entry_t current;
    if (find_current_topology_entry(module_uuid, &current)) {
        if (clear_removed_pending(module_uuid)) {
            ESP_LOGI(TAG, "removal ..%02x%02x: tombstone cancelled before sending -- the module is registered "
                          "again (re-attached)",
                     module_uuid[6], module_uuid[7]);
        }
        return;
    }

    bridge_export_topology_removed_t rem = {0};
    memcpy(rem.power_base_uuid, s_power_base_uuid, BRIDGE_EXPORT_UUID_LEN);
    memcpy(rem.module_uuid, module_uuid, BRIDGE_EXPORT_UUID_LEN);

    uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
    size_t wlen = bridge_export_pack_topology_removed(&rem, wire, sizeof(wire));
    if (wlen == 0) {
        ESP_LOGW(TAG, "failed to pack a topology-removed export message -- dropped (still pending -- the next "
                      "periodic resync retries it)");
        return;
    }
    export_send_result_t result = transmit(URI_TOPOLOGY, wire, wlen);

    uint8_t code = REMOVED_OUTCOME_NO_RESPONSE;
    taskENTER_CRITICAL(&s_send_lock);
    if (s_send_got_message) {
        code = (uint8_t)s_send_response_code;
    }
    taskEXIT_CRITICAL(&s_send_lock);

    /* Logged per uuid, but only on a transition: delivery (once), or a
     * failure whose outcome differs from the previous attempt's -- never
     * once per periodic retry. A 4.xx is a WARN, not a DEBUG: Linux refusing
     * a removal leaves the module on the Linux API indefinitely. */
    if (result == EXPORT_SEND_DELIVERED_2XX) {
        clear_removed_pending(module_uuid);
        ESP_LOGI(TAG, "removal ..%02x%02x: delivered, Linux answered %u.%02u", module_uuid[6], module_uuid[7],
                 (unsigned)(code >> 5), (unsigned)(code & 0x1F));
        return;
    }
    if (swap_removed_outcome(module_uuid, code) == code) {
        return; /* same failure as last time -- already logged */
    }
    if (code == REMOVED_OUTCOME_NO_RESPONSE) {
        ESP_LOGW(TAG, "removal ..%02x%02x: no response from Linux after %d attempts -- kept pending (and on "
                      "flash), retried every resync",
                 module_uuid[6], module_uuid[7], EXPORTER_MAX_ATTEMPTS);
    } else if (result == EXPORT_SEND_REJECTED_4XX) {
        ESP_LOGW(TAG, "removal ..%02x%02x: REJECTED by Linux with %u.%02u -- kept pending, retried every resync "
                      "(4.00 usually means the Linux ingest predates TOPOLOGY_REMOVED msg_type 4: rebuild/redeploy "
                      "pogo_matter_bridge)",
                 module_uuid[6], module_uuid[7], (unsigned)(code >> 5), (unsigned)(code & 0x1F));
    } else {
        ESP_LOGW(TAG, "removal ..%02x%02x: Linux answered %u.%02u -- kept pending, retried every resync",
                 module_uuid[6], module_uuid[7], (unsigned)(code >> 5), (unsigned)(code & 0x1F));
    }
}

/* Packs `sample` into a SAMPLE export wire message. Returns 0 (and logs) on
 * a packing failure -- shared by transmit_sample() below, called fresh
 * immediately before every single CoAP attempt (see this file's own top
 * comment on why no packed buffer is ever cached/reused across attempts). */
static size_t pack_sample_wire(const sensor_sample_t *sample, uint8_t *out_wire, size_t wire_cap)
{
    bridge_export_sample_t exp = {0};
    memcpy(exp.power_base_uuid, s_power_base_uuid, BRIDGE_EXPORT_UUID_LEN);
    uint8_t sc_buf[SENSOR_CHANNEL_MAX_WIRE_LEN];
    size_t sc_len = sensor_channel_pack(sample, sc_buf, sizeof(sc_buf));
    if (sc_len == 0) {
        ESP_LOGW(TAG, "failed to re-pack a sample for export (channel_count=%u) -- this attempt skipped",
                  sample->channel_count);
        return 0;
    }
    memcpy(exp.sensor_channel_payload, sc_buf, sc_len);
    exp.sensor_channel_len = (uint16_t)sc_len;

    size_t wlen = bridge_export_pack_sample(&exp, out_wire, wire_cap);
    if (wlen == 0) {
        ESP_LOGW(TAG, "failed to pack a sample export message -- this attempt skipped");
    }
    return wlen;
}

/* Linear scan over sensor_root_aggregator for `module_uuid`'s CURRENT
 * sample -- mirrors find_current_topology_entry()'s own identical pattern
 * below (sensor_root_aggregator.h exposes no uuid-keyed lookup either).
 * Bounded by SENSOR_ROOT_AGGREGATOR_MAX_NODES (16) either way. Returns
 * false if this uuid has never had a sample recorded, or (should not
 * happen -- sensor_root_aggregator never removes a node once created, see
 * that component's own header) is not found at all. */
static bool find_current_aggregator_sample(const uint8_t module_uuid[SENSOR_UUID_LEN], sensor_sample_t *out)
{
    size_t count = sensor_root_aggregator_get_node_count();
    for (size_t i = 0; i < count; i++) {
        sensor_root_aggregator_node_t node;
        if (sensor_root_aggregator_get_node(i, &node) && node.has_sample &&
            memcmp(node.module_uuid, module_uuid, SENSOR_UUID_LEN) == 0) {
            *out = node.last_sample;
            return true;
        }
    }
    return false;
}

/* True while module_uuid is in the Power Base's topology registry. A SAMPLE
 * is exported only then: sensor_root_aggregator keeps a module's last sample
 * forever, so without this gate every periodic resync kept re-sending the
 * last sample of a module a confirmed POGO detach had already erased. On a
 * Linux ingest that had not applied the removal, each of those identical
 * resends counted as fresh liveness (DuplicateExact refreshes last_seen_at),
 * pinning the module online:true with a frozen sample_seq. */
static bool module_is_registered(const uint8_t module_uuid[SENSOR_UUID_LEN])
{
    topology_registry_entry_t e;
    return find_current_topology_entry(module_uuid, &e);
}

/*
 * One bounded SAMPLE publish attempt sequence for `module_uuid` -- this
 * file's own transmit()'s SAMPLE-specific sibling. Deliberately NOT
 * implemented in terms of transmit() (which stays exactly as-is for
 * TOPOLOGY/HEARTBEAT, unaffected by this pass): unlike those two,
 * transmit_sample() must re-derive its payload EVERY attempt, including
 * the very first one, straight from sensor_root_aggregator's CURRENT state
 * (never a value captured earlier) -- see this file's own top comment for
 * the real-hardware-relevant bug this fixes (a stale sample_seq resent on
 * a later retry attempt while a newer sample had already arrived).
 *
 * Retry policy is otherwise IDENTICAL to transmit(): 2.xx stops
 * immediately; 4.xx (explicit rejection) stops immediately, never retried;
 * only a transient failure (timeout/OT-error/5.xx) consumes the bounded
 * EXPORTER_MAX_ATTEMPTS budget, re-fetching+re-packing before each one.
 */
static export_send_result_t transmit_sample(const uint8_t module_uuid[SENSOR_UUID_LEN])
{
    export_send_result_t result = EXPORT_SEND_TRANSIENT_FAILURE;
    for (int attempt = 0; attempt < EXPORTER_MAX_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(s_retry_backoff_ms[attempt]));
        }
        sensor_sample_t sample;
        if (!module_is_registered(module_uuid) || !find_current_aggregator_sample(module_uuid, &sample)) {
            /* Nothing valid left to send for this uuid -- should not
             * happen (see this function's own doc comment), but stop this
             * attempt cycle cleanly rather than sending garbage. Leaves
             * `result` as whatever the previous iteration (if any) saw. */
            break;
        }
        uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
        size_t wlen = pack_sample_wire(&sample, wire, sizeof(wire));
        if (wlen == 0) {
            break; /* pack failure already logged inside pack_sample_wire() */
        }
        result = send_one_attempt(URI_SAMPLE, wire, wlen);
        if (result == EXPORT_SEND_DELIVERED_2XX) {
            ESP_LOGD(TAG, "bridge export sent OK ('%s', attempt=%d/%d)", URI_SAMPLE, attempt + 1,
                      EXPORTER_MAX_ATTEMPTS);
            return result;
        }
        if (result == EXPORT_SEND_REJECTED_4XX) {
            return result; /* final, explicit rejection -- never retried, same policy as transmit() */
        }
        /* EXPORT_SEND_TRANSIENT_FAILURE: fall through and retry (bounded),
         * re-reading the aggregator fresh on the next loop iteration. */
    }
    return result;
}

/* Forward declaration -- mark_sample_pending() is defined further down
 * (alongside the rest of the pending-table machinery it shares with
 * observer_on_sample_stored()/do_full_sample_resync()), but
 * handle_sample_event() (called only from drain_pending_samples(), itself
 * defined after mark_sample_pending()) needs it too, for the
 * retry-exhausted-leaves-it-pending-again behavior below. */
static void mark_sample_pending(const uint8_t module_uuid[SENSOR_UUID_LEN], TickType_t wait_ticks, bool from_reactive_callback);

/*
 * Dispatches one bounded SAMPLE publish sequence for `module_uuid` (see
 * transmit_sample()'s own doc comment) and reports the outcome. If every
 * attempt hit only a transient failure, marks this uuid pending again --
 * see this file's own top comment: the NEXT time drain_pending_samples()
 * finds its rate-limit window open, whatever is CURRENT in the aggregator
 * at that later moment is published, bounded by the gate, never a busy
 * retry loop. A 4.xx rejection is deliberately NOT re-marked pending here
 * (same policy as before this pass) -- the periodic full resync still
 * re-marks it regardless.
 */
static void handle_sample_event(const uint8_t module_uuid[SENSOR_UUID_LEN])
{
    if (!module_is_registered(module_uuid)) {
        return; /* removed since it was marked pending -- see module_is_registered() */
    }
    export_send_result_t result = transmit_sample(module_uuid);
    if (!module_is_registered(module_uuid)) {
        return; /* removed while the attempt was in flight: nothing to retry or warn about */
    }
    report_export_result(result, "sample",
                          "sample export delivery unconfirmed (Linux OTBR host unreachable? -- the sensor sample "
                          "itself is already stored in the Power Base aggregator regardless)");
    if (result == EXPORT_SEND_TRANSIENT_FAILURE) {
        mark_sample_pending(module_uuid, pdMS_TO_TICKS(20), false);
    }
}

/* Linear scan over the registry for `uuid`'s CURRENT entry -- the only
 * lookup power_base_transport.h exposes (index-based get_node()/
 * get_node_count(), no lookup-by-uuid accessor). Bounded by
 * TOPOLOGY_REGISTRY_MAX_NODES (32) either way. */
static bool find_current_topology_entry(const uint8_t uuid[TOPOLOGY_UUID_LEN], topology_registry_entry_t *out)
{
    /* Every slot, not 0..count-1: an erase frees a slot without compacting
     * (see power_base_transport_get_node()'s doc comment). */
    for (size_t i = 0; i < TOPOLOGY_REGISTRY_MAX_NODES; i++) {
        topology_registry_entry_t e;
        if (power_base_transport_get_node(i, &e) && memcmp(e.module_uuid, uuid, TOPOLOGY_UUID_LEN) == 0) {
            *out = e;
            return true;
        }
    }
    return false;
}

/* Returns true (and marks (uuid, kind) pending) iff a fresh xQueueSend() for
 * this (uuid, kind) pair should proceed -- false means it is already
 * pending (a queued event for it will still be drained, so this duplicate
 * is redundant -- see s_topology_pending_*'s own doc comment on why `kind`
 * is part of the key: a pending REMOVED for a uuid never blocks a fresh
 * SNAPSHOT event for that SAME uuid, or vice versa). A short, bounded
 * critical section, never a blocking wait -- called only from the OT-task's
 * own observer callbacks below. */
static bool topology_pending_try_mark(const uint8_t uuid[TOPOLOGY_UUID_LEN], topology_evt_kind_t kind)
{
    bool should_enqueue = true;
    int free_slot = -1;
    taskENTER_CRITICAL(&s_topology_pending_lock);
    for (int i = 0; i < TOPOLOGY_PENDING_MAX; i++) {
        if (s_topology_pending_valid[i] && s_topology_pending_kind[i] == kind &&
            memcmp(s_topology_pending_uuids[i], uuid, TOPOLOGY_UUID_LEN) == 0) {
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
     * ever mark this (uuid, kind) pending again. */
    if (should_enqueue && free_slot >= 0) {
        s_topology_pending_valid[free_slot] = true;
        memcpy(s_topology_pending_uuids[free_slot], uuid, TOPOLOGY_UUID_LEN);
        s_topology_pending_kind[free_slot] = kind;
    }
    taskEXIT_CRITICAL(&s_topology_pending_lock);
    return should_enqueue;
}

/* Clears (uuid, kind)'s pending mark, if any -- called once a queued event
 * for it has actually been dequeued (exporter_task()) or once an enqueue
 * attempt for it failed (enqueue_topology_event() below), either way
 * freeing it to be marked pending again by a future event. */
static void topology_pending_unmark(const uint8_t uuid[TOPOLOGY_UUID_LEN], topology_evt_kind_t kind)
{
    taskENTER_CRITICAL(&s_topology_pending_lock);
    for (int i = 0; i < TOPOLOGY_PENDING_MAX; i++) {
        if (s_topology_pending_valid[i] && s_topology_pending_kind[i] == kind &&
            memcmp(s_topology_pending_uuids[i], uuid, TOPOLOGY_UUID_LEN) == 0) {
            s_topology_pending_valid[i] = false;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_topology_pending_lock);
}

/* Shared by observer_on_topology_applied()/observer_on_node_offline()/
 * observer_on_node_removed() -- each enqueues nothing but a (uuid, kind)
 * pair now (see topo_evt_payload_t's own doc comment), coalesced via
 * s_topology_pending_* before ever reaching xQueueSend(). Still strictly
 * non-blocking (xQueueSend(..., 0)) -- never anything that could block the
 * calling OT task (this file's own top comment). */
static void enqueue_topology_event(const uint8_t uuid[TOPOLOGY_UUID_LEN], topology_evt_kind_t kind,
                                    const char *drop_log_msg)
{
    if (!topology_pending_try_mark(uuid, kind)) {
        return; /* a fresh event for this (uuid, kind) is already queued -- coalesced away */
    }
    topo_evt_payload_t evt;
    memcpy(evt.uuid, uuid, TOPOLOGY_UUID_LEN);
    evt.kind = kind;
    if (xQueueSend(s_topology_queue, &evt, 0) != pdTRUE) {
        topology_pending_unmark(uuid, kind); /* enqueue failed -- let a future event for this pair try again */
        warn_rate_limited(drop_log_msg);
    }
}

/* ---- REMOVED-tombstone pending table (this follow-up) -- see
 * s_pending_removed_*'s own doc comment. Both a short, bounded critical
 * section, never a blocking wait -- safe to call from the OT task
 * (mark_removed_pending(), via observer_on_node_removed()) or the exporter
 * task (clear_removed_pending()/do_full_removed_resync()). ---- */

/* Returns true iff uuid was newly added. */
static bool mark_removed_pending(const uint8_t uuid[TOPOLOGY_UUID_LEN])
{
    taskENTER_CRITICAL(&s_pending_removed_lock);
    int free_slot = -1;
    bool already = false;
    for (int i = 0; i < PENDING_REMOVED_MAX; i++) {
        if (s_pending_removed_valid[i] && memcmp(s_pending_removed_uuids[i], uuid, TOPOLOGY_UUID_LEN) == 0) {
            already = true;
            break;
        }
        if (!s_pending_removed_valid[i] && free_slot < 0) {
            free_slot = i;
        }
    }
    if (!already && free_slot >= 0) {
        s_pending_removed_valid[free_slot] = true;
        memcpy(s_pending_removed_uuids[free_slot], uuid, TOPOLOGY_UUID_LEN);
        s_pending_removed_last_outcome[free_slot] = 0;
        s_pending_removed_dirty = true;
    }
    /* free_slot < 0 (table full) or already-present: nothing further to do
     * either way -- see PENDING_REMOVED_MAX's own sizing comment for why
     * "full" cannot happen in practice; a duplicate on-node-removed for a
     * uuid already pending is a harmless, idempotent no-op here. */
    taskEXIT_CRITICAL(&s_pending_removed_lock);
    return !already && free_slot >= 0;
}

/* Returns true iff uuid was pending (and is now cleared). */
static bool clear_removed_pending(const uint8_t uuid[TOPOLOGY_UUID_LEN])
{
    bool cleared = false;
    taskENTER_CRITICAL(&s_pending_removed_lock);
    for (int i = 0; i < PENDING_REMOVED_MAX; i++) {
        if (s_pending_removed_valid[i] && memcmp(s_pending_removed_uuids[i], uuid, TOPOLOGY_UUID_LEN) == 0) {
            s_pending_removed_valid[i] = false;
            s_pending_removed_dirty = true;
            cleared = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_pending_removed_lock);
    return cleared;
}

/* Records `outcome` for a still-pending uuid and returns the previous one
 * (0 if none, or if uuid is no longer pending). */
static uint8_t swap_removed_outcome(const uint8_t uuid[TOPOLOGY_UUID_LEN], uint8_t outcome)
{
    uint8_t prev = 0;
    taskENTER_CRITICAL(&s_pending_removed_lock);
    for (int i = 0; i < PENDING_REMOVED_MAX; i++) {
        if (s_pending_removed_valid[i] && memcmp(s_pending_removed_uuids[i], uuid, TOPOLOGY_UUID_LEN) == 0) {
            prev = s_pending_removed_last_outcome[i];
            s_pending_removed_last_outcome[i] = outcome;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_pending_removed_lock);
    return prev;
}

/* Exporter task only (blocking flash write). Commits the current pending
 * set if it changed since the last successful commit. Called at the top of
 * every exporter_task() iteration and before every CoAP attempt
 * (send_one_attempt()), so a fresh
 * tombstone is on flash before its first (possibly long, retried) CoAP
 * attempt, and a re-attach's cancel is on flash before the re-attach's own
 * SNAPSHOT goes out. A failed write re-marks the set dirty and is retried
 * on the next iteration (<= EXPORTER_POLL_TICK_MS later). A set that
 * changes again while the write is in flight is simply marked dirty again
 * by mark/clear and written on the next iteration. */
static void persist_removed_journal_if_dirty(void)
{
    uint8_t pending[PENDING_REMOVED_MAX][TOPOLOGY_UUID_LEN];
    size_t pending_count = 0;

    taskENTER_CRITICAL(&s_pending_removed_lock);
    if (!s_pending_removed_dirty) {
        taskEXIT_CRITICAL(&s_pending_removed_lock);
        return;
    }
    s_pending_removed_dirty = false;
    for (int i = 0; i < PENDING_REMOVED_MAX; i++) {
        if (s_pending_removed_valid[i]) {
            memcpy(pending[pending_count], s_pending_removed_uuids[i], TOPOLOGY_UUID_LEN);
            pending_count++;
        }
    }
    taskEXIT_CRITICAL(&s_pending_removed_lock);

    esp_err_t err = removed_journal_save(pending, pending_count);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "removal journal: %u undelivered tombstone(s) now on flash", (unsigned)pending_count);
    } else {
        taskENTER_CRITICAL(&s_pending_removed_lock);
        s_pending_removed_dirty = true;
        taskEXIT_CRITICAL(&s_pending_removed_lock);
        warn_rate_limited("removed-tombstone journal write failed -- retrying; until it succeeds an undelivered "
                          "removal would not survive a Power Base reboot");
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
    if (p->kind == TOPOLOGY_EVT_REMOVED) {
        handle_removed_event(p->uuid);
        return;
    }
    topology_registry_entry_t e;
    if (find_current_topology_entry(p->uuid, &e)) {
        send_topology_entry(&e);
        return;
    }
    ESP_LOGD(TAG, "topology export event for a uuid no longer present in the registry -- skipped");
}

static void do_full_topology_resync(void)
{
    for (size_t i = 0; i < TOPOLOGY_REGISTRY_MAX_NODES; i++) { /* every slot -- see find_current_topology_entry() */
        topology_registry_entry_t e;
        if (power_base_transport_get_node(i, &e)) {
            send_topology_entry(&e);
        }
    }
}

/* Re-enqueues every still-pending REMOVED tombstone (see
 * s_pending_removed_*'s own doc comment) on every periodic resync tick --
 * this component's own convergence guarantee for a removal, mirroring
 * do_full_topology_resync()/do_full_sample_resync()'s existing role for
 * their own export kinds. Copies the pending uuid list out under the short
 * critical section, then enqueues each OUTSIDE it (enqueue_topology_event()
 * does its own locking) -- never holds s_pending_removed_lock across the
 * queue operation. */
static void do_full_removed_resync(void)
{
    uint8_t pending[PENDING_REMOVED_MAX][TOPOLOGY_UUID_LEN];
    int pending_count = 0;

    taskENTER_CRITICAL(&s_pending_removed_lock);
    for (int i = 0; i < PENDING_REMOVED_MAX; i++) {
        if (s_pending_removed_valid[i]) {
            memcpy(pending[pending_count], s_pending_removed_uuids[i], TOPOLOGY_UUID_LEN);
            pending_count++;
        }
    }
    taskEXIT_CRITICAL(&s_pending_removed_lock);

    for (int i = 0; i < pending_count; i++) {
        enqueue_topology_event(pending[i], TOPOLOGY_EVT_REMOVED,
                                "topology queue full -- dropped one REMOVED resync retry (still pending -- the "
                                "NEXT resync tick tries again)");
    }
}

/*
 * Marks every currently-known sensor_root_aggregator uuid's slot pending
 * again (correction pass -- see this file's own top comment: resync no
 * longer dispatches a SAMPLE directly). This is what lets a Linux OTBR host
 * that just (re)started converge on the latest value without waiting for
 * that module's next sensor measurement (this follow-up's own explicit
 * requirement) -- convergence is now bounded by resync_interval + at most
 * one SAMPLE_MIN_INTERVAL_MS window (see the Kconfig option's own help
 * text), since the actual publish still goes through drain_pending_samples()'s
 * SAME per-uuid gate as every other SAMPLE publish, instead of bypassing it.
 */
static void do_full_sample_resync(void)
{
    size_t count = sensor_root_aggregator_get_node_count();
    for (size_t i = 0; i < count; i++) {
        sensor_root_aggregator_node_t node;
        if (sensor_root_aggregator_get_node(i, &node) && node.has_sample && module_is_registered(node.module_uuid)) {
            /* Bounded (not 0-tick) wait -- called from exporter_task()'s
             * own context, not the OT callback, so it can afford to
             * briefly wait for a lock the reactive callback (0-tick) might
             * transiently hold -- see mark_sample_pending()'s own doc
             * comment for why this still never blocks unboundedly. */
            mark_sample_pending(node.module_uuid, pdMS_TO_TICKS(20), false);
        }
    }
}

/*
 * Marks `module_uuid`'s slot in s_pending_samples dirty (creating it if not
 * already tracked) -- correction pass: no longer stores a sample VALUE at
 * all (see pending_sample_slot_t's own doc comment: the value actually
 * published is always re-read fresh from sensor_root_aggregator at publish
 * time, never cached here). Shared by BOTH SAMPLE publish origins:
 *
 *   - observer_on_sample_stored() (the OT task's own reactive callback,
 *     `from_reactive_callback=true`, `wait_ticks=0`) -- MUST NEVER block
 *     the OT task (this file's own top comment); a 0-tick miss (the
 *     exporter task happens to hold the mutex at that exact instant)
 *     simply drops this one mark-pending attempt with a rate-limited
 *     warning, never retried in a busy loop -- the periodic full sample
 *     resync below is the recovery path for a dropped mark, same as every
 *     other best-effort drop in this component.
 *   - do_full_sample_resync() (`from_reactive_callback=false`,
 *     `wait_ticks=pdMS_TO_TICKS(20)`) -- runs from exporter_task()'s own
 *     context (never the OT callback), so it can afford a brief bounded
 *     wait for whatever short critical section might transiently hold the
 *     mutex, making a resync mark far less likely to ever miss; a miss
 *     here is not logged as urgent -- the NEXT resync interval simply
 *     tries again.
 *   - handle_sample_event() (after a fully-exhausted transient-failure
 *     retry budget, `from_reactive_callback=false`,
 *     `wait_ticks=pdMS_TO_TICKS(20)`) -- see that function's own doc
 *     comment.
 */
static void mark_sample_pending(const uint8_t module_uuid[SENSOR_UUID_LEN], TickType_t wait_ticks,
                                 bool from_reactive_callback)
{
    if (xSemaphoreTake(s_pending_samples_mutex, wait_ticks) != pdTRUE) {
        if (from_reactive_callback) {
            warn_rate_limited("pending-sample table busy -- dropped one reactive SAMPLE mark-pending event (the "
                               "periodic full sample resync still converges to the current value)");
        }
        return;
    }
    int slot = -1;
    int free_slot = -1;
    for (int i = 0; i < PENDING_SAMPLES_MAX; i++) {
        if (s_pending_samples[i].valid && memcmp(s_pending_samples[i].module_uuid, module_uuid, SENSOR_UUID_LEN) == 0) {
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
            if (from_reactive_callback) {
                warn_rate_limited("pending-sample table full -- dropped one reactive SAMPLE mark-pending event for "
                                   "a new uuid (the periodic full sample resync still delivers it)");
            }
            return;
        }
        slot = free_slot;
        s_pending_samples[slot].valid = true;
        memcpy(s_pending_samples[slot].module_uuid, module_uuid, SENSOR_UUID_LEN);
    }
    s_pending_samples[slot].dirty = true; /* coalesce: no value stored, just "a publish is owed" */
    xSemaphoreGive(s_pending_samples_mutex);
}

/*
 * Dispatches (via handle_sample_event()) every currently-dirty pending
 * uuid that is ALSO past its own per-uuid rate-limit window (clearing
 * dirty and re-stamping next_send_allowed_tick as it goes), and returns.
 * Called once per exporter_task() loop iteration -- bounded
 * (PENDING_SAMPLES_MAX slots), never blocks on anything but its own short
 * mutex per slot. This is the ONE place in this file that ever actually
 * dispatches a SAMPLE publish -- see this file's own top comment.
 *
 * A dirty slot still inside its rate-limit window is left exactly as-is
 * (dirty stays true) -- it is neither published now nor dropped; the NEXT
 * loop iteration that finds it both dirty and past its window publishes
 * it, always with whatever is the CURRENT aggregator value at that moment
 * (handle_sample_event()/transmit_sample() re-read it fresh, never a value
 * captured here). This is the same "latest wins, no FIFO replay"
 * coalescing behavior this table already had, now also gated on time -- a
 * rate-limited slot is never a source-of-truth loss (the aggregator
 * already has the value, and the periodic full sample resync -- which now
 * shares this exact same gate, see do_full_sample_resync() -- converges
 * Linux regardless).
 *
 * Wrap-safe comparison (this pass): `(int32_t)(now - deadline) >= 0`, the
 * same idiom already used elsewhere in this codebase (e.g.
 * components/pogo_port/pogo_port.c) for a TickType_t (unsigned, wraps) that
 * must still compare correctly across a wraparound boundary.
 */
static void drain_pending_samples(void)
{
    /* Deliberately the REAL clock, never exporter_now_ms()'s test override
     * -- this gate is the per-uuid SAMPLE_MIN_INTERVAL_MS rate limit (a
     * SEPARATE, much shorter timer from the full resync/heartbeat interval
     * exporter_now_ms() exists to let tests control -- see that function's
     * own doc comment), and existing tests
     * (tools/test_linux_bridge_exporter_rate_limit.c) exercise it opening
     * and closing over genuinely elapsed real time by design. */
    TickType_t now = xTaskGetTickCount();
    TickType_t interval_ticks = pdMS_TO_TICKS(CONFIG_LINUX_BRIDGE_EXPORTER_SAMPLE_MIN_INTERVAL_MS);
    for (int i = 0; i < PENDING_SAMPLES_MAX; i++) {
        uint8_t module_uuid[SENSOR_UUID_LEN];
        bool need_send = false;
        if (xSemaphoreTake(s_pending_samples_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (s_pending_samples[i].valid && s_pending_samples[i].dirty &&
                (int32_t)(now - s_pending_samples[i].next_send_allowed_tick) >= 0) {
                memcpy(module_uuid, s_pending_samples[i].module_uuid, SENSOR_UUID_LEN);
                s_pending_samples[i].dirty = false;
                s_pending_samples[i].next_send_allowed_tick = now + interval_ticks;
                need_send = true;
            }
            xSemaphoreGive(s_pending_samples_mutex);
        }
        if (need_send) {
            handle_sample_event(module_uuid);
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
    TickType_t last_resync = exporter_now_ms();
    TickType_t last_heartbeat = exporter_now_ms();

    while (1) {
        if (s_suspended) {
            /* Unenrolling: nothing is read, sent or persisted any more. */
            vTaskDelay(pdMS_TO_TICKS(EXPORTER_POLL_TICK_MS));
            continue;
        }
        topo_evt_payload_t evt;
        bool have_evt = xQueueReceive(s_topology_queue, &evt, pdMS_TO_TICKS(EXPORTER_POLL_TICK_MS)) == pdTRUE;
        /* Before anything below can send -- see this function's own doc
         * comment. */
        persist_removed_journal_if_dirty();
        if (have_evt) {
            /* This event is no longer "pending" from the coalescing set's
             * point of view as of right now -- a NEW on_topology_applied/
             * on_node_offline for this same uuid arriving while
             * handle_topology_event() below is still in flight (e.g.
             * retrying a transient failure) is correctly treated as a fresh
             * event to enqueue, not folded into the one already being
             * processed. */
            topology_pending_unmark(evt.uuid, evt.kind);
            handle_topology_event(&evt);
        }
        /* Runs every loop iteration regardless of whether a topology event
         * arrived -- bounded (PENDING_SAMPLES_MAX slots), so this never
         * meaningfully lengthens the loop when nothing is dirty. */
        drain_pending_samples();

        taskENTER_CRITICAL(&s_dest_lock);
        const bool resync_now = s_resync_requested;
        s_resync_requested = false;
        taskEXIT_CRITICAL(&s_dest_lock);
        if (resync_now) {
            /* The hub is at a new endpoint (linux_bridge_exporter_set_destination()):
             * it gets this Base's heartbeat first -- that is what registers
             * a Base there -- and then everything the periodic resync
             * sends. Both periods start over from here. */
            ESP_LOGI(TAG, "the hub's endpoint changed -- sending heartbeat, full topology, latest samples and pending "
                          "removals to the new one");
            send_heartbeat();
            do_full_topology_resync();
            do_full_sample_resync();
            do_full_removed_resync();
            last_resync = exporter_now_ms();
            last_heartbeat = last_resync;
        }

        TickType_t now = exporter_now_ms();
        if ((now - last_resync) >= pdMS_TO_TICKS(CONFIG_LINUX_BRIDGE_EXPORTER_FULL_RESYNC_INTERVAL_MS)) {
            do_full_topology_resync();
            do_full_sample_resync();
            do_full_removed_resync();
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
    if (s_suspended) {
        return;
    }
    mark_sample_pending(sample->module_uuid, 0, true);
}

static void observer_on_topology_applied(const topology_registry_entry_t *entry, void *ctx)
{
    (void)ctx;
    if (s_suspended) {
        return;
    }
    /* Correction pass (this follow-up, item 2: rapid detach/reattach race).
     * A real, successful topology_registry_apply_report() for this
     * module_uuid is, unconditionally, proof that it is CURRENTLY present
     * in the Power Base's own topology_registry -- the one and only
     * authoritative source of truth for "does this uuid still exist" (see
     * this file's own top comment and topology_registry.h's). If a REMOVED
     * tombstone for this SAME uuid is still pending (queued, mid-retry, or
     * awaiting the next resync retry) from an EARLIER detach, it is now
     * unconditionally stale -- cancelled here, proactively, the moment the
     * fresh attach is known, rather than waiting for handle_removed_event()'s
     * own drain-time re-check (below) to catch it whenever it happens to
     * run. Together the two form the "belt and suspenders" this correction
     * pass's own design note calls for: this proactive cancel closes the
     * window as early as possible; the drain-time re-check is the
     * authoritative backstop for whatever this cancel's own timing might
     * still miss (e.g. a REMOVED event already dequeued and mid-flight in
     * transmit() when this fires -- see handle_removed_event()'s own doc
     * comment). A harmless no-op if nothing was pending. */
    if (clear_removed_pending(entry->module_uuid)) {
        ESP_LOGI(TAG, "removal ..%02x%02x: pending tombstone cancelled -- re-attached before Linux confirmed it",
                 entry->module_uuid[6], entry->module_uuid[7]);
    }
    enqueue_topology_event(entry->module_uuid, TOPOLOGY_EVT_SNAPSHOT,
                            "topology queue full -- dropped one TOPOLOGY export event (the next periodic full "
                            "resync recovers it)");
}

static void observer_on_node_offline(const uint8_t uuid[TOPOLOGY_UUID_LEN], void *ctx)
{
    (void)ctx;
    if (s_suspended) {
        return;
    }
    enqueue_topology_event(uuid, TOPOLOGY_EVT_SNAPSHOT,
                            "topology queue full -- dropped one TOPOLOGY (offline) export event (the next "
                            "periodic full resync recovers it)");
}

/* This follow-up: POGO auto-registration/removal lifecycle. Marks
 * module_uuid durably pending FIRST (mark_removed_pending() -- survives
 * even if the reactive enqueue immediately below is itself dropped, e.g. a
 * momentarily-full s_topology_queue) and only then attempts the reactive
 * fast-path enqueue -- see s_pending_removed_*'s own doc comment for why
 * this uuid keeps being retried by do_full_removed_resync() regardless of
 * whether this immediate attempt succeeds. */
static void observer_on_node_removed(const uint8_t uuid[TOPOLOGY_UUID_LEN], void *ctx)
{
    (void)ctx;
    if (s_suspended) {
        /* The whole Base is leaving its hub; the hub removes this Base's
         * modules itself when it verifies the completion proof. */
        return;
    }
    if (mark_removed_pending(uuid)) {
        ESP_LOGI(TAG, "removal ..%02x%02x: tombstone pending for Linux", uuid[6], uuid[7]);
    }
    enqueue_topology_event(uuid, TOPOLOGY_EVT_REMOVED,
                            "topology queue full -- dropped one REMOVED export event's reactive fast path (still "
                            "pending -- the next periodic resync retries it)");
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
    taskENTER_CRITICAL(&s_dest_lock);
    memcpy(s_host_addr, cfg->host_addr, POWER_BASE_BRIDGE_CONFIG_ADDR_LEN);
    s_host_port = cfg->port;
    s_dest_generation = 0;
    s_resync_requested = false;
    taskEXIT_CRITICAL(&s_dest_lock);
    s_start_tick_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_suspended = false;
    s_send_token = 0;
    s_last_send_fail_warn_tick = 0;
    memset(s_pending_samples, 0, sizeof(s_pending_samples));
    memset(s_topology_pending_valid, 0, sizeof(s_topology_pending_valid));
    memset(s_pending_removed_valid, 0, sizeof(s_pending_removed_valid));
    memset(s_pending_removed_last_outcome, 0, sizeof(s_pending_removed_last_outcome));
    s_pending_removed_dirty = false;
    /* Tombstones a previous boot never got confirmed (see
     * s_pending_removed_*'s own doc comment). They go out on the first
     * periodic resync, after modules still docked have had a moment to
     * re-register; handle_removed_event() skips (and clears) any uuid that
     * is back in the registry by then. */
    size_t restored = removed_journal_load(s_pending_removed_uuids, PENDING_REMOVED_MAX);
    for (size_t i = 0; i < restored; i++) {
        s_pending_removed_valid[i] = true;
        ESP_LOGI(TAG, "removal ..%02x%02x: undelivered tombstone restored from flash", s_pending_removed_uuids[i][6],
                 s_pending_removed_uuids[i][7]);
    }
    if (restored > 0) {
        ESP_LOGI(TAG, "restored %u undelivered topology-removed tombstone(s) from flash", (unsigned)restored);
    }

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
        .on_node_removed = observer_on_node_removed,
    };
    power_base_transport_set_bridge_observer(&observer);
    s_started = true;

    ESP_LOGI(TAG, "linux bridge exporter ready: resources='/%s','/%s','/%s' port=%u topology_queue_len=%d "
                  "pending_samples_max=%d resync_interval_ms=%u heartbeat_interval_ms=%u "
                  "sample_min_interval_ms=%u (host address never logged)",
             URI_SAMPLE, URI_TOPOLOGY, URI_HEARTBEAT, (unsigned)s_host_port, CONFIG_LINUX_BRIDGE_EXPORTER_QUEUE_LEN,
             PENDING_SAMPLES_MAX, (unsigned)CONFIG_LINUX_BRIDGE_EXPORTER_FULL_RESYNC_INTERVAL_MS,
             (unsigned)CONFIG_LINUX_BRIDGE_EXPORTER_HEARTBEAT_INTERVAL_MS,
             (unsigned)CONFIG_LINUX_BRIDGE_EXPORTER_SAMPLE_MIN_INTERVAL_MS);
    return ESP_OK;
}
