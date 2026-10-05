/*
 * pb_thread:mode=2 runtime: wires components/power_base_enrollment's
 * controller to thread_manager/OpenThread and to at most one commissioning
 * transport -- the bench "pb_join" console (sdkconfig.bench_joiner) or the
 * BLE transport (sdkconfig.ble_enrollment). See docs/power-base.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"
#include "power_base_enrollment/power_base_enrollment.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called exactly once per boot, and only when the board is enrolled (valid
 * marker + matching Dataset at boot, or Joiner + attach + marker this boot).
 * The caller starts its normal services from here. */
typedef void (*pb_enrollment_ready_cb_t)(void);

/*
 * `endpoint_policy`: what the prepared image says about enrolling without a
 * bridge endpoint (app_main.c reads it from cfg/pb_bridge). It is handed to
 * the controller before the boot rule runs, so it holds for every transport
 * and for the resume decision alike.
 *
 * Thread must be initialised and disabled. Applies the boot rule: on resume,
 * `ready` runs before this returns; otherwise the board stays in the safe
 * wait -- with a Joiner trigger only in a CONFIG_POWER_BASE_BENCH_JOINER_CONSOLE
 * or CONFIG_POWER_BASE_BLE_ENROLLMENT build. Failure = resource creation
 * failed.
 */
esp_err_t pb_enrollment_runtime_start(const uint8_t eui64[PB_ENROLL_EUI64_LEN],
                                      pb_enroll_endpoint_policy_t endpoint_policy, pb_enrollment_ready_cb_t ready);

typedef struct {
    pb_enroll_state_t state;
    pb_enroll_result_t last;
    uint32_t attempt;
    bool resumed;
    bool joiner_eui64_matches; /* OpenThread factory EUI-64 == this Base's uuid */
    pb_enroll_endpoint_policy_t endpoint_policy; /* as given to pb_enrollment_runtime_start() */
    uint8_t eui64[PB_ENROLL_EUI64_LEN];
    /* Meaningful once `state` is ENROLLED: the bridge endpoint stored with
     * this enrollment (delivered this boot, or restored from the version 2
     * record). False = none was delivered; cfg/pb_bridge applies. */
    bool has_bridge_endpoint;
    pb_enroll_bridge_endpoint_t bridge_endpoint;
} pb_enrollment_status_t;

void pb_enrollment_get_status(pb_enrollment_status_t *out);

/* `pskd` (PB_ENROLL_PSKD_BUF_LEN bytes) is wiped before returning; see
 * pb_enroll_ctl_request_join() for the return codes -- including
 * ESP_ERR_NOT_ALLOWED when the endpoint policy refuses a request without a
 * bridge endpoint. */
esp_err_t pb_enrollment_request_join(char pskd[PB_ENROLL_PSKD_BUF_LEN]);
/* The same with a delivered bridge endpoint (copied); see
 * pb_enroll_ctl_request_join_with_endpoint(). */
esp_err_t pb_enrollment_request_join_with_endpoint(char pskd[PB_ENROLL_PSKD_BUF_LEN],
                                                   const pb_enroll_bridge_endpoint_t *endpoint);
esp_err_t pb_enrollment_cancel(void);

/* Bench reset of THIS board: stops Thread, erases the enrollment marker and
 * OpenThread's persistent info. The caller must restart the chip. */
esp_err_t pb_enrollment_forget(void);

#if CONFIG_POWER_BASE_BENCH_JOINER_CONSOLE
esp_err_t pb_enrollment_console_start(void);
#endif

#if CONFIG_POWER_BASE_BLE_ENROLLMENT
/*
 * BLE transport (enrollment_ble.c). Both run in the enrollment task only --
 * never in a BLE callback -- and must not be called with the controller
 * mutex held.
 *
 * start: loads this board's Security 2 credential and opens the BLE
 * enrollment service. Any failure (no or damaged credential, a credential
 * for another EUI-64, Joiner EUI-64 mismatch, an unusable endpoint policy in
 * the prepared image, BLE stack error) leaves BLE
 * off; the board then simply stays in the safe wait.
 *
 * poll: runs the requests BLE clients queued (through
 * pb_enrollment_request_join()/pb_enrollment_request_join_with_endpoint()/
 * pb_enrollment_cancel()) and stops BLE --
 * advertising, connection, session -- once the board is enrolled, locked out
 * or past its advertising window. Thread and the services are not touched.
 */
esp_err_t pb_enrollment_ble_start(void);
void pb_enrollment_ble_poll(void);
#endif

#ifdef __cplusplus
}
#endif
