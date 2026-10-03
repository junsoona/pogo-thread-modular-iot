/*
 * Host-compiled regression test: a Thread join must be judged by what THIS
 * join attempt achieved, not by role-change events left over from an
 * earlier Thread membership.
 *
 * components/thread_manager/thread_manager.c queues every OpenThread role
 * change (ot_state_changed_cb(): xQueueSend(s_role_change_queue, &role, 0),
 * a 4-deep queue that silently drops when full) and child_service drains
 * that queue only while CS_STATE_JOINING. Role changes that happen at any
 * other time stay queued: an INTERMEDIATE's CHILD->ROUTER promotion after
 * JOIN_RESULT, a LEAF re-attaching to its parent, thread_manager_stop()'s
 * DISABLED when UPSTREAM is lost, a link-mode re-apply. The next session's
 * join then reads the OLDEST of them first.
 *
 * Links the REAL, UNMODIFIED production sources (same set as
 * tools/test_child_service_link_mode_reapply.c):
 *   components/child_service/child_service.c   (the join verdict)
 *   components/role_manager/role_manager.c     (upstream_ready, roles)
 *   module/main/module_wiring.c                (dispatcher, role-change
 *                                               reaction incl. thread_manager_stop(),
 *                                               the parent_service provisioning gate)
 *   components/sensor_service + mock sensor_provider (wireless sample hand-off),
 *   components/collector_endpoint, pogo_protocol, node_calc,
 *   sensor_root_aggregator, sensor_channel
 *
 * The thread_manager stand-in reproduces thread_manager.c's role-change
 * plumbing with the SAME calls on the same FreeRTOS queue type (capacity
 * 4, send with 0 wait, receive with 0 wait), plus what OpenThread itself
 * does to the device role on each thread_manager.c call:
 *   thread_manager_join_with_dataset(): otThreadSetEnabled(false) -> DISABLED,
 *     then esp_openthread_auto_start() enables Thread -> DETACHED, then
 *     (if the network is reachable) the attach -> CHILD [-> ROUTER].
 *   thread_manager_stop(): -> DISABLED.
 * One queued event per actual role change (OpenThread may coalesce two
 * quick changes into one callback; that only ever means fewer events).
 *
 * Every JOIN_RESULT child_service sends is recorded together with the role
 * OpenThread actually had at that instant.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs/pogo_fast -I tools/host_test_stubs -I module/main \
 *      -I components/child_service/include -I components/role_manager/include \
 *      -I components/module_config/include -I components/pogo_port/include \
 *      -I components/pogo_proto/include -I components/thread_manager/include \
 *      -I components/parent_service/include -I components/topology_reporter/include \
 *      -I components/sensor_service/include -I components/sensor_provider/include \
 *      -I components/sensor_channel/include -I components/sensor_root_aggregator/include \
 *      -I components/collector_endpoint/include -I components/node_calc/include \
 *      -o /tmp/test_child_service_stale_role_events tools/test_child_service_stale_role_events.c \
 *      components/child_service/child_service.c components/role_manager/role_manager.c \
 *      module/main/module_wiring.c components/collector_endpoint/collector_endpoint.c \
 *      components/pogo_proto/pogo_protocol.c \
 *      components/sensor_service/sensor_service.c components/sensor_provider/sensor_provider.c \
 *      components/sensor_provider/mock_sensor_provider.c components/node_calc/node_calc.c \
 *      components/sensor_root_aggregator/sensor_root_aggregator.c \
 *      components/sensor_channel/sensor_channel.c \
 *   && /tmp/test_child_service_stale_role_events
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "module_wiring.h"
#include "role_manager/role_manager.h"
#include "pogo_port/pogo_port.h"
#include "pogo_proto/pogo_frame.h"
#include "pogo_proto/pogo_protocol.h"
#include "module_config/module_config.h"
#include "sensor_provider/sensor_provider.h"
#include "sensor_service/sensor_service.h"
#include "thread_manager/thread_manager.h"
#include "child_service/child_service.h"
#include "parent_service/parent_service.h"
#include "topology_reporter/topology_reporter.h"
#include "collector_endpoint/collector_endpoint.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        g_checks++;                                                                                                  \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                                   \
            g_failures++;                                                                                            \
        } else {                                                                                                     \
            printf("ok:   %s\n", msg);                                                                               \
        }                                                                                                            \
    } while (0)

static uint32_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((uint64_t)now.tv_sec * 1000ULL + (uint64_t)now.tv_nsec / 1000000ULL);
}

static void sleep_ms(uint32_t ms)
{
    usleep(ms * 1000u);
}

/* child_service's task loop runs at least every 50ms. */
#define SETTLE_MS 300u

