#include "pogohome_api/http_server.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <map>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "pogohome_api/json_snapshot.hpp"
#include "pogohome_api/json_value.hpp"

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

} // namespace

// One client connection: the raw socket, or a TLS session over it. Every
// route reads and writes through this, so plain and TLS requests share one
// code path.
class Stream {
public:
    explicit Stream(int fd) : fd_(fd) {}
    ~Stream()
    {
        if (ssl_ != nullptr) {
            if (handshake_done_) {
                SSL_shutdown(ssl_);
            }
            SSL_free(ssl_);
        }
    }
    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;

    bool StartTls(SSL_CTX *ctx)
    {
        ssl_ = SSL_new(ctx);
        if (ssl_ == nullptr || SSL_set_fd(ssl_, fd_) != 1) {
            ERR_clear_error();
            return false;
        }
        if (SSL_accept(ssl_) != 1) {
            ERR_clear_error();
            return false;
        }
        handshake_done_ = true;
        return true;
    }

    bool is_tls() const { return ssl_ != nullptr; }

    ssize_t Recv(char *buf, size_t len)
    {
        if (ssl_ != nullptr) {
            int n = SSL_read(ssl_, buf, static_cast<int>(len));
            if (n <= 0) {
                ERR_clear_error();
                return n < 0 ? -1 : 0;
            }
            return n;
        }
        return recv(fd_, buf, len, 0);
    }

    bool SendAll(const char *data, size_t len)
    {
        size_t sent_total = 0;
        while (sent_total < len) {
            ssize_t n;
            if (ssl_ != nullptr) {
                n = SSL_write(ssl_, data + sent_total, static_cast<int>(len - sent_total));
                if (n <= 0) {
                    ERR_clear_error();
                    return false;
                }
            } else {
                n = send(fd_, data + sent_total, len - sent_total, MSG_NOSIGNAL);
                if (n <= 0) {
                    return false;
                }
            }
            sent_total += static_cast<size_t>(n);
        }
        return true;
    }

    bool SendAll(const std::string &s) { return SendAll(s.data(), s.size()); }

private:
    int fd_;
    SSL *ssl_ = nullptr;
    bool handshake_done_ = false;
};

struct Request {
    std::string method;
    std::string path;
    // Header names lower-cased; values trimmed.
    std::map<std::string, std::string> headers;
    std::string body;

    const std::string *Header(const char *name) const
    {
        auto it = headers.find(name);
        return it == headers.end() ? nullptr : &it->second;
    }
};

namespace {

void SendResponse(Stream &stream, int status_code, const char *status_text, const std::string &body,
                  const char *extra_headers = "")
{
    std::string headers;
    headers += "HTTP/1.1 ";
    headers += std::to_string(status_code);
    headers += ' ';
    headers += status_text;
    headers += "\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: ";
    headers += std::to_string(body.size());
    headers += "\r\nCache-Control: no-store\r\nConnection: close\r\n";
    headers += extra_headers;
    headers += "\r\n";
    if (stream.SendAll(headers)) {
        stream.SendAll(body);
    }
}

void SendJsonResponse(Stream &stream, int status_code, const char *status_text, const std::string &body)
{
    SendResponse(stream, status_code, status_text, body);
}

bool SendSseEvent(Stream &stream, const char *event_name, const std::string &json_data)
{
    std::string frame = "event: ";
    frame += event_name;
    frame += "\ndata: ";
    frame += json_data;
    frame += "\n\n";
    return stream.SendAll(frame);
}

bool SendSseComment(Stream &stream, const char *text)
{
    std::string frame = ": ";
    frame += text;
    frame += "\n\n";
    return stream.SendAll(frame);
}

enum class ReadResult { Ok, Incomplete, Oversized };

// Reads from `stream` into `buf` until "\r\n\r\n" (end of headers) is seen,
// the kMaxRequestHeaderBytes cap is hit, or a read returns <= 0 (peer
// closed, error, or this connection's SO_RCVTIMEO elapsed). Bytes after the
// header block (the start of a body) stay in `buf`.
ReadResult ReadRequestHeaders(Stream &stream, std::string &buf)
{
    char chunk[1024];
    while (buf.size() < kMaxRequestHeaderBytes) {
        ssize_t n = stream.Recv(chunk, sizeof(chunk));
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

// Parses the request line ("METHOD SP PATH SP HTTP-VERSION"); `path` has any
// '?query' stripped (this API defines no query parameters).
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

std::string Trim(const std::string &s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) {
        b++;
    }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) {
        e--;
    }
    return s.substr(b, e - b);
}

// Header fields after the request line. A repeated Content-Length or
// Authorization is rejected (false): two readers could disagree on which
// counts.
bool ParseHeaders(const std::string &buf, std::map<std::string, std::string> &headers)
{
    size_t pos = buf.find("\r\n");
    const size_t end = buf.find("\r\n\r\n");
    if (pos == std::string::npos || end == std::string::npos) {
        return false;
    }
    pos += 2;
    while (pos < end + 2) {
        size_t line_end = buf.find("\r\n", pos);
        if (line_end == std::string::npos || line_end > end) {
            break;
        }
        std::string line = buf.substr(pos, line_end - pos);
        pos = line_end + 2;
        if (line.empty()) {
            break;
        }
        size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) {
            return false;
        }
        std::string name = line.substr(0, colon);
        for (char &c : name) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        std::string value = Trim(line.substr(colon + 1));
        auto it = headers.find(name);
        if (it != headers.end()) {
            if (name == "content-length" || name == "authorization") {
                return false;
            }
            continue;
        }
        headers.emplace(std::move(name), std::move(value));
    }
    return true;
}

