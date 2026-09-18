/*
 * Bounded-retry-or-controlled-restart recovery for a USB Serial/JTAG
 * Spinel RX error surfacing through esp_openthread_platform_process().
 *
 * Evidence tiers -- see esp_openthread_platform_process_wrap.c's own
 * header comment for the full call-chain writeup and docs/otbr-rcp.md
 * section 12 for the complete evidence-level breakdown:
 *   - CONFIRMED by a real-hardware coredump: the crashed task was
 *     `ot_main`; $IDF_PATH/components/openthread/src/esp_openthread.cpp:279's
 *     esp_openthread_launch_mainloop() returned; esp_openthread.cpp:283's
 *     `assert(false)` then fired. The coredump does NOT preserve the
 *     `errno` value from the moment of the panic, so it does not, by
 *     itself, identify which error made esp_openthread_platform_process()
 *     return non-ESP_OK.
 *   - Identified by SOURCE ANALYSIS as the plausible trigger (not directly
 *     observed in the captured coredump): among this project's four
 *     registered workflows (alarm/UART/task queue/radio -- see
 *     esp_openthread_platform_process_wrap.c), only the UART workflow's
 *     esp_openthread_uart_process() returns ESP_FAIL for a read() errno
 *     other than EAGAIN, and USB Serial/JTAG's own VFS read path can
 *     return EIO for a momentary connectivity false negative -- a
 *     plausible way for that ESP_FAIL to reach and exit
 *     esp_openthread_launch_mainloop().
 *   - VERIFIED on real hardware: a diagnostic-build RCP and a
 *     production-build RCP, each with this RX wrapper AND the existing
 *     USB Spinel TX hardening (section 9) applied TOGETHER, each ran the
 *     full Power Base -> SHT45 INTERMEDIATE -> PMS5003 LEAF chain for one
 *     continuous hour with zero RCP/OTBR resets. This is a combined-
 *     hardening result (both fixes applied at once) -- it does not
 *     isolate this RX fix's own individual contribution.
 *
 * This is a SEPARATE fix from the existing USB Spinel TX retry hardening
 * (rcp_uart_tx_retry.h/.c, rcp_uart_tx_io.c, esp_ot_uart_send_wrap.c --
 * docs/otbr-rcp.md section 9): that one wraps otPlatUartSend() (the TX
 * path) and hardens short/failed writes being silently reported as sent.
 * This file wraps esp_openthread_platform_process() (the RX path, inside
 * the mainloop's own select()-driven poll) and absorbs the RX-side
 * ESP_FAIL/EIO condition identified above. Neither file depends on,
 * restates, or substitutes for the other -- do not conflate the two, and
 * do not credit either fix alone for the combined real-hardware result.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * `otInstance` / `esp_openthread_mainloop_context_t` are forward-declared
 * opaquely here rather than pulled in via ESP-IDF's real
 * `openthread/instance.h` / `esp_openthread_types.h` (the latter alone
 * drags in esp_netif_types.h, driver/gpio.h, driver/spi_master.h,
 * driver/spi_slave.h, driver/uart.h, driver/usb_serial_jtag.h, various
 * hal/ headers, and openthread/thread.h): every function declared below
 * only ever forwards
 * these two pointers straight through to
 * __real_esp_openthread_platform_process(), never dereferencing either
 * itself, so only their ADDRESS ever needs to cross this file's boundary,
 * never their layout -- ABI-identical to the real (complete) types either
 * way. This is what keeps esp_openthread_platform_process_wrap.c
 * host-testable: tools/test_rcp_usb_rx_recovery.c links that file
 * completely unmodified against a scripted fake
 * __real_esp_openthread_platform_process() (tools/host_test_stubs/
 * host_rcp_usb_rx_sim.c) that needs no real ESP-IDF headers either.
 * `typedef struct otInstance otInstance;` matches the real
 * `openthread/instance.h`'s own forward declaration exactly (an identical
 * repeated typedef of an incomplete struct type is valid C); this file
 * never includes that header itself, so there is nothing for it to
 * conflict with.
 */
typedef struct otInstance otInstance;
typedef struct esp_openthread_mainloop_context_s esp_openthread_mainloop_context_t;

