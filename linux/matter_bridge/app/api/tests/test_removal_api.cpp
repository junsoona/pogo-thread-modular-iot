// Power Base removal (unenroll) through the REAL pogohome_api::Server on
// loopback (docs/power-base-management.md, section 7): the routes exist only
// when configured, never over plain HTTP or without a paired device's token;
// the management key leaves the hub once, in the TLS answer that starts a
// session; the same target is the same job for every device; a job is
// completed by the board's authenticated proof and by nothing else. The
// board's side of the wire is made here with the real pb_mgmt_proto.c.
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include "ingest/minicoap.hpp"
#include "ingest/registry.hpp"
#include "pogohome_api/atomic_file.hpp"
#include "pogohome_api/crypto_util.hpp"
#include "pogohome_api/device_auth.hpp"
#include "pogohome_api/enrollment.hpp"
#include "pogohome_api/http_server.hpp"
#include "pogohome_api/power_base_management.hpp"
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
const char kRemovals[] = "/api/v1/power-base-removals";
const char kEnrollments[] = "/api/v1/power-base-enrollments";

bool Hmac(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[32], void *)
{
    unsigned int n = 0;
    return HMAC(EVP_sha256(), key, static_cast<int>(key_len), data, len, out, &n) != nullptr && n == 32;
}

template <size_t N> std::array<uint8_t, N> Bytes(const std::string &hex)
{
    std::array<uint8_t, N> out{};
    for (size_t i = 0; i < N && 2 * i + 1 < hex.size(); i++) {
        out[i] = static_cast<uint8_t>(std::stoi(hex.substr(2 * i, 2), nullptr, 16));
    }
    return out;
}

ingest::Uuid UuidOf(const std::string &eui)
{
    return Bytes<8>(eui);
}

// What a client older than removal request version 2 sends: the Base alone.
std::string LegacyRemovalBody(const std::string &uuid)
{
    return "{\"power_base_uuid\":\"" + uuid + "\"}";
}

// The request as an app makes it: the Base, the registration it was shown,
// that registration's removal serial, and an id of this confirmation.
std::string RemovalBody(const std::string &uuid, const std::string &record, uint32_t serial, const std::string &request_id)
{
    return "{\"power_base_uuid\":\"" + uuid + "\",\"record\":\"" + record + "\",\"removal_serial\":" + std::to_string(serial) +
           ",\"request_id\":\"" + request_id + "\"}";
}

// A request id nobody used before: one per confirmation.
std::string NextRemovalRequestId()
{
    static unsigned counter = 0;
    char id[33];
    std::snprintf(id, sizeof(id), "52454d4f%024x", ++counter);
    return id;
}

std::string ManagedStartBody(const std::string &eui)
{
    return "{\"eui64\":\"" + eui + "\",\"pskd\":\"" + kPskd + "\",\"bridge_endpoint_version\":1,\"management_version\":1}";
}

// The board's credential, as the app would hold it for a moment.
struct Credential {
    std::string eui, hub_id, generation, key;
};

std::vector<uint8_t> Pack(pb_mgmt_msg_t &m, const Credential &c)
{
    auto eui = Bytes<8>(c.eui);
    auto hub_id = Bytes<16>(c.hub_id);
    auto generation = Bytes<16>(c.generation);
    auto key = Bytes<32>(c.key);
    std::memcpy(m.id.eui64, eui.data(), 8);
    std::memcpy(m.id.hub_id, hub_id.data(), 16);
    std::memcpy(m.id.generation, generation.data(), 16);
    std::vector<uint8_t> wire(PB_MGMT_MAX_MESSAGE_LEN);
    wire.resize(pb_mgmt_pack(&m, key.data(), Hmac, nullptr, wire.data(), wire.size()));
    return wire;
}

std::vector<uint8_t> Poll(const Credential &c, const uint8_t *echo = nullptr)
{
    pb_mgmt_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.type = PB_MGMT_TYPE_POLL;
    RAND_bytes(m.board_nonce, 16);
    if (echo != nullptr) {
        std::memcpy(m.hub_nonce, echo, 16);
    }
    m.state = PB_MGMT_STATE_ENROLLED;
    return Pack(m, c);
}

struct Fixture {
    std::string state_dir;
    std::shared_ptr<TlsContext> tls;
    std::shared_ptr<DeviceAuth> auth;
    std::shared_ptr<SettingsStore> store;
    std::shared_ptr<FakeCommissioner> fake = std::make_shared<FakeCommissioner>();
    ingest::Registry registry;
    std::shared_ptr<PowerBaseManagement> management;
    std::shared_ptr<EnrollmentManager> enrollment;
    std::unique_ptr<Server> server;
    // Added to the moment "is this Base heard from?" is asked for: ten
    // minutes on, nothing this test sent counts as heard any more.
    std::chrono::seconds steady_shift{0};
    // Added to the wall clock management reads: the test's way of letting
    // sixteen minutes pass without waiting for them.
    std::atomic<int64_t> clock_shift_ms{0};
    // What runs right after GET /api/v1/snapshot or the SSE loop copied the
    // Registry and before the snapshot is built from that copy
    // (ServerConfig::after_registry_copy): the place where another request
    // can change what the hub lists. Set and read under `seam_mutex` -- the
    // server's threads read it.
    std::mutex seam_mutex;
    std::function<void()> after_copy;
    void SetAfterCopy(std::function<void()> fn)
    {
        std::lock_guard<std::mutex> lock(seam_mutex);
        after_copy = std::move(fn);
    }

    // `requested`: management configured; `damaged`: its state file is.
    bool Start(bool requested = true, bool damaged = false)
    {
        state_dir = test_support::MakeTempDir();
        const std::string dir = state_dir + "/pogohome";
        std::string error;
        if (!EnsurePrivateDirectory(dir, error) ||
            LoadOrCreateTlsIdentity(dir, "Jogakbo Hub test", tls, error) == TlsIdentityResult::Error) {
            return false;
        }
        store = std::make_shared<SettingsStore>(dir);
        store->Load();
        auth = std::make_shared<DeviceAuth>(dir);
        if (requested) {
            if (damaged) {
                test_support::WriteFile(dir + "/" + kPowerBaseMgmtFileName, "not json");
            }
            PowerBaseManagementConfig management_config;
            management_config.directory = dir;
            management_config.hub_id = kHubId;
            management_config.enrollment_in_progress = [this](const std::string &eui64) {
                return enrollment && enrollment->InProgressFor(eui64);
            };
            management_config.steady_now = [this]() { return std::chrono::steady_clock::now() + steady_shift; };
            management_config.now_unix_ms = [this]() { return NowMs() + clock_shift_ms.load(); };
            management = std::make_shared<PowerBaseManagement>(registry, std::move(management_config));
            management->Load(error);
        }
        EnrollmentConfig enrollment_config;
        enrollment_config.poll_interval = std::chrono::milliseconds(10);
        enrollment_config.bridge_port = 5701;
        enrollment_config.management = management;
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
        config.enrollment_requested = true;
        config.enrollment = enrollment;
        config.management_requested = requested;
        config.management = management;
        config.after_registry_copy = [this]() {
            std::function<void()> run;
            {
                std::lock_guard<std::mutex> lock(seam_mutex);
                run = after_copy;
            }
            if (run) {
                run();
            }
        };
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

    HttpResponse Request(const std::string &method, const std::string &path, const std::string &token,
                         const std::string *body = nullptr)
    {
        return TlsHttpRequest(port(), HttpRequestText(method, path, token, body));
    }
    // What the hub lists for `uuid` right now, as a device reads it before it
    // confirms: the registration and its removal serial. For a Base this hub
    // cannot remove, the registration of a removal that is under way (the
    // device still has it on its screen), or one that is none.
    void RemovalBindingNow(const std::string &uuid, std::string &record, uint32_t &serial)
    {
        std::string eui = uuid;
        for (char &c : eui) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        serial = 0;
        if (management && management->RemovalBinding(eui, record, serial)) {
            return;
        }
        record = std::string(32, 'e');
        if (management) {
            for (const RemovalJob &job : management->ListRemovals()) {
                if (job.power_base_uuid == eui && !IsFinalRemovalState(job.state)) {
                    record = job.generation;
                }
            }
        }
    }
    // A device that reads the list now and confirms the removal of what it is
    // shown, with a request id of its own (`request_id` gets it).
    HttpResponse PostRemoval(const std::string &token, const std::string &uuid, std::string *request_id = nullptr)
    {
        std::string record;
        uint32_t serial = 0;
        RemovalBindingNow(uuid, record, serial);
        const std::string id = NextRemovalRequestId();
        if (request_id != nullptr) {
            *request_id = id;
        }
        const std::string body = RemovalBody(uuid, record, serial, id);
        return Request("POST", kRemovals, token, &body);
    }
    // Exactly this request (again, late, or from another time).
    HttpResponse PostRemovalRequest(const std::string &token, const std::string &uuid, const std::string &record,
                                    uint32_t serial, const std::string &request_id)
    {
        const std::string body = RemovalBody(uuid, record, serial, request_id);
        return Request("POST", kRemovals, token, &body);
    }
    std::string RemovalState(const std::string &token, const std::string &id)
    {
        return JsonStringField(Request("GET", std::string(kRemovals) + "/" + id, token).body, "state");
    }

    // Enrolls `eui` through the API with a management credential and makes
    // its generation active with the board's first authenticated poll.
    bool EnrollManaged(const std::string &token, const std::string &eui, Credential &out)
    {
        const std::string body = ManagedStartBody(eui);
        HttpResponse r = Request("POST", kEnrollments, token, &body);
        if (r.status != 202) {
            return false;
        }
        out.eui = eui;
        out.hub_id = JsonStringField(r.body, "hub_id");
        out.generation = JsonStringField(r.body, "generation");
        out.key = JsonStringField(r.body, "key");
        const std::string session = JsonStringField(r.body, "session_id");
        if (management->HandleDatagram(Poll(out)).code != ingest::kCoapCodeContent) {
            return false;
        }
        registry.apply_heartbeat(UuidOf(eui), 1, 0);
        // The session ends by itself on that heartbeat.
        for (int i = 0; i < 150 && enrollment->InProgressFor(eui); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return !enrollment->InProgressFor(eui);
    }

    // The board's release receipt for `command`: UNENROLL_DONE / ERASED.
    std::vector<uint8_t> Receipt(const Credential &c, const std::array<uint8_t, 16> &command)
    {
        pb_mgmt_msg_t done;
        std::memset(&done, 0, sizeof(done));
        done.type = PB_MGMT_TYPE_UNENROLL_DONE;
        std::memcpy(done.command_id, command.data(), 16);
        done.result = PB_MGMT_RESULT_ERASED;
        return Pack(done, c);
    }

    static std::string ReceiptBody(const std::vector<uint8_t> &receipt)
    {
        return "{\"receipt\":\"" + LowerHex(receipt.data(), receipt.size()) + "\"}";
    }

    HttpResponse PostReceipt(const std::string &token, const std::vector<uint8_t> &receipt)
    {
        const std::string body = ReceiptBody(receipt);
        return Request("POST", std::string(kRemovals) + "/receipt", token, &body);
    }

    // The board carries a pending removal out: poll, poll with the nonce,
    // the command, its "released" notice. Returns the notice's CoAP code.
    uint8_t BoardUnenrolls(const Credential &c, std::vector<uint8_t> *proof_out = nullptr,
                           std::array<uint8_t, 16> *command_out = nullptr)
    {
        ingest::ManagementReply idle = management->HandleDatagram(Poll(c));
        pb_mgmt_msg_t a;
        if (!pb_mgmt_parse(idle.payload.data(), idle.payload.size(), &a) || a.type != PB_MGMT_TYPE_IDLE) {
            return 0;
        }
        ingest::ManagementReply command = management->HandleDatagram(Poll(c, a.hub_nonce));
        pb_mgmt_msg_t u;
        if (!pb_mgmt_parse(command.payload.data(), command.payload.size(), &u) || u.type != PB_MGMT_TYPE_UNENROLL) {
            return 0;
        }
        pb_mgmt_msg_t done;
        std::memset(&done, 0, sizeof(done));
        done.type = PB_MGMT_TYPE_UNENROLL_DONE;
        std::memcpy(done.command_id, u.command_id, 16);
        done.result = PB_MGMT_RESULT_RELEASED;
        std::vector<uint8_t> proof = Pack(done, c);
        if (proof_out != nullptr) {
            *proof_out = proof;
        }
        if (command_out != nullptr) {
            std::memcpy(command_out->data(), u.command_id, 16);
        }
        return management->HandleDatagram(proof).code;
    }

    // The board fetches the command of a pending removal (two polls); the job
    // is "sent" afterwards.
    bool BoardGetsCommand(const Credential &c, std::array<uint8_t, 16> &command_out)
    {
        ingest::ManagementReply idle = management->HandleDatagram(Poll(c));
        pb_mgmt_msg_t a;
        if (!pb_mgmt_parse(idle.payload.data(), idle.payload.size(), &a) || a.type != PB_MGMT_TYPE_IDLE) {
            return false;
        }
        ingest::ManagementReply command = management->HandleDatagram(Poll(c, a.hub_nonce));
        pb_mgmt_msg_t u;
        if (!pb_mgmt_parse(command.payload.data(), command.payload.size(), &u) || u.type != PB_MGMT_TYPE_UNENROLL) {
            return false;
        }
        std::memcpy(command_out.data(), u.command_id, 16);
        return true;
    }

    // The board's "released" notice for `command`. Returns the CoAP code.
    uint8_t BoardSaysReleased(const Credential &c, const std::array<uint8_t, 16> &command)
    {
        pb_mgmt_msg_t done;
        std::memset(&done, 0, sizeof(done));
        done.type = PB_MGMT_TYPE_UNENROLL_DONE;
        std::memcpy(done.command_id, command.data(), 16);
        done.result = PB_MGMT_RESULT_RELEASED;
        return management->HandleDatagram(Pack(done, c)).code;
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

} // namespace

// ---- /api/v1/power-base-forgets: an offline Power Base is taken out of the hub's list ----

static const char kForgets[] = "/api/v1/power-base-forgets";

static std::string ForgetBody(const std::string &uuid, const std::string &record, const std::string &request_id)
{
    return "{\"power_base_uuid\":\"" + uuid + "\",\"record\":\"" + record + "\",\"request_id\":\"" + request_id + "\"}";
}

static std::string ForgetRequestId(unsigned n)
{
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%024x%08x", 0xf06e7u, n);
    return buf;
}

// The "record" of `eui`'s entry in the snapshot ("" when it has none or is not listed).
static std::string SnapshotRecord(const std::string &snapshot, const std::string &eui)
{
    const size_t entry = snapshot.find("{\"uuid\":\"" + eui + "\"");
    if (entry == std::string::npos) {
        return std::string();
    }
    const size_t end = snapshot.find('}', entry);
    const std::string body = snapshot.substr(entry, end - entry);
    return JsonStringField(body + "\"", "record");
}


// ---- the order "copy the Registry, then read management" in GET and SSE ----

// Reads SSE frames until one complete `event: snapshot` satisfies `pred`, or
// ~5 s pass. Returns that event's data ("" when none matched). `buf` keeps
// what was read beyond it.
template <typename Pred> static std::string NextSnapshotEvent(int fd, std::string &buf, Pred pred)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        size_t start;
        while ((start = buf.find("event: snapshot\ndata: ")) != std::string::npos) {
            const size_t data_start = start + std::strlen("event: snapshot\ndata: ");
            const size_t end = buf.find("\n\n", data_start);
            if (end == std::string::npos) {
                break;
            }
            std::string data = buf.substr(data_start, end - data_start);
            buf.erase(0, end + 2);
            if (pred(data)) {
                return data;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::string();
        }
        char chunk[8192];
        const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            return std::string();
        }
        buf.append(chunk, static_cast<size_t>(n));
    }
}

static int OpenEventStream(uint16_t port)
{
    const int fd = test_support::ConnectLoopback(port);
    if (fd < 0) {
        return -1;
    }
    timeval tv{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const std::string req = "GET /api/v1/events HTTP/1.1\r\nHost: test\r\n\r\n";
    send(fd, req.data(), req.size(), 0);
    return fd;
}

static size_t CountOf(const std::string &text, const std::string &what)
{
    size_t count = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) {
        count++;
    }
    return count;
}

static bool Lists(const std::string &snapshot, const std::string &uuid)
{
    return snapshot.find("{\"uuid\":\"" + uuid + "\"") != std::string::npos;
}

static void SnapshotOrderTests(int *failures)
{
    const ingest::Uuid base = UuidOf(kEui);
    const char kOtherEui[] = "6055f9fffe00b0b0";
    const ingest::Uuid module{0x74, 0x4d, 0xbd, 0xff, 0xfe, 0x65, 0x00, 0x01};
    const std::string module_hex = "744dbdfffe650001";
    // A Base this hub lists from its data, with one module.
    auto heard = [&](Fixture &f) {
        f.registry.apply_heartbeat(base, 1, 0);
        f.registry.apply_topology(base, module, base, true, 1, 2, 2, false, 1, 1);
    };

    printf("\n-- scenario: GET /api/v1/snapshot while another request takes the Base out of the list --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts with management");
        const std::string token = f.Pair("iPad");
        heard(f);
        HttpResponse before = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        const std::string record = SnapshotRecord(before.body, kEui);
        API_CHECK(record.size() == 32 && Lists(before.body, kEui) && Lists(before.body, module_hex) &&
                      before.body.find("\"power_base_forgets\":[]") != std::string::npos,
                  "before: the snapshot lists the Base with its record and its module, and no list removal");
        f.steady_shift = std::chrono::seconds(600);
        // This GET copies the Registry -- and before it reads management,
        // another device's request is carried out through the API.
        int ran = 0;
        int forget_status = 0;
        std::string forget_id;
        f.SetAfterCopy([&]() {
            if (ran++ != 0) {
                return;
            }
            const std::string body = ForgetBody(kEui, record, ForgetRequestId(50));
            HttpResponse r = f.Request("POST", kForgets, token, &body);
            forget_status = r.status;
            forget_id = JsonStringField(r.body, "forget_id");
        });
        HttpResponse during = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        f.SetAfterCopy(nullptr);
        API_CHECK(ran == 1 && forget_status == 201 && forget_id.size() == 32,
                  "(the list removal was carried out between this GET's Registry copy and its reading of management)");
        API_CHECK(during.status == 200 && during.body.find("\"forget_id\":\"" + forget_id + "\"") != std::string::npos,
                  "the snapshot of that GET has the list removal");
        API_CHECK(!Lists(during.body, kEui) && !Lists(during.body, module_hex),
                  "and does NOT list the Base or its module with it: the copy from before the removal was not used");
        HttpResponse after = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        auto lists_of = [](const std::string &body) {
            const size_t from = body.find("\"power_bases\"");
            const size_t to = body.find("\"settings_state\"");
            return from == std::string::npos ? std::string() : body.substr(from, to == std::string::npos ? to : to - from);
        };
        API_CHECK(lists_of(after.body) == lists_of(during.body) && !lists_of(during.body).empty(),
                  "the next GET says the same of Bases, modules, removals and list removals: nothing was half done");
    }

