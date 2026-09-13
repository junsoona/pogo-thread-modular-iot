/*
 * Power base entry point (see docs/power-base.md). This is a SEPARATE
 * ESP-IDF project/binary from `module` -- the power base is never a sensor
 * module role and never runs role_manager/child_service/sensor_provider/
 * sensor_service at all (this file deliberately never touches any of those
 * headers). It reuses parent_service and pogo_port UNMODIFIED, wiring
 * parent_service's small context struct (see parent_service.h) to its own
 * always-power-base identity instead of role_manager/child_service.
 *
 * Boot sequence (section 4 of the power-base follow-up spec -- order matters,
 * every step below is exactly one item in that list):
 *   1. nvs_flash_init()            -- dynamic "nvs" partition (OpenThread's
 *                                      own Active Dataset, see
 *                                      thread_manager_start_as_root()).
 *   2. power_base_config_load()    -- fixed "cfg" partition. CONFIG_ERROR
 *                                      halts here, same policy as module_config_load().
 *   3. thread_manager_init()       -- OpenThread FTD stack up, no network yet.
 *   4/5. thread_manager_start_as_root() -- resumes a commissioned dataset if
 *                                      one exists in NVS, otherwise forms a
 *                                      new one (Dataset/Network Key never
 *                                      logged). Reused VERBATIM from
 *                                      components/thread_manager -- this is
 *                                      the ONLY place in the whole system
 *                                      that ever forms or owns a dataset.
 *   6. Block until LEADER (strictly -- not "Leader or Router"; see
 *                                      pb_provisioning_allowed()'s own
 *                                      comment for why no other role is
 *                                      ever the correct steady state here)
 *                                    -- a bounded 30s wait (same budget as
 *                                      module's root_ready_watcher_task).
 *                                      Failure here halts WITHOUT ever
 *                                      calling pogo_port_enable(), so
 *                                      DETECT_OUT stays released the whole
 *                                      time (pogo_port_init() alone touches
 *                                      no GPIO -- see pogo_port.h).
 *   7. power_base_transport_init() -- CoAP "/s" (sensor data) + "/t"
 *                                      (topology reports) resources.
 *   8. parent_service_init()       -- wired to this power base's own
 *                                      context; starts automatically
 *                                      provisioning whatever docks on the
 *                                      downstream port once its own
 *                                      provisioning_allowed() callback
 *                                      (strictly LEADER) says so.
 *   9. pogo_port_init()            -- registers the downstream port's
 *                                      config; touches no GPIO yet.
 *  10. Stale-sweep task creation CONFIRMED (xTaskCreate() checked) --
 *                                      code review follow-up (2nd pass):
 *                                      this must succeed BEFORE
 *                                      pogo_port_enable() below, or a
 *                                      sensor module could start docking
 *                                      while this power base can never
 *                                      mark a stale node offline again.
 *                                      Failure here halts -- exactly like
 *                                      every step above -- WITHOUT ever
 *                                      calling pogo_port_enable().
 *  11. pogo_port_enable()          -- the ONE downstream POGO port. This is
 *                                      also what drives DETECT_OUT active
 *                                      (Low) -- see pogo_port.c's
 *                                      pogo_port_enable(): there is no
 *                                      separate step to sequence, the
 *                                      ordering above already guarantees
 *                                      DETECT_OUT never activates before
 *                                      Thread is LEADER-ready AND the
 *                                      stale-sweep task is confirmed running.
 *  12. "power base ready" logged   -- only reachable if every step above
 *                                      succeeded.
 */
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pogo_proto/pogo_crc16.h"
#include "pogo_port/pogo_port.h"
#include "parent_service/parent_service.h"
#include "power_base_config/power_base_config.h"
#include "power_base_transport/power_base_transport.h"
#include "thread_manager/thread_manager.h"

static const char *TAG = "power_base_main";

static power_base_config_t s_cfg;
static uint8_t s_own_uuid[POGO_UUID_LEN];
static parent_service_context_t s_parent_ctx;

