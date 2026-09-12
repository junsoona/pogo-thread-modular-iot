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
    if (out_size < 1) {
        return 0;
    }
    out[0] = in->slot_id;
    return 1;
}

bool pogo_unpack_assign_slot(const uint8_t *data, uint16_t len, pogo_assign_slot_t *out)
{
    if (len != 1) {
        return false;
    }
    out->slot_id = data[0];
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