    printf("\n-- scenario: the first event of GET /api/v1/events, with the list removal in between --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts with management");
        const std::string token = f.Pair("iPad");
        heard(f);
        const std::string record = SnapshotRecord(PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot")).body, kEui);
        f.steady_shift = std::chrono::seconds(600);
        int ran = 0;
        std::string forget_id;
        f.SetAfterCopy([&]() {
            if (ran++ != 0) {
                return;
            }
            const std::string body = ForgetBody(kEui, record, ForgetRequestId(51));
            forget_id = JsonStringField(f.Request("POST", kForgets, token, &body).body, "forget_id");
        });
        const int fd = OpenEventStream(f.port());
        std::string buf;
        const std::string first = NextSnapshotEvent(fd, buf, [](const std::string &) { return true; });
        API_CHECK(ran >= 1 && forget_id.size() == 32 && !first.empty(), "(the stream's first copy of the Registry was followed by the list removal)");
        API_CHECK(first.find("\"forget_id\":\"" + forget_id + "\"") != std::string::npos && !Lists(first, kEui) &&
                      !Lists(first, module_hex),
                  "the first event has the list removal and does not list the Base or its module");
        f.SetAfterCopy(nullptr);
        close(fd);
    }

    printf("\n-- scenario: a later event of an open stream, with the list removal in between --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts with management");
        const std::string token = f.Pair("iPad");
        heard(f);
        const int fd = OpenEventStream(f.port());
        std::string buf;
        const std::string first = NextSnapshotEvent(fd, buf, [](const std::string &) { return true; });
        const std::string record = SnapshotRecord(first, kEui);
        API_CHECK(record.size() == 32 && Lists(first, module_hex), "the open stream's first event lists the Base and its module");
        f.steady_shift = std::chrono::seconds(600);
        int ran = 0;
        std::string forget_id;
        f.SetAfterCopy([&]() {
            if (ran++ != 0) {
                return;
            }
            const std::string body = ForgetBody(kEui, record, ForgetRequestId(52));
            forget_id = JsonStringField(f.Request("POST", kForgets, token, &body).body, "forget_id");
        });
        // Something unrelated changes in the Registry: the stream's loop
        // wakes, copies the Registry (the Base is still in it) -- and then
        // the list removal is carried out before it reads management.
        f.registry.apply_heartbeat(UuidOf(kOtherEui), 1, 0);
        bool mixed = false;
        const std::string next = NextSnapshotEvent(fd, buf, [&](const std::string &data) {
            mixed = mixed || (Lists(data, kEui) && data.find("\"forget_id\"") != std::string::npos);
            return data.find("\"forget_id\"") != std::string::npos;
        });
        API_CHECK(ran >= 1 && forget_id.size() == 32 && next.find("\"forget_id\":\"" + forget_id + "\"") != std::string::npos,
                  "(the loop's copy was followed by the list removal; an event with the record arrives)");
        API_CHECK(!mixed && !Lists(next, kEui) && !Lists(next, module_hex) && Lists(next, kOtherEui),
                  "no event shows the Base together with its list removal; the one with the record lists only the other Base");
        f.SetAfterCopy(nullptr);
        close(fd);
    }

    printf("\n-- scenario: GET /api/v1/snapshot while the board's 'released' notice arrives --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts with management");
        const std::string token = f.Pair("iPad");
        Credential cred;
        API_CHECK(f.EnrollManaged(token, kEui, cred), "(a Base registered with a credential)");
        f.registry.apply_topology(base, module, base, true, 1, 2, 2, false, 1, 1);
        HttpResponse posted = f.PostRemoval(token, kEui);
        const std::string removal_id = JsonStringField(posted.body, "removal_id");
        API_CHECK(posted.status == 202 && removal_id.size() == 32, "(its removal is requested)");
        // The command went out before this GET: between the GET's copy of
        // the Registry and the notice nothing else happens in the Registry.
        std::array<uint8_t, 16> command{};
        API_CHECK(f.BoardGetsCommand(cred, command), "(the board fetched the command: the job is 'sent')");
        int ran = 0;
        uint8_t notice = 0;
        f.SetAfterCopy([&]() {
            if (ran++ == 0) {
                notice = f.BoardSaysReleased(cred, command);
            }
        });
        HttpResponse during = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        f.SetAfterCopy(nullptr);
        API_CHECK(ran == 1 && notice == ingest::kCoapCodeContent, "(the board released between the copy and the reading)");
        API_CHECK(during.body.find("\"removal_id\":\"" + removal_id + "\"") != std::string::npos &&
                      during.body.find("\"state\":\"released\"") != std::string::npos && !Lists(during.body, kEui) &&
                      !Lists(during.body, module_hex),
                  "the snapshot says released and does not list the Base or its module: one picture");
    }
}

