#include "ingest/server.hpp"

#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ingest/minicoap.hpp"
#include "ingest/session.hpp"

namespace ingest {

void run_server(uint16_t port, Registry &registry, std::atomic<bool> *should_stop)
{
    int sock = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::perror("ingest: socket");
        return;
    }

    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);
    if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        std::perror("ingest: bind");
        close(sock);
        return;
    }

    // A short receive timeout so `should_stop` is checked periodically
    // instead of blocking forever in recvfrom() -- irrelevant for the
    // production entry point (should_stop == nullptr, runs forever anyway),
    // but is what lets the loopback test shut this down cleanly.
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200000; // 200ms
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[2048];
    while (should_stop == nullptr || !should_stop->load()) {
        sockaddr_in6 peer{};
        socklen_t peer_len = sizeof(peer);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&peer), &peer_len);
        if (n < 0) {
            continue; // recv timeout or a transient error -- just re-check should_stop and loop
        }
        CoapMessage req;
        if (!coap_parse(buf, static_cast<size_t>(n), req)) {
            std::printf("[malformed] failed to parse CoAP datagram (%zd bytes) -- dropped\n", n);
            continue;
        }
        uint8_t code = handle_request(registry, req);
        // A Confirmable request always gets a response; Non-confirmable
        // never does -- same policy as components/power_base_transport's
        // own send_coap_response(). Response is sent only AFTER
        // handle_request() has fully applied the state change (this
        // follow-up's own explicit ordering requirement).
        if (req.type == CoapType::Confirmable) {
            std::vector<uint8_t> ack = coap_build_ack(req, code);
            sendto(sock, ack.data(), ack.size(), 0, reinterpret_cast<sockaddr *>(&peer), peer_len);
        }
    }
    close(sock);
}

} // namespace ingest
