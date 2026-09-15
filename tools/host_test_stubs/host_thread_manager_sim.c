/*
 * Host-only stand-in for components/thread_manager/thread_manager.c, used by
 * tools/test_conditioning_provisioning.c so it can link the REAL, unmodified
 * components/child_service/child_service.c and components/parent_service/
 * parent_service.c -- both call into thread_manager.h's API, which wraps
 * real OpenThread/Thread-radio calls that cannot be host-compiled at all
 * (there is no host OpenThread stack). This provides ONLY the five
 * thread_manager.h functions child_service.c/parent_service.c actually call
 * (thread_manager_validate_dataset_tlvs/join_with_dataset/
 * poll_role_change/get_role/role_to_str) with a small, deterministic,
 * test-controllable join simulation -- it is a stand-in for the Thread-radio
 * HARDWARE dependency itself (same category as driver/gpio.h, driver/uart.h,
 * nvs.h), never a reimplementation of child_service.c's or parent_service.c's
 * OWN logic, which both run completely unmodified against this.
 *
 * Default behavior (no test override): thread_manager_join_with_dataset()
 * succeeds almost immediately, reporting OT_DEVICE_ROLE_CHILD on the next
 * thread_manager_poll_role_change() -- exercises child_service.c's real
 * CS_STATE_JOINING -> CS_STATE_JOINED path and parent_service.c's real
 * wait_for_type(..., POGO_MSG_JOIN_RESULT, ...) path without a test needing
 * to babysit Thread join mechanics it is not the one under test here.
 */
#include "thread_manager/thread_manager.h"

#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static otDeviceRole s_current_role = OT_DEVICE_ROLE_DETACHED;
static bool s_role_change_pending = false;
static otDeviceRole s_pending_role = OT_DEVICE_ROLE_CHILD;

/* Test controls (see tools/test_conditioning_provisioning.c). */
static bool s_join_fails = false;
static otDeviceRole s_join_success_role = OT_DEVICE_ROLE_CHILD;
static unsigned s_join_delay_ms = 5;

void host_thread_manager_test_reset(void)
{
    pthread_mutex_lock(&s_mutex);
    s_current_role = OT_DEVICE_ROLE_DETACHED;
    s_role_change_pending = false;
    s_pending_role = OT_DEVICE_ROLE_CHILD;
    s_join_fails = false;
    s_join_success_role = OT_DEVICE_ROLE_CHILD;
    s_join_delay_ms = 5;
    pthread_mutex_unlock(&s_mutex);
}

void host_thread_manager_test_set_join_outcome(bool fail, otDeviceRole success_role, unsigned delay_ms)
{
    pthread_mutex_lock(&s_mutex);
    s_join_fails = fail;
    s_join_success_role = success_role;
    s_join_delay_ms = delay_ms;
    pthread_mutex_unlock(&s_mutex);
}

typedef struct {
    unsigned delay_ms;
    otDeviceRole role;
} join_after_delay_arg_t;

static void *join_after_delay(void *arg)
{
    join_after_delay_arg_t *a = (join_after_delay_arg_t *)arg;
    usleep(a->delay_ms * 1000u);
    pthread_mutex_lock(&s_mutex);
    s_role_change_pending = true;
    s_pending_role = a->role;
    pthread_mutex_unlock(&s_mutex);
    free(a);
    return NULL;
}

bool thread_manager_validate_dataset_tlvs(const uint8_t *dataset_tlvs, uint16_t dataset_len)
{
    return dataset_tlvs != NULL && dataset_len > 0;
}

esp_err_t thread_manager_join_with_dataset(thread_manager_role_t role, const uint8_t *dataset_tlvs,
                                            uint16_t dataset_len, uint32_t poll_period_ms)
{
    (void)role;
    (void)dataset_tlvs;
    (void)dataset_len;
    (void)poll_period_ms;
    pthread_mutex_lock(&s_mutex);
    bool fails = s_join_fails;
    unsigned delay_ms = s_join_delay_ms;
    otDeviceRole success_role = s_join_success_role;
    pthread_mutex_unlock(&s_mutex);
    if (fails) {
        return ESP_OK; /* accepted, but no role-change event ever follows -- the caller's own join_timeout fires */
    }
    join_after_delay_arg_t *a = (join_after_delay_arg_t *)malloc(sizeof(*a));
    if (a == NULL) {
        return ESP_ERR_NO_MEM;
    }
    a->delay_ms = delay_ms;
    a->role = success_role;
    pthread_t t;
    if (pthread_create(&t, NULL, join_after_delay, a) != 0) {
        free(a);
        return ESP_ERR_NO_MEM;
    }
    pthread_detach(t);
    return ESP_OK;
}

bool thread_manager_poll_role_change(otDeviceRole *out_role)
{
    pthread_mutex_lock(&s_mutex);
    bool has = s_role_change_pending;
    if (has) {
        s_current_role = s_pending_role;
        *out_role = s_pending_role;
        s_role_change_pending = false;
    }
    pthread_mutex_unlock(&s_mutex);
    return has;
}

otDeviceRole thread_manager_get_role(void)
{
    pthread_mutex_lock(&s_mutex);
    otDeviceRole r = s_current_role;
    pthread_mutex_unlock(&s_mutex);
    return r;
}

esp_err_t thread_manager_get_active_dataset_tlvs(otOperationalDatasetTlvs *out_tlvs)
{
    if (out_tlvs == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_tlvs->mTlvs, 0xAA, 4);
    out_tlvs->mLength = 4;
    return ESP_OK;
}

const char *thread_manager_role_to_str(otDeviceRole role)
{
    switch (role) {
    case OT_DEVICE_ROLE_DISABLED: return "DISABLED";
    case OT_DEVICE_ROLE_DETACHED: return "DETACHED";
    case OT_DEVICE_ROLE_CHILD: return "CHILD";
    case OT_DEVICE_ROLE_ROUTER: return "ROUTER";
    case OT_DEVICE_ROLE_LEADER: return "LEADER";
    default: return "UNKNOWN";
    }
}
