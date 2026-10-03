/*
 * Host-compiled regression test for DOWNSTREAM transient-error isolation in
 * an AUTO sensor module (docs/state-machine.md, "DOWNSTREAM 일시 오류 격리").
 *
 * Real-hardware finding this covers: an SGP41 module that was joined and
 * reporting normally stopped its OWN Thread membership the moment a second
 * module docked below it failed its first handshake -- role_manager turned
 * the DOWNSTREAM DOCK_TIMEOUT into MODULE_ROLE_ERROR, and the role-change
 * callback stops Thread for every ROLE_ERROR.
 *
 * Unlike tools/test_role_manager_auto_chain.c (which mirrors recompute() by
 * hand), THIS test links the REAL, UNMODIFIED production sources:
 *   - components/role_manager/role_manager.c   (role derivation)
 *   - module/main/module_wiring.c              (the port-event fan-out, the
 *     role-change reaction that stops Thread / clears upstream_ready, and
 *     parent_service's provisioning gate -- exactly what app_main.c
 *     registers with role_manager_init()/pogo_port_init()/parent_service_init())
 *   - components/collector_endpoint/collector_endpoint.c,
 *     components/pogo_proto/pogo_protocol.c    (module_wiring.c's own deps)
 *
 * Every port event is injected through module_wiring_port_event_dispatcher(),
 * the same entry point pogo_port's task calls on real hardware. Only the
 * layers BELOW/BESIDE that glue are stand-ins, and they only record what
 * they were told (see "recording stand-ins" below): thread_manager (no host
 * OpenThread stack), and child_service/parent_service/topology_reporter/
 * sensor_service's event/role entry points. Those four real components, the
 * real pogo_port state machine, the real topology gate and the real
 * collector cache are exercised end to end by
 * tools/test_module_downstream_fault_isolation.c instead -- so this file
 * checks the role, the Thread stop and upstream_ready only, never the
 * collector or the gate. It is the fast, fully deterministic (no timing)
 * truth table for the role policy and the Thread-stop decision that follows
 * it.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs/pogo_fast -I tools/host_test_stubs -I module/main \
 *      -I components/role_manager/include -I components/module_config/include \
 *      -I components/pogo_port/include -I components/pogo_proto/include \
 *      -I components/thread_manager/include -I components/child_service/include \
 *      -I components/parent_service/include -I components/topology_reporter/include \
 *      -I components/sensor_service/include -I components/sensor_provider/include \
 *      -I components/sensor_channel/include -I components/sensor_root_aggregator/include \
 *      -I components/collector_endpoint/include \
 *      -o /tmp/test_role_manager_downstream_isolation tools/test_role_manager_downstream_isolation.c \
 *      components/role_manager/role_manager.c module/main/module_wiring.c \
 *      components/collector_endpoint/collector_endpoint.c components/pogo_proto/pogo_protocol.c \
 *   && /tmp/test_role_manager_downstream_isolation
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#include "module_wiring.h"
#include "role_manager/role_manager.h"
#include "pogo_port/pogo_port.h"
#include "thread_manager/thread_manager.h"
#include "child_service/child_service.h"
#include "parent_service/parent_service.h"
#include "topology_reporter/topology_reporter.h"
#include "sensor_service/sensor_service.h"
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

/* --- recording stand-ins (see this file's header comment) --- */

static int s_thread_stop_calls;
static int s_thread_start_as_root_calls;
static int s_role_change_calls;
static module_role_t s_child_service_role = MODULE_ROLE_UNASSIGNED;
static module_role_t s_sensor_service_role = MODULE_ROLE_UNASSIGNED;
static int s_role_changes_to_leaf;

typedef struct {
    int count;
    pogo_port_event_t last_event;
    pogo_port_error_t last_error;
} event_sink_t;

static event_sink_t s_child_sink, s_parent_sink, s_reporter_sink;

static void sink_record(event_sink_t *sink, pogo_port_event_t event, const pogo_port_event_data_t *data)
{
    sink->count++;
    sink->last_event = event;
    sink->last_error = (event == POGO_PORT_EVENT_ERROR && data != NULL) ? data->error_code : POGO_PORT_ERR_NONE;
}

esp_err_t thread_manager_stop(void)
{
    s_thread_stop_calls++;
    return ESP_OK;
}

