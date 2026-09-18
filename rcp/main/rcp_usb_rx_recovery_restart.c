/*
 * Production implementation of rcp_usb_rx_recovery.h's noreturn recovery
 * hook -- called only once esp_openthread_platform_process_wrap.c's
 * __wrap_esp_openthread_platform_process() has retried a transient USB RX
 * EIO for the full RCP_USB_RX_RECOVERY_TOTAL_TIMEOUT_MS budget with no
 * success.
 *
 * Silent controlled restart, same reasoning as the existing USB Spinel TX
 * retry's own rcp_uart_tx_recovery_restart() (rcp_uart_tx_io.c) --
 * intentionally kept as a SEPARATE function/file rather than reused from
 * that one: this is a fix for a different, confirmed failure on a
 * different call path (see esp_openthread_platform_process_wrap.c's own
 * header comment), kept independently auditable/removable rather than
 * silently coupled to the TX fix through a shared restart hook.
 *
 * esp_restart() itself never logs (see
 * $IDF_PATH/components/esp_system/esp_system.c -- it only runs registered
 * shutdown handlers, none of which this project registers, then a
 * low-level noos reset) and never touches any console/VFS fd, so nothing
 * reaches the USB Spinel stream. On top of that, rcp/sdkconfig.defaults's
 * CONFIG_LOG_DEFAULT_LEVEL_NONE=y already compiles out ESP_LOGx bodies
 * process-wide, so no future change elsewhere in this project could newly
 * leak text here without deliberately bypassing logging.
 * esp_restart() is declared __attribute__((noreturn)) in esp_system.h,
 * matching this function's own declared contract
 * (rcp_usb_rx_recovery.h).
 *
 * A host test (tools/test_rcp_usb_rx_recovery.c) links a fake
 * implementation of this same function instead of this file (see
 * tools/host_test_stubs/host_rcp_usb_rx_sim.c) -- this file is therefore
 * never itself linked into that host test.
 */
#include "rcp_usb_rx_recovery.h"

#include "esp_system.h"

void rcp_usb_rx_recovery_restart(void)
{
    esp_restart();
}
