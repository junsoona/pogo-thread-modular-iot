/*
 * Host-compiled test for components/bridge_export_proto/bridge_export_proto.c
 * (the Power Base -> Linux OTBR host northbound bridge wire format, this
 * follow-up). Links against the REAL, unmodified bridge_export_proto.c,
 * sensor_channel.c (the embedded SAMPLE body's real wire format) and
 * pogo_crc16.c (the reused CRC16 implementation) -- no mirrored logic.
 *
 * Covers: round-trip pack/unpack for all four message types, endian
 * correctness (spot-checked byte offsets), CRC-mismatch rejection, wrong
 * protocol version rejection, unknown msg_type rejection, truncated and
 * trailing-garbage payload rejection, and that a SAMPLE message's embedded
 * bytes are byte-for-byte the real sensor_channel_pack() output (never
 * reformatted).
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/bridge_export_proto/include \
 *      -I components/sensor_channel/include -I components/pogo_proto/include \
 *      -o /tmp/test_bridge_export_proto tools/test_bridge_export_proto.c \
 *      components/bridge_export_proto/bridge_export_proto.c \
 *      components/sensor_channel/sensor_channel.c components/pogo_proto/pogo_crc16.c \
 *   && /tmp/test_bridge_export_proto
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "bridge_export_proto/bridge_export_proto.h"
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

static void make_sensor_sample(sensor_sample_t *s, uint8_t uuid_last_byte, uint32_t seq)
{
    memset(s, 0, sizeof(*s));
    s->payload_version = SENSOR_CHANNEL_PAYLOAD_VERSION;
    memset(s->module_uuid, 0xAB, SENSOR_UUID_LEN);
    s->module_uuid[7] = uuid_last_byte;
    s->sample_seq = seq;
    s->uptime_ms = 12345;
    s->sensor_bitmap = 0x0003;
    s->channel_count = 2;
    s->channels[0] = (sensor_channel_reading_t){.type = SENSOR_CHANNEL_TEMPERATURE, .scale = SENSOR_SCALE_MILLI,
                                                  .status = SENSOR_CHANNEL_STATUS_OK, .value = 23456};
    s->channels[1] = (sensor_channel_reading_t){.type = SENSOR_CHANNEL_HUMIDITY, .scale = SENSOR_SCALE_MILLI,
                                                  .status = SENSOR_CHANNEL_STATUS_OK, .value = 45678};
}

int main(void)
{
    printf("\n-- scenario: SAMPLE round-trip preserves the embedded sensor_channel frame byte-for-byte --\n");
    {
        sensor_sample_t s;
        make_sensor_sample(&s, 0x01, 7);
        uint8_t sc_buf[SENSOR_CHANNEL_MAX_WIRE_LEN];
        size_t sc_len = sensor_channel_pack(&s, sc_buf, sizeof(sc_buf));
        CHECK(sc_len > 0, "sensor_channel_pack succeeds for the fixture sample");

        bridge_export_sample_t in = {0};
        memset(in.power_base_uuid, 0xCD, BRIDGE_EXPORT_UUID_LEN);
        memcpy(in.sensor_channel_payload, sc_buf, sc_len);
        in.sensor_channel_len = (uint16_t)sc_len;

        uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
        size_t wlen = bridge_export_pack_sample(&in, wire, sizeof(wire));
        CHECK(wlen > 0, "bridge_export_pack_sample succeeds");
        CHECK(wlen == BRIDGE_EXPORT_ENVELOPE_HEADER_LEN + BRIDGE_EXPORT_UUID_LEN + 2 + sc_len + BRIDGE_EXPORT_CRC_LEN,
              "packed length matches header+uuid+len+frame+crc exactly");
        CHECK(wire[0] == BRIDGE_EXPORT_PROTO_VERSION, "version byte is correct");
        CHECK(wire[1] == BRIDGE_EXPORT_MSG_SAMPLE, "msg_type byte is SAMPLE");

        bridge_export_msg_type_t type;
        const uint8_t *payload;
        uint16_t payload_len;
        CHECK(bridge_export_unpack_envelope(wire, wlen, &type, &payload, &payload_len), "envelope unpacks OK");
        CHECK(type == BRIDGE_EXPORT_MSG_SAMPLE, "unpacked type is SAMPLE");

        bridge_export_sample_t out;
        CHECK(bridge_export_unpack_sample(payload, payload_len, &out), "sample payload unpacks OK");
        CHECK(memcmp(out.power_base_uuid, in.power_base_uuid, BRIDGE_EXPORT_UUID_LEN) == 0,
              "power_base_uuid round-trips exactly");
        CHECK(out.sensor_channel_len == sc_len, "sensor_channel_len round-trips exactly");
        CHECK(memcmp(out.sensor_channel_payload, sc_buf, sc_len) == 0,
              "embedded sensor_channel bytes are byte-for-byte identical to sensor_channel_pack()'s own output");

        sensor_sample_t reparsed;
        CHECK(sensor_channel_unpack(out.sensor_channel_payload, out.sensor_channel_len, &reparsed),
              "the embedded frame is still a valid sensor_channel frame after the round-trip");
        CHECK(reparsed.sample_seq == 7 && reparsed.channel_count == 2,
              "reparsed sensor_channel fields match the original sample");
    }

    printf("\n-- scenario: TOPOLOGY_SNAPSHOT round-trip, one node per message --\n");
    {
        bridge_export_topology_node_t in = {0};
        memset(in.power_base_uuid, 0x11, BRIDGE_EXPORT_UUID_LEN);
        memset(in.module_uuid, 0x22, BRIDGE_EXPORT_UUID_LEN);
        memset(in.parent_uuid, 0x33, BRIDGE_EXPORT_UUID_LEN);
        in.online = true;
        in.depth = 2;
        in.role = 3; /* TOPOLOGY_ROLE_LEAF numeric value */
        in.sensor_bitmap = 0x0201;
        in.downstream_connected = false;
        in.boot_session_generation = 0xAABBCCDD;
        in.last_seq = 42;

        uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
        size_t wlen = bridge_export_pack_topology_node(&in, wire, sizeof(wire));
        CHECK(wlen == BRIDGE_EXPORT_ENVELOPE_HEADER_LEN + BRIDGE_EXPORT_TOPOLOGY_PAYLOAD_LEN + BRIDGE_EXPORT_CRC_LEN,
              "packed TOPOLOGY_SNAPSHOT length is exactly header+fixed-payload+crc");

        /* Endian spot-check: boot_session_generation is at payload offset 30
         * (envelope header is 4 bytes before the payload starts). */
        size_t gen_off = BRIDGE_EXPORT_ENVELOPE_HEADER_LEN + 30;
        CHECK(wire[gen_off] == 0xAA && wire[gen_off + 1] == 0xBB && wire[gen_off + 2] == 0xCC &&
                  wire[gen_off + 3] == 0xDD,
              "boot_session_generation is written big-endian at the documented offset");

        bridge_export_msg_type_t type;
        const uint8_t *payload;
        uint16_t payload_len;
        CHECK(bridge_export_unpack_envelope(wire, wlen, &type, &payload, &payload_len), "envelope unpacks OK");
        CHECK(type == BRIDGE_EXPORT_MSG_TOPOLOGY_SNAPSHOT, "unpacked type is TOPOLOGY_SNAPSHOT");

        bridge_export_topology_node_t out;
        CHECK(bridge_export_unpack_topology_node(payload, payload_len, &out), "topology node payload unpacks OK");
        CHECK(memcmp(&out, &in, sizeof(in)) == 0, "every topology node field round-trips exactly");
    }

    printf("\n-- scenario: HEARTBEAT round-trip --\n");
    {
        bridge_export_heartbeat_t in = {0};
        memset(in.power_base_uuid, 0x44, BRIDGE_EXPORT_UUID_LEN);
        in.uptime_ms = 999888;
        in.node_count = 5;

        uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
        size_t wlen = bridge_export_pack_heartbeat(&in, wire, sizeof(wire));
        CHECK(wlen == BRIDGE_EXPORT_ENVELOPE_HEADER_LEN + BRIDGE_EXPORT_HEARTBEAT_PAYLOAD_LEN + BRIDGE_EXPORT_CRC_LEN,
              "packed HEARTBEAT length is exactly header+fixed-payload+crc");

        bridge_export_msg_type_t type;
        const uint8_t *payload;
        uint16_t payload_len;
        CHECK(bridge_export_unpack_envelope(wire, wlen, &type, &payload, &payload_len), "envelope unpacks OK");
        CHECK(type == BRIDGE_EXPORT_MSG_HEARTBEAT, "unpacked type is HEARTBEAT");

        bridge_export_heartbeat_t out;
        CHECK(bridge_export_unpack_heartbeat(payload, payload_len, &out), "heartbeat payload unpacks OK");
        CHECK(memcmp(&out, &in, sizeof(in)) == 0, "every heartbeat field round-trips exactly");
    }

    printf("\n-- scenario: TOPOLOGY_REMOVED round-trip (this follow-up: a confirmed-detach tombstone, distinct from "
           "an ordinary online=false TOPOLOGY_SNAPSHOT) --\n");
    {
        bridge_export_topology_removed_t in = {0};
        memset(in.power_base_uuid, 0x77, BRIDGE_EXPORT_UUID_LEN);
        memset(in.module_uuid, 0x88, BRIDGE_EXPORT_UUID_LEN);

        uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
        size_t wlen = bridge_export_pack_topology_removed(&in, wire, sizeof(wire));
        CHECK(wlen == BRIDGE_EXPORT_ENVELOPE_HEADER_LEN + BRIDGE_EXPORT_TOPOLOGY_REMOVED_PAYLOAD_LEN +
                          BRIDGE_EXPORT_CRC_LEN,
              "packed TOPOLOGY_REMOVED length is exactly header+fixed-payload+crc");
        CHECK(wire[1] == BRIDGE_EXPORT_MSG_TOPOLOGY_REMOVED, "msg_type byte is TOPOLOGY_REMOVED (4)");

        bridge_export_msg_type_t type;
        const uint8_t *payload;
        uint16_t payload_len;
        CHECK(bridge_export_unpack_envelope(wire, wlen, &type, &payload, &payload_len), "envelope unpacks OK");
        CHECK(type == BRIDGE_EXPORT_MSG_TOPOLOGY_REMOVED, "unpacked type is TOPOLOGY_REMOVED");

        bridge_export_topology_removed_t out;
        CHECK(bridge_export_unpack_topology_removed(payload, payload_len, &out), "removed payload unpacks OK");
        CHECK(memcmp(&out, &in, sizeof(in)) == 0, "every removed field round-trips exactly");

        CHECK(!bridge_export_unpack_topology_removed(payload, (uint16_t)(payload_len - 1), &out),
              "a short (truncated) TOPOLOGY_REMOVED payload is rejected");

        /* Version-compatibility check (this follow-up's own explicit
         * requirement: an OLD ingest program must keep parsing every
         * pre-existing msg_type unchanged) -- a decoder that only knows
         * msg_type 1..3 would reject this NEW msg_type 4 outright at the
         * envelope layer, never misinterpret it as something else; the
         * three OLD message types above are unaffected by this addition
         * (still exactly the same wire layout, verified by their own
         * round-trip scenarios above). */
        uint8_t old_bad_type[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
        memcpy(old_bad_type, wire, wlen);
        old_bad_type[1] = 0x7E; /* simulate an even-newer, still-unknown msg_type */
        CHECK(!bridge_export_unpack_envelope(old_bad_type, wlen, &type, &payload, &payload_len),
              "a still-unknown future msg_type is rejected, never guessed at");
    }

    printf("\n-- scenario: malformed-envelope rejection --\n");
    {
        bridge_export_heartbeat_t in = {0};
        memset(in.power_base_uuid, 0x55, BRIDGE_EXPORT_UUID_LEN);
        in.uptime_ms = 1;
        in.node_count = 1;
        uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
        size_t wlen = bridge_export_pack_heartbeat(&in, wire, sizeof(wire));

        bridge_export_msg_type_t type;
        const uint8_t *payload;
        uint16_t payload_len;

        uint8_t corrupted_crc[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
        memcpy(corrupted_crc, wire, wlen);
        corrupted_crc[wlen - 1] ^= 0xFF;
        CHECK(!bridge_export_unpack_envelope(corrupted_crc, wlen, &type, &payload, &payload_len),
              "a flipped CRC byte is rejected");

        uint8_t bad_version[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
        memcpy(bad_version, wire, wlen);
        bad_version[0] = BRIDGE_EXPORT_PROTO_VERSION + 1;
        CHECK(!bridge_export_unpack_envelope(bad_version, wlen, &type, &payload, &payload_len),
              "an unsupported protocol_version is rejected");

        uint8_t bad_type[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
        memcpy(bad_type, wire, wlen);
        bad_type[1] = 0x7F; /* not SAMPLE/TOPOLOGY_SNAPSHOT/HEARTBEAT */
        CHECK(!bridge_export_unpack_envelope(bad_type, wlen, &type, &payload, &payload_len),
              "an unknown msg_type is rejected");

        CHECK(!bridge_export_unpack_envelope(wire, wlen - 1, &type, &payload, &payload_len),
              "a truncated message (one byte short) is rejected");

        uint8_t trailing[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
        memcpy(trailing, wire, wlen);
        trailing[wlen] = 0x00; /* one stray trailing byte past the declared length */
        CHECK(!bridge_export_unpack_envelope(trailing, wlen + 1, &type, &payload, &payload_len),
              "one trailing garbage byte past the declared payload_len is rejected");

        CHECK(!bridge_export_unpack_envelope(wire, BRIDGE_EXPORT_ENVELOPE_HEADER_LEN + BRIDGE_EXPORT_CRC_LEN - 1,
                                              &type, &payload, &payload_len),
              "a length below the minimum possible envelope size is rejected");

        uint8_t oversize[BRIDGE_EXPORT_MAX_MESSAGE_LEN + 16];
        memset(oversize, 0, sizeof(oversize));
        CHECK(!bridge_export_unpack_envelope(oversize, sizeof(oversize), &type, &payload, &payload_len),
              "a length above BRIDGE_EXPORT_MAX_MESSAGE_LEN is rejected outright");
    }

    printf("\n-- scenario: a SAMPLE message with a corrupted embedded sensor_channel frame is rejected as a whole "
           "--\n");
    {
        sensor_sample_t s;
        make_sensor_sample(&s, 0x02, 1);
        uint8_t sc_buf[SENSOR_CHANNEL_MAX_WIRE_LEN];
        size_t sc_len = sensor_channel_pack(&s, sc_buf, sizeof(sc_buf));

        bridge_export_sample_t in = {0};
        memset(in.power_base_uuid, 0x66, BRIDGE_EXPORT_UUID_LEN);
        memcpy(in.sensor_channel_payload, sc_buf, sc_len);
        /* Corrupt the embedded frame's channel type byte to an out-of-range
         * value (sensor_channel_unpack() rejects any type >=
         * SENSOR_CHANNEL_TYPE_COUNT) -- this is done BEFORE packing so the
         * outer CRC is still self-consistent, isolating the test to the
         * inner-frame validation path specifically. */
        in.sensor_channel_payload[SENSOR_CHANNEL_HEADER_LEN] = 0xFF;
        in.sensor_channel_len = (uint16_t)sc_len;

        uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
        size_t wlen = bridge_export_pack_sample(&in, wire, sizeof(wire));
        CHECK(wlen > 0, "packing with a pre-corrupted inner frame still succeeds (this layer doesn't validate on "
                        "the way out)");

        bridge_export_msg_type_t type;
        const uint8_t *payload;
        uint16_t payload_len;
        CHECK(bridge_export_unpack_envelope(wire, wlen, &type, &payload, &payload_len),
              "the outer envelope (correct CRC) still unpacks OK");
        bridge_export_sample_t out;
        CHECK(!bridge_export_unpack_sample(payload, payload_len, &out),
              "but the SAMPLE payload itself is rejected once the embedded sensor_channel frame is invalid");
    }

    printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
