// Pure logic test (no sockets) for ingest::Registry + ingest::handle_request(),
// linked against the REAL bridge_export_proto.c/sensor_channel.c/pogo_crc16.c
// (via ingest_core -> pogo_wire_formats) -- messages are built with the
// production bridge_export_pack_*() functions, never hand-crafted byte
// arrays, so this test can never silently drift from the real wire format.
//
// Covers: 2.04/2.03/4.00/5.03 response codes, (power_base_uuid, module_uuid,
// sample_seq) dedup, different UUIDs with the same seq never confused,
// topology exact-duplicate detection, and storage-full behavior once the
// bounded registry is exhausted.
#include <cstdio>
#include <cstring>
#include <vector>

#include "ingest/registry.hpp"
#include "ingest/session.hpp"

extern "C" {
#include "bridge_export_proto/bridge_export_proto.h"
#include "sensor_channel/sensor_channel.h"
}

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                              \
            g_failures++;                                                                                            \
        } else {                                                                                                     \
            std::printf("ok:   %s\n", msg);                                                                          \
        }                                                                                                            \
    } while (0)

using ingest::CoapMessage;
using ingest::CoapType;
using ingest::Registry;

static CoapMessage make_request(const std::string &uri, const std::vector<uint8_t> &payload)
{
    CoapMessage req;
    req.type = CoapType::Confirmable;
    req.code = ingest::kCoapCodePost;
    req.message_id = 1;
    req.token = {0x01};
    req.uri_path = uri;
    req.payload = payload;
    return req;
}

static std::vector<uint8_t> pack_sample_wire(uint8_t uuid_tag, uint32_t seq, int32_t value)
{
    sensor_sample_t s;
    std::memset(&s, 0, sizeof(s));
    s.payload_version = SENSOR_CHANNEL_PAYLOAD_VERSION;
    std::memset(s.module_uuid, uuid_tag, SENSOR_UUID_LEN);
    s.sample_seq = seq;
    s.uptime_ms = 1000;
    s.sensor_bitmap = 0x0001;
    s.channel_count = 1;
    s.channels[0].type = SENSOR_CHANNEL_TEMPERATURE;
    s.channels[0].scale = SENSOR_SCALE_MILLI;
    s.channels[0].status = SENSOR_CHANNEL_STATUS_OK;
    s.channels[0].value = value;

    uint8_t sc_buf[SENSOR_CHANNEL_MAX_WIRE_LEN];
    size_t sc_len = sensor_channel_pack(&s, sc_buf, sizeof(sc_buf));

    bridge_export_sample_t exp;
    std::memset(&exp, 0, sizeof(exp));
    std::memset(exp.power_base_uuid, 0xB0, BRIDGE_EXPORT_UUID_LEN);
    std::memcpy(exp.sensor_channel_payload, sc_buf, sc_len);
    exp.sensor_channel_len = (uint16_t)sc_len;

    uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
    size_t wlen = bridge_export_pack_sample(&exp, wire, sizeof(wire));
    return std::vector<uint8_t>(wire, wire + wlen);
}

static std::vector<uint8_t> pack_topology_wire(uint8_t uuid_tag, bool online, uint8_t depth, uint32_t last_seq)
{
    bridge_export_topology_node_t node;
    std::memset(&node, 0, sizeof(node));
    std::memset(node.power_base_uuid, 0xB0, BRIDGE_EXPORT_UUID_LEN);
    std::memset(node.module_uuid, uuid_tag, BRIDGE_EXPORT_UUID_LEN);
    std::memset(node.parent_uuid, 0xB0, BRIDGE_EXPORT_UUID_LEN);
    node.online = online;
    node.depth = depth;
    node.role = 3;
    node.sensor_bitmap = 0x0001;
    node.downstream_connected = false;
    node.boot_session_generation = 42;
    node.last_seq = last_seq;

    uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
    size_t wlen = bridge_export_pack_topology_node(&node, wire, sizeof(wire));
    return std::vector<uint8_t>(wire, wire + wlen);
}

