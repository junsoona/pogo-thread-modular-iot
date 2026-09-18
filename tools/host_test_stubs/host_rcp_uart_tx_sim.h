/*
 * Scripted fake for rcp/main/rcp_uart_tx_retry.h's I/O seam
 * (rcp_uart_tx_open()/rcp_uart_tx_write()/rcp_uart_tx_flush()) and its
 * controlled-restart hook (rcp_uart_tx_recovery_restart()), plus a
 * call-counting otPlatUartSendDone() stand-in -- backs
 * tools/test_rcp_uart_tx_retry.c, which links the REAL, unmodified
 * rcp/main/rcp_uart_tx_retry.c against this file exactly like
 * driver/uart.h's host_uart_sim.c backs other components' host tests.
 *
 * A test scenario loads a fixed sequence of write() outcomes with
 * host_rcp_tx_sim_set_script() (applied strictly in call order); once the
 * script is exhausted, further calls fall back to the default step
 * (host_rcp_tx_sim_set_default_step(), HOST_RCP_TX_ACCEPT_ALL by default)
 * so a scenario only needs to script its own interesting attempts.
 *
 * Production's rcp_uart_tx_recovery_restart() is declared noreturn and, in
 * the real firmware, really never returns (a silent esp_restart()). This
 * fake instead longjmp()s to whatever sigsetjmp() checkpoint the test set
 * with host_rcp_tx_sim_set_recovery_checkpoint() -- so a scenario can
 * prove "rcp_uart_tx_retry_send() never returns to its caller on a fatal
 * failure" for real (via setjmp/longjmp control flow), not merely by
 * documentation.
 */
#pragma once

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* ssize_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HOST_RCP_TX_STEP_OK,    /* rcp_uart_tx_write() returns `n` (>= 0, may be
                                less than requested, or exactly 0) */
    HOST_RCP_TX_STEP_ERROR, /* rcp_uart_tx_write() returns -1, errno = `err` */
} host_rcp_tx_step_kind_t;

typedef struct {
    host_rcp_tx_step_kind_t kind;
    size_t n;  /* HOST_RCP_TX_STEP_OK only; clamped to the requested length.
                  Pass HOST_RCP_TX_ACCEPT_ALL to always accept exactly
                  whatever length was requested. */
    int err;   /* HOST_RCP_TX_STEP_ERROR only (e.g. EIO, EAGAIN, ENODEV) */
} host_rcp_tx_step_t;

/* Sentinel for host_rcp_tx_step_t.n: accept exactly the requested length,
 * whatever it is (clamping happens the same way a literal `len` would). */
#define HOST_RCP_TX_ACCEPT_ALL ((size_t)-1)

/* One recorded write()/flush() call, in call order -- see
 * host_rcp_tx_sim_event_count()/_kind()/_write_result() below. Lets a test
 * assert the exact write-then-flush ORDER (not just counts), which is what
 * preserving stock otPlatUartSend()'s own DIG-727 flush semantics means in
 * practice. */
typedef enum {
    HOST_RCP_TX_EVENT_WRITE,
    HOST_RCP_TX_EVENT_FLUSH,
} host_rcp_tx_event_kind_t;

typedef struct {
    host_rcp_tx_event_kind_t kind;
    ssize_t write_result; /* only meaningful when kind == HOST_RCP_TX_EVENT_WRITE */
} host_rcp_tx_event_t;

/* Clears captured bytes, the event log, the step script, every call
 * counter, un-halts the device, resets the default (post-script) step back
 * to HOST_RCP_TX_ACCEPT_ALL, and restarts this sim's own elapsed-time
 * stopwatch. Call once at the start of every scenario. */
void host_rcp_tx_sim_reset(void);

/* Loads `steps[0..count)`, applied strictly in order, one per
 * rcp_uart_tx_write() call. The array is copied -- `steps` need not
 * outlive this call. Once these are exhausted, every further call uses
 * the default step (host_rcp_tx_sim_set_default_step(), or
 * HOST_RCP_TX_ACCEPT_ALL if never set). Ignored once the device is halted
 * (see rcp_uart_tx_recovery_restart()'s own comment below). */
