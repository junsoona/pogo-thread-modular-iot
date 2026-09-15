/*
 * Common module entry point -- ONE application binary (pogo_module.bin) for
 * every ESP32-H2, regardless of where it ends up docked (see section 3 of
 * the implementation spec). "upper"/"lower" no longer exist as compile-time
 * identities: this file only wires together the components that derive and
 * react to a runtime role.
 *
 * Boot sequence:
 *   1. nvs_flash_init()          -- dynamic "nvs" partition (Thread dataset,
 *                                    topology/session state).
 *   2. sensor_provider_register() x N -- register every real driver this
 *                                    image is compiled with (bh1750, sht45, ...).
 *   3. module_config_load()      -- fixed "cfg" partition (sensor pairing,
 *                                    port wiring). CONFIG_ERROR halts here.
 *   4. module_config_load_dev_mode() -- AUTO / DEV_ROOT / DEV_LEAF.
 *   5. thread_manager_init()     -- OpenThread FTD stack up, no network yet.
 *   6. sensor_service_init()     -- initializes the ONE paired sensor driver
 *                                    exactly once (section 3); never touched
 *                                    directly by child_service any more.
 *   7. topology_gate_init()       -- creates the cross-task-safe data-plane
 *                                    readiness gate (real-hardware
 *                                    first-sample/topology race fix, see
 *                                    components/topology_gate); must exist
 *                                    before sensor_transport_init() or
 *                                    topology_reporter_init() run.
 *   7a. collector_endpoint_init() -- creates the cross-task-safe collector
 *                                    (power base) endpoint cache (see
 *                                    components/collector_endpoint); must
 *                                    exist before sensor_transport_init()/
 *                                    topology_reporter_init() (readers) or
 *                                    child_service_init()/parent_service_init()
 *                                    (writer/forwarder) run. sensor_transport
 *                                    and topology_reporter both target this
 *                                    endpoint -- never "the current Thread
 *                                    Leader" -- so this module's data
 *                                    destination never changes while its own
 *                                    Thread role fluctuates between CHILD/
 *                                    ROUTER/LEADER (see docs/power-base.md).
 *   7b. sensor_transport_init()  -- starts the CoAP sample-receiving
 *                                    resource (section 8); the sample-ready
 *                                    callback then wires sensor_service's
 *                                    periodic samples into it. In AUTO mode,
 *                                    also gates every "/s" send on the
 *                                    topology_gate above.
 *   8. role_manager_init()       -- starts deriving ROOT/INTERMEDIATE/LEAF
 *                                    (or is forced by a dev override).
 *   9. pogo_port_init() x2       -- UPSTREAM/DOWNSTREAM, both routed through
 *                                    one dispatcher into role_manager AND
 *                                    child_service (UPSTREAM only) /
 *                                    parent_service (DOWNSTREAM only).
 *  10. child_service_init() / parent_service_init() -- always both; each
 *                                    only ever acts on its own port's events.
 *                                    parent_service_init() takes a small
 *                                    context struct (see parent_service.h) so
 *                                    the SAME component is reused unmodified
 *                                    by power_base/main/app_main.c.
 *  11. topology_reporter_init() -- reports this module's role/parent/depth/
 *                                    connection state to the power base over
 *                                    Thread CoAP ("/t", see docs/power-base.md);
 *                                    a sensor module never runs the receiving
 *                                    side of that resource, only power_base does.
 *                                    Also the sole writer of the topology_gate
 *                                    from step 7: opens it once its own
 *                                    ATTACHED report gets a real CoAP 2.xx
 *                                    response, closes it on upstream/Thread/
 *                                    role loss or a fresh provisioning session.
 *  12. pogo_port_enable() x2     -- only for ports this board actually wires
 *                                    (module_config-driven; a dev board's
 *                                    "missing" port is simply not enabled).
 */
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pogo_proto/pogo_crc16.h"
#include "module_config/module_config.h"
#include "sensor_provider/sensor_provider.h"
#include "sensor_service/sensor_service.h"
#include "sensor_transport/sensor_transport.h"
#include "bh1750/bh1750.h"
#include "sht45/sht45.h"
#include "sgp40/sgp40.h"
#include "sgp41/sgp41.h"
#include "pms5003/pms5003.h"
#include "pogo_port/pogo_port.h"
#include "role_manager/role_manager.h"
#include "thread_manager/thread_manager.h"
#include "child_service/child_service.h"
#include "parent_service/parent_service.h"
#include "topology_reporter/topology_reporter.h"
#include "topology_gate/topology_gate.h"
#include "collector_endpoint/collector_endpoint.h"
#include "openthread/coap.h" /* OT_DEFAULT_COAP_PORT -- this module's own CoAP port when it is itself the
                                 collector (MODULE_DEV_MODE_DEV_ROOT fixture only, see module_parent_get_collector()) */

