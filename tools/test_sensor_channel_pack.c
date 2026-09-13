/*
 * Host-compiled test for the common sensor-sample wire format
 * (components/sensor_channel/sensor_channel.c) -- section 6 of the follow-up
 * spec: "잘못된 payload, overflow, unknown channel, duplicate sample을
 * 처리하는 host test를 작성한다".
 *
 * Links against the REAL, unmodified sensor_channel.c (no ESP-IDF/FreeRTOS
 * dependency), unlike tools/test_polling_calc.c's copy-based approach --
 * sensor_channel.c has no hardware dependency at all, so there is no reason
 * to duplicate it.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/sensor_channel/include \
 *      -o /tmp/test_sensor_channel_pack tools/test_sensor_channel_pack.c \
 *      components/sensor_channel/sensor_channel.c \
 *   && /tmp/test_sensor_channel_pack
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "sensor_channel/sensor_channel.h"

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

static sensor_sample_t make_sample(void)
{
    sensor_sample_t s = {0};
    s.payload_version = SENSOR_CHANNEL_PAYLOAD_VERSION;
    for (int i = 0; i < SENSOR_UUID_LEN; i++) {
        s.module_uuid[i] = (uint8_t)(0x10 + i);
    }
    s.sample_seq = 0x01020304u;
    s.uptime_ms = 123456u;
    s.sensor_bitmap = (1u << SENSOR_CHANNEL_TEMPERATURE) | (1u << SENSOR_CHANNEL_HUMIDITY);
    s.channel_count = 2;
    s.channels[0] = (sensor_channel_reading_t){
        .type = SENSOR_CHANNEL_TEMPERATURE, .scale = SENSOR_SCALE_MILLI, .status = SENSOR_CHANNEL_STATUS_OK,
        .value = 23456}; /* 23.456 degC */
    s.channels[1] = (sensor_channel_reading_t){
        .type = SENSOR_CHANNEL_HUMIDITY, .scale = SENSOR_SCALE_MILLI, .status = SENSOR_CHANNEL_STATUS_OK,
        .value = 45678}; /* 45.678 %RH */
    return s;
}

static void test_roundtrip(void)
{
    sensor_sample_t s = make_sample();
    uint8_t buf[SENSOR_CHANNEL_MAX_WIRE_LEN];
    size_t n = sensor_channel_pack(&s, buf, sizeof(buf));
    CHECK(n == SENSOR_CHANNEL_HEADER_LEN + 2 * SENSOR_CHANNEL_ENTRY_LEN, "pack: exact expected length for 2 channels");

    sensor_sample_t out;
    bool ok = sensor_channel_unpack(buf, (uint16_t)n, &out);
    CHECK(ok, "unpack: well-formed 2-channel sample succeeds");
    CHECK(out.payload_version == s.payload_version, "unpack: payload_version roundtrips");
    CHECK(memcmp(out.module_uuid, s.module_uuid, SENSOR_UUID_LEN) == 0, "unpack: module_uuid roundtrips");
    CHECK(out.sample_seq == s.sample_seq, "unpack: sample_seq roundtrips");
    CHECK(out.uptime_ms == s.uptime_ms, "unpack: uptime_ms roundtrips");
    CHECK(out.sensor_bitmap == s.sensor_bitmap, "unpack: sensor_bitmap roundtrips");
    CHECK(out.channel_count == 2, "unpack: channel_count roundtrips");
    CHECK(out.channels[0].type == SENSOR_CHANNEL_TEMPERATURE && out.channels[0].value == 23456,
          "unpack: channel[0] (temperature) roundtrips");
    CHECK(out.channels[1].type == SENSOR_CHANNEL_HUMIDITY && out.channels[1].value == 45678,
          "unpack: channel[1] (humidity) roundtrips");
}

static void test_negative_value_roundtrip(void)
{
    /* Negative fixed-point values (e.g. sub-zero milli-degC) must survive
     * the big-endian i32 encode/decode, not just positive ones. */
    sensor_sample_t s = {0};
    s.payload_version = SENSOR_CHANNEL_PAYLOAD_VERSION;
    s.channel_count = 1;
    s.channels[0] = (sensor_channel_reading_t){
        .type = SENSOR_CHANNEL_TEMPERATURE, .scale = SENSOR_SCALE_MILLI, .status = SENSOR_CHANNEL_STATUS_OK,
        .value = -5250}; /* -5.250 degC */
    uint8_t buf[SENSOR_CHANNEL_MAX_WIRE_LEN];
    size_t n = sensor_channel_pack(&s, buf, sizeof(buf));
    sensor_sample_t out;
    bool ok = n > 0 && sensor_channel_unpack(buf, (uint16_t)n, &out);
    CHECK(ok && out.channels[0].value == -5250, "negative fixed-point channel value roundtrips");
}

static void test_zero_channels(void)
{
    sensor_sample_t s = {0};
    s.payload_version = SENSOR_CHANNEL_PAYLOAD_VERSION;
    s.channel_count = 0;
    uint8_t buf[SENSOR_CHANNEL_MAX_WIRE_LEN];
    size_t n = sensor_channel_pack(&s, buf, sizeof(buf));
    CHECK(n == SENSOR_CHANNEL_HEADER_LEN, "pack: zero-channel sample is exactly the header length");
    sensor_sample_t out;
    CHECK(sensor_channel_unpack(buf, (uint16_t)n, &out) && out.channel_count == 0,
          "unpack: zero-channel sample succeeds");
}

