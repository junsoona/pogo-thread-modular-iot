#include "pogohome_api/http_server.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "pogohome_api/json_snapshot.hpp"

namespace pogohome_api {

namespace {

constexpr size_t kMaxRequestHeaderBytes = 8192;
constexpr int kClientRecvTimeoutSec = 3;
constexpr int kClientSendTimeoutSec = 3;
constexpr int kAcceptPollTimeoutMs = 200;
constexpr int kListenBacklog = 16;
constexpr auto kLogRateLimit = std::chrono::milliseconds(5000);

// Same technique as linux/matter_bridge/ingest/src/server.cpp's own
// rate_limit_ok() (duplicated rather than shared -- it's four lines and
// pulling in a cross-directory dependency for it isn't worth it): at most
// once per `interval` per call site.
bool RateLimitOk(std::chrono::steady_clock::time_point &last, std::chrono::milliseconds interval)
{
    auto now = std::chrono::steady_clock::now();
    if (last.time_since_epoch().count() == 0 || (now - last) >= interval) {
        last = now;
        return true;
    }
    return false;
}

int64_t CurrentUnixMillis()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool SendAll(int fd, const char *data, size_t len)
{
    size_t sent_total = 0;
    while (sent_total < len) {
        ssize_t n = send(fd, data + sent_total, len - sent_total, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        sent_total += static_cast<size_t>(n);
    }
    return true;
}

bool SendAll(int fd, const std::string &s)
{
    return SendAll(fd, s.data(), s.size());
}

void SendJsonResponse(int fd, int status_code, const char *status_text, const std::string &body)
{
    std::string headers;
    headers += "HTTP/1.1 ";
    headers += std::to_string(status_code);
    headers += ' ';
    headers += status_text;
    headers += "\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: ";
    headers += std::to_string(body.size());
    headers += "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    if (SendAll(fd, headers)) {
        SendAll(fd, body);
    }
}

bool SendSseEvent(int fd, const char *event_name, const std::string &json_data)
{
    std::string frame = "event: ";
    frame += event_name;
    frame += "\ndata: ";
    frame += json_data;
    frame += "\n\n";
    return SendAll(fd, frame);
}

bool SendSseComment(int fd, const char *text)
{
    std::string frame = ": ";
    frame += text;
    frame += "\n\n";
    return SendAll(fd, frame);
}

enum class ReadResult { Ok, Incomplete, Oversized };

// Reads from `fd` into `buf` until "\r\n\r\n" (end of headers) is seen, the
// kMaxRequestHeaderBytes cap is hit, or recv() returns <= 0 (peer closed,
// error, or this connection's SO_RCVTIMEO elapsed). Never reads a request
// body -- this API never needs one (see http_server.hpp's own top comment).
ReadResult ReadRequestHeaders(int fd, std::string &buf)
{
    char chunk[1024];
    while (buf.size() < kMaxRequestHeaderBytes) {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            return ReadResult::Incomplete;
        }
        buf.append(chunk, static_cast<size_t>(n));
        if (buf.find("\r\n\r\n") != std::string::npos) {
            return ReadResult::Ok;
        }
    }
    return ReadResult::Oversized;
}

// Parses only the request line ("METHOD SP PATH SP HTTP-VERSION"); every
// other header is ignored (this API never needs Host/Accept/etc -- see
// http_server.hpp's own top comment). `path` has any '?query' stripped
// (this API defines no query parameters).
bool ParseRequestLine(const std::string &buf, std::string &method, std::string &path)
{
    size_t line_end = buf.find("\r\n");
    if (line_end == std::string::npos) {
        return false;
    }
    std::string line = buf.substr(0, line_end);
    size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) {
        return false;
    }
    size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) {
        return false;
    }
    method = line.substr(0, sp1);
    path = line.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t q = path.find('?');
    if (q != std::string::npos) {
        path = path.substr(0, q);
    }
    return !method.empty() && !path.empty();
}

// Closes `fd` and marks the owning Worker slot reapable on destruction --
// every HandleConnection() exit path (early return included) goes through
// this, so the socket is never leaked and the worker is never left
// unreapable regardless of which branch returns.
struct ConnectionGuard {
    int fd;
    std::shared_ptr<std::atomic<bool>> running;
    ~ConnectionGuard()
    {
        close(fd);
        running->store(false, std::memory_order_relaxed);
    }
};

} // namespace

Server::Server(ingest::Registry &registry, ServerConfig config) : registry_(registry), config_(std::move(config)) {}

Server::~Server()
{
    Stop();
}

