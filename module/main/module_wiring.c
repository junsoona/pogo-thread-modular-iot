#include "module_wiring.h"

#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sensor_service/sensor_service.h"
#include "thread_manager/thread_manager.h"
#include "child_service/child_service.h"
#include "topology_reporter/topology_reporter.h"
#include "collector_endpoint/collector_endpoint.h"
#include "openthread/coap.h" /* OT_DEFAULT_COAP_PORT -- this module's own CoAP port when it is itself the
                                 collector (MODULE_DEV_MODE_DEV_ROOT fixture only, see module_parent_get_collector()) */

static const char *TAG = "module_main";

void module_wiring_port_event_dispatcher(pogo_port_id_t port_id, pogo_port_event_t event,
                                          const pogo_port_event_data_t *data, void *user_ctx)
{
    (void)user_ctx;
    role_manager_on_port_event(port_id, event, data);
    if (port_id == POGO_PORT_UPSTREAM) {
        child_service_on_port_event(port_id, event, data);
    } else {
        parent_service_on_port_event(port_id, event, data);
    }
    topology_reporter_on_port_event(port_id, event, data);
}

/*
 * parent_service_context_t wiring for a sensor module (see
 * parent_service.h's header comment for why this indirection exists instead
 * of parent_service reaching into role_manager/child_service directly -- the
 * same component is reused verbatim by power_base/main/app_main.c with a
 * completely different context). A sensor module in AUTO mode is never
 * MODULE_ROLE_ROOT's provisioning parent by policy (role_manager_get_role()
 * simply never returns ROOT there -- see role_manager.c and
 * docs/pogo-interface.md's determination table); MODULE_DEV_MODE_DEV_ROOT's
 * forced-ROOT regression fixture is the only path where get_own_depth()
 * returns 0 for a module.
 */
static bool module_parent_provisioning_allowed(void *ctx)
{
    (void)ctx;
    module_role_t role = role_manager_get_role();
    return (role == MODULE_ROLE_ROOT || role == MODULE_ROLE_INTERMEDIATE) && role_manager_is_upstream_ready();
}

static uint8_t module_parent_get_own_depth(void *ctx)
{
    (void)ctx;
    return (role_manager_get_role() == MODULE_ROLE_ROOT) ? 0 : child_service_get_own_topology_depth();
}

static void module_parent_get_own_uuid(uint8_t out_uuid[POGO_UUID_LEN], void *ctx)
{
    (void)ctx;
    /* SENSOR_SERVICE_UUID_LEN == POGO_UUID_LEN (both 8) -- see sensor_service.h. */
    sensor_service_get_module_uuid(out_uuid);
}

static const char *module_parent_role_name(void *ctx)
{
    (void)ctx;
    return role_manager_role_to_str(role_manager_get_role());
}

/*
 * Fills *out with the collector endpoint this module hands to a docking
 * child via SET_COLLECTOR (see parent_service_context_t.get_collector's doc
 * comment).
 *
 * MODULE_ROLE_ROOT (the MODULE_DEV_MODE_DEV_ROOT module-to-module regression
 * fixture ONLY -- see docs/pogo-interface.md's "development compat mode";
 * role_manager_get_role() never returns ROOT via the AUTO path, so this
 * branch is unreachable there): there is no power base in this topology at
 * all -- this module's OWN Thread Mesh-Local EID + CoAP port IS the
 * collector, exactly mirroring power_base/main/app_main.c's own
 * pb_get_collector(). This is what lets DEV_LEAF's sensor_transport keep
 * sending its "/s" samples to DEV_ROOT, same destination as before this
 * follow-up (which used to be "the current Leader", always DEV_ROOT itself
 * in that topology) -- see docs/power-base.md.
 *
 * Otherwise (INTERMEDIATE): forwards the ORIGINAL collector endpoint this
 * module itself received from ITS OWN upstream via SET_COLLECTOR
 * (components/collector_endpoint's snapshot), completely unmodified --
 * never substituting this module's own address (section 5 of the follow-up
 * spec: "자신의 IPv6 주소로 바꾸면 안 된다"). Guaranteed already populated by
 * the time this is ever called: module_parent_provisioning_allowed() only
 * returns true once role_manager_is_upstream_ready(), which requires this
 * module's OWN child_service to have already completed SET_COLLECTOR (see
 * child_service.c's state ordering).
 */
static bool module_parent_get_collector(pogo_collector_endpoint_t *out, void *ctx)
{
    (void)ctx;
    if (role_manager_get_role() == MODULE_ROLE_ROOT) {
        otIp6Address mleid;
        if (thread_manager_get_mesh_local_eid(&mleid) != ESP_OK) {
            return false;
        }
        sensor_service_get_module_uuid(out->power_base_uuid);
        memcpy(out->addr, mleid.mFields.m8, sizeof(out->addr));
        out->port = OT_DEFAULT_COAP_PORT;
        out->generation = 1;
        return pogo_collector_endpoint_is_valid(out);
    }

    collector_endpoint_snapshot_t snap;
    if (!collector_endpoint_get(&snap) || !snap.valid) {
        return false;
    }
    memcpy(out->power_base_uuid, snap.power_base_uuid, POGO_UUID_LEN);
    memcpy(out->addr, snap.addr, sizeof(out->addr));
    out->port = snap.port;
    out->generation = snap.generation;
    return true;
}

