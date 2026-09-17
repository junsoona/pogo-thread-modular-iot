// Exposes IPV6_RECVPKTINFO/IPV6_PKTINFO/in6_pktinfo on macOS/BSD (gated
// behind this macro there for historical RFC2292-vs-RFC3542 API reasons --
// see netinet6/in6.h). A no-op on Linux glibc, where these are already
// visible by default; must be defined before any system header is included.
#define __APPLE_USE_RFC_3542

#include "ingest/server.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ingest/minicoap.hpp"
#include "ingest/session.hpp"

namespace ingest {

namespace {

// Rate-limits a diagnostic log line to at most once per `interval` per call
// SITE (a distinct static per lambda/call site, since this is a template on
// the caller's own local static) -- never a per-datagram flood while, say,
// sendto() keeps failing or wrong-interface packets keep arriving.
bool rate_limit_ok(std::chrono::steady_clock::time_point &last, std::chrono::milliseconds interval)
{
    auto now = std::chrono::steady_clock::now();
    if (last.time_since_epoch().count() == 0 || (now - last) >= interval) {
        last = now;
        return true;
    }
    return false;
}

constexpr auto kLogRateLimit = std::chrono::milliseconds(5000);

} // namespace

bool run_server(const ServerConfig &config, Registry &registry, std::atomic<bool> *should_stop)
{
    if (config.interface_name.empty()) {
        std::fprintf(stderr, "ingest: refusing to start with no --interface given (see --help)\n");
        return false;
    }
    unsigned int required_ifindex = if_nametoindex(config.interface_name.c_str());
    if (required_ifindex == 0) {
        std::fprintf(stderr, "ingest: interface '%s' does not exist (if_nametoindex failed: %s)\n",
                     config.interface_name.c_str(), std::strerror(errno));
        return false;
    }

    int sock = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::perror("ingest: socket");
        return false;
    }

    int on = 1;
    if (setsockopt(sock, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof(on)) < 0) {
        std::perror("ingest: setsockopt(IPV6_RECVPKTINFO)");
        close(sock);
        return false;
    }

    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(config.port);
    if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        std::perror("ingest: bind");
        close(sock);
        return false;
    }

    // A short receive timeout so `should_stop` is checked periodically
    // instead of blocking forever in recvmsg() -- irrelevant for the
    // production entry point (should_stop == nullptr, runs forever anyway),
    // but is what lets the loopback test shut this down cleanly.
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200000; // 200ms
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::fprintf(stdout, "ingest: bound to interface '%s' (ifindex=%u) port=%u\n", config.interface_name.c_str(),
                 required_ifindex, (unsigned)config.port);

    std::chrono::steady_clock::time_point last_recv_err_log{};
    std::chrono::steady_clock::time_point last_wrong_iface_log{};
    std::chrono::steady_clock::time_point last_send_err_log{};

    uint8_t buf[2048];
    uint8_t cmsg_buf[256];
    while (should_stop == nullptr || !should_stop->load()) {
        sockaddr_in6 peer{};
        iovec iov{};
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        msghdr msg{};
        msg.msg_name = &peer;
        msg.msg_namelen = sizeof(peer);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);

        ssize_t n = recvmsg(sock, &msg, 0);
        if (n < 0) {
            // Correction pass: distinguish an expected recv timeout
            // (EAGAIN/EWOULDBLOCK, from SO_RCVTIMEO above -- silent, just
            // loop back to re-check should_stop) from a genuine socket
            // error (logged, rate-limited, and the loop keeps running --
            // never crashes or exits on a transient error).
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            if (rate_limit_ok(last_recv_err_log, kLogRateLimit)) {
                std::fprintf(stderr, "ingest: recvmsg error: %s\n", std::strerror(errno));
            }
            continue;
        }

        // Find IPV6_PKTINFO among the ancillary data to learn which LOCAL
        // interface this datagram actually arrived on -- see server.hpp's
        // own SECURITY NOTE for why this is enforced per-packet rather than
        // only at bind() time (bind() to in6addr_any accepts every
        // interface; this is what actually restricts it to one).
        unsigned int recv_ifindex = 0;
        bool have_pktinfo = false;
        for (cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO) {
                in6_pktinfo pi{};
                std::memcpy(&pi, CMSG_DATA(cmsg), sizeof(pi));
                recv_ifindex = pi.ipi6_ifindex;
                have_pktinfo = true;
                break;
            }
        }
        if (!have_pktinfo || recv_ifindex != required_ifindex) {
            if (rate_limit_ok(last_wrong_iface_log, kLogRateLimit)) {
                std::fprintf(stderr,
                             "ingest: dropped a datagram received on ifindex=%u (expected '%s'=%u, or no "
                             "IPV6_PKTINFO at all) -- see server.hpp's SECURITY NOTE\n",
                             recv_ifindex, config.interface_name.c_str(), required_ifindex);
            }
            continue;
        }

        CoapMessage req;
        ParseStatus status = coap_parse(buf, static_cast<size_t>(n), req);
        if (status == ParseStatus::Malformed) {
            std::printf("[malformed] failed to parse CoAP datagram (%zd bytes) -- dropped\n", n);
            continue;
        }

        // Correction pass: "only a Confirmable POST changes state" means
        // exactly that -- a Non-confirmable (or Acknowledgement/Reset,
        // which this server should never receive as a request in the
        // first place) datagram must never reach handle_request() at all,
        // not merely go un-ACKed after the registry was already mutated.
        // Dropped here, before ANY method/option check or registry access.
        if (req.type != CoapType::Confirmable) {
            std::printf("[non-confirmable] uri=%s type=%d -- dropped: no state change, no response\n",
                        req.uri_path.c_str(), (int)req.type);
            continue;
        }

        uint8_t code;
        if (status == ParseStatus::UnknownCriticalOption) {
            // The header (type/message_id/token) parsed fine -- only an
            // option was unrecognized-and-critical -- so a proper 4.02 ACK
            // can still be built and sent (RFC 7252 5.4.1).
            std::printf("[bad-option] uri=%s -- unrecognized critical option, rejected (4.02)\n",
                        req.uri_path.c_str());
            code = kCoapCodeBadOption;
        } else if (req.code != kCoapCodePost) {
            // Confirmable but not POST -- 4.05 (the non-Confirmable case is
            // already handled above and never reaches here).
            std::printf("[method-not-allowed] uri=%s code=0x%02x -- rejected (4.05)\n", req.uri_path.c_str(),
                        (unsigned)req.code);
            code = kCoapCodeMethodNotAllowed;
        } else {
            code = handle_request(registry, req);
        }

        // Every request reaching this point is Confirmable and therefore
        // always gets a response -- the response is sent only AFTER
        // handle_request() (if called at all) has fully applied the state
        // change (this follow-up's own explicit ordering requirement).
        std::vector<uint8_t> ack = coap_build_ack(req, code);
        ssize_t sent =
            sendto(sock, ack.data(), ack.size(), 0, reinterpret_cast<sockaddr *>(&peer), msg.msg_namelen);
        if (sent < 0 && rate_limit_ok(last_send_err_log, kLogRateLimit)) {
            std::fprintf(stderr, "ingest: sendto() failed while ACKing uri=%s: %s\n", req.uri_path.c_str(),
                         std::strerror(errno));
        }
    }
    close(sock);
    return true;
}

} // namespace ingest