static bool role_attached(otDeviceRole r)
{
    return r == OT_DEVICE_ROLE_CHILD || r == OT_DEVICE_ROLE_ROUTER || r == OT_DEVICE_ROLE_LEADER;
}

/* ====================================================================
 * thread_manager stand-in (see the header comment)
 * ==================================================================== */

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static QueueHandle_t s_role_change_queue; /* thread_manager.c: xQueueCreate(4, sizeof(otDeviceRole)) */
static otDeviceRole s_ot_role = OT_DEVICE_ROLE_DISABLED;
static int s_queue_drops;
static int s_queue_depth; /* the stub queue has no uxQueueMessagesWaiting() */
static int s_join_calls;
static int s_stop_calls;

/* What the next join's attach does: never attach, or attach after a delay
 * (and optionally get promoted to ROUTER a little later). */
static bool s_attach_enabled;
static uint32_t s_attach_delay_ms;
static bool s_attach_promote;
static uint32_t s_attach_generation; /* invalidates a pending attach from an earlier join/stop */

/* OpenThread changed the device role: thread_manager.c's ot_state_changed_cb(). */
static void ot_role_changed(otDeviceRole role)
{
    pthread_mutex_lock(&s_mutex);
    bool changed = (role != s_ot_role);
    s_ot_role = role;
    pthread_mutex_unlock(&s_mutex);
    if (!changed) {
        return;
    }
    bool queued = xQueueSend(s_role_change_queue, &role, 0) == pdTRUE;
    pthread_mutex_lock(&s_mutex);
    if (queued) {
        s_queue_depth++;
    } else {
        s_queue_drops++;
    }
    pthread_mutex_unlock(&s_mutex);
}

typedef struct {
    uint32_t generation;
    uint32_t delay_ms;
    bool promote;
} attach_job_t;

static void *attach_thread(void *arg)
{
    attach_job_t job = *(attach_job_t *)arg;
    free(arg);
    sleep_ms(job.delay_ms);
    pthread_mutex_lock(&s_mutex);
    bool current = (job.generation == s_attach_generation);
    pthread_mutex_unlock(&s_mutex);
    if (!current) {
        return NULL;
    }
    ot_role_changed(OT_DEVICE_ROLE_CHILD);
    if (job.promote) {
        sleep_ms(80);
        pthread_mutex_lock(&s_mutex);
        current = (job.generation == s_attach_generation);
        pthread_mutex_unlock(&s_mutex);
        if (current) {
            ot_role_changed(OT_DEVICE_ROLE_ROUTER);
        }
    }
    return NULL;
}

static void set_attach(bool enabled, uint32_t delay_ms, bool promote)
{
    pthread_mutex_lock(&s_mutex);
    s_attach_enabled = enabled;
    s_attach_delay_ms = delay_ms;
    s_attach_promote = promote;
    pthread_mutex_unlock(&s_mutex);
}

bool thread_manager_validate_dataset_tlvs(const uint8_t *dataset_tlvs, uint16_t dataset_len)
{
    return dataset_tlvs != NULL && dataset_len > 0;
}

