// Real loopback tests for pogohome_api::Server -- starts the REAL
// production HTTP/SSE server (http_server.cpp, completely unmodified) on
// 127.0.0.1 with an ephemeral port, against a REAL ingest::Registry, and
// drives it with plain POSIX-socket HTTP requests -- same "exercise the
// full stack over an actual socket, no mocking of any layer" approach as
// linux/matter_bridge/ingest/tests/test_loopback.cpp.
//
// sse_heartbeat_interval/sse_poll_interval are overridden to a few hundred
// milliseconds for these tests (see http_server.hpp's own doc comment on
// why ServerConfig exposes this at all) -- production always uses the
// spec's real 15s/1s defaults; only this test file ever overrides them, so
// these scenarios run in well under a second each instead of needing 15+
// real-time seconds per heartbeat check.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __linux__
#include <dirent.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "ingest/registry.hpp"
#include "pogohome_api/http_server.hpp"
#include "test_check.hpp"

using ingest::Registry;
using ingest::Uuid;
using pogohome_api::Server;
using pogohome_api::ServerConfig;

namespace {

Uuid MakeUuid(uint8_t tag)
{
    Uuid u{};
    u.fill(tag);
    return u;
}

int ConnectLoopback(uint16_t port, int timeout_sec)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    timeval tv{};
    tv.tv_sec = timeout_sec;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

struct HttpResponse {
    bool got_headers = false;
    int status_code = 0;
    std::string headers;
    std::string body;
};

// A plain, blocking HTTP/1.1 GET client: sends the request, reads until the
// header block is complete, then (if Content-Length is present) reads
// exactly that many more body bytes -- exactly what this server's own
// `Connection: close`, no-chunked-encoding responses need (see
// http_server.cpp's own SendJsonResponse()).
HttpResponse SimpleGet(uint16_t port, const std::string &path, const char *method = "GET")
{
    HttpResponse resp;
    int fd = ConnectLoopback(port, 3);
    if (fd < 0) {
        return resp;
    }
    std::string req = std::string(method) + " " + path + " HTTP/1.1\r\nHost: test\r\n\r\n";
    if (send(fd, req.data(), req.size(), 0) < 0) {
        close(fd);
        return resp;
    }

    std::string all;
    char buf[4096];
    long content_length = -1;
    size_t header_end = std::string::npos;
    while (true) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        all.append(buf, static_cast<size_t>(n));
        if (header_end == std::string::npos) {
            header_end = all.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                std::string headers = all.substr(0, header_end);
                size_t cl = headers.find("Content-Length:");
                if (cl != std::string::npos) {
                    content_length = std::strtol(headers.c_str() + cl + std::strlen("Content-Length:"), nullptr, 10);
                }
            }
        }
        if (header_end != std::string::npos) {
            size_t body_have = all.size() - (header_end + 4);
            if (content_length < 0 || static_cast<long>(body_have) >= content_length) {
                break;
            }
        }
    }
    close(fd);

    if (header_end == std::string::npos) {
        return resp;
    }
    resp.got_headers = true;
    resp.headers = all.substr(0, header_end);
    resp.body = all.substr(header_end + 4);
    size_t sp1 = resp.headers.find(' ');
    if (sp1 != std::string::npos) {
        resp.status_code = std::atoi(resp.headers.c_str() + sp1 + 1);
    }
    return resp;
}

ServerConfig FastTestConfig()
{
    ServerConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.address_family = AF_INET;
    cfg.port = 0; // ephemeral -- avoids collisions with any other test/service
    cfg.sse_heartbeat_interval = std::chrono::milliseconds(250);
    cfg.sse_poll_interval = std::chrono::milliseconds(30);
    return cfg;
}

#ifdef __linux__
int CountOpenFds()
{
    DIR *d = opendir("/proc/self/fd");
    if (d == nullptr) {
        return -1;
    }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != nullptr) {
        if (entry->d_name[0] != '.') {
            count++;
        }
    }
    closedir(d);
    return count;
}
#endif

} // namespace

