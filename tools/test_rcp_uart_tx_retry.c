/*
 * Host-compiled test for rcp/main/rcp_uart_tx_retry.c -- a candidate
 * workaround, pending a real-hardware soak test, for the RESET_FAULT chain
 * described in docs/otbr-rcp.md's "USB Spinel TX retry" section (Power
 * Base -> SHT45 INTERMEDIATE -> PMS5003 LEAF, 4 RESET_FAULTs observed in 25
 * minutes; leading hypothesis: ESP-IDF v6.1's stock otPlatUartSend()
 * calling write() exactly once and unconditionally calling
 * otPlatUartSendDone() regardless of the result -- espressif/esp-idf#18490,
 * #18506).
 *
 * Links the REAL, unmodified rcp/main/rcp_uart_tx_retry.c against a
 * scripted fake I/O seam + recovery hook
 * (tools/host_test_stubs/host_rcp_uart_tx_sim.c) instead of the real
 * "/dev/usbserjtag" VFS device (rcp/main/rcp_uart_tx_io.c, which is
 * ESP-IDF/USB-Serial-JTAG-specific and therefore not itself host-buildable)
 * -- exactly the same "production .c compiled against a host stub seam"
 * pattern this repository already uses for driver/uart.h's
 * host_uart_sim.c stand-in.
 *
 * Covers every scenario from the correction pass:
 *   1. First write succeeds immediately (full write/flush order).
 *   2. Several transient EIO failures, then success (write/flush order
 *      across every failed attempt too).
 *   3. A transient EAGAIN failure, then success.
 *   4. Several partial writes in a row, summing to the full frame
 *      (write/flush order across partial-write boundaries).
 *   5. A 0-byte write, then success.
 *   6. A write landing exactly on, and split across, a 64-byte USB packet
 *      boundary -- still flushed the same way as any other write.
 *   7. A non-transient (hard) write error: the recovery hook fires, this
 *      function never returns to its caller, otPlatUartSendDone() is never
 *      called, and a next-frame attempt through the same (now-halted)
 *      session also cannot succeed.
 *   8. Permanent connection loss via repeated transient-looking EIO that
 *      never stops: the recovery hook fires only after
 *      RCP_UART_TX_TOTAL_TIMEOUT_MS is exhausted, never sooner and never
 *      later than a bounded multiple of it.
 *
 * Every scenario checks the exact byte sequence "sent" and the exact
 * ordered write()/flush() call log (proving rcp_uart_tx_flush() -- which
 * preserves stock otPlatUartSend()'s own unconditional post-write
 * usb_serial_jtag_ll_txfifo_flush(), "DIG-727" -- is called immediately
 * after EVERY write() attempt, success or not, exactly like the original).
 * Scenarios 7-8 use sigsetjmp()/siglongjmp() to prove
 * rcp_uart_tx_retry_send() really never returns control to its caller on a
 * fatal failure (production: a silent, noreturn esp_restart() instead) --
 * not merely that a "fake recovery hook" function got called while normal
 * control flow continued regardless.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs -I rcp/main \
 *      -o /tmp/test_rcp_uart_tx_retry tools/test_rcp_uart_tx_retry.c \
 *      rcp/main/rcp_uart_tx_retry.c tools/host_test_stubs/host_rcp_uart_tx_sim.c \
 *   && /tmp/test_rcp_uart_tx_retry
 */
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "host_rcp_uart_tx_sim.h"
#include "rcp_uart_tx_retry.h"

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

#define CHECK_WRITE_EVENT(idx, result, msg)                                                                          \
    CHECK((idx) < host_rcp_tx_sim_event_count() &&                                                                   \
              host_rcp_tx_sim_event_kind(idx) == HOST_RCP_TX_EVENT_WRITE &&                                          \
              host_rcp_tx_sim_event_write_result(idx) == (result),                                                   \
          msg)

#define CHECK_FLUSH_EVENT(idx, msg)                                                                                   \
    CHECK((idx) < host_rcp_tx_sim_event_count() && host_rcp_tx_sim_event_kind(idx) == HOST_RCP_TX_EVENT_FLUSH, msg)

static const uint8_t FRAME[] = {0x7E, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x7E};
#define FRAME_LEN ((uint16_t)sizeof(FRAME))

static bool captured_matches(const uint8_t *expected, size_t expected_len)
{
    return host_rcp_tx_sim_captured_len() == expected_len &&
           memcmp(host_rcp_tx_sim_captured_bytes(), expected, expected_len) == 0;
}

