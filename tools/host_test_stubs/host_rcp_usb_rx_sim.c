/*
 * Implementation of host_rcp_usb_rx_sim.h -- see that header's own comment.
 * Provides __real_esp_openthread_platform_process() (the seam
 * rcp/main/rcp_usb_rx_recovery.h declares) and rcp_usb_rx_recovery_restart()
 * (the noreturn recovery hook).
 */
#include "host_rcp_usb_rx_sim.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"

#define HOST_RCP_RX_SIM_MAX_STEPS 512
#define HOST_RCP_RX_SIM_MAX_CALLS 512

static host_rcp_rx_step_t s_steps[HOST_RCP_RX_SIM_MAX_STEPS];
static size_t s_step_count;
static size_t s_step_index;
static host_rcp_rx_step_t s_default_step = {HOST_RCP_RX_STEP_OK, ESP_OK, 0};

static size_t s_real_calls;
static int s_recovery_calls;
static bool s_halted;
static host_rcp_rx_step_t s_halted_step;

static int s_errno_at_entry[HOST_RCP_RX_SIM_MAX_CALLS];
static size_t s_errno_at_entry_count;

static int64_t s_reset_time_us;

static sigjmp_buf *s_recovery_checkpoint;

void host_rcp_rx_sim_reset(void)
{
    s_step_count = 0;
    s_step_index = 0;
    s_default_step = (host_rcp_rx_step_t){HOST_RCP_RX_STEP_OK, ESP_OK, 0};
    s_real_calls = 0;
    s_recovery_calls = 0;
    s_halted = false;
    memset(&s_halted_step, 0, sizeof(s_halted_step));
    s_errno_at_entry_count = 0;
    memset(s_errno_at_entry, 0, sizeof(s_errno_at_entry));
    s_recovery_checkpoint = NULL;
    s_reset_time_us = esp_timer_get_time();
}

void host_rcp_rx_sim_set_script(const host_rcp_rx_step_t *steps, size_t count)
{
    if (count > HOST_RCP_RX_SIM_MAX_STEPS) {
        count = HOST_RCP_RX_SIM_MAX_STEPS;
    }
    memcpy(s_steps, steps, count * sizeof(*steps));
    s_step_count = count;
    s_step_index = 0;
}

void host_rcp_rx_sim_set_default_step(host_rcp_rx_step_t step)
{
    s_default_step = step;
}

size_t host_rcp_rx_sim_real_call_count(void)
{
    return s_real_calls;
}

int host_rcp_rx_sim_recovery_call_count(void)
{
    return s_recovery_calls;
}

int host_rcp_rx_sim_errno_at_entry(size_t index)
{
    return s_errno_at_entry[index];
}

double host_rcp_rx_sim_elapsed_ms(void)
{
    return (double)(esp_timer_get_time() - s_reset_time_us) / 1000.0;
}

void host_rcp_rx_sim_set_recovery_checkpoint(sigjmp_buf *checkpoint)
{
    s_recovery_checkpoint = checkpoint;
}

esp_err_t __real_esp_openthread_platform_process(otInstance *instance,
                                                   const esp_openthread_mainloop_context_t *mainloop)
{
    (void)instance;
    (void)mainloop;

    /* Captured BEFORE this fake touches errno at all -- proves the caller
     * (the wrap under test) really reset it to 0 immediately before THIS
     * call, not just before the very first one. */
    if (s_errno_at_entry_count < HOST_RCP_RX_SIM_MAX_CALLS) {
        s_errno_at_entry[s_errno_at_entry_count++] = errno;
    }
    s_real_calls++;

    host_rcp_rx_step_t step;
    if (s_halted) {
        /* Device already handed off to the recovery hook once -- exactly
         * like a real chip mid-reboot, nothing can recover through this
         * session any more. */
        step = s_halted_step;
    } else {
        step = (s_step_index < s_step_count) ? s_steps[s_step_index++] : s_default_step;
    }

    if (step.kind == HOST_RCP_RX_STEP_OK) {
        return ESP_OK; /* deliberately does not touch errno */
    }

    errno = step.err_errno;
    return step.err;
}

void rcp_usb_rx_recovery_restart(void)
{
    s_recovery_calls++;
    if (!s_halted) {
        /* Freeze whatever step was in flight when recovery first fired, so
         * a next-attempt call after "reboot" keeps failing the same way
         * rather than silently resuming the script. */
        s_halted_step = (s_step_index > 0 && s_step_index <= s_step_count) ? s_steps[s_step_index - 1] : s_default_step;
        if (s_halted_step.kind != HOST_RCP_RX_STEP_FAIL) {
            s_halted_step = (host_rcp_rx_step_t){HOST_RCP_RX_STEP_FAIL, ESP_FAIL, EIO};
        }
        s_halted = true;
    }
    if (s_recovery_checkpoint == NULL) {
        fprintf(stderr, "host_rcp_usb_rx_sim: rcp_usb_rx_recovery_restart() fired with no "
                         "checkpoint registered -- call host_rcp_rx_sim_set_recovery_checkpoint() first\n");
        abort();
    }
    siglongjmp(*s_recovery_checkpoint, 1);
}