/*
 * How long a single retry attempt actually sleeps (a real vTaskDelay(),
 * never a busy spin) after __real_esp_openthread_platform_process() returns
 * ESP_FAIL with errno == EIO. rcp/sdkconfig.defaults sets
 * CONFIG_FREERTOS_HZ=1000 (1 ms/tick), so this is >= 1 real tick. Matches
 * the existing USB Spinel TX retry's own RCP_UART_TX_RETRY_YIELD_MS value
 * (rcp_uart_tx_retry.h) -- same underlying USB Serial/JTAG false-negative
 * class of transient failure, same choice of yield -- but kept as its own,
 * separately named constant (see this header's own top comment on why the
 * two fixes are not merged).
 */
#define RCP_USB_RX_RECOVERY_RETRY_YIELD_MS ((uint32_t)2u)

/*
 * Overall budget, across every retry, for ONE
 * __wrap_esp_openthread_platform_process() call to see the transient EIO
 * clear. Wide enough to absorb a few-ms USB Serial/JTAG connectivity false
 * negative with a large safety margin, but short enough to stay far under
 * any FreeRTOS task/interrupt watchdog timeout -- once this elapses with
 * the real call still returning ESP_FAIL/EIO, the USB link is treated as
 * genuinely (not just momentarily) gone and this hands off to the recovery
 * hook below rather than looping forever.
 */
#define RCP_USB_RX_RECOVERY_TOTAL_TIMEOUT_MS ((uint32_t)200u)

/*
 * Controlled restart/recovery hook: called only once
 * RCP_USB_RX_RECOVERY_TOTAL_TIMEOUT_MS has elapsed with
 * __real_esp_openthread_platform_process() still returning ESP_FAIL/EIO on
 * every attempt. NEVER RETURNS (production, rcp_usb_rx_recovery_restart.c:
 * a silent esp_restart() -- no ESP_LOGx/printf, nothing written to any
 * console/VFS fd; a host test instead links a fake
 * (tools/host_test_stubs/host_rcp_usb_rx_sim.c) that siglongjmp()s out, so
 * this contract is exercised for real rather than merely documented).
 *
 * Deliberately its own function, in its own file, rather than reusing the
 * existing rcp_uart_tx_recovery_restart() (rcp_uart_tx_io.c): the two fixes
 * address different failures on different call paths (see this header's
 * own top comment) and are kept independently auditable/removable rather
 * than silently coupled through a shared restart hook.
 */
void rcp_usb_rx_recovery_restart(void) __attribute__((noreturn));

/*
 * The real, original esp_openthread_platform_process() -- reached via
 * ESP-IDF's own documented linker `--wrap` mechanism
 * ($IDF_PATH/examples/build_system/wrappers). Never called directly by
 * anything outside esp_openthread_platform_process_wrap.c.
 */
esp_err_t __real_esp_openthread_platform_process(otInstance *instance,
                                                  const esp_openthread_mainloop_context_t *mainloop);

/*
 * Bounded-retry wrapper installed in place of every call to
 * esp_openthread_platform_process() (rcp/main/CMakeLists.txt's
 * `-Wl,--wrap=esp_openthread_platform_process`). Resets errno to 0
 * immediately before every __real_esp_openthread_platform_process() call.
 * If that call returns ESP_FAIL with errno == EIO (the one condition this
 * file treats as transient USB RX noise -- see this header's own top
 * comment for the evidence level behind that treatment): sleeps
 * RCP_USB_RX_RECOVERY_RETRY_YIELD_MS (a real, bounded yield -- never a busy
 * spin) and retries, within RCP_USB_RX_RECOVERY_TOTAL_TIMEOUT_MS overall;
 * on a successful retry, returns ESP_OK so
 * esp_openthread_launch_mainloop()'s own `while` loop keeps running exactly
 * as if nothing had happened. If EIO persists for the whole budget, calls
 * rcp_usb_rx_recovery_restart() and DOES NOT RETURN.
 *
 * Any OTHER error -- any esp_err_t other than ESP_FAIL, or ESP_FAIL with
 * any errno other than EIO -- is returned to the caller COMPLETELY
 * UNMODIFIED, on the very first attempt, with no retry and no delay: this
 * is deliberate. Hiding a genuine, non-transient failure here would defeat
 * the diagnostic assert()/coredump path a real failure is supposed to
 * reach (esp_openthread.cpp's own `assert(false)` after
 * esp_openthread_launch_mainloop() returns) -- this file only ever absorbs
 * the one specific ESP_FAIL+EIO condition it treats as transient, never
 * anything else.
 */
esp_err_t __wrap_esp_openthread_platform_process(otInstance *instance,
                                                  const esp_openthread_mainloop_context_t *mainloop);

#ifdef __cplusplus
}
#endif