enum class BodyResult { Ok, BadRequest, TooLarge, LengthRequired, Incomplete };

// Reads exactly Content-Length bytes of body (some may already be in
// `leftover`). Chunked bodies are not supported (411).
BodyResult ReadBody(Stream &stream, const std::map<std::string, std::string> &headers, std::string leftover,
                    size_t max_bytes, std::string &body)
{
    if (headers.count("transfer-encoding") != 0) {
        return BodyResult::LengthRequired;
    }
    auto it = headers.find("content-length");
    if (it == headers.end()) {
        return BodyResult::LengthRequired;
    }
    const std::string &value = it->second;
    if (value.empty() || value.size() > 12) {
        return BodyResult::BadRequest;
    }
    size_t length = 0;
    for (char c : value) {
        if (c < '0' || c > '9') {
            return BodyResult::BadRequest;
        }
        length = length * 10 + static_cast<size_t>(c - '0');
    }
    if (length > max_bytes) {
        return BodyResult::TooLarge;
    }
    body = std::move(leftover);
    if (body.size() > length) {
        body.resize(length); // no pipelining: every response closes the connection
    }
    char chunk[4096];
    while (body.size() < length) {
        size_t want = length - body.size();
        ssize_t n = stream.Recv(chunk, want < sizeof(chunk) ? want : sizeof(chunk));
        if (n <= 0) {
            return BodyResult::Incomplete;
        }
        body.append(chunk, static_cast<size_t>(n));
    }
    return BodyResult::Ok;
}

std::string ErrorJson(const char *error)
{
    std::string out = "{\"error\":";
    AppendJsonStringLiteral(out, error);
    out += '}';
    return out;
}

std::string ErrorJson(const char *error, const char *key, const std::string &value)
{
    std::string out = "{\"error\":";
    AppendJsonStringLiteral(out, error);
    out += ",\"";
    out += key;
    out += "\":";
    AppendJsonStringLiteral(out, value);
    out += '}';
    return out;
}

// The category of an Unavailable store ("corrupt"/"io"), never its file
// path or detail, which only the hub's log gets.
std::string UnavailableCategory(const std::string &reason)
{
    size_t colon = reason.find(':');
    return colon == std::string::npos ? std::string("unavailable") : reason.substr(0, colon);
}