esp_err_t thread_manager_join_with_dataset(thread_manager_role_t role, const uint8_t *dataset_tlvs,
                                            uint16_t dataset_len, uint32_t thread_data_poll_period_ms)
{
    (void)role;
    (void)dataset_tlvs;
    (void)dataset_len;
    (void)thread_data_poll_period_ms;
    pthread_mutex_lock(&s_mutex);
    s_join_calls++;
    uint32_t gen = ++s_attach_generation;
    bool attach = s_attach_enabled;
    uint32_t delay = s_attach_delay_ms;
    bool promote = s_attach_promote;
    pthread_mutex_unlock(&s_mutex);

    ot_role_changed(OT_DEVICE_ROLE_DISABLED); /* otThreadSetEnabled(false) */
    ot_role_changed(OT_DEVICE_ROLE_DETACHED); /* esp_openthread_auto_start(): Thread enabled, attaching */
    if (attach) {
        attach_job_t *job = malloc(sizeof(*job));
        job->generation = gen;
        job->delay_ms = delay;
        job->promote = promote;
        pthread_t t;
        pthread_create(&t, NULL, attach_thread, job);
        pthread_detach(t);
    }
    return ESP_OK;
}

/* thread_manager.c's own body, plus depth bookkeeping for the test's log. */
bool thread_manager_poll_role_change(otDeviceRole *out_role)
{
    bool got = xQueueReceive(s_role_change_queue, out_role, 0) == pdTRUE;
    if (got) {
        pthread_mutex_lock(&s_mutex);
        s_queue_depth--;
        pthread_mutex_unlock(&s_mutex);
    }
    return got;
}

otDeviceRole thread_manager_get_role(void)
{
    pthread_mutex_lock(&s_mutex);
    otDeviceRole r = s_ot_role;
    pthread_mutex_unlock(&s_mutex);
    return r;
}

esp_err_t thread_manager_stop(void)
{
    pthread_mutex_lock(&s_mutex);
    s_stop_calls++;
    s_attach_generation++;
    pthread_mutex_unlock(&s_mutex);
    ot_role_changed(OT_DEVICE_ROLE_DISABLED);
    return ESP_OK;
}

esp_err_t thread_manager_start_as_root(void)
{
    return ESP_OK; /* DEV_ROOT fixture only -- never reached in AUTO mode */
}

esp_err_t thread_manager_get_mesh_local_eid(otIp6Address *out_addr)
{
    memset(out_addr, 0, sizeof(*out_addr));
    return ESP_FAIL;
}

const char *thread_manager_role_to_str(otDeviceRole role)
{
    switch (role) {
    case OT_DEVICE_ROLE_DISABLED: return "DISABLED";
    case OT_DEVICE_ROLE_DETACHED: return "DETACHED";
    case OT_DEVICE_ROLE_CHILD: return "CHILD";
    case OT_DEVICE_ROLE_ROUTER: return "ROUTER";
    case OT_DEVICE_ROLE_LEADER: return "LEADER";
    default: return "UNKNOWN";
    }
}

static int queue_depth(void)
{
    pthread_mutex_lock(&s_mutex);
    int n = s_queue_depth;
    pthread_mutex_unlock(&s_mutex);
    return n;
}

/* ====================================================================
 * pogo_port_send: what child_service answered, and the Thread role at
 * the instant a JOIN_RESULT left
 * ==================================================================== */

#define SENT_CAP 64
typedef struct {
    pogo_frame_t f;
    otDeviceRole ot_role_at_send;
    uint32_t t;
} sent_t;
static sent_t s_sent[SENT_CAP];
static int s_sent_count;

esp_err_t pogo_port_send(pogo_port_id_t id, uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    if (id != POGO_PORT_UPSTREAM || len > POGO_FRAME_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }
    otDeviceRole r = thread_manager_get_role();
    pthread_mutex_lock(&s_mutex);
    if (s_sent_count < SENT_CAP) {
        sent_t *s = &s_sent[s_sent_count++];
        memset(s, 0, sizeof(*s));
        s->f.type = type;
        s->f.seq = seq;
        s->f.len = len;
        if (len > 0) {
            memcpy(s->f.payload, payload, len);
        }
        s->ot_role_at_send = r;
        s->t = now_ms();
    }
    pthread_mutex_unlock(&s_mutex);
    return ESP_OK;
}

