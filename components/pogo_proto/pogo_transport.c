#include "pogo_proto/pogo_transport.h"
#include "pogo_proto/pogo_frame.h"
#include "sdkconfig.h"
#include "esp_log.h"

static const char *TAG = "pogo_transport";
static uart_port_t s_uart_num = UART_NUM_1;

void pogo_transport_config_default(pogo_transport_config_t *cfg)
{
    cfg->uart_num = (uart_port_t)CONFIG_POGO_UART_PORT_NUM;
    cfg->tx_gpio = CONFIG_POGO_UART_TX_GPIO;
    cfg->rx_gpio = CONFIG_POGO_UART_RX_GPIO;
    cfg->baud_rate = CONFIG_POGO_UART_BAUD_RATE;
    cfg->rx_buf_size = CONFIG_POGO_UART_RX_BUF_SIZE;
    cfg->tx_buf_size = CONFIG_POGO_UART_TX_BUF_SIZE;
}

esp_err_t pogo_transport_init(const pogo_transport_config_t *cfg)
{
    pogo_transport_config_t defaults;
    if (cfg == NULL) {
        pogo_transport_config_default(&defaults);
        cfg = &defaults;
    }
    s_uart_num = cfg->uart_num;

    uart_config_t uart_cfg = {
        .baud_rate = cfg->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(s_uart_num, cfg->rx_buf_size, cfg->tx_buf_size, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(s_uart_num, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_set_pin(s_uart_num, cfg->tx_gpio, cfg->rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "UART%d ready: TX=%d RX=%d %d baud 8N1", (int)s_uart_num, cfg->tx_gpio, cfg->rx_gpio,
             cfg->baud_rate);
    return ESP_OK;
}

esp_err_t pogo_transport_send(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    uint8_t buf[POGO_FRAME_MAX_SIZE];
    size_t n = pogo_frame_encode(type, seq, payload, len, buf, sizeof(buf));
    if (n == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int written = uart_write_bytes(s_uart_num, (const char *)buf, n);
    if (written < 0 || (size_t)written != n) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

int pogo_transport_read(uint8_t *buf, size_t max_bytes, TickType_t timeout_ticks)
{
    int n = uart_read_bytes(s_uart_num, buf, max_bytes, timeout_ticks);
    return n; /* uart_read_bytes returns 0 on timeout, -1 on parameter/driver error */
}
