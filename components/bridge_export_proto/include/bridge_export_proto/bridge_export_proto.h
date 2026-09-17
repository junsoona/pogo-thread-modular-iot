/*
 * Northbound Power Base -> Linux OTBR host wire protocol (this follow-up:
 * "Matter endpoint를 바로 만들기 전에 Power Base의 aggregator 데이터를 Linux
 * OTBR 호스트로 전달하는 northbound bridge transport"). Pure, dependency-free
 * C -- no ESP-IDF/FreeRTOS, no OpenThread -- so it links unmodified into
 * THREE different binaries: components/linux_bridge_exporter (the ESP32
 * power base's sending side), tools/test_bridge_export_proto.c (a host
 * test), and linux/matter_bridge/ingest (the Linux receiving side, compiled
 * as plain C from a C++ program via an `extern "C"` include -- see that
 * program's own README).
 *
 * This is a NEW, separate wire format -- it does not touch or reinterpret
 * components/pogo_proto (wired provisioning), components/sensor_channel
 * (left completely unmodified: a SAMPLE message's sensor body is the exact,
 * unmodified byte output of sensor_channel_pack(), copied in and out
 * opaquely -- see bridge_export_sample_t below), or components/topology_proto
 * (topology_role_t's numeric values are reused for the wire `role` byte
 * here, but this component never includes or calls into topology_proto.c).
 *
 * Envelope (every message on the wire starts with this, all multi-byte
 * integers big-endian, matching every other wire format in this codebase):
 *
 *   0   1  protocol_version (must equal BRIDGE_EXPORT_PROTO_VERSION)
 *   1   1  msg_type (one of bridge_export_msg_type_t)
 *   2   2  payload_len (u16 BE) -- exact byte count of the payload that follows
 *   4   N  payload (message-type-specific, see below)
 *   4+N 2  crc16 (u16 BE, CRC16/CCITT-FALSE via components/pogo_proto's
 *           pogo_crc16_ccitt() -- reused, not reimplemented, same integrity
 *           scheme components/power_base_config already uses) over bytes
 *           [0, 4+N) -- i.e. header+payload, not including itself
 *
 * Total message length = 4 + N + 2. bridge_export_unpack_envelope() rejects
 * anything else outright: wrong version, unknown msg_type, a payload_len
 * that does not exactly account for every remaining byte (no truncation, no
 * trailing garbage), a length below the minimum envelope size, a length
 * above BRIDGE_EXPORT_MAX_MESSAGE_LEN, or a CRC mismatch. Never a bare
 * memcpy of a C struct onto the wire -- same policy as sensor_channel.h and
 * topology_proto.h's own top comments.
 *
 * BRIDGE_EXPORT_MAX_MESSAGE_LEN is kept comfortably below the IPv6 minimum
 * MTU (1280 bytes, RFC 8200) even after CoAP/UDP/IPv6 header overhead --
 * every message type this component defines is well under 128 bytes, so
 * this never needs fragmentation and a single CoAP resource never carries a
 * whole 16-node topology table in one packet (see
 * bridge_export_topology_node_t: one node per message, sent as a bounded,
 * safely-repeatable chunk, never a single oversized burst).
 *
 * SAMPLE payload (BRIDGE_EXPORT_UUID_LEN + 2 + sensor_channel_len bytes):
 *   0   8  power_base_uuid  -- which power base this sample transited (the
 *                               dedup key on the Linux side is
 *                               (power_base_uuid, module_uuid, sample_seq);
 *                               module_uuid/sample_seq are already inside
 *                               the embedded sensor_channel frame below)
 *   8   2  sensor_channel_len (u16 BE)
 *   10  N  the EXACT, unmodified output of sensor_channel_pack() -- this
 *           component only copies these bytes in and validates them with
 *           the real sensor_channel_unpack() (never reinterprets or
 *           reformats them itself)
 *
 * TOPOLOGY_SNAPSHOT payload (BRIDGE_EXPORT_TOPOLOGY_PAYLOAD_LEN bytes, fixed,
 * ONE topology_registry node per message -- never the whole table at once):
 *   0   8  power_base_uuid
 *   8   8  module_uuid
 *   16  8  parent_uuid
 *   24  1  online (u8 bool)
 *   25  1  depth
 *   26  1  role (one of topology_role_t's numeric values, see topology_proto.h)
 *   27  2  sensor_bitmap (u16 BE)
 *   29  1  downstream_connected (u8 bool)
 *   30  4  boot_session_generation (u32 BE) -- this node's current session
 *           token, carried forward unmodified from topology_registry_entry_t;
 *           together with last_seq below this IS "the current registry's own
 *           session identifier" the follow-up spec asks for (topology_registry
 *           already treats generation as an opaque per-boot token -- see that
 *           component's own header comment -- so this component does too)
 *   34  4  last_seq (u32 BE) -- this node's last-accepted report sequence
 *           within boot_session_generation
 *   = 38 bytes
 *
 * HEARTBEAT payload (BRIDGE_EXPORT_HEARTBEAT_PAYLOAD_LEN bytes, fixed):
 *   0   8  power_base_uuid
 *   8   4  uptime_ms (u32 BE) -- this power base's own monotonic uptime
 *   12  2  node_count (u16 BE) -- topology_registry_count() at send time
 *           (informational liveness signal only, never authoritative --
 *           the periodic TOPOLOGY_SNAPSHOT resync is what actually
 *           reconciles Linux-side state)
 *   = 14 bytes
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BRIDGE_EXPORT_PROTO_VERSION 1u
#define BRIDGE_EXPORT_UUID_LEN 8u

typedef enum {
    BRIDGE_EXPORT_MSG_SAMPLE = 1,
    BRIDGE_EXPORT_MSG_TOPOLOGY_SNAPSHOT = 2,
    BRIDGE_EXPORT_MSG_HEARTBEAT = 3,
} bridge_export_msg_type_t;

#define BRIDGE_EXPORT_ENVELOPE_HEADER_LEN 4u
#define BRIDGE_EXPORT_CRC_LEN 2u

/* Must match components/sensor_channel/include/sensor_channel/sensor_channel.h's
 * SENSOR_CHANNEL_MAX_WIRE_LEN -- checked by a _Static_assert in the .c file
 * so the two can never silently drift apart. */
