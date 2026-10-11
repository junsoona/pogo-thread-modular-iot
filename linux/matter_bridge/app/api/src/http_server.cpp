#include "pogohome_api/build_info.hpp"
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

#include "pogohome_api/crypto_util.hpp"
#include "pogohome_api/hex.hpp"
#include "pogohome_api/json_snapshot.hpp"
#include "pogohome_api/json_value.hpp"
#include "pogohome_api/otbr_helper_protocol.hpp"

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

constexpr char kEnrollmentsPath[] = "/api/v1/power-base-enrollments";

bool IsEnrollmentPath(const std::string &path)
{
    const size_t n = sizeof(kEnrollmentsPath) - 1;
    return path.compare(0, n, kEnrollmentsPath) == 0 && (path.size() == n || path[n] == '/');
}

// The session id of "/api/v1/power-base-enrollments/<id>", "" for the
// collection itself.
std::string EnrollmentIdOf(const std::string &path)
{
    const size_t n = sizeof(kEnrollmentsPath) - 1;
    return path.size() > n ? path.substr(n + 1) : std::string();
}

constexpr char kRemovalsPath[] = "/api/v1/power-base-removals";

bool IsRemovalPath(const std::string &path)
{
    const size_t n = sizeof(kRemovalsPath) - 1;
    return path.compare(0, n, kRemovalsPath) == 0 && (path.size() == n || path[n] == '/');
}

// The removal id of "/api/v1/power-base-removals/<id>", "" for the
// collection itself.
std::string RemovalIdOf(const std::string &path)
{
    const size_t n = sizeof(kRemovalsPath) - 1;
    return path.size() > n ? path.substr(n + 1) : std::string();
}

constexpr char kForgetsPath[] = "/api/v1/power-base-forgets";

bool IsForgetPath(const std::string &path)
{
    const size_t n = sizeof(kForgetsPath) - 1;
    return path.compare(0, n, kForgetsPath) == 0 && (path.size() == n || path[n] == '/');
}

std::string ErrorWithRemovalJson(const char *error, const RemovalJob &job)
{
    std::string out = "{\"error\":";
    AppendJsonStringLiteral(out, error);
    out += ",\"removal\":";
    out += RemovalJobJson(job);
    out += '}';
    return out;
}

// `grant`: only for the answer to the POST that started the session -- the
// one place the management key is ever written out. Every other answer names
// the generation alone.
std::string EnrollmentSessionJson(const EnrollmentSession &s, const ManagementGrant *grant = nullptr)
{
    std::string out = "{\"session_id\":";
    AppendJsonStringLiteral(out, s.session_id);
    out += ",\"eui64\":";
    AppendJsonStringLiteral(out, s.eui64);
    out += ",\"state\":";
    AppendJsonStringLiteral(out, EnrollmentStateName(s.state));
    out += ",\"finished\":";
    out += s.finished ? "true" : "false";
    out += ",\"joiner_entry\":";
    AppendJsonStringLiteral(out, JoinerEntryName(s.joiner_entry));
    out += ",\"error\":";
    if (s.error.empty()) {
        out += "null";
    } else {
        AppendJsonStringLiteral(out, s.error);
    }
    out += ",\"window_seconds\":" + std::to_string(s.window_seconds);
    out += ",\"created_at_unix_ms\":" + std::to_string(s.created_at_unix_ms);
    out += ",\"expires_at_unix_ms\":" + std::to_string(s.expires_at_unix_ms);
    out += ",\"expires_in_ms\":" + std::to_string(s.expires_in_ms);
    out += ",\"heartbeat_received_at_unix_ms\":";
    out += s.heartbeat_received_at_unix_ms > 0 ? std::to_string(s.heartbeat_received_at_unix_ms) : "null";
    out += ",\"started_by_device_id\":";
    AppendJsonStringLiteral(out, s.device_id);
    // Only a session that asked for bridge endpoint delivery has this key, so
    // every other session's JSON is what it was before the option existed.
    // null: not established (yet) -- possible while "preparing" and for a
    // session that ended before it was.
    if (s.bridge_endpoint_requested) {
        out += ",\"bridge_endpoint\":";
        if (s.has_bridge_endpoint) {
            out += "{\"version\":" + std::to_string(s.bridge_endpoint.version);
            out += ",\"address\":";
            AppendJsonStringLiteral(out, s.bridge_endpoint.address);
            out += ",\"port\":" + std::to_string(static_cast<unsigned>(s.bridge_endpoint.port));
            out += '}';
        } else {
            out += "null";
        }
    }
    // Only a session that asked for a management credential has this key.
    if (s.management_requested) {
        out += ",\"management\":{\"version\":" + std::to_string(kPowerBaseManagementVersion);
        out += ",\"generation\":";
        AppendJsonStringLiteral(out, s.management_generation);
        if (grant != nullptr && !grant->key.empty()) {
            out += ",\"hub_id\":";
            AppendJsonStringLiteral(out, grant->hub_id);
            out += ",\"key\":";
            AppendJsonStringLiteral(out, grant->key);
        }
        out += '}';
    }
    out += '}';
    return out;
}

