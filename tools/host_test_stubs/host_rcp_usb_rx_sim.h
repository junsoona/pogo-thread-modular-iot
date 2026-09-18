/*
 * Scripted fake for rcp/main/rcp_usb_rx_recovery.h's
 * __real_esp_openthread_platform_process() seam and its controlled-restart
 * hook (rcp_usb_rx_recovery_restart()) -- backs
 * tools/test_rcp_usb_rx_recovery.c, which links the REAL, unmodified
 * rcp/main/esp_openthread_platform_process_wrap.c against this file,
 * exactly like tools/host_test_stubs/host_rcp_uart_tx_sim.c backs
 * tools/test_rcp_uart_tx_retry.c for the (separate) USB Spinel TX fix.
 *
 * A test scenario loads a fixed sequence of __real_esp_openthread_platform_process()
 * outcomes with host_rcp_rx_sim_set_script() (applied strictly in call
 * order); once the script is exhausted, further calls fall back to the
 * default step (host_rcp_rx_sim_set_default_step(), HOST_RCP_RX_STEP_OK by
 * default).
 *
 * Production's rcp_usb_rx_recovery_restart() is declared noreturn and, in
 * the real firmware, really never returns (a silent esp_restart()). This
 * fake instead longjmp()s to whatever sigsetjmp() checkpoint the test set
 * with host_rcp_rx_sim_set_recovery_checkpoint() -- so a scenario can prove
 * "__wrap_esp_openthread_platform_process() never returns to its caller on
 * timeout exhaustion" for real (via setjmp/longjmp control flow), not
 * merely by documentation.
 */
#pragma once

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "rcp_usb_rx_recovery.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HOST_RCP_RX_STEP_OK,   /* __real_esp_openthread_platform_process() returns ESP_OK; leaves errno untouched */
    HOST_RCP_RX_STEP_FAIL, /* returns `err` (any esp_err_t) with errno set to `err_errno` */
} host_rcp_rx_step_kind_t;

typedef struct {
    host_rcp_rx_step_kind_t kind;
    esp_err_t err;    /* HOST_RCP_RX_STEP_FAIL only */
    int err_errno;    /* HOST_RCP_RX_STEP_FAIL only */
} host_rcp_rx_step_t;

/* Clears every call counter/log, un-halts the device, resets the default
 * (post-script) step back to HOST_RCP_RX_STEP_OK, and restarts this sim's
 * own elapsed-time stopwatch. Call once at the start of every scenario. */
void host_rcp_rx_sim_reset(void);

/* Loads `steps[0..count)`, applied strictly in order, one per
 * __real_esp_openthread_platform_process() call. The array is copied --
 * `steps` need not outlive this call. Once exhausted, every further call
 * uses the default step (host_rcp_rx_sim_set_default_step(), or
 * HOST_RCP_RX_STEP_OK if never set). Ignored once the device is halted (see
 * rcp_usb_rx_recovery_restart()'s own comment below). */
void host_rcp_rx_sim_set_script(const host_rcp_rx_step_t *steps, size_t count);

/* Sets the step every __real_esp_openthread_platform_process() call uses
 * once the explicit script (if any) is exhausted -- e.g. a repeating
 * HOST_RCP_RX_STEP_FAIL{ESP_FAIL, EIO} to simulate a connection that never
 * clears, without needing a script array long enough to outlast
 * RCP_USB_RX_RECOVERY_TOTAL_TIMEOUT_MS. */
void host_rcp_rx_sim_set_default_step(host_rcp_rx_step_t step);

size_t host_rcp_rx_sim_real_call_count(void);
int host_rcp_rx_sim_recovery_call_count(void);

/* `errno` as observed at the very start of the `index`-th
 * __real_esp_openthread_platform_process() call, BEFORE this fake touches
 * it at all -- lets a test prove __wrap_esp_openthread_platform_process()
 * really does reset errno to 0 immediately before EVERY real call
 * (including retries), not just the first one. `index` must be <
 * host_rcp_rx_sim_real_call_count(). */
int host_rcp_rx_sim_errno_at_entry(size_t index);

/* Milliseconds of REAL wall-clock time (this stub's vTaskDelay() really
 * sleeps -- see tools/host_test_stubs/freertos/task.h) elapsed since the
 * last host_rcp_rx_sim_reset(). Read this right after
 * __wrap_esp_openthread_platform_process() returns (or after the recovery
 * longjmp lands) to prove N scripted transient EIO failures actually cost
 * roughly N * RCP_USB_RX_RECOVERY_RETRY_YIELD_MS of real sleeping -- a
 * busy-spin retry loop would instead complete in near-zero time. */
double host_rcp_rx_sim_elapsed_ms(void);

/*
 * Registers the sigsetjmp() checkpoint this fake's
 * rcp_usb_rx_recovery_restart() longjmp()s back to, standing in for a real
 * device reboot (control never resumes at the call site, in either build).
 * A scenario must call this (with a jmp_buf it set up via
 * `if (sigsetjmp(buf, 1) == 0) { __wrap_esp_openthread_platform_process(...); ... }`)
 * before any call that might exhaust the retry budget. Once
 * rcp_usb_rx_recovery_restart() has fired at all, this sim also marks the
 * device "halted": every SUBSEQUENT __real_esp_openthread_platform_process()
 * call unconditionally returns the same fatal step it just fired on --
 * exactly like a real chip mid-reboot, nothing can recover through the
 * same session.
 */
void host_rcp_rx_sim_set_recovery_checkpoint(sigjmp_buf *checkpoint);

#ifdef __cplusplus
}
#endif