bool IsTlsOnlyPath(const std::string &path)
{
    return path == "/api/v1/settings" || path == "/api/v1/pairing/claim" || path == "/api/v1/devices/self";
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

int64_t Server::NowUnixMs() const
{
    return config_.now_unix_ms ? config_.now_unix_ms() : CurrentUnixMillis();
}

void Server::AppendSettingsFields(std::string &json) const
{
    if (!config_.settings_requested || json.empty() || json.back() != '}') {
        return;
    }
    const char *state = "unavailable";
    std::string revision;
    if (SettingsEnabled()) {
        SettingsSnapshot snapshot = config_.settings->Get();
        if (snapshot.state == SettingsState::Ready) {
            state = "ready";
            revision = snapshot.revision;
        } else if (snapshot.state == SettingsState::Uninitialized) {
            state = "uninitialized";
        }
    }
    json.pop_back();
    json += ",\"settings_api\":1,\"settings_state\":\"";
    json += state;
    json += "\",\"settings_revision\":";
    if (revision.empty()) {
        json += "null";
    } else {
        AppendJsonStringLiteral(json, revision);
    }
    json += '}';
}

void Server::HandleConnection(int client_fd, std::shared_ptr<std::atomic<bool>> running)
{
    ConnectionGuard guard{client_fd, running};

    // OpenSSL writes with plain send(): a peer that vanished must fail the
    // write with EPIPE, never deliver SIGPIPE to the process. Blocking it in
    // this worker thread only affects this thread.
    sigset_t sigpipe;
    sigemptyset(&sigpipe);
    sigaddset(&sigpipe, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &sigpipe, nullptr);
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif

    timeval recv_tv{};
    recv_tv.tv_sec = kClientRecvTimeoutSec;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &recv_tv, sizeof(recv_tv));
    timeval send_tv{};
    send_tv.tv_sec = kClientSendTimeoutSec;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &send_tv, sizeof(send_tv));

    Stream stream(client_fd);

    // 0x16 is a TLS handshake record; an HTTP request starts with a method
    // name. Without a TLS context such a connection is simply closed.
    unsigned char first = 0;
    ssize_t peeked = recv(client_fd, &first, 1, MSG_PEEK);
    if (peeked <= 0) {
        return;
    }
    if (first == 0x16) {
        if (!config_.tls || !stream.StartTls(config_.tls->get())) {
            return;
        }
    }

    std::string buf;
    ReadResult rr = ReadRequestHeaders(stream, buf);
    if (rr == ReadResult::Oversized) {
        SendJsonResponse(stream, 400, "Bad Request", "{\"error\":\"request_header_too_large\"}");
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

    Request request;
    if (!ParseRequestLine(buf, request.method, request.path)) {
        SendJsonResponse(stream, 400, "Bad Request", "{\"error\":\"malformed_request_line\"}");
        return;
    }
    const std::string &method = request.method;
    const std::string &path = request.path;

    if (path == "/api/v1/health") {
        if (method != "GET") {
            SendJsonResponse(stream, 405, "Method Not Allowed", "{\"error\":\"method_not_allowed\"}");
            return;
        }
        ingest::RegistryView view = registry_.view();
        uint64_t generation = registry_.generation();
        // Distinct modules, the same set /api/v1/snapshot lists (a module
        // briefly present under two Power Bases counts once).
        size_t module_count = ResolveModuleOwners(view.nodes).size();
        std::string body = BuildHealthJson(generation, module_count, view.power_bases.size(), config_.hub_id);
        AppendSettingsFields(body);
        SendJsonResponse(stream, 200, "OK", body);
        return;
    }

    if (path == "/api/v1/snapshot") {
        if (method != "GET") {
            SendJsonResponse(stream, 405, "Method Not Allowed", "{\"error\":\"method_not_allowed\"}");
            return;
        }
        ingest::RegistryView view = registry_.view();
        uint64_t generation = registry_.generation();
        std::string body = BuildSnapshotJson(view, generation, CurrentUnixMillis());
        AppendSettingsFields(body);
        SendJsonResponse(stream, 200, "OK", body);
        return;
    }

    if (path == "/api/v1/events") {
        if (method != "GET") {
            SendJsonResponse(stream, 405, "Method Not Allowed", "{\"error\":\"method_not_allowed\"}");
            return;
        }
        if (sse_clients_.load(std::memory_order_relaxed) >= config_.max_sse_clients) {
            SendJsonResponse(stream, 503, "Service Unavailable", "{\"error\":\"too_many_sse_clients\"}");
            return;
        }
        sse_clients_.fetch_add(1, std::memory_order_relaxed);
        HandleSse(stream);
        sse_clients_.fetch_sub(1, std::memory_order_relaxed);
        return;
    }

    if (config_.settings_requested && IsTlsOnlyPath(path)) {
        // Nothing from a plain-HTTP request to these routes is ever used.
        if (!stream.is_tls()) {
            SendJsonResponse(stream, 403, "Forbidden", ErrorJson("tls_required"));
            return;
        }
        if (!SettingsEnabled()) {
            SendJsonResponse(stream, 503, "Service Unavailable", ErrorJson("settings_unavailable", "reason", "startup"));
            return;
        }
        const bool is_claim = path == "/api/v1/pairing/claim";
        const bool is_settings = path == "/api/v1/settings";
        const bool method_ok = is_claim ? method == "POST"
                                        : (is_settings ? (method == "GET" || method == "PUT") : method == "DELETE");
        if (!method_ok) {
            SendJsonResponse(stream, 405, "Method Not Allowed", ErrorJson("method_not_allowed"));
            return;
        }
        if (!ParseHeaders(buf, request.headers)) {
            SendJsonResponse(stream, 400, "Bad Request", ErrorJson("malformed_headers"));
            return;
        }

        DeviceRecord device;
        if (!is_claim) {
            const std::string *authorization = request.Header("authorization");
            std::string auth_error;
            AuthOutcome auth = authorization == nullptr
                                   ? AuthOutcome::Invalid
                                   : config_.device_auth->Authenticate(*authorization, NowUnixMs(), device, auth_error);
            if (auth == AuthOutcome::StorageError) {
                std::fprintf(stderr, "pogohome_api: device credentials unavailable: %s\n", auth_error.c_str());
                SendJsonResponse(stream, 503, "Service Unavailable", ErrorJson("auth_unavailable"));
                return;
            }
            if (auth != AuthOutcome::Ok) {
                SendResponse(stream, 401, "Unauthorized", ErrorJson("unauthorized"),
                             "WWW-Authenticate: Bearer realm=\"pogohome\"\r\n");
                return;
            }
        }

        if (method == "PUT" || method == "POST") {
            size_t header_end = buf.find("\r\n\r\n") + 4;
            BodyResult body_result = ReadBody(stream, request.headers, buf.substr(header_end),
                                              config_.max_request_body_bytes, request.body);
            switch (body_result) {
            case BodyResult::Ok:
                break;
            case BodyResult::TooLarge:
                SendJsonResponse(stream, 413, "Payload Too Large", ErrorJson("request_too_large"));
                return;
            case BodyResult::LengthRequired:
                SendJsonResponse(stream, 411, "Length Required", ErrorJson("length_required"));
                return;
            case BodyResult::BadRequest:
                SendJsonResponse(stream, 400, "Bad Request", ErrorJson("invalid_content_length"));
                return;
            case BodyResult::Incomplete:
                return;
            }
        }

        if (is_claim) {
            HandlePairingClaim(stream, request);
        } else if (is_settings) {
            HandleSettings(stream, request, device);
        } else {
            HandleRevokeSelf(stream, device);
        }
        return;
    }

    SendJsonResponse(stream, 404, "Not Found", "{\"error\":\"not_found\"}");
}

