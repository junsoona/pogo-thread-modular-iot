#include "enrollment_reenroll.h"

#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "power_base_reenroll/pb_reenroll_button.h"
#include "thread_manager/thread_manager.h"
#include "enrollment_mgmt.h"

static const char *TAG = "pb_reenroll_rt";

/* Lets the last log lines out before esp_restart(). */
#define RESTART_DELAY_MS 300u

/* The BLE wire carries these as numbers (pb_ble_proto.h). */
_Static_assert(PB_REENROLL_COND_NOT_ENROLLED == PB_BLE_LR_COND_NOT_ENROLLED &&
                   PB_REENROLL_COND_ENROLLED == PB_BLE_LR_COND_ENROLLED &&
                   PB_REENROLL_COND_REMOTE_RELEASE_PENDING == PB_BLE_LR_COND_REMOTE_RELEASE_PENDING &&
                   PB_REENROLL_COND_RESET_PENDING == PB_BLE_LR_COND_RESET_PENDING &&
                   PB_REENROLL_COND_UNREADABLE == PB_BLE_LR_COND_UNREADABLE,
               "pb_reenroll_condition_t is part of the BLE wire format");
_Static_assert(PB_REENROLL_REASON_NONE == PB_BLE_LR_REASON_NONE &&
                   PB_REENROLL_REASON_JOURNAL_STORE_FAILED == PB_BLE_LR_REASON_JOURNAL_STORE_FAILED &&
                   PB_REENROLL_REASON_NOT_ENROLLED == PB_BLE_LR_REASON_NOT_ENROLLED &&
                   PB_REENROLL_REASON_REMOTE_RELEASE_PENDING == PB_BLE_LR_REASON_REMOTE_RELEASE_PENDING &&
                   PB_REENROLL_REASON_OTHER_RESET_PENDING == PB_BLE_LR_REASON_OTHER_RESET_PENDING &&
                   PB_REENROLL_REASON_RECORD_CLEAR_FAILED == PB_BLE_LR_REASON_RECORD_CLEAR_FAILED &&
                   PB_REENROLL_REASON_RECORD_STILL_PRESENT == PB_BLE_LR_REASON_RECORD_STILL_PRESENT &&
                   PB_REENROLL_REASON_NETWORK_ERASE_FAILED == PB_BLE_LR_REASON_NETWORK_ERASE_FAILED &&
                   PB_REENROLL_REASON_NETWORK_NOT_EMPTY == PB_BLE_LR_REASON_NETWORK_NOT_EMPTY &&
                   PB_REENROLL_REASON_JOURNAL_FINAL_FAILED == PB_BLE_LR_REASON_JOURNAL_FINAL_FAILED,
               "pb_reenroll_reason_t is part of the BLE wire format");
_Static_assert(PB_REENROLL_RESET_NOT_STARTED == PB_BLE_LR_RESULT_NOT_STARTED &&
                   PB_REENROLL_RESET_INCOMPLETE == PB_BLE_LR_RESULT_INCOMPLETE &&
                   PB_REENROLL_RESET_VERIFIED == PB_BLE_LR_RESULT_VERIFIED,
               "pb_reenroll_reset_result_t is what the BLE worker returns");
_Static_assert(PB_REENROLL_WINDOW_MS == PB_BLE_REENROLL_WINDOW_MS, "one window length");
_Static_assert(PB_REENROLL_OP_ID_LEN == PB_BLE_OP_ID_LEN, "one operation id length");

static uint8_t s_eui64[PB_ENROLL_EUI64_LEN];
static bool s_enabled;

/* ---- pb_reenroll_ops_t: the same OpenThread calls a remote unenroll makes ---- */

static void op_quiesce(void *ctx)
{
    (void)ctx;
    /* A window boot and the boot path run nothing else: no service, no
     * exporter, no downstream port, no management poll was ever started. */
    thread_manager_joiner_stop();
    if (thread_manager_stop() != ESP_OK) {
        ESP_LOGW(TAG, "thread_manager_stop failed before the reset -- the erase below refuses if Thread is still up");
    }
}

static esp_err_t op_network_erase(void *ctx)
{
    (void)ctx;
    return pb_network_store_erase();
}

static bool op_network_erased(void *ctx)
{
    (void)ctx;
    return pb_network_store_is_empty();
}

static const pb_reenroll_ops_t k_ops = {
    .quiesce = op_quiesce,
    .network_erase = op_network_erase,
    .network_erased = op_network_erased,
    .ctx = NULL,
};

static void restart_now(const char *why)
{
    ESP_LOGW(TAG, "%s -- restarting", why);
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
    esp_restart();
}

bool pb_reenroll_runtime_enabled(void)
{
    return s_enabled;
}

bool pb_reenroll_runtime_configure(const uint8_t eui64[PB_ENROLL_EUI64_LEN], const power_base_config_t *cfg)
{
    memcpy(s_eui64, eui64, sizeof(s_eui64));
    s_enabled = false;
#if CONFIG_POWER_BASE_REENROLL_BUTTON
    const pb_reenroll_board_pins_t pins = {
        .uart_tx_gpio = cfg->downstream_port.tx_gpio,
        .uart_rx_gpio = cfg->downstream_port.rx_gpio,
        .detect_in_gpio = cfg->downstream_port.detect_in_gpio,
        .detect_out_gpio = cfg->downstream_port.detect_out_gpio,
        .power_enable_gpio = cfg->power_enable_gpio,
    };
    const pb_reenroll_gpio_check_t check = pb_reenroll_button_gpio_check(CONFIG_POWER_BASE_REENROLL_BUTTON_GPIO, &pins);
    if (check != PB_REENROLL_GPIO_OK) {
        ESP_LOGE(TAG, "re-enrollment button: GPIO%d is not usable on this board (%s) -- the feature is OFF for this "
                      "boot; nothing else changes",
                 (int)CONFIG_POWER_BASE_REENROLL_BUTTON_GPIO, pb_reenroll_gpio_check_name(check));
        return false;
    }
    s_enabled = true;
    ESP_LOGI(TAG, "re-enrollment button: GPIO%d, pressed = %s (hold %u s, then release)",
             (int)CONFIG_POWER_BASE_REENROLL_BUTTON_GPIO, CONFIG_POWER_BASE_REENROLL_BUTTON_ACTIVE_LOW ? "low" : "high",
             (unsigned)(PB_REENROLL_BTN_HOLD_MS / 1000u));
#else
    (void)cfg;
#endif
    return s_enabled;
}