/* --- Scenario 1: first write succeeds immediately. --- */
static void scenario_first_write_succeeds(void)
{
    printf("\n-- scenario: first write succeeds --\n");
    host_rcp_tx_sim_reset();

    otError err = rcp_uart_tx_retry_send(FRAME, FRAME_LEN);

    CHECK(err == OT_ERROR_NONE, "returns OT_ERROR_NONE");
    CHECK(host_rcp_tx_sim_write_call_count() == 1, "exactly 1 write() call");
    CHECK(host_rcp_tx_sim_flush_call_count() == 1, "exactly 1 flush() call");
    CHECK(host_rcp_tx_sim_open_call_count() == 1, "exactly 1 open() call (first-ever send)");
    CHECK(host_rcp_tx_sim_send_done_call_count() == 1, "exactly 1 otPlatUartSendDone() call");
    CHECK(captured_matches(FRAME, FRAME_LEN), "full frame bytes captured in order");
    CHECK(host_rcp_tx_sim_event_count() == 2, "exactly 2 events: write, flush");
    CHECK_WRITE_EVENT(0, (ssize_t)FRAME_LEN, "event 0: write(10) succeeds");
    CHECK_FLUSH_EVENT(1, "event 1: flush immediately follows the write");
    CHECK(host_rcp_tx_sim_elapsed_ms() < 50.0, "no retry delay incurred (no failures)");
}

/* --- Scenario 2: several transient EIO failures, then success. --- */
static void scenario_eio_then_success(void)
{
    printf("\n-- scenario: EIO x3 then success --\n");
    host_rcp_tx_sim_reset();
    host_rcp_tx_step_t steps[] = {
        {HOST_RCP_TX_STEP_ERROR, 0, EIO},
        {HOST_RCP_TX_STEP_ERROR, 0, EIO},
        {HOST_RCP_TX_STEP_ERROR, 0, EIO},
        {HOST_RCP_TX_STEP_OK, HOST_RCP_TX_ACCEPT_ALL, 0},
    };
    host_rcp_tx_sim_set_script(steps, sizeof(steps) / sizeof(steps[0]));

    otError err = rcp_uart_tx_retry_send(FRAME, FRAME_LEN);

    CHECK(err == OT_ERROR_NONE, "returns OT_ERROR_NONE");
    CHECK(host_rcp_tx_sim_write_call_count() == 4, "exactly 4 write() calls (3 failed + 1 succeeded)");
    CHECK(host_rcp_tx_sim_flush_call_count() == 4, "exactly 4 flush() calls -- one after EVERY write attempt");
    CHECK(host_rcp_tx_sim_send_done_call_count() == 1, "exactly 1 otPlatUartSendDone() call");
    CHECK(captured_matches(FRAME, FRAME_LEN), "full frame bytes captured in order (nothing sent during EIO attempts)");
    CHECK(host_rcp_tx_sim_event_count() == 8, "exactly 8 events: (write, flush) x4");
    CHECK_WRITE_EVENT(0, -1, "event 0: write fails (EIO)");
    CHECK_FLUSH_EVENT(1, "event 1: flush still follows the FAILED write");
    CHECK_WRITE_EVENT(2, -1, "event 2: write fails (EIO)");
    CHECK_FLUSH_EVENT(3, "event 3: flush follows");
    CHECK_WRITE_EVENT(4, -1, "event 4: write fails (EIO)");
    CHECK_FLUSH_EVENT(5, "event 5: flush follows");
    CHECK_WRITE_EVENT(6, (ssize_t)FRAME_LEN, "event 6: write finally succeeds");
    CHECK_FLUSH_EVENT(7, "event 7: flush follows the successful write");
    /* 3 transient failures -> 3 real retry sleeps of RCP_UART_TX_RETRY_YIELD_MS each. */
    double expect_min_ms = 3.0 * (double)RCP_UART_TX_RETRY_YIELD_MS * 0.5; /* generous scheduler-jitter margin */
    CHECK(host_rcp_tx_sim_elapsed_ms() >= expect_min_ms, "retries actually slept (not a busy spin)");
    CHECK(host_rcp_tx_sim_elapsed_ms() < 1000.0, "did not wait anywhere near the full timeout budget");
}

