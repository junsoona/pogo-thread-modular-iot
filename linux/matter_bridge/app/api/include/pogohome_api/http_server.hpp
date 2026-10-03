// PogoHome Local API v1 HTTP server -- GET /api/v1/health,
// GET /api/v1/snapshot, GET /api/v1/events (Server-Sent Events), and, when
// settings sharing is configured (ServerConfig::settings/device_auth/tls),
// the TLS-only pairing and home-settings routes. See
// docs/pogohome-api-v1.md for the full contract this implements.
//
// Plain HTTP and TLS share the one listening port: each connection's first
// byte decides (0x16 starts a TLS handshake; HTTP starts with a method
// letter), so the read-only sensor routes stay exactly as they were for
// existing clients. Settings, pairing and device routes are refused over
// plain HTTP (403 tls_required) before any body or header value is used.
//
// This is a small, purpose-built HTTP/1.1 subset over plain POSIX sockets
// (no external HTTP library -- this repo's own follow-up spec: "외부
// 라이브러리를 새로 설치하지 않는다") -- same "minimal internal parser
// instead of a real library" choice linux/matter_bridge/ingest/include/ingest/minicoap.hpp
// already made for CoAP, for the same reason (stay buildable/testable with
// nothing beyond a C++17 compiler + CMake). Every response is exactly one
// per connection (`Connection: close`, no keep-alive/pipelining) except
// GET /api/v1/events, which streams for the connection's whole lifetime.
//
// Power Base enrollment (ServerConfig::enrollment_requested/enrollment) adds
// three more TLS-only, bearer-authenticated routes under
// /api/v1/power-base-enrollments; without it they answer 404
// enrollment_disabled and nothing else changes. See enrollment.hpp.
//
// Threading model: one acceptor thread (Start()) that poll()s the listening
// socket on a short bounded timeout (mirrors
// linux/matter_bridge/ingest/src/server.cpp's own SO_RCVTIMEO-poll pattern)
// so it notices Stop() promptly; each accepted connection is handled on its
// own worker thread, tracked in `workers_` so Stop() can join every one of
// them within a bounded time (see http_server.cpp's own top comment for the
// exact bound) -- never a detached, unjoinable thread. A worker thread only
// ever calls ingest::Registry::view()/wait_for_change() (see
// registry.hpp's own top comment: safe from any thread other than the CoAP
// receive thread), and only ever serializes/sends AFTER a snapshot's mutex
// has already been released -- this API never holds Registry::mutex_ while
// doing socket I/O or JSON generation.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ingest/registry.hpp"
#include "pogohome_api/device_auth.hpp"
#include "pogohome_api/enrollment.hpp"
#include "pogohome_api/settings_store.hpp"
#include "pogohome_api/tls_identity.hpp"

namespace pogohome_api {

class Stream;
struct Request;

struct ServerConfig {
    // A bare numeric literal (IPv4 or IPv6) to bind to -- see
    // interface_address.hpp's own ResolveInterfaceAddress(), which is what a
    // caller uses to turn an interface NAME into this. Never "0.0.0.0"/"::"
    // -- see this file's own top comment and docs/pogohome-api-v1.md's own
    // bind policy section.
    std::string bind_address;
    int address_family = 0; // AF_INET or AF_INET6, matching bind_address
    uint16_t port = 0;

    // Bounded resource limits -- see docs/pogohome-api-v1.md's own
    // concurrency section. max_connections bounds this server's total
    // worker-thread count at any moment (health/snapshot connections are
    // normally short-lived, but a burst -- or a client that connects and
    // stalls -- must still never grow this unboundedly). max_sse_clients is
    // a SEPARATE, smaller sub-limit specifically on how many of those may be
    // long-lived GET /api/v1/events streams at once.
    size_t max_connections = 32;
    size_t max_sse_clients = 8;

    // GET /api/v1/events tuning -- see docs/pogohome-api-v1.md's own SSE
    // section for the production defaults (15s heartbeat). Overridable here
    // ONLY so this library's own host tests don't have to spend 15
    // real-time seconds proving the heartbeat fires at all -- production
    // code (linux/matter_bridge/app/chip/main.cpp) never overrides these.
    std::chrono::milliseconds sse_heartbeat_interval = std::chrono::seconds(15);
    std::chrono::milliseconds sse_poll_interval = std::chrono::milliseconds(1000);

    // Served as GET /api/v1/health's `hub_id` (see hub_identity.hpp); empty
    // omits the field.
    std::string hub_id;