void Server::HandlePairingClaim(Stream &stream, const Request &request)
{
    static std::mutex log_mutex;
    static std::chrono::steady_clock::time_point last_reject_log{};

    JsonValue root;
    std::string parse_error;
    ClaimRequest claim;
    const JsonValue *name = nullptr;
    const JsonValue *nonce = nullptr;
    const JsonValue *proof = nullptr;
    if (ParseJson(request.body, root, parse_error) && root.IsObject()) {
        name = root.Find("device_name");
        nonce = root.Find("client_nonce");
        proof = root.Find("proof");
    }
    if (name == nullptr || !name->IsString() || nonce == nullptr || !nonce->IsString() || proof == nullptr ||
        !proof->IsString() || root.object_members.size() != 3) {
        SendJsonResponse(stream, 400, "Bad Request", ErrorJson("invalid_request"));
        return;
    }
    claim.device_name = name->string_value;
    claim.client_nonce_hex = nonce->string_value;
    claim.proof_hex = proof->string_value;

    ClaimResponse response;
    std::string error;
    ClaimOutcome outcome = config_.device_auth->Claim(claim, config_.hub_id, config_.tls->certificate_sha256(),
                                                      NowUnixMs(), response, error);
    const char *reason = nullptr;
    switch (outcome) {
    case ClaimOutcome::Paired: {
        std::fprintf(stdout, "pogohome_api: device paired: %s (\"%s\")\n", response.device_id.c_str(),
                     claim.device_name.c_str());
        std::string body = "{\"hub_id\":";
        AppendJsonStringLiteral(body, config_.hub_id);
        body += ",\"device_id\":";
        AppendJsonStringLiteral(body, response.device_id);
        body += ",\"token\":";
        AppendJsonStringLiteral(body, response.token);
        body += ",\"server_proof\":";
        AppendJsonStringLiteral(body, response.server_proof_hex);
        body += '}';
        SendJsonResponse(stream, 200, "OK", body);
        return;
    }
    case ClaimOutcome::InvalidRequest:
        SendJsonResponse(stream, 400, "Bad Request", ErrorJson("invalid_request"));
        return;
    case ClaimOutcome::TooManyDevices:
        SendJsonResponse(stream, 409, "Conflict", ErrorJson("too_many_devices"));
        return;
    case ClaimOutcome::StorageError:
        std::fprintf(stderr, "pogohome_api: pairing failed on the hub: %s\n", error.c_str());
        SendJsonResponse(stream, 503, "Service Unavailable", ErrorJson("auth_unavailable"));
        return;
    case ClaimOutcome::NoActiveCode:
        reason = "no_active_code";
        break;
    case ClaimOutcome::Expired:
        reason = "expired";
        break;
    case ClaimOutcome::BadProof:
        reason = "bad_proof";
        break;
    }
    {
        std::lock_guard<std::mutex> lock(log_mutex);
        if (RateLimitOk(last_reject_log, kLogRateLimit)) {
            std::fprintf(stderr, "pogohome_api: pairing attempt rejected (%s)\n", reason);
        }
    }
    SendJsonResponse(stream, 403, "Forbidden", ErrorJson("pairing_rejected", "reason", reason));
}

