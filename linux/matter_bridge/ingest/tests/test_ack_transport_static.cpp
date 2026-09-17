// Static source check (no sockets, no server instantiation): this follow-up
// requires the Confirmable ACK path in src/server.cpp to reply via
// sendmsg()+outgoing IPV6_PKTINFO (pinning the ACK's source address/egress
// interface) rather than a plain sendto() that lets the kernel pick any
// address configured on the interface -- see server.cpp's own comment above
// the ACK-sending code for the full rationale (a Power Base that sent to a
// specific OTBR MLEID will not recognize an ACK the kernel answered from a
// different address on the same interface, e.g. an RLOC/ALOC/OMR).
//
// A behavioral (socket-based) regression for this lives in
// test_loopback.cpp ("the ACK's source IPv6 address matches..."), but that
// test alone can't distinguish the fixed code from the original bug on a
// machine whose loopback interface only ever has one IPv6 address (::1) --
// wpan0's multi-address situation isn't reproducible there without root to
// alias extra addresses onto an interface. This static check instead reads
// the actual production source text and asserts the specific APIs it must
// use, so a regression back to sendto() fails a test even on such a
// machine.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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

size_t count_occurrences(const std::string &haystack, const std::string &needle)
{
    size_t count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// Strips `//` and `/* */` comments (this file's own top comment discusses
// the very API names -- "sendto()", "sendmsg()" -- being checked for, so
// scanning raw source text would false-positive on prose, not code). Does
// not need to understand string/char literals: server.cpp has none that
// contain "//" or "/*".
std::string strip_comments(const std::string &src)
{
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size();) {
        if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') {
                ++i;
            }
        } else if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) {
                ++i;
            }
            i += 2;
        } else {
            out.push_back(src[i]);
            ++i;
        }
    }
    return out;
}

} // namespace

int main()
{
#ifndef INGEST_SERVER_CPP_PATH
#error "INGEST_SERVER_CPP_PATH must be defined by CMake to the absolute path of src/server.cpp"
#endif
    std::ifstream in(INGEST_SERVER_CPP_PATH);
    CHECK(in.good(), "src/server.cpp opens for reading at the path CMake supplied");

    std::ostringstream ss;
    ss << in.rdbuf();
    std::string raw_src = ss.str();

    CHECK(!raw_src.empty(), "src/server.cpp is non-empty");

    std::string src = strip_comments(raw_src);

    CHECK(count_occurrences(src, "sendto(") == 0,
          "server.cpp never calls sendto() anywhere -- the ACK path must use sendmsg() so its outgoing source "
          "address/interface can be pinned via ancillary IPV6_PKTINFO");

    CHECK(count_occurrences(src, "sendmsg(") >= 1, "server.cpp calls sendmsg() to send the ACK");

    // IPV6_PKTINFO appears at least twice: once identifying the cmsg_type
    // to look for among the RECEIVED datagram's ancillary data, and once
    // setting the cmsg_type on the OUTGOING ancillary data attached to the
    // ACK's sendmsg() call.
    CHECK(count_occurrences(src, "IPV6_PKTINFO") >= 2,
          "server.cpp references IPV6_PKTINFO for both the incoming datagram's ancillary data and the "
          "outgoing ACK's ancillary data");

    CHECK(src.find("ipi6_addr") != std::string::npos,
          "server.cpp reads/sets in6_pktinfo::ipi6_addr (the exact local destination address of the "
          "request, reused as the ACK's pinned source address)");

    CHECK(src.find("ipi6_ifindex") != std::string::npos,
          "server.cpp reads/sets in6_pktinfo::ipi6_ifindex (used both to verify the receiving interface and "
          "to pin the ACK's egress interface)");

    CHECK(src.find("MSG_TRUNC") != std::string::npos && src.find("MSG_CTRUNC") != std::string::npos,
          "server.cpp checks both MSG_TRUNC and MSG_CTRUNC on the received datagram before ever inspecting "
          "its ancillary data or applying it to registry state");

    std::printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
