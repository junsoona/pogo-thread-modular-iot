/*
 * Implementation of the fake UART bank declared in driver/uart.h -- see that
 * file's own header comment. One RX ring (what uart_read_bytes() returns)
 * and one TX ring (what uart_write_bytes() sent, drained by
 * host_uart_test_take_tx_byte()) per uart_port_t, each mutex+condvar guarded
 * so the production task (blocking in uart_read_bytes()) and the test's main
 * thread (pushing RX / draining TX) can safely run concurrently.
 */
#include "driver/uart.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/queue.h" /* host_deadline_from_ticks() */

#define HOST_UART_PORT_COUNT 4
#define HOST_UART_RING_CAP 4096

typedef struct {
    bool installed;
    bool force_write_fail;
    bool force_read_fail;
    uint8_t rx_buf[HOST_UART_RING_CAP];
    size_t rx_head, rx_count;
    pthread_cond_t rx_not_empty;
    uint8_t tx_buf[HOST_UART_RING_CAP];
    size_t tx_head, tx_count;
    pthread_cond_t tx_not_empty;
    pthread_cond_t tx_not_full;
} host_uart_port_t;

static pthread_mutex_t s_uart_mutex = PTHREAD_MUTEX_INITIALIZER;
static host_uart_port_t s_ports[HOST_UART_PORT_COUNT];
static bool s_ports_inited = false;

static void ensure_inited_locked(void)
{
    if (s_ports_inited) {
        return;
    }
    for (int i = 0; i < HOST_UART_PORT_COUNT; i++) {
        memset(&s_ports[i], 0, sizeof(s_ports[i]));
        pthread_cond_init(&s_ports[i].rx_not_empty, NULL);
        pthread_cond_init(&s_ports[i].tx_not_empty, NULL);
        pthread_cond_init(&s_ports[i].tx_not_full, NULL);
    }
    s_ports_inited = true;
}

static bool port_valid(uart_port_t uart_num)
{
    return uart_num >= 0 && uart_num < HOST_UART_PORT_COUNT;
}