void Server::HandleSettings(Stream &stream, const Request &request, const DeviceRecord &device)
{
    SettingsStore &store = *config_.settings;
    if (request.method == "GET") {
        SettingsSnapshot snapshot = store.Get();
        if (snapshot.state == SettingsState::Unavailable) {
            SendJsonResponse(stream, 503, "Service Unavailable",
                             ErrorJson("settings_unavailable", "reason", UnavailableCategory(snapshot.unavailable_reason)));
            return;
        }
        SendJsonResponse(stream, 200, "OK", BuildSettingsResponseJson(snapshot, config_.hub_id));
        return;
    }

    // PUT {"base_revision": "<revision>" | null, "settings": {...}}
    JsonValue root;
    std::string parse_error;
    if (!ParseJson(request.body, root, parse_error) || !root.IsObject()) {
        SendJsonResponse(stream, 400, "Bad Request", ErrorJson("invalid_json"));
        return;
    }
    const JsonValue *base = root.Find("base_revision");
    const JsonValue *settings_value = root.Find("settings");
    if (base == nullptr || !(base->IsNull() || base->IsString()) || settings_value == nullptr ||
        root.object_members.size() != 2) {
        SendJsonResponse(stream, 400, "Bad Request", ErrorJson("invalid_request"));
        return;
    }
    HomeSettings proposed;
    SettingsError settings_error;
    if (!ParseHomeSettings(*settings_value, proposed, settings_error)) {
        SendJsonResponse(stream, 422, "Unprocessable Entity",
                         ErrorJson("invalid_settings", "detail", settings_error.code));
        return;
    }
    const std::string base_revision = base->IsNull() ? std::string() : base->string_value;
    if (base->IsString() && base_revision.empty()) {
        SendJsonResponse(stream, 400, "Bad Request", ErrorJson("invalid_request"));
        return;
    }

    SettingsUpdateResult result = store.Update(base_revision, proposed, NowUnixMs());
    switch (result.outcome) {
    case SettingsUpdateOutcome::Saved:
        std::fprintf(stdout, "pogohome_api: settings %s saved by device %s\n", result.current.revision.c_str(),
                     device.id.c_str());
        SendJsonResponse(stream, 200, "OK", BuildSettingsResponseJson(result.current, config_.hub_id));
        return;
    case SettingsUpdateOutcome::Conflict: {
        std::string body = "{\"error\":\"revision_conflict\",\"current\":";
        body += BuildSettingsResponseJson(result.current, config_.hub_id);
        body += '}';
        SendJsonResponse(stream, 409, "Conflict", body);
        return;
    }
    case SettingsUpdateOutcome::Unavailable:
        SendJsonResponse(stream, 503, "Service Unavailable",
                         ErrorJson("settings_unavailable", "reason", UnavailableCategory(result.error)));
        return;
    case SettingsUpdateOutcome::SaveFailed:
        std::fprintf(stderr, "pogohome_api: settings NOT saved (the previous settings are kept): %s\n",
                     result.error.c_str());
        SendJsonResponse(stream, 500, "Internal Server Error", ErrorJson("settings_save_failed"));
        return;
    }
}

