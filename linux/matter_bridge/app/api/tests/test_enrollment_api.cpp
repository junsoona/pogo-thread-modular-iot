// Power Base enrollment through the REAL pogohome_api::Server on loopback,
// with a fake Commissioner: the routes exist only when configured, never
// over plain HTTP or without a paired device's token, one device cannot see
// or cancel another's session, bad input gets a clear error that never
// echoes the PSKd, and success is only a new heartbeat from that exact
// EUI-64 in the Registry.
#include <chrono>
#include <string>
#include <thread>

#include "ingest/registry.hpp"
#include "pogohome_api/atomic_file.hpp"
#include "pogohome_api/device_auth.hpp"
#include "pogohome_api/enrollment.hpp"
#include "pogohome_api/http_server.hpp"
#include "pogohome_api/settings_store.hpp"
#include "pogohome_api/tls_identity.hpp"
#include "test_check.hpp"
#include "test_fake_commissioner.hpp"
#include "test_http_client.hpp"
#include "test_temp_dir.hpp"

using namespace pogohome_api;
using test_support::FakeCommissioner;
using test_support::HttpRequestText;
using test_support::HttpResponse;
using test_support::JsonStringField;
using test_support::PlainHttpRequest;
using test_support::TlsHttpRequest;

namespace {

const char kHubId[] = "3f0c9a4e8b1d4c2a9e7f6b5a4d3c2b1a";
const char kNonce[] = "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf";
const char kEui[] = "744dbdfffe65d357";
const char kOtherEui[] = "744dbdfffe65d358";
const char kPskd[] = "J01NME7K2"; // a made-up test value
const char kPath[] = "/api/v1/power-base-enrollments";

ingest::Uuid UuidOf(const std::string &eui)
{
    ingest::Uuid u{};
    std::string normalized;
    NormalizeEui64(eui, normalized, &u);
    return u;
}

std::string StartBody(const std::string &eui, const std::string &pskd)
{
    return "{\"eui64\":\"" + eui + "\",\"pskd\":\"" + pskd + "\"}";
}

struct Fixture {
    std::string state_dir;
    std::shared_ptr<TlsContext> tls;
    std::shared_ptr<DeviceAuth> auth;
    std::shared_ptr<FakeCommissioner> fake = std::make_shared<FakeCommissioner>();
    ingest::Registry registry;
    std::shared_ptr<EnrollmentManager> enrollment;
    std::unique_ptr<Server> server;

    // `requested`: enrollment configured; `with_manager`: it could start.
    bool Start(bool requested = true, bool with_manager = true)
    {
        state_dir = test_support::MakeTempDir();
        const std::string dir = state_dir + "/pogohome";
        std::string error;
        if (!EnsurePrivateDirectory(dir, error) ||
            LoadOrCreateTlsIdentity(dir, "Jogakbo Hub test", tls, error) == TlsIdentityResult::Error) {
            return false;
        }
        auto store = std::make_shared<SettingsStore>(dir);
        store->Load();
        auth = std::make_shared<DeviceAuth>(dir);
        EnrollmentConfig enrollment_config;
        enrollment_config.poll_interval = std::chrono::milliseconds(10);
        enrollment = std::make_shared<EnrollmentManager>(registry, fake, enrollment_config);

        ServerConfig config;
        config.bind_address = "127.0.0.1";
        config.address_family = AF_INET;
        config.port = 0;
        config.hub_id = kHubId;
        config.settings_requested = true;
        config.settings = store;
        config.device_auth = auth;
        config.tls = tls;
        config.enrollment_requested = requested;
        if (with_manager) {
            config.enrollment = enrollment;
        }
        server = std::make_unique<Server>(registry, config);
        return server->Start();
    }

    uint16_t port() const { return server->bound_port(); }

    std::string Pair(const std::string &name)
    {
        std::string code, error;
        int64_t expires = 0;
        auth->CreatePairingCode(NowMs(), kDefaultPairingTtlMs, code, expires, error);
        const std::string fp = tls->certificate_sha256();
        std::string body = "{\"device_name\":\"" + name + "\",\"client_nonce\":\"" + kNonce + "\",\"proof\":\"" +
                           PairingClientProof(code, kHubId, fp, kNonce, name) + "\"}";
        HttpResponse r = TlsHttpRequest(port(), HttpRequestText("POST", "/api/v1/pairing/claim", "", &body));
        return r.status == 200 ? JsonStringField(r.body, "token") : std::string();
    }

