#include "bridge_export_proto/bridge_export_proto.h"

#include <string.h>

#include "pogo_proto/pogo_crc16.h"
#include "sensor_channel/sensor_channel.h"

_Static_assert(BRIDGE_EXPORT_SAMPLE_MAX_SENSOR_CHANNEL_LEN == SENSOR_CHANNEL_MAX_WIRE_LEN,
               "bridge_export_proto's embedded-sensor-frame bound must track sensor_channel's own wire max");

static void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

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

/* Builds a complete envelope (header + payload + CRC) into `out`. Shared by
 * every bridge_export_pack_*() below -- the only place that ever writes the
 * envelope header/CRC bytes. */
static size_t build_envelope(bridge_export_msg_type_t type, const uint8_t *payload, uint16_t payload_len,
                              uint8_t *out, size_t out_size)
{
    if (out == NULL || (payload == NULL && payload_len != 0)) {
        return 0;
    }
    size_t total = (size_t)BRIDGE_EXPORT_ENVELOPE_HEADER_LEN + payload_len + BRIDGE_EXPORT_CRC_LEN;
    if (total > BRIDGE_EXPORT_MAX_MESSAGE_LEN || out_size < total) {
        return 0;
    }
    out[0] = (uint8_t)BRIDGE_EXPORT_PROTO_VERSION;
    out[1] = (uint8_t)type;
    wr_u16(out + 2, payload_len);
    if (payload_len != 0) {
        memcpy(out + BRIDGE_EXPORT_ENVELOPE_HEADER_LEN, payload, payload_len);
    }
    uint16_t crc = pogo_crc16_ccitt(out, BRIDGE_EXPORT_ENVELOPE_HEADER_LEN + payload_len);
    wr_u16(out + BRIDGE_EXPORT_ENVELOPE_HEADER_LEN + payload_len, crc);
    return total;
}

size_t bridge_export_pack_sample(const bridge_export_sample_t *in, uint8_t *out, size_t out_size)
{
    if (in == NULL) {
        return 0;
    }
    if (in->sensor_channel_len == 0 || in->sensor_channel_len > BRIDGE_EXPORT_SAMPLE_MAX_SENSOR_CHANNEL_LEN) {
        return 0;
    }
    uint8_t payload[BRIDGE_EXPORT_SAMPLE_MAX_PAYLOAD_LEN];
    memcpy(payload, in->power_base_uuid, BRIDGE_EXPORT_UUID_LEN);
    wr_u16(payload + BRIDGE_EXPORT_UUID_LEN, in->sensor_channel_len);
    memcpy(payload + BRIDGE_EXPORT_UUID_LEN + 2, in->sensor_channel_payload, in->sensor_channel_len);
    uint16_t payload_len = (uint16_t)(BRIDGE_EXPORT_UUID_LEN + 2 + in->sensor_channel_len);
    return build_envelope(BRIDGE_EXPORT_MSG_SAMPLE, payload, payload_len, out, out_size);
}

size_t bridge_export_pack_topology_node(const bridge_export_topology_node_t *in, uint8_t *out, size_t out_size)
{
    if (in == NULL) {
        return 0;
    }
    uint8_t payload[BRIDGE_EXPORT_TOPOLOGY_PAYLOAD_LEN];
    memcpy(payload + 0, in->power_base_uuid, BRIDGE_EXPORT_UUID_LEN);
    memcpy(payload + 8, in->module_uuid, BRIDGE_EXPORT_UUID_LEN);
    memcpy(payload + 16, in->parent_uuid, BRIDGE_EXPORT_UUID_LEN);
    payload[24] = in->online ? 1 : 0;
    payload[25] = in->depth;
    payload[26] = in->role;
    wr_u16(payload + 27, in->sensor_bitmap);
    payload[29] = in->downstream_connected ? 1 : 0;
    wr_u32(payload + 30, in->boot_session_generation);
    wr_u32(payload + 34, in->last_seq);
    return build_envelope(BRIDGE_EXPORT_MSG_TOPOLOGY_SNAPSHOT, payload, BRIDGE_EXPORT_TOPOLOGY_PAYLOAD_LEN, out,
                           out_size);
}

size_t bridge_export_pack_heartbeat(const bridge_export_heartbeat_t *in, uint8_t *out, size_t out_size)
{
    if (in == NULL) {
        return 0;
    }
    uint8_t payload[BRIDGE_EXPORT_HEARTBEAT_PAYLOAD_LEN];
    memcpy(payload + 0, in->power_base_uuid, BRIDGE_EXPORT_UUID_LEN);
    wr_u32(payload + 8, in->uptime_ms);
    wr_u16(payload + 12, in->node_count);
    return build_envelope(BRIDGE_EXPORT_MSG_HEARTBEAT, payload, BRIDGE_EXPORT_HEARTBEAT_PAYLOAD_LEN, out, out_size);
}