esp_err_t thread_manager_start_as_root(void)
{
    s_thread_start_as_root_calls++;
    return ESP_OK;
}

otDeviceRole thread_manager_get_role(void)
{
    return OT_DEVICE_ROLE_LEADER; /* only the DEV_ROOT fixture's watcher ever asks */
}

esp_err_t thread_manager_get_mesh_local_eid(otIp6Address *out_addr)
{
    memset(out_addr, 0, sizeof(*out_addr));
    out_addr->mFields.m8[0] = 0xFD;
    out_addr->mFields.m8[15] = 0x01;
    return ESP_OK;
}

void child_service_on_role_change(module_role_t new_role, module_role_t old_role)
{
    (void)old_role;
    s_role_change_calls++;
    s_child_service_role = new_role;
    if (new_role == MODULE_ROLE_LEAF) {
        s_role_changes_to_leaf++;
    }
}

void child_service_on_port_event(pogo_port_id_t port_id, pogo_port_event_t event, const pogo_port_event_data_t *data)
{
    (void)port_id;
    sink_record(&s_child_sink, event, data);
}

uint8_t child_service_get_own_topology_depth(void)
{
    return 1;
}

void parent_service_on_port_event(pogo_port_id_t port_id, pogo_port_event_t event, const pogo_port_event_data_t *data)
{
    (void)port_id;
    sink_record(&s_parent_sink, event, data);
}

void topology_reporter_on_port_event(pogo_port_id_t port_id, pogo_port_event_t event,
                                      const pogo_port_event_data_t *data)
{
    if (port_id == POGO_PORT_DOWNSTREAM) {
        sink_record(&s_reporter_sink, event, data);
    }
}

void sensor_service_on_role_change(module_role_t new_role, module_role_t old_role)
{
    (void)old_role;
    s_sensor_service_role = new_role;
}

void sensor_service_get_module_uuid(uint8_t out_uuid[SENSOR_SERVICE_UUID_LEN])
{
    memset(out_uuid, 0x5A, SENSOR_SERVICE_UUID_LEN);
}

/* --- event injection: always through the REAL dispatcher --- */

static void inject(pogo_port_id_t port, pogo_port_event_t event, pogo_peer_type_t peer_type, pogo_port_error_t error)
{
    pogo_port_event_data_t data;
    memset(&data, 0, sizeof(data));
    data.peer_type = peer_type;
    data.error_code = error;
    module_wiring_port_event_dispatcher(port, event, &data, NULL);
}

static void up_attached(void) { inject(POGO_PORT_UPSTREAM, POGO_PORT_EVENT_ATTACHED, 0, 0); }
static void up_detached(void) { inject(POGO_PORT_UPSTREAM, POGO_PORT_EVENT_DETACHED, 0, 0); }
static void up_ready(pogo_peer_type_t t) { inject(POGO_PORT_UPSTREAM, POGO_PORT_EVENT_PEER_READY, t, 0); }
static void up_error(pogo_port_error_t e) { inject(POGO_PORT_UPSTREAM, POGO_PORT_EVENT_ERROR, 0, e); }
static void down_attached(void) { inject(POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_ATTACHED, 0, 0); }
static void down_detached(void) { inject(POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_DETACHED, 0, 0); }
static void down_ready(pogo_peer_type_t t) { inject(POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_PEER_READY, t, 0); }
static void down_error(pogo_port_error_t e) { inject(POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_ERROR, 0, e); }

static void boot(module_dev_mode_t dev_mode)
{
    s_thread_stop_calls = 0;
    s_thread_start_as_root_calls = 0;
    s_role_change_calls = 0;
    s_role_changes_to_leaf = 0;
    s_child_service_role = MODULE_ROLE_UNASSIGNED;
    s_sensor_service_role = MODULE_ROLE_UNASSIGNED;
    memset(&s_child_sink, 0, sizeof(s_child_sink));
    memset(&s_parent_sink, 0, sizeof(s_parent_sink));
    memset(&s_reporter_sink, 0, sizeof(s_reporter_sink));
    role_manager_init(dev_mode, module_wiring_on_role_change, NULL);
}