void Server::HandleRevokeSelf(Stream &stream, const DeviceRecord &device)
{
    bool found = false;
    std::string error;
    if (!config_.device_auth->RevokeDevice(device.id, found, error)) {
        std::fprintf(stderr, "pogohome_api: could not revoke device %s: %s\n", device.id.c_str(), error.c_str());
        SendJsonResponse(stream, 503, "Service Unavailable", ErrorJson("auth_unavailable"));
        return;
    }
    std::fprintf(stdout, "pogohome_api: device %s removed itself\n", device.id.c_str());
    SendJsonResponse(stream, 200, "OK", "{\"revoked\":true}");
}

void Server::HandleSse(Stream &stream)
{
    static const char kSseHeaders[] = "HTTP/1.1 200 OK\r\n"
                                       "Content-Type: text/event-stream\r\n"
                                       "Cache-Control: no-store, no-cache\r\n"
                                       "Connection: keep-alive\r\n"
                                       "\r\n";
    if (!stream.SendAll(kSseHeaders, sizeof(kSseHeaders) - 1)) {
        return;
    }

    auto snapshot_json = [this](const ingest::RegistryView &view, uint64_t generation) {
        std::string json = BuildSnapshotJson(view, generation, CurrentUnixMillis());
        AppendSettingsFields(json);
        return json;
    };

    // Ordering note: generation() is read BEFORE snapshot() so the worst
    // case if a change lands between the two calls is one harmless extra
    // "nothing really new" resend on the very next wait_for_change() wakeup
    // (last_seen ends up one generation older than what the snapshot
    // already reflects) -- never a MISSED update, which is what the
    // opposite order could cause (a change landing between snapshot() and
    // generation() would make last_seen look newer than what was actually
    // captured, and that change would never trigger a wakeup on its own).
    // The settings change counter follows the same rule.
    uint64_t last_seen = registry_.generation();
    uint64_t last_settings = config_.settings ? config_.settings->change_counter() : 0;
    ingest::RegistryView view = registry_.view();
    if (!SendSseEvent(stream, "snapshot", snapshot_json(view, last_seen))) {
        return;
    }
    auto last_activity = std::chrono::steady_clock::now();

    while (!should_stop_.load(std::memory_order_relaxed)) {
        bool changed = registry_.wait_for_change(&last_seen, config_.sse_poll_interval);
        if (should_stop_.load(std::memory_order_relaxed)) {
            return;
        }
        // A saved settings change is announced like a registry change (within
        // one poll interval), so every open app sees the new
        // settings_revision without polling for it.
        if (config_.settings) {
            uint64_t settings_now = config_.settings->change_counter();
            if (settings_now != last_settings) {
                last_settings = settings_now;
                changed = true;
            }
        }
        if (changed) {
            ingest::RegistryView latest = registry_.view();
            if (!SendSseEvent(stream, "snapshot", snapshot_json(latest, last_seen))) {
                return;
            }
            last_activity = std::chrono::steady_clock::now();
        } else {
            auto now = std::chrono::steady_clock::now();
            if (now - last_activity >= config_.sse_heartbeat_interval) {
                if (!SendSseComment(stream, "heartbeat")) {
                    return;
                }
                last_activity = now;
            }
        }
    }
}

} // namespace pogohome_api
