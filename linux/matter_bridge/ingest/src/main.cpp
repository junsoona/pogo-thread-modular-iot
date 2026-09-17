// Smoke-test executable for the Power Base -> Linux OTBR northbound bridge
// ingest program (this follow-up). Production usage on the Linux OTBR host:
//
//   ./ingest_server [port]     (default port: 5684)
//
// Prints one line per received sample/topology/heartbeat message to stdout
// (see ingest::handle_request()'s own doc comment for the exact fields --
// UUID suffixes only, never Dataset/key material, of which this program
// never has any in the first place). Runs forever; stop with Ctrl-C.
//
// This is NOT the Matter Bridge itself -- see docs/matter-bridge.md for the
// planned Matter endpoint mapping. The receiving/registry logic here
// (ingest::Registry) is deliberately kept separate from this stdout
// representation so a future Matter Bridge process can read the same
// registry logic directly (see registry.hpp's own top comment).
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <atomic>

#include "ingest/registry.hpp"
#include "ingest/server.hpp"

namespace {
std::atomic<bool> g_stop{false};
void handle_sigint(int)
{
    g_stop.store(true);
}
} // namespace

int main(int argc, char **argv)
{
    uint16_t port = 5684;
    if (argc > 1) {
        int parsed = std::atoi(argv[1]);
        if (parsed <= 0 || parsed > 0xFFFF) {
            std::fprintf(stderr, "usage: %s [port]\n", argv[0]);
            return 1;
        }
        port = static_cast<uint16_t>(parsed);
    }

    std::signal(SIGINT, handle_sigint);
    std::signal(SIGTERM, handle_sigint);

    ingest::Registry registry;
    std::printf("pogo bridge ingest: listening on UDP/IPv6 port %u (resources: pogo/v1/sample, pogo/v1/topology, "
                "pogo/v1/heartbeat)\n",
                (unsigned)port);
    ingest::run_server(port, registry, &g_stop);
    std::printf("pogo bridge ingest: stopped\n");
    return 0;
}