/* What child_service.c does at its own JOINING -> JOINED step (the real one
 * runs in tools/test_module_downstream_fault_isolation.c): this module's own
 * Thread join completed. */
static void own_thread_join_completes(void)
{
    role_manager_set_upstream_ready(true);
}

static void bring_up_joined_leaf(void)
{
    boot(MODULE_DEV_MODE_AUTO);
    up_attached();
    up_ready(POGO_PEER_TYPE_POWER_BASE);
    own_thread_join_completes();
}

static void bring_up_joined_intermediate(void)
{
    bring_up_joined_leaf();
    down_attached();
    down_ready(POGO_PEER_TYPE_MODULE);
}

static bool provisioning_allowed(void)
{
    const parent_service_context_t *ctx = module_wiring_parent_context();
    return ctx->provisioning_allowed(ctx->ctx);
}

/* What this glue itself decides: Thread never stopped, upstream_ready
 * intact. (The collector cache and the topology gate are owned by
 * child_service/topology_reporter, which are stand-ins here -- their
 * preservation is checked against the real components in
 * tools/test_module_downstream_fault_isolation.c.) */
#define CHECK_OWN_PATH_INTACT(label)                                                                                 \
    do {                                                                                                             \
        CHECK(s_thread_stop_calls == 0, label ": Thread is NOT stopped");                                            \
        CHECK(role_manager_is_upstream_ready(), label ": upstream_ready is kept");                                   \
    } while (0)

/* --- A: joined LEAF, a module docks below, its first handshake times out --- */
static void scenario_a_leaf_downstream_dock_timeout(void)
{
    printf("\n-- A: joined LEAF, DOWNSTREAM attaches then DOCK_TIMEOUT --\n");
    bring_up_joined_leaf();
    CHECK(role_manager_get_role() == MODULE_ROLE_LEAF, "precondition: joined LEAF");
    int changes_before = s_role_change_calls;

    down_attached();
    down_error(POGO_PORT_ERR_DOCK_TIMEOUT);

    CHECK(role_manager_get_role() == MODULE_ROLE_LEAF, "role stays LEAF (not ROLE_ERROR)");
    CHECK(s_role_change_calls == changes_before, "no role-change callback at all for the DOWNSTREAM timeout");
    CHECK_OWN_PATH_INTACT("after DOWNSTREAM DOCK_TIMEOUT");
    CHECK(s_sensor_service_role == MODULE_ROLE_LEAF, "sensor_service keeps a sending role");
    CHECK(!provisioning_allowed(), "DOWNSTREAM provisioning stays disallowed for the failed link");
    CHECK(s_parent_sink.last_event == POGO_PORT_EVENT_ERROR && s_parent_sink.last_error == POGO_PORT_ERR_DOCK_TIMEOUT,
          "parent_service still receives the ERROR (it cancels its own session)");
    CHECK(s_reporter_sink.last_event == POGO_PORT_EVENT_ERROR,
          "topology_reporter still receives the ERROR (downstream is not reported connected)");
    CHECK(s_child_sink.count == 2, "child_service saw only its own two UPSTREAM events, nothing from DOWNSTREAM");
}

/* --- B: joined INTERMEDIATE, the established child stops answering PING --- */
static void scenario_b_intermediate_downstream_liveness_lost(void)
{
    printf("\n-- B: joined INTERMEDIATE, DOWNSTREAM LIVENESS_LOST --\n");
    bring_up_joined_intermediate();
    CHECK(role_manager_get_role() == MODULE_ROLE_INTERMEDIATE, "precondition: joined INTERMEDIATE");
    int changes_before = s_role_change_calls;

    down_error(POGO_PORT_ERR_LIVENESS_LOST);

    CHECK(role_manager_get_role() == MODULE_ROLE_INTERMEDIATE, "role stays INTERMEDIATE (not ROLE_ERROR, not LEAF)");
    CHECK(s_role_change_calls == changes_before, "no role-change callback for the DOWNSTREAM liveness loss");
    CHECK_OWN_PATH_INTACT("after DOWNSTREAM LIVENESS_LOST");
    CHECK(s_parent_sink.last_event == POGO_PORT_EVENT_ERROR && s_parent_sink.last_error == POGO_PORT_ERR_LIVENESS_LOST,
          "parent_service receives the ERROR and invalidates the child session");

    /* The peer_type remembered from the earlier PEER_READY must not come back
     * as "a MODULE is ready below" once the link really goes away and a new,
     * still-unanswered attach follows. */
    down_detached();
    CHECK(role_manager_get_role() == MODULE_ROLE_LEAF, "a real DOWNSTREAM detach -> LEAF");
    down_attached();
    CHECK(role_manager_get_role() == MODULE_ROLE_LEAF, "re-attach with no handshake yet stays LEAF");
    down_error(POGO_PORT_ERR_DOCK_TIMEOUT);
    CHECK(role_manager_get_role() == MODULE_ROLE_LEAF,
          "a failed handshake after re-attach does not resurrect INTERMEDIATE from the old peer_type");
    CHECK_OWN_PATH_INTACT("across detach / re-attach / timeout");
}

