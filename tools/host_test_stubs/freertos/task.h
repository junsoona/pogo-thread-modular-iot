/*
 * Host-only, pthread-backed stand-in for FreeRTOS's task.h -- unlike every
 * other stub in this directory, this one provides REAL concurrency (real
 * OS threads, real wall-clock delays), not just enough types to make a
 * hardware-independent component compile. It exists specifically so host
 * tests can link and run REAL production .c files that create their own
 * long-running FreeRTOS task (components/pogo_port/pogo_port.c's port_task,
 * components/child_service/child_service.c's child_service_task,
 * components/parent_service/parent_service.c's parent_service_task,
 * components/sensor_service/sensor_service.c's periodic_task) exactly as
 * they run on real hardware -- see tools/test_pogo_port_hotswap_recovery.c
 * and tools/test_conditioning_provisioning.c's own header comments for why
 * this is required instead of mirroring those files' logic by hand.
 *
 * TickType_t is real elapsed milliseconds since an arbitrary point (derived
 * from CLOCK_MONOTONIC, truncated to 32 bits) -- portTICK_PERIOD_MS is 1, so
 * pdMS_TO_TICKS(ms) is the identity. Every production call site compares
 * tick values with FreeRTOS's own signed-difference idiom
 * ((int32_t)(a - b) >= 0), which remains correct across this truncation's
 * wraparound exactly as it does across real FreeRTOS's own 32-bit tick
 * overflow -- so no shared "epoch" needs to be coordinated across the
 * several translation units (the test file itself, pogo_port.c,
 * child_service.c, ...) that each include this header.
 *
 * A "task" is a real pthread. vTaskDelay() sleeps via nanosleep() (a POSIX
 * cancellation point), and xQueueReceive()/xQueueSend() block via
 * pthread_cond_timedwait() (also a cancellation point) -- so vTaskDelete()'s
 * pthread_cancel()+pthread_join() can always promptly stop a task no matter
 * what it is currently blocked on, mirroring real FreeRTOS's vTaskDelete()
 * removing a task from the scheduler regardless of what it was waiting on.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t TickType_t;
typedef int32_t BaseType_t;
typedef uint32_t UBaseType_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0

#define portTICK_PERIOD_MS 1u
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)

#define portYIELD_FROM_ISR() ((void)0)

static inline TickType_t xTaskGetTickCount(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t ms = (uint64_t)now.tv_sec * 1000ULL + (uint64_t)now.tv_nsec / 1000000ULL;
    return (TickType_t)ms;
}

/* Host has no separate ISR context -- an "ISR" here is just a direct call
 * (see driver/gpio.h's host_gpio_test_edge()), so this is the same clock. */
static inline TickType_t xTaskGetTickCountFromISR(void)
{
    return xTaskGetTickCount();
}

static inline void vTaskDelay(TickType_t ticks)
{
    struct timespec ts;
    ts.tv_sec = ticks / 1000u;
    ts.tv_nsec = (long)(ticks % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
}

typedef void (*TaskFunction_t)(void *);

typedef struct {
    pthread_t thread;
    TaskFunction_t fn;
    void *arg;
} host_task_record_t;

typedef host_task_record_t *TaskHandle_t;

static inline void *host_task_trampoline(void *arg)
{
    host_task_record_t *t = (host_task_record_t *)arg;
    t->fn(t->arg); /* every real production task function loops forever; this
                       only returns if pthread_cancel() interrupts it at a
                       cancellation point (vTaskDelay/xQueueReceive/xQueueSend
                       above/below) mid-iteration -- normal, expected shutdown */
    return NULL;
}

static inline BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_depth, void *arg,
                                      UBaseType_t priority, TaskHandle_t *out_handle)
{
    (void)name;
    (void)stack_depth;
    (void)priority;
    host_task_record_t *t = (host_task_record_t *)malloc(sizeof(*t));
    if (t == NULL) {
        return pdFAIL;
    }
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(&t->thread, NULL, host_task_trampoline, t) != 0) {
        free(t);
        return pdFAIL;
    }
    if (out_handle != NULL) {
        *out_handle = t;
    }
    return pdPASS;
}

static inline void vTaskDelete(TaskHandle_t handle)
{
    if (handle == NULL) {
        pthread_exit(NULL); /* self-delete, mirrors vTaskDelete(NULL) on real FreeRTOS */
        return;
    }
    pthread_cancel(handle->thread);
    pthread_join(handle->thread, NULL);
    free(handle);
}

#ifdef __cplusplus
}
#endif
