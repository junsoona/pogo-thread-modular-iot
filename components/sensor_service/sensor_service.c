#include "sensor_service/sensor_service.h"

#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "node_calc/node_calc.h"

static const char *TAG = "sensor_service";

/*
 * Owned copy, not a pointer into the caller's storage -- same reasoning as
 * child_service's s_cfg (see child_service.c's doc comment on that field):
 * sensor_service_init() is called with the address of a local module_config_t
 * in module/main/app_main.c's app_main(), which goes out of scope once
 * app_main() returns.
 */
static module_config_t s_cfg;
static const sensor_provider_ops_t *s_ops;
static sensor_provider_ctx_t *s_ctx;
static SemaphoreHandle_t s_measure_mutex;

static uint32_t s_sample_seq;
static uint8_t s_provisioned_uuid[SENSOR_SERVICE_UUID_LEN];
static bool s_has_provisioned_uuid;
static volatile uint32_t s_effective_period_ms;
static TaskHandle_t s_periodic_task;
static volatile bool s_thread_joined;
static volatile module_role_t s_role = MODULE_ROLE_UNASSIGNED;
static sensor_service_sample_ready_cb_t s_sample_ready_cb;
static int64_t s_boot_time_us;

uint32_t sensor_service_get_driver_min_period_ms(void)
{
    if (s_ops == NULL || s_ops->get_min_polling_period_ms == NULL) {
        return 0;
    }
    return s_ops->get_min_polling_period_ms(s_ctx);
}

uint32_t sensor_service_compute_effective_period_ms(uint32_t dynamic_period_ms)
{
    uint32_t min_period = sensor_service_get_driver_min_period_ms();
    return (dynamic_period_ms > min_period) ? dynamic_period_ms : min_period;
}

esp_err_t sensor_service_measure_once(sensor_measurement_t *out, TickType_t timeout_ticks)
{
    if (s_ops == NULL || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_measure_mutex, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    memset(out, 0, sizeof(*out));
    esp_err_t err = s_ops->start_measurement(s_ctx);
    if (err == ESP_OK) {
        err = s_ops->read_measurement(s_ctx, out, timeout_ticks);
    }
    xSemaphoreGive(s_measure_mutex);
    return err;
}

uint16_t sensor_service_get_sensor_bitmap(void)
{
    return (s_ops != NULL) ? s_ops->get_sensor_bitmap(s_ctx) : 0;
}

sensor_status_t sensor_service_get_status(void)
{
    return (s_ops != NULL) ? s_ops->get_status(s_ctx) : SENSOR_STATUS_NOT_INITIALIZED;
}

void sensor_service_set_provisioned_uuid(const uint8_t uuid[SENSOR_SERVICE_UUID_LEN])
{
    memcpy(s_provisioned_uuid, uuid, SENSOR_SERVICE_UUID_LEN);
    s_has_provisioned_uuid = true;
}

void sensor_service_get_module_uuid(uint8_t out_uuid[SENSOR_SERVICE_UUID_LEN])
{
    if (s_has_provisioned_uuid) {
        memcpy(out_uuid, s_provisioned_uuid, SENSOR_SERVICE_UUID_LEN);
        return;
    }
    /* ROOT (never provisioned by anyone) or not-yet-provisioned child:
     * derive a stable pseudo-UUID from this module's own MAC so every
     * module's samples still carry a consistent identity across reboots,
     * without inventing a second identity scheme. */
    uint8_t mac8[8] = {0};
    if (esp_efuse_mac_get_default(mac8) != ESP_OK) {
        ESP_LOGW(TAG, "esp_efuse_mac_get_default failed -- falling back to an all-zero pseudo-UUID");
    }
    memcpy(out_uuid, mac8, SENSOR_SERVICE_UUID_LEN);
}

void sensor_service_set_thread_joined(bool joined)
{
    s_thread_joined = joined;
}

void sensor_service_set_sample_ready_cb(sensor_service_sample_ready_cb_t cb)
{
    s_sample_ready_cb = cb;
}

void sensor_service_on_role_change(module_role_t new_role, module_role_t old_role)
{
    (void)old_role;
    s_role = new_role;
}

static void periodic_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t period = s_effective_period_ms;
        if (period == 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(period));

        sensor_measurement_t meas;
        esp_err_t err = sensor_service_measure_once(&meas, pdMS_TO_TICKS(s_cfg.measurement_timeout_ms));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "periodic measurement failed: %s (status=%d)", esp_err_to_name(err),
                     (int)sensor_service_get_status());
            continue;
        }

        sensor_sample_t sample = {0};
        sample.payload_version = SENSOR_CHANNEL_PAYLOAD_VERSION;
        sensor_service_get_module_uuid(sample.module_uuid);
        sample.sample_seq = ++s_sample_seq;
        sample.uptime_ms = (uint32_t)((esp_timer_get_time() - s_boot_time_us) / 1000);
        sample.sensor_bitmap = meas.sensor_type_bitmap;
        sample.channel_count = meas.channel_count;
        memcpy(sample.channels, meas.channels, sizeof(meas.channels[0]) * meas.channel_count);

        module_role_t role = s_role;
        if (role == MODULE_ROLE_ROOT) {
            /* ROOT's own permanent sensor: straight into the local
             * aggregator, no wireless hop of its own data needed. */
            sensor_service_root_record_sample(&sample);
        } else if (s_thread_joined && s_sample_ready_cb != NULL) {
            s_sample_ready_cb(&sample);
        }
        /* else: not ROOT and (not yet Thread-joined, or no transport wired
         * up yet) -- measurement still happened (kept for the next tick's
         * sample_seq continuity and local diagnostics), only the wireless
         * hand-off is skipped, per section 3/9: "Thread 미가입 상태에서는
         * 무선 전송만 중지". */
    }
}