/* --- C: repeated errors, retries, then PEER_READY --- */
static void scenario_c_repeated_errors_then_recovery(void)
{
    printf("\n-- C1: LEAF, repeated DOWNSTREAM DOCK_TIMEOUT, then PEER_READY --\n");
    bring_up_joined_leaf();
    down_attached();
    int changes_before = s_role_change_calls;
    for (int i = 0; i < 4; i++) {
        down_error(POGO_PORT_ERR_DOCK_TIMEOUT);
        CHECK(role_manager_get_role() == MODULE_ROLE_LEAF, "each retry cycle's DOCK_TIMEOUT leaves the role at LEAF");
    }
    CHECK(s_role_change_calls == changes_before, "four error cycles produce zero role-change callbacks");
    down_ready(POGO_PEER_TYPE_MODULE);
    CHECK(role_manager_get_role() == MODULE_ROLE_INTERMEDIATE, "PEER_READY(MODULE) -> INTERMEDIATE");
    CHECK(s_role_change_calls == changes_before + 1, "exactly one role change: the normal LEAF -> INTERMEDIATE");
    CHECK_OWN_PATH_INTACT("after recovery from LEAF");
    CHECK(provisioning_allowed(), "DOWNSTREAM provisioning is allowed again");

    printf("\n-- C2: INTERMEDIATE, LIVENESS_LOST + repeated DOCK_TIMEOUT, then PEER_READY --\n");
    bring_up_joined_intermediate();
    changes_before = s_role_change_calls;
    down_error(POGO_PORT_ERR_LIVENESS_LOST);
    down_error(POGO_PORT_ERR_DOCK_TIMEOUT);
    down_error(POGO_PORT_ERR_DOCK_TIMEOUT);
    down_ready(POGO_PEER_TYPE_MODULE);
    CHECK(role_manager_get_role() == MODULE_ROLE_INTERMEDIATE, "back to a ready INTERMEDIATE");
    CHECK(s_role_change_calls == changes_before,
          "zero role changes across fault and recovery -- no link-mode re-apply, no rejoin of its own");
    CHECK_OWN_PATH_INTACT("after recovery from INTERMEDIATE");
    CHECK(provisioning_allowed(), "DOWNSTREAM provisioning is allowed again");
}

