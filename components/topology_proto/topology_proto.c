#include "topology_proto/topology_proto.h"

#include <string.h>

static bool role_is_valid(uint8_t role)
{
    return role <= TOPOLOGY_ROLE_ERROR;
}

static bool report_type_is_valid(uint8_t report_type)
{
    return report_type <= TOPOLOGY_REPORT_HEARTBEAT;
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

size_t topology_report_pack(const topology_report_t *in, uint8_t *out, size_t out_size)
{
    if (in == NULL || out == NULL || out_size < TOPOLOGY_REPORT_WIRE_LEN) {
        return 0;
    }
    if (!role_is_valid(in->role) || !report_type_is_valid(in->report_type)) {
        return 0;
    }

    uint8_t *p = out;
    p[0] = TOPOLOGY_REPORT_PAYLOAD_VERSION;
    memcpy(p + 1, in->module_uuid, TOPOLOGY_UUID_LEN);
    memcpy(p + 9, in->parent_uuid, TOPOLOGY_UUID_LEN);
    p[17] = in->topology_depth;
    p[18] = in->role;
    p[19] = (uint8_t)(in->sensor_type_bitmap >> 8);
    p[20] = (uint8_t)(in->sensor_type_bitmap & 0xFF);
    wr_u32(p + 21, in->boot_session_generation);
    wr_u32(p + 25, in->sequence);
    p[29] = in->upstream_connected ? 1 : 0;
    p[30] = in->downstream_connected ? 1 : 0;
    p[31] = in->report_type;
    return TOPOLOGY_REPORT_WIRE_LEN;
}

bool topology_report_unpack(const uint8_t *data, uint16_t len, topology_report_t *out)
{
    if (data == NULL || out == NULL || len != TOPOLOGY_REPORT_WIRE_LEN) {
        return false;
    }
    if (data[0] != TOPOLOGY_REPORT_PAYLOAD_VERSION) {
        return false; /* unsupported version: never guess at a different layout */
    }
    if (!role_is_valid(data[18]) || !report_type_is_valid(data[31])) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->payload_version = data[0];
    memcpy(out->module_uuid, data + 1, TOPOLOGY_UUID_LEN);
    memcpy(out->parent_uuid, data + 9, TOPOLOGY_UUID_LEN);
    out->topology_depth = data[17];
    out->role = data[18];
    out->sensor_type_bitmap = (uint16_t)(((uint16_t)data[19] << 8) | data[20]);
    out->boot_session_generation = rd_u32(data + 21);
    out->sequence = rd_u32(data + 25);
    out->upstream_connected = data[29] != 0;
    out->downstream_connected = data[30] != 0;
    out->report_type = data[31];
    return true;
}
