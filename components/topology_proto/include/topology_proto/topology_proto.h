/*
 * Topology report wire format -- sent by every sensor module to the power
 * base over Thread CoAP (resource "/t", see components/power_base_transport
 * and docs/power-base.md), transported and interpreted separately from
 * pogo_proto (the wired POGO provisioning protocol, left alone) and from
 * sensor_channel (the sensor-sample wire format, also left alone) -- same
 * reasoning as sensor_channel.h's own top comment: a network layer never
 * needs to know a per-purpose struct, only an opaque, versioned,
 * length-prefixed byte buffer.
 *
 * Fixed-length, versioned binary format (no ESP-IDF/FreeRTOS dependency --
 * host-testable directly, see tools/test_topology_proto_pack.c). All
 * multi-byte integers are big-endian, matching every other wire format in
 * this codebase (pogo_proto, sensor_channel, module_config).
 *
 * Wire layout (32 bytes, TOPOLOGY_REPORT_WIRE_LEN):
 *   0   1  payload_version (must equal TOPOLOGY_REPORT_PAYLOAD_VERSION)
 *   1   8  module_uuid          -- this reporting module's own UUID
 *   9   8  parent_uuid          -- this module's immediate upstream parent's
 *                                   UUID (power base or another module),
 *                                   from POGO_MSG_ASSIGN_PARENT_INFO
 *   17  1  topology_depth       -- hops from the power base; the power
 *                                   base's direct child reports depth=1
 *   18  1  role                 -- one of topology_role_t (see below)
 *   19  2  sensor_type_bitmap   -- this module's fixed sensor_type_bitmap
 *   21  4  boot_session_generation -- random, chosen once at boot; lets the
 *                                   registry tell "this module rebooted and
 *                                   reset its sequence counter" apart from
 *                                   "this is a stale/reordered packet from
 *                                   the same boot"
 *   25  4  sequence             -- monotonically increasing within one
 *                                   boot_session_generation, starting at 1
 *   29  1  upstream_connected   -- bool: this module's own UPSTREAM POGO port
 *                                   is currently attached+PEER_READY
 *   30  1  downstream_connected -- bool: this module's own DOWNSTREAM POGO
 *                                   port currently has a MODULE peer
 *                                   attached+PEER_READY (false if nothing is
 *                                   docked there, or if a docked module has
 *                                   not yet resolved its handshake)
 *   31  1  report_type          -- one of topology_report_type_t
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TOPOLOGY_REPORT_PAYLOAD_VERSION 1u
#define TOPOLOGY_UUID_LEN 8
#define TOPOLOGY_REPORT_WIRE_LEN 32u

/*
 * Numeric values MUST match components/role_manager/include/role_manager/role_manager.h's
 * module_role_t exactly (same convention as sensor_channel_type_t mirroring
 * module_config.sensor_type_bitmap's bit numbering: one shared numbering,
 * defined independently in each side's own dependency-free header rather
 * than forcing a pure wire-format component to depend on role_manager/
 * ESP-IDF). ROOT is included only for the MODULE_DEV_MODE_DEV_ROOT
 * regression fixture and passive-power-base bench rigs -- an AUTO-mode
 * sensor module never reports ROOT (see role_manager.c's determination
 * table / docs/pogo-interface.md).
 */
typedef enum {
    TOPOLOGY_ROLE_UNASSIGNED = 0,
    TOPOLOGY_ROLE_ROOT = 1,
    TOPOLOGY_ROLE_INTERMEDIATE = 2,
    TOPOLOGY_ROLE_LEAF = 3,
    TOPOLOGY_ROLE_ERROR = 4,
} topology_role_t;

typedef enum {
    TOPOLOGY_REPORT_ATTACHED = 0,  /* first report after this module newly settled into a role */
    TOPOLOGY_REPORT_UPDATED = 1,   /* role, parent, depth, or connection state changed */
    TOPOLOGY_REPORT_DETACHED = 2,  /* best-effort: this module is about to lose its own upstream */
    TOPOLOGY_REPORT_HEARTBEAT = 3, /* periodic "still here, nothing changed" */
} topology_report_type_t;

typedef struct {
    uint8_t  payload_version;
    uint8_t  module_uuid[TOPOLOGY_UUID_LEN];
    uint8_t  parent_uuid[TOPOLOGY_UUID_LEN];
    uint8_t  topology_depth;
    uint8_t  role; /* one of topology_role_t */
    uint16_t sensor_type_bitmap;
    uint32_t boot_session_generation;
    uint32_t sequence;
    bool     upstream_connected;
    bool     downstream_connected;
    uint8_t  report_type; /* one of topology_report_type_t */
} topology_report_t;

/*
 * Serializes `in` into `out` (capacity out_size, must be >= TOPOLOGY_REPORT_WIRE_LEN).
 * Returns the number of bytes written (always exactly TOPOLOGY_REPORT_WIRE_LEN
 * on success), or 0 if out_size is too small, `in->role` is not a valid
 * topology_role_t, or `in->report_type` is not a valid topology_report_type_t
 * (a corrupt/buggy caller must not be allowed to produce a "valid-looking"
 * frame a receiver would then also have to defend against -- same policy as
 * sensor_channel_pack()).
 */
size_t topology_report_pack(const topology_report_t *in, uint8_t *out, size_t out_size);

/*
 * Parses `data`/`len` into `out`. Returns false for: len != TOPOLOGY_REPORT_WIRE_LEN
 * exactly (no truncation, no trailing garbage), an unsupported
 * payload_version, an unknown role/report_type byte value.
 */
bool topology_report_unpack(const uint8_t *data, uint16_t len, topology_report_t *out);

#ifdef __cplusplus
}
#endif
