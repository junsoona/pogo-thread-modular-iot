#include "enrollment_runtime.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "thread_manager/thread_manager.h"
#if CONFIG_POWER_BASE_BLE_ENROLLMENT
#include "power_base_ble_enroll/pb_ble_service.h"
#endif

static const char *TAG = "pb_enroll_rt";

#define ENROLLMENT_POLL_MS 200

/* The enrollment task exists whenever the build has a commissioning
 * transport; which one is the transport's business, not the task's. */
#define PB_ENROLLMENT_HAS_TRANSPORT (CONFIG_POWER_BASE_BENCH_JOINER_CONSOLE || CONFIG_POWER_BASE_BLE_ENROLLMENT)
/* The BLE build also runs the BLE stack start/stop and the Joiner start on
 * this task's stack; the bench build keeps its tested size. */
#if CONFIG_POWER_BASE_BLE_ENROLLMENT
#define ENROLLMENT_TASK_STACK 6144
#else
#define ENROLLMENT_TASK_STACK 4096
#endif

typedef struct {
    uint32_t attempt;
    pb_enroll_joiner_outcome_t outcome;
} joiner_event_t;

/* s_lock serialises every pb_enroll_ctl_* call (console task, enrollment
 * task, app_main). The OpenThread task never takes it: the Joiner callback
 * only posts to s_joiner_q, so holding s_lock while taking the OpenThread
 * lock cannot deadlock. The BLE callbacks (NimBLE host task) never take it
 * either: they only queue requests under the BLE service's own leaf mutex,
 * and the enrollment task runs those requests through the functions below
 * (lock order: s_lock -> OpenThread lock; the leaf mutex is never held
 * together with either -- see pb_ble_service.h). */
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_joiner_q;
static pb_enroll_ctl_t s_ctl;
static pb_enrollment_ready_cb_t s_ready;
static bool s_ready_fired;
static bool s_joiner_eui64_matches;
/* A re-enrollment window boot: nothing is resumed and nothing may join. */
static bool s_reenroll_window;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* OpenThread task, API lock held: no blocking, no OpenThread calls. */
static void joiner_callback(otError error, void *context)
{
    joiner_event_t ev = {.attempt = (uint32_t)(uintptr_t)context};
    switch (error) {
    case OT_ERROR_NONE: ev.outcome = PB_ENROLL_JOINER_OK; break;
    case OT_ERROR_SECURITY: ev.outcome = PB_ENROLL_JOINER_ERR_SECURITY; break;
    case OT_ERROR_NOT_FOUND: ev.outcome = PB_ENROLL_JOINER_ERR_NOT_FOUND; break;
    case OT_ERROR_RESPONSE_TIMEOUT: ev.outcome = PB_ENROLL_JOINER_ERR_TIMEOUT; break;
    default: ev.outcome = PB_ENROLL_JOINER_ERR_OTHER; break;
    }
    /* If the queue is full the result is lost; the controller's Joiner
     * guard then ends the attempt as joiner_guard_expired. */
    (void)xQueueSend(s_joiner_q, &ev, 0);
}

static esp_err_t op_joiner_start(const char *pskd, uint32_t attempt, void *ctx)
{
    (void)ctx;
    if (!s_joiner_eui64_matches) {
        ESP_LOGE(TAG, "Joiner refused: OpenThread's factory EUI-64 differs from this Base's uuid, so the hub "
                      "could never match a heartbeat to this Joiner");
        return ESP_ERR_INVALID_STATE;
    }
    return thread_manager_joiner_start(pskd, joiner_callback, (void *)(uintptr_t)attempt);
}

static void op_joiner_stop(void *ctx)
{
    (void)ctx;
    thread_manager_joiner_stop();
}

static esp_err_t op_thread_start_stored(bool require_complete, void *ctx)
{
    (void)ctx;
    return thread_manager_start_as_always_on_ftd_stored(require_complete);
}

static void op_thread_stop(void *ctx)
{
    (void)ctx;
    if (thread_manager_stop() != ESP_OK) {
        ESP_LOGE(TAG, "thread_manager_stop failed while returning to the safe wait");
    }
}

