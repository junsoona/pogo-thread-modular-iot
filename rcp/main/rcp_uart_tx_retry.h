/*
 * Guaranteed-delivery-or-controlled-restart retry policy for the RCP's USB
 * Serial/JTAG Spinel TX path -- see docs/otbr-rcp.md's "USB Spinel TX
 * retry" section for the full incident writeup (repeated real-hardware
 * RESET_FAULT under the Power Base -> SHT45 -> PMS5003 chain) this is a
 * candidate workaround for, pending a real-hardware soak test.
 *
 * Leading hypothesis (not yet confirmed by a real-hardware soak test --
 * see docs/otbr-rcp.md): ESP-IDF v6.1's own
 * $IDF_PATH/components/openthread/src/port/esp_openthread_uart.c
 * otPlatUartSend() calls write() exactly once and unconditionally calls
 * otPlatUartSendDone() right after, even if that write() failed outright
 * or wrote fewer bytes than requested (espressif/esp-idf#18490, #18506).
 * $IDF_PATH/components/openthread/src/ncp/esp_openthread_ncp.cpp's own
 * NcpSend() (the only caller, via otNcpHdlcInit()) then IGNORES
 * otPlatUartSend()'s returned otError entirely and reports the full length
 * as accepted regardless -- this is why a failed/permanently-stuck send
 * must never simply return an error code up to NcpSend(): NcpSend() would
 * report it to the OpenThread NCP/HDLC layer as a complete success anyway,
 * the same corruption-disguised-as-success bug this file exists to avoid.
 * So on a non-transient or timed-out failure, this file's
 * rcp_uart_tx_retry_send() does not return to its caller at all -- it
 * hands off to a controlled restart/recovery hook instead (see below).
 * USB Serial/JTAG's own VFS write path
 * ($IDF_PATH/components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c)
 * can return -1/EIO for the width of one usb_serial_jtag_is_connected()
 * false negative (a few ms around attach/enumeration) even though the link
 * is otherwise fine one write() call later -- this is the transient case
 * the retry loop is meant to absorb.
 *
 * This header's rcp_uart_tx_retry_send() is factored out of the
 * ESP-IDF-specific `--wrap` glue (esp_ot_uart_send_wrap.c) so it can be
 * linked directly, unmodified, into a host test
 * (tools/test_rcp_uart_tx_retry.c) against a scripted fake I/O seam
 * (tools/host_test_stubs/host_rcp_uart_tx_sim.c) instead of the real
 * "/dev/usbserjtag" VFS device (rcp_uart_tx_io.c) -- exactly the same
 * "production .c compiled against a host stub seam" pattern this
 * repository already uses for driver/uart.h's host_uart_sim.c stand-in.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* ssize_t */