bool Server::Start()
{
    listen_fd_ = socket(config_.address_family, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::fprintf(stderr, "pogohome_api: socket() failed: %s\n", std::strerror(errno));
        return false;
    }

    int on = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    if (config_.address_family == AF_INET) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config_.port);
        if (inet_pton(AF_INET, config_.bind_address.c_str(), &addr.sin_addr) != 1) {
            std::fprintf(stderr, "pogohome_api: invalid IPv4 bind address '%s'\n", config_.bind_address.c_str());
            close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
            std::fprintf(stderr, "pogohome_api: bind(%s:%u) failed: %s\n", config_.bind_address.c_str(),
                         (unsigned)config_.port, std::strerror(errno));
            close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        socklen_t len = sizeof(addr);
        if (getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&addr), &len) == 0) {
            bound_port_ = ntohs(addr.sin_port);
        }
    } else if (config_.address_family == AF_INET6) {
        int v6only = 1;
        setsockopt(listen_fd_, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(config_.port);
        if (inet_pton(AF_INET6, config_.bind_address.c_str(), &addr.sin6_addr) != 1) {
            std::fprintf(stderr, "pogohome_api: invalid IPv6 bind address '%s'\n", config_.bind_address.c_str());
            close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
            std::fprintf(stderr, "pogohome_api: bind([%s]:%u) failed: %s\n", config_.bind_address.c_str(),
                         (unsigned)config_.port, std::strerror(errno));
            close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        socklen_t len = sizeof(addr);
        if (getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&addr), &len) == 0) {
            bound_port_ = ntohs(addr.sin6_port);
        }
    } else {
        std::fprintf(stderr, "pogohome_api: unsupported address family %d\n", config_.address_family);
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (listen(listen_fd_, kListenBacklog) < 0) {
        std::fprintf(stderr, "pogohome_api: listen() failed: %s\n", std::strerror(errno));
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    std::string display_host =
        config_.address_family == AF_INET6 ? ("[" + config_.bind_address + "]") : config_.bind_address;
    std::fprintf(stdout, "pogohome_api: listening on http://%s:%u\n", display_host.c_str(), (unsigned)bound_port_);

    started_ = true;
    accepting_.store(true, std::memory_order_relaxed);
    acceptor_thread_ = std::thread([this] { AcceptLoop(); });
    return true;
}

void Server::Stop()
{
    accepting_.store(false, std::memory_order_relaxed);
    if (should_stop_.exchange(true)) {
        return; // already stopped/stopping
    }
    // Join the acceptor BEFORE touching listen_fd_ -- AcceptLoop() is the
    // only thread that ever reads/accepts on it, so closing it only after
    // that thread has fully exited avoids a concurrent close()/poll() race
    // on the same fd number (see this file's own AcceptLoop() comment).
    if (acceptor_thread_.joinable()) {
        acceptor_thread_.join();
    }
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }

    std::lock_guard<std::mutex> lock(workers_mutex_);
    for (auto &w : workers_) {
        if (w.thread.joinable()) {
            w.thread.join();
        }
    }
    workers_.clear();
}

void Server::ReapFinishedWorkersLocked()
{
    for (auto it = workers_.begin(); it != workers_.end();) {
        if (!it->running->load(std::memory_order_relaxed) && it->thread.joinable()) {
            it->thread.join();
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }
}

void Server::AcceptLoop()
{
    std::chrono::steady_clock::time_point last_cap_log{};
    std::chrono::steady_clock::time_point last_accept_err_log{};

    while (!should_stop_.load(std::memory_order_relaxed)) {
        pollfd pfd{};
        pfd.fd = listen_fd_;
        pfd.events = POLLIN;
        int rc = poll(&pfd, 1, kAcceptPollTimeoutMs);

        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            ReapFinishedWorkersLocked();
        }

        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::fprintf(stderr, "pogohome_api: poll() on the listening socket failed: %s -- no longer accepting\n",
                         std::strerror(errno));
            accepting_.store(false, std::memory_order_relaxed);
            break;
        }
        if (rc == 0 || !(pfd.revents & POLLIN)) {
            continue; // poll timeout -- loop back to re-check should_stop_
        }

        int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (RateLimitOk(last_accept_err_log, kLogRateLimit)) {
                std::fprintf(stderr, "pogohome_api: accept() failed: %s\n", std::strerror(errno));
            }
            continue;
        }

        std::lock_guard<std::mutex> lock(workers_mutex_);
        if (workers_.size() >= config_.max_connections) {
            if (RateLimitOk(last_cap_log, kLogRateLimit)) {
                std::fprintf(stderr, "pogohome_api: connection cap (%zu) reached -- rejecting a new connection\n",
                             config_.max_connections);
            }
            close(client_fd);
            continue;
        }

        Worker w;
        w.running = std::make_shared<std::atomic<bool>>(true);
        auto running = w.running;
        w.thread = std::thread([this, client_fd, running] { HandleConnection(client_fd, running); });
        workers_.push_back(std::move(w));
    }
}