esp_err_t uart_driver_install(uart_port_t uart_num, int rx_buffer_size, int tx_buffer_size, int queue_size,
                               void *uart_queue, int intr_alloc_flags)
{
    (void)rx_buffer_size;
    (void)tx_buffer_size;
    (void)queue_size;
    (void)uart_queue;
    (void)intr_alloc_flags;
    if (!port_valid(uart_num)) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_uart_mutex);
    ensure_inited_locked();
    if (s_ports[uart_num].installed) {
        pthread_mutex_unlock(&s_uart_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_ports[uart_num].installed = true;
    s_ports[uart_num].rx_head = s_ports[uart_num].rx_count = 0;
    s_ports[uart_num].tx_head = s_ports[uart_num].tx_count = 0;
    pthread_mutex_unlock(&s_uart_mutex);
    return ESP_OK;
}

esp_err_t uart_param_config(uart_port_t uart_num, const uart_config_t *uart_config)
{
    (void)uart_config;
    return port_valid(uart_num) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t uart_set_pin(uart_port_t uart_num, int tx_io_num, int rx_io_num, int rts_io_num, int cts_io_num)
{
    (void)tx_io_num;
    (void)rx_io_num;
    (void)rts_io_num;
    (void)cts_io_num;
    return port_valid(uart_num) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

int uart_write_bytes(uart_port_t uart_num, const void *src, size_t size)
{
    if (!port_valid(uart_num) || src == NULL) {
        return -1;
    }
    const uint8_t *bytes = (const uint8_t *)src;
    pthread_mutex_lock(&s_uart_mutex);
    ensure_inited_locked();
    host_uart_port_t *p = &s_ports[uart_num];
    if (p->force_write_fail) {
        p->force_write_fail = false;
        pthread_mutex_unlock(&s_uart_mutex);
        return -1;
    }
    for (size_t i = 0; i < size; i++) {
        while (p->tx_count == HOST_UART_RING_CAP) {
            /* Real uart_write_bytes() has no timeout parameter -- it blocks
             * until space frees. A host test is expected to drain TX
             * promptly (see host_uart_test_take_tx_byte()), so this ring
             * filling up should not happen in practice. */
            pthread_cond_wait(&p->tx_not_full, &s_uart_mutex);
        }
        size_t tail = (p->tx_head + p->tx_count) % HOST_UART_RING_CAP;
        p->tx_buf[tail] = bytes[i];
        p->tx_count++;
    }
    pthread_cond_broadcast(&p->tx_not_empty);
    pthread_mutex_unlock(&s_uart_mutex);
    return (int)size;
}

int uart_read_bytes(uart_port_t uart_num, void *buf, uint32_t length, TickType_t ticks_to_wait)
{
    if (!port_valid(uart_num) || buf == NULL) {
        return -1;
    }
    uint8_t *out = (uint8_t *)buf;
    struct timespec deadline;
    bool have_deadline = false;
    size_t got = 0;
    pthread_mutex_lock(&s_uart_mutex);
    ensure_inited_locked();
    host_uart_port_t *p = &s_ports[uart_num];
    if (p->force_read_fail) {
        p->force_read_fail = false;
        pthread_mutex_unlock(&s_uart_mutex);
        return -1;
    }
    while (got < length) {
        while (p->rx_count > 0 && got < length) {
            out[got++] = p->rx_buf[p->rx_head];
            p->rx_head = (p->rx_head + 1) % HOST_UART_RING_CAP;
            p->rx_count--;
        }
        if (got >= length || ticks_to_wait == 0) {
            break;
        }
        if (!have_deadline) {
            host_deadline_from_ticks(&deadline, ticks_to_wait);
            have_deadline = true;
        }
        if (pthread_cond_timedwait(&p->rx_not_empty, &s_uart_mutex, &deadline) != 0) {
            break; /* timeout: return whatever was collected so far (possibly 0), matching real uart_read_bytes() */
        }
    }
    pthread_mutex_unlock(&s_uart_mutex);
    return (int)got;
}

esp_err_t uart_flush_input(uart_port_t uart_num)
{
    if (!port_valid(uart_num)) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_uart_mutex);
    ensure_inited_locked();
    s_ports[uart_num].rx_head = s_ports[uart_num].rx_count = 0;
    pthread_mutex_unlock(&s_uart_mutex);
    return ESP_OK;
}

esp_err_t uart_wait_tx_done(uart_port_t uart_num, TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    /* Our fake uart_write_bytes() already copies synchronously -- there is
     * no separate "still transmitting" state to wait out. */
    return port_valid(uart_num) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t uart_driver_delete(uart_port_t uart_num)
{
    if (!port_valid(uart_num)) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_uart_mutex);
    ensure_inited_locked();
    s_ports[uart_num].installed = false;
    pthread_mutex_unlock(&s_uart_mutex);
    return ESP_OK;
}

void host_uart_test_push_rx(uart_port_t uart_num, const uint8_t *data, size_t len)
{
    if (!port_valid(uart_num)) {
        return;
    }
    pthread_mutex_lock(&s_uart_mutex);
    ensure_inited_locked();
    host_uart_port_t *p = &s_ports[uart_num];
    for (size_t i = 0; i < len && p->rx_count < HOST_UART_RING_CAP; i++) {
        size_t tail = (p->rx_head + p->rx_count) % HOST_UART_RING_CAP;
        p->rx_buf[tail] = data[i];
        p->rx_count++;
    }
    pthread_cond_broadcast(&p->rx_not_empty);
    pthread_mutex_unlock(&s_uart_mutex);
}

bool host_uart_test_take_tx_byte(uart_port_t uart_num, uint8_t *out_byte, uint32_t timeout_ms)
{
    if (!port_valid(uart_num)) {
        return false;
    }
    struct timespec deadline;
    host_deadline_from_ticks(&deadline, (TickType_t)timeout_ms);
    pthread_mutex_lock(&s_uart_mutex);
    ensure_inited_locked();
    host_uart_port_t *p = &s_ports[uart_num];
    while (p->tx_count == 0) {
        if (pthread_cond_timedwait(&p->tx_not_empty, &s_uart_mutex, &deadline) != 0) {
            pthread_mutex_unlock(&s_uart_mutex);
            return false;
        }
    }
    *out_byte = p->tx_buf[p->tx_head];
    p->tx_head = (p->tx_head + 1) % HOST_UART_RING_CAP;
    p->tx_count--;
    pthread_cond_signal(&p->tx_not_full);
    pthread_mutex_unlock(&s_uart_mutex);
    return true;
}

void host_uart_test_force_next_write_fail(uart_port_t uart_num)
{
    if (!port_valid(uart_num)) {
        return;
    }
    pthread_mutex_lock(&s_uart_mutex);
    ensure_inited_locked();
    s_ports[uart_num].force_write_fail = true;
    pthread_mutex_unlock(&s_uart_mutex);
}

void host_uart_test_force_next_read_fail(uart_port_t uart_num)
{
    if (!port_valid(uart_num)) {
        return;
    }
    pthread_mutex_lock(&s_uart_mutex);
    ensure_inited_locked();
    s_ports[uart_num].force_read_fail = true;
    pthread_mutex_unlock(&s_uart_mutex);
}

void host_uart_test_reset(void)
{
    pthread_mutex_lock(&s_uart_mutex);
    for (int i = 0; i < HOST_UART_PORT_COUNT; i++) {
        s_ports[i].installed = false;
        s_ports[i].force_write_fail = false;
        s_ports[i].force_read_fail = false;
        s_ports[i].rx_head = s_ports[i].rx_count = 0;
        s_ports[i].tx_head = s_ports[i].tx_count = 0;
    }
    s_ports_inited = true;
    pthread_mutex_unlock(&s_uart_mutex);
}