#include "openthread/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How long a single retry attempt sleeps after a transient (EIO/EAGAIN/
 * EWOULDBLOCK/0-byte) write before trying again. rcp/sdkconfig.defaults
 * sets CONFIG_FREERTOS_HZ=1000 (1 ms/tick), so this is >= 1 tick and
 * vTaskDelay() always actually yields instead of rounding down to zero.
 * Chosen small relative to RCP_UART_TX_TOTAL_TIMEOUT_MS so a genuinely
 * transient failure (the few-ms usb_serial_jtag_is_connected() false
 * negative from espressif/esp-idf#18490/#18506) gets several retries well
 * within budget.
 */
#define RCP_UART_TX_RETRY_YIELD_MS ((uint32_t)2u)

/*
 * Overall budget, across every retry, for ONE otPlatUartSend() call (one
 * already-HDLC-framed Spinel buffer) to finish transmitting. Wide enough to
 * absorb the transient failure this is meant to work around (observed on
 * the order of a few ms) with a large safety margin, but short enough to
 * stay far under any FreeRTOS task/interrupt watchdog timeout -- once this
 * elapses with no further progress, the USB link is treated as genuinely
 * (not just momentarily) gone and this call hands off to the recovery hook
 * below rather than looping forever.
 */
#define RCP_UART_TX_TOTAL_TIMEOUT_MS ((uint32_t)200u)

/*
 * I/O seam. Production (rcp_uart_tx_io.c) implements these against the
 * real "/dev/usbserjtag" USB Serial/JTAG VFS device; a host test instead
 * links tools/host_test_stubs/host_rcp_uart_tx_sim.c, which implements the
 * exact same functions against a scripted fake -- so
 * rcp_uart_tx_retry_send() below, the actual retry/backoff logic under
 * test, runs completely unmodified in both environments.
 *
 * rcp_uart_tx_open() may be called more than once (rcp_uart_tx_retry_send()
 * retries it, within the same bounded budget, if the device was not yet
 * available) and must return a fd usable with rcp_uart_tx_write(), or a
 * negative value on failure (errno set, POSIX open() convention).
 *
 * rcp_uart_tx_write() must have write()'s exact POSIX semantics: >=0 is the
 * number of bytes actually accepted (which may be less than `len`, or 0),
 * -1 on error with errno set.
 *
 * rcp_uart_tx_flush() preserves upstream stock otPlatUartSend()'s own
 * unconditional post-write usb_serial_jtag_ll_txfifo_flush() call (tagged
 * "DIG-727" in $IDF_PATH/components/openthread/src/port/esp_openthread_uart.c)
 * -- called immediately after EVERY rcp_uart_tx_write() attempt below,
 * success or not, exactly matching that file's own write()-then-flush
 * ordering, so a trailing sub-64-byte USB packet is still pushed to the
 * host promptly instead of waiting on the driver's own internal timeout.
 * Takes no fd: like the original LL call, it operates on the single
 * hardware TX FIFO, not a per-fd/per-VFS-device object.
 */
int rcp_uart_tx_open(void);
ssize_t rcp_uart_tx_write(int fd, const uint8_t *buf, size_t len);
void rcp_uart_tx_flush(void);

/*
 * Controlled restart/recovery hook: called instead of returning whenever
 * rcp_uart_tx_retry_send() cannot deliver a frame (a non-transient errno,
 * or RCP_UART_TX_TOTAL_TIMEOUT_MS exhausted with no further progress).
 * NEVER RETURNS (production: a silent esp_restart() -- no ESP_LOGx/printf,
 * nothing written to any console/VFS fd, see rcp_uart_tx_io.c's own
 * comment; a host test instead links a fake that longjmp()s out, so this
 * contract -- "control never resumes after this call, in either build" --
 * is exercised for real rather than merely documented).
 *
 * This is why rcp_uart_tx_retry_send() below can ONLY ever return
 * OT_ERROR_NONE, never OT_ERROR_FAILED: NcpSend() (the only caller of
 * otPlatUartSend(), see this file's own header comment) ignores whatever
 * otError it gets back and reports success to the OpenThread NCP/HDLC
 * layer regardless -- so a failure this function cannot recover from must
 * never reach NcpSend() as a return value at all, only as a restart.
 */
void rcp_uart_tx_recovery_restart(void) __attribute__((noreturn));

/*
 * Sends exactly one already-HDLC-framed Spinel buffer, retrying partial
 * writes and transient EIO/EAGAIN/EWOULDBLOCK/0-byte writes (bounded,
 * yielding waits -- never a busy spin: every retry actually sleeps
 * RCP_UART_TX_RETRY_YIELD_MS via vTaskDelay()) until every byte of `buf`
 * has been written, calls otPlatUartSendDone() EXACTLY ONCE (only once the
 * full frame is confirmed delivered), and returns OT_ERROR_NONE.
 *
 * If delivery cannot be completed -- a non-transient errno, or
 * RCP_UART_TX_TOTAL_TIMEOUT_MS elapses with no further progress -- this
 * function calls rcp_uart_tx_recovery_restart() and DOES NOT RETURN: it
 * never calls otPlatUartSendDone() and never reaches NcpSend() with any
 * return value (see rcp_uart_tx_recovery_restart()'s own comment for why).
 */
otError rcp_uart_tx_retry_send(const uint8_t *buf, uint16_t buf_length);

#ifdef __cplusplus
}
#endif