/* --- D: UPSTREAM DETACHED/ERROR while DOWNSTREAM is already faulted --- */
static void scenario_d_upstream_loss_during_downstream_fault(void)
{
    printf("\n-- D1: DOWNSTREAM fault, then UPSTREAM DETACHED --\n");
    bring_up_joined_intermediate();
    down_error(POGO_PORT_ERR_LIVENESS_LOST);
    CHECK(s_thread_stop_calls == 0, "precondition: the DOWNSTREAM fault alone did not stop Thread");
    up_detached();
    CHECK(role_manager_get_role() == MODULE_ROLE_UNASSIGNED, "UPSTREAM DETACHED -> UNASSIGNED");
    CHECK(s_thread_stop_calls == 1, "Thread is stopped exactly once");
    CHECK(!role_manager_is_upstream_ready(), "upstream_ready is cleared");
    CHECK(s_child_sink.last_event == POGO_PORT_EVENT_DETACHED,
          "child_service receives DETACHED (it clears session + collector)");

    printf("\n-- D2: DOWNSTREAM fault, then UPSTREAM LIVENESS_LOST --\n");
    bring_up_joined_intermediate();
    down_error(POGO_PORT_ERR_LIVENESS_LOST);
    up_error(POGO_PORT_ERR_LIVENESS_LOST);
    CHECK(role_manager_get_role() == MODULE_ROLE_ERROR, "UPSTREAM ERROR -> ROLE_ERROR even while DOWNSTREAM is faulted");
    CHECK(s_thread_stop_calls == 1, "Thread is stopped exactly once");
    CHECK(!role_manager_is_upstream_ready(), "upstream_ready is cleared");
    CHECK(s_child_sink.last_event == POGO_PORT_EVENT_ERROR, "child_service receives the UPSTREAM ERROR");

    printf("\n-- D3: LEAF with a never-resolved DOWNSTREAM, then UPSTREAM DOCK_TIMEOUT --\n");
    bring_up_joined_leaf();
    down_attached();
    down_error(POGO_PORT_ERR_DOCK_TIMEOUT);
    up_error(POGO_PORT_ERR_DOCK_TIMEOUT);
    CHECK(role_manager_get_role() == MODULE_ROLE_ERROR, "UPSTREAM DOCK_TIMEOUT -> ROLE_ERROR");
    CHECK(s_thread_stop_calls == 1 && !role_manager_is_upstream_ready(), "Thread stopped, upstream_ready cleared");

    printf("\n-- D4: UPSTREAM error first, DOWNSTREAM fault second --\n");
    bring_up_joined_intermediate();
    up_error(POGO_PORT_ERR_LIVENESS_LOST);
    down_error(POGO_PORT_ERR_LIVENESS_LOST);
    CHECK(role_manager_get_role() == MODULE_ROLE_ERROR, "the UPSTREAM error keeps priority");
    CHECK(s_thread_stop_calls == 1 && !role_manager_is_upstream_ready(), "Thread stays stopped, upstream_ready cleared");
}

/* --- E: after an UPSTREAM error, DOWNSTREAM recovering changes nothing --- */
static void scenario_e_downstream_recovery_after_upstream_error(void)
{
    printf("\n-- E: UPSTREAM ERROR, then DOWNSTREAM PEER_READY --\n");
    bring_up_joined_intermediate();
    down_error(POGO_PORT_ERR_LIVENESS_LOST);
    up_error(POGO_PORT_ERR_LIVENESS_LOST);
    int stops = s_thread_stop_calls;

    down_ready(POGO_PEER_TYPE_MODULE);
    CHECK(role_manager_get_role() == MODULE_ROLE_ERROR, "role stays ROLE_ERROR");
    CHECK(!role_manager_is_upstream_ready(), "upstream_ready stays false -- the topology gate cannot reopen");
    CHECK(!provisioning_allowed(), "DOWNSTREAM provisioning stays disallowed");
    CHECK(s_thread_stop_calls == stops, "no further Thread action either way");

    down_error(POGO_PORT_ERR_DOCK_TIMEOUT);
    down_ready(POGO_PEER_TYPE_MODULE);
    CHECK(role_manager_get_role() == MODULE_ROLE_ERROR && !role_manager_is_upstream_ready(),
          "another DOWNSTREAM fault/recovery cycle still changes nothing");
}

