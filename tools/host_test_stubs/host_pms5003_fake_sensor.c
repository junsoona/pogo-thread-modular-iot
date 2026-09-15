#include "host_pms5003_fake_sensor.h"

#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#include "driver/uart.h"

#define FAKE_FRAME_LEN 32u

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_thread;
static volatile bool s_running = false;
static int s_uart_num;
static bool s_active_mode;
static uint16_t s_pm1_0, s_pm2_5, s_pm10;
static uint8_t s_pending_raw[256];
static size_t s_pending_raw_len;

/* Mirrors tools/test_pms5003_parser.c's own build_frame() helper -- a valid
 * 32-byte PMS5003 output frame with a correct checksum, exactly as the real
 * sensor would emit it. Fixture data, not a reimplementation of
 * pms5003_parser.c's own decode logic. */
static void build_frame(uint8_t out[FAKE_FRAME_LEN], uint16_t pm1_0, uint16_t pm2_5, uint16_t pm10)
{
    memset(out, 0, FAKE_FRAME_LEN);
    out[0] = 0x42;
    out[1] = 0x4D;
    out[2] = 0x00;
    out[3] = 28;
    out[10] = (uint8_t)(pm1_0 >> 8);
    out[11] = (uint8_t)(pm1_0 & 0xFF);
    out[12] = (uint8_t)(pm2_5 >> 8);
    out[13] = (uint8_t)(pm2_5 & 0xFF);
    out[14] = (uint8_t)(pm10 >> 8);
    out[15] = (uint8_t)(pm10 & 0xFF);
    uint32_t sum = 0;
    for (int i = 0; i < 30; i++) {
        sum += out[i];
    }
    out[30] = (uint8_t)((sum >> 8) & 0xFF);
    out[31] = (uint8_t)(sum & 0xFF);
}

static bool take_pending_raw_locked(uint8_t *out, size_t *out_len)
{
    if (s_pending_raw_len == 0) {
        return false;
    }
    memcpy(out, s_pending_raw, s_pending_raw_len);
    *out_len = s_pending_raw_len;
    s_pending_raw_len = 0;
    return true;
}

static void push_one_reading(void)
{
    uint8_t raw[256];
    size_t raw_len = 0;
    bool have_raw;
    pthread_mutex_lock(&s_mutex);
    have_raw = take_pending_raw_locked(raw, &raw_len);
    pthread_mutex_unlock(&s_mutex);
    if (have_raw) {
        host_uart_test_push_rx(s_uart_num, raw, raw_len);
        return;
    }
    uint8_t frame[FAKE_FRAME_LEN];
    pthread_mutex_lock(&s_mutex);
    build_frame(frame, s_pm1_0, s_pm2_5, s_pm10);
    pthread_mutex_unlock(&s_mutex);
    host_uart_test_push_rx(s_uart_num, frame, FAKE_FRAME_LEN);
}

static void *fake_sensor_thread(void *arg)
{
    (void)arg;
    if (s_active_mode) {
        while (s_running) {
            push_one_reading();
            usleep(100 * 1000);
        }
        return NULL;
    }

    /* PASSIVE mode: watch the port's TX for one 7-byte PMS5003_COMMAND_LEN
     * command at a time; a READ command (byte[2]==0xE2) gets exactly one
     * frame in reply, matching real PMS5003 passive-mode behavior. SLEEP/
     * SET_MODE commands sent during pms5003_init() are simply drained here
     * (this driver never waits for a reply to those -- see write_command()'s
     * own comment in pms5003.c) and otherwise ignored. */
    uint8_t cmd[7];
    size_t have = 0;
    while (s_running) {
        uint8_t b;
        if (!host_uart_test_take_tx_byte(s_uart_num, &b, 100)) {
            continue;
        }
        cmd[have++] = b;
        if (have < sizeof(cmd)) {
            continue;
        }
        have = 0;
        if (cmd[0] == 0x42 && cmd[1] == 0x4D && cmd[2] == 0xE2) {
            push_one_reading();
        }
    }
    return NULL;
}

void host_pms5003_fake_sensor_start(int uart_num, bool active_mode, uint16_t pm1_0, uint16_t pm2_5, uint16_t pm10)
{
    host_pms5003_fake_sensor_stop();
    pthread_mutex_lock(&s_mutex);
    s_uart_num = uart_num;
    s_active_mode = active_mode;
    s_pm1_0 = pm1_0;
    s_pm2_5 = pm2_5;
    s_pm10 = pm10;
    s_pending_raw_len = 0;
    s_running = true;
    pthread_mutex_unlock(&s_mutex);
    pthread_create(&s_thread, NULL, fake_sensor_thread, NULL);
}

void host_pms5003_fake_sensor_set_values(uint16_t pm1_0, uint16_t pm2_5, uint16_t pm10)
{
    pthread_mutex_lock(&s_mutex);
    s_pm1_0 = pm1_0;
    s_pm2_5 = pm2_5;
    s_pm10 = pm10;
    pthread_mutex_unlock(&s_mutex);
}

void host_pms5003_fake_sensor_inject_raw(const uint8_t *data, size_t len)
{
    if (len > sizeof(s_pending_raw)) {
        len = sizeof(s_pending_raw);
    }
    pthread_mutex_lock(&s_mutex);
    memcpy(s_pending_raw, data, len);
    s_pending_raw_len = len;
    pthread_mutex_unlock(&s_mutex);
}

void host_pms5003_fake_sensor_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;
    pthread_join(s_thread, NULL);
}
