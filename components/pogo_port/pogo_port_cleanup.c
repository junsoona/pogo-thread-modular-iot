#include "pogo_port/pogo_port_cleanup.h"

#include <stddef.h>

esp_err_t pogo_port_run_cleanup_ops(bool do_isr_remove, pogo_port_cleanup_op_t isr_remove_op,
                                     pogo_port_cleanup_op_t detect_release_op, bool do_uart_delete,
                                     pogo_port_cleanup_op_t uart_delete_op, void *ctx)
{
    esp_err_t first_err = ESP_OK;

    if (do_isr_remove && isr_remove_op != NULL) {
        esp_err_t e = isr_remove_op(ctx);
        if (e != ESP_OK && first_err == ESP_OK) {
            first_err = e;
        }
    }

    if (detect_release_op != NULL) {
        esp_err_t e = detect_release_op(ctx);
        if (e != ESP_OK && first_err == ESP_OK) {
            first_err = e;
        }
    }

    if (do_uart_delete && uart_delete_op != NULL) {
        esp_err_t e = uart_delete_op(ctx);
        if (e != ESP_OK && first_err == ESP_OK) {
            first_err = e;
        }
    }

    return first_err;
}
