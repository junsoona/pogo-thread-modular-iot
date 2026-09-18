/*
 * Production implementation of the I/O seam declared in
 * rcp_uart_tx_retry.h, against the real USB Serial/JTAG VFS device.
 *
 * esp_openthread_host_rcp_usb_init() (upstream ESP-IDF, unmodified --
 * $IDF_PATH/components/openthread/src/port/esp_openthread_uart.c) already
 * installs the USB Serial/JTAG driver and registers/uses its VFS at
 * "/dev/usbserjtag" during esp_openthread_start(), well before
 * otAppNcpInit()/NcpSend() can ever be reached -- so by the time
 * rcp_uart_tx_open() below is first called, that device is already
 * present.
 *
 * Opening a SECOND, TX-only fd on the same VFS path here (rather than
 * reaching into that file's private s_uart_fd, which this project does not
 * touch) is safe: usb_serial_jtag_vfs.c's open()/write() are stateless
 * beyond a single shared non-blocking-mode flag, and the original RX-side
 * fd already sets that same flag the same way (O_NONBLOCK) -- see
 * $IDF_PATH/components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c.
 *
 * A host test links tools/host_test_stubs/host_rcp_uart_tx_sim.c instead of
 * this file (see rcp_uart_tx_retry.h's own comment) -- this file is
 * therefore never itself linked into tools/test_rcp_uart_tx_retry.c and
 * has no host-buildable stand-in of its own to keep it honest beyond the
 * real `idf.py build` covered in the final report.
 */
#include "rcp_uart_tx_retry.h"

#include <fcntl.h>
#include <unistd.h>

#include "esp_system.h"
#include "hal/usb_serial_jtag_ll.h"

int rcp_uart_tx_open(void)
{
    return open("/dev/usbserjtag", O_WRONLY | O_NONBLOCK);
}

ssize_t rcp_uart_tx_write(int fd, const uint8_t *buf, size_t len)
{
    return write(fd, buf, len);
}

void rcp_uart_tx_flush(void)
{
    /* Same direct hardware-level call stock otPlatUartSend() makes right
     * after its own write() -- see rcp_uart_tx_retry.h's own comment on
     * this function. Not gated by CONFIG_OPENTHREAD_RCP_USB_SERIAL_JTAG
     * the way upstream's call site is: this whole file is only ever
     * compiled in as part of rcp/'s own project, which always uses USB
     * Serial/JTAG transport (see docs/otbr-rcp.md section 2) -- there is
     * no UART/SPI build of this project for the guard to matter for. */
    usb_serial_jtag_ll_txfifo_flush();
}

void rcp_uart_tx_recovery_restart(void)
{
    /*
     * Silent controlled restart: esp_restart() itself never logs (see
     * $IDF_PATH/components/esp_system/esp_system.c -- it only runs
     * registered shutdown handlers, none of which this project registers,
     * then a low-level noos reset) and never touches any console/VFS fd,
     * so nothing reaches the USB Spinel stream. On top of that,
     * rcp/sdkconfig.defaults's CONFIG_LOG_DEFAULT_LEVEL_NONE=y already
     * compiles out ESP_LOGx bodies process-wide, so even a future
     * shutdown-handler registration elsewhere in this project could not
     * newly leak text here without deliberately bypassing logging.
     * esp_restart() is declared __attribute__((noreturn)) in
     * esp_system.h, matching this function's own declared contract.
     */
    esp_restart();
}