/* --- F: not provisioned / UPSTREAM not ready --- */
static void scenario_f_no_upstream(void)
{
    printf("\n-- F1: nothing docked UPSTREAM, DOWNSTREAM events only --\n");
    boot(MODULE_DEV_MODE_AUTO);
    down_attached();
    down_error(POGO_PORT_ERR_DOCK_TIMEOUT);
    down_ready(POGO_PEER_TYPE_MODULE);
    CHECK(role_manager_get_role() == MODULE_ROLE_UNASSIGNED, "role stays UNASSIGNED");
    CHECK(!role_manager_is_upstream_ready() && !provisioning_allowed(), "not ready, no provisioning");
    CHECK(s_role_change_calls == 0, "no role-change callback: nothing was started or stopped");

    printf("\n-- F2: UPSTREAM attached but handshake unresolved --\n");
    up_attached();
    down_error(POGO_PORT_ERR_LIVENESS_LOST);
    down_ready(POGO_PEER_TYPE_MODULE);
    CHECK(role_manager_get_role() == MODULE_ROLE_UNASSIGNED, "role stays UNASSIGNED until UPSTREAM resolves");
    CHECK(!role_manager_is_upstream_ready() && !provisioning_allowed(), "not ready, no provisioning");

    printf("\n-- F3: UPSTREAM resolves while DOWNSTREAM is mid-fault, own join not done yet --\n");
    boot(MODULE_DEV_MODE_AUTO);
    up_attached();
    down_attached();
    down_error(POGO_PORT_ERR_DOCK_TIMEOUT);
    CHECK(role_manager_get_role() == MODULE_ROLE_UNASSIGNED, "DOWNSTREAM fault alone leaves the role UNASSIGNED");
    up_ready(POGO_PEER_TYPE_POWER_BASE);
    CHECK(role_manager_get_role() == MODULE_ROLE_LEAF,
          "UPSTREAM PEER_READY gives the module its own role; an unresolved DOWNSTREAM is not a ready MODULE");
    CHECK(!role_manager_is_upstream_ready(), "upstream_ready is still false -- only a real join sets it");
    CHECK(!provisioning_allowed(), "no DOWNSTREAM provisioning before this module's own join");
    CHECK(s_thread_stop_calls == 0, "nothing to stop");

    printf("\n-- F4: ready MODULE below, but this module's own join is not done --\n");
    boot(MODULE_DEV_MODE_AUTO);
    up_attached();
    up_ready(POGO_PEER_TYPE_MODULE);
    down_attached();
    down_ready(POGO_PEER_TYPE_MODULE);
    CHECK(role_manager_get_role() == MODULE_ROLE_INTERMEDIATE, "role is INTERMEDIATE");
    CHECK(!role_manager_is_upstream_ready() && !provisioning_allowed(),
          "but DOWNSTREAM provisioning waits for this module's own join");
}

/* --- G: permanent errors and forced development roles keep their policy --- */
static void scenario_g_permanent_errors_and_dev_modes(void)
{
    static const struct {
        pogo_port_error_t code;
        const char *name;
    } permanent[] = {
        {POGO_PORT_ERR_PEER_TYPE_CONFLICT, "PEER_TYPE_CONFLICT"},
        {POGO_PORT_ERR_PROTOCOL_VERSION, "PROTOCOL_VERSION"},
        {POGO_PORT_ERR_CAPABILITY_MISMATCH, "CAPABILITY_MISMATCH"},
    };
    char msg[160];
    for (size_t i = 0; i < sizeof(permanent) / sizeof(permanent[0]); i++) {
        printf("\n-- G: DOWNSTREAM %s (permanent) --\n", permanent[i].name);
        bring_up_joined_leaf();
        down_attached();
        down_error(permanent[i].code);
        snprintf(msg, sizeof(msg), "LEAF + DOWNSTREAM %s -> ROLE_ERROR", permanent[i].name);
        CHECK(role_manager_get_role() == MODULE_ROLE_ERROR, msg);
        CHECK(s_thread_stop_calls == 1 && !role_manager_is_upstream_ready(), "Thread stopped, upstream_ready cleared");

        bring_up_joined_intermediate();
        down_error(POGO_PORT_ERR_LIVENESS_LOST);
        CHECK(s_thread_stop_calls == 0, "a transient fault first: still isolated");
        down_error(permanent[i].code);
        snprintf(msg, sizeof(msg), "transient fault escalating to %s -> ROLE_ERROR", permanent[i].name);
        CHECK(role_manager_get_role() == MODULE_ROLE_ERROR, msg);
        CHECK(s_thread_stop_calls == 1 && !role_manager_is_upstream_ready(), "Thread stopped, upstream_ready cleared");
    }

    printf("\n-- G: DOWNSTREAM ERROR with no recognised transient code --\n");
    bring_up_joined_intermediate();
    down_error(POGO_PORT_ERR_NONE);
    CHECK(role_manager_get_role() == MODULE_ROLE_ERROR, "an unclassified DOWNSTREAM error is NOT isolated");
    CHECK(s_thread_stop_calls == 1, "Thread stopped");

    bring_up_joined_intermediate();
    module_wiring_port_event_dispatcher(POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_ERROR, NULL, NULL);
    CHECK(role_manager_get_role() == MODULE_ROLE_ERROR, "a DOWNSTREAM error with no event data at all is NOT isolated");
    CHECK(s_thread_stop_calls == 1, "Thread stopped");

    printf("\n-- G: DEV_ROOT forced role --\n");
    boot(MODULE_DEV_MODE_DEV_ROOT);
    CHECK(role_manager_get_role() == MODULE_ROLE_ROOT, "DEV_ROOT forces ROOT");
    CHECK(s_thread_start_as_root_calls == 1, "Thread is started as root exactly once");
    down_attached();
    down_error(POGO_PORT_ERR_DOCK_TIMEOUT);
    down_error(POGO_PORT_ERR_CAPABILITY_MISMATCH);
    up_error(POGO_PORT_ERR_LIVENESS_LOST);
    CHECK(role_manager_get_role() == MODULE_ROLE_ROOT, "port events never change a forced role");
    CHECK(s_thread_stop_calls == 0, "Thread is not stopped");
    CHECK(s_parent_sink.count == 3, "parent_service still receives every DOWNSTREAM event");
    bool ready = false;
    for (int i = 0; i < 100 && !ready; i++) { /* the real root_ready_watcher_task polls every 200ms */
        ready = role_manager_is_upstream_ready();
        if (!ready) {
            usleep(10 * 1000);
        }
    }
    CHECK(ready, "DEV_ROOT becomes upstream-ready through its own watcher, as before");
    CHECK(provisioning_allowed(), "DEV_ROOT may provision its DOWNSTREAM, as before");

    printf("\n-- G: DEV_LEAF forced role --\n");
    boot(MODULE_DEV_MODE_DEV_LEAF);
    CHECK(role_manager_get_role() == MODULE_ROLE_LEAF, "DEV_LEAF forces LEAF");
    up_error(POGO_PORT_ERR_DOCK_TIMEOUT);
    down_error(POGO_PORT_ERR_DOCK_TIMEOUT);
    CHECK(role_manager_get_role() == MODULE_ROLE_LEAF, "port events never change a forced role");
    CHECK(s_thread_stop_calls == 0, "Thread is not stopped");
    CHECK(s_child_sink.last_event == POGO_PORT_EVENT_ERROR, "child_service still receives the UPSTREAM ERROR");
}

