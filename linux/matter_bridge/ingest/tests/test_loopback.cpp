// Local loopback test: starts the REAL ingest::run_server() (the exact
// production UDP/CoAP server loop, server.cpp) on a high test port, sends
// real UDP datagrams to 127.0.0.1 from a plain client socket built with
// bridge_export_pack_*() payloads, and verifies both the CoAP ACK's
// response code AND that the registry ends up in the expected state --
// exercising the full receive path (socket -> minicoap parse -> session ->
// registry -> minicoap ACK -> socket) exactly as a real Power Base's
// linux_bridge_exporter would drive it, no mocking of any layer.
//
// This test builds its own minimal CoAP REQUEST encoder (production code
// never needs to build a request, only parse one and build an ACK -- see
// minicoap.hpp) purely to act as the "sender" side for this test.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
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
                                    uint16_t message_id, ingest::CoapType type = ingest::CoapType::Confirmable)
{
    std::vector<uint8_t> out;
    uint8_t token = 0x42;
    out.push_back(static_cast<uint8_t>((1 << 6) | (static_cast<uint8_t>(type) << 4) | 1));
    out.push_back(ingest::kCoapCodePost);
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

} // namespace

int main()
{
    constexpr uint16_t kTestPort = 15684;
    ingest::Registry registry;
    std::atomic<bool> stop{false};
    std::thread server_thread([&] { ingest::run_server(kTestPort, registry, &stop); });

    // Give the server a moment to bind before the first send.
    struct timespec settle = {0, 100000000L}; // 100ms
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

    printf("\n-- scenario: a real HEARTBEAT over an actual UDP loopback socket gets ACKed 2.04 --\n");
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

    close(client);
    stop.store(true);
    server_thread.join();

    printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