    // Home settings sharing (docs/pogohome-api-v1.md "Home settings").
    // Enabled when all three are set. `settings_requested` with any of them
    // missing means sharing was wanted but could not start (a damaged TLS
    // identity, say): the routes answer 503 and health/snapshot report
    // `settings_state: "unavailable"`, while the sensor routes work as
    // always. Neither set: the API is exactly the read-only one it was.
    bool settings_requested = false;
    std::shared_ptr<SettingsStore> settings;
    std::shared_ptr<DeviceAuth> device_auth;
    std::shared_ptr<TlsContext> tls;
    // Largest accepted request body (PUT /api/v1/settings, pairing).
    size_t max_request_body_bytes = 256 * 1024;
    // The wall clock, overridable so tests can expire pairing codes.
    std::function<int64_t()> now_unix_ms;

    // Power Base enrollment (docs/pogohome-api-v1.md "Power Base
    // enrollment"). Off unless requested; it needs settings sharing (TLS and
    // paired devices) as well. Requested but `enrollment` or settings sharing
    // missing: the routes answer 503 enrollment_unavailable.
    bool enrollment_requested = false;
    std::shared_ptr<EnrollmentManager> enrollment;
    // Largest accepted POST /api/v1/power-base-enrollments body.
    size_t max_enrollment_body_bytes = 256;
};

class Server {
public:
    Server(ingest::Registry &registry, ServerConfig config);
    ~Server();

    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    // Binds the listening socket and starts the acceptor thread. Returns
    // false immediately (after logging a clear reason to stderr) on ANY
    // startup failure -- socket()/bind()/listen() failing -- same
    // "refuse to start, never silently degrade" policy as
    // ingest::run_server() (see ingest/include/ingest/server.hpp). Calling
    // Start() more than once, or after Stop(), is a programming error (not
    // defended against -- construct a new Server instead).
    bool Start();

    // Signals every thread this Server owns to stop, joins all of them
    // within a bounded total time, and closes the listening socket. Safe to
    // call even if Start() was never called or already failed. Idempotent:
    // a second call is a harmless no-op.
    void Stop();

    // The actual bound port -- equal to config.port unless it was 0 (bind
    // an ephemeral port), which this test-only path supports so host tests
    // never need to hardcode a port number that might collide with another
    // test running concurrently.
    uint16_t bound_port() const { return bound_port_; }

    // True from a successful Start() until Stop() -- or until the acceptor
    // thread gives up on the listening socket (a poll() error), after which
    // no new request can be answered. What the Bonjour advertiser checks
    // before (and while) announcing this server (see mdns_advertiser.hpp).
    bool accepting() const { return accepting_.load(std::memory_order_relaxed); }

private:
    struct Worker {
        std::thread thread;
        // Set to false by the worker itself right before it returns --
        // Stop() (and the acceptor's own periodic reap pass) only ever
        // joins a thread whose handler has already finished running, never
        // blocks indefinitely inside a lock waiting for one that hasn't.
        std::shared_ptr<std::atomic<bool>> running;
    };

    void AcceptLoop();
    void ReapFinishedWorkersLocked();
    void HandleConnection(int client_fd, std::shared_ptr<std::atomic<bool>> running);
    void HandleSse(Stream &stream);
    // TLS-only routes; `request` has its body read already.
    void HandlePairingClaim(Stream &stream, const Request &request);
    void HandleSettings(Stream &stream, const Request &request, const DeviceRecord &device);
    void HandleRevokeSelf(Stream &stream, const DeviceRecord &device);
    // Every /api/v1/power-base-enrollments route, from the TLS check on.
    // `buf` is the raw request read so far (it may hold the start of a body).
    void HandleEnrollmentRoute(Stream &stream, Request &request, std::string &buf);
    void HandleEnrollmentStart(Stream &stream, Request &request, const DeviceRecord &device);

    // Shared by the settings and enrollment routes: the bearer token check
    // (answers 401/503 itself and returns false), and the body read (answers
    // 411/413/400 itself, or just closes, and returns false).
    bool AuthenticateDevice(Stream &stream, const Request &request, DeviceRecord &device);
    bool ReadRequestBody(Stream &stream, Request &request, const std::string &buf, size_t max_bytes);

    bool SettingsEnabled() const { return config_.settings && config_.device_auth && config_.tls; }
    // Appends settings_api/settings_state/settings_revision to a health or
    // snapshot JSON object (nothing unless settings sharing was requested).
    void AppendSettingsFields(std::string &json) const;
    // Appends power_base_enrollment to the health JSON (only when requested).
    void AppendEnrollmentField(std::string &json) const;
    int64_t NowUnixMs() const;

    ingest::Registry &registry_;
    ServerConfig config_;
    int listen_fd_ = -1;
    uint16_t bound_port_ = 0;
    std::atomic<bool> should_stop_{false};
    std::atomic<bool> accepting_{false};
    std::atomic<size_t> sse_clients_{0};
    std::thread acceptor_thread_;
    bool started_ = false;

    std::mutex workers_mutex_;
    std::vector<Worker> workers_;
};

} // namespace pogohome_api
