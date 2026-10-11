// End-to-end tests of settings sharing over the REAL pogohome_api::Server on
// loopback: plain HTTP and TLS on one port, pairing with the code-bound
// proofs, bearer tokens, revisioned PUTs, conflicts, validation, request
// limits, SSE announcements of a settings change, revocation, and the
// failure states. The TLS client here does what the app does: it accepts
// the hub's self-signed certificate only for pairing, hashes it, and binds
// that hash into the proof.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ingest/registry.hpp"
#include "pogohome_api/atomic_file.hpp"
#include "pogohome_api/crypto_util.hpp"
#include "pogohome_api/device_auth.hpp"
#include "pogohome_api/http_server.hpp"
#include "pogohome_api/json_snapshot.hpp"
#include "pogohome_api/settings_store.hpp"
#include "pogohome_api/tls_identity.hpp"
#include "test_check.hpp"
#include "test_temp_dir.hpp"

using namespace pogohome_api;
using test_support::MakeTempDir;
using test_support::RemoveTree;
using test_support::WriteFile;

namespace {

const char kHubId[] = "3f0c9a4e8b1d4c2a9e7f6b5a4d3c2b1a";
const char kNonce[] = "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf";

int Connect(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    timeval tv{};
    tv.tv_sec = 3;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

struct Response {
    bool ok = false;
    int status = 0;
    std::string headers;
    std::string body;
    std::string peer_fp; // TLS only
};

Response Parse(const std::string &raw)
{
    Response r;
    size_t end = raw.find("\r\n\r\n");
    if (raw.rfind("HTTP/1.1 ", 0) != 0 || end == std::string::npos) {
        return r;
    }
    r.ok = true;
    r.status = std::atoi(raw.c_str() + 9);
    r.headers = raw.substr(0, end);
    r.body = raw.substr(end + 4);
    return r;
}

Response PlainRequest(uint16_t port, const std::string &request)
{
    int fd = Connect(port);
    if (fd < 0) {
        return Response();
    }
    send(fd, request.data(), request.size(), 0);
    std::string raw;
    char buf[4096];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        raw.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    return Parse(raw);
}

Response TlsRequest(uint16_t port, const std::string &request)
{
    Response r;
    int fd = Connect(port);
    if (fd < 0) {
        return r;
    }
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    // Like the app during pairing: the certificate is not trusted by a CA;
    // its hash is what gets checked (by the proof, and later by pinning).
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) == 1) {
        X509 *peer = SSL_get_peer_certificate(ssl);
        if (peer != nullptr) {
            unsigned char *der = nullptr;
            int len = i2d_X509(peer, &der);
            if (len > 0) {
                r.peer_fp = Sha256Hex(der, static_cast<size_t>(len));
                OPENSSL_free(der);
            }
            X509_free(peer);
        }
        SSL_write(ssl, request.data(), static_cast<int>(request.size()));
        std::string raw;
        char buf[4096];
        int n;
        while ((n = SSL_read(ssl, buf, sizeof(buf))) > 0) {
            raw.append(buf, static_cast<size_t>(n));
        }
        std::string fp = r.peer_fp;
        r = Parse(raw);
        r.peer_fp = fp;
        SSL_shutdown(ssl);
    }
    ERR_clear_error();
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);
    return r;
}

std::string Get(const std::string &path, const std::string &token = "")
{
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: hub\r\n";
    if (!token.empty()) {
        req += "Authorization: Bearer " + token + "\r\n";
    }
    return req + "\r\n";
}

std::string WithBody(const std::string &method, const std::string &path, const std::string &body,
                     const std::string &token = "")
{
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: hub\r\nContent-Type: application/json\r\n";
    if (!token.empty()) {
        req += "Authorization: Bearer " + token + "\r\n";
    }
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    return req;
}

std::string ClaimBody(const std::string &code, const std::string &cert_fp, const std::string &name)
{
    return "{\"device_name\":\"" + name + "\",\"client_nonce\":\"" + kNonce + "\",\"proof\":\"" +
           PairingClientProof(code, kHubId, cert_fp, kNonce, name) + "\"}";
}

std::string JsonField(const std::string &json, const std::string &key)
{
    std::string needle = "\"" + key + "\":\"";
    size_t start = json.find(needle);
    if (start == std::string::npos) {
        return std::string();
    }
    start += needle.size();
    return json.substr(start, json.find('"', start) - start);
}

