/*
 * Host-compiled test for rcp/main/esp_openthread_platform_process_wrap.c --
 * the RCP USB RX recovery fix for a real-hardware coredump analysis's
 * CONFIRMED root cause: crashed task `ot_main`, assert address 0x4200f9ee,
 * `assert(false)` at esp_openthread.cpp:283 right after
 * esp_openthread.cpp:279's esp_openthread_launch_mainloop() returned (see
 * rcp/main/rcp_usb_rx_recovery.h and esp_openthread_platform_process_wrap.c's
 * own header comments for the full call-chain writeup).
 *
 * Links the REAL, unmodified rcp/main/esp_openthread_platform_process_wrap.c
 * (the actual production `--wrap=esp_openthread_platform_process` wrapper,
 * including its own errno-reset/retry/timeout logic -- there is no
 * separate "retry engine" file to link instead here, unlike the TX fix)
 * against a scripted fake __real_esp_openthread_platform_process() +
 * recovery hook (tools/host_test_stubs/host_rcp_usb_rx_sim.c), exactly like
 * this repository's other host tests link real component/RCP `.c` files
 * against fakes under tools/host_test_stubs/.
 *
 * Covers:
 *   1. OK 통과: __real_esp_openthread_platform_process() returns ESP_OK
 *      immediately -- no retry, no delay, ESP_OK returned.
 *   2. 일시적 EIO 후 복구: a few ESP_FAIL/EIO attempts, then ESP_OK --
 *      __wrap_esp_openthread_platform_process() itself returns ESP_OK
 *      (the mainloop never sees the transient failure), and errno was
 *      reset to 0 before EVERY real call, not just the first.
 *   3. 반복 EIO bounded delay (busy spin 없음): several ESP_FAIL/EIO
 *      attempts before eventual success -- real wall-clock time actually
 *      elapses (vTaskDelay() really sleeps), proving this is a bounded,
 *      yielding retry loop, never a tight busy spin.
 *   4. 200ms 소진 시 noreturn recovery: EIO that never clears --
 *      rcp_usb_rx_recovery_restart() fires only after
 *      RCP_USB_RX_RECOVERY_TOTAL_TIMEOUT_MS has actually elapsed (never
 *      sooner, never much later), and
 *      __wrap_esp_openthread_platform_process() never returns to its
 *      caller (proven via sigsetjmp()/siglongjmp(), not just a call
 *      count).
 *   5. non-EIO 보존: an ESP_FAIL with a DIFFERENT errno (ENODEV) is
 *      returned completely unmodified on the very first attempt -- no
 *      retry, no delay, no recovery hook call.
 *   6. non-EIO 보존 (다른 esp_err_t): a non-ESP_FAIL esp_err_t (e.g.
 *      ESP_ERR_INVALID_STATE) is ALSO returned completely unmodified, even
 *      if errno happens to be EIO -- only the exact ESP_FAIL+EIO
 *      combination is ever treated as transient.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs -I rcp/main \
 *      -o /tmp/test_rcp_usb_rx_recovery tools/test_rcp_usb_rx_recovery.c \
 *      rcp/main/esp_openthread_platform_process_wrap.c \
 *      tools/host_test_stubs/host_rcp_usb_rx_sim.c \
 *   && /tmp/test_rcp_usb_rx_recovery
 */
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>

#include "host_rcp_usb_rx_sim.h"
#include "rcp_usb_rx_recovery.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                                   \
            g_failures++;                                                                                            \
        } else {                                                                                                     \
            printf("ok:   %s\n", msg);                                                                               \
        }                                                                                                            \
    } while (0)

static bool all_errno_at_entry_zero(size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (host_rcp_rx_sim_errno_at_entry(i) != 0) {
            return false;
        }
    }
    return true;
}

/* --- Scenario 1: OK 통과 (immediate success). --- */
static void scenario_ok_passthrough(void)
{
    printf("\n-- scenario: OK passthrough --\n");
    host_rcp_rx_sim_reset();

    esp_err_t err = __wrap_esp_openthread_platform_process(NULL, NULL);

    CHECK(err == ESP_OK, "returns ESP_OK");
    CHECK(host_rcp_rx_sim_real_call_count() == 1, "exactly 1 real call");
    CHECK(host_rcp_rx_sim_recovery_call_count() == 0, "recovery hook never fired");
    CHECK(all_errno_at_entry_zero(host_rcp_rx_sim_real_call_count()), "errno was 0 at the real call's entry");
    CHECK(host_rcp_rx_sim_elapsed_ms() < 50.0, "no retry delay incurred (no failures)");
}

