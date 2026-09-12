/*
 * POGO UART transport -- thin wrapper around the ESP-IDF UART driver.
 *
 * Pin/UART/baud/buffer defaults all come from Kconfig (POGO_UART_*, see
 * this component's Kconfig) so both the upper and lower firmware read the
 * same single source of board configuration.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uart_port_t uart_num;
    int tx_gpio;
    int rx_gpio;
    int baud_rate;
    int rx_buf_size;
    int tx_buf_size;
} pogo_transport_config_t;

/* Fills cfg with the Kconfig (POGO_UART_*) defaults. */
void pogo_transport_config_default(pogo_transport_config_t *cfg);

/* Installs the UART driver and configures pins/baud per cfg. */
esp_err_t pogo_transport_init(const pogo_transport_config_t *cfg);

/* Encodes one frame (see pogo_frame.h) and writes it out over UART. */
esp_err_t pogo_transport_send(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len);

/*
 * Reads up to max_bytes into buf, waiting up to timeout_ticks.
 * Returns the number of bytes actually read (0 on timeout with no data),
 * or a negative value ((int)-esp_err_t) on driver error.
 */
int pogo_transport_read(uint8_t *buf, size_t max_bytes, TickType_t timeout_ticks);

#ifdef __cplusplus
}
#endif
