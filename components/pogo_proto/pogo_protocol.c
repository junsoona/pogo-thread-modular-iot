#include "pogo_proto/pogo_protocol.h"
#include <string.h>

size_t pogo_pack_identify_resp(const pogo_identify_resp_t *in, uint8_t *out, size_t out_size)
{
    if (out_size < 8) {
        return 0;
    }
    memcpy(&out[0], in->mac, 6);
    out[6] = in->fw_version;
    out[7] = in->sensor_bitmap;
    return 8;
}

bool pogo_unpack_identify_resp(const uint8_t *data, uint16_t len, pogo_identify_resp_t *out)
{
    if (len != 8) {
        return false;
    }
    memcpy(out->mac, &data[0], 6);
    out->fw_version = data[6];
    out->sensor_bitmap = data[7];
    return true;
}

size_t pogo_pack_assign_uuid(const pogo_assign_uuid_t *in, uint8_t *out, size_t out_size)
{
    if (out_size < POGO_UUID_LEN) {
        return 0;
    }
    memcpy(out, in->uuid, POGO_UUID_LEN);
    return POGO_UUID_LEN;
}

bool pogo_unpack_assign_uuid(const uint8_t *data, uint16_t len, pogo_assign_uuid_t *out)
{
    if (len != POGO_UUID_LEN) {
        return false;
    }
    memcpy(out->uuid, data, POGO_UUID_LEN);
    return true;
}

size_t pogo_pack_assign_slot(const pogo_assign_slot_t *in, uint8_t *out, size_t out_size)
{
    if (out_size < 2) {
        return 0;
    }
    out[0] = in->slot_id;
    out[1] = in->topology_depth;
    return 2;
}

bool pogo_unpack_assign_slot(const uint8_t *data, uint16_t len, pogo_assign_slot_t *out)
{
    if (len != 2) {
        return false;
    }
    out->slot_id = data[0];
    out->topology_depth = data[1];
    return true;
}

size_t pogo_pack_ack(const pogo_ack_t *in, uint8_t *out, size_t out_size)
{
    if (out_size < 1) {
        return 0;
    }
    out[0] = in->acked_type;
    return 1;
}

bool pogo_unpack_ack(const uint8_t *data, uint16_t len, pogo_ack_t *out)
{
    if (len != 1) {
        return false;
    }
    out->acked_type = data[0];
    return true;
}

size_t pogo_pack_nack(const pogo_nack_t *in, uint8_t *out, size_t out_size)
{
    if (out_size < 2) {
        return 0;
    }
    out[0] = in->acked_type;
    out[1] = in->reason;
    return 2;
}

bool pogo_unpack_nack(const uint8_t *data, uint16_t len, pogo_nack_t *out)
{
    if (len != 2) {
        return false;
    }
    out->acked_type = data[0];
    out->reason = data[1];
    return true;
}

size_t pogo_pack_join_result(const pogo_join_result_t *in, uint8_t *out, size_t out_size)
{
    if (out_size < 2) {
        return 0;
    }
    out[0] = in->success;
    out[1] = in->role;
    return 2;
}

bool pogo_unpack_join_result(const uint8_t *data, uint16_t len, pogo_join_result_t *out)
{
    if (len != 2) {
        return false;
    }
    out->success = data[0];
    out->role = data[1];
    return true;
}

size_t pogo_pack_peer_hello(const pogo_peer_hello_t *in, uint8_t *out, size_t out_size)
{
    if (out_size < 2) {
        return 0;
    }
    out[0] = in->protocol_version;
    out[1] = in->peer_type;
    return 2;
}

bool pogo_unpack_peer_hello(const uint8_t *data, uint16_t len, pogo_peer_hello_t *out)
{
    if (len != 2) {
        return false;
    }
    out->protocol_version = data[0];
    out->peer_type = data[1];
    return true;
}

size_t pogo_pack_sensor_info_ack(const pogo_sensor_info_ack_t *in, uint8_t *out, size_t out_size)
{
    if (out_size < 8) {
        return 0;
    }
    out[0] = (uint8_t)(in->sensor_type_bitmap >> 8);
    out[1] = (uint8_t)(in->sensor_type_bitmap & 0xFF);
    out[2] = (uint8_t)((in->response_time_ms >> 24) & 0xFF);
    out[3] = (uint8_t)((in->response_time_ms >> 16) & 0xFF);
    out[4] = (uint8_t)((in->response_time_ms >> 8) & 0xFF);
    out[5] = (uint8_t)(in->response_time_ms & 0xFF);
    out[6] = in->fw_version;
    out[7] = in->sensor_status;
    return 8;
}

bool pogo_unpack_sensor_info_ack(const uint8_t *data, uint16_t len, pogo_sensor_info_ack_t *out)
{
    if (len != 8) {
        return false;
    }
    out->sensor_type_bitmap = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
    out->response_time_ms = ((uint32_t)data[2] << 24) | ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 8) |
                             (uint32_t)data[5];
    out->fw_version = data[6];
    out->sensor_status = data[7];
    return true;
}

size_t pogo_pack_poll_config(const pogo_poll_config_t *in, uint8_t *out, size_t out_size)
{
    if (out_size < 4) {
        return 0;
    }
    out[0] = (uint8_t)((in->polling_period_ms >> 24) & 0xFF);
    out[1] = (uint8_t)((in->polling_period_ms >> 16) & 0xFF);
    out[2] = (uint8_t)((in->polling_period_ms >> 8) & 0xFF);
    out[3] = (uint8_t)(in->polling_period_ms & 0xFF);
    return 4;
}

bool pogo_unpack_poll_config(const uint8_t *data, uint16_t len, pogo_poll_config_t *out)
{
    if (len != 4) {
        return false;
    }
    out->polling_period_ms =
        ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | (uint32_t)data[3];
    return true;
}

size_t pogo_pack_assign_parent_info(const pogo_assign_parent_info_t *in, uint8_t *out, size_t out_size)
{
    if (out_size < POGO_UUID_LEN + 1) {
        return 0;
    }
    memcpy(out, in->parent_uuid, POGO_UUID_LEN);
    out[POGO_UUID_LEN] = in->parent_is_power_base;
    return POGO_UUID_LEN + 1;
}

bool pogo_unpack_assign_parent_info(const uint8_t *data, uint16_t len, pogo_assign_parent_info_t *out)
{
    if (len != POGO_UUID_LEN + 1) {
        return false;
    }
    memcpy(out->parent_uuid, data, POGO_UUID_LEN);
    out->parent_is_power_base = data[POGO_UUID_LEN];
    return true;
}

size_t pogo_pack_propose_uuid(const pogo_propose_uuid_t *in, uint8_t *out, size_t out_size)
{
    if (out_size < POGO_UUID_LEN) {
        return 0;
    }
    memcpy(out, in->uuid, POGO_UUID_LEN);
    return POGO_UUID_LEN;
}

bool pogo_unpack_propose_uuid(const uint8_t *data, uint16_t len, pogo_propose_uuid_t *out)
{
    if (len != POGO_UUID_LEN) {
        return false;
    }
    memcpy(out->uuid, data, POGO_UUID_LEN);
    return true;
}