static void ForgetApiTests(int *failures)
{
    printf("\n-- scenario: without management the forget routes do not exist --\n");
    {
        Fixture f;
        API_CHECK(f.Start(false), "the server starts without management");
        std::string token = f.Pair("iPad");
        const std::string body = ForgetBody(kEui, ForgetRequestId(1), ForgetRequestId(2));
        HttpResponse r = f.Request("POST", kForgets, token, &body);
        API_CHECK(r.status == 404 && r.body == "{\"error\":\"forget_disabled\"}", "POST over TLS with a valid token is 404 forget_disabled");
        HttpResponse health = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/health"));
        f.registry.apply_heartbeat(UuidOf(kEui), 1, 0);
        HttpResponse snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(health.body.find("power_base_forget") == std::string::npos &&
                      snapshot.body.find("\"record\"") == std::string::npos &&
                      snapshot.body.find("power_base_forgets") == std::string::npos,
                  "health and the snapshot do not mention it: a client must not assume the routes");
    }

    printf("\n-- scenario: the forget routes behind TLS and a paired device's token --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts with management");
        std::string token = f.Pair("iPad");
        f.registry.apply_heartbeat(UuidOf(kEui), 1, 0);
        HttpResponse snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        const std::string record = SnapshotRecord(snapshot.body, kEui);
        API_CHECK(record.size() == 32 && snapshot.body.find("\"power_base_forgets\":[]") != std::string::npos,
                  "the snapshot gives the listed Base a record and an empty power_base_forgets");
        f.steady_shift = std::chrono::seconds(600);
        const std::string body = ForgetBody(kEui, record, ForgetRequestId(1));
        HttpResponse r = PlainHttpRequest(f.port(), HttpRequestText("POST", kForgets, token, &body));
        API_CHECK(r.status == 403 && r.body == "{\"error\":\"tls_required\"}", "plain HTTP: 403 tls_required, even with a valid token");
        r = PlainHttpRequest(f.port(), HttpRequestText("GET", kForgets, token));
        API_CHECK(r.status == 403, "plain-HTTP GET of the list too");
        r = f.Request("POST", kForgets, "", &body);
        API_CHECK(r.status == 401, "TLS without a token: 401");
        r = f.Request("POST", kForgets, std::string(64, 'a'), &body);
        API_CHECK(r.status == 401, "TLS with an unknown token: 401");
        r = f.Request("GET", kForgets, std::string(64, 'a'));
        API_CHECK(r.status == 401, "the list with an unknown token: 401");
        API_CHECK(!f.management->IsBlocked(kEui) && f.management->ListForgets().empty() &&
                      f.registry.view().power_bases.size() == 1,
                  "none of those requests changed anything: the Base is still listed");

        printf("\n-- scenario: requests that are not a list removal --\n");
        auto post = [&](const std::string &b) { return f.Request("POST", kForgets, token, &b); };
        API_CHECK(post("{").status == 400 && post("[]").status == 400 &&
                      post("{\"power_base_uuid\":\"" + std::string(kEui) + "\"}").status == 400 &&
                      post("{\"power_base_uuid\":\"" + std::string(kEui) + "\",\"record\":\"" + record + "\"}").status == 400 &&
                      post("{\"power_base_uuid\":\"" + std::string(kEui) + "\",\"record\":\"" + record + "\",\"request_id\":\"" +
                           ForgetRequestId(1) + "\",\"force\":true}")
                              .status == 400 &&
                      post("{\"power_base_uuid\":1,\"record\":\"" + record + "\",\"request_id\":\"" + ForgetRequestId(1) + "\"}")
                              .status == 400,
                  "malformed JSON, a missing member, an extra member, a member of the wrong type: 400");
        r = post(ForgetBody("zz", record, ForgetRequestId(1)));
        API_CHECK(r.status == 422 && r.body == "{\"error\":\"invalid_power_base_uuid\"}", "a malformed EUI-64: 422, the value is not echoed");
        r = post(ForgetBody(kEui, "ABCDEF", ForgetRequestId(1)));
        API_CHECK(r.status == 422 && r.body == "{\"error\":\"invalid_record\"}", "a malformed record: 422");
        r = post(ForgetBody(kEui, record, "1234"));
        API_CHECK(r.status == 422 && r.body == "{\"error\":\"invalid_request_id\"}", "a malformed request id: 422");
        r = f.Request("DELETE", std::string(kForgets) + "/" + ForgetRequestId(9), token);
        API_CHECK(r.status == 405, "DELETE: 405 -- there is nothing to cancel or undo");
        r = f.Request("PUT", kForgets, token, &body);
        API_CHECK(r.status == 405, "PUT: 405");
        r = f.Request("GET", std::string(kForgets) + "/" + ForgetRequestId(9), token);
        API_CHECK(r.status == 404 && r.body == "{\"error\":\"forget_not_found\"}", "GET of an unknown id: 404 forget_not_found");
        r = post(ForgetBody(kOtherEui, record, ForgetRequestId(1)));
        API_CHECK(r.status == 404 && r.body == "{\"error\":\"unknown_power_base\"}", "a Base the hub does not list: 404 unknown_power_base");
        r = post(ForgetBody(kEui, ForgetRequestId(77), ForgetRequestId(1)));
        API_CHECK(r.status == 409 && r.body == "{\"error\":\"stale_record\"}", "a record that is not the listed one: 409 stale_record");
        f.steady_shift = std::chrono::seconds(0);
        r = post(body);
        API_CHECK(r.status == 409 && r.body == "{\"error\":\"power_base_online\"}", "while the Base is heard: 409 power_base_online");
        API_CHECK(!f.management->IsBlocked(kEui) && f.management->ListForgets().empty() &&
                      f.registry.view().power_bases.size() == 1,
                  "after all of those the Base is still listed");

        printf("\n-- scenario: an offline Base without a credential is taken out of the list over the API --\n");
        const ingest::Uuid base = UuidOf(kEui);
        const ingest::Uuid module{0x74, 0x4d, 0xbd, 0xff, 0xfe, 0x65, 0x00, 0x01};
        f.registry.apply_topology(base, module, base, true, 1, 3, 6, false, 1, 1);
        f.steady_shift = std::chrono::seconds(600);
        std::string other = f.Pair("iPhone");
        r = post(body);
        const std::string forget_id = JsonStringField(r.body, "forget_id");
        API_CHECK(r.status == 201 && forget_id.size() == 32 &&
                      r.headers.find(std::string("Location: ") + kForgets + "/" + forget_id) != std::string::npos,
                  "201 with the record and its Location");
        API_CHECK(JsonStringField(r.body, "state") == "removed_from_hub" && JsonStringField(r.body, "power_base_uuid") == kEui &&
                      JsonStringField(r.body, "record") == record && JsonStringField(r.body, "request_id") == ForgetRequestId(1) &&
                      r.body.find("\"managed\":false") != std::string::npos &&
                      r.body.find("\"affected_module_count\":1") != std::string::npos &&
                      r.body.find("\"cancelled_removal_id\":null,\"kept_removal_id\":null") != std::string::npos &&
                      r.body.find("completed") == std::string::npos && r.body.find("token") == std::string::npos,
                  "the body: removed_from_hub, the Base, the record, not managed, one module, no removal touched -- "
                  "nothing called completed, no token");
        snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(snapshot.body.find(std::string("\"uuid\":\"") + kEui) == std::string::npos &&
                      snapshot.body.find("\"modules\":[]") != std::string::npos &&
                      snapshot.body.find("\"power_base_removals\":[]") != std::string::npos &&
                      snapshot.body.find("\"power_base_forgets\":[{\"forget_id\":\"" + forget_id + "\"") != std::string::npos,
                  "the snapshot: the Base and its module are gone, no removal job exists, the list removal is reported");
        r = post(body);
        API_CHECK(r.status == 200 && JsonStringField(r.body, "forget_id") == forget_id, "the same request again: 200, the same record");
        r = f.Request("POST", kForgets, other, &body);
        API_CHECK(r.status == 200 && JsonStringField(r.body, "forget_id") == forget_id,
                  "from the other paired device with the same request id: the same record");
        const std::string other_body = ForgetBody(kEui, record, ForgetRequestId(2));
        r = f.Request("POST", kForgets, other, &other_body);
        API_CHECK(r.status == 404 && r.body == "{\"error\":\"unknown_power_base\"}" && f.management->ListForgets().size() == 1,
                  "a different request for the same Base from the other device: 404 -- it is not listed; one record, not two");
        r = f.Request("GET", std::string(kForgets) + "/" + forget_id, other);
        API_CHECK(r.status == 200 && JsonStringField(r.body, "forget_id") == forget_id, "either device reads the record by its id");
        r = f.Request("GET", kForgets, other);
        API_CHECK(r.status == 200 && r.body.find("{\"forgets\":[{\"forget_id\":\"" + forget_id + "\"") == 0, "and in the list");
        r = f.Request("GET", kRemovals, token);
        API_CHECK(r.status == 200 && r.body == "{\"removals\":[]}", "the removal list is unaffected: a list removal is not a removal job");
        f.registry.apply_heartbeat(base, 2, 0);
        snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(snapshot.body.find(std::string("\"uuid\":\"") + kEui) == std::string::npos,
                  "a late heartbeat of that Base does not list it again");
        HttpResponse again_token = f.Request("GET", kRemovals, token);
        HttpResponse other_token = f.Request("GET", kRemovals, other);
        API_CHECK(again_token.status == 200 && other_token.status == 200,
                  "both devices' tokens are as valid as before: a list removal touches no device's pairing");
    }

    printf("\n-- scenario: a managed Base, two devices asking at the same moment --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts");
        std::string phone = f.Pair("iPhone 13 mini");
        std::string max = f.Pair("iPhone 16 Pro Max");
        Credential cred, keep;
        API_CHECK(f.EnrollManaged(phone, kEui, cred) && f.EnrollManaged(phone, kOtherEui, keep),
                  "(two Bases are enrolled with a management credential)");
        HttpResponse snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(SnapshotRecord(snapshot.body, kEui) == cred.generation && SnapshotRecord(snapshot.body, kOtherEui) == keep.generation,
                  "each device reads the same record for each Base: its registration's generation");
        f.steady_shift = std::chrono::seconds(600);
        const std::string a = ForgetBody(kEui, cred.generation, ForgetRequestId(10));
        const std::string b = ForgetBody(kEui, cred.generation, ForgetRequestId(11));
        HttpResponse ra, rb;
        std::thread first([&] { ra = f.Request("POST", kForgets, phone, &a); });
        std::thread second([&] { rb = f.Request("POST", kForgets, max, &b); });
        first.join();
        second.join();
        const bool one_each = (ra.status == 201 && rb.status == 404) || (ra.status == 404 && rb.status == 201);
        API_CHECK(one_each && f.management->ListForgets().size() == 1,
                  "both ask at once with their own request ids: exactly one is carried out (201), the other finds the "
                  "Base not listed (404) -- one record");
        API_CHECK(f.management->IsBlocked(kEui) && !f.management->IsManaged(kEui) && f.management->IsManaged(kOtherEui) &&
                      !f.management->IsBlocked(kOtherEui),
                  "the Base is out; the other Base is untouched");
        snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(snapshot.body.find(std::string("\"uuid\":\"") + kEui) == std::string::npos &&
                      SnapshotRecord(snapshot.body, kOtherEui) == keep.generation &&
                      snapshot.body.find("\"managed\":true") != std::string::npos,
                  "the snapshot both devices get: the Base gone, the other with its record, the list removal marked managed");

        // An old confirmation against a new registration of the same board.
        Credential renewed;
        f.steady_shift = std::chrono::seconds(0);
        API_CHECK(f.EnrollManaged(max, kEui, renewed) && renewed.generation != cred.generation && f.management->IsManaged(kEui),
                  "(the board is registered again from the other device)");
        f.steady_shift = std::chrono::seconds(600);
        const std::string old_confirmation = ForgetBody(kEui, cred.generation, ForgetRequestId(12));
        HttpResponse r = f.Request("POST", kForgets, phone, &old_confirmation);
        API_CHECK(r.status == 409 && r.body == "{\"error\":\"stale_record\"}" && f.management->IsManaged(kEui) &&
                      !f.management->IsBlocked(kEui),
                  "the first device's old confirmation (the earlier record): 409 stale_record -- the new registration stays");
        // Whichever of the two requests was carried out above: sent again, the one that was is the
        // same record (200), and the one that was not names the earlier registration (409).
        r = f.Request("POST", kForgets, phone, &a);
        const bool as_it_must = ra.status == 201 ? r.status == 200 : (r.status == 409 && r.body == "{\"error\":\"stale_record\"}");
        API_CHECK(as_it_must && f.management->IsManaged(kEui) && !f.management->IsBlocked(kEui),
                  "and the first device's old request sent again does not remove the new registration either");

        // A removal whose command went out: left alone, reported in the record.
        r = f.PostRemoval(phone, kOtherEui);
        const std::string removal_id = JsonStringField(r.body, "removal_id");
        ingest::ManagementReply idle = f.management->HandleDatagram(Poll(keep));
        pb_mgmt_msg_t ack;
        pb_mgmt_parse(idle.payload.data(), idle.payload.size(), &ack);
        ingest::ManagementReply command = f.management->HandleDatagram(Poll(keep, ack.hub_nonce));
        API_CHECK(r.status == 202 && command.code == ingest::kCoapCodeContent && f.RemovalState(phone, removal_id) == "sent",
                  "(the other Base: a removal is requested and its command goes out -- 'sent')");
        const std::string with_job = ForgetBody(kOtherEui, keep.generation, ForgetRequestId(13));
        r = f.Request("POST", kForgets, max, &with_job);
        API_CHECK(r.status == 201 && JsonStringField(r.body, "kept_removal_id") == removal_id &&
                      f.RemovalState(phone, removal_id) == "sent",
                  "list removal of it: 201, the record names the kept removal, and that job is still 'sent'");
        r = f.Request("DELETE", std::string(kRemovals) + "/" + removal_id, phone);
        API_CHECK(r.status == 409 && f.RemovalState(phone, removal_id) == "sent", "which still cannot be cancelled");
    }

    printf("\n-- scenario: the state file is damaged --\n");
    {
        Fixture f;
        API_CHECK(f.Start(true, true), "the server starts");
        std::string token = f.Pair("iPad");
        const std::string body = ForgetBody(kEui, ForgetRequestId(1), ForgetRequestId(2));
        HttpResponse r = f.Request("POST", kForgets, token, &body);
        API_CHECK(r.status == 503 && JsonStringField(r.body, "error") == "forget_unavailable", "POST: 503 forget_unavailable");
        r = f.Request("GET", kForgets, token);
        API_CHECK(r.status == 503, "the list too");
        f.registry.apply_heartbeat(UuidOf(kEui), 1, 0);
        HttpResponse snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(snapshot.body.find("\"record\"") == std::string::npos && snapshot.body.find("power_base_forgets") == std::string::npos,
                  "the snapshot offers no record to act on");
    }
}


// ---- a device that missed a whole removal: what the snapshot still says ----