void Server::HandleConnection(int client_fd, std::shared_ptr<std::atomic<bool>> running)
{
    ConnectionGuard guard{client_fd, running};

    timeval recv_tv{};
    recv_tv.tv_sec = kClientRecvTimeoutSec;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &recv_tv, sizeof(recv_tv));
    timeval send_tv{};
    send_tv.tv_sec = kClientSendTimeoutSec;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &send_tv, sizeof(send_tv));

    std::string buf;
    ReadResult rr = ReadRequestHeaders(client_fd, buf);
    if (rr == ReadResult::Oversized) {
        SendJsonResponse(client_fd, 400, "Bad Request", "{\"error\":\"request_header_too_large\"}");
        return;
    }
    if (rr == ReadResult::Incomplete) {
        // Peer closed, a transport error, or this connection's own
        // SO_RCVTIMEO elapsed before a full header block ever arrived --
        // nothing coherent to answer; close without attempting a response
        // (see docs/pogohome-api-v1.md's own HTTP behavior section: "400
        // 또는 연결 종료" -- either is acceptable here).
        return;
    }

    std::string method, path;
    if (!ParseRequestLine(buf, method, path)) {
        SendJsonResponse(client_fd, 400, "Bad Request", "{\"error\":\"malformed_request_line\"}");
        return;
    }

    if (path == "/api/v1/health") {
        if (method != "GET") {
            SendJsonResponse(client_fd, 405, "Method Not Allowed", "{\"error\":\"method_not_allowed\"}");
            return;
        }
        ingest::RegistryView view = registry_.view();
        uint64_t generation = registry_.generation();
        // Distinct modules, the same set /api/v1/snapshot lists (a module
        // briefly present under two Power Bases counts once).
        size_t module_count = ResolveModuleOwners(view.nodes).size();
        SendJsonResponse(client_fd, 200, "OK",
                         BuildHealthJson(generation, module_count, view.power_bases.size(), config_.hub_id));
        return;
    }

    if (path == "/api/v1/snapshot") {
        if (method != "GET") {
            SendJsonResponse(client_fd, 405, "Method Not Allowed", "{\"error\":\"method_not_allowed\"}");
            return;
        }
        ingest::RegistryView view = registry_.view();
        uint64_t generation = registry_.generation();
        SendJsonResponse(client_fd, 200, "OK", BuildSnapshotJson(view, generation, CurrentUnixMillis()));
        return;
    }

    if (path == "/api/v1/events") {
        if (method != "GET") {
            SendJsonResponse(client_fd, 405, "Method Not Allowed", "{\"error\":\"method_not_allowed\"}");
            return;
        }
        if (sse_clients_.load(std::memory_order_relaxed) >= config_.max_sse_clients) {
            SendJsonResponse(client_fd, 503, "Service Unavailable", "{\"error\":\"too_many_sse_clients\"}");
            return;
        }
        sse_clients_.fetch_add(1, std::memory_order_relaxed);
        HandleSse(client_fd);
        sse_clients_.fetch_sub(1, std::memory_order_relaxed);
        return;
    }

    SendJsonResponse(client_fd, 404, "Not Found", "{\"error\":\"not_found\"}");
}

void Server::HandleSse(int client_fd)
{
    static const char kSseHeaders[] = "HTTP/1.1 200 OK\r\n"
                                       "Content-Type: text/event-stream\r\n"
                                       "Cache-Control: no-store, no-cache\r\n"
                                       "Connection: keep-alive\r\n"
                                       "\r\n";
    if (!SendAll(client_fd, kSseHeaders, sizeof(kSseHeaders) - 1)) {
        return;
    }

    // Ordering note: generation() is read BEFORE snapshot() so the worst
    // case if a change lands between the two calls is one harmless extra
    // "nothing really new" resend on the very next wait_for_change() wakeup
    // (last_seen ends up one generation older than what the snapshot
    // already reflects) -- never a MISSED update, which is what the
    // opposite order could cause (a change landing between snapshot() and
    // generation() would make last_seen look newer than what was actually
    // captured, and that change would never trigger a wakeup on its own).
    uint64_t last_seen = registry_.generation();
    ingest::RegistryView view = registry_.view();
    if (!SendSseEvent(client_fd, "snapshot", BuildSnapshotJson(view, last_seen, CurrentUnixMillis()))) {
        return;
    }
    auto last_activity = std::chrono::steady_clock::now();

    while (!should_stop_.load(std::memory_order_relaxed)) {
        bool changed = registry_.wait_for_change(&last_seen, config_.sse_poll_interval);
        if (should_stop_.load(std::memory_order_relaxed)) {
            return;
        }
        if (changed) {
            ingest::RegistryView latest = registry_.view();
            if (!SendSseEvent(client_fd, "snapshot", BuildSnapshotJson(latest, last_seen, CurrentUnixMillis()))) {
                return;
            }
            last_activity = std::chrono::steady_clock::now();
        } else {
            auto now = std::chrono::steady_clock::now();
            if (now - last_activity >= config_.sse_heartbeat_interval) {
                if (!SendSseComment(client_fd, "heartbeat")) {
                    return;
                }
                last_activity = now;
            }
        }
    }
}

} // namespace pogohome_api