static bool op_read_dataset(pb_enroll_dataset_info_t *out, void *ctx)
{
    (void)ctx;
    thread_manager_dataset_summary_t s;
    if (thread_manager_get_stored_dataset_summary(&s) != ESP_OK) {
        return false;
    }
    out->has_network_key = s.has_network_key;
    out->complete = s.complete;
    memcpy(out->id.ext_pan_id, s.ext_pan_id, sizeof(out->id.ext_pan_id));
    memcpy(out->id.mesh_local_prefix, s.mesh_local_prefix, sizeof(out->id.mesh_local_prefix));
    return true;
}

static const pb_enroll_ops_t k_ops = {
    .joiner_start = op_joiner_start,
    .joiner_stop = op_joiner_stop,
    .thread_start_stored = op_thread_start_stored,
    .thread_stop = op_thread_stop,
    .read_dataset = op_read_dataset,
    .ctx = NULL,
};

#if PB_ENROLLMENT_HAS_TRANSPORT
static bool role_is_attached(otDeviceRole role)
{
    return role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER;
}

/* Delivers Joiner outcomes and runs the attach/marker checks until the board
 * is enrolled, then hands over to the caller's normal service start. */
static void enrollment_task(void *arg)
{
    (void)arg;
#if CONFIG_POWER_BASE_BLE_ENROLLMENT
    /* Opened here so that BLE is started, polled and stopped by one task. */
    esp_err_t ble_err = pb_enrollment_ble_start();
    if (ble_err != ESP_OK) {
        ESP_LOGE(TAG, "safe wait: BLE enrollment is not available on this board (%s) -- Thread and services stay "
                      "off; nothing can enroll it until that is fixed",
                 esp_err_to_name(ble_err));
        vTaskDelete(NULL);
        return;
    }
#endif
    while (1) {
        joiner_event_t ev;
        bool have_event = xQueueReceive(s_joiner_q, &ev, pdMS_TO_TICKS(ENROLLMENT_POLL_MS)) == pdTRUE;
        bool attached = role_is_attached(thread_manager_get_role());

        bool fire = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (have_event) {
            pb_enroll_ctl_on_joiner_done(&s_ctl, ev.attempt, ev.outcome, now_ms());
        }
        pb_enroll_ctl_tick(&s_ctl, attached, now_ms());
        if (pb_enroll_ctl_services_allowed(&s_ctl) && !s_ready_fired) {
            s_ready_fired = true;
            fire = true;
        }
        xSemaphoreGive(s_lock);

#if CONFIG_POWER_BASE_BLE_ENROLLMENT
        /* Outside s_lock: it runs queued BLE requests through
         * pb_enrollment_request_join()/pb_enrollment_cancel(), and it stops
         * BLE once enrolled -- before the services start below. */
        pb_enrollment_ble_poll();
#endif

        if (fire) {
            s_ready();
            vTaskDelete(NULL);
            return;
        }
    }
}
#endif