static void test_malformed_payload(void)
{
    sensor_sample_t out;
    uint8_t too_short[SENSOR_CHANNEL_HEADER_LEN - 1] = {0};
    CHECK(!sensor_channel_unpack(too_short, sizeof(too_short), &out), "reject: shorter than header length");

    uint8_t bad_version[SENSOR_CHANNEL_HEADER_LEN] = {0};
    bad_version[0] = SENSOR_CHANNEL_PAYLOAD_VERSION + 1;
    CHECK(!sensor_channel_unpack(bad_version, sizeof(bad_version), &out), "reject: unsupported payload_version");

    /* Header says channel_count=1 but the buffer is truncated before the
     * channel entry -- must not read past the declared length. */
    uint8_t truncated[SENSOR_CHANNEL_HEADER_LEN] = {0};
    truncated[0] = SENSOR_CHANNEL_PAYLOAD_VERSION;
    truncated[19] = 1;
    CHECK(!sensor_channel_unpack(truncated, sizeof(truncated), &out), "reject: channel_count declared but bytes missing (truncated)");

    /* Trailing garbage past the last declared channel. */
    sensor_sample_t s = make_sample();
    uint8_t buf[SENSOR_CHANNEL_MAX_WIRE_LEN + 4];
    size_t n = sensor_channel_pack(&s, buf, sizeof(buf));
    buf[n] = 0xAA;
    buf[n + 1] = 0xBB;
    CHECK(!sensor_channel_unpack(buf, (uint16_t)(n + 2), &out), "reject: trailing garbage past the last channel");
}

static void test_overflow_channel_count(void)
{
    sensor_sample_t out;
    uint8_t buf[SENSOR_CHANNEL_HEADER_LEN] = {0};
    buf[0] = SENSOR_CHANNEL_PAYLOAD_VERSION;
    buf[19] = SENSOR_CHANNEL_MAX_COUNT + 1; /* declares more channels than the format allows at all */
    CHECK(!sensor_channel_unpack(buf, sizeof(buf), &out), "reject: channel_count overflows SENSOR_CHANNEL_MAX_COUNT");

    /* pack() must refuse to build an over-count sample too, not just unpack(). */
    sensor_sample_t s = {0};
    s.payload_version = SENSOR_CHANNEL_PAYLOAD_VERSION;
    s.channel_count = SENSOR_CHANNEL_MAX_COUNT + 1;
    uint8_t out_buf[256];
    CHECK(sensor_channel_pack(&s, out_buf, sizeof(out_buf)) == 0, "pack: refuses channel_count > SENSOR_CHANNEL_MAX_COUNT");
}

static void test_unknown_channel_type(void)
{
    sensor_sample_t out;
    uint8_t buf[SENSOR_CHANNEL_HEADER_LEN + SENSOR_CHANNEL_ENTRY_LEN] = {0};
    buf[0] = SENSOR_CHANNEL_PAYLOAD_VERSION;
    buf[19] = 1;
    buf[20] = (uint8_t)SENSOR_CHANNEL_TYPE_COUNT; /* one past the last known type */
    CHECK(!sensor_channel_unpack(buf, sizeof(buf), &out), "reject: unknown channel type on unpack");

    sensor_sample_t s = {0};
    s.payload_version = SENSOR_CHANNEL_PAYLOAD_VERSION;
    s.channel_count = 1;
    s.channels[0].type = (uint8_t)SENSOR_CHANNEL_TYPE_COUNT;
    uint8_t out_buf[64];
    CHECK(sensor_channel_pack(&s, out_buf, sizeof(out_buf)) == 0, "pack: refuses unknown channel type");
}

static void test_duplicate_channel_type(void)
{
    sensor_sample_t s = {0};
    s.payload_version = SENSOR_CHANNEL_PAYLOAD_VERSION;
    s.channel_count = 2;
    s.channels[0] = (sensor_channel_reading_t){.type = SENSOR_CHANNEL_TEMPERATURE, .scale = SENSOR_SCALE_MILLI, .value = 1};
    s.channels[1] = (sensor_channel_reading_t){.type = SENSOR_CHANNEL_TEMPERATURE, .scale = SENSOR_SCALE_MILLI, .value = 2};
    uint8_t out_buf[64];
    CHECK(sensor_channel_pack(&s, out_buf, sizeof(out_buf)) == 0, "pack: refuses two channels sharing the same type");

    /* Hand-craft the same duplicate directly on the wire (bypassing pack())
     * so unpack()'s own guard is what's actually under test. */
    uint8_t buf[SENSOR_CHANNEL_HEADER_LEN + 2 * SENSOR_CHANNEL_ENTRY_LEN] = {0};
    buf[0] = SENSOR_CHANNEL_PAYLOAD_VERSION;
    buf[19] = 2;
    buf[20] = SENSOR_CHANNEL_TEMPERATURE;
    buf[27] = SENSOR_CHANNEL_TEMPERATURE;
    sensor_sample_t out;
    CHECK(!sensor_channel_unpack(buf, sizeof(buf), &out), "reject: duplicate channel type on unpack");
}

static void test_out_size_too_small(void)
{
    sensor_sample_t s = make_sample();
    uint8_t tiny[4];
    CHECK(sensor_channel_pack(&s, tiny, sizeof(tiny)) == 0, "pack: refuses an out buffer smaller than needed");
}

int main(void)
{
    test_roundtrip();
    test_negative_value_roundtrip();
    test_zero_channels();
    test_malformed_payload();
    test_overflow_channel_count();
    test_unknown_channel_type();
    test_duplicate_channel_type();
    test_out_size_too_small();

    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