int main()
{
    std::printf("\n-- scenario: a real SAMPLE gets 2.04, an exact-duplicate (same uuid+seq) gets 2.03 --\n");
    {
        Registry reg;
        auto wire1 = pack_sample_wire(0x01, 7, 21000);
        uint8_t code1 = ingest::handle_request(reg, make_request("pogo/v1/sample", wire1));
        CHECK(code1 == ingest::kCoapCodeChanged, "a brand-new sample gets 2.04 Changed");

        auto wire2 = pack_sample_wire(0x01, 7, 21000); // identical uuid+seq
        uint8_t code2 = ingest::handle_request(reg, make_request("pogo/v1/sample", wire2));
        CHECK(code2 == ingest::kCoapCodeValid, "an exact-duplicate (same uuid+seq) retransmit gets 2.03 Valid");

        CHECK(reg.node_count() == 1, "only one node is tracked after a duplicate");
    }

    std::printf("\n-- scenario: different UUIDs with the SAME seq are never confused --\n");
    {
        Registry reg;
        auto wire_a = pack_sample_wire(0xAA, 5, 100);
        auto wire_b = pack_sample_wire(0xBB, 5, 200);
        uint8_t code_a = ingest::handle_request(reg, make_request("pogo/v1/sample", wire_a));
        uint8_t code_b = ingest::handle_request(reg, make_request("pogo/v1/sample", wire_b));
        CHECK(code_a == ingest::kCoapCodeChanged, "module A's seq=5 sample is new (2.04)");
        CHECK(code_b == ingest::kCoapCodeChanged,
              "module B's seq=5 sample is ALSO new (2.04) -- the same seq on a different uuid is never treated as "
              "a duplicate of module A's");
        CHECK(reg.node_count() == 2, "both modules are tracked as separate nodes");

        ingest::NodeState node_a, node_b;
        bool found_a = false, found_b = false;
        for (size_t i = 0; i < reg.node_count(); i++) {
            ingest::NodeState n;
            reg.get_node(i, n);
            if (n.module_uuid[7] == 0xAA) {
                node_a = n;
                found_a = true;
            }
            if (n.module_uuid[7] == 0xBB) {
                node_b = n;
                found_b = true;
            }
        }
        CHECK(found_a && found_b, "both distinct uuids are individually retrievable");
        CHECK(!node_a.last_channels.empty() && node_a.last_channels[0].value == 100,
              "module A's stored channel value is its own (100), not module B's");
        CHECK(!node_b.last_channels.empty() && node_b.last_channels[0].value == 200,
              "module B's stored channel value is its own (200), not module A's");
    }

    std::printf("\n-- scenario: a reordered/different seq is NOT treated as a duplicate --\n");
    {
        Registry reg;
        ingest::handle_request(reg, make_request("pogo/v1/sample", pack_sample_wire(0x01, 10, 1)));
        uint8_t code = ingest::handle_request(reg, make_request("pogo/v1/sample", pack_sample_wire(0x01, 9, 2)));
        CHECK(code == ingest::kCoapCodeChanged,
              "a different (lower) seq for the same uuid is applied as new, not rejected as a duplicate -- this "
              "ingest program mirrors sensor_root_aggregator's own 'exact seq match only' dedup rule, not a "
              "monotonicity check");
    }

    std::printf("\n-- scenario: topology exact-duplicate detection --\n");
    {
        Registry reg;
        uint8_t code1 = ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, true, 1, 5)));
        CHECK(code1 == ingest::kCoapCodeChanged, "a brand-new topology node gets 2.04");
        uint8_t code2 = ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, true, 1, 5)));
        CHECK(code2 == ingest::kCoapCodeValid, "an exact-duplicate topology snapshot gets 2.03");
        uint8_t code3 = ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, false, 1, 6)));
        CHECK(code3 == ingest::kCoapCodeChanged, "a real change (online flips false) gets 2.04 again");

        ingest::NodeState n;
        reg.get_node(0, n);
        CHECK(n.online == false, "the node's stored state reflects the latest applied snapshot");
    }

    std::printf("\n-- scenario: malformed payload is rejected with 4.00 --\n");
    {
        Registry reg;
        std::vector<uint8_t> garbage = {0x01, 0x02, 0x03};
        uint8_t code = ingest::handle_request(reg, make_request("pogo/v1/sample", garbage));
        CHECK(code == ingest::kCoapCodeBadRequest, "an undecodable envelope gets 4.00 Bad Request");
        CHECK(reg.node_count() == 0, "nothing is stored for a malformed message");
    }

    std::printf("\n-- scenario: a msg_type/resource mismatch is rejected with 4.00 --\n");
    {
        Registry reg;
        auto topo_wire = pack_topology_wire(0x01, true, 1, 1);
        uint8_t code = ingest::handle_request(reg, make_request("pogo/v1/sample", topo_wire));
        CHECK(code == ingest::kCoapCodeBadRequest,
              "posting a TOPOLOGY_SNAPSHOT message to the sample resource is rejected (4.00), not silently "
              "reinterpreted");
    }

    std::printf("\n-- scenario: storage-full (5.03) once the bounded registry is exhausted --\n");
    {
        Registry reg;
        for (int i = 0; i < (int)Registry::kMaxNodes; i++) {
            uint8_t code = ingest::handle_request(reg, make_request("pogo/v1/sample", pack_sample_wire((uint8_t)i, 1, i)));
            CHECK(code == ingest::kCoapCodeChanged, "filling the registry: each distinct uuid is accepted as new");
        }
        CHECK(reg.node_count() == Registry::kMaxNodes, "the registry is now exactly full");
        uint8_t overflow_code =
            ingest::handle_request(reg, make_request("pogo/v1/sample", pack_sample_wire(0xFE, 1, 999)));
        CHECK(overflow_code == ingest::kCoapCodeServiceUnavailable,
              "a brand-new uuid past capacity gets 5.03 Service Unavailable");
        CHECK(reg.node_count() == Registry::kMaxNodes, "the registry size never exceeds its bound");

        // An update to an ALREADY-known uuid must still work even while full.
        uint8_t existing_update_code =
            ingest::handle_request(reg, make_request("pogo/v1/sample", pack_sample_wire(0x00, 2, 12345)));
        CHECK(existing_update_code == ingest::kCoapCodeChanged,
              "a new sample for an ALREADY-tracked uuid still succeeds while the registry is full (it's an "
              "update, not a new slot)");
    }

    std::printf("\n-- scenario: heartbeat is always accepted (2.04), never rejected as a duplicate --\n");
    {
        Registry reg;
        bridge_export_heartbeat_t hb;
        std::memset(&hb, 0, sizeof(hb));
        std::memset(hb.power_base_uuid, 0xB0, BRIDGE_EXPORT_UUID_LEN);
        hb.uptime_ms = 1000;
        hb.node_count = 3;
        uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
        size_t wlen = bridge_export_pack_heartbeat(&hb, wire, sizeof(wire));
        std::vector<uint8_t> payload(wire, wire + wlen);

        uint8_t code1 = ingest::handle_request(reg, make_request("pogo/v1/heartbeat", payload));
        uint8_t code2 = ingest::handle_request(reg, make_request("pogo/v1/heartbeat", payload));
        CHECK(code1 == ingest::kCoapCodeChanged, "the first heartbeat gets 2.04");
        CHECK(code2 == ingest::kCoapCodeChanged, "an identical repeat heartbeat ALSO gets 2.04 -- never treated as "
                                                   "a duplicate (heartbeats have no dedup key)");
    }

    std::printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
