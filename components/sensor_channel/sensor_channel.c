#include "sensor_channel/sensor_channel.h"

#include <string.h>

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t rd_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr_i32(uint8_t *p, int32_t v)
{
    wr_u32(p, (uint32_t)v);
}

static int32_t rd_i32(const uint8_t *p)
{
    return (int32_t)rd_u32(p);
}

size_t sensor_channel_pack(const sensor_sample_t *in, uint8_t *out, size_t out_size)
{
    if (in == NULL || out == NULL) {
        return 0;
    }
    if (in->channel_count > SENSOR_CHANNEL_MAX_COUNT) {
        return 0;
    }
    size_t needed = SENSOR_CHANNEL_HEADER_LEN + (size_t)SENSOR_CHANNEL_ENTRY_LEN * in->channel_count;
    if (out_size < needed) {
        return 0;
    }

    /* Reject an unknown or duplicate channel type up front -- a corrupt/buggy
     * caller must not be allowed to produce a "valid-looking" frame that a
     * receiver would then also have to defend against. */
    for (uint8_t i = 0; i < in->channel_count; i++) {
        if (in->channels[i].type >= SENSOR_CHANNEL_TYPE_COUNT) {
            return 0;
        }
        for (uint8_t j = i + 1; j < in->channel_count; j++) {
            if (in->channels[i].type == in->channels[j].type) {
                return 0;
            }
        }
    }

    uint8_t *p = out;
    p[0] = SENSOR_CHANNEL_PAYLOAD_VERSION;
    memcpy(p + 1, in->module_uuid, SENSOR_UUID_LEN);
    wr_u32(p + 9, in->sample_seq);
    wr_u32(p + 13, in->uptime_ms);
    p[17] = (uint8_t)(in->sensor_bitmap >> 8);
    p[18] = (uint8_t)(in->sensor_bitmap & 0xFF);
    p[19] = in->channel_count;

    uint8_t *cp = p + SENSOR_CHANNEL_HEADER_LEN;
    for (uint8_t i = 0; i < in->channel_count; i++) {
        const sensor_channel_reading_t *ch = &in->channels[i];
        cp[0] = ch->type;
        cp[1] = ch->scale;
        cp[2] = ch->status;
        wr_i32(cp + 3, ch->value);
        cp += SENSOR_CHANNEL_ENTRY_LEN;
    }
    return needed;
}

bool sensor_channel_unpack(const uint8_t *data, uint16_t len, sensor_sample_t *out)
{
    if (data == NULL || out == NULL || len < SENSOR_CHANNEL_HEADER_LEN) {
        return false;
    }
    if (data[0] != SENSOR_CHANNEL_PAYLOAD_VERSION) {
        return false; /* unsupported version: never guess at a different layout */
    }
    uint8_t channel_count = data[19];
    if (channel_count > SENSOR_CHANNEL_MAX_COUNT) {
        return false; /* overflow: declared more channels than this format ever allows */
    }
    size_t expected_len = SENSOR_CHANNEL_HEADER_LEN + (size_t)SENSOR_CHANNEL_ENTRY_LEN * channel_count;
    if ((size_t)len != expected_len) {
        return false; /* truncated, or trailing garbage -- exact length only */
    }

    memset(out, 0, sizeof(*out));
    out->payload_version = data[0];
    memcpy(out->module_uuid, data + 1, SENSOR_UUID_LEN);
    out->sample_seq = rd_u32(data + 9);
    out->uptime_ms = rd_u32(data + 13);
    out->sensor_bitmap = (uint16_t)(((uint16_t)data[17] << 8) | data[18]);
    out->channel_count = channel_count;

    const uint8_t *cp = data + SENSOR_CHANNEL_HEADER_LEN;
    for (uint8_t i = 0; i < channel_count; i++) {
        uint8_t type = cp[0];
        if (type >= SENSOR_CHANNEL_TYPE_COUNT) {
            return false; /* unknown channel type */
        }
        for (uint8_t j = 0; j < i; j++) {
            if (out->channels[j].type == type) {
                return false; /* duplicate channel type within one sample */
            }
        }
        out->channels[i].type = type;
        out->channels[i].scale = cp[1];
        out->channels[i].status = cp[2];
        out->channels[i].value = rd_i32(cp + 3);
        cp += SENSOR_CHANNEL_ENTRY_LEN;
    }
    return true;
}