/* --- H: physically attached below, but faulted/retrying --- */
static void scenario_h_attached_but_faulted_is_not_an_empty_port(void)
{
    printf("\n-- H: INTERMEDIATE whose child is attached but faulted is never demoted to LEAF --\n");
    bring_up_joined_intermediate();
    int to_leaf_before = s_role_changes_to_leaf;
    down_error(POGO_PORT_ERR_LIVENESS_LOST);
    for (int i = 0; i < 5; i++) {
        down_error(POGO_PORT_ERR_DOCK_TIMEOUT);
        CHECK(role_manager_get_role() == MODULE_ROLE_INTERMEDIATE, "still INTERMEDIATE during the retry cycle");
    }
    CHECK(s_role_changes_to_leaf == to_leaf_before,
          "child_service was never told LEAF -- no sleepy-end-device link mode while a module is still docked");
    CHECK(s_child_service_role == MODULE_ROLE_INTERMEDIATE && s_sensor_service_role == MODULE_ROLE_INTERMEDIATE,
          "child_service/sensor_service keep the INTERMEDIATE role");
    CHECK_OWN_PATH_INTACT("throughout the faulted period");

    down_detached();
    CHECK(role_manager_get_role() == MODULE_ROLE_LEAF, "only a real DOWNSTREAM detach (empty port) gives LEAF");
    CHECK(s_role_changes_to_leaf == to_leaf_before + 1, "LEAF link mode is applied once, for the real detach");
    CHECK_OWN_PATH_INTACT("after the real detach");
}

int main(void)
{
    scenario_a_leaf_downstream_dock_timeout();
    scenario_b_intermediate_downstream_liveness_lost();
    scenario_c_repeated_errors_then_recovery();
    scenario_d_upstream_loss_during_downstream_fault();
    scenario_e_downstream_recovery_after_upstream_error();
    scenario_f_no_upstream();
    scenario_g_permanent_errors_and_dev_modes();
    scenario_h_attached_but_faulted_is_not_an_empty_port();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