esp_err_t pb_enrollment_runtime_start(const uint8_t eui64[PB_ENROLL_EUI64_LEN],
                                      pb_enroll_endpoint_policy_t endpoint_policy, pb_enrollment_ready_cb_t ready)
{
    s_ready = ready;
    s_lock = xSemaphoreCreateMutex();
    s_joiner_q = xQueueCreate(4, sizeof(joiner_event_t));
    if (s_lock == NULL || s_joiner_q == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t factory[PB_ENROLL_EUI64_LEN];
    s_joiner_eui64_matches = thread_manager_get_factory_eui64(factory) == ESP_OK &&
                             memcmp(factory, eui64, PB_ENROLL_EUI64_LEN) == 0;
    if (!s_joiner_eui64_matches) {
        ESP_LOGE(TAG, "OpenThread factory EUI-64 != efuse uuid -- the Joiner will be refused on this board");
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    pb_enroll_ctl_init(&s_ctl, &k_ops, eui64);
    /* Before the boot rule: it decides whether a record without a bridge
     * endpoint may be resumed, and from here on refuses a request without
     * one -- for the BLE worker and the bench console alike. */
    pb_enroll_ctl_set_endpoint_policy(&s_ctl, endpoint_policy);
    pb_enroll_state_t state = pb_enroll_ctl_boot(&s_ctl);
    bool resumed = (state == PB_ENROLL_STATE_ENROLLED);
    if (resumed) {
        s_ready_fired = true;
    }
    xSemaphoreGive(s_lock);

    if (resumed) {
        s_ready();
    } else {
#if PB_ENROLLMENT_HAS_TRANSPORT
        if (xTaskCreate(enrollment_task, "pb_enroll", ENROLLMENT_TASK_STACK, NULL, 4, NULL) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
#endif
#if CONFIG_POWER_BASE_BENCH_JOINER_CONSOLE
        ESP_LOGW(TAG, "safe wait: BENCH build -- a Joiner can be started with the 'pb_join' console command "
                      "('pb_join status' shows this board's EUI-64)");
#elif CONFIG_POWER_BASE_BLE_ENROLLMENT
        ESP_LOGW(TAG, "safe wait: BLE enrollment build -- the enrollment task opens BLE if this board has a valid "
                      "setup credential");
#else
        ESP_LOGW(TAG, "safe wait: this build has no commissioning transport (bench trials use "
                      "power_base/sdkconfig.bench_joiner); Thread and services stay off");
#endif
    }

#if CONFIG_POWER_BASE_BENCH_JOINER_CONSOLE
    esp_err_t err = pb_enrollment_console_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pb_join console failed to start (%s)", esp_err_to_name(err));
    }
#endif
    return ESP_OK;
}

void pb_enrollment_note_bridge_endpoint(const pb_enroll_bridge_endpoint_t *endpoint)
{
    if (s_lock == NULL || endpoint == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ctl.state == PB_ENROLL_STATE_ENROLLED && s_ctl.has_bridge_endpoint) {
        s_ctl.bridge_endpoint = *endpoint;
    }
    xSemaphoreGive(s_lock);
}

bool pb_enrollment_busy(void)
{
    if (s_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = s_ctl.state == PB_ENROLL_STATE_JOINING || s_ctl.state == PB_ENROLL_STATE_ATTACHING;
    xSemaphoreGive(s_lock);
#if CONFIG_POWER_BASE_BLE_ENROLLMENT
    busy = busy || pb_ble_service_busy();
#endif
    return busy;
}

#if CONFIG_POWER_BASE_REENROLL_BUTTON
bool pb_enrollment_in_reenroll_window(void)
{
    return s_reenroll_window;
}

/* Owns BLE for the whole window boot: starts it, polls it (which also runs
 * an accepted local reset, here, never in a BLE callback) and takes it down.
 * Every way out of the loop is a restart; the next boot is an ordinary one
 * because the window request was used up before this boot opened anything. */
static void reenroll_window_task(void *arg)
{
    (void)arg;
    esp_err_t ble_err = pb_enrollment_ble_start();
    if (ble_err != ESP_OK) {
        ESP_LOGE(TAG, "re-enrollment window: BLE is not available on this board (%s) -- nothing was changed",
                 esp_err_to_name(ble_err));
    } else {
        while (!pb_enrollment_ble_is_down()) {
            vTaskDelay(pdMS_TO_TICKS(ENROLLMENT_POLL_MS));
            pb_enrollment_ble_poll();
        }
    }
    ESP_LOGW(TAG, "re-enrollment window over -- restarting into the ordinary boot (it resumes the stored enrollment "
                  "unless a reset was confirmed and carried out)");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

esp_err_t pb_enrollment_runtime_start_reenroll_window(const uint8_t eui64[PB_ENROLL_EUI64_LEN],
                                                      pb_enroll_endpoint_policy_t endpoint_policy)
{
    s_ready = NULL;
    s_lock = xSemaphoreCreateMutex();
    s_joiner_q = xQueueCreate(4, sizeof(joiner_event_t));
    if (s_lock == NULL || s_joiner_q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    uint8_t factory[PB_ENROLL_EUI64_LEN];
    s_joiner_eui64_matches = thread_manager_get_factory_eui64(factory) == ESP_OK &&
                             memcmp(factory, eui64, PB_ENROLL_EUI64_LEN) == 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Initialised for its EUI-64 and status only. pb_enroll_ctl_boot() is
     * deliberately NOT called: it is what would enable Thread on the stored
     * Dataset. */
    pb_enroll_ctl_init(&s_ctl, &k_ops, eui64);
    pb_enroll_ctl_set_endpoint_policy(&s_ctl, endpoint_policy);
    s_reenroll_window = true;
    xSemaphoreGive(s_lock);

    if (xTaskCreate(reenroll_window_task, "pb_reenroll", ENROLLMENT_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGW(TAG, "re-enrollment window boot: the stored enrollment is NOT resumed; Thread, the exporter, the "
                  "downstream port and the management poll stay off");
    return ESP_OK;
}
#endif /* CONFIG_POWER_BASE_REENROLL_BUTTON */

void pb_enrollment_get_status(pb_enrollment_status_t *out)
{
    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->state = s_ctl.state;
    out->last = s_ctl.last;
    out->attempt = s_ctl.attempt;
    out->resumed = s_ctl.resumed;
    out->endpoint_policy = s_ctl.endpoint_policy;
    memcpy(out->eui64, s_ctl.eui64, sizeof(out->eui64));
    if (s_ctl.state == PB_ENROLL_STATE_ENROLLED && s_ctl.has_bridge_endpoint) {
        out->has_bridge_endpoint = true;
        out->bridge_endpoint = s_ctl.bridge_endpoint;
    }
    out->has_mgmt = s_ctl.state == PB_ENROLL_STATE_ENROLLED && s_ctl.has_mgmt;
    xSemaphoreGive(s_lock);
    out->joiner_eui64_matches = s_joiner_eui64_matches;
}

static esp_err_t request_join(char pskd[PB_ENROLL_PSKD_BUF_LEN], const pb_enroll_bridge_endpoint_t *endpoint,
                              const pb_enroll_mgmt_credential_t *mgmt)
{
    if (s_reenroll_window) {
        /* The BLE admission rules refuse every start in such a boot already;
         * this holds for any other caller. Nothing is started or erased. */
        pb_enroll_wipe(pskd, PB_ENROLL_PSKD_BUF_LEN);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Drop any outcome left from an earlier attempt; the attempt number
     * would reject it anyway. */
    joiner_event_t stale;
    while (xQueueReceive(s_joiner_q, &stale, 0) == pdTRUE) {
    }
    esp_err_t err;
    if (mgmt != NULL) {
        err = pb_enroll_ctl_request_join_managed(&s_ctl, pskd, endpoint, mgmt, now_ms());
    } else if (endpoint != NULL) {
        err = pb_enroll_ctl_request_join_with_endpoint(&s_ctl, pskd, endpoint, now_ms());
    } else {
        err = pb_enroll_ctl_request_join(&s_ctl, pskd, now_ms());
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t pb_enrollment_request_join(char pskd[PB_ENROLL_PSKD_BUF_LEN])
{
    return request_join(pskd, NULL, NULL);
}

esp_err_t pb_enrollment_request_join_managed(char pskd[PB_ENROLL_PSKD_BUF_LEN],
                                             const pb_enroll_bridge_endpoint_t *endpoint,
                                             const pb_enroll_mgmt_credential_t *mgmt)
{
    if (endpoint == NULL || mgmt == NULL) {
        pb_enroll_wipe(pskd, PB_ENROLL_PSKD_BUF_LEN);
        return ESP_ERR_INVALID_ARG;
    }
    return request_join(pskd, endpoint, mgmt);
}

esp_err_t pb_enrollment_request_join_with_endpoint(char pskd[PB_ENROLL_PSKD_BUF_LEN],
                                                   const pb_enroll_bridge_endpoint_t *endpoint)
{
    if (endpoint == NULL) {
        pb_enroll_wipe(pskd, PB_ENROLL_PSKD_BUF_LEN);
        return ESP_ERR_INVALID_ARG;
    }
    return request_join(pskd, endpoint, NULL);
}

esp_err_t pb_enrollment_cancel(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = pb_enroll_ctl_cancel(&s_ctl);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t pb_enrollment_forget(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ctl.state == PB_ENROLL_STATE_JOINING || s_ctl.state == PB_ENROLL_STATE_ATTACHING) {
        (void)pb_enroll_ctl_cancel(&s_ctl);
    }
    esp_err_t err = thread_manager_stop();
    if (err == ESP_OK) {
        err = pb_enroll_marker_clear();
    }
    if (err == ESP_OK) {
        err = thread_manager_erase_persistent_info();
    }
    xSemaphoreGive(s_lock);
    return err;
}