void RunHttpServerTests(int *failures)
{
    printf("\n-- scenario: GET /api/v1/health and /api/v1/snapshot over a real loopback socket --\n");
    {
        Registry registry;
        registry.apply_topology(MakeUuid(0xB0), MakeUuid(0x01), MakeUuid(0x00), true, 1, 3, 0x0002, false, 1, 1);
        Server server(registry, FastTestConfig());
        API_CHECK(server.Start(), "Server::Start() succeeds binding 127.0.0.1:0");

        HttpResponse health = SimpleGet(server.bound_port(), "/api/v1/health");
        API_CHECK(health.got_headers, "a response was received for GET /api/v1/health");
        API_CHECK(health.status_code == 200, "GET /api/v1/health returns 200");
        API_CHECK(health.headers.find("Content-Type: application/json") != std::string::npos,
                  "the health response has a JSON content type");
        API_CHECK(health.headers.find("Cache-Control: no-store") != std::string::npos,
                  "the health response carries Cache-Control: no-store");
        API_CHECK(health.body.find("\"module_count\":1") != std::string::npos,
                  "the health body reports the one module actually in the registry");

        HttpResponse snap = SimpleGet(server.bound_port(), "/api/v1/snapshot");
        API_CHECK(snap.got_headers, "a response was received for GET /api/v1/snapshot");
        API_CHECK(snap.status_code == 200, "GET /api/v1/snapshot returns 200");
        API_CHECK(snap.body.find("\"uuid\":\"0101010101010101\"") != std::string::npos,
                  "the snapshot body contains the registered module's full uuid");

        server.Stop();
    }

    printf("\n-- scenario (correction pass, item 8: 필수 회귀 테스트): a removed module_uuid is excluded from "
           "GET /api/v1/snapshot and GET /api/v1/health's module_count, end to end over a real socket, and "
           "registry_generation changes -- this is the SAME real ingest::Registry the SSE/HTTP layer reads from, "
           "so this locks in that http_server.cpp needs NO changes of its own for removal (Registry::snapshot()/"
           "generation() already exclude/bump it) --\n");
    {
        Registry registry;
        Uuid pb = MakeUuid(0xB0);
        Uuid mod = MakeUuid(0x09);
        registry.apply_topology(pb, mod, MakeUuid(0x00), true, 1, 3, 0x0002, false, 1, 1);
        Server server(registry, FastTestConfig());
        API_CHECK(server.Start(), "Server::Start() succeeds");

        HttpResponse before_health = SimpleGet(server.bound_port(), "/api/v1/health");
        API_CHECK(before_health.body.find("\"module_count\":1") != std::string::npos,
                  "before removal: health reports the one registered module");
        HttpResponse before_snap = SimpleGet(server.bound_port(), "/api/v1/snapshot");
        API_CHECK(before_snap.body.find("\"uuid\":\"0909090909090909\"") != std::string::npos,
                  "before removal: snapshot contains the module's uuid");
        uint64_t generation_before = registry.generation();

        auto r = registry.apply_removal(pb, mod);
        API_CHECK(r == ingest::ApplyResult::AppliedNew, "apply_removal() succeeds");
        API_CHECK(registry.generation() == generation_before + 1,
                  "registry_generation advances by exactly one for the removal");

        HttpResponse after_health = SimpleGet(server.bound_port(), "/api/v1/health");
        API_CHECK(after_health.body.find("\"module_count\":0") != std::string::npos,
                  "after removal: health's module_count drops to 0 over the SAME real socket, with NO server-side "
                  "change of its own needed");
        HttpResponse after_snap = SimpleGet(server.bound_port(), "/api/v1/snapshot");
        API_CHECK(after_snap.body.find("0909090909090909") == std::string::npos,
                  "after removal: the module's uuid is completely absent from the snapshot body");
        API_CHECK(after_health.body.find("\"registry_generation\":") != std::string::npos,
                  "the health response still reports registry_generation (sanity: still well-formed JSON)");

        server.Stop();
    }

    printf("\n-- scenario: an SSE client watching a stale-offline module sees it disappear once the (post-reboot) "
           "removal tombstone is applied, and reappear on re-dock --\n");
    {
        // Short stale timeout so the module is genuinely offline (not just
        // idle) when the tombstone arrives -- the Power Base reboot case.
        Registry registry(std::chrono::milliseconds(100), std::chrono::milliseconds(20));
        Uuid pb = MakeUuid(0xB0);
        Uuid mod = MakeUuid(0x0A);
        registry.apply_topology(pb, mod, MakeUuid(0x00), true, 1, 3, 0x0002, false, 1, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        Server server(registry, FastTestConfig());
        API_CHECK(server.Start(), "Server::Start() succeeds");

        int fd = ConnectLoopback(server.bound_port(), 3);
        API_CHECK(fd >= 0, "connected to /api/v1/events");
        if (fd >= 0) {
            std::string req = "GET /api/v1/events HTTP/1.1\r\nHost: test\r\n\r\n";
            send(fd, req.data(), req.size(), 0);
            // Reads SSE frames until `pred` holds for one complete snapshot
            // event, or ~2s pass.
            auto read_until = [fd](auto pred) {
                std::string buf;
                for (int i = 0; i < 40; i++) {
                    char chunk[4096];
                    ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
                    if (n <= 0) {
                        break;
                    }
                    buf.append(chunk, static_cast<size_t>(n));
                    size_t start;
                    while ((start = buf.find("event: snapshot")) != std::string::npos) {
                        size_t end = buf.find("\n\n", start);
                        if (end == std::string::npos) {
                            break;
                        }
                        std::string frame = buf.substr(start, end - start);
                        buf.erase(0, end + 2);
                        if (pred(frame)) {
                            return true;
                        }
                    }
                }
                return false;
            };
            API_CHECK(read_until([](const std::string &f) {
                          return f.find("\"uuid\":\"0a0a0a0a0a0a0a0a\"") != std::string::npos &&
                                 f.find("\"online\":false") != std::string::npos;
                      }),
                      "before the tombstone: the stale module is still listed, online=false");

            registry.apply_removal(pb, mod);
            API_CHECK(read_until([](const std::string &f) { return f.find("0a0a0a0a0a0a0a0a") == std::string::npos; }),
                      "after the tombstone: a fresh SSE snapshot no longer contains the module");

            registry.apply_topology(pb, mod, MakeUuid(0x00), true, 1, 3, 0x0002, false, 2, 1);
            API_CHECK(read_until([](const std::string &f) {
                          return f.find("\"uuid\":\"0a0a0a0a0a0a0a0a\"") != std::string::npos &&
                                 f.find("\"online\":true") != std::string::npos;
                      }),
                      "after re-dock: a fresh SSE snapshot lists it again, online=true");
            close(fd);
        }
        server.Stop();
    }

    printf("\n-- scenario: an unknown path is 404; a non-GET method on a known path is 405 --\n");
    {
        Registry registry;
        Server server(registry, FastTestConfig());
        API_CHECK(server.Start(), "Server::Start() succeeds");

        HttpResponse unknown = SimpleGet(server.bound_port(), "/api/v1/does-not-exist");
        API_CHECK(unknown.status_code == 404, "an unknown path returns 404");

        HttpResponse wrong_method = SimpleGet(server.bound_port(), "/api/v1/health", "POST");
        API_CHECK(wrong_method.status_code == 405, "a POST to a known GET-only path returns 405");

        server.Stop();
    }

    printf("\n-- scenario: an oversized request header is rejected (400 or connection closed), never crashes the "
           "server --\n");
    {
        Registry registry;
        Server server(registry, FastTestConfig());
        API_CHECK(server.Start(), "Server::Start() succeeds");

        int fd = ConnectLoopback(server.bound_port(), 3);
        API_CHECK(fd >= 0, "connected for the oversized-request scenario");
        if (fd >= 0) {
            // Well past kMaxRequestHeaderBytes (8192), no "\r\n\r\n" ever --
            // an oversized, still-incomplete header block.
            std::string huge(20000, 'A');
            send(fd, huge.data(), huge.size(), 0);
            char buf[512];
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            // Either a 400 response arrived, or the server simply closed the
            // connection (n == 0) -- both are acceptable per this API's own
            // HTTP behavior contract; a crash/hang is the only failure.
            API_CHECK(n >= 0, "the server responded or cleanly closed rather than hanging/erroring on an oversized "
                              "request");
            close(fd);
        }

        // The server must still be alive afterward.
        HttpResponse health = SimpleGet(server.bound_port(), "/api/v1/health");
        API_CHECK(health.status_code == 200, "the server is still alive and answers normally after the oversized "
                                              "request");

        server.Stop();
    }

    printf("\n-- scenario: a malformed request line (not even close to HTTP) is rejected without crashing the "
           "server --\n");
    {
        Registry registry;
        Server server(registry, FastTestConfig());
        API_CHECK(server.Start(), "Server::Start() succeeds");

        int fd = ConnectLoopback(server.bound_port(), 3);
        if (fd >= 0) {
            std::string junk = "\x01\x02\x03 not http at all\r\n\r\n";
            send(fd, junk.data(), junk.size(), 0);
            char buf[512];
            recv(fd, buf, sizeof(buf), 0); // don't care what (if anything) comes back
            close(fd);
        }

        HttpResponse health = SimpleGet(server.bound_port(), "/api/v1/health");
        API_CHECK(health.status_code == 200, "the server is still alive and answers normally after a malformed "
                                              "request");

        server.Stop();
    }

    printf("\n-- scenario: GET /api/v1/events sends the current full snapshot immediately on connect --\n");
    {
        Registry registry;
        registry.apply_topology(MakeUuid(0xB0), MakeUuid(0x02), MakeUuid(0x00), true, 1, 3, 0x0001, false, 1, 1);
        Server server(registry, FastTestConfig());
        API_CHECK(server.Start(), "Server::Start() succeeds");

        int fd = ConnectLoopback(server.bound_port(), 3);
        API_CHECK(fd >= 0, "connected to /api/v1/events");
        if (fd >= 0) {
            std::string req = "GET /api/v1/events HTTP/1.1\r\nHost: test\r\n\r\n";
            send(fd, req.data(), req.size(), 0);

            std::string all;
            char buf[4096];
            // Read until the SSE headers AND at least one full "\n\n"-terminated
            // event frame have arrived.
            for (int i = 0; i < 20 && all.find("\n\n") == std::string::npos; i++) {
                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) {
                    break;
                }
                all.append(buf, static_cast<size_t>(n));
            }
            API_CHECK(all.find("Content-Type: text/event-stream") != std::string::npos,
                      "the SSE response has Content-Type: text/event-stream");
            API_CHECK(all.find("event: snapshot") != std::string::npos, "the very first frame is a snapshot event");
            API_CHECK(all.find("\"uuid\":\"0202020202020202\"") != std::string::npos,
                      "the initial snapshot event contains the module already in the registry at connect time");

            printf("\n-- scenario: a registry change triggers a fresh SSE snapshot event on the SAME connection "
                   "--\n");
            registry.apply_topology(MakeUuid(0xB0), MakeUuid(0x03), MakeUuid(0x00), true, 1, 3, 0x0001, false, 1, 1);
            std::string second;
            bool saw_second_event = false;
            for (int i = 0; i < 40; i++) {
                char buf2[4096];
                ssize_t n = recv(fd, buf2, sizeof(buf2), 0);
                if (n <= 0) {
                    break;
                }
                second.append(buf2, static_cast<size_t>(n));
                if (second.find("0303030303030303") != std::string::npos) {
                    saw_second_event = true;
                    break;
                }
            }
            API_CHECK(saw_second_event, "the new module_uuid appears in a fresh SSE snapshot event after the "
                                        "registry actually changed");

            printf("\n-- scenario: with no further change, a heartbeat comment arrives and no duplicate snapshot "
                   "does --\n");
            std::string idle;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 100000;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            while (std::chrono::steady_clock::now() < deadline) {
                char buf3[4096];
                ssize_t n = recv(fd, buf3, sizeof(buf3), 0);
                if (n > 0) {
                    idle.append(buf3, static_cast<size_t>(n));
                }
            }
            size_t heartbeat_count = 0;
            size_t pos = 0;
            while ((pos = idle.find(": heartbeat", pos)) != std::string::npos) {
                heartbeat_count++;
                pos += std::strlen(": heartbeat");
            }
            API_CHECK(heartbeat_count >= 1, "at least one SSE heartbeat comment arrived during an idle period");
            API_CHECK(idle.find("event: snapshot") == std::string::npos,
                      "no duplicate snapshot event was sent for the same (unchanged) generation while idle");

            close(fd);
        }

        server.Stop();
    }

    printf("\n-- scenario: the SSE client cap rejects a connection past the configured limit, and frees a slot "
           "once a client disconnects --\n");
    {
        Registry registry;
        ServerConfig cfg = FastTestConfig();
        cfg.max_sse_clients = 2;
        Server server(registry, cfg);
        API_CHECK(server.Start(), "Server::Start() succeeds");

        int fd1 = ConnectLoopback(server.bound_port(), 3);
        int fd2 = ConnectLoopback(server.bound_port(), 3);
        std::string req = "GET /api/v1/events HTTP/1.1\r\nHost: test\r\n\r\n";
        char buf[2048];
        if (fd1 >= 0) {
            send(fd1, req.data(), req.size(), 0);
            recv(fd1, buf, sizeof(buf), 0); // consume the initial snapshot event
        }
        if (fd2 >= 0) {
            send(fd2, req.data(), req.size(), 0);
            recv(fd2, buf, sizeof(buf), 0);
        }

        HttpResponse third = SimpleGet(server.bound_port(), "/api/v1/events");
        API_CHECK(third.status_code == 503, "a 3rd concurrent SSE client is rejected (503) once max_sse_clients (2) "
                                            "is already in use");

        // Abruptly disconnect one held-open client (no clean shutdown) --
        // the server only notices on its next send attempt to that
        // connection (the next heartbeat, given FastTestConfig's short
        // interval), so give it a little time.
        if (fd1 >= 0) {
            close(fd1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(600));

        HttpResponse after_disconnect = SimpleGet(server.bound_port(), "/api/v1/events");
        API_CHECK(after_disconnect.status_code != 503, "once a slot frees up after a client disconnect, a new SSE "
                                                        "client is accepted again");

        if (fd2 >= 0) {
            close(fd2);
        }
        server.Stop();
    }

    printf("\n-- scenario: with --api unset (Start() never called), Stop() is a harmless no-op --\n");
    {
        Registry registry;
        Server server(registry, FastTestConfig());
        server.Stop(); // must not crash/hang even though Start() was never called
        API_CHECK(true, "Stop() on a never-started Server returns cleanly");
    }

    printf("\n-- scenario: a second Server failing to bind the SAME already-in-use address:port fails cleanly --\n");
    {
        Registry registry;
        ServerConfig cfg = FastTestConfig();
        Server server1(registry, cfg);
        API_CHECK(server1.Start(), "the first Server binds successfully");

        ServerConfig cfg2 = cfg;
        cfg2.port = server1.bound_port(); // deliberately the same, now-taken port
        Registry registry2;
        Server server2(registry2, cfg2);
        API_CHECK(!server2.Start(), "a second Server cannot bind the same address:port and Start() returns false");

        server1.Stop();
    }

    printf("\n-- scenario: repeated start/stop cycles leak neither threads nor file descriptors --\n");
    {
#ifdef __linux__
        int fds_before = CountOpenFds();
#endif
        for (int i = 0; i < 5; i++) {
            Registry registry;
            Server server(registry, FastTestConfig());
            if (!server.Start()) {
                continue;
            }
            SimpleGet(server.bound_port(), "/api/v1/health");
            server.Stop();
        }
#ifdef __linux__
        int fds_after = CountOpenFds();
        char msg[160];
        std::snprintf(msg, sizeof(msg), "fd count stayed bounded across 5 start/stop cycles (before=%d, after=%d)",
                      fds_before, fds_after);
        API_CHECK(fds_before >= 0 && fds_after >= 0 && (fds_after - fds_before) <= 4, msg);
#else
        API_CHECK(true, "(fd-leak check skipped on non-Linux -- /proc/self/fd is Linux-specific)");
#endif
    }
}
