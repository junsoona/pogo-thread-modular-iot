/*
 * Host-compiled regression test for WHEN child_service re-applies this
 * module's own Thread link mode after it has already joined.
 *
 * thread_manager_join_with_dataset() disables and re-enables Thread, so
 * every call is a real re-attach of this module. It is needed exactly when
 * the link mode itself changes (LEAF <-> INTERMEDIATE) and must not happen
 * otherwise. The case this pins down: a module that boots with something
 * docked DOWNSTREAM that never answers HELLO.
 *
 *   - UPSTREAM resolves first, DOWNSTREAM is still mid-handshake: role_manager
 *     has no settled role yet (UNASSIGNED).
 *   - The power base provisions the module; SET_DATASET arrives while the
 *     role is still UNASSIGNED, so the join is made in LEAF link mode.
 *   - The DOWNSTREAM handshake then times out. With DOWNSTREAM transient
 *     errors isolated (docs/state-machine.md), the role settles to LEAF --
 *     the link mode that is ALREADY applied. No second join may follow.
 *
 * Before the fix child_service remembered the raw role at join time
 * (UNASSIGNED) instead of the link mode it applied, saw "LEAF != UNASSIGNED"
 * and re-joined the Thread network once, right after the first join.
 *
 * Links the REAL, UNMODIFIED production sources:
 *   components/child_service/child_service.c   (the join / re-apply decision)
 *   components/role_manager/role_manager.c     (the role it reacts to)
 *   module/main/module_wiring.c                (the real fan-out + role-change
 *                                               callback between the two)
 *   components/sensor_service/sensor_service.c + the mock sensor_provider,
 *   components/collector_endpoint/collector_endpoint.c,
 *   components/pogo_proto/pogo_protocol.c, components/node_calc/node_calc.c,
 *   components/sensor_root_aggregator, components/sensor_channel
 *
 * Fully deterministic: there is no pogo_port here. Port events and the power
 * base's provisioning frames are handed straight to
 * module_wiring_port_event_dispatcher() -- the entry point pogo_port's own
 * task calls on hardware -- one at a time, each request waiting for
 * child_service's reply before the next is sent. The stand-ins only record:
 * pogo_port_send() (what child_service answered), thread_manager (how many
 * joins/stops were requested, and with which link mode), and the two
 * DOWNSTREAM consumers this test does not exercise (parent_service,
 * topology_reporter).
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
 *      -o /tmp/test_child_service_link_mode_reapply tools/test_child_service_link_mode_reapply.c \
 *      components/child_service/child_service.c components/role_manager/role_manager.c \
 *      module/main/module_wiring.c components/collector_endpoint/collector_endpoint.c \
 *      components/pogo_proto/pogo_protocol.c \
 *      components/sensor_service/sensor_service.c components/sensor_provider/sensor_provider.c \
 *      components/sensor_provider/mock_sensor_provider.c components/node_calc/node_calc.c \
 *      components/sensor_root_aggregator/sensor_root_aggregator.c \
 *      components/sensor_channel/sensor_channel.c \
 *   && /tmp/test_child_service_link_mode_reapply
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

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

/* child_service's task re-evaluates its re-apply condition once per loop
 * pass (at most every 50ms); several passes is "it had every chance to". */
#define SETTLE_MS 300u

/* ====================================================================
 * Recording stand-ins
 * ==================================================================== */

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;

/* --- thread_manager: how often, and in which link mode, a join was asked for --- */
static int s_join_calls;
static int s_join_leaf_calls;
static int s_join_intermediate_calls;
static thread_manager_role_t s_last_join_role;
static int s_stop_calls;
static otDeviceRole s_ot_role = OT_DEVICE_ROLE_DISABLED;
static bool s_ot_role_change_pending;

bool thread_manager_validate_dataset_tlvs(const uint8_t *dataset_tlvs, uint16_t dataset_len)
{
    return dataset_tlvs != NULL && dataset_len > 0;
}