static void clear_sent(void)
{
    pthread_mutex_lock(&s_mutex);
    s_sent_count = 0;
    pthread_mutex_unlock(&s_mutex);
}

static bool take_sent(uint8_t type, bool match_seq, uint8_t seq, sent_t *out, uint32_t timeout_ms)
{
    uint32_t start = now_ms();
    while (now_ms() - start <= timeout_ms) {
        bool found = false;
        pthread_mutex_lock(&s_mutex);
        for (int i = 0; i < s_sent_count; i++) {
            if (s_sent[i].f.type == type && (!match_seq || s_sent[i].f.seq == seq)) {
                *out = s_sent[i];
                memmove(&s_sent[i], &s_sent[i + 1], sizeof(sent_t) * (size_t)(s_sent_count - i - 1));
                s_sent_count--;
                found = true;
                break;
            }
        }
        pthread_mutex_unlock(&s_mutex);
        if (found) {
            return true;
        }
        sleep_ms(2);
    }
    return false;
}

/* --- DOWNSTREAM consumers this test does not exercise --- */
void parent_service_on_port_event(pogo_port_id_t port_id, pogo_port_event_t event, const pogo_port_event_data_t *data)
{
    (void)port_id;
    (void)event;
    (void)data;
}

void topology_reporter_on_port_event(pogo_port_id_t port_id, pogo_port_event_t event,
                                      const pogo_port_event_data_t *data)
{
    (void)port_id;
    (void)event;
    (void)data;
}

/* --- sensor_service's wireless hand-off (only while "Thread joined") --- */
static int s_handoffs;

static void sample_ready(const sensor_sample_t *sample)
{
    (void)sample;
    pthread_mutex_lock(&s_mutex);
    s_handoffs++;
    pthread_mutex_unlock(&s_mutex);
}

static int handoffs(void)
{
    pthread_mutex_lock(&s_mutex);
    int n = s_handoffs;
    pthread_mutex_unlock(&s_mutex);
    return n;
}

/* ====================================================================
 * Event injection through the REAL dispatcher
 * ==================================================================== */

static void port_event(pogo_port_id_t port, pogo_port_event_t event, pogo_peer_type_t peer_type,
                        pogo_port_error_t error)
{
    pogo_port_event_data_t data;
    memset(&data, 0, sizeof(data));
    data.peer_type = peer_type;
    data.error_code = error;
    module_wiring_port_event_dispatcher(port, event, &data, NULL);
}

static void up_attached(void) { port_event(POGO_PORT_UPSTREAM, POGO_PORT_EVENT_ATTACHED, 0, 0); }
static void up_detached(void) { port_event(POGO_PORT_UPSTREAM, POGO_PORT_EVENT_DETACHED, 0, 0); }
static void up_ready(void) { port_event(POGO_PORT_UPSTREAM, POGO_PORT_EVENT_PEER_READY, POGO_PEER_TYPE_POWER_BASE, 0); }
static void up_error(pogo_port_error_t e) { port_event(POGO_PORT_UPSTREAM, POGO_PORT_EVENT_ERROR, 0, e); }
static void down_attached(void) { port_event(POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_ATTACHED, 0, 0); }
static void down_detached(void) { port_event(POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_DETACHED, 0, 0); }
static void down_ready(void) { port_event(POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_PEER_READY, POGO_PEER_TYPE_MODULE, 0); }

static void up_frame(uint8_t type, uint8_t seq, const uint8_t *payload, size_t len)
{
    pogo_port_event_data_t data;
    memset(&data, 0, sizeof(data));
    data.frame.type = type;
    data.frame.seq = seq;
    data.frame.len = (uint16_t)len;
    if (len > 0) {
        memcpy(data.frame.payload, payload, len);
    }
    module_wiring_port_event_dispatcher(POGO_PORT_UPSTREAM, POGO_PORT_EVENT_FRAME_RECEIVED, &data, NULL);
}

static uint8_t s_pb_seq;

