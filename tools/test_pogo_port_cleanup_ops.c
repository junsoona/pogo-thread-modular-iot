/*
 * Host-compiled verification of components/pogo_port/pogo_port_cleanup.c --
 * code review follow-up (2nd pass), item 1: "pogo_port_enable()/
 * pogo_port_disable() 실패 경로 수정".
 *
 * Unlike every other pogo_port-related test in this directory (which mirror
 * pogo_port.c's logic because the real file has real GPIO/UART/FreeRTOS
 * calls that cannot be host-compiled), THIS test links against the REAL,
 * UNMODIFIED components/pogo_port/pogo_port_cleanup.c -- the actual object
 * code pogo_port.c's pogo_port_enable() failure-unwind path and
 * pogo_port_disable() both call (via cleanup_op_isr_remove()/
 * cleanup_op_detect_release()/cleanup_op_uart_delete() adapters in
 * pogo_port.c itself). pogo_port_cleanup.c takes the three cleanup
 * operations as function pointers specifically so this policy -- "attempt
 * every requested step regardless of an earlier failure, return the FIRST
 * failure seen" -- can be exercised here with fault-injecting stub
 * operations instead of real hardware calls, with zero logic duplicated or
 * re-derived by hand.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I tools/host_test_stubs -I components/pogo_port/include \
 *      -o /tmp/test_pogo_port_cleanup_ops tools/test_pogo_port_cleanup_ops.c \
 *      components/pogo_port/pogo_port_cleanup.c \
 *   && /tmp/test_pogo_port_cleanup_ops
 */
#include <stdio.h>
#include <stdbool.h>

#include "pogo_port/pogo_port_cleanup.h"

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

/* Fault-injectable stub context: each op can be told to fail with a
 * specific code, and every call is counted so a test can assert "still
 * attempted even though an earlier step failed". */
typedef struct {
    esp_err_t isr_remove_result;
    esp_err_t detect_release_result;
    esp_err_t uart_delete_result;
    int isr_remove_calls;
    int detect_release_calls;
    int uart_delete_calls;
} stub_ctx_t;

static void stub_ctx_init(stub_ctx_t *s)
{
    s->isr_remove_result = ESP_OK;
    s->detect_release_result = ESP_OK;
    s->uart_delete_result = ESP_OK;
    s->isr_remove_calls = 0;
    s->detect_release_calls = 0;
    s->uart_delete_calls = 0;
}

static esp_err_t stub_isr_remove(void *ctx)
{
    stub_ctx_t *s = (stub_ctx_t *)ctx;
    s->isr_remove_calls++;
    return s->isr_remove_result;
}

static esp_err_t stub_detect_release(void *ctx)
{
    stub_ctx_t *s = (stub_ctx_t *)ctx;
    s->detect_release_calls++;
    return s->detect_release_result;
}

static esp_err_t stub_uart_delete(void *ctx)
{
    stub_ctx_t *s = (stub_ctx_t *)ctx;
    s->uart_delete_calls++;
    return s->uart_delete_result;
}

static void scenario_all_succeed(void)
{
    printf("\n-- scenario: every step succeeds -> ESP_OK, every op called exactly once --\n");
    stub_ctx_t s;
    stub_ctx_init(&s);
    esp_err_t r = pogo_port_run_cleanup_ops(true, stub_isr_remove, stub_detect_release, true, stub_uart_delete, &s);
    CHECK(r == ESP_OK, "returns ESP_OK when everything succeeds");
    CHECK(s.isr_remove_calls == 1 && s.detect_release_calls == 1 && s.uart_delete_calls == 1,
          "every requested op was called exactly once");
}

static void scenario_isr_remove_fails_others_still_run(void)
{
    printf("\n-- scenario: ISR remove fails -- DETECT release and UART delete are STILL attempted --\n");
    stub_ctx_t s;
    stub_ctx_init(&s);
    s.isr_remove_result = ESP_FAIL;
    esp_err_t r = pogo_port_run_cleanup_ops(true, stub_isr_remove, stub_detect_release, true, stub_uart_delete, &s);
    CHECK(r == ESP_FAIL, "the ISR-remove failure is returned as the first error");
    CHECK(s.detect_release_calls == 1, "DETECT_OUT release is still attempted after the ISR-remove failure");
    CHECK(s.uart_delete_calls == 1, "UART delete is still attempted after the ISR-remove failure");
}