/* --- Scenario 3: a transient EAGAIN failure, then success. --- */
static void scenario_eagain_then_success(void)
{
    printf("\n-- scenario: EAGAIN then success --\n");
    host_rcp_tx_sim_reset();
    host_rcp_tx_step_t steps[] = {
        {HOST_RCP_TX_STEP_ERROR, 0, EAGAIN},
        {HOST_RCP_TX_STEP_OK, HOST_RCP_TX_ACCEPT_ALL, 0},
    };
    host_rcp_tx_sim_set_script(steps, sizeof(steps) / sizeof(steps[0]));

    otError err = rcp_uart_tx_retry_send(FRAME, FRAME_LEN);

    CHECK(err == OT_ERROR_NONE, "returns OT_ERROR_NONE");
    CHECK(host_rcp_tx_sim_write_call_count() == 2, "exactly 2 write() calls");
    CHECK(host_rcp_tx_sim_flush_call_count() == 2, "exactly 2 flush() calls");
    CHECK(host_rcp_tx_sim_send_done_call_count() == 1, "exactly 1 otPlatUartSendDone() call");
    CHECK(captured_matches(FRAME, FRAME_LEN), "full frame bytes captured in order");
    CHECK(host_rcp_tx_sim_event_count() == 4, "exactly 4 events: (write, flush) x2");
    CHECK_WRITE_EVENT(0, -1, "event 0: write fails (EAGAIN)");
    CHECK_FLUSH_EVENT(1, "event 1: flush follows");
    CHECK_WRITE_EVENT(2, (ssize_t)FRAME_LEN, "event 2: write succeeds");
    CHECK_FLUSH_EVENT(3, "event 3: flush follows");
}

/* --- Scenario 4: several partial writes in a row. --- */
static void scenario_partial_writes(void)
{
    printf("\n-- scenario: partial writes x3 --\n");
    host_rcp_tx_sim_reset();
    /* 10-byte frame, accepted 3 + 4 + 3 bytes across 3 calls. */
    host_rcp_tx_step_t steps[] = {
        {HOST_RCP_TX_STEP_OK, 3, 0},
        {HOST_RCP_TX_STEP_OK, 4, 0},
        {HOST_RCP_TX_STEP_OK, 3, 0},
    };
    host_rcp_tx_sim_set_script(steps, sizeof(steps) / sizeof(steps[0]));

    otError err = rcp_uart_tx_retry_send(FRAME, FRAME_LEN);

    CHECK(err == OT_ERROR_NONE, "returns OT_ERROR_NONE");
    CHECK(host_rcp_tx_sim_write_call_count() == 3, "exactly 3 write() calls (3+4+3 bytes)");
    CHECK(host_rcp_tx_sim_flush_call_count() == 3, "exactly 3 flush() calls");
    CHECK(host_rcp_tx_sim_send_done_call_count() == 1, "exactly 1 otPlatUartSendDone() call");
    CHECK(captured_matches(FRAME, FRAME_LEN), "full frame bytes captured in the correct order across partial-write boundaries");
    CHECK(host_rcp_tx_sim_event_count() == 6, "exactly 6 events: (write, flush) x3");
    CHECK_WRITE_EVENT(0, 3, "event 0: write accepts 3 bytes");
    CHECK_FLUSH_EVENT(1, "event 1: flush follows the partial write");
    CHECK_WRITE_EVENT(2, 4, "event 2: write accepts 4 more bytes");
    CHECK_FLUSH_EVENT(3, "event 3: flush follows");
    CHECK_WRITE_EVENT(4, 3, "event 4: write accepts the final 3 bytes");
    CHECK_FLUSH_EVENT(5, "event 5: flush follows the last partial write");
    CHECK(host_rcp_tx_sim_elapsed_ms() < 50.0, "no retry delay incurred (every attempt made real progress)");
}