#define BRIDGE_EXPORT_SAMPLE_MAX_SENSOR_CHANNEL_LEN 90u
#define BRIDGE_EXPORT_SAMPLE_MAX_PAYLOAD_LEN \
    (BRIDGE_EXPORT_UUID_LEN + 2u + BRIDGE_EXPORT_SAMPLE_MAX_SENSOR_CHANNEL_LEN)

#define BRIDGE_EXPORT_TOPOLOGY_PAYLOAD_LEN 38u
#define BRIDGE_EXPORT_HEARTBEAT_PAYLOAD_LEN 14u

/* Comfortably below the IPv6 minimum MTU (1280 bytes) even after every
 * CoAP/UDP/IPv6 header -- see this header's top comment. */
#define BRIDGE_EXPORT_MAX_MESSAGE_LEN 256u
#define BRIDGE_EXPORT_MAX_TOTAL_LEN \
    (BRIDGE_EXPORT_ENVELOPE_HEADER_LEN + BRIDGE_EXPORT_SAMPLE_MAX_PAYLOAD_LEN + BRIDGE_EXPORT_CRC_LEN)

typedef struct {
    uint8_t  power_base_uuid[BRIDGE_EXPORT_UUID_LEN];
    uint8_t  sensor_channel_payload[BRIDGE_EXPORT_SAMPLE_MAX_SENSOR_CHANNEL_LEN];
    uint16_t sensor_channel_len;
} bridge_export_sample_t;

typedef struct {
    uint8_t  power_base_uuid[BRIDGE_EXPORT_UUID_LEN];
    uint8_t  module_uuid[BRIDGE_EXPORT_UUID_LEN];
    uint8_t  parent_uuid[BRIDGE_EXPORT_UUID_LEN];
    bool     online;
    uint8_t  depth;
    uint8_t  role;
    uint16_t sensor_bitmap;
    bool     downstream_connected;
    uint32_t boot_session_generation;
    uint32_t last_seq;
} bridge_export_topology_node_t;

typedef struct {
    uint8_t  power_base_uuid[BRIDGE_EXPORT_UUID_LEN];
    uint32_t uptime_ms;
    uint16_t node_count;
} bridge_export_heartbeat_t;

/*
 * Each pack function serializes `in` as a complete, CRC-covered envelope
 * into `out` (capacity out_size). Returns the total number of bytes
 * written, or 0 on any of: `in`/`out` NULL, out_size too small, or (SAMPLE
 * only) sensor_channel_len == 0 or > BRIDGE_EXPORT_SAMPLE_MAX_SENSOR_CHANNEL_LEN.
 * Never partially writes `out` on failure.
 */
size_t bridge_export_pack_sample(const bridge_export_sample_t *in, uint8_t *out, size_t out_size);
size_t bridge_export_pack_topology_node(const bridge_export_topology_node_t *in, uint8_t *out, size_t out_size);
size_t bridge_export_pack_heartbeat(const bridge_export_heartbeat_t *in, uint8_t *out, size_t out_size);

/*
 * Validates and strips the envelope only -- see this header's top comment
 * for the exact rejection list (bad version, unknown type, length mismatch,
 * over-length, CRC mismatch). On success, *out_type is set and *out_payload/
 * *out_payload_len point INTO `data` (valid only as long as `data` is) --
 * pass them straight to the matching bridge_export_unpack_*() below.
 */
bool bridge_export_unpack_envelope(const uint8_t *data, size_t len, bridge_export_msg_type_t *out_type,
                                    const uint8_t **out_payload, uint16_t *out_payload_len);

/*
 * Parses one payload already extracted by bridge_export_unpack_envelope().
 * Returns false for a payload_len that does not exactly match this message
 * type's expected size (SAMPLE: variable, but exactly
 * BRIDGE_EXPORT_UUID_LEN+2+sensor_channel_len for the length byte actually
 * present; TOPOLOGY_SNAPSHOT/HEARTBEAT: fixed), or -- SAMPLE only -- an
 * embedded sensor_channel frame that sensor_channel_unpack() itself rejects
 * (an invalid inner frame makes the whole SAMPLE message malformed, not a
 * separately-reported error).
 */
bool bridge_export_unpack_sample(const uint8_t *payload, uint16_t payload_len, bridge_export_sample_t *out);
bool bridge_export_unpack_topology_node(const uint8_t *payload, uint16_t payload_len,
                                         bridge_export_topology_node_t *out);
bool bridge_export_unpack_heartbeat(const uint8_t *payload, uint16_t payload_len, bridge_export_heartbeat_t *out);

#ifdef __cplusplus
}
#endif