static void scenario_detect_release_fails_uart_still_runs(void)
{
    printf("\n-- scenario: DETECT_OUT release fails -- UART delete is STILL attempted, first error preserved --\n");
    stub_ctx_t s;
    stub_ctx_init(&s);
    s.detect_release_result = ESP_ERR_TIMEOUT;
    s.uart_delete_result = ESP_FAIL; /* a SECOND failure -- must not overwrite the first */
    esp_err_t r = pogo_port_run_cleanup_ops(true, stub_isr_remove, stub_detect_release, true, stub_uart_delete, &s);
    CHECK(r == ESP_ERR_TIMEOUT, "the FIRST failure (DETECT_OUT release) is what's returned, not the later UART one");
    CHECK(s.uart_delete_calls == 1, "UART delete is still attempted even though DETECT_OUT release already failed");
}

static void scenario_uart_delete_fails_alone(void)
{
    printf("\n-- scenario: only UART delete fails --\n");
    stub_ctx_t s;
    stub_ctx_init(&s);
    s.uart_delete_result = ESP_ERR_INVALID_ARG;
    esp_err_t r = pogo_port_run_cleanup_ops(true, stub_isr_remove, stub_detect_release, true, stub_uart_delete, &s);
    CHECK(r == ESP_ERR_INVALID_ARG, "the UART-delete failure is returned when it is the only one that failed");
    CHECK(s.isr_remove_calls == 1 && s.detect_release_calls == 1, "the earlier two steps still ran normally");
}

static void scenario_isr_remove_skipped_when_not_requested(void)
{
    printf("\n-- scenario: do_isr_remove=false -- ISR op is never called at all --\n");
    stub_ctx_t s;
    stub_ctx_init(&s);
    esp_err_t r = pogo_port_run_cleanup_ops(false, stub_isr_remove, stub_detect_release, true, stub_uart_delete, &s);
    CHECK(r == ESP_OK, "succeeds when the only requested ops succeed");
    CHECK(s.isr_remove_calls == 0, "ISR remove is never called when do_isr_remove is false (mirrors: no DETECT_IN "
                                    "configured, so no ISR was ever installed to remove)");
    CHECK(s.detect_release_calls == 1, "DETECT_OUT release is unconditional regardless of do_isr_remove");
}

static void scenario_uart_delete_skipped_when_not_requested(void)
{
    printf("\n-- scenario: do_uart_delete=false -- UART op is never called at all --\n");
    stub_ctx_t s;
    stub_ctx_init(&s);
    esp_err_t r = pogo_port_run_cleanup_ops(true, stub_isr_remove, stub_detect_release, false, stub_uart_delete, &s);
    CHECK(r == ESP_OK, "succeeds when the only requested ops succeed");
    CHECK(s.uart_delete_calls == 0, "UART delete is never called when do_uart_delete is false (mirrors: "
                                     "uart_driver_install() itself never succeeded, e.g. fail_no_uart:)");
}

static esp_err_t always_ok(void *ctx)
{
    (void)ctx;
    return ESP_OK;
}

static void scenario_null_ops_are_harmless(void)
{
    printf("\n-- scenario: a NULL op for a requested step is treated as a harmless no-op, not a crash --\n");
    esp_err_t r = pogo_port_run_cleanup_ops(true, NULL, always_ok, true, NULL, NULL);
    CHECK(r == ESP_OK, "NULL isr/uart ops with a NULL ctx do not crash and do not count as failures");
}

static void scenario_only_detect_release_requested(void)
{
    printf("\n-- scenario: neither ISR removal nor UART delete requested -- only DETECT_OUT release runs --\n");
    stub_ctx_t s;
    stub_ctx_init(&s);
    s.detect_release_result = ESP_FAIL;
    esp_err_t r = pogo_port_run_cleanup_ops(false, stub_isr_remove, stub_detect_release, false, stub_uart_delete, &s);
    CHECK(r == ESP_FAIL, "DETECT_OUT release's own failure is still returned even when it is the only step run");
    CHECK(s.isr_remove_calls == 0 && s.uart_delete_calls == 0, "the two optional steps were correctly skipped");
}

int main(void)
{
    scenario_all_succeed();
    scenario_isr_remove_fails_others_still_run();
    scenario_detect_release_fails_uart_still_runs();
    scenario_uart_delete_fails_alone();
    scenario_isr_remove_skipped_when_not_requested();
    scenario_uart_delete_skipped_when_not_requested();
    scenario_null_ops_are_harmless();
    scenario_only_detect_release_requested();

    printf("\n");
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