void sensor_service_set_polling_period_ms(uint32_t effective_period_ms)
{
    s_effective_period_ms = effective_period_ms;
    if (s_periodic_task == NULL && effective_period_ms > 0) {
        BaseType_t ok = xTaskCreate(periodic_task, "sensor_service", 4096, NULL, 4, &s_periodic_task);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "failed to start periodic sampling task");
            s_periodic_task = NULL;
        }
    }
}

esp_err_t sensor_service_init(const module_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg; /* owned copy -- see this file's doc comment on s_cfg */
    s_ops = sensor_provider_lookup(s_cfg.driver_id);
    if (s_ops == NULL) {
        ESP_LOGE(TAG, "CONFIG_ERROR: no sensor_provider for driver_id=%u", (unsigned)s_cfg.driver_id);
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t err = s_ops->init(&s_cfg, &s_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sensor_provider '%s' init failed: %s", s_ops->name, esp_err_to_name(err));
        return err;
    }

    s_measure_mutex = xSemaphoreCreateMutex();
    if (s_measure_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t agg_err = sensor_root_aggregator_init();
    if (agg_err != ESP_OK) {
        return agg_err;
    }
    s_sample_seq = 0;
    s_has_provisioned_uuid = false;
    s_effective_period_ms = 0;
    s_periodic_task = NULL;
    s_thread_joined = false;
    s_role = MODULE_ROLE_UNASSIGNED;
    s_boot_time_us = esp_timer_get_time();

    /* Self-calibration measurement: doubles as this component's ownership of
     * "프로비저닝용 응답시간 측정" (section 3) and seeds an initial
     * effective period so ROOT (which never receives a POLL_CONFIG from
     * anyone) still has one. INTERMEDIATE/LEAF get a real one shortly after,
     * once child_service's own SENSOR_HELLO measurement + the parent's
     * POLL_CONFIG complete -- see child_service.c's handle_poll_config(). */
    sensor_measurement_t initial_meas;
    esp_err_t merr = sensor_service_measure_once(&initial_meas, pdMS_TO_TICKS(s_cfg.measurement_timeout_ms));
    if (merr == ESP_OK && initial_meas.response_time_ms > 0) {
        uint32_t dynamic_period_ms;
        if (node_calc_polling_period_ms(initial_meas.response_time_ms, &dynamic_period_ms) == ESP_OK) {
            uint32_t effective = sensor_service_compute_effective_period_ms(dynamic_period_ms);
            sensor_service_set_polling_period_ms(effective);
            ESP_LOGI(TAG, "self-calibrated: response_time=%ums -> effective_polling_period_ms=%u",
                     (unsigned)initial_meas.response_time_ms, (unsigned)effective);
        }
    } else {
        ESP_LOGW(TAG, "self-calibration measurement failed (%s) -- periodic sampling paused until a polling "
                      "period is set explicitly",
                 esp_err_to_name(merr));
    }

    ESP_LOGI(TAG, "sensor_service ready: driver='%s' bitmap=0x%04x", s_ops->name, sensor_service_get_sensor_bitmap());
    return ESP_OK;
}

/* --- ROOT-side local aggregator: thin wrappers around sensor_root_aggregator
 * (see sensor_service.h's doc comment on this section for why the actual
 * storage/dedup logic moved there) --- */

void sensor_service_root_record_sample(const sensor_sample_t *sample)
{
    sensor_root_aggregator_record_sample(sample);
}

void sensor_service_root_record_error(const uint8_t module_uuid[SENSOR_SERVICE_UUID_LEN])
{
    sensor_root_aggregator_record_error(module_uuid);
}

size_t sensor_service_root_get_node_count(void)
{
    return sensor_root_aggregator_get_node_count();
}

bool sensor_service_root_get_node(size_t index, sensor_service_node_record_t *out)
{
    return sensor_root_aggregator_get_node(index, out);
}
