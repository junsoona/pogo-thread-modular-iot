// Pure logic test (no sockets) for ingest::Registry + ingest::handle_request(),
// linked against the REAL bridge_export_proto.c/sensor_channel.c/pogo_crc16.c
// (via ingest_core -> pogo_wire_formats) -- messages are built with the
// production bridge_export_pack_*() functions, never hand-crafted byte
// arrays, so this test can never silently drift from the real wire format.
//
// Covers: 2.04/2.03/4.00/4.04/4.12/5.03 response codes, (power_base_uuid,
// module_uuid, sample_seq) dedup, different UUIDs with the same seq never
// confused, topology exact-duplicate detection, storage-full behavior once
// the bounded registry is exhausted, the correction pass's SAMPLE topology
// gating (unknown/offline/bitmap-mismatch), and the correction pass's
// TOPOLOGY generation/sequence ordering protection (stale/reordered
// snapshots rejected, a delayed old-generation "online" snapshot can never
// resurrect a state a newer report already turned offline, a genuine reboot
// generation is accepted).
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

static std::vector<uint8_t> pack_sample_wire(uint8_t uuid_tag, uint32_t seq, int32_t value,
                                              uint16_t sensor_bitmap = 0x0002 /* bit for TEMPERATURE */)
{
    sensor_sample_t s;
    std::memset(&s, 0, sizeof(s));
    s.payload_version = SENSOR_CHANNEL_PAYLOAD_VERSION;
    std::memset(s.module_uuid, uuid_tag, SENSOR_UUID_LEN);
    s.sample_seq = seq;
    s.uptime_ms = 1000;
    s.sensor_bitmap = sensor_bitmap;
    s.channel_count = 1;
    s.channels[0].type = SENSOR_CHANNEL_TEMPERATURE; // bit value (1 << 1) = 0x0002
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

static std::vector<uint8_t> pack_topology_wire(uint8_t uuid_tag, bool online, uint8_t depth, uint32_t generation,
                                                uint32_t last_seq, uint16_t sensor_bitmap = 0x0002)
{
    bridge_export_topology_node_t node;
    std::memset(&node, 0, sizeof(node));
    std::memset(node.power_base_uuid, 0xB0, BRIDGE_EXPORT_UUID_LEN);
    std::memset(node.module_uuid, uuid_tag, BRIDGE_EXPORT_UUID_LEN);
    std::memset(node.parent_uuid, 0xB0, BRIDGE_EXPORT_UUID_LEN);
    node.online = online;
    node.depth = depth;
    node.role = 3;
    node.sensor_bitmap = sensor_bitmap;
    node.downstream_connected = false;
    node.boot_session_generation = generation;
    node.last_seq = last_seq;

    uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
    size_t wlen = bridge_export_pack_topology_node(&node, wire, sizeof(wire));
    return std::vector<uint8_t>(wire, wire + wlen);
}

static std::vector<uint8_t> pack_removed_wire(uint8_t uuid_tag)
{
    bridge_export_topology_removed_t rem;
    std::memset(&rem, 0, sizeof(rem));
    std::memset(rem.power_base_uuid, 0xB0, BRIDGE_EXPORT_UUID_LEN);
    std::memset(rem.module_uuid, uuid_tag, BRIDGE_EXPORT_UUID_LEN);

    uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
    size_t wlen = bridge_export_pack_topology_removed(&rem, wire, sizeof(wire));
    return std::vector<uint8_t>(wire, wire + wlen);
}

int main()
{
    printf("\n-- scenario: a TOPOLOGY_REMOVED tombstone over CoAP removes the node (2.04), and a duplicate is "
           "idempotent (2.03) -- this follow-up's POGO removal lifecycle, dispatched through handle_request() "
           "exactly like every other message type --\n");
    {
        Registry reg;
        ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, true, 1, 1, 1)));
        CHECK(reg.node_count() == 1, "the node is active before removal");

        uint8_t code1 = ingest::handle_request(reg, make_request("pogo/v1/topology", pack_removed_wire(0x01)));
        CHECK(code1 == ingest::kCoapCodeChanged, "the first TOPOLOGY_REMOVED for this uuid is 2.04 Changed");
        CHECK(reg.node_count() == 0, "the node is gone immediately");

        uint8_t code2 = ingest::handle_request(reg, make_request("pogo/v1/topology", pack_removed_wire(0x01)));
        CHECK(code2 == ingest::kCoapCodeValid, "a duplicate TOPOLOGY_REMOVED (e.g. the firmware's own retry) is "
                                                 "2.03 Valid, not re-applied");

        printf("\n-- and: a stray SAMPLE for the now-removed uuid is rejected 4.04, never silently applied --\n");
        uint8_t sample_code = ingest::handle_request(reg, make_request("pogo/v1/sample", pack_sample_wire(0x01, 1, 100)));
        CHECK(sample_code == ingest::kCoapCodeNotFound, "a sample for a removed uuid is rejected exactly like a "
                                                          "never-seen uuid (4.04) -- it is not in the registry at all");

        printf("\n-- and: a stray online=false topology resync for the removed uuid is rejected 4.12, never "
               "resurrecting it --\n");
        uint8_t stale_resync_code =
            ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, false, 1, 1, 2)));
        CHECK(stale_resync_code == ingest::kCoapCodePreconditionFailed,
              "an offline resync of the removed uuid is rejected, not treated as a new sighting");
        CHECK(reg.node_count() == 0, "still removed");

        printf("\n-- and: the module's LAST sample re-sent byte-for-byte (what an older Power Base's periodic sample "
               "resync did after a detach) is rejected and never recreates it --\n");
        {
            Registry reg2;
            ingest::handle_request(reg2, make_request("pogo/v1/topology", pack_topology_wire(0x02, true, 1, 1, 1)));
            std::vector<uint8_t> last_sample = pack_sample_wire(0x02, 7, 700);
            CHECK(ingest::handle_request(reg2, make_request("pogo/v1/sample", last_sample)) == ingest::kCoapCodeChanged,
                  "the sample applies while the module is registered");
            CHECK(ingest::handle_request(reg2, make_request("pogo/v1/sample", last_sample)) == ingest::kCoapCodeValid,
                  "(before removal an identical resend is a 2.03 duplicate -- it counts as liveness)");
            ingest::handle_request(reg2, make_request("pogo/v1/topology", pack_removed_wire(0x02)));
            uint64_t gen = reg2.generation();
            bool all_rejected = true;
            for (int i = 0; i < 5; i++) {
                all_rejected = all_rejected && ingest::handle_request(reg2, make_request("pogo/v1/sample", last_sample)) ==
                                                   ingest::kCoapCodeNotFound;
            }
            CHECK(all_rejected, "after removal every identical resend is 4.04");
            CHECK(reg2.node_count() == 0 && reg2.snapshot().empty(), "the module stays absent from every reader");
            CHECK(reg2.generation() == gen, "registry_generation does not move (no SSE/Matter churn)");
        }

        printf("\n-- and: re-docking (a fresh online=true report) automatically re-approves and re-registers --\n");
        uint8_t reattach_code =
            ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, true, 1, 2, 1)));
        CHECK(reattach_code == ingest::kCoapCodeChanged, "the re-attach report applies as a fresh 2.04 Changed");
        CHECK(reg.node_count() == 1, "the module is active again after re-docking");
    }

    printf("\n-- scenario: a SAMPLE with no topology at all is rejected 4.04 (correction pass) --\n");
    {
        Registry reg;
        uint8_t code = ingest::handle_request(reg, make_request("pogo/v1/sample", pack_sample_wire(0x01, 1, 100)));
        CHECK(code == ingest::kCoapCodeNotFound, "a sample for an unknown (power_base_uuid, module_uuid) gets 4.04");
        CHECK(reg.node_count() == 0, "nothing is stored for a rejected sample");
    }

    printf("\n-- scenario: a SAMPLE for a known but OFFLINE uuid is rejected 4.12 --\n");
    {
        Registry reg;
        ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, false, 1, 1, 1)));
        uint8_t code = ingest::handle_request(reg, make_request("pogo/v1/sample", pack_sample_wire(0x01, 1, 100)));
        CHECK(code == ingest::kCoapCodePreconditionFailed, "a sample for a known-but-offline uuid gets 4.12");
    }

    printf("\n-- scenario: a SAMPLE whose bitmap is not a subset of the topology's bitmap is rejected 4.12 --\n");
    {
        Registry reg;
        /* Topology declares ONLY illuminance (bit 0 = 0x0001). */
        ingest::handle_request(reg,
                                make_request("pogo/v1/topology", pack_topology_wire(0x01, true, 1, 1, 1, 0x0001)));
        /* Sample declares temperature (bit 1 = 0x0002) -- not in 0x0001. */
        uint8_t code =
            ingest::handle_request(reg, make_request("pogo/v1/sample", pack_sample_wire(0x01, 1, 100, 0x0002)));
        CHECK(code == ingest::kCoapCodePreconditionFailed,
              "a sample bitmap not a subset of the topology's own bitmap gets 4.12");
    }

    printf("\n-- scenario: a real SAMPLE (topology known+online, matching bitmap) gets 2.04, an exact-duplicate "
           "(same uuid+seq) gets 2.03 --\n");
    {
        Registry reg;
        ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, true, 1, 1, 1)));

        auto wire1 = pack_sample_wire(0x01, 7, 21000);
        uint8_t code1 = ingest::handle_request(reg, make_request("pogo/v1/sample", wire1));
        CHECK(code1 == ingest::kCoapCodeChanged, "a brand-new sample gets 2.04 Changed");

        auto wire2 = pack_sample_wire(0x01, 7, 21000); // identical uuid+seq
        uint8_t code2 = ingest::handle_request(reg, make_request("pogo/v1/sample", wire2));
        CHECK(code2 == ingest::kCoapCodeValid, "an exact-duplicate (same uuid+seq) retransmit gets 2.03 Valid");
    }

    printf("\n-- scenario: different UUIDs with the SAME seq are never confused --\n");
    {
        Registry reg;
        ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0xAA, true, 1, 1, 1)));
        ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0xBB, true, 1, 1, 1)));
        auto wire_a = pack_sample_wire(0xAA, 5, 100);
        auto wire_b = pack_sample_wire(0xBB, 5, 200);
        uint8_t code_a = ingest::handle_request(reg, make_request("pogo/v1/sample", wire_a));
        uint8_t code_b = ingest::handle_request(reg, make_request("pogo/v1/sample", wire_b));
        CHECK(code_a == ingest::kCoapCodeChanged, "module A's seq=5 sample is new (2.04)");
        CHECK(code_b == ingest::kCoapCodeChanged,
              "module B's seq=5 sample is ALSO new (2.04) -- the same seq on a different uuid is never treated as "
              "a duplicate of module A's");

        ingest::NodeState node_a, node_b;
        bool found_a = false, found_b = false;
        for (size_t i = 0; i < reg.node_count(); i++) {
            ingest::NodeState n;
            reg.get_node(i, n);
            if (n.module_uuid[7] == 0xAA && n.has_sample) {
                node_a = n;
                found_a = true;
            }
            if (n.module_uuid[7] == 0xBB && n.has_sample) {
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

    printf("\n-- scenario: a reordered/different seq is NOT treated as a duplicate (sample dedup has no "
           "monotonicity requirement) --\n");
    {
        Registry reg;
        ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, true, 1, 1, 1)));
        ingest::handle_request(reg, make_request("pogo/v1/sample", pack_sample_wire(0x01, 10, 1)));
        uint8_t code = ingest::handle_request(reg, make_request("pogo/v1/sample", pack_sample_wire(0x01, 9, 2)));
        CHECK(code == ingest::kCoapCodeChanged,
              "a different (lower) seq for the same uuid is applied as new, not rejected as a duplicate -- this "
              "ingest program mirrors sensor_root_aggregator's own 'exact seq match only' dedup rule");
    }

    printf("\n-- scenario: topology exact-duplicate detection --\n");
    {
        Registry reg;
        uint8_t code1 =
            ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, true, 1, 100, 5)));
        CHECK(code1 == ingest::kCoapCodeChanged, "a brand-new topology node gets 2.04");
        uint8_t code2 =
            ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, true, 1, 100, 5)));
        CHECK(code2 == ingest::kCoapCodeValid, "an exact-duplicate topology snapshot (same generation+seq) gets 2.03");
        uint8_t code3 =
            ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, false, 1, 100, 6)));
        CHECK(code3 == ingest::kCoapCodeChanged, "a real change (online flips false, seq increases) gets 2.04 again");

        ingest::NodeState n;
        reg.get_node(0, n);
        CHECK(n.online == false, "the node's stored state reflects the latest applied snapshot");
    }

    printf("\n-- scenario (correction pass): a stale/reordered seq within the SAME generation is rejected 4.12, "
           "never applied --\n");
    {
        Registry reg;
        ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, false, 1, 100, 10)));
        /* A delayed packet with a SMALLER seq in the SAME generation,
         * claiming online=true -- must NOT resurrect the node. */
        uint8_t code =
            ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, true, 1, 100, 5)));
        CHECK(code == ingest::kCoapCodePreconditionFailed, "a smaller seq in the same generation gets 4.12");

        ingest::NodeState n;
        reg.get_node(0, n);
        CHECK(n.online == false, "the node's online flag is UNCHANGED by the stale packet -- still offline");
        CHECK(n.last_seq == 10, "the node's last_seq is UNCHANGED by the stale packet");
    }

    printf("\n-- scenario (correction pass): a delayed packet from the IMMEDIATELY-PRECEDING generation is "
           "rejected 4.12 and can never resurrect a newer offline state --\n");
    {
        Registry reg;
        /* Session 1 (generation=100): node comes online. */
        ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, true, 1, 100, 1)));
        /* Session 2 (generation=200, e.g. this module rebooted): comes
         * online again, then a LATER report in session 2 marks it offline. */
        ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, true, 1, 200, 1)));
        ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, false, 1, 200, 2)));

        /* A DELAYED packet from session 1 (the immediately-preceding
         * generation) finally arrives, claiming online=true. */
        uint8_t code =
            ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, true, 1, 100, 2)));
        CHECK(code == ingest::kCoapCodePreconditionFailed,
              "a delayed packet from the superseded (immediately-preceding) generation gets 4.12");

        ingest::NodeState n;
        reg.get_node(0, n);
        CHECK(n.online == false, "the delayed session-1 packet never resurrects the node -- still offline (the "
                                   "REAL, current session-2 state)");
        CHECK(n.boot_session_generation == 200, "the current generation is still session 2");
    }

    printf("\n-- scenario (correction pass): a genuinely NEW generation (e.g. a Power Base reboot) is accepted "
           "normally --\n");
    {
        Registry reg;
        ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, true, 1, 100, 1)));
        ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, false, 1, 100, 2)));
        /* A brand-new generation (300) -- neither the current (100) nor the
         * (nonexistent, since this node has never crossed a generation
         * boundary yet) superseded one. */
        uint8_t code =
            ingest::handle_request(reg, make_request("pogo/v1/topology", pack_topology_wire(0x01, true, 1, 300, 1)));
        CHECK(code == ingest::kCoapCodeChanged, "a genuinely new generation is accepted as a fresh session (2.04)");

        ingest::NodeState n;
        reg.get_node(0, n);
        CHECK(n.online == true, "the new session's state (online=true) is applied");
        CHECK(n.boot_session_generation == 300, "the generation is updated to the new session");
    }

    printf("\n-- scenario: malformed payload is rejected with 4.00 --\n");
    {
        Registry reg;
        std::vector<uint8_t> garbage = {0x01, 0x02, 0x03};
        uint8_t code = ingest::handle_request(reg, make_request("pogo/v1/sample", garbage));
        CHECK(code == ingest::kCoapCodeBadRequest, "an undecodable envelope gets 4.00 Bad Request");
        CHECK(reg.node_count() == 0, "nothing is stored for a malformed message");
    }

    printf("\n-- scenario: a msg_type/resource mismatch is rejected with 4.00 --\n");
    {
        Registry reg;
        auto topo_wire = pack_topology_wire(0x01, true, 1, 1, 1);
        uint8_t code = ingest::handle_request(reg, make_request("pogo/v1/sample", topo_wire));
        CHECK(code == ingest::kCoapCodeBadRequest,
              "posting a TOPOLOGY_SNAPSHOT message to the sample resource is rejected (4.00), not silently "
              "reinterpreted");
    }

    printf("\n-- scenario: storage-full (5.03) once the bounded registry is exhausted --\n");
    {
        Registry reg;
        for (int i = 0; i < (int)Registry::kMaxNodes; i++) {
            uint8_t code = ingest::handle_request(
                reg, make_request("pogo/v1/topology", pack_topology_wire((uint8_t)i, true, 1, 1, 1)));
            CHECK(code == ingest::kCoapCodeChanged, "filling the registry: each distinct uuid is accepted as new");
        }
        CHECK(reg.node_count() == Registry::kMaxNodes, "the registry is now exactly full");
        uint8_t overflow_code = ingest::handle_request(
            reg, make_request("pogo/v1/topology", pack_topology_wire(0xFE, true, 1, 1, 1)));
        CHECK(overflow_code == ingest::kCoapCodeServiceUnavailable,
              "a brand-new uuid past capacity gets 5.03 Service Unavailable");
        CHECK(reg.node_count() == Registry::kMaxNodes, "the registry size never exceeds its bound");

        // An update to an ALREADY-known uuid must still work even while full.
        uint8_t existing_update_code = ingest::handle_request(
            reg, make_request("pogo/v1/topology", pack_topology_wire(0x00, true, 1, 1, 2)));
        CHECK(existing_update_code == ingest::kCoapCodeChanged,
              "a new report for an ALREADY-tracked uuid still succeeds while the registry is full (it's an "
              "update, not a new slot)");
    }

    printf("\n-- scenario: heartbeat is always accepted (2.04), never rejected as a duplicate --\n");
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