std::string SettingsBody(const std::string &base, const std::string &home_name, bool with_module_in_office = true)
{
    std::string settings = "{\"home_name\":\"" + home_name +
                           "\",\"rooms\":[{\"id\":\"11111111-2222-4333-8444-555555555555\",\"name_kind\":\"custom\","
                           "\"name\":\"Office\",\"symbol\":\"desktopcomputer\"}],\"modules\":[";
    if (with_module_in_office) {
        settings += "{\"uuid\":\"744dbdfffe65d357\",\"name\":\"Gas\",\"room_id\":\"11111111-2222-4333-8444-555555555555\"}";
    }
    settings += "]}";
    return "{\"base_revision\":" + (base.empty() ? std::string("null") : "\"" + base + "\"") +
           ",\"settings\":" + settings + "}";
}

struct Fixture {
    std::string state_dir;
    std::string dir;
    std::shared_ptr<TlsContext> tls;
    std::shared_ptr<SettingsStore> store;
    std::shared_ptr<DeviceAuth> auth;
    ingest::Registry registry;
    std::unique_ptr<Server> server;

    bool Start(bool with_tls = true, size_t max_body = 256 * 1024)
    {
        state_dir = MakeTempDir();
        dir = state_dir + "/pogohome";
        std::string error;
        if (!EnsurePrivateDirectory(dir, error)) {
            return false;
        }
        if (with_tls && LoadOrCreateTlsIdentity(dir, "Jogakbo Hub test", tls, error) == TlsIdentityResult::Error) {
            return false;
        }
        store = std::make_shared<SettingsStore>(dir);
        store->Load();
        auth = std::make_shared<DeviceAuth>(dir);
        ServerConfig config;
        config.bind_address = "127.0.0.1";
        config.address_family = AF_INET;
        config.port = 0;
        config.hub_id = kHubId;
        config.sse_heartbeat_interval = std::chrono::milliseconds(300);
        config.sse_poll_interval = std::chrono::milliseconds(50);
        config.settings_requested = true;
        config.settings = store;
        config.device_auth = auth;
        config.tls = tls;
        config.max_request_body_bytes = max_body;
        server = std::make_unique<Server>(registry, config);
        return server->Start();
    }

    std::string Pair(const std::string &name = "iPad")
    {
        std::string code, error;
        int64_t expires = 0;
        auth->CreatePairingCode(NowMs(), kDefaultPairingTtlMs, code, expires, error);
        Response r = TlsRequest(server->bound_port(),
                                WithBody("POST", "/api/v1/pairing/claim", ClaimBody(code, tls->certificate_sha256(), name)));
        return r.status == 200 ? JsonField(r.body, "token") : std::string();
    }

    static int64_t NowMs()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    ~Fixture()
    {
        if (server) {
            server->Stop();
        }
        RemoveTree(state_dir);
    }
};

} // namespace