static void DepartureSnapshotTests(int *failures)
{
    const ingest::Uuid base = UuidOf(kEui);
    const char kOtherEui[] = "6055f9fffe00b0b0";
    const ingest::Uuid other = UuidOf(kOtherEui);
    const ingest::Uuid module{0x74, 0x4d, 0xbd, 0xff, 0xfe, 0x65, 0x00, 0x01};
    const ingest::Uuid other_module{0x74, 0x4d, 0xbd, 0xff, 0xfe, 0x65, 0x00, 0x02};
    const std::string module_hex = "744dbdfffe650001";
    const std::string other_module_hex = "744dbdfffe650002";
    auto departures_of = [](const std::string &body) {
        const size_t from = body.find("\"power_base_departures\":[");
        if (from == std::string::npos) {
            return std::string("<none>");
        }
        return body.substr(from, body.find(']', from) - from + 1);
    };
    auto removals_of = [](const std::string &body) {
        const size_t from = body.find("\"power_base_removals\":[");
        const size_t to = body.find("\"power_base_forgets\"");
        return from == std::string::npos || to == std::string::npos ? std::string("<none>") : body.substr(from, to - from - 1);
    };
    // Removes `eui` the ordinary way, to the board's receipt.
    auto removed = [&](Fixture &f, const std::string &token, const char *eui, const Credential &cred, std::string &removal_id) {
        HttpResponse posted = f.PostRemoval(token, eui);
        removal_id = JsonStringField(posted.body, "removal_id");
        std::array<uint8_t, 16> command{};
        if (posted.status != 202 || f.BoardUnenrolls(cred, nullptr, &command) != ingest::kCoapCodeContent) {
            return false;
        }
        HttpResponse r = f.PostReceipt(token, f.Receipt(cred, command));
        return r.status == 200 && JsonStringField(r.body, "state") == "completed";
    };

    printf("\n-- scenario: the hub's last Base is removed; a device reads the snapshot sixteen minutes later --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts with management");
        const std::string token = f.Pair("iPhone A");
        Credential cred;
        API_CHECK(f.EnrollManaged(token, kEui, cred), "(a Base registered with a credential)");
        f.registry.apply_topology(base, module, base, true, 1, 2, 2, false, 1, 1);
        HttpResponse before = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        const std::string record = SnapshotRecord(before.body, kEui);
        API_CHECK(Lists(before.body, kEui) && Lists(before.body, module_hex) && record.size() == 32 &&
                      departures_of(before.body) == "\"power_base_departures\":[]",
                  "before: the Base and its module are listed, and no Base is named as out of the list");
        std::string removal_id;
        API_CHECK(removed(f, token, kEui, cred, removal_id), "device A removes the Base; the board's receipt completes the job");
        HttpResponse soon = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(!Lists(soon.body, kEui) && !Lists(soon.body, module_hex) &&
                      soon.body.find("\"removal_id\":\"" + removal_id + "\"") != std::string::npos &&
                      departures_of(soon.body).find("\"power_base_uuid\":\"" + std::string(kEui) + "\"") != std::string::npos,
                  "right after: the job is shown, and the Base is named as out of the list");
        // The room clean-up of the removal is done (nothing was assigned),
        // then sixteen minutes pass on the hub's clock.
        f.management->ProcessSettingsCleanups();
        f.clock_shift_ms = 16 * 60 * 1000;
        HttpResponse late = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(late.status == 200 && late.body.find(removal_id) == std::string::npos &&
                      removals_of(late.body) == "\"power_base_removals\":[]",
                  "the snapshot a late device gets has no removal job any more: the time a finished job is shown is over");
        const std::string departures = departures_of(late.body);
        API_CHECK(departures.find("{\"power_base_uuid\":\"" + std::string(kEui) + "\",\"record\":\"" + record +
                                  "\",\"reason\":\"removed\",\"since_unix_ms\":") != std::string::npos &&
                      CountOf(departures, "\"power_base_uuid\"") == 1,
                  "it still says that the Base is out of the list: which registration ended, and that it was removed");
        API_CHECK(!Lists(late.body, kEui) && !Lists(late.body, module_hex) &&
                      late.body.find("\"modules\":[]") != std::string::npos,
                  "and lists neither the Base nor its module: an empty list WITH its reason");
        // The first event of a new event stream says the same.
        const int fd = OpenEventStream(f.port());
        std::string buf;
        const std::string first = NextSnapshotEvent(fd, buf, [](const std::string &) { return true; });
        close(fd);
        API_CHECK(!first.empty() && departures_of(first) == departures && !Lists(first, kEui) &&
                      removals_of(first) == "\"power_base_removals\":[]",
                  "so does the first event of GET /api/v1/events");
    }

    printf("\n-- scenario: one of two Bases is removed; then it is registered again --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts with management");
        const std::string token = f.Pair("iPhone A");
        Credential cred, keep;
        API_CHECK(f.EnrollManaged(token, kEui, cred) && f.EnrollManaged(token, kOtherEui, keep), "(two Bases registered)");
        f.registry.apply_topology(base, module, base, true, 1, 2, 2, false, 1, 1);
        f.registry.apply_topology(other, other_module, other, true, 1, 2, 2, false, 1, 1);
        const std::string kept_record =
            SnapshotRecord(PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot")).body, kOtherEui);
        std::string removal_id;
        API_CHECK(removed(f, token, kEui, cred, removal_id), "(one of them is removed and completed)");
        f.management->ProcessSettingsCleanups();
        f.clock_shift_ms = 16 * 60 * 1000;
        f.management->ProcessSettingsCleanups();
        HttpResponse late = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(CountOf(departures_of(late.body), "\"power_base_uuid\"") == 1 &&
                      departures_of(late.body).find(kEui) != std::string::npos &&
                      departures_of(late.body).find(kOtherEui) == std::string::npos,
                  "sixteen minutes on: the removed Base is named as out of the list, the other one is not");
        API_CHECK(Lists(late.body, kOtherEui) && Lists(late.body, other_module_hex) &&
                      SnapshotRecord(late.body, kOtherEui) == kept_record && !Lists(late.body, kEui),
                  "the other Base and its module are listed as before, under the same registration");
        // Registered again: listed, and no longer out of the list.
        Credential renewed;
        API_CHECK(f.EnrollManaged(token, kEui, renewed) && renewed.generation != cred.generation,
                  "(the removed Base is registered again)");
        f.registry.apply_topology(base, module, base, true, 1, 2, 2, false, 2, 1);
        HttpResponse again = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(departures_of(again.body) == "\"power_base_departures\":[]" && Lists(again.body, kEui) &&
                      Lists(again.body, module_hex) && SnapshotRecord(again.body, kEui) == renewed.generation,
                  "the snapshot lists it under the new registration and names nothing as out of the list");
    }

    printf("\n-- scenario: without the list-removal capability nothing is added --\n");
    {
        Fixture f;
        API_CHECK(f.Start(false), "the server starts without management");
        HttpResponse r = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(r.status == 200 && r.body.find("power_base_departures") == std::string::npos,
                  "a hub without management does not have the field at all");
    }
}

// ---- a removal request names its registration (section 16), over the real routes ----

// The integer after "key": in a JSON text (-1 when it is not there).
static long long JsonIntAfter(const std::string &text, const std::string &key, size_t from = 0)
{
    const std::string needle = "\"" + key + "\":";
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) {
        return -1;
    }
    return std::strtoll(text.c_str() + at + needle.size(), nullptr, 10);
}

// "removal_serial" of `uuid`'s entry in a snapshot's `power_bases` (-1: none).
static long long SnapshotRemovalSerial(const std::string &snapshot, const std::string &uuid)
{
    const size_t entry = snapshot.find("{\"uuid\":\"" + uuid + "\"");
    if (entry == std::string::npos) {
        return -1;
    }
    const size_t end = snapshot.find('}', entry);
    return JsonIntAfter(snapshot.substr(entry, end - entry), "removal_serial");
}

static void RemovalBindingApiTests(int *failures)
{
    const char kOtherEui[] = "6055f9fffe00b0b0";
    auto snapshot_of = [](Fixture &f) { return PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot")).body; };
    auto removals_of = [](const std::string &body) {
        const size_t from = body.find("\"power_base_removals\":");
        const size_t to = body.find(",\"power_base_forgets\"");
        return from == std::string::npos || to == std::string::npos ? std::string() : body.substr(from, to - from);
    };
    // Whether the hub has an UNENROLL for this registration's board.
    auto board_gets_command = [](Fixture &f, const Credential &c) {
        std::array<uint8_t, 16> command{};
        return f.BoardGetsCommand(c, command);
    };

    printf("\n-- scenario: the same POST again after the Base was removed and registered again (the reviewed order) --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts with management");
        const std::string phone_a = f.Pair("iPhone A");
        const std::string phone_b = f.Pair("iPhone B");
        Credential old_reg;
        API_CHECK(f.EnrollManaged(phone_b, kEui, old_reg), "(the Base is registered)");
        // Phone A reads the list and the user confirms.
        const std::string seen = snapshot_of(f);
        const std::string record = SnapshotRecord(seen, kEui);
        const long long serial = SnapshotRemovalSerial(seen, kEui);
        API_CHECK(record == old_reg.generation && serial == 0,
                  "the snapshot gives the Base its registration (record) and removal_serial 0: what a request names");
        const std::string request_id = NextRemovalRequestId();
        const std::string body = RemovalBody(kEui, record, static_cast<uint32_t>(serial), request_id);
        HttpResponse r = f.Request("POST", kRemovals, phone_a, &body);
        const std::string removal_id = JsonStringField(r.body, "removal_id");
        API_CHECK(r.status == 202 && removal_id.size() == 32 && JsonStringField(r.body, "generation") == record &&
                      JsonStringField(r.body, "request_id") == request_id,
                  "202: the job, for that registration, with the id of the request that made it (the answer is lost)");
        API_CHECK(SnapshotRemovalSerial(snapshot_of(f), kEui) == 1, "the Base's removal_serial is 1 now");
        std::array<uint8_t, 16> command{};
        API_CHECK(f.BoardUnenrolls(old_reg, nullptr, &command) == ingest::kCoapCodeContent &&
                      f.PostReceipt(phone_a, f.Receipt(old_reg, command)).status == 200 &&
                      f.RemovalState(phone_a, removal_id) == "completed",
                  "(the board carries it out and its receipt completes the job)");
        Credential new_reg;
        API_CHECK(f.EnrollManaged(phone_b, kEui, new_reg) && new_reg.generation != old_reg.generation,
                  "(phone B registers the same Base again: another registration)");
        const std::string relisted = snapshot_of(f);
        API_CHECK(SnapshotRecord(relisted, kEui) == new_reg.generation && SnapshotRemovalSerial(relisted, kEui) == 0,
                  "the snapshot lists it under the new registration, removal_serial 0");

        r = f.Request("POST", kRemovals, phone_a, &body);
        API_CHECK(r.status == 200 && JsonStringField(r.body, "removal_id") == removal_id &&
                      JsonStringField(r.body, "state") == "completed" && JsonStringField(r.body, "generation") == old_reg.generation,
                  "the very same POST again: 200 with the job it created -- the completed removal of the earlier registration");
        const std::string after_retry = snapshot_of(f);
        API_CHECK(CountOf(removals_of(after_retry), "\"removal_id\"") == 1 &&
                      removals_of(after_retry).find(new_reg.generation) == std::string::npos &&
                      SnapshotRecord(after_retry, kEui) == new_reg.generation && SnapshotRemovalSerial(after_retry, kEui) == 0,
                  "the snapshot has no job of the new registration, and that registration is untouched");
        API_CHECK(!board_gets_command(f, new_reg), "the new registration's board polls: no UNENROLL");

        // The finished job leaves the hub's history.
        f.management->ProcessSettingsCleanups();
        f.clock_shift_ms = 16 * 60 * 1000;
        f.management->ProcessSettingsCleanups();
        Credential unrelated;
        API_CHECK(f.EnrollManaged(phone_b, kOtherEui, unrelated) && removals_of(snapshot_of(f)) == "\"power_base_removals\":[]",
                  "(sixteen minutes on: the finished job is no longer listed)");
        r = f.Request("POST", kRemovals, phone_a, &body);
        API_CHECK(r.status == 409 && r.body == "{\"error\":\"stale_record\"}",
                  "the same POST once more: 409 stale_record, and no value is echoed");
        API_CHECK(removals_of(snapshot_of(f)) == "\"power_base_removals\":[]" && !board_gets_command(f, new_reg) &&
                      f.management->ListRemovals().empty(),
                  "no job was created and the new registration's board is given no command");

        // A new confirmation of what is listed now.
        const std::string now_listed = snapshot_of(f);
        const std::string fresh =
            RemovalBody(kEui, SnapshotRecord(now_listed, kEui), static_cast<uint32_t>(SnapshotRemovalSerial(now_listed, kEui)),
                        NextRemovalRequestId());
        r = f.Request("POST", kRemovals, phone_a, &fresh);
        API_CHECK(r.status == 202 && JsonStringField(r.body, "generation") == new_reg.generation && board_gets_command(f, new_reg),
                  "a request that names what is listed now is carried out: 202, and the command for that board");
    }

    printf("\n-- scenario: the request of an app that names no registration --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts with management");
        const std::string token = f.Pair("old iPhone");
        Credential cred;
        API_CHECK(f.EnrollManaged(token, kEui, cred), "(the Base is registered and can be removed)");
        const std::string legacy = LegacyRemovalBody(kEui);
        HttpResponse r = f.Request("POST", kRemovals, token, &legacy);
        API_CHECK(r.status == 422 && r.body == "{\"error\":\"removal_registration_required\"}",
                  "the Base alone: 422 removal_registration_required -- for a Base that IS listed and removable");
        API_CHECK(f.management->ListRemovals().empty() && !board_gets_command(f, cred) &&
                      SnapshotRemovalSerial(snapshot_of(f), kEui) == 0,
                  "nothing was created, the board is given no command, the serial did not move");
        const std::string upper = LegacyRemovalBody("6055F9FFFE00A0A0");
        r = f.Request("POST", kRemovals, token, &upper);
        API_CHECK(r.status == 422 && JsonStringField(r.body, "error") == "removal_registration_required",
                  "whatever Base it names: the same answer (nothing is looked up)");
        // The other routes of that app still work: the list, and a cancel.
        HttpResponse posted = f.PostRemoval(token, kEui);
        const std::string id = JsonStringField(posted.body, "removal_id");
        API_CHECK(posted.status == 202 && f.Request("GET", kRemovals, token).body.find(id) != std::string::npos &&
                      f.Request("DELETE", std::string(kRemovals) + "/" + id, token).status == 200,
                  "(a job made by a current request is listed and cancelled through the unchanged routes)");
    }

    printf("\n-- scenario: a cancelled request again; two devices; the request id of another Base --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts with management");
        const std::string phone_a = f.Pair("iPhone A");
        const std::string phone_b = f.Pair("iPhone B");
        Credential cred, other;
        API_CHECK(f.EnrollManaged(phone_a, kEui, cred) && f.EnrollManaged(phone_a, kOtherEui, other), "(two Bases registered)");
        const std::string record = SnapshotRecord(snapshot_of(f), kEui);
        const std::string id_a = NextRemovalRequestId(), id_b = NextRemovalRequestId();
        HttpResponse ra = f.PostRemovalRequest(phone_a, kEui, record, 0, id_a);
        HttpResponse rb = f.PostRemovalRequest(phone_b, kEui, record, 0, id_b);
        const std::string job = JsonStringField(ra.body, "removal_id");
        API_CHECK(ra.status == 202 && rb.status == 200 && JsonStringField(rb.body, "removal_id") == job &&
                      f.management->ListRemovals().size() == 1,
                  "two devices confirm the same registration: 202 for one, 200 with the same job for the other");
        API_CHECK(f.PostRemovalRequest(phone_a, kEui, record, 0, id_a).status == 200 &&
                      f.PostRemovalRequest(phone_b, kEui, record, 0, id_b).status == 200 && f.management->ListRemovals().size() == 1,
                  "each of them again: 200, still one job");
        HttpResponse conflict = f.PostRemovalRequest(phone_a, kOtherEui, SnapshotRecord(snapshot_of(f), kOtherEui), 0, id_a);
        API_CHECK(conflict.status == 409 && conflict.body == "{\"error\":\"request_id_conflict\"}" &&
                      f.management->ListRemovals().size() == 1,
                  "that request id for another Base: 409 request_id_conflict, nothing created");
        API_CHECK(f.Request("DELETE", std::string(kRemovals) + "/" + job, phone_a).status == 200, "(the job is cancelled)");
        HttpResponse again = f.PostRemovalRequest(phone_a, kEui, record, 0, id_a);
        API_CHECK(again.status == 200 && JsonStringField(again.body, "removal_id") == job &&
                      JsonStringField(again.body, "state") == "cancelled" && !board_gets_command(f, cred),
                  "the cancelled request again: 200 with the cancelled job, and no command for the board");
        HttpResponse late_b = f.PostRemovalRequest(phone_b, kEui, record, 0, id_b);
        API_CHECK(late_b.status == 409 && JsonStringField(late_b.body, "error") == "stale_record" &&
                      f.management->ListRemovals().size() == 1,
                  "the other device's request from before the cancel, again: 409 stale_record (a removal was asked of "
                  "this registration and ended since it was confirmed)");
        HttpResponse fresh = f.PostRemoval(phone_b, kEui);
        API_CHECK(fresh.status == 202 && JsonStringField(fresh.body, "removal_id") != job && board_gets_command(f, cred),
                  "a new confirmation (the present serial, a new request id): 202, and the command");
    }
}

