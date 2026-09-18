/*
 * Linker-level override of the `openthread` component's own
 * src/port/esp_openthread_uart.c:otPlatUartSend() -- ESP-IDF's own
 * documented mechanism for redefining a public function without touching
 * $IDF_PATH itself, see
 * $IDF_PATH/examples/build_system/wrappers/README.md ("Using wrapper to
 * redefine IDF functions") and rcp/main/CMakeLists.txt's own comment for
 * the `-Wl,--wrap=otPlatUartSend` link flag this relies on.
 *
 * The real ESP-IDF install under ~/.espressif is never modified: this is a
 * project-local (rcp/main/) override, exactly like docs/otbr-rcp.md
 * describes for the rest of this project's ESP-IDF integration.
 *
 * `--wrap=otPlatUartSend` redirects every call to otPlatUartSend() (this
 * project has exactly one caller: esp_openthread_ncp.cpp's NcpSend(), via
 * otNcpHdlcInit()) to __wrap_otPlatUartSend() below instead. The actual
 * retry policy that fixes the real-hardware RESET_FAULT issue lives in
 * rcp_uart_tx_retry.c (host-tested directly, unmodified, by
 * tools/test_rcp_uart_tx_retry.c) -- this file only wires that linker
 * substitution to it.
 */
#include "rcp_uart_tx_retry.h"

otError __wrap_otPlatUartSend(const uint8_t *aBuf, uint16_t aBufLength)
{
    return rcp_uart_tx_retry_send(aBuf, aBufLength);
}