static const char *TAG = "module_main";

static void port_event_dispatcher(pogo_port_id_t port_id, pogo_port_event_t event, const pogo_port_event_data_t *data,
                                   void *user_ctx)
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

static void on_role_change(module_role_t new_role, module_role_t old_role, void *ctx)
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
         * state -- see docs/state-machine.md. module_config and the
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

static pogo_port_config_t to_port_cfg(const module_port_config_t *mp)
{
    pogo_port_config_t out = {
        .enabled = (mp->uart_num >= 0),
        .uart_num = mp->uart_num,
        .tx_gpio = mp->tx_gpio,
        .rx_gpio = mp->rx_gpio,
        .detect_in_gpio = mp->detect_in_gpio,
        .detect_out_gpio = mp->detect_out_gpio,
        .base_id_gpio = mp->base_id_gpio,
        .detect_out_active_low = mp->detect_out_active_low,
        .debounce_ms = (mp->debounce_ms != 0) ? mp->debounce_ms : 30,
        .baud_rate = CONFIG_POGO_UART_BAUD_RATE,
    };
    return out;
}

void app_main(void)
{
    /*
     * Minimal, unconditional boot-diagnostic trail (does not by itself fix
     * "no serial output until reset" -- if these lines never appear on a
     * real board, that is a USB re-enumeration / power / monitor-connection
     * problem, not something the following log lines can paper over; see
     * this file's own note further down and the final report that added
     * these). esp_reset_reason() first, before anything else can fail.
     */
    ESP_LOGI(TAG, "app_main: entry (reset_reason=%d)", (int)esp_reset_reason());

    if (!pogo_crc16_selftest()) {
        ESP_LOGE(TAG, "CRC16 self-test failed -- refusing to start");
        abort();
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Register every real driver this firmware image is compiled with, so
     * sensor_provider_lookup()/sensor_service_init() below can find them by
     * module_config.driver_id -- see sensor_provider_register()'s doc
     * comment for why this happens here (in module main) instead of each
     * driver component depending on sensor_provider circularly. Harmless if
     * this board's config never selects one of them. */
    sensor_provider_register(SENSOR_DRIVER_ID_BH1750, &g_bh1750_provider_ops);
    sensor_provider_register(SENSOR_DRIVER_ID_SHT45, &g_sht45_provider_ops);
    sensor_provider_register(SENSOR_DRIVER_ID_SGP40, &g_sgp40_provider_ops);
    sensor_provider_register(SENSOR_DRIVER_ID_SGP41, &g_sgp41_provider_ops);
    /* PMS5003 is a real terminal-only UART driver; module_config rejects it
     * unless the downstream POGO face is fully disabled. */
    sensor_provider_register(SENSOR_DRIVER_ID_PMS5003, &g_pms5003_provider_ops);

    module_config_t cfg;
    if (module_config_load(&cfg) != ESP_OK) {
        /* module_config_load() already logged a specific CONFIG_ERROR line.
         * No sensor, no pairing, no Thread network -- ever -- without a
         * valid fixed config (section 9). */
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    ESP_LOGI(TAG, "module_config_load: OK (driver_id=%u schema_version=%u)", (unsigned)cfg.driver_id,
             (unsigned)cfg.schema_version);

    module_dev_mode_t dev_mode = module_config_load_dev_mode();
    if (dev_mode != MODULE_DEV_MODE_AUTO) {
        ESP_LOGW(TAG,
                 "DEVELOPMENT OVERRIDE ACTIVE: dev_mode=%s (see docs/pogo-interface.md) -- "
                 "this image is NOT representative of final hardware behavior",
                 module_dev_mode_to_str(dev_mode));
    }
    if (dev_mode == MODULE_DEV_MODE_AUTO && cfg.driver_id == SENSOR_DRIVER_ID_MOCK) {
        ESP_LOGE(TAG, "CONFIG_ERROR: mock sensor_provider selected with dev_mode=AUTO -- refusing to start "
                      "(mock is development-only; see docs/module-config.md)");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    ESP_ERROR_CHECK(thread_manager_init());

    /* sensor_service owns the one permanently-paired sensor driver from
     * here on -- initialized exactly once, before child_service_init()
     * (which now only ever calls sensor_service_measure_once(), never
     * sensor_provider_ops_t directly -- section 3 of the follow-up spec).
     * sensor_transport needs the OpenThread instance thread_manager_init()
     * just brought up, so it comes after that, and the sample-ready
     * callback wiring (sensor_service -> sensor_transport, see that
     * callback's own doc comment for why it's a function pointer and not a
     * direct component dependency) happens right after both exist. */
    ESP_LOGI(TAG, "sensor_service_init: starting");
    ESP_ERROR_CHECK(sensor_service_init(&cfg));
    ESP_LOGI(TAG, "sensor_service_init: complete");
    /*
     * topology_gate must exist before sensor_transport_init() (reads it on
     * every send) or topology_reporter_init() (the only thing that ever
     * opens/closes it) run -- see topology_gate.h's header comment and this
     * file's own top comment (steps 7/7b/11). require_topology_registration
     * is true only for the AUTO path against an active power base:
     * DEV_ROOT/DEV_LEAF module-to-module regression peers never run
     * power_base_transport and so never have a "/t" resource at all --
     * gating their "/s" sends on a registration that could never succeed
     * would permanently block sensor data in that mode (section 10 of the
     * follow-up spec).
     */
    ESP_ERROR_CHECK(topology_gate_init());
    /* collector_endpoint must exist before sensor_transport_init()/
     * topology_reporter_init() (both read its snapshot on every send) or
     * child_service_init()/parent_service_init() (child_service is the sole
     * writer; parent_service's context reads it via module_parent_get_collector()
     * to forward it downstream) run. */
    ESP_ERROR_CHECK(collector_endpoint_init());
    ESP_ERROR_CHECK(sensor_transport_init(dev_mode == MODULE_DEV_MODE_AUTO));
    sensor_service_set_sample_ready_cb(sensor_transport_send_sample);

    ESP_ERROR_CHECK(role_manager_init(dev_mode, on_role_change, NULL));

    pogo_port_config_t up_cfg = to_port_cfg(&cfg.upstream_port);
    pogo_port_config_t down_cfg = to_port_cfg(&cfg.downstream_port);
    ESP_ERROR_CHECK(pogo_port_init(POGO_PORT_UPSTREAM, &up_cfg, port_event_dispatcher, NULL));
    ESP_ERROR_CHECK(pogo_port_init(POGO_PORT_DOWNSTREAM, &down_cfg, port_event_dispatcher, NULL));

    ESP_ERROR_CHECK(child_service_init(&cfg));
    ESP_ERROR_CHECK(parent_service_init(&s_module_parent_ctx));
    ESP_ERROR_CHECK(topology_reporter_init(&cfg));

    if (up_cfg.enabled) {
        ESP_ERROR_CHECK(pogo_port_enable(POGO_PORT_UPSTREAM, POGO_PEER_TYPE_MODULE));
        ESP_LOGI(TAG, "pogo_port UPSTREAM: task created and enabled");
    } else {
        ESP_LOGI(TAG, "UPSTREAM port disabled by config (dev compat mode or not wired on this board)");
    }
    if (down_cfg.enabled) {
        ESP_ERROR_CHECK(pogo_port_enable(POGO_PORT_DOWNSTREAM, POGO_PEER_TYPE_MODULE));
        ESP_LOGI(TAG, "pogo_port DOWNSTREAM: task created and enabled");
    } else {
        ESP_LOGI(TAG, "DOWNSTREAM port disabled by config (dev compat mode or not wired on this board)");
    }

    ESP_LOGI(TAG, "module ready: driver_id=%u dev_mode=%s role=%s", (unsigned)cfg.driver_id,
             module_dev_mode_to_str(dev_mode), role_manager_role_to_str(role_manager_get_role()));
}