/* --- Scenario 2: 일시적 EIO 후 복구 (a few transient EIO, then success). --- */
static void scenario_transient_eio_then_recovery(void)
{
    printf("\n-- scenario: transient EIO x3 then recovery --\n");
    host_rcp_rx_sim_reset();
    host_rcp_rx_step_t steps[] = {
        {HOST_RCP_RX_STEP_FAIL, ESP_FAIL, EIO},
        {HOST_RCP_RX_STEP_FAIL, ESP_FAIL, EIO},
        {HOST_RCP_RX_STEP_FAIL, ESP_FAIL, EIO},
        {HOST_RCP_RX_STEP_OK, ESP_OK, 0},
    };
    host_rcp_rx_sim_set_script(steps, sizeof(steps) / sizeof(steps[0]));

    esp_err_t err = __wrap_esp_openthread_platform_process(NULL, NULL);

    CHECK(err == ESP_OK, "__wrap_esp_openthread_platform_process() itself returns ESP_OK "
                          "(the mainloop's own while-loop never sees the transient failure)");
    CHECK(host_rcp_rx_sim_real_call_count() == 4, "exactly 4 real calls (3 failed + 1 succeeded)");
    CHECK(host_rcp_rx_sim_recovery_call_count() == 0, "recovery hook never fired");
    CHECK(all_errno_at_entry_zero(host_rcp_rx_sim_real_call_count()),
          "errno was reset to 0 before EVERY real call, including every retry -- not just the first");
    /* 3 transient failures -> 3 real retry sleeps of RCP_USB_RX_RECOVERY_RETRY_YIELD_MS each. */
    double expect_min_ms = 3.0 * (double)RCP_USB_RX_RECOVERY_RETRY_YIELD_MS * 0.5; /* generous scheduler-jitter margin */
    CHECK(host_rcp_rx_sim_elapsed_ms() >= expect_min_ms, "retries actually slept (not a busy spin)");
    CHECK(host_rcp_rx_sim_elapsed_ms() < 1000.0, "did not wait anywhere near the full timeout budget");
}

/* --- Scenario 3: 반복 EIO bounded delay / busy spin 없음 (more transient EIO
 * attempts before eventual success, still well inside the budget). --- */
static void scenario_repeated_eio_bounded_delay(void)
{
    printf("\n-- scenario: repeated EIO (bounded, yielding delay -- no busy spin) --\n");
    host_rcp_rx_sim_reset();
    host_rcp_rx_step_t steps[20];
    for (size_t i = 0; i < 19; i++) {
        steps[i] = (host_rcp_rx_step_t){HOST_RCP_RX_STEP_FAIL, ESP_FAIL, EIO};
    }
    steps[19] = (host_rcp_rx_step_t){HOST_RCP_RX_STEP_OK, ESP_OK, 0};
    host_rcp_rx_sim_set_script(steps, sizeof(steps) / sizeof(steps[0]));

    esp_err_t err = __wrap_esp_openthread_platform_process(NULL, NULL);

    CHECK(err == ESP_OK, "returns ESP_OK once the real call finally succeeds");
    CHECK(host_rcp_rx_sim_real_call_count() == 20, "exactly 20 real calls (19 failed + 1 succeeded)");
    CHECK(host_rcp_rx_sim_recovery_call_count() == 0,
          "recovery hook never fired -- 19 * 2ms is well within the 200ms budget");
    double expect_min_ms = 19.0 * (double)RCP_USB_RX_RECOVERY_RETRY_YIELD_MS * 0.5;
    CHECK(host_rcp_rx_sim_elapsed_ms() >= expect_min_ms,
          "19 retries actually slept roughly 19 * RCP_USB_RX_RECOVERY_RETRY_YIELD_MS of real wall-clock time "
          "(a busy spin would instead finish in near-zero time)");
    CHECK(host_rcp_rx_sim_elapsed_ms() < (double)RCP_USB_RX_RECOVERY_TOTAL_TIMEOUT_MS,
          "stayed comfortably under the 200ms total budget");
}