/* --- Scenario 5: a 0-byte write, then success. --- */
static void scenario_zero_byte_write_then_success(void)
{
    printf("\n-- scenario: 0-byte write then success --\n");
    host_rcp_tx_sim_reset();
    host_rcp_tx_step_t steps[] = {
        {HOST_RCP_TX_STEP_OK, 0, 0}, /* write() returns 0, not -1: no progress, no error */
        {HOST_RCP_TX_STEP_OK, HOST_RCP_TX_ACCEPT_ALL, 0},
    };
    host_rcp_tx_sim_set_script(steps, sizeof(steps) / sizeof(steps[0]));

    otError err = rcp_uart_tx_retry_send(FRAME, FRAME_LEN);

    CHECK(err == OT_ERROR_NONE, "returns OT_ERROR_NONE");
    CHECK(host_rcp_tx_sim_write_call_count() == 2, "exactly 2 write() calls");
    CHECK(host_rcp_tx_sim_flush_call_count() == 2, "exactly 2 flush() calls -- even the 0-byte attempt is flushed");
    CHECK(host_rcp_tx_sim_send_done_call_count() == 1, "exactly 1 otPlatUartSendDone() call");
    CHECK(captured_matches(FRAME, FRAME_LEN), "full frame bytes captured in order");
    CHECK(host_rcp_tx_sim_event_count() == 4, "exactly 4 events: (write, flush) x2");
    CHECK_WRITE_EVENT(0, 0, "event 0: write accepts 0 bytes");
    CHECK_FLUSH_EVENT(1, "event 1: flush follows the 0-byte write");
    CHECK_WRITE_EVENT(2, (ssize_t)FRAME_LEN, "event 2: write succeeds");
    CHECK_FLUSH_EVENT(3, "event 3: flush follows");
    CHECK(host_rcp_tx_sim_elapsed_ms() >= 0.5, "the 0-byte attempt actually incurred a retry sleep");
}

/* --- Scenario 6: a write landing exactly on, and split across, a 64-byte
 * USB packet boundary -- still flushed the same way as any other write
 * (usb_serial_jtag_ll_txfifo_flush() has no special-case for packet size;
 * this scenario exists to make that explicit, since a forgotten flush
 * right at this boundary is exactly the kind of trailing-partial-USB-packet
 * stall the original DIG-727 flush call exists to prevent). --- */
static void scenario_64_byte_boundary(void)
{
    printf("\n-- scenario: write split across a 64-byte USB packet boundary --\n");
    host_rcp_tx_sim_reset();

    uint8_t buf[100];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)i;
    }
    host_rcp_tx_step_t steps[] = {
        {HOST_RCP_TX_STEP_OK, 64, 0}, /* exactly one USB full-speed packet */
        {HOST_RCP_TX_STEP_OK, HOST_RCP_TX_ACCEPT_ALL, 0}, /* remaining 36 bytes */
    };
    host_rcp_tx_sim_set_script(steps, sizeof(steps) / sizeof(steps[0]));

    otError err = rcp_uart_tx_retry_send(buf, (uint16_t)sizeof(buf));

    CHECK(err == OT_ERROR_NONE, "returns OT_ERROR_NONE");
    CHECK(host_rcp_tx_sim_write_call_count() == 2, "exactly 2 write() calls (64 + 36 bytes)");
    CHECK(host_rcp_tx_sim_flush_call_count() == 2, "exactly 2 flush() calls");
    CHECK(host_rcp_tx_sim_send_done_call_count() == 1, "exactly 1 otPlatUartSendDone() call");
    CHECK(captured_matches(buf, sizeof(buf)), "full 100-byte buffer captured in order across the 64-byte boundary");
    CHECK(host_rcp_tx_sim_event_count() == 4, "exactly 4 events: (write, flush) x2");
    CHECK_WRITE_EVENT(0, 64, "event 0: write accepts exactly 64 bytes (one full USB packet)");
    CHECK_FLUSH_EVENT(1, "event 1: flush immediately follows the 64-byte write -- pushes the full packet without waiting");
    CHECK_WRITE_EVENT(2, 36, "event 2: write accepts the remaining 36 bytes");
    CHECK_FLUSH_EVENT(3, "event 3: flush follows the trailing short packet");
}

/* --- Scenario 7: a non-transient (hard) write error fires the recovery
 * hook. rcp_uart_tx_retry_send() must never return to its caller (proven
 * here via sigsetjmp()/siglongjmp(), not just a fake-hook call count), must
 * never call otPlatUartSendDone(), and a next-frame attempt through the
 * same now-halted session must also be unable to succeed. --- */
