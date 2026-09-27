// Multi-Power-Base milestone: two (or more) Power Bases reporting to ONE
// ingest::Registry / ONE ingest server instance.
//
// Covers:
//   - two Bases register independently (heartbeat alone makes a Base
//     visible; steady heartbeats never advance generation());
//   - modules stay separated per Base, even with the same sensor kind on
//     both, and a Base can never feed samples into another Base's module;
//   - one Base goes offline (stale timeout) while the other stays online,
//     then comes back with an unchanged resync and its modules recover;
//   - a detach tombstone from one Base never removes the same module now
//     docked to another Base (Base-scoped removal);
//   - attach_order orders the most recent physical attach first;
//   - a single Base behaves exactly as before (removal/re-dock, no churn);
//   - two senders on different UDP source ports through the REAL
//     ingest::run_server() both get ACKed and land under their own Base.
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ingest/minicoap.hpp"
#include "ingest/registry.hpp"
#include "ingest/server.hpp"

extern "C" {
#include "bridge_export_proto/bridge_export_proto.h"
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

using ingest::ApplyResult;
using ingest::NodeState;
using ingest::PowerBaseState;
using ingest::Registry;
using ingest::RegistryView;
using ingest::Uuid;

namespace {

constexpr uint16_t kSht45Bitmap = 0x0006; // temperature | humidity
constexpr uint16_t kPmsBitmap = 0x0380;   // pm1 | pm2_5 | pm10

Uuid make_uuid(uint8_t tag)
{
    Uuid u{};
    u.fill(tag);
    return u;
}

const NodeState *find_node(const RegistryView &v, const Uuid &pb, const Uuid &mod)
{
    for (const auto &n : v.nodes) {
        if (n.power_base_uuid == pb && n.module_uuid == mod) {
            return &n;
        }
    }
    return nullptr;
}

const PowerBaseState *find_base(const RegistryView &v, const Uuid &pb)
{
    for (const auto &b : v.power_bases) {
        if (b.power_base_uuid == pb) {
            return &b;
        }
    }
    return nullptr;
}

size_t count_module(const RegistryView &v, const Uuid &mod)
{
    return static_cast<size_t>(
        std::count_if(v.nodes.begin(), v.nodes.end(), [&mod](const NodeState &n) { return n.module_uuid == mod; }));
}

// --- loopback helpers (test-only CoAP request encoder, same shape as
// test_loopback.cpp's -- production code never builds requests) ---

std::vector<uint8_t> build_request(const std::string &uri_path, const std::vector<uint8_t> &payload,
                                    uint16_t message_id)
{
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>((1 << 6) | (static_cast<uint8_t>(ingest::CoapType::Confirmable) << 4) | 1));
    out.push_back(ingest::kCoapCodePost);
    out.push_back(static_cast<uint8_t>(message_id >> 8));
    out.push_back(static_cast<uint8_t>(message_id & 0xFF));
    out.push_back(0x42);
    size_t start = 0;
    uint32_t last_option_number = 0;
    while (start <= uri_path.size()) {
        size_t slash = uri_path.find('/', start);
        std::string segment = uri_path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        uint32_t delta = 11 - last_option_number;
        last_option_number = 11;
        out.push_back(static_cast<uint8_t>((delta << 4) | (segment.size() & 0x0F)));
        out.insert(out.end(), segment.begin(), segment.end());
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    out.push_back(0xFF);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t> heartbeat_wire(const Uuid &pb)
{
    bridge_export_heartbeat_t hb;
    std::memset(&hb, 0, sizeof(hb));
    std::memcpy(hb.power_base_uuid, pb.data(), BRIDGE_EXPORT_UUID_LEN);
    hb.uptime_ms = 1000;
    uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
    size_t wlen = bridge_export_pack_heartbeat(&hb, wire, sizeof(wire));
    return std::vector<uint8_t>(wire, wire + wlen);
}

std::vector<uint8_t> topology_wire(const Uuid &pb, const Uuid &mod, uint16_t bitmap)
{
    bridge_export_topology_node_t node;
    std::memset(&node, 0, sizeof(node));
    std::memcpy(node.power_base_uuid, pb.data(), BRIDGE_EXPORT_UUID_LEN);
    std::memcpy(node.module_uuid, mod.data(), BRIDGE_EXPORT_UUID_LEN);
    std::memcpy(node.parent_uuid, pb.data(), BRIDGE_EXPORT_UUID_LEN);
    node.online = true;
    node.depth = 1;
    node.role = 3;
    node.sensor_bitmap = bitmap;
    node.boot_session_generation = 1;
    node.last_seq = 1;
    uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
    size_t wlen = bridge_export_pack_topology_node(&node, wire, sizeof(wire));
    return std::vector<uint8_t>(wire, wire + wlen);
}

// Sends one Confirmable POST from `client` and returns the ACK's code, or 0.
uint8_t post(int client, const sockaddr_in6 &server, const std::string &uri, const std::vector<uint8_t> &payload,
             uint16_t mid)
{
    auto req = build_request(uri, payload, mid);
    if (sendto(client, req.data(), req.size(), 0, reinterpret_cast<const sockaddr *>(&server), sizeof(server)) < 0) {
        return 0;
    }
    uint8_t resp[512];
    ssize_t n = recv(client, resp, sizeof(resp), 0);
    return n >= 4 ? resp[1] : 0;
}

std::string resolve_loopback_interface_name()
{
    if (if_nametoindex("lo") != 0) {
        return "lo";
    }
    if (if_nametoindex("lo0") != 0) {
        return "lo0";
    }
    std::fprintf(stderr, "no loopback interface -- cannot run\n");
    std::exit(1);
}

} // namespace

int main()
{
    using namespace std::chrono;
    const auto t0 = steady_clock::now();
    const Uuid pb_a = make_uuid(0xA0);
    const Uuid pb_b = make_uuid(0xB0);
    const Uuid a_sht = make_uuid(0x11);
    const Uuid a_pms = make_uuid(0x12);
    const Uuid b_sht = make_uuid(0x21);
    const Uuid b_pms = make_uuid(0x22);

    printf("\n-- scenario: two Power Bases register independently from heartbeats alone --\n");
    {
        Registry reg(milliseconds(120000));
        reg.apply_heartbeat(pb_a, 1000, 0, t0);
        CHECK(reg.generation() == 1, "the first heartbeat from Base A is a visible change (generation 1)");
        reg.apply_heartbeat(pb_b, 1000, 0, t0);
        CHECK(reg.generation() == 2, "Base B's first heartbeat is its own, separate change (generation 2)");

        RegistryView v = reg.view(t0);
        CHECK(v.power_bases.size() == 2, "two Power Bases are tracked");
        CHECK(v.nodes.empty(), "no modules are invented for a Base with nothing docked");
        CHECK(find_base(v, pb_a) != nullptr && find_base(v, pb_a)->online, "Base A is online");
        CHECK(find_base(v, pb_b) != nullptr && find_base(v, pb_b)->online, "Base B is online");

        for (int i = 1; i <= 5; i++) {
            reg.apply_heartbeat(pb_a, 1000 + i * 10000, 0, t0 + seconds(10 * i));
            reg.apply_heartbeat(pb_b, 1000 + i * 10000, 0, t0 + seconds(10 * i));
        }
        CHECK(reg.generation() == 2, "steady 10s heartbeats never advance generation() (no SSE churn)");
    }

    printf("\n-- scenario: modules stay separated per Base, even with the same sensor kinds on both --\n");
    {
        Registry reg(milliseconds(120000));
        CHECK(reg.apply_topology(pb_a, a_sht, pb_a, true, 1, 2, kSht45Bitmap, true, 7, 1, t0) == ApplyResult::AppliedNew,
              "A's SHT45 module registers");
        CHECK(reg.apply_topology(pb_a, a_pms, a_sht, true, 2, 3, kPmsBitmap, false, 8, 1, t0) == ApplyResult::AppliedNew,
              "A's PMS5003 module (depth 2, parent = A's SHT45) registers");
        CHECK(reg.apply_topology(pb_b, b_sht, pb_b, true, 1, 2, kSht45Bitmap, true, 7, 1, t0) == ApplyResult::AppliedNew,
              "B's SHT45 module -- same kind, same depth, same generation/seq numbers -- registers separately");
        CHECK(reg.apply_topology(pb_b, b_pms, b_sht, true, 2, 3, kPmsBitmap, false, 8, 1, t0) == ApplyResult::AppliedNew,
              "B's PMS5003 module registers separately");

        RegistryView v = reg.view(t0);
        CHECK(v.nodes.size() == 4, "four distinct nodes, none merged");
        CHECK(v.power_bases.size() == 2, "a topology report alone also makes its Base known");
        CHECK(find_node(v, pb_a, a_sht) && find_node(v, pb_a, a_pms), "A's two modules are under A");
        CHECK(find_node(v, pb_b, b_sht) && find_node(v, pb_b, b_pms), "B's two modules are under B");
        CHECK(!find_node(v, pb_a, b_sht) && !find_node(v, pb_b, a_sht), "no module appears under the other Base");
        CHECK(find_node(v, pb_b, b_pms)->parent_uuid == b_sht, "B's depth-2 module keeps its reported parent");

        std::vector<ingest::ChannelReading> temp = {ingest::ChannelReading{1, 1, 0, 21000}};
        CHECK(reg.apply_sample(pb_b, a_sht, 1, 1000, 0x0002, temp, t0) == ApplyResult::TopologyUnknown,
              "Base B cannot inject a sample into Base A's module (4.04)");
        CHECK(reg.apply_sample(pb_a, a_sht, 1, 1000, 0x0002, temp, t0) == ApplyResult::AppliedNew,
              "Base A's own sample for its module applies");
        v = reg.view(t0);
        CHECK(find_node(v, pb_a, a_sht)->has_sample && !find_node(v, pb_b, b_sht)->has_sample,
              "the sample landed only on A's module, not on B's same-kind module");
    }

    printf("\n-- scenario: one Base goes offline, the other stays online (read-time staleness) --\n");
    {
        Registry reg(milliseconds(120000));
        reg.apply_topology(pb_a, a_sht, pb_a, true, 1, 3, kSht45Bitmap, false, 7, 1, t0);
        reg.apply_topology(pb_b, b_sht, pb_b, true, 1, 3, kSht45Bitmap, false, 7, 1, t0);
        // Only B keeps talking: heartbeat + unchanged resync every 10s.
        for (int i = 1; i <= 20; i++) {
            reg.apply_heartbeat(pb_b, 0, 1, t0 + seconds(10 * i));
            reg.apply_topology(pb_b, b_sht, pb_b, true, 1, 3, kSht45Bitmap, false, 7, 1, t0 + seconds(10 * i));
        }
        RegistryView v = reg.view(t0 + seconds(200));
        CHECK(!find_base(v, pb_a)->online, "Base A (silent for 200s) is offline");
        CHECK(!find_node(v, pb_a, a_sht)->online, "Base A's module is offline too");
        CHECK(find_base(v, pb_b)->online, "Base B is still online");
        CHECK(find_node(v, pb_b, b_sht)->online, "Base B's module is still online");
    }

    printf("\n-- scenario: one Base offline via the ACTIVE sweep, then back with an unchanged resync --\n");
    {
        const auto kTimeout = milliseconds(150);
        const auto kSweep = milliseconds(10);
        Registry reg(kTimeout, kSweep);
        reg.apply_topology(pb_a, a_sht, pb_a, true, 1, 3, kSht45Bitmap, false, 7, 1);
        reg.apply_topology(pb_b, b_sht, pb_b, true, 1, 3, kSht45Bitmap, false, 7, 1);
        uint64_t g_before = reg.generation();

        // Keep B alive while A goes silent past the timeout.
        auto until = steady_clock::now() + milliseconds(400);
        while (steady_clock::now() < until) {
            reg.apply_heartbeat(pb_b, 0, 1);
            reg.apply_topology(pb_b, b_sht, pb_b, true, 1, 3, kSht45Bitmap, false, 7, 1);
            std::this_thread::sleep_for(milliseconds(20));
        }
        RegistryView v = reg.view();
        CHECK(!find_base(v, pb_a)->online && !find_node(v, pb_a, a_sht)->online,
              "the sweep took Base A and its module offline");
        CHECK(find_node(v, pb_a, a_sht)->stale_demoted, "A's module is marked as demoted by the timeout, not by a report");
        CHECK(find_base(v, pb_b)->online && find_node(v, pb_b, b_sht)->online, "Base B and its module stayed online");
        CHECK(reg.generation() > g_before, "the offline transition advanced generation() (SSE wakes)");

        // A's link comes back: heartbeat, then the exporter's unchanged resync
        // (same generation, same seq, online=true).
        uint64_t g_offline = reg.generation();
        reg.apply_heartbeat(pb_a, 0, 1);
        CHECK(reg.generation() == g_offline + 1, "Base A coming back online is one visible change");
        auto r = reg.apply_topology(pb_a, a_sht, pb_a, true, 1, 3, kSht45Bitmap, false, 7, 1);
        CHECK(r == ApplyResult::AppliedNew, "the unchanged resync restores the timeout-demoted module (AppliedNew)");
        v = reg.view();
        CHECK(find_base(v, pb_a)->online && find_node(v, pb_a, a_sht)->online, "Base A and its module are back online");
        CHECK(!find_node(v, pb_a, a_sht)->stale_demoted, "the demotion marker is cleared");
        CHECK(reg.apply_topology(pb_a, a_sht, pb_a, true, 1, 3, kSht45Bitmap, false, 7, 1) == ApplyResult::DuplicateExact,
              "the next unchanged resync is an ordinary duplicate again");
    }

    printf("\n-- scenario: a report-driven offline is NOT undone by an unchanged resync --\n");
    {
        Registry reg(milliseconds(120000));
        reg.apply_topology(pb_a, a_sht, pb_a, true, 1, 3, kSht45Bitmap, false, 7, 1, t0);
        reg.apply_topology(pb_a, a_sht, pb_a, false, 1, 3, kSht45Bitmap, false, 7, 2, t0);
        CHECK(reg.apply_topology(pb_a, a_sht, pb_a, false, 1, 3, kSht45Bitmap, false, 7, 2, t0) ==
                  ApplyResult::DuplicateExact,
              "the Base's own offline report stays a plain duplicate");
        CHECK(!reg.view(t0).nodes[0].online, "and the module stays offline");
    }

    printf("\n-- scenario: a module moved from Base A to Base B -- attach_order and Base-scoped removal --\n");
    {
        Registry reg(milliseconds(120000));
        reg.apply_topology(pb_a, a_sht, pb_a, true, 1, 3, kSht45Bitmap, false, 7, 1, t0);
        // Physically moved: B provisions it and reports it online while A
        // still lists it online (A's own stale sweep has not run yet).
        reg.apply_topology(pb_b, a_sht, pb_b, true, 1, 3, kSht45Bitmap, false, 7, 5, t0 + seconds(1));
        RegistryView v = reg.view(t0 + seconds(1));
        CHECK(count_module(v, a_sht) == 2, "the Registry briefly holds the module under both Bases");
        CHECK(find_node(v, pb_b, a_sht)->attach_order > find_node(v, pb_a, a_sht)->attach_order,
              "the most recent attach (Base B) has the larger attach_order");

        // Base A's detach tombstone arrives AFTER Base B already owns it.
        CHECK(reg.apply_removal(pb_a, a_sht, t0 + seconds(2)) == ApplyResult::AppliedNew, "A's tombstone applies");
        v = reg.view(t0 + seconds(2));
        CHECK(find_node(v, pb_a, a_sht) == nullptr, "the module is gone from Base A");
        CHECK(find_node(v, pb_b, a_sht) != nullptr && find_node(v, pb_b, a_sht)->online,
              "the module stays online under Base B -- A's tombstone never removes B's copy");
        CHECK(reg.apply_topology(pb_a, a_sht, pb_a, false, 1, 3, kSht45Bitmap, false, 7, 1, t0 + seconds(3)) ==
                  ApplyResult::PreconditionFailed,
              "a stale offline resync from Base A for the removed module is still rejected");
        CHECK(reg.apply_topology(pb_b, a_sht, pb_b, true, 1, 3, kSht45Bitmap, false, 7, 6, t0 + seconds(3)) ==
                  ApplyResult::AppliedNew,
              "Base B's own reports for it keep applying normally");
    }

    printf("\n-- scenario: an offline -> online report bumps attach_order; stale recovery does not --\n");
    {
        Registry reg(milliseconds(120000));
        reg.apply_topology(pb_a, a_sht, pb_a, true, 1, 3, kSht45Bitmap, false, 7, 1, t0);
        uint64_t first = reg.view(t0).nodes[0].attach_order;
        CHECK(first > 0, "a first online sighting gets an attach_order");
        reg.apply_topology(pb_a, a_sht, pb_a, true, 1, 3, kSht45Bitmap, false, 7, 2, t0);
        CHECK(reg.view(t0).nodes[0].attach_order == first, "an online -> online update keeps it");
        reg.apply_topology(pb_a, a_sht, pb_a, false, 1, 3, kSht45Bitmap, false, 7, 3, t0);
        reg.apply_topology(pb_a, a_sht, pb_a, true, 1, 3, kSht45Bitmap, false, 7, 4, t0);
        CHECK(reg.view(t0).nodes[0].attach_order > first, "a reported re-attach (offline -> online) raises it");
        reg.apply_topology(pb_a, a_sht, pb_a, true, 1, 3, kSht45Bitmap, false, 99, 1, t0);
        CHECK(reg.view(t0).nodes[0].attach_order > first + 1, "a new boot session reporting online raises it");
    }

    printf("\n-- scenario: single-Base behavior is unchanged --\n");
    {
        Registry reg(milliseconds(120000));
        reg.apply_topology(pb_a, a_sht, pb_a, true, 1, 3, kSht45Bitmap, false, 7, 1, t0);
        CHECK(reg.generation() == 1, "one report, one generation, even though it also introduced Base A");
        CHECK(reg.apply_topology(pb_a, a_sht, pb_a, true, 1, 3, kSht45Bitmap, false, 7, 1, t0) ==
                  ApplyResult::DuplicateExact,
              "an unchanged resync is a duplicate");
        CHECK(reg.generation() == 1, "and does not advance generation()");
        reg.apply_heartbeat(pb_a, 5000, 1, t0);
        CHECK(reg.generation() == 1, "a heartbeat from the already-known, online Base changes nothing");
        CHECK(reg.apply_removal(pb_a, a_sht, t0) == ApplyResult::AppliedNew, "removal applies");
        CHECK(reg.apply_removal(pb_a, a_sht, t0) == ApplyResult::DuplicateExact, "a repeated tombstone is idempotent");
        CHECK(reg.node_count() == 0, "the module is gone");
        CHECK(reg.apply_topology(pb_a, a_sht, pb_a, true, 1, 3, kSht45Bitmap, false, 8, 1, t0) == ApplyResult::AppliedNew,
              "a re-dock (online=true) re-registers it");
        CHECK(reg.view(t0).power_bases.size() == 1, "still exactly one Power Base");
    }

    printf("\n-- scenario: the Power Base table is bounded --\n");
    {
        Registry reg(milliseconds(1000));
        for (size_t i = 0; i < Registry::kMaxPowerBases; i++) {
            reg.apply_heartbeat(make_uuid(static_cast<uint8_t>(0x40 + i)), 0, 0, t0);
        }
        reg.apply_heartbeat(make_uuid(0x7F), 0, 0, t0);
        CHECK(reg.view(t0).power_bases.size() == Registry::kMaxPowerBases,
              "a 17th Base is not tracked while all 16 are online");
        reg.apply_heartbeat(make_uuid(0x7F), 0, 0, t0 + seconds(5));
        RegistryView v = reg.view(t0 + seconds(5));
        CHECK(v.power_bases.size() == Registry::kMaxPowerBases && find_base(v, make_uuid(0x7F)) != nullptr,
              "once others are offline with no modules, the oldest is evicted to make room");
    }

    printf("\n-- scenario: two Bases through ONE real ingest server over UDP loopback --\n");
    {
        constexpr uint16_t kPort = 15694;
        Registry reg;
        std::atomic<bool> stop{false};
        ingest::ServerConfig cfg;
        cfg.port = kPort;
        cfg.interface_name = resolve_loopback_interface_name();
        std::thread server([&] { ingest::run_server(cfg, reg, &stop); });
        std::this_thread::sleep_for(milliseconds(150));

        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(kPort);
        inet_pton(AF_INET6, "::1", &addr.sin6_addr);
        timeval tv{2, 0};
        int base_a = socket(AF_INET6, SOCK_DGRAM, 0);
        int base_b = socket(AF_INET6, SOCK_DGRAM, 0);
        setsockopt(base_a, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(base_b, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        CHECK(post(base_a, addr, "pogo/v1/heartbeat", heartbeat_wire(pb_a), 1) == ingest::kCoapCodeChanged,
              "Base A's heartbeat is ACKed 2.04");
        CHECK(post(base_b, addr, "pogo/v1/heartbeat", heartbeat_wire(pb_b), 1) == ingest::kCoapCodeChanged,
              "Base B's heartbeat (different source port, same message id) is ACKed 2.04");
        CHECK(post(base_a, addr, "pogo/v1/topology", topology_wire(pb_a, a_sht, kSht45Bitmap), 2) ==
                  ingest::kCoapCodeChanged,
              "Base A's SHT45 topology is ACKed 2.04");
        CHECK(post(base_b, addr, "pogo/v1/topology", topology_wire(pb_b, b_sht, kSht45Bitmap), 2) ==
                  ingest::kCoapCodeChanged,
              "Base B's SHT45 topology is ACKed 2.04 -- not mistaken for a retransmit of A's");

        RegistryView v = reg.view();
        CHECK(v.power_bases.size() == 2, "one server instance tracks both Bases");
        CHECK(find_node(v, pb_a, a_sht) && find_node(v, pb_b, b_sht) && v.nodes.size() == 2,
              "each module is stored under the Base that sent it");

        stop = true;
        server.join();
        close(base_a);
        close(base_b);
    }

    std::printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