// ---- where a snapshot stands among the snapshots of this run (snapshot_stream / snapshot_sequence) ----

static std::string SnapshotStream(const std::string &body)
{
    return JsonStringField(body, "snapshot_stream");
}

static long long SnapshotSequence(const std::string &body)
{
    return JsonIntAfter(body, "snapshot_sequence");
}

static void SnapshotSequenceTests(int *failures)
{
    const ingest::Uuid base = UuidOf(kEui);
    const char kOtherEui[] = "6055f9fffe00b0b0";
    auto get = [](Fixture &f) { return PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot")).body; };

    printf("\n-- scenario: every snapshot says where it stands; GET and events share one order --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts with management");
        const std::string token = f.Pair("iPad");
        f.registry.apply_heartbeat(base, 1, 0);
        const std::string first = get(f);
        const std::string stream = SnapshotStream(first);
        API_CHECK(stream.size() == 32 && SnapshotSequence(first) == 1,
                  "the first snapshot of this run: a stream id of 32 hex digits and sequence 1");
        const std::string last_fields = ",\"snapshot_stream\":\"" + stream + "\",\"snapshot_sequence\":1}";
        API_CHECK(first.size() > last_fields.size() &&
                      first.compare(first.size() - last_fields.size(), last_fields.size(), last_fields) == 0 &&
                      first.find("\"settings_revision\":null" + last_fields) != std::string::npos,
                  "the two fields are the last of the object, after every earlier field");
        const std::string second = get(f);
        API_CHECK(SnapshotStream(second) == stream && SnapshotSequence(second) == 2,
                  "the next one: the same stream, sequence 2 -- also when nothing changed in between");

        // An event stream of the same server counts in the same order.
        const int fd = OpenEventStream(f.port());
        std::string buf;
        const std::string event1 = NextSnapshotEvent(fd, buf, [](const std::string &) { return true; });
        API_CHECK(SnapshotStream(event1) == stream && SnapshotSequence(event1) == 3,
                  "the first event of GET /api/v1/events is sequence 3 of the same stream");
        const std::string third_get = get(f);
        API_CHECK(SnapshotSequence(third_get) == 4, "a GET after it is 4");

        // A change of the settings alone is a new snapshot with a larger number.
        const std::string settings =
            "{\"base_revision\":null,\"settings\":{\"home_name\":\"Home\",\"rooms\":[{\"id\":\"11111111-2222-4333-8444-555555555555\","
            "\"name_kind\":\"custom\",\"name\":\"Office\",\"symbol\":\"desktopcomputer\"}],\"modules\":[]}}";
        HttpResponse put = f.Request("PUT", "/api/v1/settings", token, &settings);
        const std::string revision = JsonStringField(put.body, "revision");
        API_CHECK(put.status == 200 && !revision.empty(), "(a device saves the shared settings: nothing else changes)");
        // A settings change is announced within one poll interval of the
        // event loop (1 s), not at once: the reader's own one-second receive
        // timeout may pass first, so it is asked a few times.
        std::string event2;
        for (int attempt = 0; attempt < 5 && event2.empty(); attempt++) {
            event2 = NextSnapshotEvent(fd, buf, [&](const std::string &data) {
                return data.find("\"settings_revision\":\"" + revision + "\"") != std::string::npos;
            });
        }
        if (event2.empty() || SnapshotSequence(event2) <= SnapshotSequence(third_get)) {
            printf("   (diagnosis: event of %zu bytes, sequence %lld after %lld; PUT answered %d %s)\n", event2.size(),
                   SnapshotSequence(event2), SnapshotSequence(third_get), put.status, put.body.substr(0, 120).c_str());
        }
        API_CHECK(!event2.empty() && SnapshotStream(event2) == stream && SnapshotSequence(event2) > SnapshotSequence(third_get),
                  "the event that carries the new settings revision has a larger sequence than everything before it");
        const std::string after_settings = get(f);
        API_CHECK(after_settings.find("\"settings_revision\":\"" + revision + "\"") != std::string::npos &&
                      SnapshotSequence(after_settings) > SnapshotSequence(event2),
                  "and a GET after it a larger one still, with the same revision");
        close(fd);
    }

    printf("\n-- scenario: a GET that copied the Registry early is built late --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts with management");
        f.registry.apply_heartbeat(base, 1, 0);
        get(f);
        // This GET copies the Registry. Before its snapshot is built, another
        // Base is heard and a second GET is answered completely.
        int ran = 0;
        std::string inner;
        f.SetAfterCopy([&]() {
            if (ran++ != 0) {
                return;
            }
            f.registry.apply_heartbeat(UuidOf(kOtherEui), 1, 0);
            inner = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot")).body;
        });
        const std::string outer = get(f);
        f.SetAfterCopy(nullptr);
        API_CHECK(ran >= 1 && Lists(inner, kOtherEui) && SnapshotSequence(inner) > 0,
                  "(the second GET was answered in between, and lists the Base that was heard meanwhile)");
        API_CHECK(SnapshotSequence(outer) > SnapshotSequence(inner) && SnapshotStream(outer) == SnapshotStream(inner),
                  "the first GET is built after it: the larger sequence");
        API_CHECK(Lists(outer, kOtherEui),
                  "and it does NOT say the older state under the larger number: its early copy of the Registry was "
                  "replaced, so it lists that Base too");
    }

    printf("\n-- scenario: the same on a hub without management (nothing but the snapshot's own section orders it) --\n");
    {
        Fixture f;
        API_CHECK(f.Start(false), "the server starts without management");
        f.registry.apply_heartbeat(base, 1, 0);
        get(f);
        int ran = 0;
        std::string inner;
        f.SetAfterCopy([&]() {
            if (ran++ != 0) {
                return;
            }
            f.registry.apply_heartbeat(UuidOf(kOtherEui), 1, 0);
            inner = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot")).body;
        });
        const std::string outer = get(f);
        f.SetAfterCopy(nullptr);
        API_CHECK(ran >= 1 && Lists(inner, kOtherEui) && SnapshotSequence(outer) > SnapshotSequence(inner) &&
                      SnapshotSequence(inner) > 0,
                  "(the second GET was answered in between; the first is built after it and has the larger sequence)");
        API_CHECK(Lists(outer, kOtherEui), "the first GET lists the Base that was heard meanwhile: its early copy was replaced");
        // An event stream that copied early is ordered the same way.
        int ran_again = 0;
        const char kThirdEui[] = "6055f9fffe00c0c0";
        f.SetAfterCopy([&]() {
            if (ran_again++ != 0) {
                return;
            }
            f.registry.apply_heartbeat(UuidOf(kThirdEui), 1, 0);
            inner = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot")).body;
        });
        const int fd = OpenEventStream(f.port());
        std::string buf;
        const std::string first_event = NextSnapshotEvent(fd, buf, [](const std::string &) { return true; });
        f.SetAfterCopy(nullptr);
        close(fd);
        API_CHECK(ran_again >= 1 && Lists(inner, kThirdEui) && SnapshotSequence(first_event) > SnapshotSequence(inner) &&
                      Lists(first_event, kThirdEui),
                  "the first event of a stream whose Registry copy was made before that GET: the larger sequence, and "
                  "not the older state");
    }

    printf("\n-- scenario: management built a snapshot from a later copy than its caller's; the next one copied in between --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts with management");
        const std::string token = f.Pair("iPad");
        const ingest::Uuid module{0x74, 0x4d, 0xbd, 0xff, 0xfe, 0x65, 0x00, 0x01};
        f.registry.apply_heartbeat(base, 1, 0);
        f.registry.apply_topology(base, module, base, true, 1, 2, 2, false, 1, 1);
        const std::string record = SnapshotRecord(get(f), kEui);
        API_CHECK(record.size() == 32, "(a Base this hub lists from its data, with its record)");
        f.steady_shift = std::chrono::seconds(600);
        // Two GETs. The first copies the Registry; then the Base is taken out
        // of the list (management changes what is listed), the second GET
        // copies the Registry -- after that change -- and waits; another Base
        // is heard; the first GET is built and answered; then the second.
        //   first GET:  copy from before the list change -> management puts a
        //               copy made at that moment in its place (the other Base
        //               is in it).
        //   second GET: its own copy is from after the list change, so
        //               management has no reason to replace it -- and it is
        //               from before the other Base was heard.
        std::mutex m;
        std::condition_variable cv;
        bool second_copied = false, first_answered = false;
        int calls = 0;
        int forget_status = 0;
        std::string second;
        std::thread second_get;
        f.SetAfterCopy([&]() {
            int call;
            {
                std::lock_guard<std::mutex> lock(m);
                call = ++calls;
            }
            if (call == 1) {
                const std::string body = ForgetBody(kEui, record, ForgetRequestId(60));
                forget_status = f.Request("POST", kForgets, token, &body).status;
                second_get = std::thread([&]() { second = get(f); });
                std::unique_lock<std::mutex> lock(m);
                cv.wait_for(lock, std::chrono::seconds(10), [&]() { return second_copied; });
                lock.unlock();
                f.registry.apply_heartbeat(UuidOf(kOtherEui), 1, 0);
            } else if (call == 2) {
                std::unique_lock<std::mutex> lock(m);
                second_copied = true;
                cv.notify_all();
                cv.wait_for(lock, std::chrono::seconds(10), [&]() { return first_answered; });
            }
        });
        const std::string first = get(f);
        {
            std::lock_guard<std::mutex> lock(m);
            first_answered = true;
        }
        cv.notify_all();
        if (second_get.joinable()) {
            second_get.join();
        }
        f.SetAfterCopy(nullptr);
        API_CHECK(calls == 2 && second_copied && forget_status == 201 && first.find("\"forget_id\"") != std::string::npos &&
                      !Lists(first, kEui) && Lists(first, kOtherEui),
                  "(the first GET was built from the copy management made: without the Base that was taken out, with "
                  "the Base that was heard after the second GET had copied)");
        API_CHECK(SnapshotStream(second) == SnapshotStream(first) && SnapshotSequence(second) > SnapshotSequence(first),
                  "the second GET is built after it: the larger sequence");
        API_CHECK(Lists(second, kOtherEui) && !Lists(second, kEui),
                  "and it does not say the older state under the larger number: its copy -- newer than the first "
                  "GET's own, older than the one management built the first from -- was replaced (by management, "
                  "which takes in no copy older than the last)");
    }

    printf("\n-- scenario: another run of the bridge is another stream; a hub without management counts too --\n");
    {
        Fixture one, two, plain;
        API_CHECK(one.Start() && two.Start() && plain.Start(false), "three servers start (the third without management)");
        const std::string a = get(one), b = get(two), c = get(plain);
        API_CHECK(SnapshotStream(a).size() == 32 && SnapshotStream(b).size() == 32 && SnapshotStream(a) != SnapshotStream(b) &&
                      SnapshotSequence(a) == 1 && SnapshotSequence(b) == 1,
                  "two runs: two stream ids, each starting at sequence 1");
        API_CHECK(SnapshotStream(c).size() == 32 && SnapshotSequence(c) == 1 && SnapshotSequence(get(plain)) == 2 &&
                      c.find("power_base_removals") == std::string::npos,
                  "the snapshot of a hub without management has the two fields as well, and nothing of management");
    }
}

