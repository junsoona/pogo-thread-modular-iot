/*
 * Host-compiled verification of components/pogo_port/pogo_port.c's
 * pogo_port_disable() idempotency -- required test item 22 of the
 * power-base follow-up spec: "pogo_port_disable()을 구현했다면 반복 호출
 * 안전성 및 DETECT 해제".
 *
 * pogo_port.c cannot be host-compiled (real GPIO/UART/ISR ESP-IDF driver
 * calls) -- this mirrors its `enabled` guard exactly (see pogo_port.c's own
 * comment on that field): a call while already disabled (or never enabled)
 * skips vTaskDelete/gpio_isr_handler_remove/uart_driver_delete entirely and
 * only re-asserts that DETECT_OUT is released, rather than re-running
 * teardown against already-freed resources. Modeled with call counters
 * standing in for "would this crash/double-free the real driver calls".
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -o /tmp/test_pogo_port_disable_idempotent tools/test_pogo_port_disable_idempotent.c \
 *   && /tmp/test_pogo_port_disable_idempotent
 */
#include <stdio.h>
#include <stdbool.h>

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

typedef struct {
    bool inited;
    bool enabled;
    bool detect_out_active; /* mirrors set_detect_out(p, asserted) */
    int  task_delete_calls;
    int  isr_remove_calls;
    int  uart_delete_calls;
} mock_port_t;

static void mock_init(mock_port_t *p)
{
    p->inited = true;
    p->enabled = false;
    p->detect_out_active = false;
    p->task_delete_calls = 0;
    p->isr_remove_calls = 0;
    p->uart_delete_calls = 0;
}

static void mock_enable(mock_port_t *p)
{
    p->detect_out_active = true; /* pogo_port_enable() drives DETECT_OUT active as part of bring-up */
    p->enabled = true;
}

/* Mirrors the FIXED pogo_port_disable(): idempotent via the `enabled` guard. */
static bool mock_disable(mock_port_t *p)
{
    if (!p->inited) {
        return false; /* ESP_ERR_INVALID_STATE */
    }
    if (!p->enabled) {
        p->detect_out_active = false;
        return true;
    }
    p->task_delete_calls++;
    p->isr_remove_calls++;
    p->detect_out_active = false;
    p->uart_delete_calls++;
    p->enabled = false;
    return true;
}

static void scenario_repeated_disable_is_safe_and_idempotent(void)
{
    printf("\n-- scenario: enable then disable() repeatedly -- exactly one real teardown --\n");
    mock_port_t p;
    mock_init(&p);
    mock_enable(&p);
    CHECK(p.detect_out_active, "DETECT_OUT is active after enable()");

    CHECK(mock_disable(&p), "first disable() succeeds");
    CHECK(!p.detect_out_active, "DETECT_OUT is released after the first disable()");
    CHECK(p.task_delete_calls == 1 && p.isr_remove_calls == 1 && p.uart_delete_calls == 1,
          "the real teardown (task/ISR/UART) runs exactly once");

    for (int i = 0; i < 5; i++) {
        CHECK(mock_disable(&p), "a repeated disable() call still returns success, never an error/crash");
    }
    CHECK(!p.detect_out_active, "DETECT_OUT stays released across every repeated call");
    CHECK(p.task_delete_calls == 1 && p.isr_remove_calls == 1 && p.uart_delete_calls == 1,
          "repeated calls do NOT re-run teardown against already-freed resources (no double-free)");
}

static void scenario_disable_without_ever_enabling(void)
{
    printf("\n-- scenario: disable() on a port that was never enabled --\n");
    mock_port_t p;
    mock_init(&p);
    CHECK(mock_disable(&p), "disable() on a never-enabled port is still safe");
    CHECK(!p.detect_out_active, "DETECT_OUT ends up released even though it was never actively driven");
    CHECK(p.task_delete_calls == 0 && p.isr_remove_calls == 0 && p.uart_delete_calls == 0,
          "no teardown calls are made against resources that were never allocated");
}

int main(void)
{
    scenario_repeated_disable_is_safe_and_idempotent();
    scenario_disable_without_ever_enabling();

    printf("\n");
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