void RunSettingsApiTests(int *failures)
{
    printf("\n-- scenario: plain HTTP and TLS share the port; settings routes refuse plain HTTP --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts with settings sharing and a new TLS identity");
        uint16_t port = f.server->bound_port();

        Response plain_health = PlainRequest(port, Get("/api/v1/health"));
        API_CHECK(plain_health.status == 200, "plain GET /api/v1/health still works");
        API_CHECK(plain_health.body.find("\"hub_id\":\"3f0c9a4e8b1d4c2a9e7f6b5a4d3c2b1a\",\"settings_api\":1,"
                                         "\"settings_state\":\"uninitialized\",\"settings_revision\":null}") !=
                      std::string::npos,
                  "health adds settings_api/settings_state/settings_revision after every earlier field");

        Response plain_settings = PlainRequest(port, Get("/api/v1/settings", std::string(43, 'A')));
        API_CHECK(plain_settings.status == 403 && plain_settings.body.find("tls_required") != std::string::npos,
                  "GET /api/v1/settings over plain HTTP is refused with tls_required");
        Response plain_claim = PlainRequest(port, WithBody("POST", "/api/v1/pairing/claim", "{}"));
        API_CHECK(plain_claim.status == 403, "a pairing claim over plain HTTP is refused");

        Response tls_health = TlsRequest(port, Get("/api/v1/health"));
        API_CHECK(tls_health.status == 200, "GET /api/v1/health over TLS on the same port works");
        API_CHECK(tls_health.peer_fp == f.tls->certificate_sha256(),
                  "the certificate the client sees hashes to the hub's certificate_sha256");

        Response tls_snapshot = TlsRequest(port, Get("/api/v1/snapshot"));
        API_CHECK(tls_snapshot.status == 200 &&
                      tls_snapshot.body.find("\"settings_revision\":null,\"snapshot_stream\":\"") != std::string::npos,
                  "the snapshot also reports the settings revision (followed by where the snapshot stands)");
    }

    printf("\n-- scenario: pairing, then an authenticated first upload, update and conflict --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts");
        uint16_t port = f.server->bound_port();

        std::string code, error;
        int64_t expires = 0;
        f.auth->CreatePairingCode(Fixture::NowMs(), kDefaultPairingTtlMs, code, expires, error);

        Response wrong = TlsRequest(port, WithBody("POST", "/api/v1/pairing/claim",
                                                   ClaimBody("000000000000", f.tls->certificate_sha256(), "iPad")));
        API_CHECK(wrong.status == 403 && wrong.body.find("\"reason\":\"bad_proof\"") != std::string::npos,
                  "a wrong code is refused (403 pairing_rejected, bad_proof)");

        Response impostor = TlsRequest(port, WithBody("POST", "/api/v1/pairing/claim",
                                                      ClaimBody(code, Sha256Hex(std::string("other")), "iPad")));
        API_CHECK(impostor.status == 403, "the right code bound to a different certificate is refused");

        // The client binds the proof to the certificate it actually sees.
        std::string seen_fp = TlsRequest(port, Get("/api/v1/health")).peer_fp;
        Response claim = TlsRequest(port, WithBody("POST", "/api/v1/pairing/claim", ClaimBody(code, seen_fp, "iPad")));
        API_CHECK(claim.status == 200, "the right code bound to the certificate the client saw pairs the device");
        std::string token = JsonField(claim.body, "token");
        std::string device_id = JsonField(claim.body, "device_id");
        API_CHECK(JsonField(claim.body, "server_proof") ==
                      PairingServerProof(code, kHubId, seen_fp, kNonce, device_id, token),
                  "the hub's server_proof verifies against the code and the certificate the client saw");

        Response no_auth = TlsRequest(port, Get("/api/v1/settings"));
        API_CHECK(no_auth.status == 401 && no_auth.headers.find("WWW-Authenticate: Bearer") != std::string::npos,
                  "GET /api/v1/settings without a token is 401");
        Response bad_auth = TlsRequest(port, Get("/api/v1/settings", std::string(43, 'B')));
        API_CHECK(bad_auth.status == 401, "an unknown token is 401");

        Response empty = TlsRequest(port, Get("/api/v1/settings", token));
        API_CHECK(empty.status == 200 && empty.body.find("\"initialized\":false") != std::string::npos,
                  "before the first upload the hub reports initialized:false");

        Response created = TlsRequest(port, WithBody("PUT", "/api/v1/settings", SettingsBody("", "Home"), token));
        API_CHECK(created.status == 200 && created.body.find("\"initialized\":true") != std::string::npos,
                  "the first upload (base_revision null) is saved");
        std::string rev1 = JsonField(created.body, "revision");

        Response second_first = TlsRequest(port, WithBody("PUT", "/api/v1/settings", SettingsBody("", "Other"), token));
        API_CHECK(second_first.status == 409 && second_first.body.find("\"home_name\":\"Home\"") != std::string::npos,
                  "another first upload is a conflict carrying the settings already on the hub");

        Response health = PlainRequest(port, Get("/api/v1/health"));
        API_CHECK(health.body.find("\"settings_state\":\"ready\",\"settings_revision\":\"" + rev1 + "\"") !=
                      std::string::npos,
                  "health reports the new revision");

        Response updated = TlsRequest(port, WithBody("PUT", "/api/v1/settings", SettingsBody(rev1, "Home 2"), token));
        std::string rev2 = JsonField(updated.body, "revision");
        API_CHECK(updated.status == 200 && !rev2.empty() && rev2 != rev1, "an update based on the current revision is saved");

        Response stale = TlsRequest(port, WithBody("PUT", "/api/v1/settings", SettingsBody(rev1, "Stale", false), token));
        API_CHECK(stale.status == 409 && stale.body.find("\"error\":\"revision_conflict\"") != std::string::npos,
                  "an update based on an older revision is 409 revision_conflict");
        API_CHECK(stale.body.find("\"revision\":\"" + rev2 + "\"") != std::string::npos &&
                      stale.body.find("\"home_name\":\"Home 2\"") != std::string::npos,
                  "the conflict carries the current revision and settings");
        Response after = TlsRequest(port, Get("/api/v1/settings", token));
        API_CHECK(after.body.find("\"name\":\"Gas\"") != std::string::npos &&
                      after.body.find("\"revision\":\"" + rev2 + "\"") != std::string::npos,
                  "the stale write changed nothing (the module placement is still there)");
    }

    printf("\n-- scenario: invalid, malformed and oversized settings requests --\n");
    {
        Fixture f;
        API_CHECK(f.Start(true, 2048), "the server starts with a 2 KiB body limit");
        uint16_t port = f.server->bound_port();
        std::string token = f.Pair();
        API_CHECK(!token.empty(), "a device pairs");

        std::string bad_room = "{\"base_revision\":null,\"settings\":{\"home_name\":null,\"rooms\":[],\"modules\":"
                               "[{\"uuid\":\"744dbdfffe65d357\",\"name\":null,\"room_id\":"
                               "\"11111111-2222-4333-8444-555555555555\"}]}}";
        Response invalid = TlsRequest(port, WithBody("PUT", "/api/v1/settings", bad_room, token));
        API_CHECK(invalid.status == 422 && invalid.body.find("\"detail\":\"unknown_room_id\"") != std::string::npos,
                  "a module in a missing room is 422 invalid_settings/unknown_room_id");

        Response malformed = TlsRequest(port, WithBody("PUT", "/api/v1/settings", "{\"base_revision\":", token));
        API_CHECK(malformed.status == 400, "malformed JSON is 400");

        Response missing_settings = TlsRequest(port, WithBody("PUT", "/api/v1/settings", "{\"base_revision\":null}", token));
        API_CHECK(missing_settings.status == 400, "a body without settings is 400");

        Response too_large = TlsRequest(port, WithBody("PUT", "/api/v1/settings", std::string(4096, ' '), token));
        API_CHECK(too_large.status == 413, "a body over the limit is 413 (and not read)");

        std::string no_length = "PUT /api/v1/settings HTTP/1.1\r\nHost: hub\r\nAuthorization: Bearer " + token +
                                "\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
        Response chunked = TlsRequest(port, no_length);
        API_CHECK(chunked.status == 411, "a chunked body is 411 Length Required");

        Response wrong_method = TlsRequest(port, WithBody("POST", "/api/v1/settings", "{}", token));
        API_CHECK(wrong_method.status == 405, "POST /api/v1/settings is 405");

        API_CHECK(f.store->Get().state == SettingsState::Uninitialized, "none of these created settings");
    }

    printf("\n-- scenario: an open SSE stream announces a saved settings change --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts");
        uint16_t port = f.server->bound_port();
        std::string token = f.Pair();

        int fd = Connect(port);
        std::string req = Get("/api/v1/events");
        send(fd, req.data(), req.size(), 0);
        std::string stream;
        char buf[4096];
        while (stream.find("\n\n", stream.find("event: snapshot")) == std::string::npos) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            stream.append(buf, static_cast<size_t>(n));
        }
        API_CHECK(stream.find("\"settings_revision\":null") != std::string::npos,
                  "the first SSE snapshot has no settings revision yet");

        Response created = TlsRequest(port, WithBody("PUT", "/api/v1/settings", SettingsBody("", "Home"), token));
        std::string rev = JsonField(created.body, "revision");
        bool announced = false;
        for (int i = 0; i < 40 && !announced; i++) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            stream.append(buf, static_cast<size_t>(n));
            announced = stream.find("\"settings_revision\":\"" + rev + "\"") != std::string::npos;
        }
        API_CHECK(announced, "a new SSE snapshot carries the new settings_revision (registry unchanged)");
        close(fd);
    }

    printf("\n-- scenario: revocation, self-removal, and a hub restart --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts");
        uint16_t port = f.server->bound_port();
        std::string phone = f.Pair("iPhone");
        std::string mac = f.Pair("Mac");
        API_CHECK(!phone.empty() && !mac.empty() && phone != mac, "two devices pair with their own tokens");

        Response created = TlsRequest(port, WithBody("PUT", "/api/v1/settings", SettingsBody("", "Home"), phone));
        std::string rev = JsonField(created.body, "revision");

        std::vector<DeviceRecord> devices;
        std::string error;
        f.auth->ListDevices(devices, error);
        bool found = false;
        for (const auto &d : devices) {
            if (d.name == "iPhone") {
                f.auth->RevokeDevice(d.id, found, error);
            }
        }
        API_CHECK(found, "the operator revokes the iPhone (pogohome-hubctl revoke)");
        API_CHECK(TlsRequest(port, Get("/api/v1/settings", phone)).status == 401,
                  "the revoked iPhone's next request is 401, without a restart");
        API_CHECK(TlsRequest(port, Get("/api/v1/settings", mac)).status == 200, "the Mac is unaffected");

        Response removed = TlsRequest(port, WithBody("DELETE", "/api/v1/devices/self", "", mac));
        API_CHECK(removed.status == 200, "DELETE /api/v1/devices/self removes the calling device");
        API_CHECK(TlsRequest(port, Get("/api/v1/settings", mac)).status == 401, "its token no longer works");

        // Restart: a new server and store over the same state directory.
        std::string fp_before = f.tls->certificate_sha256();
        f.server->Stop();
        std::shared_ptr<TlsContext> tls2;
        API_CHECK(LoadOrCreateTlsIdentity(f.dir, "Jogakbo Hub test", tls2, error) == TlsIdentityResult::Loaded &&
                      tls2->certificate_sha256() == fp_before,
                  "after a restart the TLS identity is loaded, not replaced (pins stay valid)");
        SettingsStore reopened(f.dir);
        reopened.Load();
        API_CHECK(reopened.Get().revision == rev && reopened.Get().settings.home_name == "Home",
                  "after a restart the settings and revision are the same");
    }

    printf("\n-- scenario: failure states answer 503 and leave the sensor API alone --\n");
    {
        Fixture corrupt;
        API_CHECK(corrupt.Start(), "the server starts");
        std::string token = corrupt.Pair();
        WriteFile(corrupt.dir + "/settings.json", "{damaged");
        corrupt.store->Load();
        Response r = TlsRequest(corrupt.server->bound_port(), Get("/api/v1/settings", token));
        API_CHECK(r.status == 503 && r.body.find("\"reason\":\"corrupt\"") != std::string::npos &&
                      r.body.find("settings.json") == std::string::npos,
                  "a damaged settings file is 503 settings_unavailable/corrupt (no file path is sent)");
        Response health = PlainRequest(corrupt.server->bound_port(), Get("/api/v1/health"));
        API_CHECK(health.status == 200 && health.body.find("\"settings_state\":\"unavailable\"") != std::string::npos,
                  "health says the settings are unavailable while the sensor API keeps working");

        Fixture no_tls;
        API_CHECK(no_tls.Start(false), "a server whose TLS identity could not be loaded still starts");
        Response settings = PlainRequest(no_tls.server->bound_port(), Get("/api/v1/settings"));
        API_CHECK(settings.status == 403, "settings routes still refuse plain HTTP");
        Response tls_attempt = TlsRequest(no_tls.server->bound_port(), Get("/api/v1/health"));
        API_CHECK(!tls_attempt.ok, "a TLS connection is simply closed");
        Response health2 = PlainRequest(no_tls.server->bound_port(), Get("/api/v1/health"));
        API_CHECK(health2.status == 200 && health2.body.find("\"settings_state\":\"unavailable\"") != std::string::npos,
                  "health reports settings as unavailable");
    }

    printf("\n-- scenario: without settings sharing the API is exactly the read-only one --\n");
    {
        ingest::Registry registry;
        ServerConfig config;
        config.bind_address = "127.0.0.1";
        config.address_family = AF_INET;
        config.hub_id = kHubId;
        Server server(registry, config);
        API_CHECK(server.Start(), "a server without settings starts");
        Response health = PlainRequest(server.bound_port(), Get("/api/v1/health"));
        API_CHECK(health.body == BuildHealthJson(0, 0, 0, kHubId), "health is byte for byte the previous shape");
        API_CHECK(PlainRequest(server.bound_port(), Get("/api/v1/settings")).status == 404,
                  "/api/v1/settings does not exist (404)");
        API_CHECK(!TlsRequest(server.bound_port(), Get("/api/v1/health")).ok, "TLS is not offered");
        server.Stop();
    }
}
