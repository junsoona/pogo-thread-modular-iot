/*
 * Guaranteed-delivery-or-controlled-restart USB Spinel TX retry -- see
 * rcp_uart_tx_retry.h's own header comment for the full leading-hypothesis
 * writeup and contract this implements. Depends only on the I/O seam
 * declared in that header plus FreeRTOS/esp_timer, exactly like every
 * other host-tested production file in this repository -- no ESP-IDF VFS
 * or USB Serial/JTAG header is included here, so this file links
 * unmodified into both the real firmware (via rcp_uart_tx_io.c) and
 * tools/test_rcp_uart_tx_retry.c (via
 * tools/host_test_stubs/host_rcp_uart_tx_sim.c).
 */
#include "rcp_uart_tx_retry.h"

#include <errno.h>
#include <stdbool.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * otPlatUartSendDone()'s real prototype lives in the `openthread`
 * component's PRIVATE header
 * openthread/examples/platforms/utils/uart.h, not visible outside that
 * component -- forward-declared here instead, exactly like
 * esp_ot_rcp.c already forward-declares otAppNcpInit() rather than
 * reaching into a private openthread header. A host test provides its own
 * definition (tools/host_test_stubs/host_rcp_uart_tx_sim.c) that counts
 * calls instead.
 */
extern void otPlatUartSendDone(void);

/*
 * Cached across calls, exactly like esp_openthread_uart.c's own s_uart_fd:
 * the USB Serial/JTAG hardware (and its VFS registration) persists across
 * host cable reconnects, so there is no reason to reopen it on every
 * frame -- only if it was never successfully opened yet does
 * rcp_uart_tx_open() get retried, within the same bounded budget as any
 * other transient failure below.
 */
static int s_tx_fd = -1;

static inline uint32_t retry_yield_ticks(void)
{
    TickType_t ticks = pdMS_TO_TICKS(RCP_UART_TX_RETRY_YIELD_MS);
    return ticks > 0 ? ticks : 1; /* never zero -- always a real yield */
}

otError rcp_uart_tx_retry_send(const uint8_t *buf, uint16_t buf_length)
{
    size_t total_written = 0;
    int64_t deadline_us = esp_timer_get_time() + (int64_t)RCP_UART_TX_TOTAL_TIMEOUT_MS * 1000;

    if (s_tx_fd < 0) {
        s_tx_fd = rcp_uart_tx_open();
    }

    while (total_written < (size_t)buf_length) {
        if (s_tx_fd < 0) {
            /* Device not open yet (driver not ready, or gone). Treated
             * exactly like any other transient failure below: retried
             * within the same bounded budget, never spun on tightly. */
            if (esp_timer_get_time() >= deadline_us) {
                /* Never returns -- see rcp_uart_tx_recovery_restart()'s
                 * own comment for why this must not become a return
                 * value instead. */
                rcp_uart_tx_recovery_restart();
            }
            vTaskDelay(retry_yield_ticks());
            s_tx_fd = rcp_uart_tx_open();
            continue;
        }

        ssize_t n = rcp_uart_tx_write(s_tx_fd, buf + total_written, (size_t)buf_length - total_written);
        /*
         * Preserves stock otPlatUartSend()'s own unconditional post-write
         * usb_serial_jtag_ll_txfifo_flush() (tagged "DIG-727" in
         * $IDF_PATH/components/openthread/src/port/esp_openthread_uart.c):
         * called immediately after every write() attempt, success or
         * not, with the same write()-then-flush ordering -- see
         * rcp_uart_tx_retry.h's own comment on rcp_uart_tx_flush().
         */
        rcp_uart_tx_flush();

        if (n > 0) {
            /* Real progress (a full OR partial write) -- keep sending the
             * remainder immediately, no delay needed. */
            total_written += (size_t)n;
            continue;
        }

        /* n == 0 (nothing accepted this attempt) or n < 0 (error). */
        int err = (n < 0) ? errno : 0;
        bool transient = (n == 0) || (err == EAGAIN) || (err == EWOULDBLOCK) || (err == EIO);

        if (!transient) {
            /* A real, non-transient error (e.g. ENODEV, EBADF) -- retrying
             * will not help. Never returns. */
            rcp_uart_tx_recovery_restart();
        }

        if (esp_timer_get_time() >= deadline_us) {
            /* Permanent-loss timeout: the connection has not merely
             * blipped, it has been unavailable for the whole budget.
             * Never returns. */
            rcp_uart_tx_recovery_restart();
        }

        vTaskDelay(retry_yield_ticks()); /* bounded yield -- never a busy spin */
    }

    /*
     * Reached only once every byte of `buf` was actually confirmed
     * written -- exactly one SendDone call, only for a genuinely complete
     * send. See rcp_uart_tx_recovery_restart()'s own comment for why the
     * failure paths above call that instead of ever reaching here.
     */
    otPlatUartSendDone();
    return OT_ERROR_NONE;
}
