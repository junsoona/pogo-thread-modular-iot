/*
 * Linker-level override of the `openthread` component's own
 * $IDF_PATH/components/openthread/src/esp_openthread_platform.cpp:
 * esp_openthread_platform_process().
 *
 * CONFIRMED by a real-hardware coredump (diagnostic build, section 10 of
 * docs/otbr-rcp.md), read back with `idf.py coredump-debug`:
 *
 *   - crashed task: ot_main
 *   - assert address 0x4200f9ee
 *   - esp_openthread.cpp:279's esp_openthread_launch_mainloop() returns,
 *     then esp_openthread.cpp:283's `assert(false)` fires
 *
 * The coredump does NOT preserve the `errno` value from the moment of the
 * panic, so it confirms ONLY that esp_openthread_launch_mainloop() returned
 * and that the subsequent assert(false) fired -- it does not, by itself,
 * identify which registered workflow or which specific error caused that
 * return. Steps 1-2 and 4-5 below are read directly from ESP-IDF v6.1's
 * own sources (never modified) and match the coredump exactly; step 3
 * (which workflow, and the EIO trigger) is the most plausible mechanism
 * this source reading points to, cross-checked against two real-hardware
 * trials (see docs/otbr-rcp.md section 12.1) rather than read out of the
 * coredump itself:
 *
 *   1. esp_openthread.cpp:esp_openthread_launch_mainloop()'s own `while`
 *      loop calls `select()` on the registered workflow fds, then calls
 *      esp_openthread_platform_process(instance, &mainloop) once per
 *      iteration.
 *   2. esp_openthread_platform.cpp:esp_openthread_platform_process() walks
 *      every registered workflow's process_func() via
 *      `ESP_RETURN_ON_ERROR(current_workflow->process_func(...), ...)` --
 *      the FIRST workflow to return non-ESP_OK makes
 *      esp_openthread_platform_process() itself return that same
 *      non-ESP_OK immediately.
 *   3. For this project's RCP-over-USB-Serial/JTAG build, FOUR workflows
 *      are registered during esp_openthread_platform_init(): alarm
 *      (esp_openthread_alarm.c), UART (esp_openthread_uart.c, since
 *      CONFIG_OPENTHREAD_RCP_USB_SERIAL_JTAG=y selects the USB host
 *      connection mode), task queue (esp_openthread_task_queue.c), and
 *      radio (esp_openthread_radio.c). Of those four, only the UART
 *      workflow's process_func -- esp_openthread_uart_process() -- can
 *      return ESP_FAIL from this path (the other three either always
 *      return ESP_OK or `assert()` internally instead of returning
 *      ESP_FAIL; confirmed by reading each one's own process_func):
 *      it does exactly one `read(s_uart_fd, ...)`, and treats ANY read()
 *      errno other than EAGAIN as fatal: `return ESP_FAIL;`. USB
 *      Serial/JTAG's own VFS read path CAN return -1/EIO for the width of
 *      one usb_serial_jtag_is_connected() false negative -- a few ms
 *      around attach/enumeration/brief host-side backpressure -- even
 *      though the link is otherwise fine one read() call later (the SAME
 *      espressif/esp-idf#18490/#18506 class of false negative already
 *      documented for the USB Spinel TX path, see rcp_uart_tx_retry.h and
 *      docs/otbr-rcp.md section 9 -- but here on the RX side). This EIO
 *      path is a plausible producer of the ESP_FAIL step 4 needs, not
 *      something the coredump itself shows happening.
 *   4. Back in esp_openthread_launch_mainloop(): `error != ESP_OK` breaks
 *      the mainloop's own `while` loop, and esp_openthread_launch_mainloop()
 *      returns that error.
 *   5. Back in esp_openthread.cpp:ot_task_worker(), the return value is
 *      discarded, but since this project builds CONFIG_OPENTHREAD_RADIO=y
 *      (RCP), the code right after the call is:
 *          ESP_LOGE(OT_PLAT_LOG_TAG, "RCP deinitialization is not supported for now");
 *          assert(false);
 *      -- i.e. this project's RCP build NEVER expects the mainloop to
 *      exit for any reason. Steps 4-5 are exactly what the coredump shows;
 *      step 3 is why this file specifically targets ESP_FAIL+EIO rather
 *      than absorbing every possible non-ESP_OK return.
 *
 * This file absorbs the one specific condition identified above (ESP_FAIL
 * from esp_openthread_platform_process() with errno == EIO) with a
 * bounded retry, so esp_openthread_launch_mainloop() never sees a
 * non-ESP_OK return for that condition and its `while` loop simply keeps
 * running -- exactly as if the transient read() had never happened. Any
 * OTHER error is passed through unmodified (see
 * __wrap_esp_openthread_platform_process()'s own comment below), so this
 * fix narrows rather than widens what can reach the diagnostic
 * assert()/coredump path. See rcp_usb_rx_recovery.h's own comment for why
 * this is a SEPARATE fix from the existing USB Spinel TX retry hardening
 * (rcp_uart_tx_retry.c et al.) and does not claim to be a fix for that
 * TX-side RESET_FAULT symptom -- the two were applied and verified
 * TOGETHER on real hardware (docs/otbr-rcp.md section 12.6), which does
 * not by itself isolate either fix's own individual contribution.
 *
 * ESP-IDF's own documented `--wrap` linker mechanism
 * ($IDF_PATH/examples/build_system/wrappers/README.md, "Using wrapper to
 * redefine IDF functions") is used again here, exactly like the TX fix --
 * see rcp/main/CMakeLists.txt's own comment for the
 * `-Wl,--wrap=esp_openthread_platform_process` link flag this relies on.
 * The real ESP-IDF install under ~/.espressif is never modified.
 *
 * Written with zero ESP-IDF VFS/USB-Serial-JTAG/OpenThread-instance
 * dependency (only FreeRTOS + esp_timer + <errno.h>, plus the opaque
 * pointer typedefs in rcp_usb_rx_recovery.h) -- exactly like
 * rcp_uart_tx_retry.c -- so this file links completely unmodified into a
 * host test (tools/test_rcp_usb_rx_recovery.c) against a scripted fake
 * __real_esp_openthread_platform_process() (tools/host_test_stubs/
 * host_rcp_usb_rx_sim.c) instead of the real openthread component.
 */
