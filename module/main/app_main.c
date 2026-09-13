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
 *   2. module_config_load()      -- fixed "cfg" partition (sensor pairing,
 *                                    port wiring). CONFIG_ERROR halts here.
 *   3. module_config_load_dev_mode() -- AUTO / DEV_ROOT / DEV_LEAF.
 *   4. thread_manager_init()     -- OpenThread FTD stack up, no network yet.
 *   5. role_manager_init()       -- starts deriving ROOT/INTERMEDIATE/LEAF
 *                                    (or is forced by a dev override).
 *   6. pogo_port_init() x2       -- UPSTREAM/DOWNSTREAM, both routed through
 *                                    one dispatcher into role_manager AND
 *                                    child_service (UPSTREAM only) /
 *                                    parent_service (DOWNSTREAM only).
 *   7. child_service_init() / parent_service_init() -- always both; each
 *                                    only ever acts on its own port's events.
 *   8. pogo_port_enable() x2     -- only for ports this board actually wires
 *                                    (module_config-driven; a dev board's
 *                                    "missing" port is simply not enabled).
 */
#include <stdint.h>
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pogo_proto/pogo_crc16.h"
#include "module_config/module_config.h"
#include "sensor_provider/sensor_provider.h"
#include "pogo_port/pogo_port.h"
#include "role_manager/role_manager.h"
#include "thread_manager/thread_manager.h"
#include "child_service/child_service.h"
#include "parent_service/parent_service.h"

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

static void on_role_change(module_role_t new_role, module_role_t old_role, void *ctx)
{
    (void)ctx;
    child_service_on_role_change(new_role, old_role);

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

    module_config_t cfg;
    if (module_config_load(&cfg) != ESP_OK) {
        /* module_config_load() already logged a specific CONFIG_ERROR line.
         * No sensor, no pairing, no Thread network -- ever -- without a
         * valid fixed config (section 9). */
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

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
    ESP_ERROR_CHECK(role_manager_init(dev_mode, on_role_change, NULL));

    pogo_port_config_t up_cfg = to_port_cfg(&cfg.upstream_port);
    pogo_port_config_t down_cfg = to_port_cfg(&cfg.downstream_port);
    ESP_ERROR_CHECK(pogo_port_init(POGO_PORT_UPSTREAM, &up_cfg, port_event_dispatcher, NULL));
    ESP_ERROR_CHECK(pogo_port_init(POGO_PORT_DOWNSTREAM, &down_cfg, port_event_dispatcher, NULL));

    ESP_ERROR_CHECK(child_service_init(&cfg));
    ESP_ERROR_CHECK(parent_service_init(&cfg));

    if (up_cfg.enabled) {
        ESP_ERROR_CHECK(pogo_port_enable(POGO_PORT_UPSTREAM, POGO_PEER_TYPE_MODULE));
    } else {
        ESP_LOGI(TAG, "UPSTREAM port disabled by config (dev compat mode or not wired on this board)");
    }
    if (down_cfg.enabled) {
        ESP_ERROR_CHECK(pogo_port_enable(POGO_PORT_DOWNSTREAM, POGO_PEER_TYPE_MODULE));
    } else {
        ESP_LOGI(TAG, "DOWNSTREAM port disabled by config (dev compat mode or not wired on this board)");
    }

    ESP_LOGI(TAG, "module ready: driver_id=%u dev_mode=%s role=%s", (unsigned)cfg.driver_id,
             module_dev_mode_to_str(dev_mode), role_manager_role_to_str(role_manager_get_role()));
}