static bool pb_request(uint8_t type, const uint8_t *payload, size_t len, uint8_t expect, uint32_t timeout_ms,
                        sent_t *out)
{
    uint8_t seq = ++s_pb_seq;
    up_frame(type, seq, payload, len);
    return take_sent(expect, true, seq, out, timeout_ms);
}

/* The power base's side of UPSTREAM provisioning (parent_service.c's
 * order) up to and including the SET_DATASET ACK -- the join starts there. */
static bool provision_until_join_starts(void)
{
    static const uint8_t pb_uuid[POGO_UUID_LEN] = {0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x01};
    static uint32_t generation;
    sent_t s;
    uint8_t payload[POGO_COLLECTOR_ENDPOINT_WIRE_LEN];
    size_t len;
    clear_sent();

#define STEP(ok, what)                                                                                               \
    do {                                                                                                             \
        if (!(ok)) {                                                                                                 \
            printf("info: provisioning stopped at %s\n", what);                                                      \
            return false;                                                                                            \
        }                                                                                                            \
    } while (0)

    STEP(pb_request(POGO_MSG_IDENTIFY_REQ, NULL, 0, POGO_MSG_IDENTIFY_RESP, 1000, &s), "IDENTIFY");
    STEP(pb_request(POGO_MSG_REQUEST_UUID, NULL, 0, POGO_MSG_PROPOSE_UUID, 1000, &s), "REQUEST_UUID");
    pogo_propose_uuid_t proposed;
    STEP(pogo_unpack_propose_uuid(s.f.payload, s.f.len, &proposed), "PROPOSE_UUID decode");
    pogo_assign_uuid_t assign;
    memcpy(assign.uuid, proposed.uuid, POGO_UUID_LEN);
    len = pogo_pack_assign_uuid(&assign, payload, sizeof(payload));
    STEP(pb_request(POGO_MSG_ASSIGN_UUID, payload, len, POGO_MSG_ACK, 1000, &s), "ASSIGN_UUID");
    pogo_assign_slot_t slot = {.slot_id = 0, .topology_depth = 1};
    len = pogo_pack_assign_slot(&slot, payload, sizeof(payload));
    STEP(pb_request(POGO_MSG_ASSIGN_SLOT, payload, len, POGO_MSG_ACK, 1000, &s), "ASSIGN_SLOT");
    pogo_assign_parent_info_t pinfo = {.parent_is_power_base = 1};
    memcpy(pinfo.parent_uuid, pb_uuid, POGO_UUID_LEN);
    len = pogo_pack_assign_parent_info(&pinfo, payload, sizeof(payload));
    STEP(pb_request(POGO_MSG_ASSIGN_PARENT_INFO, payload, len, POGO_MSG_ACK, 1000, &s), "ASSIGN_PARENT_INFO");
    STEP(pb_request(POGO_MSG_SENSOR_HELLO, NULL, 0, POGO_MSG_SENSOR_INFO_ACK, 3000, &s), "SENSOR_HELLO");
    pogo_poll_config_t poll = {.polling_period_ms = 300};
    len = pogo_pack_poll_config(&poll, payload, sizeof(payload));
    STEP(pb_request(POGO_MSG_POLL_CONFIG, payload, len, POGO_MSG_ACK, 1000, &s), "POLL_CONFIG");
    pogo_collector_endpoint_t collector;
    memset(&collector, 0, sizeof(collector));
    memcpy(collector.power_base_uuid, pb_uuid, POGO_UUID_LEN);
    collector.addr[0] = 0xFD;
    collector.addr[15] = 0x01;
    collector.port = 5683;
    collector.generation = ++generation;
    len = pogo_pack_set_collector(&collector, payload, sizeof(payload));
    STEP(pb_request(POGO_MSG_SET_COLLECTOR, payload, len, POGO_MSG_ACK, 1000, &s), "SET_COLLECTOR");
    static const uint8_t dataset[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    STEP(pb_request(POGO_MSG_SET_DATASET, dataset, sizeof(dataset), POGO_MSG_ACK, 1000, &s), "SET_DATASET");
#undef STEP
    return true;
}

typedef struct {
    bool sent;
    bool success;
    otDeviceRole reported_role;
    otDeviceRole ot_role_at_send;
    uint32_t t;
} join_outcome_t;

static join_outcome_t wait_join_result(uint32_t timeout_ms)
{
    join_outcome_t o = {0};
    sent_t s;
    if (take_sent(POGO_MSG_JOIN_RESULT, false, 0, &s, timeout_ms)) {
        pogo_join_result_t r;
        o.sent = pogo_unpack_join_result(s.f.payload, s.f.len, &r);
        o.success = o.sent && r.success == 1;
        o.reported_role = o.sent ? (otDeviceRole)r.role : OT_DEVICE_ROLE_DISABLED;
        o.ot_role_at_send = s.ot_role_at_send;
        o.t = s.t;
        up_frame(POGO_MSG_JOIN_RESULT_ACK, s.f.seq, NULL, 0);
    }
    return o;
}

static bool provisioning_allowed(void)
{
    const parent_service_context_t *ctx = module_wiring_parent_context();
    return ctx->provisioning_allowed(ctx->ctx);
}

static void undock_everything(void)
{
    down_detached();
    up_detached();
    sleep_ms(SETTLE_MS);
}

/* The verdict on a join attempt whose network is unreachable (Thread stays
 * DETACHED), with stale events from an earlier membership queued. */
static void check_unreachable_join(const char *what, bool intermediate)
{
    printf("info: role-change queue holds %d stale event(s) as the new join starts\n", queue_depth());
    uint32_t t_join = now_ms();
    CHECK(provision_until_join_starts(), "re-provisioning reaches SET_DATASET");
    int h0 = handoffs();
    join_outcome_t o = wait_join_result(CONFIG_POGO_JOIN_TIMEOUT_MS + 1000);
    char msg[200];
    snprintf(msg, sizeof(msg), "%s: JOIN_RESULT is a failure (got %s, role=%s, %u ms after SET_DATASET; Thread %s at "
                               "send)",
             what, !o.sent ? "nothing" : o.success ? "SUCCESS" : "failure",
             thread_manager_role_to_str(o.reported_role), (unsigned)(o.t - t_join),
             thread_manager_role_to_str(o.ot_role_at_send));
    CHECK(o.sent && !o.success, msg);
    CHECK(!o.sent || !o.success || role_attached(o.ot_role_at_send),
          "a JOIN_RESULT success is never sent while Thread is not attached");
    CHECK(!role_manager_is_upstream_ready(), "upstream_ready stays false (topology registration / gate not started)");
    if (intermediate) {
        CHECK(!provisioning_allowed(), "this INTERMEDIATE does not start provisioning the module below it");
    }
    sleep_ms(1000); /* several 300ms sensor periods */
    CHECK(handoffs() == h0, "no sensor sample handed to the wireless transport");
}

/* ====================================================================
 * Scenarios
 * ==================================================================== */

/* LEAF: re-attached to its parent while joined (DETACHED, CHILD queued),
 * then UPSTREAM is lost (thread_manager_stop -> DISABLED queued). */
static void scenario_leaf_reattach_then_upstream_lost(void)
{
    printf("\n-- LEAF: stale DETACHED/CHILD from a re-attach while joined, then UPSTREAM lost and re-provisioned "
           "on an unreachable network --\n");
    set_attach(true, 20, false);
    up_attached();
    up_ready();
    CHECK(role_manager_get_role() == MODULE_ROLE_LEAF, "nothing below: LEAF");
    CHECK(provision_until_join_starts(), "first provisioning reaches SET_DATASET");
    join_outcome_t o = wait_join_result(2000);
    CHECK(o.sent && o.success && o.ot_role_at_send == OT_DEVICE_ROLE_CHILD, "first join: real attach, JOIN_RESULT OK");
    sleep_ms(SETTLE_MS);

    ot_role_changed(OT_DEVICE_ROLE_DETACHED); /* parent lost ... */
    ot_role_changed(OT_DEVICE_ROLE_CHILD);    /* ... and re-attached, all while JOINED */
    up_error(POGO_PORT_ERR_LIVENESS_LOST);
    sleep_ms(SETTLE_MS);
    CHECK(thread_manager_get_role() == OT_DEVICE_ROLE_DISABLED && !role_manager_is_upstream_ready(),
          "UPSTREAM lost: Thread stopped (DISABLED), upstream_ready false");

    set_attach(false, 0, false);
    up_ready();
    check_unreachable_join("LEAF, network unreachable", false);
    undock_everything();
}

/* INTERMEDIATE: promoted to ROUTER after JOIN_RESULT (ROUTER queued), then
 * UPSTREAM lost (DISABLED queued). */
static void scenario_intermediate_promoted_then_upstream_lost(void)
{
    printf("\n-- INTERMEDIATE: stale ROUTER promotion, then UPSTREAM lost and re-provisioned on an unreachable "
           "network --\n");
    set_attach(true, 20, true);
    up_attached();
    down_attached();
    down_ready();
    up_ready();
    CHECK(role_manager_get_role() == MODULE_ROLE_INTERMEDIATE, "ready module below: INTERMEDIATE");
    CHECK(provision_until_join_starts(), "first provisioning reaches SET_DATASET");
    join_outcome_t o = wait_join_result(2000);
    CHECK(o.sent && o.success, "first join: real attach, JOIN_RESULT OK");
    sleep_ms(SETTLE_MS); /* the CHILD -> ROUTER promotion lands while JOINED */
    CHECK(thread_manager_get_role() == OT_DEVICE_ROLE_ROUTER, "promoted to ROUTER");
    CHECK(provisioning_allowed(), "joined INTERMEDIATE may provision the module below (control)");

    up_error(POGO_PORT_ERR_LIVENESS_LOST);
    sleep_ms(SETTLE_MS);
    CHECK(thread_manager_get_role() == OT_DEVICE_ROLE_DISABLED && !role_manager_is_upstream_ready(),
          "UPSTREAM lost: Thread stopped (DISABLED), upstream_ready false");

    set_attach(false, 0, false);
    up_ready();
    CHECK(role_manager_get_role() == MODULE_ROLE_INTERMEDIATE, "UPSTREAM back: INTERMEDIATE again");
    check_unreachable_join("INTERMEDIATE, network unreachable", true);
    undock_everything();
}

/* Stale events queued, and THIS join does attach -- just later. Success
 * must be reported, and only once the attach really happened. Also covers
 * a full queue: the new join's own events are dropped by xQueueSend(.., 0). */
static void scenario_stale_events_then_slow_real_attach(void)
{
    printf("\n-- stale events (queue full), then this join attaches 150 ms later --\n");
    set_attach(true, 20, false);
    up_attached();
    up_ready();
    CHECK(provision_until_join_starts(), "first provisioning reaches SET_DATASET");
    join_outcome_t o = wait_join_result(2000);
    CHECK(o.sent && o.success, "first join OK");
    sleep_ms(SETTLE_MS);
    for (int i = 0; i < 2; i++) {
        ot_role_changed(OT_DEVICE_ROLE_DETACHED);
        ot_role_changed(OT_DEVICE_ROLE_CHILD);
    }
    up_error(POGO_PORT_ERR_LIVENESS_LOST);
    sleep_ms(SETTLE_MS);
    printf("info: queue depth %d (capacity 4), events dropped so far %d\n", queue_depth(), s_queue_drops);

    set_attach(true, 150, false);
    up_ready();
    uint32_t t0 = now_ms();
    CHECK(provision_until_join_starts(), "re-provisioning reaches SET_DATASET");
    o = wait_join_result(CONFIG_POGO_JOIN_TIMEOUT_MS + 1000);
    char msg[200];
    snprintf(msg, sizeof(msg), "JOIN_RESULT success sent only after the real attach (%u ms after SET_DATASET, Thread "
                               "%s at send)",
             (unsigned)(o.t - t0), thread_manager_role_to_str(o.ot_role_at_send));
    CHECK(o.sent && o.success && o.ot_role_at_send == OT_DEVICE_ROLE_CHILD, msg);
    sleep_ms(SETTLE_MS);
    CHECK(role_manager_is_upstream_ready(), "upstream_ready set after the real join");
    int h0 = handoffs();
    sleep_ms(1000);
    CHECK(handoffs() > h0, "sensor samples flow after a real join (control for the hand-off checks)");
    undock_everything();
}

/* SET_DATASET and an UPSTREAM error reach child_service back to back: the
 * role change stops Thread (DISABLED) before child_service's task looks at
 * the join at all. */
static void scenario_upstream_lost_right_after_set_dataset(void)
{
    printf("\n-- UPSTREAM lost right after SET_DATASET while a stale CHILD is queued --\n");
    set_attach(true, 20, false);
    up_attached();
    up_ready();
    CHECK(provision_until_join_starts(), "first provisioning reaches SET_DATASET");
    join_outcome_t o = wait_join_result(2000);
    CHECK(o.sent && o.success, "first join OK");
    sleep_ms(SETTLE_MS);
    up_error(POGO_PORT_ERR_LIVENESS_LOST); /* DISABLED queued */
    sleep_ms(SETTLE_MS);
    up_ready();
    ot_role_changed(OT_DEVICE_ROLE_DETACHED); /* stand-ins for any stale pair */
    ot_role_changed(OT_DEVICE_ROLE_CHILD);
    ot_role_changed(OT_DEVICE_ROLE_DISABLED);

    set_attach(false, 0, false);
    CHECK(provision_until_join_starts(), "re-provisioning reaches SET_DATASET");
    up_error(POGO_PORT_ERR_LIVENESS_LOST);
    sleep_ms(CONFIG_POGO_JOIN_TIMEOUT_MS + 500);
    sent_t s;
    bool any_ok = false;
    while (take_sent(POGO_MSG_JOIN_RESULT, false, 0, &s, 0)) {
        pogo_join_result_t r;
        if (pogo_unpack_join_result(s.f.payload, s.f.len, &r) && r.success == 1) {
            any_ok = true;
            printf("info: JOIN_RESULT success sent with Thread %s\n", thread_manager_role_to_str(s.ot_role_at_send));
        }
    }
    CHECK(thread_manager_get_role() == OT_DEVICE_ROLE_DISABLED, "Thread is DISABLED (stopped by the UPSTREAM loss)");
    CHECK(!any_ok, "no JOIN_RESULT success");
    CHECK(!role_manager_is_upstream_ready(), "upstream_ready stays false");
    undock_everything();
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    s_role_change_queue = xQueueCreate(4, sizeof(otDeviceRole));

    module_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = MODULE_CONFIG_SCHEMA_VERSION;
    cfg.driver_id = SENSOR_DRIVER_ID_MOCK;
    cfg.sensor_type_bitmap = 0x0001;
    cfg.measurement_timeout_ms = 500;

    CHECK(sensor_service_init(&cfg) == ESP_OK, "sensor_service_init (mock sensor)");
    sensor_service_set_sample_ready_cb(sample_ready);
    CHECK(collector_endpoint_init() == ESP_OK, "collector_endpoint_init");
    CHECK(role_manager_init(MODULE_DEV_MODE_AUTO, module_wiring_on_role_change, NULL) == ESP_OK,
          "role_manager_init (AUTO) with the real role-change callback");
    CHECK(child_service_init(&cfg) == ESP_OK, "child_service_init");

    scenario_leaf_reattach_then_upstream_lost();
    scenario_intermediate_promoted_then_upstream_lost();
    scenario_stale_events_then_slow_real_attach();
    scenario_upstream_lost_right_after_set_dataset();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
    } else {
        printf("%d CHECK(S) FAILED\n", g_failures);
    }
    fflush(stdout);
    _exit(g_failures == 0 ? 0 : 1); /* service tasks have no teardown */
}