bool bridge_export_unpack_envelope(const uint8_t *data, size_t len, bridge_export_msg_type_t *out_type,
                                    const uint8_t **out_payload, uint16_t *out_payload_len)
{
    if (data == NULL || out_type == NULL || out_payload == NULL || out_payload_len == NULL) {
        return false;
    }
    if (len < (size_t)BRIDGE_EXPORT_ENVELOPE_HEADER_LEN + BRIDGE_EXPORT_CRC_LEN) {
        return false; /* truncated: not even a full empty-payload envelope */
    }
    if (len > BRIDGE_EXPORT_MAX_MESSAGE_LEN) {
        return false;
    }
    if (data[0] != (uint8_t)BRIDGE_EXPORT_PROTO_VERSION) {
        return false; /* unsupported version: never guess at a different layout */
    }
    uint8_t type = data[1];
    if (type != BRIDGE_EXPORT_MSG_SAMPLE && type != BRIDGE_EXPORT_MSG_TOPOLOGY_SNAPSHOT &&
        type != BRIDGE_EXPORT_MSG_HEARTBEAT) {
        return false;
    }
    uint16_t payload_len = rd_u16(data + 2);
    size_t expected_total = (size_t)BRIDGE_EXPORT_ENVELOPE_HEADER_LEN + payload_len + BRIDGE_EXPORT_CRC_LEN;
    if (expected_total != len) {
        return false; /* truncated, or trailing garbage -- exact length only */
    }
    uint16_t stored_crc = rd_u16(data + BRIDGE_EXPORT_ENVELOPE_HEADER_LEN + payload_len);
    uint16_t computed_crc = pogo_crc16_ccitt(data, BRIDGE_EXPORT_ENVELOPE_HEADER_LEN + payload_len);
    if (stored_crc != computed_crc) {
        return false;
    }
    *out_type = (bridge_export_msg_type_t)type;
    *out_payload = data + BRIDGE_EXPORT_ENVELOPE_HEADER_LEN;
    *out_payload_len = payload_len;
    return true;
}

bool bridge_export_unpack_sample(const uint8_t *payload, uint16_t payload_len, bridge_export_sample_t *out)
{
    if (payload == NULL || out == NULL) {
        return false;
    }
    if (payload_len < BRIDGE_EXPORT_UUID_LEN + 2) {
        return false;
    }
    uint16_t sc_len = rd_u16(payload + BRIDGE_EXPORT_UUID_LEN);
    if (sc_len == 0 || sc_len > BRIDGE_EXPORT_SAMPLE_MAX_SENSOR_CHANNEL_LEN) {
        return false;
    }
    if ((size_t)payload_len != (size_t)BRIDGE_EXPORT_UUID_LEN + 2 + sc_len) {
        return false; /* exact length only */
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->power_base_uuid, payload, BRIDGE_EXPORT_UUID_LEN);
    memcpy(out->sensor_channel_payload, payload + BRIDGE_EXPORT_UUID_LEN + 2, sc_len);
    out->sensor_channel_len = sc_len;

    /* The embedded frame must itself be a valid, unmodified sensor_channel
     * frame -- an invalid inner frame makes the whole SAMPLE message
     * malformed (see this component's header comment). The parsed result is
     * discarded here on purpose: interpreting channel values is the
     * aggregator/registry's job, not this wire layer's (same "network layer
     * never interprets the payload" policy as sensor_channel.h itself). */
    sensor_sample_t discard;
    if (!sensor_channel_unpack(out->sensor_channel_payload, sc_len, &discard)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

bool bridge_export_unpack_topology_node(const uint8_t *payload, uint16_t payload_len,
                                         bridge_export_topology_node_t *out)
{
    if (payload == NULL || out == NULL) {
        return false;
    }
    if (payload_len != BRIDGE_EXPORT_TOPOLOGY_PAYLOAD_LEN) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->power_base_uuid, payload + 0, BRIDGE_EXPORT_UUID_LEN);
    memcpy(out->module_uuid, payload + 8, BRIDGE_EXPORT_UUID_LEN);
    memcpy(out->parent_uuid, payload + 16, BRIDGE_EXPORT_UUID_LEN);
    out->online = payload[24] != 0;
    out->depth = payload[25];
    out->role = payload[26];
    out->sensor_bitmap = rd_u16(payload + 27);
    out->downstream_connected = payload[29] != 0;
    out->boot_session_generation = rd_u32(payload + 30);
    out->last_seq = rd_u32(payload + 34);
    return true;
}

bool bridge_export_unpack_heartbeat(const uint8_t *payload, uint16_t payload_len, bridge_export_heartbeat_t *out)
{
    if (payload == NULL || out == NULL) {
        return false;
    }
    if (payload_len != BRIDGE_EXPORT_HEARTBEAT_PAYLOAD_LEN) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->power_base_uuid, payload + 0, BRIDGE_EXPORT_UUID_LEN);
    out->uptime_ms = rd_u32(payload + 8);
    out->node_count = rd_u16(payload + 12);
    return true;
}