pb_reenroll_boot_action_t pb_reenroll_runtime_boot(bool *recovery)
{
    pb_reenroll_boot_t boot;
    pb_reenroll_boot(s_eui64, s_enabled, &k_ops, &boot);
    if (recovery != NULL) {
        *recovery = boot.recovery;
    }
    if (boot.action == PB_REENROLL_BOOT_RESTART) {
        /* OpenThread still holds in RAM what it loaded at init; a restart is
         * the one clean way to "nothing stored". */
        restart_now("local reset finished at boot (erase verified)");
    }
    if (boot.action == PB_REENROLL_BOOT_SAFE_STOP || boot.action == PB_REENROLL_BOOT_WINDOW) {
        /* Neither resumes anything: Thread stays as app_main left it. */
        (void)thread_manager_stop();
    }
    return boot.action;
}

uint8_t pb_reenroll_runtime_local_reset(const uint8_t op_id[PB_BLE_OP_ID_LEN], uint8_t *reason, void *ctx)
{
    (void)ctx;
    pb_reenroll_reason_t why = PB_REENROLL_REASON_NONE;
    pb_reenroll_reset_result_t result = pb_reenroll_reset_begin(s_eui64, op_id, &k_ops, &why);
    if (reason != NULL) {
        *reason = (uint8_t)why;
    }
    return (uint8_t)result;
}

void pb_reenroll_runtime_snapshot(pb_ble_lr_snapshot_t *out, void *ctx)
{
    (void)ctx;
    pb_reenroll_snapshot_t snap;
    pb_reenroll_snapshot(s_eui64, &snap);
    memset(out, 0, sizeof(*out));
    out->condition = (uint8_t)snap.condition;
    if (snap.journal_status == PB_REENROLL_REC_VALID) {
        out->journal_valid = true;
        out->journal_verified = snap.journal.stage == PB_REENROLL_STAGE_VERIFIED;
        memcpy(out->journal_op, snap.journal.op_id, sizeof(out->journal_op));
    }
}

#if CONFIG_POWER_BASE_REENROLL_BUTTON

#define BUTTON_TASK_STACK 3584

static pb_reenroll_probe_cb_t s_probe;
/* Set once the window request is stored; the restart follows. */
static bool s_requested;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool button_pressed(void)
{
    const int level = gpio_get_level((gpio_num_t)CONFIG_POWER_BASE_REENROLL_BUTTON_GPIO);
#if CONFIG_POWER_BASE_REENROLL_BUTTON_ACTIVE_LOW
    return level == 0;
#else
    return level != 0;
#endif
}

/* A task, sampled: no interrupt handler exists for this pin, so nothing here
 * can run in an ISR. The task itself only reads the pin, asks the gate,
 * stores the request record and restarts. It erases nothing, and it never
 * starts or stops BLE or Thread. */
static void button_task(void *arg)
{
    (void)arg;
    pb_reenroll_button_t button;
    pb_reenroll_button_init(&button);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(PB_REENROLL_BTN_POLL_MS));
        if (!pb_reenroll_button_step(&button, button_pressed(), now_ms())) {
            continue;
        }
        pb_reenroll_gate_input_t in = {
            .feature_enabled = s_enabled,
            .already_requested = s_requested,
        };
        if (s_probe != NULL) {
            s_probe(&in);
        }
        const pb_reenroll_gate_t gate = pb_reenroll_gate(&in);
        if (gate != PB_REENROLL_GATE_OK) {
            ESP_LOGW(TAG, "re-enrollment button: long press ignored (%s) -- nothing changed", pb_reenroll_gate_name(gate));
            continue;
        }
        if (pb_reenroll_window_store(s_eui64) != ESP_OK) {
            ESP_LOGE(TAG, "re-enrollment button: the window request could not be stored -- nothing changed, the "
                          "board keeps running as it was");
            continue;
        }
        s_requested = true;
        restart_now("re-enrollment button: window requested; the enrollment is paused for one boot, nothing is erased");
    }
}

esp_err_t pb_reenroll_runtime_start_button(pb_reenroll_probe_cb_t probe)
{
    if (!s_enabled) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_probe = probe;
    const gpio_config_t in_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_POWER_BASE_REENROLL_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
#if CONFIG_POWER_BASE_REENROLL_BUTTON_ACTIVE_LOW
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#else
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&in_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "re-enrollment button: gpio_config failed (%s) -- the button is not watched in this boot",
                 esp_err_to_name(err));
        return err;
    }
    if (xTaskCreate(button_task, "pb_reenroll_btn", BUTTON_TASK_STACK, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#else /* !CONFIG_POWER_BASE_REENROLL_BUTTON */

esp_err_t pb_reenroll_runtime_start_button(pb_reenroll_probe_cb_t probe)
{
    (void)probe;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