/* --- Scenario 4: 200ms 소진 시 noreturn recovery (EIO that never clears). --- */
static void scenario_timeout_exhaustion_recovery(void)
{
    printf("\n-- scenario: recovery hook on 200ms timeout exhaustion (never returns to caller) --\n");
    host_rcp_rx_sim_reset();
    host_rcp_rx_sim_set_default_step((host_rcp_rx_step_t){HOST_RCP_RX_STEP_FAIL, ESP_FAIL, EIO});

    sigjmp_buf checkpoint;
    host_rcp_rx_sim_set_recovery_checkpoint(&checkpoint);

    if (sigsetjmp(checkpoint, 1) == 0) {
        __wrap_esp_openthread_platform_process(NULL, NULL);
        CHECK(false, "__wrap_esp_openthread_platform_process() must never return on timeout exhaustion");
        return;
    }

    /* Landed here via siglongjmp() from the fake recovery hook -- the call
     * above genuinely never returned. */
    CHECK(host_rcp_rx_sim_recovery_call_count() == 1, "recovery hook called exactly once");
    CHECK(all_errno_at_entry_zero(host_rcp_rx_sim_real_call_count()),
          "errno was reset to 0 before every real call throughout the whole retry storm");
    double elapsed = host_rcp_rx_sim_elapsed_ms();
    printf("    (elapsed %.1f ms, %zu real calls)\n", elapsed, host_rcp_rx_sim_real_call_count());
    CHECK(elapsed >= (double)RCP_USB_RX_RECOVERY_TOTAL_TIMEOUT_MS * 0.95,
          "waited out (at least) the full 200ms retry budget before recovering");
    CHECK(elapsed < (double)RCP_USB_RX_RECOVERY_TOTAL_TIMEOUT_MS * 3.0,
          "recovered within a bounded multiple of the budget -- never an infinite retry");

    /* A next-attempt call through the same (now-halted) session must ALSO
     * end up back at the recovery hook, never at a successful process(). */
    sigjmp_buf checkpoint2;
    host_rcp_rx_sim_set_recovery_checkpoint(&checkpoint2);
    if (sigsetjmp(checkpoint2, 1) == 0) {
        __wrap_esp_openthread_platform_process(NULL, NULL);
        CHECK(false, "a next-attempt call after recovery must also never return normally");
        return;
    }
    CHECK(host_rcp_rx_sim_recovery_call_count() == 2, "recovery hook fires again on the next-attempt call");
}

/* --- Scenario 5: non-EIO 보존 (ESP_FAIL with a different errno). --- */
static void scenario_non_eio_errno_preserved(void)
{
    printf("\n-- scenario: non-EIO errno preserved (ESP_FAIL / ENODEV) --\n");
    host_rcp_rx_sim_reset();
    host_rcp_rx_sim_set_default_step((host_rcp_rx_step_t){HOST_RCP_RX_STEP_FAIL, ESP_FAIL, ENODEV});

    esp_err_t err = __wrap_esp_openthread_platform_process(NULL, NULL);

    CHECK(err == ESP_FAIL, "the ORIGINAL error (ESP_FAIL) is returned completely unmodified");
    CHECK(host_rcp_rx_sim_real_call_count() == 1,
          "exactly 1 real call -- no retry for a non-EIO errno, "
          "preserving the diagnostic assert()/coredump path");
    CHECK(host_rcp_rx_sim_recovery_call_count() == 0, "recovery hook never fired -- this is not a bounded-retry case");
    CHECK(host_rcp_rx_sim_elapsed_ms() < 50.0, "bailed out immediately, no retry delay incurred");
}

/* --- Scenario 6: non-EIO 보존 (a non-ESP_FAIL esp_err_t, even with errno == EIO). --- */
static void scenario_non_esp_fail_error_preserved(void)
{
    printf("\n-- scenario: non-ESP_FAIL error code preserved even with errno == EIO --\n");
    host_rcp_rx_sim_reset();
    host_rcp_rx_sim_set_default_step((host_rcp_rx_step_t){HOST_RCP_RX_STEP_FAIL, ESP_ERR_INVALID_STATE, EIO});

    esp_err_t err = __wrap_esp_openthread_platform_process(NULL, NULL);

    CHECK(err == ESP_ERR_INVALID_STATE,
          "the ORIGINAL error (ESP_ERR_INVALID_STATE) is returned unmodified -- only the exact "
          "ESP_FAIL+EIO combination is ever treated as transient, never EIO alone");
    CHECK(host_rcp_rx_sim_real_call_count() == 1, "exactly 1 real call -- no retry");
    CHECK(host_rcp_rx_sim_recovery_call_count() == 0, "recovery hook never fired");
    CHECK(host_rcp_rx_sim_elapsed_ms() < 50.0, "bailed out immediately, no retry delay incurred");
}

int main(void)
{
    scenario_ok_passthrough();
    scenario_transient_eio_then_recovery();
    scenario_repeated_eio_bounded_delay();
    scenario_timeout_exhaustion_recovery();
    scenario_non_eio_errno_preserved();
    scenario_non_esp_fail_error_preserved();

    if (g_failures == 0) {
        printf("\nALL CHECKS PASSED\n");
        return 0;
    }
    printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
