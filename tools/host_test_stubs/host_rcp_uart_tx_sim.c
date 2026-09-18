/*
 * Implementation of host_rcp_uart_tx_sim.h -- see that header's own
 * comment. Provides rcp_uart_tx_open()/rcp_uart_tx_write()/
 * rcp_uart_tx_flush()/rcp_uart_tx_recovery_restart() (the I/O seam +
 * recovery hook rcp/main/rcp_uart_tx_retry.h declares) and
 * otPlatUartSendDone() (which real production code forward-declares extern
 * and calls, and which this host build has no other definition of).
 */
#include "host_rcp_uart_tx_sim.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "rcp_uart_tx_retry.h" /* for the exact seam/hook signatures */

#define HOST_RCP_TX_SIM_MAX_STEPS 64
#define HOST_RCP_TX_SIM_MAX_CAPTURE 4096
#define HOST_RCP_TX_SIM_MAX_EVENTS 256

static host_rcp_tx_step_t s_steps[HOST_RCP_TX_SIM_MAX_STEPS];
static size_t s_step_count;
static size_t s_step_index;
static host_rcp_tx_step_t s_default_step = {HOST_RCP_TX_STEP_OK, HOST_RCP_TX_ACCEPT_ALL, 0};

static bool s_open_fails;
static size_t s_open_calls;
static size_t s_write_calls;
static size_t s_flush_calls;
static int s_send_done_calls;
static int s_recovery_calls;
static bool s_halted;

static uint8_t s_captured[HOST_RCP_TX_SIM_MAX_CAPTURE];
static size_t s_captured_len;

static host_rcp_tx_event_t s_events[HOST_RCP_TX_SIM_MAX_EVENTS];
static size_t s_event_count;

static int64_t s_reset_time_us;

static sigjmp_buf *s_recovery_checkpoint;

static void push_event(host_rcp_tx_event_kind_t kind, ssize_t write_result)
{
    if (s_event_count < HOST_RCP_TX_SIM_MAX_EVENTS) {
        s_events[s_event_count].kind = kind;
        s_events[s_event_count].write_result = write_result;
        s_event_count++;
    }
}

void host_rcp_tx_sim_reset(void)
{
    s_step_count = 0;
    s_step_index = 0;
    s_default_step = (host_rcp_tx_step_t){HOST_RCP_TX_STEP_OK, HOST_RCP_TX_ACCEPT_ALL, 0};
    s_open_fails = false;
    s_open_calls = 0;
    s_write_calls = 0;
    s_flush_calls = 0;
    s_send_done_calls = 0;
    s_recovery_calls = 0;
    s_halted = false;
    s_captured_len = 0;
    memset(s_captured, 0, sizeof(s_captured));
    s_event_count = 0;
    s_recovery_checkpoint = NULL;
    s_reset_time_us = esp_timer_get_time();
}

void host_rcp_tx_sim_set_script(const host_rcp_tx_step_t *steps, size_t count)
{
    if (count > HOST_RCP_TX_SIM_MAX_STEPS) {
        count = HOST_RCP_TX_SIM_MAX_STEPS;
    }
    memcpy(s_steps, steps, count * sizeof(*steps));
    s_step_count = count;
    s_step_index = 0;
}

void host_rcp_tx_sim_set_default_step(host_rcp_tx_step_t step)
{
    s_default_step = step;
}

void host_rcp_tx_sim_set_open_fails(bool fails)
{
    s_open_fails = fails;
}

size_t host_rcp_tx_sim_write_call_count(void)
{
    return s_write_calls;
}

size_t host_rcp_tx_sim_open_call_count(void)
{
    return s_open_calls;
}

size_t host_rcp_tx_sim_flush_call_count(void)
{
    return s_flush_calls;
}

int host_rcp_tx_sim_send_done_call_count(void)
{
    return s_send_done_calls;
}

int host_rcp_tx_sim_recovery_call_count(void)
{
    return s_recovery_calls;
}

const uint8_t *host_rcp_tx_sim_captured_bytes(void)
{
    return s_captured;
}

size_t host_rcp_tx_sim_captured_len(void)
{
    return s_captured_len;
}

size_t host_rcp_tx_sim_event_count(void)
{
    return s_event_count;
}

host_rcp_tx_event_kind_t host_rcp_tx_sim_event_kind(size_t index)
{
    return s_events[index].kind;
}

ssize_t host_rcp_tx_sim_event_write_result(size_t index)
{
    return s_events[index].write_result;
}

double host_rcp_tx_sim_elapsed_ms(void)
{
    return (double)(esp_timer_get_time() - s_reset_time_us) / 1000.0;
}

void host_rcp_tx_sim_set_recovery_checkpoint(sigjmp_buf *checkpoint)
{
    s_recovery_checkpoint = checkpoint;
}

int rcp_uart_tx_open(void)
{
    s_open_calls++;
    if (s_open_fails || s_halted) {
        errno = ENODEV;
        return -1;
    }
    return 42; /* any non-negative placeholder fd -- never actually used for real I/O */
}

ssize_t rcp_uart_tx_write(int fd, const uint8_t *buf, size_t len)
{
    (void)fd;
    s_write_calls++;

    if (s_halted) {
        /* Device already handed off to the recovery hook once -- exactly
         * like a real chip mid-reboot, nothing can be sent through this
         * session any more. A non-transient errno so the retry engine
         * bails out (to recovery again) immediately rather than retrying
         * for the full timeout budget. */
        push_event(HOST_RCP_TX_EVENT_WRITE, -1);
        errno = ENODEV;
        return -1;
    }

    host_rcp_tx_step_t step = (s_step_index < s_step_count) ? s_steps[s_step_index++] : s_default_step;

    if (step.kind == HOST_RCP_TX_STEP_ERROR) {
        push_event(HOST_RCP_TX_EVENT_WRITE, -1);
        errno = step.err;
        return -1;
    }

    size_t n = step.n;
    if (n > len) {
        n = len; /* also handles HOST_RCP_TX_ACCEPT_ALL, which is always > len */
    }
    if (s_captured_len + n > HOST_RCP_TX_SIM_MAX_CAPTURE) {
        n = HOST_RCP_TX_SIM_MAX_CAPTURE - s_captured_len;
    }
    memcpy(s_captured + s_captured_len, buf, n);
    s_captured_len += n;
    push_event(HOST_RCP_TX_EVENT_WRITE, (ssize_t)n);
    return (ssize_t)n;
}

void rcp_uart_tx_flush(void)
{
    s_flush_calls++;
    push_event(HOST_RCP_TX_EVENT_FLUSH, 0);
}

void rcp_uart_tx_recovery_restart(void)
{
    s_recovery_calls++;
    s_halted = true;
    if (s_recovery_checkpoint == NULL) {
        fprintf(stderr, "host_rcp_uart_tx_sim: rcp_uart_tx_recovery_restart() fired with no "
                         "checkpoint registered -- call host_rcp_tx_sim_set_recovery_checkpoint() first\n");
        abort();
    }
    siglongjmp(*s_recovery_checkpoint, 1);
}

void otPlatUartSendDone(void)
{
    s_send_done_calls++;
}