void host_rcp_tx_sim_set_script(const host_rcp_tx_step_t *steps, size_t count);

/* Sets the step every rcp_uart_tx_write() call uses once the explicit
 * script (if any) is exhausted -- e.g. a repeating HOST_RCP_TX_STEP_ERROR
 * to simulate a connection that never comes back, without needing a
 * script array long enough to outlast RCP_UART_TX_TOTAL_TIMEOUT_MS. */
void host_rcp_tx_sim_set_default_step(host_rcp_tx_step_t step);

/* When true, rcp_uart_tx_open() always returns -1/ENODEV (the device never
 * becomes available at all). Default false (open always succeeds).
 * Auto-reset to false by host_rcp_tx_sim_reset(). */
void host_rcp_tx_sim_set_open_fails(bool fails);

size_t host_rcp_tx_sim_write_call_count(void);
size_t host_rcp_tx_sim_open_call_count(void);
size_t host_rcp_tx_sim_flush_call_count(void);
int host_rcp_tx_sim_send_done_call_count(void);
int host_rcp_tx_sim_recovery_call_count(void);

/* Every byte actually "accepted" (per the script's HOST_RCP_TX_STEP_OK `n`
 * values) across every rcp_uart_tx_write() call since the last reset, in
 * call order -- lets a test assert the full, exact byte sequence a real
 * USB Serial/JTAG peer would have seen on the wire, including across
 * partial-write boundaries. */
const uint8_t *host_rcp_tx_sim_captured_bytes(void);
size_t host_rcp_tx_sim_captured_len(void);

/* The ordered write()/flush() call log -- see host_rcp_tx_event_kind_t's
 * own comment. `index` must be < host_rcp_tx_sim_event_count(). */
size_t host_rcp_tx_sim_event_count(void);
host_rcp_tx_event_kind_t host_rcp_tx_sim_event_kind(size_t index);
/* Only meaningful for a HOST_RCP_TX_EVENT_WRITE entry: the value
 * rcp_uart_tx_write() returned for that call (>=0 accepted bytes, or -1). */
ssize_t host_rcp_tx_sim_event_write_result(size_t index);

/* Milliseconds of REAL wall-clock time (this stub's vTaskDelay() really
 * sleeps -- see tools/host_test_stubs/freertos/task.h) elapsed since the
 * last host_rcp_tx_sim_reset(). Read this right after
 * rcp_uart_tx_retry_send() returns (or after the recovery longjmp lands)
 * to prove N scripted transient failures actually cost roughly N *
 * RCP_UART_TX_RETRY_YIELD_MS of real sleeping -- a busy-spin retry loop
 * would instead complete in near-zero time. */
double host_rcp_tx_sim_elapsed_ms(void);

/*
 * Registers the sigsetjmp() checkpoint this fake's
 * rcp_uart_tx_recovery_restart() longjmp()s back to, standing in for a
 * real device reboot (control never resumes at the call site, in either
 * build). A scenario must call this (with a jmp_buf it set up via
 * `if (sigsetjmp(buf, 1) == 0) { rcp_uart_tx_retry_send(...); ... }`)
 * before any call that might hit a fatal failure. Once
 * rcp_uart_tx_recovery_restart() has fired at all, this sim also marks the
 * device "halted": every SUBSEQUENT rcp_uart_tx_open()/rcp_uart_tx_write()
 * call (i.e. a next-frame attempt made after this "reboot", exactly like
 * production's still-stale-but-now-meaningless cached fd right up until a
 * real chip actually reboots) unconditionally fails with a non-transient
 * errno -- proving a next frame cannot be sent through the same session,
 * not merely that the hook was called once.
 */
void host_rcp_tx_sim_set_recovery_checkpoint(sigjmp_buf *checkpoint);

#ifdef __cplusplus
}
#endif
