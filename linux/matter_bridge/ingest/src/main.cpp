// Smoke-test executable for the Power Base -> Linux OTBR northbound bridge
// ingest program (this follow-up). Production usage on the Linux OTBR host:
//
//   ./ingest_server --interface wpan0 [--port 5684]
//
// --interface is REQUIRED (this follow-up's correction pass, item 2): the
// server only ever accepts datagrams actually received on that ONE local
// network interface -- see ingest/server.hpp's own SECURITY NOTE for why,
// and why there is no "listen on everything" default.
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
#include <cstring>
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

void print_usage(const char *argv0)
{
    std::fprintf(stderr, "usage: %s --interface <ifname> [--port <port>]\n", argv0);
}
} // namespace

int main(int argc, char **argv)
{
    ingest::ServerConfig config;
    config.port = 5684;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--interface") == 0 && i + 1 < argc) {
            config.interface_name = argv[++i];
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            int parsed = std::atoi(argv[++i]);
            if (parsed <= 0 || parsed > 0xFFFF) {
                std::fprintf(stderr, "%s: --port must be 1..65535\n", argv[0]);
                print_usage(argv[0]);
                return 1;
            }
            config.port = static_cast<uint16_t>(parsed);
        } else {
            std::fprintf(stderr, "%s: unrecognized argument '%s'\n", argv[0], argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (config.interface_name.empty()) {
        std::fprintf(stderr, "%s: --interface is required (this program only ever listens on ONE named local "
                              "interface -- see ingest/server.hpp's own SECURITY NOTE)\n",
                     argv[0]);
        print_usage(argv[0]);
        return 1;
    }

    std::signal(SIGINT, handle_sigint);
    std::signal(SIGTERM, handle_sigint);

    ingest::Registry registry;
    std::printf("pogo bridge ingest: starting on interface '%s' UDP/IPv6 port %u (resources: pogo/v1/sample, "
                "pogo/v1/topology, pogo/v1/heartbeat)\n",
                config.interface_name.c_str(), (unsigned)config.port);
    if (!ingest::run_server(config, registry, &g_stop)) {
        std::fprintf(stderr, "%s: failed to start (see the error above) -- exiting\n", argv[0]);
        return 1;
    }
    std::printf("pogo bridge ingest: stopped\n");
    return 0;
}