#include "rcp_usb_rx_recovery.h"

#include <errno.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static inline uint32_t retry_yield_ticks(void)
{
    TickType_t ticks = pdMS_TO_TICKS(RCP_USB_RX_RECOVERY_RETRY_YIELD_MS);
    return ticks > 0 ? ticks : 1; /* never zero -- always a real yield, never a busy spin */
}

esp_err_t __wrap_esp_openthread_platform_process(otInstance *instance,
                                                  const esp_openthread_mainloop_context_t *mainloop)
{
    int64_t deadline_us = esp_timer_get_time() + (int64_t)RCP_USB_RX_RECOVERY_TOTAL_TIMEOUT_MS * 1000;

    for (;;) {
        errno = 0; /* only errno == EIO immediately after the real call below counts as this file's transient condition */
        esp_err_t err = __real_esp_openthread_platform_process(instance, mainloop);

        if (err == ESP_OK) {
            return ESP_OK;
        }

        if (err != ESP_FAIL || errno != EIO) {
            /*
             * Not the one ESP_FAIL+EIO condition this file treats as
             * transient USB RX noise -- return the ORIGINAL error,
             * completely unmodified, on this very first attempt. Hiding
             * this would defeat the diagnostic assert()/coredump path a
             * genuine, non-transient failure is supposed to reach (this
             * file's own header comment, step 5) -- never suppress or
             * retry anything else.
             */
            return err;
        }

        if (esp_timer_get_time() >= deadline_us) {
            /*
             * EIO persisted for the whole budget -- not a momentary blip
             * any more. Silent, noreturn controlled restart; never logs
             * (see rcp_usb_rx_recovery_restart()'s own comment).
             */
            rcp_usb_rx_recovery_restart();
        }

        vTaskDelay(retry_yield_ticks()); /* bounded real yield -- never a busy spin */
    }
}