static void halt_forever(const char *why)
{
    ESP_LOGE(TAG, "%s -- halting (no sensor, no POGO pairing, no Thread network without a valid config/role)", why);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/*
 * Power rail sequencing (section 4): no power-switching GPIO has been
 * decided on current hardware. power_enable_gpio<0 means the 5V rail is
 * assumed ALWAYS ON -- this firmware never claims to be able to cut real 5V
 * power when that is the case (see docs/power-base.md's safety notes).
 */
static void apply_power_rail_sequencing(void)
{
    if (s_cfg.power_enable_gpio < 0) {
        ESP_LOGI(TAG, "power_enable_gpio not configured -- 5V rail assumed always-on (no switching hardware yet)");
        return;
    }
    gpio_config_t out_cfg = {
        .pin_bit_mask = 1ULL << s_cfg.power_enable_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&out_cfg);
    if (err != ESP_OK) {
        halt_forever("power_enable_gpio: gpio_config failed -- refusing to continue with an unconfigured power rail "
                     "control pin");
    }
    err = gpio_set_level((gpio_num_t)s_cfg.power_enable_gpio, s_cfg.power_enable_active_high ? 1 : 0);
    if (err != ESP_OK) {
        halt_forever("power_enable_gpio: gpio_set_level failed -- the power rail's actual state is now unknown");
    }
    ESP_LOGI(TAG, "power_enable_gpio=%d driven %s -- waiting power_startup_delay_ms=%u before continuing",
             (int)s_cfg.power_enable_gpio, s_cfg.power_enable_active_high ? "HIGH" : "LOW",
             (unsigned)s_cfg.power_startup_delay_ms);
    if (s_cfg.power_startup_delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(s_cfg.power_startup_delay_ms));
    }
}

/* --- parent_service_context_t: this power base's own identity/readiness,
 * reusing parent_service completely unmodified (see parent_service.h). --- */

/*
 * Code review follow-up section 5: strictly LEADER, not "LEADER or ROUTER".
 * The power base is this system's architectural requirement's "one and only
 * Thread ROOT/Leader" -- it is the sole device that ever forms or resumes
 * the network's Active Dataset (thread_manager_start_as_root()), and no
 * sensor module ever forms one of its own (see docs/power-base.md). In a
 * network where this is the only device that can ever legitimately become
 * Leader, a solo-forming/resuming FTD node attaches directly into the
 * Leader role -- there is no other partition to defer Leadership to, so
 * there is no scenario in THIS topology where "merely Router" is the
 * correct, expected steady state for the power base. Tolerating ROUTER here
 * was a leftover hedge, not a deliberate design choice with an OpenThread
 * justification -- removed.
 */
static bool pb_provisioning_allowed(void *ctx)
{
    (void)ctx;
    return thread_manager_get_role() == OT_DEVICE_ROLE_LEADER;
}

static uint8_t pb_get_own_depth(void *ctx)
{
    (void)ctx;
    return 0; /* the true network root -- its direct child is always assigned depth=1 */
}

static void pb_get_own_uuid(uint8_t out_uuid[POGO_UUID_LEN], void *ctx)
{
    (void)ctx;
    memcpy(out_uuid, s_own_uuid, POGO_UUID_LEN);
}

static const char *pb_role_name(void *ctx)
{
    (void)ctx;
    return "POWER_BASE";
}

static void port_event_dispatcher(pogo_port_id_t port_id, pogo_port_event_t event, const pogo_port_event_data_t *data,
                                   void *user_ctx)
{
    (void)user_ctx;
    if (port_id != POGO_PORT_DOWNSTREAM) {
        return; /* the power base has no UPSTREAM port at all */
    }
    parent_service_on_port_event(port_id, event, data);
    if (event == POGO_PORT_EVENT_DETACHED) {
        /* Immediate hardware signal: the direct child (and its whole
         * subtree) is unreachable the instant this physical link drops --
         * see docs/power-base.md's detach handling. */
        power_base_transport_on_direct_downstream_detached();
    }
}

