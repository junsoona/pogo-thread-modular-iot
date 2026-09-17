// Local loopback test: starts the REAL ingest::run_server() (the exact
// production UDP/CoAP server loop, server.cpp) on a high test port, sends
// real UDP datagrams to ::1 from a plain client socket built with
// bridge_export_pack_*() payloads, and verifies both the CoAP ACK's
// response code AND that the registry ends up in the expected state --
// exercising the full receive path (socket -> minicoap parse -> session ->
// registry -> minicoap ACK -> socket) exactly as a real Power Base's
// linux_bridge_exporter would drive it, no mocking of any layer.
//
// Correction pass (this follow-up, item 2): explicitly resolves and uses
// the machine's real loopback interface name (tries "lo" then "lo0", since
// the name differs between Linux and macOS) rather than any implicit "any
// interface" default -- ServerConfig::interface_name is a required field.
// Also verifies the interface-restriction filter itself: a second server
// bound to a DIFFERENT real interface (if this machine has one) never
// responds to a datagram sent over loopback.
//
// This test builds its own minimal CoAP REQUEST encoder (production code
// never needs to build a request, only parse one and build an ACK -- see
// minicoap.hpp) purely to act as the "sender" side for this test.
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
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

namespace {

// Test-only CoAP request encoder: version=1, type, code=POST, a fixed
// message_id/token, one Uri-Path option per '/'-separated segment, a
// payload marker, then the payload -- exactly what
// components/linux_bridge_exporter's real otCoapSendRequestWithParameters()
// call produces on the wire (see that component's send_one_attempt()).
std::vector<uint8_t> build_request(const std::string &uri_path, const std::vector<uint8_t> &payload,
                                    uint16_t message_id, ingest::CoapType type = ingest::CoapType::Confirmable,
                                    uint8_t code = ingest::kCoapCodePost)
{
    std::vector<uint8_t> out;
    uint8_t token = 0x42;
    out.push_back(static_cast<uint8_t>((1 << 6) | (static_cast<uint8_t>(type) << 4) | 1));
    out.push_back(code);
    out.push_back(static_cast<uint8_t>(message_id >> 8));
    out.push_back(static_cast<uint8_t>(message_id & 0xFF));
    out.push_back(token);

    // Split uri_path on '/' and append one Uri-Path option (number 11) per
    // segment, deltas encoded directly (every segment name here is short
    // enough that length <= 12, so no extended length encoding is needed).
    size_t start = 0;
    uint32_t last_option_number = 0;
    while (start <= uri_path.size()) {
        size_t slash = uri_path.find('/', start);
        std::string segment = uri_path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        uint32_t delta = 11 - last_option_number; // first segment: delta=11 (option 11); subsequent: delta=0
        last_option_number = 11;
        out.push_back(static_cast<uint8_t>((delta << 4) | (segment.size() & 0x0F)));
        out.insert(out.end(), segment.begin(), segment.end());
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }

    out.push_back(0xFF); // payload marker
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t> pack_heartbeat_wire(uint8_t pb_tag, uint32_t uptime_ms, uint16_t node_count)
{
    bridge_export_heartbeat_t hb;
    std::memset(&hb, 0, sizeof(hb));
    std::memset(hb.power_base_uuid, pb_tag, BRIDGE_EXPORT_UUID_LEN);
    hb.uptime_ms = uptime_ms;
    hb.node_count = node_count;
    uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
    size_t wlen = bridge_export_pack_heartbeat(&hb, wire, sizeof(wire));
    return std::vector<uint8_t>(wire, wire + wlen);
}

std::vector<uint8_t> pack_topology_wire(uint8_t uuid_tag, bool online, uint8_t depth, uint32_t generation,
                                         uint32_t last_seq)
{
    bridge_export_topology_node_t node;
    std::memset(&node, 0, sizeof(node));
    std::memset(node.power_base_uuid, 0xB0, BRIDGE_EXPORT_UUID_LEN);
    std::memset(node.module_uuid, uuid_tag, BRIDGE_EXPORT_UUID_LEN);
    std::memset(node.parent_uuid, 0xB0, BRIDGE_EXPORT_UUID_LEN);
    node.online = online;
    node.depth = depth;
    node.role = 3;
    node.sensor_bitmap = 0x0002;
    node.downstream_connected = false;
    node.boot_session_generation = generation;
    node.last_seq = last_seq;
    uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
    size_t wlen = bridge_export_pack_topology_node(&node, wire, sizeof(wire));
    return std::vector<uint8_t>(wire, wire + wlen);
}

// Resolves the loopback interface's real name on this machine -- "lo" on
// Linux, "lo0" on macOS/BSD. Aborts the test loudly if neither exists
// (every POSIX system this program targets has one).
std::string resolve_loopback_interface_name()
{
    if (if_nametoindex("lo") != 0) {
        return "lo";
    }
    if (if_nametoindex("lo0") != 0) {
        return "lo0";
    }
    std::fprintf(stderr, "test_loopback: neither 'lo' nor 'lo0' resolves on this machine -- cannot run\n");
    std::exit(1);
}

// Finds a real, non-loopback interface name on this machine, if any exists
// (used only for the interface-restriction negative test below) -- returns
// empty if none is found, in which case that one sub-test is skipped rather
// than failed (a machine with only a loopback interface is unusual but not
// a bug in the code under test).
std::string find_a_non_loopback_interface_name(const std::string &loopback_name)
{
    struct if_nameindex *list = if_nameindex();
    if (list == nullptr) {
        return "";
    }
    std::string found;
    for (struct if_nameindex *p = list; p->if_name != nullptr; p++) {
        if (loopback_name != p->if_name) {
            found = p->if_name;
            break;
        }
    }
    if_freenameindex(list);
    return found;
}

} // namespace

int main()
{
    constexpr uint16_t kTestPort = 15684;
    std::string loopback_name = resolve_loopback_interface_name();
    std::printf("(using loopback interface '%s' for this test)\n", loopback_name.c_str());

    printf("\n-- scenario: a nonexistent interface name fails startup cleanly (no silent 'listen everywhere' "
           "fallback) --\n");
    {
        ingest::Registry bogus_registry;
        ingest::ServerConfig bogus_config;
        bogus_config.port = kTestPort + 2;
        bogus_config.interface_name = "this-interface-does-not-exist-xyz";
        bool ok = ingest::run_server(bogus_config, bogus_registry, nullptr);
        CHECK(!ok, "run_server() returns false immediately for a nonexistent interface name");
    }

    ingest::Registry registry;
    std::atomic<bool> stop{false};
    ingest::ServerConfig config;
    config.port = kTestPort;
    config.interface_name = loopback_name;
    bool server_start_ok = true;
    std::thread server_thread([&] { server_start_ok = ingest::run_server(config, registry, &stop); });

    // Give the server a moment to bind before the first send.
    struct timespec settle = {0, 150000000L}; // 150ms
    nanosleep(&settle, nullptr);

    int client = socket(AF_INET6, SOCK_DGRAM, 0);
    CHECK(client >= 0, "client socket() succeeds");

    sockaddr_in6 server_addr{};
    server_addr.sin6_family = AF_INET6;
    server_addr.sin6_port = htons(kTestPort);
    inet_pton(AF_INET6, "::1", &server_addr.sin6_addr);

    // Client recv timeout so a bug that drops the ACK fails the test
    // quickly instead of hanging forever.
    timeval tv{};
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    printf("\n-- scenario: a real HEARTBEAT over an actual UDP loopback socket, on the named loopback "
           "interface, gets ACKed 2.04 --\n");
    {
        auto payload = pack_heartbeat_wire(0xAB, 5000, 2);
        auto request = build_request("pogo/v1/heartbeat", payload, 100);
        ssize_t sent = sendto(client, request.data(), request.size(), 0, reinterpret_cast<sockaddr *>(&server_addr),
                               sizeof(server_addr));
        CHECK(sent == static_cast<ssize_t>(request.size()), "the request datagram was sent in full");

        uint8_t resp_buf[512];
        sockaddr_in6 from{};
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(client, resp_buf, sizeof(resp_buf), 0, reinterpret_cast<sockaddr *>(&from), &from_len);
        CHECK(n >= 4, "an ACK datagram was received over the real loopback socket");
        if (n >= 4) {
            uint8_t resp_code = resp_buf[1];
            uint16_t resp_mid = static_cast<uint16_t>((resp_buf[2] << 8) | resp_buf[3]);
            CHECK(resp_code == ingest::kCoapCodeChanged, "the ACK's response code is 2.04 Changed");
            CHECK(resp_mid == 100, "the ACK's message_id matches the request's");
        }
        // The core regression this follow-up fixes: `from` here is the
        // ACK datagram's actual IP-layer SOURCE address (recvfrom()'s
        // out-parameter reports who the packet came from, i.e. the source
        // address the server's sendmsg()+IPV6_PKTINFO pinned it to) -- it
        // must exactly match server_addr, the address this test's request
        // was sent TO, not some other address the kernel might otherwise
        // have chosen for an interface with more than one configured
        // address (this loopback interface has only ::1, but the same
        // sendmsg()+IPV6_PKTINFO mechanism is what pins the source address
        // on wpan0's MLEID/RLOC/ALOC/OMR too -- see
        // test_ack_transport_static.cpp for a check that specifically
        // guards against silently reverting to a plain sendto() here).
        CHECK(std::memcmp(&from.sin6_addr, &server_addr.sin6_addr, sizeof(in6_addr)) == 0,
              "the ACK's source IPv6 address exactly matches the destination address the request was sent to");
    }

    printf("\n-- scenario: an oversized Confirmable datagram that exceeds the server's receive buffer "
           "(MSG_TRUNC) is discarded outright -- no ACK, no registry mutation, and the server stays alive --\n");
    {
        size_t node_count_before = registry.node_count();

        // Larger than the server's fixed 2048-byte recv buffer (see
        // server.cpp's `uint8_t buf[2048]`) so the kernel sets MSG_TRUNC on
        // this recvmsg(). The content doesn't need to be a valid CoAP
        // message at all -- the server must discard it due to truncation
        // before ever attempting to parse it.
        std::vector<uint8_t> oversized(3000, 0xAA);

        timeval short_tv4{};
        short_tv4.tv_sec = 0;
        short_tv4.tv_usec = 300000; // 300ms -- long enough for any real ACK to have arrived
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &short_tv4, sizeof(short_tv4));

        ssize_t oversized_sent = sendto(client, oversized.data(), oversized.size(), 0,
                                         reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr));
        CHECK(oversized_sent == static_cast<ssize_t>(oversized.size()),
              "the oversized datagram was sent in full (truncation happens on the receiving side, not here)");