std::string ErrorWithSessionJson(const char *error, const EnrollmentSession &s)
{
    std::string out = "{\"error\":";
    AppendJsonStringLiteral(out, error);
    out += ",\"session\":";
    out += EnrollmentSessionJson(s);
    out += '}';
    return out;
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

Server::Server(ingest::Registry &registry, ServerConfig config) : registry_(registry), config_(std::move(config))
{
    // One value per run of the bridge (SnapshotBody()). Not a secret.
    snapshot_stream_ = RandomHex(16);
    if (!IsLowerHex(snapshot_stream_, 32)) {
        snapshot_stream_.clear();
    }
}

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
        AppendEnrollmentField(body);
        AppendManagementFields(body);
        AppendBuildField(body);
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
        if (config_.after_registry_copy) {
            config_.after_registry_copy();
        }
        SendJsonResponse(stream, 200, "OK", SnapshotBody(std::move(view), generation));
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

    if (IsEnrollmentPath(path)) {
        HandleEnrollmentRoute(stream, request, buf);
        return;
    }

    if (IsRemovalPath(path)) {
        HandleRemovalRoute(stream, request, buf);
        return;
    }

    if (IsForgetPath(path)) {
        HandleForgetRoute(stream, request, buf);
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
        if (!is_claim && !AuthenticateDevice(stream, request, device)) {
            return;
        }

        if ((method == "PUT" || method == "POST") &&
            !ReadRequestBody(stream, request, buf, config_.max_request_body_bytes)) {
            return;
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

void Server::AppendEnrollmentField(std::string &json) const
{
    if (!config_.enrollment_requested || json.empty() || json.back() != '}') {
        return;
    }
    json.pop_back();
    json += ",\"power_base_enrollment\":\"";
    json += SettingsEnabled() && config_.enrollment ? "available" : "unavailable";
    json += "\"}";
}

// Which build of the bridge answers (docs/versioning.md): identification
// only, after every earlier field. A client that does not know the field
// ignores it; a bridge built without the version stamp leaves it out.
void Server::AppendBuildField(std::string &json) const
{
    const std::string build = config_.report_build ? BuildInfoJson() : std::string();
    if (build.empty() || json.empty() || json.back() != '}') {
        return;
    }
    json.pop_back();
    json += ",\"build\":" + build + "}";
}

void Server::AppendManagementFields(std::string &json) const
{
    if (!config_.management_requested || json.empty() || json.back() != '}') {
        return;
    }
    json.pop_back();
    // The version of the management credential and removal API this bridge
    // speaks, and whether it can be used right now.
    json += ",\"power_base_management\":" + std::to_string(kPowerBaseManagementVersion);
    json += ",\"power_base_removal\":\"";
    json += ManagementEnabled() ? "available" : "unavailable";
    // Taking an offline Power Base out of this hub's list
    // (/api/v1/power-base-forgets): a client that does not find this field
    // must not assume the routes exist.
    json += "\",\"power_base_forget\":" + std::to_string(kPowerBaseForgetVersion);
    // What POST /api/v1/power-base-removals has to say: 2 is the request that
    // names the registration it was confirmed for. A client that does not
    // find this field (or finds a number it does not know) must not send a
    // removal request at all -- the request of version 1 named the Base
    // alone, and this hub does not carry that out.
    json += ",\"power_base_removal_request\":" + std::to_string(kPowerBaseRemovalRequestVersion);
    json += "}";
}

std::string Server::SnapshotBody(ingest::RegistryView view, uint64_t generation) const
{
    std::lock_guard<std::mutex> order(snapshot_order_mutex_);
    // The caller copied the Registry before it came here, and another caller
    // may have built a snapshot from a later copy meanwhile: this one would
    // then get the larger number for the older state. Such a copy is replaced
    // by one made now.
    if (view.generation < snapshot_view_generation_) {
        view = registry_.view();
        generation = view.generation;
    }
    std::string body = SnapshotJson(view, generation);
    // Without management the snapshot was built from `view`. With management
    // it may have been built from a later copy that management made itself --
    // and management remembers that one: it takes in no copy older than the
    // last (PowerBaseManagement::DescribeForSnapshot()).
    if (view.generation > snapshot_view_generation_) {
        snapshot_view_generation_ = view.generation;
    }
    AppendSettingsFields(body);
    if (snapshot_stream_.empty() || body.empty() || body.back() != '}') {
        return body;
    }
    body.pop_back();
    body += ",\"snapshot_stream\":";
    AppendJsonStringLiteral(body, snapshot_stream_);
    body += ",\"snapshot_sequence\":" + std::to_string(++snapshot_sequence_);
    body += '}';
    return body;
}

std::string Server::SnapshotJson(const ingest::RegistryView &view, uint64_t generation) const
{
    if (!config_.management_requested || !ManagementEnabled()) {
        // Exactly the snapshot of a bridge without management.
        return BuildSnapshotJson(view, generation, CurrentUnixMillis());
    }
    // The caller copied the Registry some time ago; management is read now.
    // Both are made to agree in one critical section of management: its
    // lists are read together, and a copy from before it last changed which
    // Bases are listed is replaced by one made at that moment.
    ingest::RegistryView listed = view;
    PowerBaseManagement::Description described;
    if (!config_.management->DescribeForSnapshot(listed, described)) {
        return BuildSnapshotJson(view, generation, CurrentUnixMillis());
    }
    SnapshotManagement management;
    management.managed_power_bases = std::move(described.managed_power_bases);
    management.self_confirming_power_bases = std::move(described.self_confirming_power_bases);
    std::string list = RemovalListJson(described.removals); // {"removals":[...]}
    const size_t open = list.find('[');
    const size_t close = list.rfind(']');
    if (open != std::string::npos && close != std::string::npos && close > open) {
        management.removals_json = list.substr(open, close - open + 1);
    }
    // The registration behind every listed Base, and the list removals.
    management.forget_supported = true;
    management.records = std::move(described.records);
    management.removal_serials = std::move(described.removal_serials);
    std::string forgets = ForgetListJson(described.forgets); // {"forgets":[...]}
    const size_t forgets_open = forgets.find('[');
    const size_t forgets_close = forgets.rfind(']');
    if (forgets_open != std::string::npos && forgets_close != std::string::npos && forgets_close > forgets_open) {
        management.forgets_json = forgets.substr(forgets_open, forgets_close - forgets_open + 1);
    }
    // And the Bases that are out of the list for good until they are
    // registered again: why a Base (and its modules) is not above.
    management.departures_json = DeparturesJson(described.departures);
    return BuildSnapshotJson(listed, generation, CurrentUnixMillis(), &management);
}

void Server::HandleForgetRoute(Stream &stream, Request &request, std::string &buf)
{
    // The gates of the removal routes, in the same order. This is NOT a
    // removal and not its DELETE (that cancels a pending removal): these
    // routes take an offline Power Base out of this hub's list and say
    // nothing about the board.
    if (!config_.management_requested) {
        SendJsonResponse(stream, 404, "Not Found", ErrorJson("forget_disabled"));
        return;
    }
    if (!stream.is_tls()) {
        SendJsonResponse(stream, 403, "Forbidden", ErrorJson("tls_required"));
        return;
    }
    if (!ManagementEnabled()) {
        SendJsonResponse(stream, 503, "Service Unavailable", ErrorJson("forget_unavailable", "reason", "startup"));
        return;
    }
    const bool is_collection = request.path == kForgetsPath;
    const std::string forget_id = is_collection ? std::string() : request.path.substr(sizeof(kForgetsPath));
    const bool method_ok = is_collection ? (request.method == "POST" || request.method == "GET") : request.method == "GET";
    if (!method_ok) {
        // In particular no DELETE: there is nothing to cancel or to undo. A
        // Base comes back by being registered again.
        SendJsonResponse(stream, 405, "Method Not Allowed", ErrorJson("method_not_allowed"));
        return;
    }
    if (!ParseHeaders(buf, request.headers)) {
        SendJsonResponse(stream, 400, "Bad Request", ErrorJson("malformed_headers"));
        return;
    }
    DeviceRecord device;
    if (!AuthenticateDevice(stream, request, device)) {
        return;
    }
    if (is_collection && request.method == "POST") {
        if (ReadRequestBody(stream, request, buf, config_.max_forget_body_bytes)) {
            HandleForgetStart(stream, request, device);
        }
        return;
    }
    if (is_collection) {
        SendJsonResponse(stream, 200, "OK", ForgetListJson(config_.management->ListForgets()));
        return;
    }
    ForgetRecord record;
    if (!IsLowerHex(forget_id, 32) || !config_.management->GetForget(forget_id, record)) {
        SendJsonResponse(stream, 404, "Not Found", ErrorJson("forget_not_found"));
        return;
    }
    SendJsonResponse(stream, 200, "OK", ForgetRecordJson(record));
}

void Server::HandleForgetStart(Stream &stream, Request &request, const DeviceRecord &device)
{
    // Exactly {"power_base_uuid": "<16 hex>", "record": "<32 hex>",
    // "request_id": "<32 hex>"}. Error answers never echo a value.
    JsonValue root;
    std::string parse_error;
    JsonParseLimits limits;
    limits.max_depth = 2;
    limits.max_string_bytes = 64;
    limits.max_values = 8;
    const bool parsed = ParseJson(request.body, root, parse_error, limits);
    const JsonValue *uuid = parsed && root.IsObject() ? root.Find("power_base_uuid") : nullptr;
    const JsonValue *record = parsed && root.IsObject() ? root.Find("record") : nullptr;
    const JsonValue *request_id = parsed && root.IsObject() ? root.Find("request_id") : nullptr;
    if (uuid == nullptr || !uuid->IsString() || record == nullptr || !record->IsString() || request_id == nullptr ||
        !request_id->IsString() || root.object_members.size() != 3) {
        SendJsonResponse(stream, 400, "Bad Request", ErrorJson("invalid_request"));
        return;
    }
    std::string eui64;
    if (!NormalizeEui64(uuid->string_value, eui64)) {
        SendJsonResponse(stream, 422, "Unprocessable Entity", ErrorJson("invalid_power_base_uuid"));
        return;
    }
    if (!IsLowerHex(record->string_value, 32)) {
        SendJsonResponse(stream, 422, "Unprocessable Entity", ErrorJson("invalid_record"));
        return;
    }
    if (!IsLowerHex(request_id->string_value, 32)) {
        SendJsonResponse(stream, 422, "Unprocessable Entity", ErrorJson("invalid_request_id"));
        return;
    }

    ForgetRecord done;
    switch (config_.management->ForgetPowerBase(device.id, eui64, record->string_value, request_id->string_value, done)) {
    case ForgetOutcome::Removed: {
        std::string location = "Location: ";
        location += kForgetsPath;
        location += "/" + done.forget_id + "\r\n";
        SendResponse(stream, 201, "Created", ForgetRecordJson(done), location.c_str());
        return;
    }
    case ForgetOutcome::Existing:
        // The same request again: the same record, nothing done twice.
        SendJsonResponse(stream, 200, "OK", ForgetRecordJson(done));
        return;
    case ForgetOutcome::UnknownPowerBase:
        SendJsonResponse(stream, 404, "Not Found", ErrorJson("unknown_power_base"));
        return;
    case ForgetOutcome::Online:
        // Heard from right now: this is what the removal is for.
        SendJsonResponse(stream, 409, "Conflict", ErrorJson("power_base_online"));
        return;
    case ForgetOutcome::StaleRecord:
        // Registered again (or a registration was started) since the list
        // was read: that registration is not this request's to remove.
        SendJsonResponse(stream, 409, "Conflict", ErrorJson("stale_record"));
        return;
    case ForgetOutcome::EnrollmentInProgress:
        SendJsonResponse(stream, 409, "Conflict", ErrorJson("enrollment_in_progress"));
        return;
    case ForgetOutcome::RequestConflict:
        SendJsonResponse(stream, 409, "Conflict", ErrorJson("request_id_conflict"));
        return;
    case ForgetOutcome::CapacityFull:
        SendJsonResponse(stream, 409, "Conflict",
                         ErrorJson("forget_capacity_full", "limit", std::to_string(kMaxBlockedPowerBases)));
        return;
    case ForgetOutcome::TooMany:
        SendJsonResponse(stream, 503, "Service Unavailable", ErrorJson("too_many_forgets"));
        return;
    case ForgetOutcome::Unavailable:
        SendJsonResponse(stream, 503, "Service Unavailable", ErrorJson("forget_unavailable", "reason", "storage"));
        return;
    }
}

void Server::HandleRemovalRoute(Stream &stream, Request &request, std::string &buf)
{
    // The same gate order as the enrollment routes: nothing from a
    // plain-HTTP request is used, and nothing happens without a paired
    // device's token. This is NOT the enrollment session's DELETE: that one
    // cancels a registration attempt; these routes unenroll a registered
    // Power Base.
    if (!config_.management_requested) {
        SendJsonResponse(stream, 404, "Not Found", ErrorJson("removal_disabled"));
        return;
    }
    if (!stream.is_tls()) {
        SendJsonResponse(stream, 403, "Forbidden", ErrorJson("tls_required"));
        return;
    }
    if (!ManagementEnabled()) {
        SendJsonResponse(stream, 503, "Service Unavailable", ErrorJson("removal_unavailable", "reason", "startup"));
        return;
    }
    const std::string removal_id = RemovalIdOf(request.path);
    const bool is_collection = request.path == kRemovalsPath;
    // "receipt" is not a removal id (those are 32 hex digits).
    const bool is_receipt = removal_id == "receipt";
    const bool method_ok = is_collection ? (request.method == "POST" || request.method == "GET")
                                         : is_receipt ? request.method == "POST"
                                                      : (request.method == "GET" || request.method == "DELETE");
    if (!method_ok) {
        SendJsonResponse(stream, 405, "Method Not Allowed", ErrorJson("method_not_allowed"));
        return;
    }
    if (!ParseHeaders(buf, request.headers)) {
        SendJsonResponse(stream, 400, "Bad Request", ErrorJson("malformed_headers"));
        return;
    }
    DeviceRecord device;
    if (!AuthenticateDevice(stream, request, device)) {
        return;
    }

    if (is_collection && request.method == "POST") {
        if (ReadRequestBody(stream, request, buf, config_.max_removal_body_bytes)) {
            HandleRemovalStart(stream, request, device);
        }
        return;
    }
    if (is_collection) {
        SendJsonResponse(stream, 200, "OK", RemovalListJson(config_.management->ListRemovals()));
        return;
    }
    if (is_receipt) {
        if (ReadRequestBody(stream, request, buf, config_.max_receipt_body_bytes)) {
            HandleRemovalReceipt(stream, request);
        }
        return;
    }

    RemovalJob job;
    if (!IsLowerHex(removal_id, 32)) {
        SendJsonResponse(stream, 404, "Not Found", ErrorJson("removal_not_found"));
        return;
    }
    if (request.method == "GET") {
        if (!config_.management->GetRemoval(removal_id, job)) {
            SendJsonResponse(stream, 404, "Not Found", ErrorJson("removal_not_found"));
            return;
        }
        SendJsonResponse(stream, 200, "OK", RemovalJobJson(job));
        return;
    }
    switch (config_.management->CancelRemoval(removal_id, job)) {
    case RemovalCancelOutcome::Cancelled:
        SendJsonResponse(stream, 200, "OK", RemovalJobJson(job));
        return;
    case RemovalCancelOutcome::NotCancellable:
        // The command may already be on the board (or the job is over):
        // nothing a cancel could promise.
        SendJsonResponse(stream, 409, "Conflict", ErrorWithRemovalJson("removal_not_cancellable", job));
        return;
    case RemovalCancelOutcome::NotFound:
        SendJsonResponse(stream, 404, "Not Found", ErrorJson("removal_not_found"));
        return;
    case RemovalCancelOutcome::Unavailable:
        SendJsonResponse(stream, 503, "Service Unavailable", ErrorJson("removal_unavailable", "reason", "storage"));
        return;
    }
}

void Server::HandleRemovalStart(Stream &stream, Request &request, const DeviceRecord &device)
{
    // Exactly {"power_base_uuid": "<16 hex>", "record": "<32 hex>",
    // "removal_serial": <integer>, "request_id": "<32 hex>"}: the Base, the
    // registration the device was shown and confirmed, how many removals had
    // been asked of that registration then, and an id of this confirmation.
    // Error answers never echo a value.
    JsonValue root;
    std::string parse_error;
    JsonParseLimits limits;
    limits.max_depth = 2;
    limits.max_string_bytes = 64;
    limits.max_values = 8;
    const bool parsed = ParseJson(request.body, root, parse_error, limits);
    const JsonValue *uuid = parsed && root.IsObject() ? root.Find("power_base_uuid") : nullptr;
    const JsonValue *record = parsed && root.IsObject() ? root.Find("record") : nullptr;
    const JsonValue *serial = parsed && root.IsObject() ? root.Find("removal_serial") : nullptr;
    const JsonValue *request_id = parsed && root.IsObject() ? root.Find("request_id") : nullptr;
    if (uuid != nullptr && uuid->IsString() && root.object_members.size() == 1) {
        // What a client older than this contract sends: the Base alone. It
        // names no registration, so it cannot be told from the same request
        // made for an earlier registration of that Base -- and is not carried
        // out, for any Base, whatever is listed. Nothing was looked up.
        std::fprintf(stdout,
                     "pogohome_api: power base removal: a request of device %s that names no registration was "
                     "refused (the app is older than this hub's removal request, version %d)\n",
                     device.id.c_str(), kPowerBaseRemovalRequestVersion);
        SendJsonResponse(stream, 422, "Unprocessable Entity", ErrorJson("removal_registration_required"));
        return;
    }
    if (uuid == nullptr || !uuid->IsString() || record == nullptr || !record->IsString() || serial == nullptr ||
        serial->type != JsonValue::Type::Number || request_id == nullptr || !request_id->IsString() ||
        root.object_members.size() != 4) {
        SendJsonResponse(stream, 400, "Bad Request", ErrorJson("invalid_request"));
        return;
    }
    std::string eui64;
    if (!NormalizeEui64(uuid->string_value, eui64)) {
        SendJsonResponse(stream, 422, "Unprocessable Entity", ErrorJson("invalid_power_base_uuid"));
        return;
    }
    if (!IsLowerHex(record->string_value, 32)) {
        SendJsonResponse(stream, 422, "Unprocessable Entity", ErrorJson("invalid_record"));
        return;
    }
    if (!serial->IsInteger() || serial->int_value < 0 || serial->int_value > 0xffffffffLL) {
        SendJsonResponse(stream, 422, "Unprocessable Entity", ErrorJson("invalid_removal_serial"));
        return;
    }
    if (!IsLowerHex(request_id->string_value, 32)) {
        SendJsonResponse(stream, 422, "Unprocessable Entity", ErrorJson("invalid_request_id"));
        return;
    }

    RemovalJob job;
    switch (config_.management->RequestRemoval(device.id, eui64, record->string_value,
                                               static_cast<uint32_t>(serial->int_value), request_id->string_value,
                                               job)) {
    case RemovalRequestOutcome::Created: {
        std::string location = "Location: ";
        location += kRemovalsPath;
        location += "/" + job.removal_id + "\r\n";
        SendResponse(stream, 202, "Accepted", RemovalJobJson(job), location.c_str());
        return;
    }
    case RemovalRequestOutcome::Existing:
        // This request again, or the registration it names is being removed
        // already: that job, whoever asks, and nothing new.
        SendJsonResponse(stream, 200, "OK", RemovalJobJson(job));
        return;
    case RemovalRequestOutcome::StaleRecord:
        // Registered again (or a registration was started), or a removal was
        // asked of this registration and ended, since the device read the
        // list: what is listed now is not what this request confirmed.
        SendJsonResponse(stream, 409, "Conflict", ErrorJson("stale_record"));
        return;
    case RemovalRequestOutcome::RequestConflict:
        SendJsonResponse(stream, 409, "Conflict", ErrorJson("request_id_conflict"));
        return;
    case RemovalRequestOutcome::UnknownPowerBase:
        SendJsonResponse(stream, 404, "Not Found", ErrorJson("unknown_power_base"));
        return;
    case RemovalRequestOutcome::Unsupported:
        // No fallback: hiding the Base in a list is not unenrolling it.
        SendJsonResponse(stream, 409, "Conflict", ErrorJson("power_base_management_unsupported"));
        return;
    case RemovalRequestOutcome::EnrollmentInProgress:
        SendJsonResponse(stream, 409, "Conflict", ErrorJson("enrollment_in_progress"));
        return;
    case RemovalRequestOutcome::TooMany:
        SendJsonResponse(stream, 503, "Service Unavailable", ErrorJson("too_many_removals"));
        return;
    case RemovalRequestOutcome::CapacityFull:
        // Not "try again later": the hub remembers as many removed Power
        // Bases as it can, and forgets none of them to make room. Nothing
        // was stored and the board gets no command.
        SendJsonResponse(stream, 409, "Conflict",
                         ErrorJson("removal_capacity_full", "limit", std::to_string(kMaxBlockedPowerBases)));
        return;
    case RemovalRequestOutcome::Unavailable:
        SendJsonResponse(stream, 503, "Service Unavailable", ErrorJson("removal_unavailable", "reason", "storage"));
        return;
    }
}

void Server::HandleRemovalReceipt(Stream &stream, Request &request)
{
    // Exactly {"receipt": "<192 lowercase hex>"}: the board's release receipt
    // (its UNENROLL_DONE/ERASED management message) as an app read it over
    // BLE. The device token only says who relays it; what the receipt says
    // is believed only if its MAC verifies with the key of that enrollment.
    JsonValue root;
    std::string parse_error;
    JsonParseLimits limits;
    limits.max_depth = 2;
    limits.max_string_bytes = 2 * PB_MGMT_UNENROLL_DONE_LEN;
    limits.max_values = 4;
    const bool parsed = ParseJson(request.body, root, parse_error, limits);
    const JsonValue *hex = parsed && root.IsObject() ? root.Find("receipt") : nullptr;
    if (hex == nullptr || !hex->IsString() || root.object_members.size() != 1) {
        SendJsonResponse(stream, 400, "Bad Request", ErrorJson("invalid_request"));
        return;
    }
    if (!IsLowerHex(hex->string_value, 2 * PB_MGMT_UNENROLL_DONE_LEN)) {
        SendJsonResponse(stream, 422, "Unprocessable Entity", ErrorJson("invalid_receipt"));
        return;
    }
    std::vector<uint8_t> receipt(PB_MGMT_UNENROLL_DONE_LEN);
    auto nibble = [](char c) { return static_cast<uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10); };
    for (size_t i = 0; i < receipt.size(); i++) {
        receipt[i] = static_cast<uint8_t>(nibble(hex->string_value[2 * i]) << 4 | nibble(hex->string_value[2 * i + 1]));
    }
    RemovalJob job;
    switch (config_.management->SubmitReleaseReceipt(receipt, job)) {
    case ReceiptOutcome::Completed:
    case ReceiptOutcome::AlreadyCompleted:
        // The same receipt again is the same answer.
        SendJsonResponse(stream, 200, "OK", RemovalJobJson(job));
        return;
    case ReceiptOutcome::Malformed:
        SendJsonResponse(stream, 422, "Unprocessable Entity", ErrorJson("invalid_receipt"));
        return;
    case ReceiptOutcome::Rejected:
        // One answer for another hub's receipt, an unknown or superseded
        // enrollment, a cancelled job and a wrong MAC.
        SendJsonResponse(stream, 409, "Conflict", ErrorJson("receipt_rejected"));
        return;
    case ReceiptOutcome::Unavailable:
        SendJsonResponse(stream, 503, "Service Unavailable", ErrorJson("removal_unavailable", "reason", "storage"));
        return;
    }
}

bool Server::AuthenticateDevice(Stream &stream, const Request &request, DeviceRecord &device)
{
    const std::string *authorization = request.Header("authorization");
    std::string auth_error;
    AuthOutcome auth = authorization == nullptr
                           ? AuthOutcome::Invalid
                           : config_.device_auth->Authenticate(*authorization, NowUnixMs(), device, auth_error);
    if (auth == AuthOutcome::StorageError) {
        std::fprintf(stderr, "pogohome_api: device credentials unavailable: %s\n", auth_error.c_str());
        SendJsonResponse(stream, 503, "Service Unavailable", ErrorJson("auth_unavailable"));
        return false;
    }
    if (auth != AuthOutcome::Ok) {
        SendResponse(stream, 401, "Unauthorized", ErrorJson("unauthorized"),
                     "WWW-Authenticate: Bearer realm=\"pogohome\"\r\n");
        return false;
    }
    return true;
}

bool Server::ReadRequestBody(Stream &stream, Request &request, const std::string &buf, size_t max_bytes)
{
    size_t header_end = buf.find("\r\n\r\n") + 4;
    BodyResult body_result = ReadBody(stream, request.headers, buf.substr(header_end), max_bytes, request.body);
    switch (body_result) {
    case BodyResult::Ok:
        return true;
    case BodyResult::TooLarge:
        SendJsonResponse(stream, 413, "Payload Too Large", ErrorJson("request_too_large"));
        return false;
    case BodyResult::LengthRequired:
        SendJsonResponse(stream, 411, "Length Required", ErrorJson("length_required"));
        return false;
    case BodyResult::BadRequest:
        SendJsonResponse(stream, 400, "Bad Request", ErrorJson("invalid_content_length"));
        return false;
    case BodyResult::Incomplete:
        return false;
    }
    return false;
}

void Server::HandleEnrollmentRoute(Stream &stream, Request &request, std::string &buf)
{
    // The same gate order as the settings routes: nothing from a plain-HTTP
    // request is used, and nothing happens without a paired device's token.
    if (!config_.enrollment_requested) {
        SendJsonResponse(stream, 404, "Not Found", ErrorJson("enrollment_disabled"));
        return;
    }
    if (!stream.is_tls()) {
        SendJsonResponse(stream, 403, "Forbidden", ErrorJson("tls_required"));
        return;
    }
    if (!SettingsEnabled() || !config_.enrollment) {
        SendJsonResponse(stream, 503, "Service Unavailable", ErrorJson("enrollment_unavailable", "reason", "startup"));
        return;
    }
    const std::string session_id = EnrollmentIdOf(request.path);
    const bool is_collection = request.path == kEnrollmentsPath;
    const bool method_ok = is_collection ? request.method == "POST"
                                         : (request.method == "GET" || request.method == "DELETE");
    if (!method_ok) {
        SendJsonResponse(stream, 405, "Method Not Allowed", ErrorJson("method_not_allowed"));
        return;
    }
    if (!ParseHeaders(buf, request.headers)) {
        SendJsonResponse(stream, 400, "Bad Request", ErrorJson("malformed_headers"));
        return;
    }
    DeviceRecord device;
    if (!AuthenticateDevice(stream, request, device)) {
        return;
    }

    if (is_collection) {
        const bool body_ok = ReadRequestBody(stream, request, buf, config_.max_enrollment_body_bytes);
        SecureWipe(buf); // may hold the start of the body, i.e. the PSKd
        if (body_ok) {
            HandleEnrollmentStart(stream, request, device);
        }
        SecureWipe(request.body);
        return;
    }

    EnrollmentSession session;
    if (!IsLowerHex(session_id, 32)) {
        SendJsonResponse(stream, 404, "Not Found", ErrorJson("session_not_found"));
        return;
    }
    if (request.method == "GET") {
        if (!config_.enrollment->Get(device.id, session_id, session)) {
            SendJsonResponse(stream, 404, "Not Found", ErrorJson("session_not_found"));
            return;
        }
        SendJsonResponse(stream, 200, "OK", EnrollmentSessionJson(session));
        return;
    }
    switch (config_.enrollment->Cancel(device.id, session_id, session)) {
    case EnrollmentCancelOutcome::Cancelled:
        SendJsonResponse(stream, 200, "OK", EnrollmentSessionJson(session));
        return;
    case EnrollmentCancelOutcome::AlreadyFinal:
        SendJsonResponse(stream, 409, "Conflict", ErrorWithSessionJson("session_already_final", session));
        return;
    case EnrollmentCancelOutcome::NotFound:
        SendJsonResponse(stream, 404, "Not Found", ErrorJson("session_not_found"));
        return;
    }
}

void Server::HandleEnrollmentStart(Stream &stream, Request &request, const DeviceRecord &device)
{
    // {"eui64": "<16 hex>", "pskd": "<6..32 of 0-9 A-Z without I O Q Z>"},
    // optionally with "bridge_endpoint_version": 1 to have this hub's bridge
    // endpoint established and reported with the session. The client never
    // sends an address or a port: the endpoint is made here.
    // Error answers never echo any value.
    JsonValue root;
    std::string parse_error;
    JsonParseLimits limits;
    limits.max_depth = 2;
    limits.max_string_bytes = 64;
    limits.max_values = 8;
    const bool parsed = ParseJson(request.body, root, parse_error, limits);
    SecureWipe(request.body);
    auto wipe_json = [&root]() {
        for (auto &member : root.object_members) {
            SecureWipe(member.second.string_value);
        }
    };
    const JsonValue *eui = parsed && root.IsObject() ? root.Find("eui64") : nullptr;
    const JsonValue *pskd_value = parsed && root.IsObject() ? root.Find("pskd") : nullptr;
    const JsonValue *endpoint_version = parsed && root.IsObject() ? root.Find("bridge_endpoint_version") : nullptr;
    const JsonValue *management_version = parsed && root.IsObject() ? root.Find("management_version") : nullptr;
    // Exactly the two fields, or those two and the options; each option must
    // be a plain integer. "management_version" (docs/power-base-management.md)
    // is only accepted together with "bridge_endpoint_version": the
    // credential is used on the endpoint that option establishes.
    const size_t expected_members = 2 + (endpoint_version != nullptr ? 1 : 0) + (management_version != nullptr ? 1 : 0);
    if (eui == nullptr || !eui->IsString() || pskd_value == nullptr || !pskd_value->IsString() ||
        root.object_members.size() != expected_members || (endpoint_version != nullptr && !endpoint_version->IsInteger()) ||
        (management_version != nullptr && (!management_version->IsInteger() || endpoint_version == nullptr))) {
        wipe_json();
        SendJsonResponse(stream, 400, "Bad Request", ErrorJson("invalid_request"));
        return;
    }
    EnrollmentStartOptions options;
    if (management_version != nullptr) {
        if (management_version->int_value != kPowerBaseManagementVersion) {
            wipe_json();
            SendJsonResponse(stream, 422, "Unprocessable Entity", ErrorJson("unsupported_management_version"));
            return;
        }
        options.management_version = kPowerBaseManagementVersion;
    }
    if (endpoint_version != nullptr) {
        if (endpoint_version->int_value != kBridgeEndpointVersion) {
            wipe_json();
            SendJsonResponse(stream, 422, "Unprocessable Entity", ErrorJson("unsupported_bridge_endpoint_version"));
            return;
        }
        options.bridge_endpoint_version = kBridgeEndpointVersion;
    }
    std::string eui64;
    if (!NormalizeEui64(eui->string_value, eui64)) {
        wipe_json();
        SendJsonResponse(stream, 422, "Unprocessable Entity", ErrorJson("invalid_eui64"));
        return;
    }
    if (!IsValidPskd(pskd_value->string_value)) {
        wipe_json();
        SendJsonResponse(stream, 422, "Unprocessable Entity", ErrorJson("invalid_pskd"));
        return;
    }
    std::string pskd = pskd_value->string_value;
    wipe_json();

    EnrollmentSession session;
    ManagementGrant grant;
    EnrollmentStartOutcome outcome = config_.enrollment->Start(device.id, eui64, pskd, session, options, &grant);
    SecureWipe(pskd);
    switch (outcome) {
    case EnrollmentStartOutcome::Started: {
        std::string location = "Location: ";
        location += kEnrollmentsPath;
        location += "/" + session.session_id + "\r\n";
        // The one answer that carries the management key: over TLS, to the
        // paired device that started the session, with Cache-Control:
        // no-store like every answer of this API. It is never logged.
        std::string body = EnrollmentSessionJson(session, &grant);
        SecureWipe(grant.key);
        SendResponse(stream, 202, "Accepted", body, location.c_str());
        SecureWipe(body);
        return;
    }
    case EnrollmentStartOutcome::Busy:
        SendJsonResponse(stream, 409, "Conflict",
                         session.session_id.empty() ? ErrorJson("enrollment_in_progress")
                                                    : ErrorWithSessionJson("enrollment_in_progress", session));
        return;
    case EnrollmentStartOutcome::AlreadyConnected:
        SendJsonResponse(stream, 409, "Conflict", ErrorJson("power_base_already_connected"));
        return;
    case EnrollmentStartOutcome::Stopping:
        SendJsonResponse(stream, 503, "Service Unavailable",
                         ErrorJson("enrollment_unavailable", "reason", "stopping"));
        return;
    case EnrollmentStartOutcome::InternalError:
        SendJsonResponse(stream, 500, "Internal Server Error", ErrorJson("internal_error"));
        return;
    case EnrollmentStartOutcome::BridgeEndpointUnavailable:
        SendJsonResponse(stream, 503, "Service Unavailable",
                         ErrorJson("enrollment_unavailable", "reason", "bridge_endpoint"));
        return;
    case EnrollmentStartOutcome::ManagementUnavailable:
        SendJsonResponse(stream, 503, "Service Unavailable", ErrorJson("enrollment_unavailable", "reason", "management"));
        return;
    case EnrollmentStartOutcome::ManagementRequired:
        // This Base was unenrolled from this hub; only a new authenticated
        // enrollment generation brings it back.
        SendJsonResponse(stream, 409, "Conflict", ErrorJson("management_required"));
        return;
    }
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
        return SnapshotBody(view, generation);
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
    if (config_.after_registry_copy) {
        config_.after_registry_copy();
    }
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
            if (config_.after_registry_copy) {
                config_.after_registry_copy();
            }
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