esp_err_t thread_manager_join_with_dataset(thread_manager_role_t role, const uint8_t *dataset_tlvs,
                                            uint16_t dataset_len, uint32_t thread_data_poll_period_ms)
{
    (void)dataset_tlvs;
    (void)dataset_len;
    (void)thread_data_poll_period_ms;
    pthread_mutex_lock(&s_mutex);
    s_join_calls++;
    if (role == THREAD_MANAGER_ROLE_LEAF) {
        s_join_leaf_calls++;
    } else {
        s_join_intermediate_calls++;
    }
    s_last_join_role = role;
    s_ot_role = OT_DEVICE_ROLE_CHILD;
    s_ot_role_change_pending = true;
    pthread_mutex_unlock(&s_mutex);
    return ESP_OK;
}

bool thread_manager_poll_role_change(otDeviceRole *out_role)
{
    pthread_mutex_lock(&s_mutex);
    bool has = s_ot_role_change_pending;
    if (has) {
        *out_role = s_ot_role;
        s_ot_role_change_pending = false;
    }
    pthread_mutex_unlock(&s_mutex);
    return has;
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
    s_ot_role = OT_DEVICE_ROLE_DISABLED;
    s_ot_role_change_pending = false;
    pthread_mutex_unlock(&s_mutex);
    return ESP_OK;
}

esp_err_t thread_manager_start_as_root(void)
{
    return ESP_OK; /* DEV_ROOT fixture only -- never reached in AUTO mode */
}