static const parent_service_context_t s_module_parent_ctx = {
    .provisioning_allowed = module_parent_provisioning_allowed,
    .get_own_depth = module_parent_get_own_depth,
    .get_own_uuid = module_parent_get_own_uuid,
    .role_name = module_parent_role_name,
    .is_power_base = false,
    .get_collector = module_parent_get_collector,
    .ack_timeout_ms = 0, /* use CONFIG_POGO_ACK_TIMEOUT_MS, unchanged from before this refactor */
    .ctx = NULL,
};

const parent_service_context_t *module_wiring_parent_context(void)
{
    return &s_module_parent_ctx;
}

/*
 * Bumped every time a NEW root_ready_watcher_task is started, and every time
 * ROOT stops being the current role. A watcher only ever acts (setting
 * upstream_ready=true, or logging the timeout error) if the generation it
 * was launched with still matches -- so a stale watcher from a role we've
 * since left cannot act on a role change that has nothing to do with it
 * (e.g. this module later becomes INTERMEDIATE/LEAF and legitimately
 * reaches CHILD/ROUTER for a completely different, unrelated reason).
 * There is at most one watcher whose result matters at any time; an old one
 * notices the mismatch within one poll interval (<=200ms) and exits.
 */
static volatile uint32_t s_root_watch_generation;

static void root_ready_watcher_task(void *arg)
{
    uint32_t my_gen = (uint32_t)(uintptr_t)arg;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
    while ((int32_t)(xTaskGetTickCount() - deadline) < 0) {
        if (my_gen != s_root_watch_generation) {
            vTaskDelete(NULL); /* superseded: role left ROOT or a newer watcher took over */
            return;
        }
        otDeviceRole role = thread_manager_get_role();
        if (role == OT_DEVICE_ROLE_LEADER || role == OT_DEVICE_ROLE_ROUTER) {
            if (my_gen == s_root_watch_generation) {
                ESP_LOGI(TAG, "ROOT ready: otDeviceRole=%u -- downstream provisioning may now proceed",
                         (unsigned)role);
                role_manager_set_upstream_ready(true);
            }
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (my_gen == s_root_watch_generation) {
        ESP_LOGE(TAG, "ROLE_TRANSITION_ERROR: ROOT never reached Leader/Router within 30s");
    }
    vTaskDelete(NULL);
}

void module_wiring_on_role_change(module_role_t new_role, module_role_t old_role, void *ctx)
{
    (void)ctx;
    child_service_on_role_change(new_role, old_role);
    sensor_service_on_role_change(new_role, old_role);
    /* topology_reporter does NOT hook this callback: role_manager's role
     * settles (LEAF/INTERMEDIATE) independently of, and generally BEFORE,
     * role_manager_is_upstream_ready() becoming true (that only happens once
     * child_service finishes the full Thread join) -- see
     * topology_reporter.c's header comment for why it instead polls
     * role_manager_is_upstream_ready()/role_manager_get_role() on its own
     * short tick rather than reacting to this callback. */

    if (old_role == MODULE_ROLE_ROOT && new_role != MODULE_ROLE_ROOT) {
        /* Leaving ROOT: invalidate any in-flight watcher immediately. */
        s_root_watch_generation++;
        role_manager_set_upstream_ready(false);
    }

    if (new_role == MODULE_ROLE_UNASSIGNED || new_role == MODULE_ROLE_ERROR) {
        /* Cancel any in-flight role transition (a ROOT watch, or a pending
         * child_service join attempt loses its role_manager gate) and take
         * Thread down: UPSTREAM having just detached (the only way to reach
         * UNASSIGNED from a previously-valid role) means the physical
         * relationship this Thread membership was provisioned through is
         * gone, so keep the physical (POGO) and Thread layers in the same
         * state -- see docs/state-machine.md. ROLE_ERROR is the same for an
         * UPSTREAM error or a permanent DOWNSTREAM one; a transient
         * DOWNSTREAM DOCK_TIMEOUT/LIVENESS_LOST never gets here at all
         * (role_manager holds the role instead). module_config and the
         * sensor_provider selection are untouched; a fresh POGO handshake
         * and a new SET_DATASET are required before rejoining, never an
         * automatic resume. */
        s_root_watch_generation++;
        ESP_LOGW(TAG, "role -> %s: stopping Thread (no auto-rejoin until a fresh handshake) -- see docs/state-machine.md",
                 role_manager_role_to_str(new_role));
        if (thread_manager_stop() != ESP_OK) {
            ESP_LOGE(TAG, "ROLE_TRANSITION_ERROR: thread_manager_stop failed while entering %s",
                     role_manager_role_to_str(new_role));
        }
        role_manager_set_upstream_ready(false);
    } else if (new_role == MODULE_ROLE_ROOT && old_role != MODULE_ROLE_ROOT) {
        ESP_LOGI(TAG, "role -> ROOT: starting Thread as Leader/Router candidate");
        if (thread_manager_start_as_root() == ESP_OK) {
            uint32_t gen = ++s_root_watch_generation;
            xTaskCreate(root_ready_watcher_task, "root_ready", 3072, (void *)(uintptr_t)gen, 4, NULL);
        } else {
            ESP_LOGE(TAG, "ROLE_TRANSITION_ERROR: thread_manager_start_as_root failed");
        }
    }
    /* INTERMEDIATE/LEAF (not coming from UNASSIGNED/ERROR, handled above):
     * child_service reacts to its own UPSTREAM handshake (IDENTIFY..
     * SET_DATASET) and calls thread_manager_join_with_dataset() itself once
     * ready -- nothing to do here. DOWNSTREAM-only changes (LEAF<->INTERMEDIATE)
     * never force a Thread stop: this module's own Thread membership does
     * not depend on whether anything is docked below it. */
}
