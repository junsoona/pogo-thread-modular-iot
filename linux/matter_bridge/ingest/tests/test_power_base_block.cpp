// Power Base management milestone (docs/power-base-management.md), the
// ingest side: a Power Base that was unenrolled is refused on the data path
// until it is unblocked, removing a Base removes exactly the nodes registered
// under it, and the management resource "pogo/v1/mgmt" is routed to its
// handler -- answer payload included -- without ever reaching the Registry.
// The last part runs over a real UDP loopback socket through the production
// server loop (server.cpp).
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "ingest/minicoap.hpp"
#include "ingest/registry.hpp"
#include "ingest/server.hpp"
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

namespace {

using ingest::ApplyResult;
using ingest::Uuid;

Uuid base(uint8_t tag)
{
    return Uuid{0x74, 0x4d, 0xbd, 0xff, 0xfe, 0x65, tag, 0x00};
}

Uuid module(uint8_t tag)
{
    return Uuid{0x74, 0x4d, 0xbd, 0xff, 0xfe, 0x65, 0x00, tag};
}

ApplyResult topo(ingest::Registry &r, const Uuid &b, const Uuid &m, uint32_t seq, bool online = true)
{
    return r.apply_topology(b, m, b, online, 1, 3, 0x0006, false, 1, seq);
}

bool has_node(const ingest::RegistryView &view, const Uuid &b, const Uuid &m)
{
    for (const auto &n : view.nodes) {
        if (n.power_base_uuid == b && n.module_uuid == m) {
            return true;
        }
    }
    return false;
}

bool has_base(const ingest::RegistryView &view, const Uuid &b)
{
    for (const auto &p : view.power_bases) {
        if (p.power_base_uuid == b) {
            return true;
        }
    }
    return false;
}

std::vector<uint8_t> heartbeat_wire(const Uuid &b)
{
    bridge_export_heartbeat_t hb;
    std::memset(&hb, 0, sizeof(hb));
    std::memcpy(hb.power_base_uuid, b.data(), 8);
    hb.uptime_ms = 1000;
    uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
    size_t n = bridge_export_pack_heartbeat(&hb, wire, sizeof(wire));
    return std::vector<uint8_t>(wire, wire + n);
}

std::vector<uint8_t> topology_wire(const Uuid &b, const Uuid &m, uint32_t seq)
{
    bridge_export_topology_node_t node;
    std::memset(&node, 0, sizeof(node));
    std::memcpy(node.power_base_uuid, b.data(), 8);
    std::memcpy(node.module_uuid, m.data(), 8);
    std::memcpy(node.parent_uuid, b.data(), 8);
    node.online = true;
    node.depth = 1;
    node.role = 3;
    node.sensor_bitmap = 0x0006;
    node.boot_session_generation = 1;
    node.last_seq = seq;
    uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
    size_t n = bridge_export_pack_topology_node(&node, wire, sizeof(wire));
    return std::vector<uint8_t>(wire, wire + n);
}

std::vector<uint8_t> removed_wire(const Uuid &b, const Uuid &m)
{
    bridge_export_topology_removed_t rem;
    std::memset(&rem, 0, sizeof(rem));
    std::memcpy(rem.power_base_uuid, b.data(), 8);
    std::memcpy(rem.module_uuid, m.data(), 8);
    uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
    size_t n = bridge_export_pack_topology_removed(&rem, wire, sizeof(wire));
    return std::vector<uint8_t>(wire, wire + n);
}

ingest::CoapMessage post(const std::string &uri, const std::vector<uint8_t> &payload)
{
    ingest::CoapMessage req;
    req.code = ingest::kCoapCodePost;
    req.uri_path = uri;
    req.payload = payload;
    return req;
}

// The same test-only request encoder as test_loopback.cpp.
std::vector<uint8_t> build_request(const std::string &uri_path, const std::vector<uint8_t> &payload, uint16_t message_id)
{
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>((1 << 6) | (0 << 4) | 1)); // version 1, Confirmable, 1-byte token
    out.push_back(ingest::kCoapCodePost);
    out.push_back(static_cast<uint8_t>(message_id >> 8));
    out.push_back(static_cast<uint8_t>(message_id & 0xFF));
    out.push_back(0x42);
    size_t start = 0;
    uint32_t last = 0;
    while (start <= uri_path.size()) {
        size_t slash = uri_path.find('/', start);
        std::string segment = uri_path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        out.push_back(static_cast<uint8_t>(((11 - last) << 4) | (segment.size() & 0x0F)));
        last = 11;
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

} // namespace

int main()
{
    printf("\n-- scenario: a blocked Power Base is refused by every apply function --\n");
    {
        ingest::Registry r;
        const Uuid a = base(0x0a), b = base(0x0b);
        r.apply_heartbeat(a, 1, 0);
        topo(r, a, module(1), 1);
        r.apply_heartbeat(b, 1, 0);
        topo(r, b, module(3), 1);
        CHECK(!r.is_power_base_blocked(a), "not blocked by default");
        r.set_power_base_blocked(a, true);
        r.set_power_base_blocked(a, true);
        CHECK(r.is_power_base_blocked(a) && !r.is_power_base_blocked(b), "blocking is per Base, and idempotent");

        const uint64_t generation = r.generation();
        const uint64_t serial = r.heartbeat_serial();
        std::vector<ingest::ChannelReading> channels = {{SENSOR_CHANNEL_TEMPERATURE, 1, 0, 23000}};
        CHECK(r.apply_heartbeat(a, 2, 1) == ApplyResult::Blocked, "heartbeat: Blocked");
        CHECK(topo(r, a, module(1), 2) == ApplyResult::Blocked, "topology of a known module: Blocked");
        CHECK(topo(r, a, module(9), 1) == ApplyResult::Blocked, "topology of a new module: Blocked");
        CHECK(r.apply_sample(a, module(1), 5, 100, 0x0002, channels) == ApplyResult::Blocked, "sample: Blocked");
        CHECK(r.apply_removal(a, module(1)) == ApplyResult::Blocked, "removal tombstone: Blocked");
        ingest::RegistryView view = r.view();
        CHECK(r.generation() == generation && r.heartbeat_serial() == serial && !has_node(view, a, module(9)) &&
                  has_node(view, a, module(1)) && view.nodes.size() == 2,
              "nothing changed: no generation bump, no heartbeat serial, no node added or erased");
        ingest::PowerBaseState state;
        CHECK(r.get_power_base(a, state) && state.last_heartbeat_serial < serial + 1,
              "the blocked Base did not get a new heartbeat serial (an enrollment session cannot be satisfied by it)");
        CHECK(r.apply_heartbeat(b, 2, 1) == ApplyResult::AppliedNew && topo(r, b, module(3), 2) == ApplyResult::AppliedNew,
              "the other Base is applied as always");

        r.set_power_base_blocked(a, false);
        CHECK(!r.is_power_base_blocked(a) && r.apply_heartbeat(a, 3, 1) == ApplyResult::AppliedNew &&
                  topo(r, a, module(9), 1) == ApplyResult::AppliedNew,
              "unblocked: its data is applied again");
    }

    printf("\n-- scenario: removing a Power Base removes exactly what is registered under it --\n");
    {
        ingest::Registry r;
        const Uuid a = base(0x0a), b = base(0x0b);
        r.apply_heartbeat(a, 1, 0);
        r.apply_heartbeat(b, 1, 0);
        topo(r, a, module(1), 1);
        topo(r, a, module(2), 1);
        topo(r, b, module(2), 1); // the same module, moved to B and registered there
        topo(r, b, module(3), 1);
        r.apply_removal(a, module(7)); // a removed-module marker of A
        const uint64_t generation = r.generation();

        std::vector<Uuid> erased = r.remove_power_base(a);
        ingest::RegistryView view = r.view();
        CHECK(erased.size() == 2 && ((erased[0] == module(1) && erased[1] == module(2)) ||
                                     (erased[0] == module(2) && erased[1] == module(1))),
              "it returns the modules that were registered under A");
        CHECK(!has_base(view, a) && !has_node(view, a, module(1)) && !has_node(view, a, module(2)),
              "A and A's nodes are gone");
        CHECK(has_base(view, b) && has_node(view, b, module(2)) && has_node(view, b, module(3)) && view.nodes.size() == 2,
              "B, the module that moved to B, and B's own module stay");
        CHECK(r.generation() == generation + 1, "one generation bump: watchers (SSE, Matter reconcile) see one change");
        CHECK(r.remove_power_base(a).empty() && r.generation() == generation + 1, "removing it again does nothing");
        CHECK(topo(r, a, module(7), 1, false) == ApplyResult::AppliedNew,
              "A's removed-module markers went with it (an offline report for that module is no longer refused)");

        const uint64_t before = r.generation();
        uint64_t seen = before;
        r.notify_external_change();
        CHECK(r.generation() == before + 1 && r.wait_for_change(&seen, std::chrono::milliseconds(10)) && seen == before + 1,
              "notify_external_change() advances the generation and wakes waiters");
    }

    printf("\n-- scenario: the session layer answers a blocked Base 4.03 and routes the management resource --\n");
    {
        ingest::Registry r;
        const Uuid a = base(0x0a);
        CHECK(ingest::handle_request(r, post("pogo/v1/heartbeat", heartbeat_wire(a))) == ingest::kCoapCodeChanged,
              "(a heartbeat is 2.04)");
        r.set_power_base_blocked(a, true);
        CHECK(ingest::handle_request(r, post("pogo/v1/heartbeat", heartbeat_wire(a))) == ingest::kCoapCodeForbidden,
              "blocked: heartbeat is 4.03");
        CHECK(ingest::handle_request(r, post("pogo/v1/topology", topology_wire(a, module(1), 1))) == ingest::kCoapCodeForbidden,
              "blocked: topology is 4.03");
        CHECK(ingest::handle_request(r, post("pogo/v1/topology", removed_wire(a, module(1)))) == ingest::kCoapCodeForbidden,
              "blocked: a removal tombstone is 4.03");
        CHECK(r.view().nodes.empty(), "and nothing was applied");

        const uint64_t generation = r.generation();
        int calls = 0;
        std::vector<uint8_t> seen;
        ingest::ManagementHandler handler = [&](const std::vector<uint8_t> &request) {
            calls++;
            seen = request;
            ingest::ManagementReply reply;
            reply.code = ingest::kCoapCodeContent;
            reply.payload = {0xde, 0xad, 0xbe, 0xef};
            return reply;
        };
        const std::vector<uint8_t> request = {1, 2, 3, 4, 5};
        ingest::Response response = ingest::handle_request(r, post(ingest::kManagementUriPath, request), handler);
        CHECK(calls == 1 && seen == request && response.code == ingest::kCoapCodeContent &&
                  response.payload == std::vector<uint8_t>({0xde, 0xad, 0xbe, 0xef}),
              "pogo/v1/mgmt: the payload goes to the handler as it arrived; its code and payload come back");
        CHECK(r.generation() == generation, "and the Registry is not touched by it");
        response = ingest::handle_request(r, post(ingest::kManagementUriPath, request), nullptr);
        CHECK(response.code == ingest::kCoapCodeNotFound && response.payload.empty(), "without a handler: 4.04, no payload");
        response = ingest::handle_request(r, post("pogo/v1/heartbeat", heartbeat_wire(base(0x0b))), handler);
        CHECK(calls == 1 && response.code == ingest::kCoapCodeChanged && response.payload.empty(),
              "a data resource never reaches the handler and never answers with a payload");
        CHECK(ingest::handle_request(r, post(ingest::kManagementUriPath, heartbeat_wire(a))) == ingest::kCoapCodeBadRequest,
              "the entry point without management still answers the resource 4.00 (what a hub without it does)");
    }

    printf("\n-- scenario: an Acknowledgement with a payload --\n");
    {
        ingest::CoapMessage req;
        req.message_id = 0x1234;
        req.token = {0x42};
        std::vector<uint8_t> plain = ingest::coap_build_ack(req, ingest::kCoapCodeChanged);
        CHECK(ingest::coap_build_ack(req, ingest::kCoapCodeChanged, {}) == plain,
              "an empty payload gives exactly the ACK without one (no marker)");
        std::vector<uint8_t> with = ingest::coap_build_ack(req, ingest::kCoapCodeContent, {0x01, 0x02});
        CHECK(with.size() == plain.size() + 3 && with[1] == ingest::kCoapCodeContent && with[plain.size()] == 0xFF &&
                  with[plain.size() + 1] == 0x01 && with[plain.size() + 2] == 0x02 && with[2] == 0x12 && with[3] == 0x34 &&
                  with[4] == 0x42,
              "with a payload: the same header and token, the 0xFF marker, the bytes");
        CHECK(ingest::kCoapCodeContent == 0x45 && ingest::kCoapCodeUnauthorized == 0x81 && ingest::kCoapCodeForbidden == 0x83,
              "2.05, 4.01 and 4.03 have their RFC 7252 values");
    }

    printf("\n-- scenario: the management resource over a real UDP socket, through the production server loop --\n");
    {
        std::string loopback = if_nametoindex("lo") != 0 ? "lo" : "lo0";
        constexpr uint16_t kPort = 15694;
        ingest::Registry r;
        std::atomic<bool> stop{false};
        std::atomic<int> calls{0};
        ingest::ServerConfig config;
        config.port = kPort;
        config.interface_name = loopback;
        config.management = [&](const std::vector<uint8_t> &request) {
            calls++;
            ingest::ManagementReply reply;
            if (request.size() == 3) {
                reply.code = ingest::kCoapCodeContent;
                reply.payload = {0xa1, 0xa2, 0xa3, 0xa4, 0xa5};
            } else {
                reply.code = ingest::kCoapCodeUnauthorized; // no payload
            }
            return reply;
        };
        bool started = true;
        std::thread server([&] { started = ingest::run_server(config, r, &stop); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        int client = socket(AF_INET6, SOCK_DGRAM, 0);
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(kPort);
        inet_pton(AF_INET6, "::1", &addr.sin6_addr);
        timeval tv{};
        tv.tv_sec = 2;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        auto exchange = [&](const std::vector<uint8_t> &request) {
            sendto(client, request.data(), request.size(), 0, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
            uint8_t buf[512];
            ssize_t n = recv(client, buf, sizeof(buf), 0);
            return n > 0 ? std::vector<uint8_t>(buf, buf + n) : std::vector<uint8_t>();
        };

        std::vector<uint8_t> ack = exchange(build_request("pogo/v1/mgmt", {7, 8, 9}, 300));
        CHECK(started && calls.load() == 1 && ack.size() == 5 + 1 + 5 && ack[1] == ingest::kCoapCodeContent &&
                  ack[2] == 0x01 && ack[3] == 0x2c && ack[4] == 0x42 && ack[5] == 0xFF && ack[6] == 0xa1 && ack[10] == 0xa5,
              "the answer is one ACK: 2.05, the request's message id and token, the marker, the handler's payload");
        ack = exchange(build_request("pogo/v1/mgmt", {7, 8, 9, 10}, 301));
        CHECK(calls.load() == 2 && ack.size() == 5 && ack[1] == ingest::kCoapCodeUnauthorized,
              "a refusal is an ACK with its code and no payload");
        ack = exchange(build_request("pogo/v1/heartbeat", heartbeat_wire(base(0x0c)), 302));
        CHECK(calls.load() == 2 && ack.size() == 5 && ack[1] == ingest::kCoapCodeChanged && has_base(r.view(), base(0x0c)),
              "a data resource on the same socket: applied, ACKed 2.04 without a payload, the handler not called");
        r.set_power_base_blocked(base(0x0c), true);
        const uint64_t generation = r.generation();
        ack = exchange(build_request("pogo/v1/heartbeat", heartbeat_wire(base(0x0c)), 303));
        CHECK(ack.size() == 5 && ack[1] == ingest::kCoapCodeForbidden && r.generation() == generation,
              "the blocked Base's heartbeat over the socket: 4.03, nothing applied");

        stop.store(true);
        server.join();
        close(client);
    }

    std::printf("\n-- block_and_remove_power_base_if_offline(): decided, made durable and applied as one step --\n");
    {
        using Outcome = ingest::Registry::OfflineRemoval;
        ingest::Registry r;
        const Uuid a = base(0x21), b = base(0x22);
        r.apply_heartbeat(a, 1, 0);
        r.apply_heartbeat(b, 1, 0);
        topo(r, a, module(1), 1);
        topo(r, b, module(2), 1);
        int commits = 0;
        auto stores = [&]() { commits++; return true; };
        auto cannot_store = [&]() { commits++; return false; };
        // Ten minutes on: nothing applied so far counts as heard.
        const auto later = std::chrono::steady_clock::now() + std::chrono::minutes(10);
        const uint64_t generation = r.generation();

        CHECK(r.block_and_remove_power_base_if_offline(a, stores) == Outcome::Online && commits == 0 &&
                  has_base(r.view(), a) && has_node(r.view(), a, module(1)) && !r.is_power_base_blocked(a) &&
                  r.generation() == generation,
              "a Base that is heard from: Online -- the commit is not even called and nothing changes");
        CHECK(r.block_and_remove_power_base_if_offline(a, cannot_store, nullptr, later) == Outcome::CommitFailed &&
                  commits == 1 && has_base(r.view(), a) && has_node(r.view(), a, module(1)) && !r.is_power_base_blocked(a) &&
                  r.generation() == generation,
              "offline, but the commit fails: CommitFailed -- not blocked, nothing erased, no generation bump");
        CHECK(r.block_and_remove_power_base_if_offline(a, std::function<bool()>(), nullptr, later) == Outcome::CommitFailed &&
                  !r.is_power_base_blocked(a) && has_base(r.view(), a),
              "without a commit there is nothing durable: the same");
        std::vector<Uuid> erased;
        CHECK(r.block_and_remove_power_base_if_offline(a, stores, &erased, later) == Outcome::Removed && commits == 2 &&
                  erased == std::vector<Uuid>{module(1)},
              "offline and committed: Removed, with the modules that were registered under it");
        CHECK(r.is_power_base_blocked(a) && !has_base(r.view(), a) && !has_node(r.view(), a, module(1)) &&
                  has_base(r.view(), b) && has_node(r.view(), b, module(2)) && !r.is_power_base_blocked(b) &&
                  r.generation() == generation + 1,
              "the Base is blocked and it and its node are gone; the other Base is untouched; one generation bump");
        CHECK(r.apply_heartbeat(a, 2, 0) == ApplyResult::Blocked && topo(r, a, module(1), 2) == ApplyResult::Blocked &&
                  !has_base(r.view(), a),
              "its late heartbeat and topology report are refused and list nothing");

        // A Base this Registry does not track (its table of Bases is full), known from its node.
        ingest::Registry full;
        for (uint8_t i = 0; i < ingest::Registry::kMaxPowerBases; i++) {
            full.apply_heartbeat(base(static_cast<uint8_t>(0x40 + i)), 1, 0);
        }
        const Uuid untracked = base(0x60);
        topo(full, untracked, module(9), 1);
        const int before = commits;
        CHECK(!has_base(full.view(), untracked) && has_node(full.view(), untracked, module(9)),
              "(the table of Bases is full: a 17th Base is known only from its online node)");
        CHECK(full.block_and_remove_power_base_if_offline(untracked, stores) == Outcome::Online && commits == before &&
                  has_node(full.view(), untracked, module(9)) && !full.is_power_base_blocked(untracked),
              "an untracked Base with an online node is heard from too: Online, nothing changes");
        CHECK(full.block_and_remove_power_base_if_offline(untracked, stores, &erased, later) == Outcome::Removed &&
                  erased == std::vector<Uuid>{module(9)} && full.is_power_base_blocked(untracked) &&
                  !has_node(full.view(), untracked, module(9)),
              "once its node is stale it is removed like any other");
    }

    std::printf("\n%s\n", g_failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_failures == 0 ? 0 : 1;
}