void RunRemovalApiTests(int *failures)
{
    printf("\n-- scenario: without management the removal routes do not exist --\n");
    {
        Fixture f;
        API_CHECK(f.Start(false), "the server starts without management");
        std::string token = f.Pair("iPad");
        HttpResponse r = f.PostRemoval(token, kEui);
        API_CHECK(r.status == 404 && r.body == "{\"error\":\"removal_disabled\"}",
                  "POST over TLS with a valid token is 404 removal_disabled");
        r = PlainHttpRequest(f.port(), HttpRequestText("GET", kRemovals));
        API_CHECK(r.status == 404 && r.body.find("removal_disabled") != std::string::npos, "so is a plain-HTTP GET");
        HttpResponse health = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/health"));
        API_CHECK(health.body.find("power_base_management") == std::string::npos &&
                      health.body.find("power_base_removal") == std::string::npos,
                  "health does not mention management");
        f.registry.apply_heartbeat(UuidOf(kEui), 1, 0);
        HttpResponse snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(snapshot.body.find("\"management\"") == std::string::npos &&
                      snapshot.body.find("power_base_removals") == std::string::npos,
                  "the snapshot has no management fields");
        const std::string body = ManagedStartBody(kOtherEui);
        r = f.Request("POST", kEnrollments, token, &body);
        API_CHECK(r.status == 503 && r.body.find("\"reason\":\"management\"") != std::string::npos &&
                      r.body.find("key") == std::string::npos && f.fake->ensure_calls.load() == 0,
                  "an enrollment that asks for a credential is refused (503), and the OTBR is not touched");
    }

    printf("\n-- scenario: TLS and a paired device's token are required --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts with management");
        std::string token = f.Pair("iPad");
        const std::string body = RemovalBody(kEui, std::string(32, 'a'), 0, NextRemovalRequestId());
        HttpResponse health = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/health"));
        API_CHECK(health.body.find("\"power_base_enrollment\":\"available\",\"power_base_management\":1,"
                                   "\"power_base_removal\":\"available\",\"power_base_forget\":1,"
                                   "\"power_base_removal_request\":2}") != std::string::npos,
                  "health ends with power_base_management, power_base_removal, power_base_forget and the removal "
                  "request version (2: a request names its registration)");

        HttpResponse r = PlainHttpRequest(f.port(), HttpRequestText("POST", kRemovals, token, &body));
        API_CHECK(r.status == 403 && r.body == "{\"error\":\"tls_required\"}", "plain HTTP with a valid token is 403 tls_required");
        r = PlainHttpRequest(f.port(), HttpRequestText("GET", kRemovals, token));
        API_CHECK(r.status == 403, "plain-HTTP GET of the list is 403 too");
        r = PlainHttpRequest(f.port(), HttpRequestText("DELETE", std::string(kRemovals) + "/" + std::string(32, 'a'), token));
        API_CHECK(r.status == 403, "and plain-HTTP DELETE");
        r = f.Request("POST", kRemovals, "", &body);
        API_CHECK(r.status == 401 && r.headers.find("WWW-Authenticate: Bearer") != std::string::npos, "no token is 401");
        r = f.Request("POST", kRemovals, std::string(43, 'B'), &body);
        API_CHECK(r.status == 401, "an unknown token is 401");
        r = f.Request("GET", kRemovals, "");
        API_CHECK(r.status == 401, "the list without a token is 401");
        API_CHECK(f.management->ListRemovals().empty(), "none of that created a job");

        r = f.Request("PUT", kRemovals, token, &body);
        API_CHECK(r.status == 405, "PUT on the collection is 405");
        r = f.Request("DELETE", kRemovals, token);
        API_CHECK(r.status == 405, "DELETE on the collection is 405");
        r = f.Request("POST", std::string(kRemovals) + "/" + std::string(32, 'a'), token, &body);
        API_CHECK(r.status == 405, "POST on a job is 405");
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
        const std::string rec(32, 'a');
        const std::string rid(32, 'b');
        const std::string uuid_json = "\"power_base_uuid\":\"" + std::string(kEui) + "\"";
        const std::string record_json = "\"record\":\"" + rec + "\"";
        const std::string id_json = "\"request_id\":\"" + rid + "\"";
        const Case cases[] = {
            {"", 400, "invalid_request", "an empty body"},
            {"[]", 400, "invalid_request", "an array"},
            {"{}", 400, "invalid_request", "no power_base_uuid"},
            {"{\"power_base_uuid\":7}", 400, "invalid_request", "a number"},
            {"{\"power_base_uuid\":\"744dbdfffe65d357\",\"force\":true}", 400, "invalid_request", "an extra member"},
            {"{" + uuid_json + "," + record_json + "," + id_json + "}", 400, "invalid_request", "no removal_serial"},
            {"{" + uuid_json + ",\"removal_serial\":0," + id_json + "}", 400, "invalid_request", "no record"},
            {"{" + uuid_json + "," + record_json + ",\"removal_serial\":0}", 400, "invalid_request", "no request_id"},
            {"{" + record_json + ",\"removal_serial\":0," + id_json + "}", 400, "invalid_request", "no power_base_uuid, the rest given"},
            {"{" + uuid_json + "," + record_json + ",\"removal_serial\":0," + id_json + ",\"force\":true}", 400, "invalid_request",
             "the four members and one more"},
            {"{" + uuid_json + ",\"record\":7,\"removal_serial\":0," + id_json + "}", 400, "invalid_request", "a record that is no string"},
            {"{" + uuid_json + "," + record_json + ",\"removal_serial\":\"0\"," + id_json + "}", 400, "invalid_request",
             "a removal_serial that is no number"},
            {"{" + uuid_json + "," + record_json + ",\"removal_serial\":0,\"request_id\":7}", 400, "invalid_request",
             "a request_id that is no string"},
            {RemovalBody("744dbdfffe65d35", rec, 0, rid), 422, "invalid_power_base_uuid", "a 15-digit uuid"},
            {RemovalBody("744dbdfffe65d35g", rec, 0, rid), 422, "invalid_power_base_uuid", "a non-hex uuid"},
            {RemovalBody("0000000000000000", rec, 0, rid), 422, "invalid_power_base_uuid", "an all-zero uuid"},
            {RemovalBody("*", rec, 0, rid), 422, "invalid_power_base_uuid", "a wildcard"},
            {RemovalBody(kEui, std::string(31, 'a'), 0, rid), 422, "invalid_record", "a 31-digit record"},
            {RemovalBody(kEui, std::string(32, 'A'), 0, rid), 422, "invalid_record", "an upper-case record"},
            {"{" + uuid_json + "," + record_json + ",\"removal_serial\":-1," + id_json + "}", 422, "invalid_removal_serial",
             "a negative removal_serial"},
            {"{" + uuid_json + "," + record_json + ",\"removal_serial\":1.5," + id_json + "}", 422, "invalid_removal_serial",
             "a removal_serial with a fraction"},
            {"{" + uuid_json + "," + record_json + ",\"removal_serial\":4294967296," + id_json + "}", 422, "invalid_removal_serial",
             "a removal_serial beyond 32 bits"},
            {RemovalBody(kEui, rec, 0, std::string(32, 'g')), 422, "invalid_request_id", "a request id that is no hex"},
            {RemovalBody(kEui, rec, 0, rid), 404, "unknown_power_base", "a Base this hub has never heard of"},
            // What an app older than removal request version 2 sends: refused
            // as what it is, before anything is looked up.
            {LegacyRemovalBody(kEui), 422, "removal_registration_required", "the Base alone (the request of an older app)"},
        };
        for (const Case &c : cases) {
            HttpResponse r = f.Request("POST", kRemovals, token, &c.body);
            API_CHECK(r.status == c.status && JsonStringField(r.body, "error") == c.error, c.what);
        }
        const std::string big = "{\"power_base_uuid\":\"" + std::string(300, 'a') + "\"}";
        HttpResponse r = f.Request("POST", kRemovals, token, &big);
        API_CHECK(r.status == 413, "a body over 256 bytes is 413");
        r = f.Request("GET", std::string(kRemovals) + "/nope", token);
        API_CHECK(r.status == 404 && JsonStringField(r.body, "error") == "removal_not_found", "a malformed job id is 404");
        r = f.Request("GET", std::string(kRemovals) + "/" + std::string(32, 'a'), token);
        API_CHECK(r.status == 404, "an unknown job id is 404");
        r = f.Request("DELETE", std::string(kRemovals) + "/" + std::string(32, 'a'), token);
        API_CHECK(r.status == 404, "cancelling an unknown job is 404");

        // A Base known only from the data path.
        f.registry.apply_heartbeat(UuidOf(kEui), 1, 0);
        r = f.PostRemoval(token, kEui);
        API_CHECK(r.status == 409 && r.body == "{\"error\":\"power_base_management_unsupported\"}",
                  "a Base without a management credential: 409 power_base_management_unsupported");
        HttpResponse snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(snapshot.body.find("\"management\":\"unsupported\",\"record\":\"") != std::string::npos &&
                      snapshot.body.find("\"power_base_removals\":[],\"power_base_forgets\":[]") != std::string::npos,
                  "the snapshot says so too (with the record of that entry), and lists no removals");
        API_CHECK(f.management->ListRemovals().empty(), "no job was created by any of these");
    }

    printf("\n-- scenario: the management credential of an enrollment session --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts");
        std::string token = f.Pair("iPad");
        std::string other = f.Pair("iPhone");

        std::string body = "{\"eui64\":\"" + std::string(kEui) + "\",\"pskd\":\"" + kPskd + "\",\"management_version\":1}";
        HttpResponse r = f.Request("POST", kEnrollments, token, &body);
        API_CHECK(r.status == 400 && JsonStringField(r.body, "error") == "invalid_request",
                  "management_version without bridge_endpoint_version: 400");
        body = "{\"eui64\":\"" + std::string(kEui) + "\",\"pskd\":\"" + kPskd +
               "\",\"bridge_endpoint_version\":1,\"management_version\":2}";
        r = f.Request("POST", kEnrollments, token, &body);
        API_CHECK(r.status == 422 && JsonStringField(r.body, "error") == "unsupported_management_version",
                  "another management version: 422");
        body = "{\"eui64\":\"" + std::string(kEui) + "\",\"pskd\":\"" + kPskd +
               "\",\"bridge_endpoint_version\":1,\"management_version\":\"1\"}";
        r = f.Request("POST", kEnrollments, token, &body);
        API_CHECK(r.status == 400, "a management_version that is not a number: 400");
        body = ManagedStartBody(kEui);
        r = PlainHttpRequest(f.port(), HttpRequestText("POST", kEnrollments, token, &body));
        API_CHECK(r.status == 403 && r.body.find("key") == std::string::npos, "over plain HTTP: 403, nothing issued");
        API_CHECK(f.fake->ensure_calls.load() == 0 && !f.management->IsManaged(kEui), "none of these started anything");
        std::string state_file;
        API_CHECK(!test_support::ReadFile(f.state_dir + "/pogohome/" + kPowerBaseMgmtFileName, state_file) || state_file.empty(),
                  "and no credential was stored");

        r = f.Request("POST", kEnrollments, token, &body);
        const std::string session = JsonStringField(r.body, "session_id");
        const std::string generation = JsonStringField(r.body, "generation");
        const std::string key = JsonStringField(r.body, "key");
        API_CHECK(r.status == 202 && IsLowerHex(generation, 32) && IsLowerHex(key, 64) &&
                      JsonStringField(r.body, "hub_id") == kHubId &&
                      r.body.find("\"management\":{\"version\":1,\"generation\":\"") != std::string::npos,
                  "202: the session carries management {version, generation, hub_id, key}");
        API_CHECK(r.headers.find("Cache-Control: no-store") != std::string::npos, "with Cache-Control: no-store");
        API_CHECK(test_support::ReadFile(f.state_dir + "/pogohome/" + kPowerBaseMgmtFileName, state_file) &&
                      state_file.find(key) != std::string::npos &&
                      test_support::FileMode(f.state_dir + "/pogohome/" + kPowerBaseMgmtFileName) == 0600,
                  "the credential was stored (0600) before the answer left");

        HttpResponse get = f.Request("GET", std::string(kEnrollments) + "/" + session, token);
        API_CHECK(get.status == 200 && get.body.find(key) == std::string::npos && get.body.find("\"key\"") == std::string::npos &&
                      get.body.find("hub_id") == std::string::npos &&
                      get.body.find("\"management\":{\"version\":1,\"generation\":\"" + generation + "\"}") != std::string::npos,
                  "a later GET names the generation and never the key");
        get = f.Request("GET", std::string(kEnrollments) + "/" + session, other);
        API_CHECK(get.status == 404 && get.body.find(generation) == std::string::npos, "another paired device sees nothing of the session");
        r = f.Request("POST", kEnrollments, token, &body);
        API_CHECK(r.status == 409 && r.body.find("\"key\"") == std::string::npos && r.body.find(key) == std::string::npos,
                  "asking again while it runs: 409 with the running session, without any key");
        HttpResponse del = f.Request("DELETE", std::string(kEnrollments) + "/" + session, token);
        API_CHECK(del.status == 200 && del.body.find(key) == std::string::npos && del.body.find("\"key\"") == std::string::npos,
                  "the cancel answer carries no key either");
        HttpResponse snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        HttpResponse health = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/health"));
        API_CHECK(snapshot.body.find(key) == std::string::npos && health.body.find(key) == std::string::npos &&
                      snapshot.body.find(generation) == std::string::npos,
                  "nothing of the credential is in the unauthenticated snapshot or health");

        // A session without the option is what it was.
        for (int i = 0; i < 150 && f.enrollment->InProgressFor(kEui); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        body = "{\"eui64\":\"" + std::string(kOtherEui) + "\",\"pskd\":\"" + kPskd + "\",\"bridge_endpoint_version\":1}";
        r = f.Request("POST", kEnrollments, token, &body);
        API_CHECK(r.status == 202 && r.body.find("management") == std::string::npos,
                  "a session that does not ask has no management key at all in its JSON");
    }

    printf("\n-- scenario: a removal from request to the board's proof --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts");
        std::string token = f.Pair("iPad");
        std::string other = f.Pair("iPhone");
        Credential cred;
        API_CHECK(f.EnrollManaged(token, kEui, cred), "(a Base is enrolled with a management credential and polls once)");
        const ingest::Uuid base = UuidOf(kEui);
        const ingest::Uuid module{0x74, 0x4d, 0xbd, 0xff, 0xfe, 0x65, 0x00, 0x01};
        f.registry.apply_topology(base, module, base, true, 1, 3, 6, false, 1, 1);

        HttpResponse snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(snapshot.body.find(std::string("{\"uuid\":\"") + kEui +
                                     "\",\"online\":true,\"module_count\":1,\"online_module_count\":1,"
                                     "\"management\":\"supported\",\"erase_confirmation\":\"manual\",\"record\":\"" +
                                     cred.generation + "\",\"removal_serial\":0}") != std::string::npos,
                  "the snapshot lists it with \"management\":\"supported\" (and, its board having announced nothing, "
                  "a removal confirmed by hand), its registration and that no removal was asked of it yet");

        HttpResponse r = f.PostRemoval(token, kEui);
        const std::string id = JsonStringField(r.body, "removal_id");
        API_CHECK(r.status == 202 && IsLowerHex(id, 32) && JsonStringField(r.body, "state") == "pending" &&
                      r.body.find("\"cancellable\":true") != std::string::npos &&
                      r.body.find("\"result_unknown\":false") != std::string::npos &&
                      r.body.find("\"affected_module_count\":1") != std::string::npos &&
                      r.body.find("\"completed_at_unix_ms\":null") != std::string::npos &&
                      JsonStringField(r.body, "generation") == cred.generation,
                  "202: a pending, cancellable job bound to the Base and its generation");
        API_CHECK(r.headers.find(std::string("Location: ") + kRemovals + "/" + id) != std::string::npos &&
                      r.headers.find("Cache-Control: no-store") != std::string::npos,
                  "with its Location and Cache-Control: no-store");
        API_CHECK(r.body.find(cred.key) == std::string::npos && r.body.find("command") == std::string::npos,
                  "and neither the key nor a command id");
        r = f.PostRemoval(other, kEui);
        API_CHECK(r.status == 200 && JsonStringField(r.body, "removal_id") == id,
                  "the same Base from another device: 200 and the same job");
        r = f.PostRemoval(token, "744DBDFFFE65D357");
        API_CHECK(r.status == 200 && JsonStringField(r.body, "removal_id") == id, "upper-case hex names the same Base");
        r = f.Request("GET", kRemovals, other);
        API_CHECK(r.status == 200 && r.body.find("{\"removals\":[{\"removal_id\":\"" + id) == 0,
                  "every paired device reads the same list");
        snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(snapshot.body.find("\"power_base_removals\":[{\"removal_id\":\"" + id) != std::string::npos &&
                      snapshot.body.find(kEui) != std::string::npos && snapshot.body.find("744dbdfffe650001") != std::string::npos,
                  "the snapshot shows the job; the Base and its module are still listed while it waits");

        // Cancel, by the other device; then again; then a new request.
        r = f.Request("DELETE", std::string(kRemovals) + "/" + id, other);
        API_CHECK(r.status == 200 && JsonStringField(r.body, "state") == "cancelled", "a pending job can be cancelled (by any paired device)");
        r = f.Request("DELETE", std::string(kRemovals) + "/" + id, token);
        API_CHECK(r.status == 409 && JsonStringField(r.body, "error") == "removal_not_cancellable" &&
                      r.body.find("\"removal\":{\"removal_id\":\"" + id) != std::string::npos,
                  "cancelling it again: 409 removal_not_cancellable with the job");
        r = f.PostRemoval(token, kEui);
        const std::string id2 = JsonStringField(r.body, "removal_id");
        API_CHECK(r.status == 202 && id2 != id, "a new request is a new job");

        // The board carries it out: its "released" notice arrives.
        std::vector<uint8_t> proof;
        std::array<uint8_t, 16> command{};
        API_CHECK(f.BoardUnenrolls(cred, &proof, &command) == ingest::kCoapCodeContent,
                  "(the board polls, gets the command, sends its released notice)");
        r = f.Request("GET", std::string(kRemovals) + "/" + id2, token);
        API_CHECK(JsonStringField(r.body, "state") == "released" && r.body.find("\"completed_at_unix_ms\":null") != std::string::npos &&
                      r.body.find("\"cancellable\":false") != std::string::npos &&
                      r.body.find("\"result_unknown\":false") != std::string::npos &&
                      r.body.find("\"settings_cleanup_pending\":") != std::string::npos,
                  "the job is 'released' -- not completed: the board's stored network is not confirmed erased");
        r = f.Request("DELETE", std::string(kRemovals) + "/" + id2, token);
        API_CHECK(r.status == 409, "a released job cannot be cancelled");
        snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(snapshot.body.find(std::string("\"uuid\":\"") + kEui) == std::string::npos &&
                      snapshot.body.find("744dbdfffe650001") == std::string::npos &&
                      snapshot.body.find("\"power_bases\":[]") != std::string::npos,
                  "the Base and the module registered under it are gone from the snapshot");
        API_CHECK(snapshot.body.find("\"state\":\"released\"") != std::string::npos &&
                      snapshot.body.find("\"state\":\"completed\"") == std::string::npos,
                  "the job is still in the snapshot, as 'released': it does not vanish and it is not called completed");
        // Late data does not bring it back.
        API_CHECK(f.registry.apply_heartbeat(base, 2, 0) == ingest::ApplyResult::Blocked &&
                      f.registry.apply_topology(base, module, base, true, 1, 3, 6, false, 1, 2) == ingest::ApplyResult::Blocked,
                  "a late heartbeat and topology report are refused");
        snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(snapshot.body.find(std::string("\"uuid\":\"") + kEui) == std::string::npos, "and the snapshot stays without it");
        r = f.PostRemoval(token, kEui);
        API_CHECK(r.status == 200 && JsonStringField(r.body, "removal_id") == id2 && JsonStringField(r.body, "state") == "released",
                  "asking to remove it again: the same job, still 'released'");

        // The receipt route: the only way to 'completed'.
        const std::string receipt_path = std::string(kRemovals) + "/receipt";
        const std::vector<uint8_t> receipt = f.Receipt(cred, command);
        std::string receipt_body = Fixture::ReceiptBody(receipt);
        r = f.Request("POST", receipt_path, "", &receipt_body);
        API_CHECK(r.status == 401, "a receipt without a paired device's token: 401");
        r = PlainHttpRequest(f.port(), HttpRequestText("POST", receipt_path, "", &receipt_body));
        API_CHECK(r.status == 403 && JsonStringField(r.body, "error") == "tls_required", "over plain HTTP: 403 tls_required");
        r = f.Request("GET", receipt_path, token);
        API_CHECK(r.status == 405, "GET on the receipt route: 405");
        for (const std::string &bad : {std::string("{}"), std::string("{\"receipt\":5}"), std::string("[]"),
                                       std::string("{\"receipt\":\"") + LowerHex(receipt.data(), receipt.size()) + "\",\"x\":1}"}) {
            r = f.Request("POST", receipt_path, token, &bad);
            API_CHECK(r.status == 400 && JsonStringField(r.body, "error") == "invalid_request", "a body that is not exactly {\"receipt\":…}: 400");
        }
        std::string short_body = "{\"receipt\":\"0103\"}";
        r = f.Request("POST", receipt_path, token, &short_body);
        API_CHECK(r.status == 422 && JsonStringField(r.body, "error") == "invalid_receipt", "not 192 lower-case hex digits: 422 invalid_receipt");
        std::string upper = receipt_body;
        for (char &c : upper) {
            if (c >= 'a' && c <= 'f') {
                c = static_cast<char>(c - 32);
            }
        }
        upper.replace(2, 7, "receipt");
        r = f.Request("POST", receipt_path, token, &upper);
        API_CHECK(r.status == 422, "upper-case hex: 422");
        r = f.PostReceipt(token, proof);
        API_CHECK(r.status == 422 && JsonStringField(r.body, "error") == "invalid_receipt" && f.RemovalState(token, id2) == "released",
                  "the board's released NOTICE handed in as a receipt: 422, the job stays 'released'");
        std::vector<uint8_t> forged = receipt;
        forged.back() ^= 0x01;
        r = f.PostReceipt(token, forged);
        API_CHECK(r.status == 409 && r.body == "{\"error\":\"receipt_rejected\"}" && f.RemovalState(token, id2) == "released",
                  "a receipt whose MAC does not verify: 409 receipt_rejected, nothing changes");
        r = f.PostReceipt(other, receipt);
        API_CHECK(r.status == 200 && JsonStringField(r.body, "removal_id") == id2 && JsonStringField(r.body, "state") == "completed" &&
                      r.body.find("\"completed_at_unix_ms\":null") == std::string::npos &&
                      r.headers.find("Cache-Control: no-store") != std::string::npos && r.body.find(cred.key) == std::string::npos,
                  "the genuine receipt, relayed by any paired device: 200 and the job, completed");
        r = f.PostReceipt(token, receipt);
        API_CHECK(r.status == 200 && JsonStringField(r.body, "state") == "completed", "the same receipt again: the same answer");
        API_CHECK(f.RemovalState(token, id2) == "completed" && f.registry.is_power_base_blocked(base),
                  "the job is completed; the Base stays blocked");
        snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(snapshot.body.find("\"state\":\"completed\"") != std::string::npos,
                  "the finished job is still listed for a while, so every device can see how it ended");
        r = f.PostRemoval(token, kEui);
        API_CHECK(r.status == 404 && JsonStringField(r.body, "error") == "unknown_power_base", "the removed Base is unknown now");

        // Enrolling it again: only with a management credential.
        std::string body = "{\"eui64\":\"" + std::string(kEui) + "\",\"pskd\":\"" + kPskd + "\",\"bridge_endpoint_version\":1}";
        r = f.Request("POST", kEnrollments, token, &body);
        API_CHECK(r.status == 409 && r.body == "{\"error\":\"management_required\"}" && f.fake->add_calls.load() == 1,
                  "a session without a credential for the removed Base: 409 management_required, no Joiner opened");
        Credential renewed;
        API_CHECK(f.EnrollManaged(token, kEui, renewed) && renewed.generation != cred.generation && renewed.key != cred.key,
                  "a session with one issues a new generation and key; its authenticated poll lifts the block");
        snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(snapshot.body.find(std::string("{\"uuid\":\"") + kEui) != std::string::npos, "the Base is listed again");
        API_CHECK(f.management->HandleDatagram(proof).code == ingest::kCoapCodeUnauthorized &&
                      !f.registry.is_power_base_blocked(base),
                  "the previous enrollment's notice, played again, does nothing to the new enrollment");
        r = f.PostReceipt(token, f.Receipt(renewed, command));
        API_CHECK(r.status == 409 && !f.registry.is_power_base_blocked(base) && f.management->IsManaged(kEui),
                  "a 'receipt' for the new enrollment naming the old command: rejected, the Base stays enrolled");
    }

    printf("\n-- scenario: removal and enrollment of the same Base at once; two devices at once --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts");
        std::string token = f.Pair("iPad");
        std::string other = f.Pair("iPhone");
        Credential cred;
        API_CHECK(f.EnrollManaged(token, kEui, cred), "(a managed Base)");
        // The Base goes silent and an enrollment session for it starts (the
        // only way one can: it is not online).
        f.registry.remove_power_base(UuidOf(kEui));
        const std::string body = ManagedStartBody(kEui);
        HttpResponse r = f.Request("POST", kEnrollments, token, &body);
        const std::string session = JsonStringField(r.body, "session_id");
        API_CHECK(r.status == 202, "an enrollment session for that EUI-64 is running");
        r = f.PostRemoval(other, kEui);
        API_CHECK(r.status == 409 && r.body == "{\"error\":\"enrollment_in_progress\"}" && f.management->ListRemovals().empty(),
                  "a removal of it meanwhile: 409 enrollment_in_progress, no job");
        f.Request("DELETE", std::string(kEnrollments) + "/" + session, token);
        for (int i = 0; i < 150 && f.enrollment->InProgressFor(kEui); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        // Two devices ask for the same removal at the same moment.
        std::string id_a, id_b;
        int status_a = 0, status_b = 0;
        std::thread a([&]() {
            HttpResponse ra = f.PostRemoval(token, kEui);
            status_a = ra.status;
            id_a = JsonStringField(ra.body, "removal_id");
        });
        std::thread b([&]() {
            HttpResponse rb = f.PostRemoval(other, kEui);
            status_b = rb.status;
            id_b = JsonStringField(rb.body, "removal_id");
        });
        a.join();
        b.join();
        API_CHECK(!id_a.empty() && id_a == id_b && f.management->ListRemovals().size() == 1 &&
                      ((status_a == 202 && status_b == 200) || (status_a == 200 && status_b == 202)),
                  "two devices at once: one job; one gets 202, the other 200 with the same id");

        // Another Base is not held up by any of this.
        Credential second;
        API_CHECK(f.EnrollManaged(other, kOtherEui, second), "another Base is enrolled while the first one's removal waits");
        r = f.PostRemoval(other, kOtherEui);
        API_CHECK(r.status == 202 && JsonStringField(r.body, "removal_id") != id_a, "and can be removed independently");
        API_CHECK(f.BoardUnenrolls(second) == ingest::kCoapCodeContent && f.RemovalState(token, id_a) == "pending",
                  "its release leaves the first Base's job untouched");
    }

    printf("\n-- scenario: requested, but the state file is damaged --\n");
    {
        Fixture f;
        API_CHECK(f.Start(true, true), "the server starts");
        std::string token = f.Pair("iPad");
        HttpResponse r = f.PostRemoval(token, kEui);
        API_CHECK(r.status == 503 && JsonStringField(r.body, "error") == "removal_unavailable", "removal: 503 removal_unavailable");
        r = f.Request("GET", kRemovals, token);
        API_CHECK(r.status == 503, "the list too");
        HttpResponse health = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/health"));
        API_CHECK(health.body.find("\"power_base_management\":1,\"power_base_removal\":\"unavailable\","
                                   "\"power_base_forget\":1,\"power_base_removal_request\":2}") != std::string::npos,
                  "health says unavailable");
        f.registry.apply_heartbeat(UuidOf(kOtherEui), 1, 0);
        HttpResponse snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(snapshot.status == 200 && snapshot.body.find("\"management\"") == std::string::npos,
                  "the sensor snapshot is served as always, without management fields");
        std::string body = ManagedStartBody(kEui);
        r = f.Request("POST", kEnrollments, token, &body);
        API_CHECK(r.status == 503 && r.body.find("\"reason\":\"management\"") != std::string::npos,
                  "an enrollment that needs a credential: 503");
        body = "{\"eui64\":\"" + std::string(kEui) + "\",\"pskd\":\"" + kPskd + "\",\"bridge_endpoint_version\":1}";
        r = f.Request("POST", kEnrollments, token, &body);
        API_CHECK(r.status == 202, "an enrollment without one still works");
        std::string text;
        API_CHECK(test_support::ReadFile(f.state_dir + "/pogohome/" + kPowerBaseMgmtFileName, text) && text == "not json",
                  "and the damaged file was not replaced");
    }

    printf("\n-- scenario: the block list is full -- the request is refused, nothing is forgotten --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts");
        std::string token = f.Pair("iPad");
        auto eui_of = [](unsigned i) {
            char text[17];
            std::snprintf(text, sizeof(text), "744dbdfffe02%04x", i);
            return std::string(text);
        };
        // An enrolled, active Power Base, straight through the manager (the
        // enrollment route is exercised above).
        auto enroll = [&](unsigned i, Credential &c) {
            ManagementGrant grant;
            if (!f.management->IssueCredential(eui_of(i), grant)) {
                return false;
            }
            c.eui = eui_of(i);
            c.hub_id = grant.hub_id;
            c.generation = grant.generation;
            c.key = grant.key;
            if (f.management->HandleDatagram(Poll(c)).code != ingest::kCoapCodeContent) {
                return false;
            }
            f.registry.apply_heartbeat(UuidOf(c.eui), 1, 0);
            return true;
        };
        std::vector<Credential> removed(kMaxBlockedPowerBases);
        bool all_ok = true;
        for (unsigned i = 0; i < kMaxBlockedPowerBases; i++) {
            all_ok = all_ok && enroll(i + 1, removed[i]) && f.PostRemoval(token, removed[i].eui).status == 202 &&
                     f.BoardUnenrolls(removed[i]) == ingest::kCoapCodeContent;
        }
        API_CHECK(all_ok && f.management->FreeBlockSlots() == 0, "(64 Power Bases enrolled, removed over the API and released)");

        Credential next;
        API_CHECK(enroll(100, next), "(one more Power Base is enrolled)");
        const size_t jobs_before = f.management->ListRemovals().size();
        HttpResponse r = f.PostRemoval(token, next.eui);
        API_CHECK(r.status == 409 && r.body == "{\"error\":\"removal_capacity_full\",\"limit\":\"64\"}",
                  "POST for a 65th: 409 removal_capacity_full with the limit");
        API_CHECK(r.headers.find("Location:") == std::string::npos && f.management->ListRemovals().size() == jobs_before,
                  "no job was created");
        ingest::ManagementReply idle = f.management->HandleDatagram(Poll(next));
        pb_mgmt_msg_t a;
        bool no_command = pb_mgmt_parse(idle.payload.data(), idle.payload.size(), &a) && a.type == PB_MGMT_TYPE_IDLE;
        if (no_command) {
            ingest::ManagementReply second = f.management->HandleDatagram(Poll(next, a.hub_nonce));
            pb_mgmt_msg_t b;
            no_command = pb_mgmt_parse(second.payload.data(), second.payload.size(), &b) && b.type == PB_MGMT_TYPE_IDLE;
        }
        API_CHECK(no_command, "and the board is given no command: it stays enrolled");
        HttpResponse snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        const std::string first_base = "\"uuid\":\"" + removed[0].eui + "\"";
        const std::string next_base = "\"uuid\":\"" + next.eui + "\"";
        API_CHECK(snapshot.body.find(next_base) != std::string::npos && snapshot.body.find(first_base) == std::string::npos,
                  "the snapshot still lists it as a Power Base, and still does not list the first removed Base as one");
        API_CHECK(f.management->IsBlocked(removed[0].eui) && f.registry.is_power_base_blocked(UuidOf(removed[0].eui)),
                  "the first removed Base is still blocked: nothing was dropped to make room");
        r = f.PostRemoval(token, next.eui);
        API_CHECK(r.status == 409 && JsonStringField(r.body, "error") == "removal_capacity_full",
                  "asking again changes nothing");

        // The only thing that frees a slot: a removed board enrolled again.
        Credential again;
        API_CHECK(enroll(1, again) && !f.management->IsBlocked(removed[0].eui) && f.management->FreeBlockSlots() == 1,
                  "the first removed board is enrolled again (a new authenticated generation): its block ends");
        r = f.PostRemoval(token, next.eui);
        API_CHECK(r.status == 202 && JsonStringField(r.body, "state") == "pending" && f.management->FreeBlockSlots() == 0,
                  "now the removal is accepted, and it holds the freed slot");
    }

    printf("\n-- scenario: a board that sends its receipt itself, as the API reports it --\n");
    {
        Fixture f;
        API_CHECK(f.Start(), "the server starts");
        std::string token = f.Pair("iPad");
        Credential cred;
        API_CHECK(f.EnrollManaged(token, kEui, cred), "(a Base is enrolled with a management credential and polls once)");
        pb_mgmt_msg_t hello;
        std::memset(&hello, 0, sizeof(hello));
        hello.type = PB_MGMT_TYPE_HELLO;
        hello.capabilities = PB_MGMT_BOARD_CAP_THREAD_RECEIPT;
        API_CHECK(f.management->HandleDatagram(Pack(hello, cred)).code == ingest::kCoapCodeContent,
                  "(its board announces that it sends its release receipt itself)");
        HttpResponse snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(snapshot.body.find("\"management\":\"supported\",\"erase_confirmation\":\"automatic\",\"record\":\"" +
                                     cred.generation + "\",\"removal_serial\":0}") != std::string::npos,
                  "the snapshot says of that Base: a removal's erase is confirmed automatically");

        std::string asked_with;
        HttpResponse r = f.PostRemoval(token, kEui, &asked_with);
        const std::string id = JsonStringField(r.body, "removal_id");
        API_CHECK(r.status == 202 && JsonStringField(r.body, "state") == "pending" &&
                      JsonStringField(r.body, "erase_confirmation") == "automatic",
                  "202: the pending job says the same");
        std::array<uint8_t, 16> command{};
        API_CHECK(f.BoardUnenrolls(cred, nullptr, &command) == ingest::kCoapCodeContent,
                  "(the board polls, gets the command, sends its released notice)");
        r = f.Request("GET", std::string(kRemovals) + "/" + id, token);
        API_CHECK(JsonStringField(r.body, "state") == "released" && JsonStringField(r.body, "erase_confirmation") == "automatic" &&
                      r.body.find("\"completed_at_unix_ms\":null") != std::string::npos,
                  "released: not completed, and nothing for anyone to do -- the receipt is expected to come by itself");
        snapshot = PlainHttpRequest(f.port(), HttpRequestText("GET", "/api/v1/snapshot"));
        API_CHECK(snapshot.body.find("\"state\":\"released\"") != std::string::npos &&
                      snapshot.body.find("\"erase_confirmation\":\"automatic\",\"request_id\":\"" + asked_with + "\"}]") !=
                          std::string::npos,
                  "the snapshot's job carries the same field (and, last, the id of the request that made it)");

        // The receipt arrives on the management resource: no app relayed it.
        ingest::ManagementReply ack = f.management->HandleDatagram(f.Receipt(cred, command));
        pb_mgmt_msg_t a;
        API_CHECK(ack.code == ingest::kCoapCodeContent && pb_mgmt_parse(ack.payload.data(), ack.payload.size(), &a) &&
                      a.type == PB_MGMT_TYPE_RESULT_ACK && a.acked_type == PB_MGMT_TYPE_UNENROLL_DONE &&
                      a.acked_result == PB_MGMT_RESULT_ERASED,
                  "(the board's receipt arrives over Thread and is acknowledged)");
        r = f.Request("GET", std::string(kRemovals) + "/" + id, token);
        API_CHECK(JsonStringField(r.body, "state") == "completed" && r.body.find("erase_confirmation") == std::string::npos &&
                      r.body.find("\"completed_at_unix_ms\":null") == std::string::npos,
                  "the job is completed, without any request from an app; a final job has no erase_confirmation");
        r = f.PostReceipt(token, f.Receipt(cred, command));
        API_CHECK(r.status == 200 && JsonStringField(r.body, "state") == "completed",
                  "an app that relays the same receipt afterwards gets the same completed job");
    }

    ForgetApiTests(failures);
    SnapshotOrderTests(failures);
    DepartureSnapshotTests(failures);
    RemovalBindingApiTests(failures);
    SnapshotSequenceTests(failures);
}