        uint8_t resp_buf[512];
        sockaddr_in6 from{};
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(client, resp_buf, sizeof(resp_buf), 0, reinterpret_cast<sockaddr *>(&from), &from_len);
        CHECK(n < 0, "no ACK is ever sent for a datagram the server received truncated (MSG_TRUNC)");

        struct timespec small_wait2 = {0, 50000000L}; // 50ms
        nanosleep(&small_wait2, nullptr);
        CHECK(registry.node_count() == node_count_before,
              "the truncated datagram never touched registry state");

        // Server must still be alive and answer a follow-up valid request.
        timeval tv5{};
        tv5.tv_sec = 2;
        tv5.tv_usec = 0;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv5, sizeof(tv5));
        auto payload = pack_heartbeat_wire(0xAB, 6500, 2);
        auto request = build_request("pogo/v1/heartbeat", payload, 106);
        sendto(client, request.data(), request.size(), 0, reinterpret_cast<sockaddr *>(&server_addr),
               sizeof(server_addr));
        uint8_t resp_buf2[512];
        sockaddr_in6 from2{};
        socklen_t from_len2 = sizeof(from2);
        ssize_t n2 = recvfrom(client, resp_buf2, sizeof(resp_buf2), 0, reinterpret_cast<sockaddr *>(&from2),
                               &from_len2);
        CHECK(n2 >= 4, "the server is still alive and answers a follow-up request after a truncated datagram");
    }

    printf("\n-- scenario: a malformed datagram over the real socket is dropped without crashing the server --\n");
    {
        std::vector<uint8_t> junk = {0xDE, 0xAD};
        sendto(client, junk.data(), junk.size(), 0, reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr));

        // The server must still be alive and answer a follow-up valid
        // request -- proves the malformed datagram didn't crash/wedge it.
        auto payload = pack_heartbeat_wire(0xAB, 6000, 2);
        auto request = build_request("pogo/v1/heartbeat", payload, 101);
        sendto(client, request.data(), request.size(), 0, reinterpret_cast<sockaddr *>(&server_addr),
               sizeof(server_addr));
        uint8_t resp_buf[512];
        sockaddr_in6 from{};
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(client, resp_buf, sizeof(resp_buf), 0, reinterpret_cast<sockaddr *>(&from), &from_len);
        CHECK(n >= 4, "the server is still alive and answers a follow-up request after a malformed datagram");
    }

    printf("\n-- scenario: a Non-confirmable request never gets an ACK --\n");
    {
        auto payload = pack_heartbeat_wire(0xAB, 7000, 2);
        auto request = build_request("pogo/v1/heartbeat", payload, 102, ingest::CoapType::NonConfirmable);
        sendto(client, request.data(), request.size(), 0, reinterpret_cast<sockaddr *>(&server_addr),
               sizeof(server_addr));

        timeval short_tv{};
        short_tv.tv_sec = 0;
        short_tv.tv_usec = 300000; // 300ms -- long enough for any real ACK to have arrived
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &short_tv, sizeof(short_tv));
        uint8_t resp_buf[512];
        sockaddr_in6 from{};
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(client, resp_buf, sizeof(resp_buf), 0, reinterpret_cast<sockaddr *>(&from), &from_len);
        CHECK(n < 0, "no response is ever sent for a Non-confirmable request");
    }

    printf("\n-- scenario (correction pass, item 1): a Non-confirmable TOPOLOGY request never changes registry "
           "state, not just 'goes un-ACKed' -- verified via the REAL run_server() + registry, not the "
           "heartbeat-only no-response check above --\n");
    {
        CHECK(registry.node_count() == 0, "sanity: the registry is still empty before this scenario (nothing "
                                            "prior in this test file ever POSTed a real TOPOLOGY_SNAPSHOT)");

        timeval short_tv3{};
        short_tv3.tv_sec = 0;
        short_tv3.tv_usec = 300000; // 300ms -- long enough for any real ACK to have arrived
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &short_tv3, sizeof(short_tv3));

        auto payload = pack_topology_wire(0x77, true, 1, 1, 1);
        auto request = build_request("pogo/v1/topology", payload, 104, ingest::CoapType::NonConfirmable);
        sendto(client, request.data(), request.size(), 0, reinterpret_cast<sockaddr *>(&server_addr),
               sizeof(server_addr));

        uint8_t resp_buf2[512];
        sockaddr_in6 from3{};
        socklen_t from_len3 = sizeof(from3);
        ssize_t n = recvfrom(client, resp_buf2, sizeof(resp_buf2), 0, reinterpret_cast<sockaddr *>(&from3),
                              &from_len3);
        CHECK(n < 0, "no response is ever sent for a Non-confirmable TOPOLOGY request");

        // Give the server loop a moment in case handle_request() were
        // (incorrectly) still being called asynchronously -- there is no
        // such asynchrony in this server, but this keeps the check honest
        // rather than racing the very next line against the recv above.
        struct timespec small_wait = {0, 50000000L}; // 50ms
        nanosleep(&small_wait, nullptr);
        CHECK(registry.node_count() == 0,
              "the registry was NEVER touched by the Non-confirmable TOPOLOGY request -- handle_request() must "
              "not have been called at all, not merely left un-ACKed after mutating state");
    }

    printf("\n-- scenario: a Confirmable POST TOPOLOGY request still applies normally (regression: the "
           "Non-confirmable fix above must not affect the Confirmable path) --\n");
    {
        timeval tv4{};
        tv4.tv_sec = 2;
        tv4.tv_usec = 0;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv4, sizeof(tv4));

        auto payload = pack_topology_wire(0x78, true, 1, 1, 1);
        auto request = build_request("pogo/v1/topology", payload, 105, ingest::CoapType::Confirmable);
        sendto(client, request.data(), request.size(), 0, reinterpret_cast<sockaddr *>(&server_addr),
               sizeof(server_addr));

        uint8_t resp_buf3[512];
        sockaddr_in6 from4{};
        socklen_t from_len4 = sizeof(from4);
        ssize_t n = recvfrom(client, resp_buf3, sizeof(resp_buf3), 0, reinterpret_cast<sockaddr *>(&from4),
                              &from_len4);
        CHECK(n >= 4, "a response was received for the Confirmable POST TOPOLOGY request");
        if (n >= 4) {
            CHECK(resp_buf3[1] == ingest::kCoapCodeChanged, "the response code is 2.04 Changed");
        }
        CHECK(registry.node_count() == 1, "the Confirmable POST DID apply to the registry (exactly the one new "
                                            "node from this scenario)");
    }

    printf("\n-- scenario: a non-POST method gets 4.05 Method Not Allowed --\n");
    {
        timeval tv2{};
        tv2.tv_sec = 2;
        tv2.tv_usec = 0;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof(tv2));

        auto payload = pack_heartbeat_wire(0xAB, 8000, 2);
        uint8_t fake_get_code = ingest::CoapCode(0, 1); // GET, not POST
        auto request = build_request("pogo/v1/heartbeat", payload, 103, ingest::CoapType::Confirmable,
                                      fake_get_code);
        sendto(client, request.data(), request.size(), 0, reinterpret_cast<sockaddr *>(&server_addr),
               sizeof(server_addr));
        uint8_t resp_buf[512];
        sockaddr_in6 from{};
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(client, resp_buf, sizeof(resp_buf), 0, reinterpret_cast<sockaddr *>(&from), &from_len);
        CHECK(n >= 4, "a response was received for the non-POST request");
        if (n >= 4) {
            CHECK(resp_buf[1] == ingest::kCoapCodeMethodNotAllowed, "the response code is 4.05 Method Not Allowed");
        }
    }

    close(client);
    stop.store(true);
    server_thread.join();
    CHECK(server_start_ok, "the loopback-interface server started successfully");

    printf("\n-- scenario: a server bound to a DIFFERENT real interface never responds to loopback traffic "
           "(interface restriction, this follow-up's correction pass item 2) --\n");
    {
        std::string other_iface = find_a_non_loopback_interface_name(loopback_name);
        if (other_iface.empty()) {
            printf("(skipped -- this machine has no second real network interface to test against)\n");
        } else {
            printf("(using '%s' as the deliberately-WRONG interface)\n", other_iface.c_str());
            ingest::Registry registry2;
            std::atomic<bool> stop2{false};
            ingest::ServerConfig config2;
            config2.port = kTestPort + 1;
            config2.interface_name = other_iface;
            bool start_ok2 = true;
            std::thread server_thread2([&] { start_ok2 = ingest::run_server(config2, registry2, &stop2); });
            nanosleep(&settle, nullptr);

            int client2 = socket(AF_INET6, SOCK_DGRAM, 0);
            sockaddr_in6 addr2{};
            addr2.sin6_family = AF_INET6;
            addr2.sin6_port = htons(kTestPort + 1);
            inet_pton(AF_INET6, "::1", &addr2.sin6_addr);
            timeval short_tv2{};
            short_tv2.tv_sec = 0;
            short_tv2.tv_usec = 400000;
            setsockopt(client2, SOL_SOCKET, SO_RCVTIMEO, &short_tv2, sizeof(short_tv2));

            auto payload = pack_heartbeat_wire(0xCD, 1000, 1);
            auto request = build_request("pogo/v1/heartbeat", payload, 200);
            sendto(client2, request.data(), request.size(), 0, reinterpret_cast<sockaddr *>(&addr2), sizeof(addr2));

            uint8_t resp_buf[512];
            sockaddr_in6 from2{};
            socklen_t from_len2 = sizeof(from2);
            ssize_t n = recvfrom(client2, resp_buf, sizeof(resp_buf), 0, reinterpret_cast<sockaddr *>(&from2),
                                  &from_len2);
            CHECK(n < 0, "a datagram sent over loopback gets NO response from a server bound to a different "
                          "interface -- it was silently dropped as wrong-interface, never reaching the registry");
            CHECK(registry2.node_count() == 0, "the wrong-interface datagram never reached the registry at all");

            close(client2);
            stop2.store(true);
            server_thread2.join();
            CHECK(start_ok2, "the other-interface server itself started successfully (the interface DOES exist "
                              "and is usable -- the drop above is the filter working, not a startup failure)");
        }
    }

    printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