    HttpResponse Post(const std::string &token, const std::string &body)
    {
        return TlsHttpRequest(port(), HttpRequestText("POST", kPath, token, &body));
    }
    HttpResponse GetSession(const std::string &token, const std::string &id)
    {
        return TlsHttpRequest(port(), HttpRequestText("GET", std::string(kPath) + "/" + id, token));
    }
    HttpResponse Delete(const std::string &token, const std::string &id)
    {
        return TlsHttpRequest(port(), HttpRequestText("DELETE", std::string(kPath) + "/" + id, token));
    }

    std::string StateOf(const std::string &token, const std::string &id)
    {
        return JsonStringField(GetSession(token, id).body, "state");
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
        if (enrollment) {
            enrollment->Stop();
        }
        test_support::RemoveTree(state_dir);
    }
};

template <typename Pred> bool WaitFor(Pred pred)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

} // namespace

void RunEnrollmentApiTests(int *failures)
{
    printf("\n-- scenario: without --api-enroll-helper the routes do not exist --\n");
    {
        Fixture f;
        API_CHECK(f.Start(false), "the server starts without enrollment");
        std::string token = f.Pair("iPad");
        HttpResponse r = f.Post(token, StartBody(kEui, kPskd));
        API_CHECK(r.status == 404 && r.body == "{\"error\":\"enrollment_disabled\"}",
                  "POST over TLS with a valid token is 404 enrollment_disabled");
        r = PlainHttpRequest(f.port(), HttpRequestText("GET", std::string(kPath) + "/" + std::string(32, 'a')));
        API_CHECK(r.status == 404 && r.body.find("enrollment_disabled") != std::string::npos,
                  "so is a plain-HTTP GET of a session");
        HttpResponse health = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/health"));
        API_CHECK(health.status == 200 && health.body.find("power_base_enrollment") == std::string::npos,
                  "health does not mention enrollment");
        API_CHECK(f.fake->ensure_calls.load() == 0, "the OTBR was never touched");
    }

    printf("\n-- scenario: requested but not able to start --\n");
    {
        Fixture f;
        API_CHECK(f.Start(true, false), "the server starts");
        std::string token = f.Pair("iPad");
        HttpResponse r = f.Post(token, StartBody(kEui, kPskd));
        API_CHECK(r.status == 503 && r.body.find("enrollment_unavailable") != std::string::npos,
                  "503 enrollment_unavailable");
        HttpResponse health = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/health"));
        API_CHECK(health.body.find("\"power_base_enrollment\":\"unavailable\"}") != std::string::npos,
                  "health says unavailable");
    }

    printf("\n-- scenario: TLS and a paired device's token are required --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts with enrollment");
        std::string token = f.Pair("iPad");
        API_CHECK(!token.empty(), "a device pairs");
        const std::string body = StartBody(kEui, kPskd);

        HttpResponse health = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/health"));
        API_CHECK(health.body.find("\"settings_revision\":null,\"power_base_enrollment\":\"available\"}") !=
                      std::string::npos,
                  "health ends with power_base_enrollment after the settings fields");

        HttpResponse r = PlainHttpRequest(f.port(), HttpRequestText("POST", kPath, token, &body));
        API_CHECK(r.status == 403 && r.body == "{\"error\":\"tls_required\"}",
                  "plain HTTP with a valid token is 403 tls_required");
        r = PlainHttpRequest(f.port(), HttpRequestText("DELETE", std::string(kPath) + "/" + std::string(32, 'a'), token));
        API_CHECK(r.status == 403, "plain-HTTP DELETE is 403 too");
        r = f.Post("", body);
        API_CHECK(r.status == 401 && r.headers.find("WWW-Authenticate: Bearer") != std::string::npos,
                  "no token is 401");
        r = f.Post(std::string(43, 'B'), body);
        API_CHECK(r.status == 401, "an unknown token is 401");
        API_CHECK(f.fake->ensure_calls.load() == 0, "none of that reached the OTBR");

        r = TlsHttpRequest(f.port(), HttpRequestText("GET", kPath, token));
        API_CHECK(r.status == 405, "GET on the collection is 405");
        r = TlsHttpRequest(f.port(), HttpRequestText("PUT", kPath, token, &body));
        API_CHECK(r.status == 405, "PUT on the collection is 405");
        r = TlsHttpRequest(f.port(), HttpRequestText("POST", std::string(kPath) + "/" + std::string(32, 'a'), token, &body));
        API_CHECK(r.status == 405, "POST on a session is 405");
    }

    printf("\n-- scenario: request validation --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts");
        std::string token = f.Pair("iPad");
        struct Case {
            std::string body;
            int status;
            const char *error;
            const char *what;
        };
        const Case cases[] = {
            {StartBody("*", kPskd), 422, "invalid_eui64", "the wildcard Joiner"},
            {StartBody("744dbdfffe65d35", kPskd), 422, "invalid_eui64", "a 15-digit EUI-64"},
            {StartBody("744dbdfffe65d35g", kPskd), 422, "invalid_eui64", "a non-hex EUI-64"},
            {StartBody("0000000000000000", kPskd), 422, "invalid_eui64", "an all-zero EUI-64"},
            {StartBody(kEui, "J01NM"), 422, "invalid_pskd", "a 5-character PSKd"},
            {StartBody(kEui, std::string(33, 'A')), 422, "invalid_pskd", "a 33-character PSKd"},
            {StartBody(kEui, "j01nme7k2"), 422, "invalid_pskd", "a lowercase PSKd"},
            {StartBody(kEui, "J01NMEI"), 422, "invalid_pskd", "a PSKd with I"},
            {"{\"eui64\":\"744dbdfffe65d357\"}", 400, "invalid_request", "a missing PSKd"},
            {"{\"eui64\":\"744dbdfffe65d357\",\"pskd\":\"J01NME7K2\",\"x\":1}", 400, "invalid_request",
             "an unknown field"},
            {"{\"eui64\":7,\"pskd\":\"J01NME7K2\"}", 400, "invalid_request", "a non-string EUI-64"},
            {"not json", 400, "invalid_request", "a body that is not JSON"},
            {"{\"eui64\":\"744dbdfffe65d357\",\"pskd\":\"" + std::string(300, 'A') + "\"}", 413, "request_too_large",
             "a body over 256 bytes"},
        };
        bool never_echoed = true;
        for (const Case &c : cases) {
            HttpResponse r = f.Post(token, c.body);
            API_CHECK(r.status == c.status && r.body == std::string("{\"error\":\"") + c.error + "\"}",
                      (std::string(c.what) + " -> " + std::to_string(c.status) + " " + c.error).c_str());
            API_CHECK(r.headers.find("Cache-Control: no-store") != std::string::npos,
                      (std::string(c.what) + ": Cache-Control: no-store").c_str());
            never_echoed = never_echoed && r.body.find("J01NM") == std::string::npos;
        }
        API_CHECK(never_echoed, "no error answer echoes a PSKd");
        std::string no_length = "POST " + std::string(kPath) + " HTTP/1.1\r\nHost: hub\r\nAuthorization: Bearer " +
                                token + "\r\n\r\n";
        API_CHECK(TlsHttpRequest(f.port(), no_length).status == 411, "a body without Content-Length is 411");
        API_CHECK(f.fake->ensure_calls.load() == 0, "no invalid request reached the OTBR");
    }

    printf("\n-- scenario: a full session over the API --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts");
        std::string a = f.Pair("iPad");
        std::string b = f.Pair("Mac");
        API_CHECK(!a.empty() && !b.empty() && a != b, "two devices pair");

        HttpResponse r = f.Post(a, StartBody("744DBDFFFE65D357", kPskd));
        API_CHECK(r.status == 202, "POST starts a session (202)");
        const std::string id = JsonStringField(r.body, "session_id");
        API_CHECK(id.size() == 32 && r.headers.find("Location: /api/v1/power-base-enrollments/" + id) != std::string::npos,
                  "with its id and Location");
        API_CHECK(r.headers.find("Cache-Control: no-store") != std::string::npos, "and Cache-Control: no-store");
        API_CHECK(JsonStringField(r.body, "eui64") == kEui && r.body.find(kPskd) == std::string::npos,
                  "the EUI-64 is normalized and the PSKd is not in the answer");
        API_CHECK(r.body.find("\"window_seconds\":120") != std::string::npos &&
                      r.body.find("\"heartbeat_received_at_unix_ms\":null") != std::string::npos,
                  "a 120 s window, no heartbeat yet");

        API_CHECK(WaitFor([&] { return f.StateOf(a, id) == "waiting_for_heartbeat"; }),
                  "the starting device sees waiting_for_heartbeat");
        HttpResponse other = f.GetSession(b, id);
        API_CHECK(other.status == 404 && other.body == "{\"error\":\"session_not_found\"}",
                  "another device's GET is 404 session_not_found");
        other = f.Delete(b, id);
        API_CHECK(other.status == 404, "another device's DELETE is 404");
        API_CHECK(f.StateOf(a, id) == "waiting_for_heartbeat", "and did not cancel it");
        API_CHECK(f.GetSession(a, "not-a-session-id").status == 404, "a malformed id is 404");

        HttpResponse dup = f.Post(a, StartBody(kOtherEui, kPskd));
        API_CHECK(dup.status == 409 && JsonStringField(dup.body, "error") == "enrollment_in_progress" &&
                      dup.body.find(id) != std::string::npos,
                  "a second POST by the same device is 409 with its running session");
        dup = f.Post(b, StartBody(kOtherEui, kPskd));
        API_CHECK(dup.status == 409 && dup.body == "{\"error\":\"enrollment_in_progress\"}",
                  "another device's POST is 409 without the session");

        f.registry.apply_heartbeat(UuidOf(kOtherEui), 1000, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        API_CHECK(f.StateOf(a, id) == "waiting_for_heartbeat", "another Base's heartbeat changes nothing");

        f.registry.apply_heartbeat(UuidOf(kEui), 1000, 0);
        API_CHECK(WaitFor([&] {
                      HttpResponse g = f.GetSession(a, id);
                      return g.body.find("\"state\":\"heartbeat_received\",\"finished\":true") != std::string::npos;
                  }),
                  "that Base's new heartbeat: heartbeat_received, finished");
        HttpResponse done = f.GetSession(a, id);
        API_CHECK(JsonStringField(done.body, "joiner_entry") == "removed" &&
                      done.body.find("\"heartbeat_received_at_unix_ms\":null") == std::string::npos,
                  "the entry is removed and the heartbeat time is set");
        HttpResponse late = f.Delete(a, id);
        API_CHECK(late.status == 409 && JsonStringField(late.body, "error") == "session_already_final",
                  "DELETE after the end is 409 session_already_final");

        HttpResponse again = f.Post(a, StartBody(kEui, kPskd));
        API_CHECK(again.status == 409 && again.body == "{\"error\":\"power_base_already_connected\"}",
                  "the now-online Base cannot be enrolled again (409 power_base_already_connected)");
    }

    printf("\n-- scenario: cancel over the API --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts");
        std::string a = f.Pair("iPad");
        const std::string id = JsonStringField(f.Post(a, StartBody(kEui, kPskd)).body, "session_id");
        WaitFor([&] { return f.StateOf(a, id) == "waiting_for_heartbeat"; });
        HttpResponse c = f.Delete(a, id);
        API_CHECK(c.status == 200 && JsonStringField(c.body, "state") == "cancelled", "DELETE cancels (200)");
        API_CHECK(WaitFor([&] { return JsonStringField(f.GetSession(a, id).body, "joiner_entry") == "removed"; }),
                  "the Joiner entry is then removed");
        API_CHECK(f.fake->remove_calls.load() == 1 && f.fake->removed[0] == kEui, "exactly that EUI-64's entry");
    }

    printf("\n-- scenario: a slow OTBR helper never blocks the API --\n");
    {
        Fixture f;
        f.fake->add_delay = std::chrono::milliseconds(1500);
        API_CHECK(f.Start(), "the server starts");
        std::string a = f.Pair("iPad");
        const std::string id = JsonStringField(f.Post(a, StartBody(kEui, kPskd)).body, "session_id");
        WaitFor([&] { return f.fake->add_calls.load() == 1; });
        auto t0 = std::chrono::steady_clock::now();
        HttpResponse health = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/health"));
        HttpResponse snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        HttpResponse session = f.GetSession(a, id);
        auto took = std::chrono::steady_clock::now() - t0;
        API_CHECK(health.status == 200 && snapshot.status == 200 && session.status == 200,
                  "health, snapshot and the session answer while the Joiner add runs");
        API_CHECK(took < std::chrono::milliseconds(800), "promptly (the helper call holds no API or Registry lock)");
        API_CHECK(JsonStringField(session.body, "joiner_entry") == "adding", "the session shows the add in progress");
        t0 = std::chrono::steady_clock::now();
        f.registry.apply_heartbeat(UuidOf(kOtherEui), 1000, 0);
        API_CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(100),
                  "and the Registry keeps taking heartbeats meanwhile");
    }
}
