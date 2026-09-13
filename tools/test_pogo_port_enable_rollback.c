/*
 * Host-compiled verification of pogo_port.c's pogo_port_enable() rollback
 * logic -- code review follow-up section 6: every ESP-IDF call's return
 * value is checked, and a failure at ANY step unwinds everything already
 * set up in that same call (DETECT_OUT released, any installed ISR
 * removed, the UART driver removed, no task left running, `enabled` left
 * false) so a subsequent retry starts from a clean state.
 *
 * pogo_port.c cannot be host-compiled (real GPIO/UART/ISR/FreeRTOS calls) --
 * this mirrors its exact step order and goto-based unwind structure
 * (uart_driver_install -> uart_param_config/uart_set_pin -> DETECT_OUT
 * gpio_config -> BASE_ID gpio_config -> DETECT_IN gpio_config ->
 * gpio_isr_handler_add -> xTaskCreate) with call counters standing in for
 * "would this leave real hardware/driver state behind".
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -o /tmp/test_pogo_port_enable_rollback tools/test_pogo_port_enable_rollback.c \
 *   && /tmp/test_pogo_port_enable_rollback
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

typedef enum {
    FAIL_NONE = 0,
    FAIL_UART_INSTALL,
    FAIL_UART_CONFIG,
    FAIL_DETECT_OUT_GPIO_CONFIG,
    FAIL_BASE_ID_GPIO_CONFIG,
    FAIL_DETECT_IN_GPIO_CONFIG,
    FAIL_ISR_ADD,
    FAIL_TASK_CREATE,
} fail_point_t;

typedef struct {
    bool enabled;
    bool uart_installed;
    bool isr_added;
    bool detect_out_asserted; /* true = actively driven Low ("asserted"), false = released */
    bool task_running;
    /* Call counters -- diagnostic, mirror what a real retry must not double-do. */
    int uart_install_calls;
    int uart_delete_calls;
    int isr_add_calls;
    int isr_remove_calls;
    int task_create_calls;
} port_t;

static void port_init(port_t *p)
{
    port_t zero = {0};
    *p = zero;
    p->detect_out_asserted = false; /* High-Z / released at reset */
}

/* Mirrors the hardened pogo_port_enable(): every step checked, `fail`
 * unwinds isr/detect_out/uart, `fail_no_uart` skips the uart_driver_delete
 * (nothing was ever installed in that case). */
static bool enable_port(port_t *p, fail_point_t fail_at)
{
    if (p->enabled) {
        return false; /* ESP_ERR_INVALID_STATE -- already enabled */
    }

    if (fail_at == FAIL_UART_INSTALL) {
        goto fail_no_uart;
    }
    p->uart_installed = true;
    p->uart_install_calls++;

    if (fail_at == FAIL_UART_CONFIG) {
        goto fail;
    }

    /* DETECT_OUT gpio_config + assert. */
    if (fail_at == FAIL_DETECT_OUT_GPIO_CONFIG) {
        goto fail;
    }
    p->detect_out_asserted = true;

    /* BASE_ID gpio_config (UPSTREAM-only in real hardware, modeled here
     * unconditionally since only the fail-injection point matters). */
    if (fail_at == FAIL_BASE_ID_GPIO_CONFIG) {
        goto fail;
    }

    /* DETECT_IN gpio_config. */
    if (fail_at == FAIL_DETECT_IN_GPIO_CONFIG) {
        goto fail;
    }

    if (fail_at == FAIL_ISR_ADD) {
        goto fail;
    }
    p->isr_added = true;
    p->isr_add_calls++;

    if (fail_at == FAIL_TASK_CREATE) {
        goto fail;
    }
    p->task_running = true;
    p->task_create_calls++;

    p->enabled = true;
    return true;

fail:
    if (p->isr_added) {
        p->isr_remove_calls++;
        p->isr_added = false;
    }
    p->detect_out_asserted = false; /* set_detect_out(p, false) -- always attempted */
    p->uart_delete_calls++;
    p->uart_installed = false;
fail_no_uart:
    p->task_running = false;
    p->enabled = false;
    return false;
}

static void check_fully_torn_down(const port_t *p, const char *label)
{
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: enabled==false", label);
    CHECK(!p->enabled, msg);
    snprintf(msg, sizeof(msg), "%s: DETECT_OUT is released (not asserted)", label);
    CHECK(!p->detect_out_asserted, msg);
    snprintf(msg, sizeof(msg), "%s: no ISR handler left installed", label);
    CHECK(!p->isr_added, msg);
    snprintf(msg, sizeof(msg), "%s: no UART driver left installed", label);
    CHECK(!p->uart_installed, msg);
    snprintf(msg, sizeof(msg), "%s: no port task left running", label);
    CHECK(!p->task_running, msg);
}

static void scenario_failure_at_each_step_fully_unwinds(void)
{
    printf("\n-- scenario: a failure at each step fully unwinds everything set up so far --\n");

    fail_point_t points[] = {FAIL_UART_INSTALL,        FAIL_UART_CONFIG,     FAIL_DETECT_OUT_GPIO_CONFIG,
                              FAIL_BASE_ID_GPIO_CONFIG, FAIL_DETECT_IN_GPIO_CONFIG, FAIL_ISR_ADD, FAIL_TASK_CREATE};
    const char *names[] = {"UART_INSTALL", "UART_CONFIG",     "DETECT_OUT_GPIO_CONFIG",
                            "BASE_ID_GPIO_CONFIG", "DETECT_IN_GPIO_CONFIG", "ISR_ADD", "TASK_CREATE"};

    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        port_t p;
        port_init(&p);
        bool ok = enable_port(&p, points[i]);
        CHECK(!ok, names[i]);
        check_fully_torn_down(&p, names[i]);
    }
}

static void scenario_retry_after_failure_succeeds_cleanly(void)
{
    printf("\n-- scenario: after a failure, a retry (whatever caused it now fixed) succeeds cleanly --\n");
    port_t p;
    port_init(&p);

    bool first = enable_port(&p, FAIL_ISR_ADD);
    CHECK(!first, "first attempt fails at ISR_ADD");
    check_fully_torn_down(&p, "after first failed attempt");

    bool second = enable_port(&p, FAIL_NONE);
    CHECK(second, "retry (no injected failure) succeeds");
    CHECK(p.enabled, "port is enabled after the successful retry");
    CHECK(p.detect_out_asserted, "DETECT_OUT is asserted after a successful enable");
    CHECK(p.uart_install_calls == 2, "uart_driver_install was called once per attempt (installed, torn down, "
                                      "installed again) -- no leaked double-install");
    CHECK(p.uart_delete_calls == 1, "uart_driver_delete was called exactly once, only for the failed attempt");
}

static void scenario_double_enable_rejected(void)
{
    printf("\n-- scenario: calling enable twice without disable() in between is rejected, not double-installed --\n");
    port_t p;
    port_init(&p);
    CHECK(enable_port(&p, FAIL_NONE), "first enable succeeds");
    bool second = enable_port(&p, FAIL_NONE);
    CHECK(!second, "a second enable() call while already enabled is rejected outright");
    CHECK(p.uart_install_calls == 1, "the UART driver was only ever installed once, not twice");
}

int main(void)
{
    scenario_failure_at_each_step_fully_unwinds();
    scenario_retry_after_failure_succeeds_cleanly();
    scenario_double_enable_rejected();

    printf("\n");
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
