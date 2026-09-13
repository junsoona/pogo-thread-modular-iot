/*
 * Pure cleanup-aggregation policy shared by pogo_port_enable()'s
 * failure-unwind path and pogo_port_disable() (code review follow-up:
 * "pogo_port_enable()/pogo_port_disable() 실패 경로 수정"): attempt every
 * requested cleanup step regardless of whether an earlier one failed, and
 * report the FIRST failure seen (ESP_OK if everything succeeded, or if
 * nothing was requested).
 *
 * Deliberately has no ESP-IDF hardware/FreeRTOS dependency of its own --
 * only `esp_err_t` and the operations themselves are passed in as function
 * pointers -- so this exact policy (not a copy of it) can be linked
 * directly into a host test that injects failing stub operations. See
 * tools/test_pogo_port_cleanup_ops.c: it builds and links
 * pogo_port_cleanup.c unmodified, the same object code pogo_port.c itself
 * calls into.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*pogo_port_cleanup_op_t)(void *ctx);

/*
 * Runs, in this fixed order, up to three cleanup operations:
 *   1. isr_remove_op, only if do_isr_remove is true
 *   2. detect_release_op, always (DETECT_OUT release is always attempted,
 *      the same "best-effort, always try to leave the pin released" policy
 *      pogo_port.c already documents)
 *   3. uart_delete_op, only if do_uart_delete is true
 * A NULL op pointer for a step that IS requested is treated as "nothing to
 * do" for that step (returns ESP_OK for it) rather than a crash -- so a
 * caller that genuinely has no detect_release_op (should not happen in
 * pogo_port.c, but defensive here since this is a general-purpose function)
 * does not fault.
 *
 * Every requested step is attempted even if an earlier one failed. Returns
 * the esp_err_t of the FIRST step that failed, or ESP_OK if all requested
 * steps succeeded.
 */
esp_err_t pogo_port_run_cleanup_ops(bool do_isr_remove, pogo_port_cleanup_op_t isr_remove_op,
                                     pogo_port_cleanup_op_t detect_release_op, bool do_uart_delete,
                                     pogo_port_cleanup_op_t uart_delete_op, void *ctx);

#ifdef __cplusplus
}
#endif
