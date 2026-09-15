/*
 * Host-only stand-in for ESP-IDF's driver/uart.h, backing REAL, unmodified
 * components/pogo_port/pogo_port.c and components/pms5003/pms5003.c with a
 * small in-memory fake UART bank (one independent RX byte ring + one TX byte
 * ring per uart_port_t) -- see tools/host_test_stubs/host_uart_sim.c for the
 * implementation and the test-only control functions declared at the bottom
 * of this file.
 *
 * A host test acts as "the peer at the other end of the wire": it injects
 * bytes via host_uart_test_push_rx() (what uart_read_bytes() will return)
 * and drains bytes via host_uart_test_take_tx_byte() (what uart_write_bytes()
 * sent) -- both using the REAL wire-format code in components/pogo_proto or
 * components/pms5003/pms5003_parser.c to build/parse frames, never
 * reimplementing that logic by hand.
 *
 * Declarations only (extern, not static/inline) for the same cross-
 * translation-unit sharing reason as driver/gpio.h.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int uart_port_t;

#define UART_PIN_NO_CHANGE (-1)

typedef enum {
    UART_DATA_5_BITS = 0,
    UART_DATA_6_BITS,
    UART_DATA_7_BITS,
    UART_DATA_8_BITS,
} uart_word_length_t;

typedef enum {
    UART_PARITY_DISABLE = 0,
    UART_PARITY_EVEN,
    UART_PARITY_ODD,
} uart_parity_t;

typedef enum {
    UART_STOP_BITS_1 = 1,
    UART_STOP_BITS_1_5,
    UART_STOP_BITS_2,
} uart_stop_bits_t;

typedef enum {
    UART_HW_FLOWCTRL_DISABLE = 0,
} uart_hw_flowcontrol_t;

typedef enum {
    UART_SCLK_DEFAULT = 0,
} uart_sclk_t;

typedef struct {
    int baud_rate;
    uart_word_length_t data_bits;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
    uart_hw_flowcontrol_t flow_ctrl;
    uart_sclk_t source_clk;
} uart_config_t;

esp_err_t uart_driver_install(uart_port_t uart_num, int rx_buffer_size, int tx_buffer_size, int queue_size,
                               void *uart_queue, int intr_alloc_flags);
esp_err_t uart_param_config(uart_port_t uart_num, const uart_config_t *uart_config);
esp_err_t uart_set_pin(uart_port_t uart_num, int tx_io_num, int rx_io_num, int rts_io_num, int cts_io_num);
int uart_write_bytes(uart_port_t uart_num, const void *src, size_t size);
int uart_read_bytes(uart_port_t uart_num, void *buf, uint32_t length, TickType_t ticks_to_wait);
esp_err_t uart_flush_input(uart_port_t uart_num);
esp_err_t uart_wait_tx_done(uart_port_t uart_num, TickType_t ticks_to_wait);
esp_err_t uart_driver_delete(uart_port_t uart_num);

/* --- Test-only controls (not part of the real ESP-IDF API), implemented in
 * host_uart_sim.c --- */

/* Injects `len` bytes into `uart_num`'s RX ring -- the next
 * uart_read_bytes() calls against this port will return them, in order,
 * exactly as if a real peer had transmitted them on the wire. */
void host_uart_test_push_rx(uart_port_t uart_num, const uint8_t *data, size_t len);

/* Pops one byte this port has transmitted via uart_write_bytes(), blocking
 * up to timeout_ms for one to become available. Returns true and fills
 * *out_byte on success, false on timeout -- lets a test reconstruct exactly
 * what the real production code under test sent, byte by byte, by feeding
 * this into a real pogo_frame_parser_t/pms5003_parser_t of its own. */
bool host_uart_test_take_tx_byte(uart_port_t uart_num, uint8_t *out_byte, uint32_t timeout_ms);

/* Clears every port's RX/TX rings and installed state -- call between test
 * scenarios that reuse the same uart_num, so one scenario's leftover bytes
 * can never leak into the next. */
void host_uart_test_reset(void);

/* Makes the NEXT uart_write_bytes() call on `uart_num` fail (return -1,
 * writing nothing) -- a one-shot fault, automatically cleared after that one
 * call, whether or not it was ever actually made. Used to exercise a
 * production driver's real write-failure cleanup path (e.g.
 * components/pms5003/pms5003.c's pms5003_init()'s wake/mode command
 * failure -> uart_driver_delete()) without needing a real broken UART. */
void host_uart_test_force_next_write_fail(uart_port_t uart_num);

/* Same idea for the next uart_read_bytes() call: it returns -1 immediately
 * (no bytes collected, matching a real UART/DMA error) instead of its usual
 * blocking-collect behavior. One-shot, auto-cleared. */
void host_uart_test_force_next_read_fail(uart_port_t uart_num);

#ifdef __cplusplus
}
#endif