static void scenario_recovery_on_hard_error(void)
{
    printf("\n-- scenario: recovery hook on non-transient error (never returns to caller) --\n");
    host_rcp_tx_sim_reset();
    host_rcp_tx_sim_set_default_step((host_rcp_tx_step_t){HOST_RCP_TX_STEP_ERROR, 0, ENODEV});

    sigjmp_buf checkpoint;
    host_rcp_tx_sim_set_recovery_checkpoint(&checkpoint);

    if (sigsetjmp(checkpoint, 1) == 0) {
        rcp_uart_tx_retry_send(FRAME, FRAME_LEN);
        CHECK(false, "rcp_uart_tx_retry_send() must never return on a non-transient error");
        return;
    }

    /* Landed here via siglongjmp() from the fake recovery hook -- the call
     * above genuinely never returned. */
    CHECK(host_rcp_tx_sim_recovery_call_count() == 1, "recovery hook called exactly once");
    CHECK(host_rcp_tx_sim_send_done_call_count() == 0, "otPlatUartSendDone() never called");
    CHECK(host_rcp_tx_sim_write_call_count() == 1, "exactly 1 write() call -- no retry on a non-transient error");
    CHECK(host_rcp_tx_sim_flush_call_count() == 1, "the single failed write is still flushed, matching the original");
    CHECK(host_rcp_tx_sim_captured_len() == 0, "no bytes were ever accepted (never mistaken for a partial success)");
    CHECK(host_rcp_tx_sim_elapsed_ms() < 50.0, "bailed out immediately, did not wait out the retry budget");

    /* "다음 프레임 전송 불가": a next-frame attempt through the same
     * (now-halted) session must ALSO end up back at the recovery hook,
     * never at a successful send. */
    sigjmp_buf checkpoint2;
    host_rcp_tx_sim_set_recovery_checkpoint(&checkpoint2);
    if (sigsetjmp(checkpoint2, 1) == 0) {
        rcp_uart_tx_retry_send(FRAME, FRAME_LEN);
        CHECK(false, "a next-frame attempt after recovery must also never return normally");
        return;
    }
    CHECK(host_rcp_tx_sim_recovery_call_count() == 2, "recovery hook fires again on the next-frame attempt");
    CHECK(host_rcp_tx_sim_send_done_call_count() == 0, "still never reached SendDone -- no next frame was ever sent");
}

/* --- Scenario 8: permanent connection loss (transient-looking failures
 * that never stop) -- exercises the RCP_UART_TX_TOTAL_TIMEOUT_MS bound
 * itself, proving there is no infinite retry before recovery fires. --- */
static void scenario_recovery_on_timeout(void)
{
    printf("\n-- scenario: recovery hook on timeout exhaustion (repeated transient EIO) --\n");
    host_rcp_tx_sim_reset();
    host_rcp_tx_sim_set_default_step((host_rcp_tx_step_t){HOST_RCP_TX_STEP_ERROR, 0, EIO});

    sigjmp_buf checkpoint;
    host_rcp_tx_sim_set_recovery_checkpoint(&checkpoint);

    if (sigsetjmp(checkpoint, 1) == 0) {
        rcp_uart_tx_retry_send(FRAME, FRAME_LEN);
        CHECK(false, "rcp_uart_tx_retry_send() must never return on timeout exhaustion");
        return;
    }

    CHECK(host_rcp_tx_sim_recovery_call_count() == 1, "recovery hook called exactly once");
    CHECK(host_rcp_tx_sim_send_done_call_count() == 0, "otPlatUartSendDone() never called");
    CHECK(host_rcp_tx_sim_captured_len() == 0, "no bytes were ever accepted");
    CHECK(host_rcp_tx_sim_write_call_count() == host_rcp_tx_sim_flush_call_count(),
          "every write attempt during the retry storm was flushed exactly once");
    double elapsed = host_rcp_tx_sim_elapsed_ms();
    printf("    (elapsed %.1f ms, %zu write() attempts)\n", elapsed, host_rcp_tx_sim_write_call_count());
    CHECK(elapsed >= (double)RCP_UART_TX_TOTAL_TIMEOUT_MS * 0.95, "waited out (at least) the full retry budget before recovering");
    CHECK(elapsed < (double)RCP_UART_TX_TOTAL_TIMEOUT_MS * 3.0,
          "recovered within a bounded multiple of the budget -- never an infinite retry");
}

int main(void)
{
    scenario_first_write_succeeds();
    scenario_eio_then_success();
    scenario_eagain_then_success();
    scenario_partial_writes();
    scenario_zero_byte_write_then_success();
    scenario_64_byte_boundary();
    scenario_recovery_on_hard_error();
    scenario_recovery_on_timeout();

    if (g_failures == 0) {
        printf("\nALL CHECKS PASSED\n");
        return 0;
    }
    printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