esp_err_t thread_manager_get_mesh_local_eid(otIp6Address *out_addr)
{
    memset(out_addr, 0, sizeof(*out_addr));
    return ESP_FAIL; /* DEV_ROOT fixture only */
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

typedef struct {
    int joins;
    int leaf;
    int intermediate;
    int stops;
} tm_counts_t;

static tm_counts_t tm_counts(void)
{
    pthread_mutex_lock(&s_mutex);
    tm_counts_t c = {s_join_calls, s_join_leaf_calls, s_join_intermediate_calls, s_stop_calls};
    pthread_mutex_unlock(&s_mutex);
    return c;
}

static bool wait_for_joins(int expected, uint32_t timeout_ms)
{
    uint32_t start = now_ms();
    while (now_ms() - start <= timeout_ms) {
        if (tm_counts().joins >= expected) {
            return true;
        }
        sleep_ms(5);
    }
    return tm_counts().joins >= expected;
}

/* --- pogo_port_send: what child_service answered on UPSTREAM --- */
#define SENT_CAP 32
static pogo_frame_t s_sent[SENT_CAP];
static int s_sent_count;

esp_err_t pogo_port_send(pogo_port_id_t id, uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    if (id != POGO_PORT_UPSTREAM || len > POGO_FRAME_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_mutex);
    if (s_sent_count < SENT_CAP) {
        pogo_frame_t *f = &s_sent[s_sent_count++];
        memset(f, 0, sizeof(*f));
        f->type = type;
        f->seq = seq;
        f->len = len;
        if (len > 0) {
            memcpy(f->payload, payload, len);
        }
    }
    pthread_mutex_unlock(&s_mutex);
    return ESP_OK;
}

static bool take_sent(uint8_t type, bool match_seq, uint8_t seq, pogo_frame_t *out, uint32_t timeout_ms)
{
    uint32_t start = now_ms();
    while (now_ms() - start <= timeout_ms) {
        bool found = false;
        pthread_mutex_lock(&s_mutex);
        for (int i = 0; i < s_sent_count; i++) {
            if (s_sent[i].type == type && (!match_seq || s_sent[i].seq == seq)) {
                *out = s_sent[i];
                memmove(&s_sent[i], &s_sent[i + 1], sizeof(pogo_frame_t) * (size_t)(s_sent_count - i - 1));
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

/* ====================================================================
 * Event injection: always through the REAL dispatcher
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
static void down_attached(void) { port_event(POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_ATTACHED, 0, 0); }
static void down_detached(void) { port_event(POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_DETACHED, 0, 0); }
static void down_ready(void) { port_event(POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_PEER_READY, POGO_PEER_TYPE_MODULE, 0); }
static void down_error(pogo_port_error_t e) { port_event(POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_ERROR, 0, e); }

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

static bool pb_request(uint8_t type, const uint8_t *payload, size_t len, uint8_t expect, pogo_frame_t *out,
                        uint32_t timeout_ms)
{
    uint8_t seq = ++s_pb_seq;
    up_frame(type, seq, payload, len);
    return take_sent(expect, true, seq, out, timeout_ms);
}

/* The power base's side of UPSTREAM provisioning, in parent_service.c's
 * order, up to and including the JOIN_RESULT_ACK. */
static bool provision_and_join(void)
{
    static const uint8_t pb_uuid[POGO_UUID_LEN] = {0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x01};
    pogo_frame_t f;
    uint8_t payload[POGO_COLLECTOR_ENDPOINT_WIRE_LEN];
    size_t len;

    pthread_mutex_lock(&s_mutex);
    s_sent_count = 0;
    pthread_mutex_unlock(&s_mutex);

#define STEP(ok, what)                                                                                               \
    do {                                                                                                             \
        if (!(ok)) {                                                                                                 \
            printf("info: provisioning stopped at %s\n", what);                                                      \
            return false;                                                                                            \
        }                                                                                                            \
    } while (0)

    STEP(pb_request(POGO_MSG_IDENTIFY_REQ, NULL, 0, POGO_MSG_IDENTIFY_RESP, &f, 1000), "IDENTIFY");
    STEP(pb_request(POGO_MSG_REQUEST_UUID, NULL, 0, POGO_MSG_PROPOSE_UUID, &f, 1000), "REQUEST_UUID");
    pogo_propose_uuid_t proposed;
    STEP(pogo_unpack_propose_uuid(f.payload, f.len, &proposed), "PROPOSE_UUID decode");

    pogo_assign_uuid_t assign;
    memcpy(assign.uuid, proposed.uuid, POGO_UUID_LEN);
    len = pogo_pack_assign_uuid(&assign, payload, sizeof(payload));
    STEP(pb_request(POGO_MSG_ASSIGN_UUID, payload, len, POGO_MSG_ACK, &f, 1000), "ASSIGN_UUID");

    pogo_assign_slot_t slot = {.slot_id = 0, .topology_depth = 1};
    len = pogo_pack_assign_slot(&slot, payload, sizeof(payload));
    STEP(pb_request(POGO_MSG_ASSIGN_SLOT, payload, len, POGO_MSG_ACK, &f, 1000), "ASSIGN_SLOT");

    pogo_assign_parent_info_t pinfo = {.parent_is_power_base = 1};
    memcpy(pinfo.parent_uuid, pb_uuid, POGO_UUID_LEN);
    len = pogo_pack_assign_parent_info(&pinfo, payload, sizeof(payload));
    STEP(pb_request(POGO_MSG_ASSIGN_PARENT_INFO, payload, len, POGO_MSG_ACK, &f, 1000), "ASSIGN_PARENT_INFO");

    STEP(pb_request(POGO_MSG_SENSOR_HELLO, NULL, 0, POGO_MSG_SENSOR_INFO_ACK, &f, 3000), "SENSOR_HELLO");

    pogo_poll_config_t poll = {.polling_period_ms = 1000};
    len = pogo_pack_poll_config(&poll, payload, sizeof(payload));
    STEP(pb_request(POGO_MSG_POLL_CONFIG, payload, len, POGO_MSG_ACK, &f, 1000), "POLL_CONFIG");

    pogo_collector_endpoint_t collector;
    memset(&collector, 0, sizeof(collector));
    memcpy(collector.power_base_uuid, pb_uuid, POGO_UUID_LEN);
    collector.addr[0] = 0xFD; /* ULA: not unspecified/multicast/link-local */
    collector.addr[15] = 0x01;
    collector.port = 5683;
    collector.generation = 1;
    len = pogo_pack_set_collector(&collector, payload, sizeof(payload));
    STEP(pb_request(POGO_MSG_SET_COLLECTOR, payload, len, POGO_MSG_ACK, &f, 1000), "SET_COLLECTOR");

    static const uint8_t dataset[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    STEP(pb_request(POGO_MSG_SET_DATASET, dataset, sizeof(dataset), POGO_MSG_ACK, &f, 1000), "SET_DATASET");

    STEP(take_sent(POGO_MSG_JOIN_RESULT, false, 0, &f, 3000), "JOIN_RESULT");
    pogo_join_result_t result;
    STEP(pogo_unpack_join_result(f.payload, f.len, &result) && result.success == 1, "JOIN_RESULT success");
    up_frame(POGO_MSG_JOIN_RESULT_ACK, f.seq, NULL, 0);
#undef STEP
    return true;
}

/* Brings the module back to "nothing docked on either port". */
static void undock_everything(void)
{
    down_detached();
    up_detached();
    sleep_ms(SETTLE_MS); /* child_service drains its own queue */
}

/* ====================================================================
 * Scenarios
 * ==================================================================== */

/* The defect: boots with a silent module docked below. */
static void scenario_join_before_role_settles_then_downstream_times_out(void)
{
    printf("\n-- joins while the role is still unsettled, then DOWNSTREAM times out --\n");
    tm_counts_t before = tm_counts();

    up_attached();
    down_attached();
    up_ready();
    CHECK(role_manager_get_role() == MODULE_ROLE_UNASSIGNED,
          "UPSTREAM resolved, DOWNSTREAM still mid-handshake: no settled role yet");

    CHECK(provision_and_join(), "the power base provisions the module and it joins");
    tm_counts_t c = tm_counts();
    CHECK(c.joins - before.joins == 1 && c.leaf - before.leaf == 1, "exactly one Thread join, in LEAF link mode");
    CHECK(role_manager_is_upstream_ready(), "upstream_ready is set");

    down_error(POGO_PORT_ERR_DOCK_TIMEOUT);
    CHECK(role_manager_get_role() == MODULE_ROLE_LEAF, "DOWNSTREAM DOCK_TIMEOUT settles the role to LEAF");
    sleep_ms(SETTLE_MS);
    c = tm_counts();
    CHECK(c.joins - before.joins == 1,
          "no second join: LEAF link mode was already applied by the first one");
    CHECK(c.stops == before.stops && role_manager_is_upstream_ready(), "Thread not stopped, upstream_ready kept");

    for (int i = 0; i < 3; i++) {
        down_error(POGO_PORT_ERR_DOCK_TIMEOUT);
    }
    sleep_ms(SETTLE_MS);
    CHECK(tm_counts().joins - before.joins == 1, "further DOCK_TIMEOUT retry cycles cause no join either");

    printf("\n-- ... the docked module finally answers --\n");
    down_ready();
    CHECK(role_manager_get_role() == MODULE_ROLE_INTERMEDIATE, "PEER_READY(MODULE) -> INTERMEDIATE");
    CHECK(wait_for_joins(before.joins + 2, 2000), "the link mode is re-applied once, because it really changed");
    sleep_ms(SETTLE_MS);
    c = tm_counts();
    CHECK(c.joins - before.joins == 2 && c.intermediate - before.intermediate == 1,
          "exactly one re-apply, in INTERMEDIATE link mode");

    printf("\n-- ... the established child goes silent and comes back --\n");
    down_error(POGO_PORT_ERR_LIVENESS_LOST);
    down_error(POGO_PORT_ERR_DOCK_TIMEOUT);
    down_error(POGO_PORT_ERR_DOCK_TIMEOUT);
    sleep_ms(SETTLE_MS);
    CHECK(role_manager_get_role() == MODULE_ROLE_INTERMEDIATE, "role held at INTERMEDIATE during the fault");
    CHECK(tm_counts().joins - before.joins == 2, "no re-apply during the fault");
    down_ready();
    sleep_ms(SETTLE_MS);
    CHECK(tm_counts().joins - before.joins == 2, "no re-apply on recovery");

    printf("\n-- ... and is physically undocked --\n");
    down_detached();
    CHECK(role_manager_get_role() == MODULE_ROLE_LEAF, "a real DOWNSTREAM detach -> LEAF");
    CHECK(wait_for_joins(before.joins + 3, 2000), "LEAF link mode is applied, because it really changed");
    sleep_ms(SETTLE_MS);
    c = tm_counts();
    CHECK(c.joins - before.joins == 3 && c.leaf - before.leaf == 2, "exactly one re-apply, in LEAF link mode");
    CHECK(c.stops == before.stops && role_manager_is_upstream_ready(),
          "Thread was never stopped and upstream_ready never dropped along the way");

    undock_everything();
}

/* A healthy but slow child: the same unsettled-role join, resolved the other way. */
static void scenario_join_before_role_settles_then_downstream_becomes_ready(void)
{
    printf("\n-- joins while the role is still unsettled, then DOWNSTREAM becomes ready --\n");
    tm_counts_t before = tm_counts();

    up_attached();
    down_attached();
    up_ready();
    CHECK(role_manager_get_role() == MODULE_ROLE_UNASSIGNED, "no settled role yet");
    CHECK(provision_and_join(), "the module joins");
    tm_counts_t c = tm_counts();
    CHECK(c.joins - before.joins == 1 && c.leaf - before.leaf == 1, "exactly one Thread join, in LEAF link mode");

    down_ready();
    CHECK(role_manager_get_role() == MODULE_ROLE_INTERMEDIATE, "PEER_READY(MODULE) -> INTERMEDIATE");
    CHECK(wait_for_joins(before.joins + 2, 2000), "the needed INTERMEDIATE re-apply still happens");
    sleep_ms(SETTLE_MS);
    c = tm_counts();
    CHECK(c.joins - before.joins == 2 && c.intermediate - before.intermediate == 1,
          "exactly one re-apply, in INTERMEDIATE link mode");

    undock_everything();
}

/* The two plain cases: the role is already settled when SET_DATASET arrives. */
static void scenario_role_settled_before_join(void)
{
    printf("\n-- role already LEAF at join time --\n");
    tm_counts_t before = tm_counts();
    up_attached();
    up_ready();
    CHECK(role_manager_get_role() == MODULE_ROLE_LEAF, "nothing below: LEAF");
    CHECK(provision_and_join(), "the module joins");
    sleep_ms(SETTLE_MS);
    tm_counts_t c = tm_counts();
    CHECK(c.joins - before.joins == 1 && c.leaf - before.leaf == 1, "one join in LEAF link mode, nothing after it");

    /* A module docks below afterwards and never answers. */
    down_attached();
    down_error(POGO_PORT_ERR_DOCK_TIMEOUT);
    sleep_ms(SETTLE_MS);
    CHECK(role_manager_get_role() == MODULE_ROLE_LEAF && tm_counts().joins - before.joins == 1,
          "a later DOWNSTREAM DOCK_TIMEOUT causes no join");
    undock_everything();

    printf("\n-- role already INTERMEDIATE at join time --\n");
    before = tm_counts();
    up_attached();
    down_attached();
    down_ready();
    up_ready();
    CHECK(role_manager_get_role() == MODULE_ROLE_INTERMEDIATE, "ready module below: INTERMEDIATE");
    CHECK(provision_and_join(), "the module joins");
    sleep_ms(SETTLE_MS);
    c = tm_counts();
    CHECK(c.joins - before.joins == 1 && c.intermediate - before.intermediate == 1,
          "one join in INTERMEDIATE link mode, nothing after it");
    undock_everything();
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    module_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = MODULE_CONFIG_SCHEMA_VERSION;
    cfg.driver_id = SENSOR_DRIVER_ID_MOCK;
    cfg.sensor_type_bitmap = 0x0001;
    cfg.measurement_timeout_ms = 500;

    CHECK(sensor_service_init(&cfg) == ESP_OK, "sensor_service_init (mock sensor)");
    CHECK(collector_endpoint_init() == ESP_OK, "collector_endpoint_init");
    CHECK(role_manager_init(MODULE_DEV_MODE_AUTO, module_wiring_on_role_change, NULL) == ESP_OK,
          "role_manager_init (AUTO) with the real role-change callback");
    CHECK(child_service_init(&cfg) == ESP_OK, "child_service_init");

    scenario_join_before_role_settles_then_downstream_times_out();
    scenario_join_before_role_settles_then_downstream_becomes_ready();
    scenario_role_settled_before_join();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
    } else {
        printf("%d CHECK(S) FAILED\n", g_failures);
    }
    fflush(stdout);
    /* child_service/sensor_service tasks have no teardown; exit directly. */
    _exit(g_failures == 0 ? 0 : 1);
}
