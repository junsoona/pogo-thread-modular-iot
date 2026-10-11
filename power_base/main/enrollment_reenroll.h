/*
 * Local re-enrollment runtime (docs/power-base.md "BOOT 버튼 로컬 재등록"):
 * wires components/power_base_reenroll's decisions to the button GPIO,
 * OpenThread's store and the BLE worker.
 *
 * The boot step (pb_reenroll_runtime_boot) is in every Power Base build and
 * runs in every pb_thread:mode=2 boot, because a reset that was journaled
 * must be finished whatever firmware boots next; on a board that never used
 * the feature it reads nothing, writes nothing and returns NORMAL. Everything
 * else -- the button, the window, the BLE messages -- exists only with
 * CONFIG_POWER_BASE_REENROLL_BUTTON, which needs the BLE enrollment build.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "power_base_ble_enroll/pb_ble_admission.h"
#include "power_base_config/power_base_config.h"
#include "power_base_enrollment/power_base_enrollment.h"
#include "power_base_reenroll/pb_reenroll.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Mode 2 only, once, before pb_reenroll_runtime_boot(): decides whether this
 * boot has the feature. False -- and one log line saying why -- when the
 * build has no button, or the configured GPIO is reserved on this chip or
 * already used by this board's cfg (POGO UART, DETECT, power enable). Touches
 * no GPIO.
 */
bool pb_reenroll_runtime_configure(const uint8_t eui64[PB_ENROLL_EUI64_LEN], const power_base_config_t *cfg);
bool pb_reenroll_runtime_enabled(void);

/*
 * Mode 2 only, after thread_manager_init() + thread_manager_stop() and before
 * anything looks at the enrollment record. Restarts the chip itself (and does
 * not return) when it finished a journaled reset; otherwise returns what the
 * caller must do: NORMAL, WINDOW or SAFE_STOP. *recovery (WINDOW only): the
 * window is there to replace a journal that cannot be read.
 */
pb_reenroll_boot_action_t pb_reenroll_runtime_boot(bool *recovery);

/* Fills what only the caller knows: is an enrollment attempt under way, is a
 * remote unenroll being carried out. Called from the button task. */
typedef void (*pb_reenroll_probe_cb_t)(pb_reenroll_gate_input_t *in);

/*
 * Starts the button task (never an ISR: the pin is sampled every
 * PB_REENROLL_BTN_POLL_MS). On a qualifying long press it asks
 * pb_reenroll_gate(), stores the one-shot window request and restarts the
 * chip; when the request cannot be stored nothing changes and nothing
 * restarts. ESP_ERR_NOT_SUPPORTED when this boot has no feature. `probe` may
 * be NULL (a stopped board: nothing can be under way).
 */
esp_err_t pb_reenroll_runtime_start_button(pb_reenroll_probe_cb_t probe);

/* pb_ble_worker_ops_t.local_reset / .get_local_reset for enrollment_ble.c.
 * Worker context only: they read and write flash. */
uint8_t pb_reenroll_runtime_local_reset(const uint8_t op_id[PB_BLE_OP_ID_LEN], uint8_t *reason, void *ctx);
void pb_reenroll_runtime_snapshot(pb_ble_lr_snapshot_t *out, void *ctx);

#ifdef __cplusplus
}
#endif