static void stale_sweep_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        power_base_transport_sweep_stale(now_ms, s_cfg.topology_stale_timeout_ms);
    }
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

    if (power_base_config_load(&s_cfg) != ESP_OK) {
        /* power_base_config_load() already logged a specific CONFIG_ERROR line. */
        halt_forever("CONFIG_ERROR loading power_base_config");
    }

    apply_power_rail_sequencing();

    /* Stable identity (section 6): the ESP32-H2's own efuse-provisioned
     * EUI-64 is used directly as this power base's 8-byte UUID -- permanent
     * hardware identity, never regenerated, never logged in full (only the
     * last-byte suffix, same convention as every other UUID log line in this
     * codebase). */
    uint8_t mac8[8];
    if (esp_efuse_mac_get_default(mac8) != ESP_OK) {
        halt_forever("esp_efuse_mac_get_default failed -- cannot derive a stable power-base identity");
    }
    static const uint8_t all_zero_mac[8] = {0};
    if (memcmp(mac8, all_zero_mac, sizeof(mac8)) == 0) {
        /* An all-zero efuse read is never a valid permanent identifier --
         * same policy child_service.c's compute_permanent_uuid() applies to
         * a sensor module's own uuid (code review follow-up section 1).
         * Proceeding would give every unprovisioned power base the exact
         * same identity, which would be indistinguishable from a real
         * collision in the topology registry. */
        halt_forever("esp_efuse_mac_get_default returned an all-zero EUI-64 -- refusing to use it as this power "
                     "base's identity");
    }
    memcpy(s_own_uuid, mac8, POGO_UUID_LEN);

    ESP_ERROR_CHECK(thread_manager_init());

    if (thread_manager_start_as_root() != ESP_OK) {
        halt_forever("ROLE_TRANSITION_ERROR: thread_manager_start_as_root failed");
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
    otDeviceRole role;
    while (1) {
        role = thread_manager_get_role();
        if (role == OT_DEVICE_ROLE_LEADER) {
            break;
        }
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            ESP_LOGE(TAG, "ROLE_TRANSITION_ERROR: last observed otDeviceRole=%u(%s) -- never reached LEADER within 30s",
                     role, thread_manager_role_to_str(role));
            halt_forever("ROLE_TRANSITION_ERROR: Thread never reached LEADER within 30s");
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGI(TAG, "Thread ready: otDeviceRole=%u(%s) -- starting CoAP services and downstream provisioning",
             (unsigned)role, thread_manager_role_to_str(role));

    ESP_ERROR_CHECK(power_base_transport_init(s_own_uuid));

    s_parent_ctx = (parent_service_context_t){
        .provisioning_allowed = pb_provisioning_allowed,
        .get_own_depth = pb_get_own_depth,
        .get_own_uuid = pb_get_own_uuid,
        .role_name = pb_role_name,
        .is_power_base = true,
        .ack_timeout_ms = s_cfg.ack_timeout_ms,
        .ctx = NULL,
    };
    ESP_ERROR_CHECK(parent_service_init(&s_parent_ctx));

    pogo_port_config_t down_cfg = {
        .enabled = true,
        .uart_num = s_cfg.downstream_port.uart_num,
        .tx_gpio = s_cfg.downstream_port.tx_gpio,
        .rx_gpio = s_cfg.downstream_port.rx_gpio,
        .detect_in_gpio = s_cfg.downstream_port.detect_in_gpio,
        .detect_out_gpio = s_cfg.downstream_port.detect_out_gpio,
        .base_id_gpio = -1, /* BASE_ID is read on a MODULE's UPSTREAM side only -- meaningless here */
        .detect_out_active_low = s_cfg.downstream_port.detect_out_active_low,
        .debounce_ms = (s_cfg.downstream_port.debounce_ms != 0) ? s_cfg.downstream_port.debounce_ms : 30,
        .baud_rate = CONFIG_POGO_UART_BAUD_RATE,
    };
    ESP_ERROR_CHECK(pogo_port_init(POGO_PORT_DOWNSTREAM, &down_cfg, port_event_dispatcher, NULL));

    /*
     * Code review follow-up (2nd pass): the stale-sweep task must be
     * CONFIRMED running BEFORE pogo_port_enable() -- previously it was
     * created after enable(), so a sensor module could already be docking
     * (DETECT_OUT active, provisioning under way) while this power base's
     * own ability to ever mark a stale node offline again was silently
     * broken. If this task cannot be created, halt here -- exactly like
     * every other boot-time failure above -- WITHOUT ever calling
     * pogo_port_enable(): DETECT_OUT stays released, "power base ready" is
     * never logged, and no sensor module is ever invited to dock against a
     * power base that cannot maintain its own topology registry.
     */
    BaseType_t sweep_task_ok = xTaskCreate(stale_sweep_task, "topology_stale_sweep", 3072, NULL, 3, NULL);
    if (sweep_task_ok != pdPASS) {
        halt_forever("STALE_SWEEP_TASK_FAILED: xTaskCreate failed -- refusing to enable downstream POGO "
                     "provisioning with no way to ever mark a stale node offline again");
    }

    /* pogo_port_enable() itself drives DETECT_OUT active (Low) as part of
     * bringing the port up -- simply never having called it before Thread
     * was confirmed LEADER (above) AND the stale-sweep task was confirmed
     * running (just above) IS this system's "DETECT_OUT only once fully
     * ready" guarantee; there is no separate GPIO write to sequence. */
    ESP_ERROR_CHECK(pogo_port_enable(POGO_PORT_DOWNSTREAM, POGO_PEER_TYPE_POWER_BASE));

    ESP_LOGI(TAG, "power base ready: uuid=..%02x%02x otDeviceRole=%u(%s) stale_sweep=running -- awaiting the "
                  "first sensor module",
             s_own_uuid[6], s_own_uuid[7], (unsigned)role, thread_manager_role_to_str(role));
}
