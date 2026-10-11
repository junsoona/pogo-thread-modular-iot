// Power Base management on the hub (docs/power-base-management.md), tested
// with the REAL pogohome_api::PowerBaseManagement, the real ingest::Registry
// and ingest::handle_request(), and -- as the board -- the REAL firmware code:
// components/power_base_mgmt/pb_mgmt_client.c and
// components/power_base_enrollment/power_base_enrollment.c, compiled into
// this test with the functional NVS simulator. Every byte that travels
// between the two is produced and checked by production code on both sides;
// the HMAC is OpenSSL's on both (the firmware uses PSA on the board).
//
// What this does not show: the CoAP transport itself (covered by the ingest
// loopback test), a real board, real flash, a real Thread network.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include "ingest/minicoap.hpp"
#include "ingest/registry.hpp"
#include "ingest/session.hpp"
#include "pogohome_api/atomic_file.hpp"
#include "pogohome_api/crypto_util.hpp"
#include "pogohome_api/hex.hpp"
#include "pogohome_api/json_snapshot.hpp"
#include "pogohome_api/power_base_management.hpp"
#include "pogohome_api/settings_store.hpp"
#include "test_check.hpp"
#include "test_temp_dir.hpp"

extern "C" {
#include "bridge_export_proto/bridge_export_proto.h"
#include "sensor_channel/sensor_channel.h"
#include "nvs.h"
#include "nvs_sim.h"
#include "power_base_enrollment/power_base_enrollment.h"
#include "power_base_mgmt/pb_mgmt_client.h"
#include "power_base_mgmt/pb_mgmt_proto.h"
}

using namespace pogohome_api;

namespace {

const char kHubId[] = "101112131415161718191a1b1c1d1e1f";
const char kEuiA[] = "6055f9fffe001234";
const char kEuiB[] = "6055f9fffe005678";

int64_t g_now_ms = 1'790'000'000'000;

bool Hmac(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[32], void *)
{
    unsigned int n = 0;
    return HMAC(EVP_sha256(), key, static_cast<int>(key_len), data, len, out, &n) != nullptr && n == 32;
}

std::string Hex(const uint8_t *p, size_t n)
{
    return LowerHex(p, n);
}

template <size_t N> std::array<uint8_t, N> Bytes(const std::string &hex)
{
    std::array<uint8_t, N> out{};
    for (size_t i = 0; i < N && 2 * i + 1 < hex.size(); i++) {
        out[i] = static_cast<uint8_t>(std::stoi(hex.substr(2 * i, 2), nullptr, 16));
    }
    return out;
}

ingest::Uuid Uuid(const std::string &hex)
{
    return Bytes<8>(hex);
}

ingest::Uuid Module(uint8_t last)
{
    return ingest::Uuid{0x74, 0x4d, 0xbd, 0xff, 0xfe, 0x65, 0x00, last};
}

// One module of a clean-up: with the room it is bound to, or (no room given)
// with a room that is not known.
RoomCleanupTarget Target(const std::string &module_uuid, const std::string &room_id = std::string())
{
    RoomCleanupTarget target;
    target.module_uuid = module_uuid;
    target.room_known = !room_id.empty();
    target.room_id = room_id;
    return target;
}

// ---- a removal request, as a device makes it ----

// A request id nobody used before: one per confirmation.
std::string NextRemovalRequestId()
{
    static unsigned counter = 0;
    char id[33];
    std::snprintf(id, sizeof(id), "52454d4f%024x", ++counter);
    return id;
}

// What a device does that reads the hub's list at this moment and confirms
// the removal of what it is shown: the request names the registration and the
// removal serial listed now, and carries an id of its own. For a Base this
// hub cannot remove (it lists none, or holds no credential) the device still
// has the registration of a removal that is under way on its screen, if there
// is one; otherwise it names a registration that is none.
RemovalRequestOutcome RequestRemovalNow(PowerBaseManagement &management, const std::string &device,
                                        const std::string &eui, RemovalJob &out, std::string *request_id = nullptr)
{
    std::string record;
    uint32_t serial = 0;
    if (!management.RemovalBinding(eui, record, serial)) {
        record = std::string(32, 'e');
        for (const RemovalJob &job : management.ListRemovals()) {
            if (job.power_base_uuid == eui && !IsFinalRemovalState(job.state)) {
                record = job.generation;
            }
        }
    }
    const std::string id = NextRemovalRequestId();
    if (request_id != nullptr) {
        *request_id = id;
    }
    return management.RequestRemoval(device, eui, record, serial, id, out);
}

// ---- the hub under test ----

struct Hub {
    std::string dir;
    ingest::Registry registry;
    std::unique_ptr<PowerBaseManagement> management;
    // Every call of the settings clean-up hook, with the modules it was given.
    std::vector<std::vector<std::string>> removed_modules;
    // The same calls as they were given: each module with the room the
    // clean-up is bound to ("" when it is not known).
    std::vector<std::vector<RoomCleanupTarget>> removed_targets;
    // And what such a call would really clear: the modules the section the
    // hook is handed (RoomCleanupGuard::commit()) lets a write have -- asked
    // the way the production hook asks it, after `on_unassign` ran.
    std::vector<std::vector<std::string>> cleared_modules;
    // What the hook answers: false = "could not be written now".
    bool unassign_ok = true;
    // Called inside the hook, before it asks and answers (to change the world
    // while a clean-up is under way).
    std::function<void()> on_unassign;
    // The room each module is in when its Base leaves the list (the reader
    // hook). Unset in most scenarios: the rooms are then not known. Set: only
    // the modules it names have a room.
    std::function<bool(const std::vector<std::string> &, std::vector<RoomCleanupTarget> &)> read_rooms;
    bool enrolling = false;
    bool fail_writes = false;
    // The next N state-file writes fail, then writes work again.
    int fail_next_writes = 0;
    // Every attempt to write the state file, failed or not.
    int writes_tried = 0;
    // False: the hub is not told the room of a module when its Base leaves
    // the list (a clean-up as a hub before 0.4.1 stored it).
    bool bind_rooms = true;
    // After-restart grace of the clean-ups (0 in most scenarios).
    std::chrono::milliseconds cleanup_grace{0};
    // Where this hub's ingest listener is (EndpointLocate()). Unset in every
    // other scenario: that hub does not answer LOCATE and does not say it can.
    std::function<bool(std::array<uint8_t, 16> &, uint16_t &)> ingest_endpoint;
    // Added to the moment "is this Base heard from?" is asked for
    // (ListRemoval()): ten minutes on, nothing sent so far counts as heard.
    std::chrono::seconds steady_shift{0};
    std::string hub_id = kHubId;
    bool owns_dir = true;
    // The real shared settings. Set: the clean-up hooks are the PRODUCTION
    // ones around this store (MakeRoomUnassigner() and MakeRoomReader()).
    // `on_unassign` then runs before the production hook does anything, and
    // `before_settings_write` between its read of the settings and its write
    // (with it the hook is UnassignModuleRooms() itself, which has that seam).
    std::shared_ptr<SettingsStore> settings;
    std::function<void()> before_settings_write;

    Hub() { dir = test_support::MakeTempDir(); }
    // The next process over the same directory: a restart as it really is,
    // with a new, empty Registry.
    explicit Hub(const Hub &previous_process) : dir(previous_process.dir), hub_id(previous_process.hub_id), owns_dir(false) {}
    ~Hub()
    {
        if (owns_dir) {
            test_support::RemoveTree(dir);
        }
    }

    // A new manager over the same directory and Registry: what a restart of
    // the bridge process does (the Registry is new there too; see Restart()).
    bool Open()
    {
        PowerBaseManagementConfig config;
        config.directory = dir;
        config.hub_id = hub_id;
        config.now_unix_ms = []() { return g_now_ms; };
        config.steady_now = [this]() { return std::chrono::steady_clock::now() + steady_shift; };
        config.enrollment_in_progress = [this](const std::string &) { return enrolling; };
        config.unassign_rooms = [this](const std::vector<RoomCleanupTarget> &targets, const RoomCleanupGuard &still_owed) {
            std::vector<std::string> given;
            for (const RoomCleanupTarget &t : targets) {
                given.push_back(t.module_uuid);
            }
            removed_modules.push_back(given);
            removed_targets.push_back(targets);
            if (on_unassign) {
                on_unassign();
            }
            std::vector<std::string> cleared;
            still_owed.commit(given, [&](const std::vector<std::string> &owed) { cleared = owed; });
            cleared_modules.push_back(cleared);
            return unassign_ok;
        };
        config.read_rooms = read_rooms;
        if (settings) {
            auto real = MakeRoomUnassigner(settings, []() { return g_now_ms; });
            config.unassign_rooms = [this, real](const std::vector<RoomCleanupTarget> &targets,
                                                 const RoomCleanupGuard &still_owed) {
                std::vector<std::string> given;
                for (const RoomCleanupTarget &t : targets) {
                    given.push_back(t.module_uuid);
                }
                removed_modules.push_back(given);
                removed_targets.push_back(targets);
                if (on_unassign) {
                    on_unassign();
                }
                if (!before_settings_write) {
                    return real(targets, still_owed);
                }
                const RoomUnassignOutcome outcome =
                    UnassignModuleRooms(*settings, targets, g_now_ms, still_owed, before_settings_write);
                return outcome == RoomUnassignOutcome::Saved || outcome == RoomUnassignOutcome::Nothing;
            };
            config.read_rooms = MakeRoomReader(settings);
        }
        if (!bind_rooms) {
            config.read_rooms = nullptr;
        }
        config.cleanup_startup_grace = cleanup_grace;
        config.ingest_endpoint = ingest_endpoint;
        config.cleanup_retry_min = std::chrono::milliseconds(1000);
        config.cleanup_retry_max = std::chrono::milliseconds(8000);
        config.writer = [this](const std::string &path, const std::string &contents, std::string &error) {
            writes_tried++;
            if (fail_writes || fail_next_writes > 0) {
                if (fail_next_writes > 0) {
                    fail_next_writes--;
                }
                error = "injected write failure";
                return false;
            }
            return WriteFileAtomically(path, contents, error);
        };
        management = std::make_unique<PowerBaseManagement>(registry, std::move(config));
        std::string error;
        return management->Load(error);
    }

    std::string StateFile() const
    {
        std::string text;
        test_support::ReadFile(dir + "/" + kPowerBaseMgmtFileName, text);
        return text;
    }
    // One list of the state file, as it is written: "cleanups" (what is owed
    // a clean-up) or "members" (where every module was seen last).
    std::string StateList(const std::string &name) const
    {
        const std::string text = StateFile();
        const size_t from = text.find("\"" + name + "\":[");
        if (from == std::string::npos) {
            return std::string();
        }
        // To the bracket that closes it (no string of this file holds one).
        int depth = 0;
        for (size_t i = text.find('[', from); i < text.size(); i++) {
            depth += text[i] == '[' ? 1 : (text[i] == ']' ? -1 : 0);
            if (depth == 0) {
                return text.substr(from, i - from + 1);
            }
        }
        return std::string();
    }
};

// ---- the board: the real firmware client over the NVS simulator ----

struct Board;
Board *g_board = nullptr;

struct Board {
    Hub *hub = nullptr;
    std::string eui_hex;
    ingest::Uuid eui{};
    pb_mgmt_client_t client{};
    // What happened, in order: P poll, H hello, A accepted, D the "released"
    // notice, Z the receipt put on the wire (never by a board without the
    // delivery session), Q quiesce, E erase, V the store read back after an
    // erase, S / s the delivery session up / down, v the store read back
    // inside that session (before a transmission of the receipt).
    std::string events;
    // The firmware's delivery session (pb_mgmt_client.h): off = a board that
    // hands its receipt out over BLE only, like every scenario before
    // ThreadReceipt().
    bool session = false;
    bool ram_dataset = false;    // what the platform kept in RAM across the erase
    bool session_attaches = true;
    bool in_session = false;
    bool session_stores_again = false; // the session's attach writes the Dataset to the store
    bool link_up = true;        // false: every request is lost
    bool drop_answers = false;  // the hub gets the request, the board not the answer
    bool dataset = true;        // OpenThread's persistent store holds a Dataset
    bool erase_fails = false;   // the erase returns an error
    bool erase_lies = false;    // the erase returns ESP_OK and erases nothing
    std::vector<std::vector<uint8_t>> sent;      // every request put on the wire
    std::vector<std::vector<uint8_t>> answers;   // every 2.xx answer payload
    std::vector<uint8_t> last_code;

    // EndpointLocate() only. The Thread network between this board and its
    // hub: which of the hub's sockets a request to `to` reaches. 0 = nothing
    // answers there, 1 = the ingest socket, 2 = the locate-only socket.
    // Unset: every request reaches the ingest socket, wherever it was sent.
    std::function<int(const pb_enroll_bridge_endpoint_t &to)> route;
    // The firmware platform's locate operations (off: a board that never
    // looks for its hub, like every scenario before EndpointLocate()).
    bool locate = false;
    std::vector<std::array<uint8_t, 16>> hints;
    int moved = 0;
    pb_enroll_bridge_endpoint_t moved_to{};
    std::vector<pb_enroll_bridge_endpoint_t> sent_to; // where each request went

    static pb_mgmt_xfer_t Exchange(const pb_enroll_bridge_endpoint_t *to, const uint8_t *req, size_t req_len, uint8_t *resp,
                                   size_t resp_cap, size_t *resp_len, void *)
    {
        Board &b = *g_board;
        *resp_len = 0;
        std::vector<uint8_t> request(req, req + req_len);
        b.sent.push_back(request);
        b.sent_to.push_back(*to);
        b.events += req[1] == PB_MGMT_TYPE_POLL
                        ? 'P'
                        : req[1] == PB_MGMT_TYPE_HELLO
                              ? 'H'
                              : req[1] == PB_MGMT_TYPE_LOCATE
                                    ? 'L'
                                    : (req[1] == PB_MGMT_TYPE_UNENROLL_ACCEPTED
                                           ? 'A'
                                           : (req[PB_MGMT_HEADER_LEN + 16] == PB_MGMT_RESULT_ERASED ? 'Z' : 'D'));
        if (!b.link_up) {
            return PB_MGMT_XFER_NO_ANSWER;
        }
        const int socket = b.route ? b.route(*to) : 1;
        if (socket == 0) {
            return PB_MGMT_XFER_NO_ANSWER;
        }
        if (socket == 2) {
            // The locate-only socket, as server.cpp serves it: the handler's
            // reply, or nothing at all when it is `silent`.
            ingest::ManagementReply reply = b.hub->management->HandleLocateDatagram(request);
            if (reply.silent) {
                return PB_MGMT_XFER_NO_ANSWER;
            }
            b.last_code.push_back(reply.code);
            const uint8_t reply_class = static_cast<uint8_t>(reply.code >> 5);
            if (reply_class != 2) {
                return reply_class == 4 ? PB_MGMT_XFER_REJECTED : PB_MGMT_XFER_NO_ANSWER;
            }
            b.answers.push_back(reply.payload);
            std::memcpy(resp, reply.payload.data(), reply.payload.size());
            *resp_len = reply.payload.size();
            return PB_MGMT_XFER_ANSWERED;
        }
        // Through the real ingest dispatch, as server.cpp does.
        ingest::CoapMessage coap;
        coap.code = ingest::kCoapCodePost;
        coap.uri_path = ingest::kManagementUriPath;
        coap.payload = request;
        ingest::ManagementHandler handler = [&b](const std::vector<uint8_t> &r) {
            return b.hub->management->HandleDatagram(r);
        };
        ingest::Response response = ingest::handle_request(b.hub->registry, coap, handler);
        b.last_code.push_back(response.code);
        if (b.drop_answers) {
            return PB_MGMT_XFER_NO_ANSWER;
        }
        const uint8_t cls = static_cast<uint8_t>(response.code >> 5);
        if (cls == 4) {
            return PB_MGMT_XFER_REJECTED;
        }
        if (cls != 2) {
            return PB_MGMT_XFER_NO_ANSWER;
        }
        b.answers.push_back(response.payload);
        if (response.payload.size() <= resp_cap) {
            std::memcpy(resp, response.payload.data(), response.payload.size());
            *resp_len = response.payload.size();
        }
        return PB_MGMT_XFER_ANSWERED;
    }
    static bool Random(uint8_t *buf, size_t len, void *) { return RAND_bytes(buf, static_cast<int>(len)) == 1; }
    static void Quiesce(void *) { g_board->events += 'Q'; }
    static void Delay(uint32_t, void *) {}
    static bool NetworkUp(void *) { return g_board->dataset; }
    static esp_err_t NetworkErase(void *)
    {
        g_board->events += 'E';
        g_board->ram_dataset = g_board->dataset; // kept for the delivery session, in RAM
        if (g_board->on_erase) {
            g_board->on_erase();
        }
        if (g_board->erase_fails) {
            return ESP_FAIL;
        }
        if (!g_board->erase_lies) {
            g_board->dataset = false;
        }
        return ESP_OK;
    }
    static bool NetworkErased(void *)
    {
        g_board->events += g_board->in_session ? 'v' : 'V';
        return !g_board->dataset;
    }
    // Called when the erase starts (to look at the hub at that moment).
    std::function<void()> on_erase;
    // The delivery session: Thread from the Dataset kept in RAM; nothing is
    // written to the store -- unless session_stores_again says this one does.
    static bool ReceiptUp(void *)
    {
        g_board->events += 'S';
        g_board->in_session = true;
        if (g_board->on_session) {
            g_board->on_session();
        }
        if (!g_board->ram_dataset || g_board->dataset) {
            return false;
        }
        if (g_board->session_stores_again) {
            g_board->dataset = true;
        }
        return g_board->session_attaches;
    }
    // Called when the session is taken down (before the store is read back).
    std::function<void()> on_session_down;
    static bool ReceiptDown(void *)
    {
        g_board->events += 's';
        g_board->in_session = false;
        if (g_board->on_session_down) {
            g_board->on_session_down();
        }
        g_board->ram_dataset = false;
        return !g_board->dataset;
    }
    // Called when the session starts (to look at the hub at that moment).
    std::function<void()> on_session;
    static size_t LocateHints(uint8_t (*out)[PB_ENROLL_BRIDGE_ADDR_LEN], size_t cap, void *)
    {
        size_t n = 0;
        for (const auto &h : g_board->hints) {
            if (n < cap) {
                std::memcpy(out[n++], h.data(), h.size());
            }
        }
        return n;
    }
    static void EndpointChanged(const pb_enroll_bridge_endpoint_t *endpoint, void *)
    {
        g_board->moved++;
        g_board->moved_to = *endpoint;
    }
    const pb_mgmt_ops_t *Ops() const
    {
        static const pb_mgmt_ops_t plain = {Hmac,      Random,       Exchange,      Quiesce, Delay,
                                            NetworkUp, NetworkErase, NetworkErased, nullptr, nullptr,
                                            nullptr,   nullptr,      nullptr,       nullptr};
        static const pb_mgmt_ops_t with_session = {Hmac,      Random,       Exchange,      Quiesce,   Delay,      NetworkUp,
                                                   NetworkErase, NetworkErased, ReceiptUp, ReceiptDown,
                                                   nullptr,      nullptr,       nullptr,   nullptr};
        static const pb_mgmt_ops_t with_locate = {Hmac,         Random,        Exchange,    Quiesce,     Delay,
                                                  NetworkUp,    NetworkErase,  NetworkErased, ReceiptUp, ReceiptDown,
                                                  LocateHints,  EndpointChanged, nullptr,    nullptr};
        return locate ? &with_locate : (session ? &with_session : &plain);
    }

    // The release receipt, as a client reads it from the board over BLE:
    // empty when the board has none.
    std::vector<uint8_t> Receipt()
    {
        std::vector<uint8_t> out(PB_MGMT_RECEIPT_LEN);
        if (!pb_mgmt_receipt_load(eui.data(), NoNetworkStored, nullptr, out.data())) {
            out.clear();
        }
        return out;
    }
    // What the firmware asks before it hands out a receipt: is a network in
    // the stores?
    static bool NoNetworkStored(void *) { return !g_board->dataset; }

    // "Enrolls" the board: the version 3 record the firmware writes when a
    // managed enrollment completes, with exactly the hub's grant.
    bool Enroll(Hub &h, const std::string &eui64, const ManagementGrant &grant)
    {
        hub = &h;
        eui_hex = eui64;
        eui = Uuid(eui64);
        pb_enroll_record_t rec;
        std::memset(&rec, 0, sizeof(rec));
        const uint8_t xpan[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
        const uint8_t mlp[8] = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00};
        const uint8_t addr[16] = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00,
                                  0x8c, 0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79};
        std::memcpy(rec.id.ext_pan_id, xpan, 8);
        std::memcpy(rec.id.mesh_local_prefix, mlp, 8);
        rec.has_bridge_endpoint = true;
        std::memcpy(rec.bridge_endpoint.addr, addr, 16);
        rec.bridge_endpoint.port = 5684;
        rec.has_mgmt = true;
        rec.mgmt_state = PB_ENROLL_MGMT_ENROLLED;
        auto hub_id = Bytes<16>(grant.hub_id);
        auto generation = Bytes<16>(grant.generation);
        auto key = Bytes<32>(grant.key);
        std::memcpy(rec.mgmt.hub_id, hub_id.data(), 16);
        std::memcpy(rec.mgmt.generation, generation.data(), 16);
        std::memcpy(rec.mgmt.key, key.data(), 32);
        nvs_sim_reset();
        if (pb_enroll_record_store(eui.data(), &rec) != ESP_OK) {
            return false;
        }
        dataset = true;
        return Boot();
    }

    // The boot path of a board whose record is a final release: the check
    // against its stores.
    pb_mgmt_final_check_t CheckFinalRelease()
    {
        g_board = this;
        ram_dataset = false;
        in_session = false;
        pb_mgmt_client_init(&client, Ops(), eui.data());
        if (pb_mgmt_client_load(&client) == PB_MGMT_BOOT_RELEASE_PENDING) {
            return PB_MGMT_FINAL_NONE;
        }
        return pb_mgmt_client_check_final_release(&client, NoNetworkStored, nullptr);
    }

    // The boot path of a board whose release is not final.
    pb_mgmt_release_result_t FinishRelease()
    {
        g_board = this;
        ram_dataset = false; // a boot starts with nothing in RAM
        in_session = false;
        pb_mgmt_client_init(&client, Ops(), eui.data());
        if (pb_mgmt_client_load(&client) != PB_MGMT_BOOT_RELEASE_PENDING) {
            return PB_MGMT_RELEASE_NOTHING;
        }
        return pb_mgmt_client_resume_release(&client);
    }

    // A reboot of the board: RAM gone, the simulated flash kept.
    bool Boot()
    {
        g_board = this;
        ram_dataset = false; // a boot starts with nothing in RAM
        pb_mgmt_client_init(&client, Ops(), eui.data());
        return pb_mgmt_client_load(&client) == PB_MGMT_BOOT_ARMED;
    }

    pb_mgmt_poll_result_t Poll()
    {
        g_board = this;
        return pb_mgmt_client_poll(&client);
    }

    int StoredState()
    {
        pb_enroll_record_t rec;
        if (pb_enroll_record_load(eui.data(), &rec) != PB_ENROLL_MARKER_VALID || !rec.has_mgmt) {
            return 0;
        }
        return static_cast<int>(rec.mgmt_state) * 10 + rec.release_flags;
    }
};

// A signed POLL made directly from a grant, for boards this test does not
// model in full (many Bases at once).
std::vector<uint8_t> PollFor(const std::string &eui64, const ManagementGrant &grant, const std::array<uint8_t, 16> &echo = {})
{
    pb_mgmt_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.type = PB_MGMT_TYPE_POLL;
    auto eui = Bytes<8>(eui64);
    auto hub_id = Bytes<16>(grant.hub_id);
    auto generation = Bytes<16>(grant.generation);
    auto key = Bytes<32>(grant.key);
    std::memcpy(m.id.eui64, eui.data(), 8);
    std::memcpy(m.id.hub_id, hub_id.data(), 16);
    std::memcpy(m.id.generation, generation.data(), 16);
    RAND_bytes(m.board_nonce, 16);
    std::memcpy(m.hub_nonce, echo.data(), 16);
    m.state = PB_MGMT_STATE_ENROLLED;
    std::vector<uint8_t> wire(PB_MGMT_MAX_MESSAGE_LEN);
    wire.resize(pb_mgmt_pack(&m, key.data(), Hmac, nullptr, wire.data(), wire.size()));
    return wire;
}

// The data path, through the real session code.
uint8_t SendHeartbeat(ingest::Registry &registry, const ingest::Uuid &base)
{
    bridge_export_heartbeat_t hb{};
    std::memcpy(hb.power_base_uuid, base.data(), 8);
    hb.uptime_ms = 1000;
    uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
    size_t n = bridge_export_pack_heartbeat(&hb, wire, sizeof(wire));
    ingest::CoapMessage req;
    req.code = ingest::kCoapCodePost;
    req.uri_path = "pogo/v1/heartbeat";
    req.payload.assign(wire, wire + n);
    return ingest::handle_request(registry, req);
}

uint8_t SendTopology(ingest::Registry &registry, const ingest::Uuid &base, const ingest::Uuid &module, uint32_t seq,
                     bool online = true)
{
    bridge_export_topology_node_t node{};
    std::memcpy(node.power_base_uuid, base.data(), 8);
    std::memcpy(node.module_uuid, module.data(), 8);
    std::memcpy(node.parent_uuid, base.data(), 8);
    node.online = online;
    node.depth = 1;
    node.role = 3;
    node.sensor_bitmap = 6;
    node.boot_session_generation = 1;
    node.last_seq = seq;
    uint8_t wire[BRIDGE_EXPORT_MAX_MESSAGE_LEN];
    size_t n = bridge_export_pack_topology_node(&node, wire, sizeof(wire));
    ingest::CoapMessage req;
    req.code = ingest::kCoapCodePost;
    req.uri_path = "pogo/v1/topology";
    req.payload.assign(wire, wire + n);
    return ingest::handle_request(registry, req);
}

uint8_t SendSample(ingest::Registry &registry, const ingest::Uuid &base, const ingest::Uuid &module, uint32_t seq)
{
    sensor_sample_t sample;
    std::memset(&sample, 0, sizeof(sample));
    sample.payload_version = SENSOR_CHANNEL_PAYLOAD_VERSION;
    std::memcpy(sample.module_uuid, module.data(), 8);
    sample.sample_seq = seq;
    sample.uptime_ms = 1000;
    sample.sensor_bitmap = 0x0002;
    sample.channel_count = 1;
    sample.channels[0].type = SENSOR_CHANNEL_TEMPERATURE;
    sample.channels[0].scale = SENSOR_SCALE_MILLI;
    sample.channels[0].status = SENSOR_CHANNEL_STATUS_OK;
    sample.channels[0].value = 21500;
    bridge_export_sample_t exported;
    std::memset(&exported, 0, sizeof(exported));
    std::memcpy(exported.power_base_uuid, base.data(), 8);
    exported.sensor_channel_len = static_cast<uint16_t>(
        sensor_channel_pack(&sample, exported.sensor_channel_payload, sizeof(exported.sensor_channel_payload)));
    uint8_t wire[BRIDGE_EXPORT_MAX_TOTAL_LEN];
    size_t n = bridge_export_pack_sample(&exported, wire, sizeof(wire));
    ingest::CoapMessage req;
    req.code = ingest::kCoapCodePost;
    req.uri_path = "pogo/v1/sample";
    req.payload.assign(wire, wire + n);
    return ingest::handle_request(registry, req);
}

// ---- boards driven directly from their grant (many Bases at once) ----

// A signed UNENROLL_DONE of `result` for `command`.
std::vector<uint8_t> DoneFor(const std::string &eui64, const ManagementGrant &grant, const std::array<uint8_t, 16> &command,
                             uint8_t result)
{
    pb_mgmt_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.type = PB_MGMT_TYPE_UNENROLL_DONE;
    auto eui = Bytes<8>(eui64);
    auto hub_id = Bytes<16>(grant.hub_id);
    auto generation = Bytes<16>(grant.generation);
    auto key = Bytes<32>(grant.key);
    std::memcpy(m.id.eui64, eui.data(), 8);
    std::memcpy(m.id.hub_id, hub_id.data(), 16);
    std::memcpy(m.id.generation, generation.data(), 16);
    std::memcpy(m.command_id, command.data(), 16);
    m.result = result;
    std::vector<uint8_t> wire(PB_MGMT_MAX_MESSAGE_LEN);
    wire.resize(pb_mgmt_pack(&m, key.data(), Hmac, nullptr, wire.data(), wire.size()));
    return wire;
}

// The board's first authenticated poll: activates the generation.
bool ActivateDirect(PowerBaseManagement &management, const std::string &eui64, const ManagementGrant &grant)
{
    ingest::ManagementReply reply = management.HandleDatagram(PollFor(eui64, grant));
    pb_mgmt_msg_t a;
    return reply.code == ingest::kCoapCodeContent && pb_mgmt_parse(reply.payload.data(), reply.payload.size(), &a) &&
           a.type == PB_MGMT_TYPE_IDLE;
}

// Polls until the hub gives the command (at most two polls) and returns its
// id; false when the hub has no command for this board.
bool FetchCommandDirect(PowerBaseManagement &management, const std::string &eui64, const ManagementGrant &grant,
                        std::array<uint8_t, 16> &command)
{
    std::array<uint8_t, 16> echo{};
    for (int i = 0; i < 3; i++) {
        ingest::ManagementReply reply = management.HandleDatagram(PollFor(eui64, grant, echo));
        pb_mgmt_msg_t a;
        if (reply.code != ingest::kCoapCodeContent || !pb_mgmt_parse(reply.payload.data(), reply.payload.size(), &a)) {
            return false;
        }
        if (a.type == PB_MGMT_TYPE_UNENROLL) {
            std::memcpy(command.data(), a.command_id, 16);
            return true;
        }
        if ((a.flags & PB_MGMT_IDLE_F_COMMAND_PENDING) == 0) {
            return false;
        }
        std::memcpy(echo.data(), a.hub_nonce, 16);
    }
    return false;
}

// Command, then the board's "released" notice. The returned code is the
// hub's answer to the notice.
uint8_t ReleaseDirect(PowerBaseManagement &management, const std::string &eui64, const ManagementGrant &grant,
                      std::array<uint8_t, 16> &command)
{
    if (!FetchCommandDirect(management, eui64, grant, command)) {
        return 0;
    }
    return management.HandleDatagram(DoneFor(eui64, grant, command, PB_MGMT_RESULT_RELEASED)).code;
}

bool HasNode(const ingest::RegistryView &view, const ingest::Uuid &base, const ingest::Uuid &module)
{
    return std::any_of(view.nodes.begin(), view.nodes.end(), [&](const ingest::NodeState &n) {
        return n.power_base_uuid == base && n.module_uuid == module;
    });
}

bool HasBase(const ingest::RegistryView &view, const ingest::Uuid &base)
{
    return std::any_of(view.power_bases.begin(), view.power_bases.end(),
                       [&](const ingest::PowerBaseState &b) { return b.power_base_uuid == base; });
}

RemovalState StateOf(Hub &hub, const std::string &id)
{
    RemovalJob job;
    hub.management->GetRemoval(id, job);
    return job.state;
}

void GoldenVectors(int *failures)
{
    std::printf("\n-- management wire format: the firmware test's golden MACs, computed here with OpenSSL --\n");
    pb_mgmt_msg_t m;
    uint8_t wire[PB_MGMT_MAX_MESSAGE_LEN];
    uint8_t key[32];
    for (int i = 0; i < 32; i++) {
        key[i] = static_cast<uint8_t>(0x30 + i);
    }
    auto base = [&](uint8_t type) {
        std::memset(&m, 0, sizeof(m));
        m.type = type;
        auto eui = Bytes<8>(kEuiA);
        std::memcpy(m.id.eui64, eui.data(), 8);
        for (int i = 0; i < 16; i++) {
            m.id.hub_id[i] = static_cast<uint8_t>(0x10 + i);
            m.id.generation[i] = static_cast<uint8_t>(0x20 + i);
        }
    };
    auto seq = [](uint8_t *out, uint8_t first) {
        for (int i = 0; i < 16; i++) {
            out[i] = static_cast<uint8_t>(first + i);
        }
    };
    auto mac = [&](size_t n) { return Hex(&wire[n - 32], 32); };

    base(PB_MGMT_TYPE_POLL);
    seq(m.board_nonce, 0xb0);
    seq(m.hub_nonce, 0xd0);
    m.state = PB_MGMT_STATE_ENROLLED;
    size_t n = pb_mgmt_pack(&m, key, Hmac, nullptr, wire, sizeof(wire));
    API_CHECK(n == 112 && mac(n) == "e688613943593c0dcca578bede6c46ea660c135bfe5dd7b613f9a9a04cc77c5f", "POLL");
    base(PB_MGMT_TYPE_IDLE);
    m.flags = PB_MGMT_IDLE_F_COMMAND_PENDING;
    seq(m.board_nonce, 0xb0);
    seq(m.hub_nonce, 0xd0);
    n = pb_mgmt_pack(&m, key, Hmac, nullptr, wire, sizeof(wire));
    API_CHECK(n == 108 && mac(n) == "7a1c2f5a3a0a982d10d1fe6d8bfc81209403bc879521526069d478ba32a2b6f2", "IDLE (command pending)");
    base(PB_MGMT_TYPE_UNENROLL);
    seq(m.board_nonce, 0xb0);
    seq(m.command_id, 0xe0);
    n = pb_mgmt_pack(&m, key, Hmac, nullptr, wire, sizeof(wire));
    API_CHECK(n == 108 && mac(n) == "2125a1901a7889b262b44d33ff43fc6fb3b22d0217227f91a551ee6bd2bb1a8c", "UNENROLL");
    base(PB_MGMT_TYPE_UNENROLL_ACCEPTED);
    seq(m.command_id, 0xe0);
    n = pb_mgmt_pack(&m, key, Hmac, nullptr, wire, sizeof(wire));
    API_CHECK(n == 92 && mac(n) == "cca2b43ad652e17a0a6b9b37119c65ee8362074788c3f27ae9eb8013b3f78dc7", "UNENROLL_ACCEPTED");
    base(PB_MGMT_TYPE_UNENROLL_DONE);
    seq(m.command_id, 0xe0);
    m.result = PB_MGMT_RESULT_RELEASED;
    n = pb_mgmt_pack(&m, key, Hmac, nullptr, wire, sizeof(wire));
    API_CHECK(n == 96 && mac(n) == "02f00a8d63fba56ff747911889aae31a29b60d053178db827a8e8730a05b7eba",
              "UNENROLL_DONE / RELEASED (the notice)");
    m.result = PB_MGMT_RESULT_ERASED;
    n = pb_mgmt_pack(&m, key, Hmac, nullptr, wire, sizeof(wire));
    API_CHECK(n == 96 && mac(n) == "efd7833baf44c006d3f980b77630e2250e2d7f61328cceee83ac30a4a41fe01b",
              "UNENROLL_DONE / ERASED (the receipt)");
    base(PB_MGMT_TYPE_RESULT_ACK);
    seq(m.command_id, 0xe0);
    m.acked_type = PB_MGMT_TYPE_UNENROLL_DONE;
    m.acked_result = PB_MGMT_RESULT_RELEASED;
    n = pb_mgmt_pack(&m, key, Hmac, nullptr, wire, sizeof(wire));
    API_CHECK(n == 96 && mac(n) == "b49c98d6300315bc13e54178bd1e8600401dd5c0304858516fdf469a33a84112",
              "RESULT_ACK of the released notice");
    m.acked_result = PB_MGMT_RESULT_ERASED;
    n = pb_mgmt_pack(&m, key, Hmac, nullptr, wire, sizeof(wire));
    API_CHECK(n == 96 && mac(n) == "174d78c5ec891b13dad88c9eaf93c5c4d9b21602eafeab955569673dbf8085e4",
              "RESULT_ACK of the receipt");
    m.acked_type = PB_MGMT_TYPE_UNENROLL_ACCEPTED;
    m.acked_result = 0;
    n = pb_mgmt_pack(&m, key, Hmac, nullptr, wire, sizeof(wire));
    API_CHECK(n == 96 && mac(n) == "33e83b068d5d9ff3975fae6cd65eccb1c5458c957816eb2af45475d826bac23d",
              "RESULT_ACK of UNENROLL_ACCEPTED");
}

void OnlineRemoval(int *failures)
{
    std::printf("\n-- an online Power Base is unenrolled: firmware client <-> hub, end to end --\n");
    Hub hub;
    API_CHECK(hub.Open() && hub.management->available(), "management starts with no state file");
    API_CHECK(hub.StateFile().empty(), "and creates none until there is something to store");

    const ingest::Uuid base_a = Uuid(kEuiA), base_b = Uuid(kEuiB);
    const ingest::Uuid m1 = Module(1), m2 = Module(2), m3 = Module(3);

    // A Base the hub knows only from the data path cannot be unenrolled.
    SendHeartbeat(hub.registry, base_a);
    RemovalJob job;
    API_CHECK(RequestRemovalNow(*hub.management, "dev1", kEuiA, job) == RemovalRequestOutcome::Unsupported,
              "a Base without a management credential: unsupported (no local 'delete' in its place)");
    API_CHECK(RequestRemovalNow(*hub.management, "dev1", "6055f9fffe00ffff", job) == RemovalRequestOutcome::UnknownPowerBase,
              "a Base the hub has never heard of: unknown");

    ManagementGrant grant;
    API_CHECK(hub.management->IssueCredential(kEuiA, grant) && grant.hub_id == kHubId && grant.generation.size() == 32 &&
                  grant.key.size() == 64 && grant.version == 1,
              "a credential is issued: hub id, 16-byte generation, 32-byte key");
    API_CHECK(test_support::FileMode(hub.dir + "/" + kPowerBaseMgmtFileName) == 0600, "the state file is 0600");
    API_CHECK(hub.StateFile().find(grant.key) != std::string::npos &&
                  hub.StateFile().find("\"provisional\"") != std::string::npos,
              "it is stored before it is handed out, as provisional");
    API_CHECK(!hub.management->IsManaged(kEuiA) &&
                  RequestRemovalNow(*hub.management, "dev1", kEuiA, job) == RemovalRequestOutcome::Unsupported,
              "issued is not active: the Base is not 'managed' until the board has used the key");

    Board board;
    API_CHECK(board.Enroll(hub, kEuiA, grant), "(the board is enrolled with exactly that credential: record version 3)");
    API_CHECK(board.Poll() == PB_MGMT_POLL_IDLE && hub.management->IsManaged(kEuiA) &&
                  hub.StateFile().find("\"active\"") != std::string::npos,
              "the board's first authenticated poll activates the generation");

    // Modules: two under A; one of them (m2) has since moved to B and is
    // registered there too; B has one of its own.
    SendTopology(hub.registry, base_a, m1, 1);
    SendTopology(hub.registry, base_a, m2, 1);
    SendHeartbeat(hub.registry, base_b);
    SendTopology(hub.registry, base_b, m2, 1);
    SendTopology(hub.registry, base_b, m3, 1);

    const uint64_t gen_before = hub.registry.generation();
    API_CHECK(RequestRemovalNow(*hub.management, "dev1", kEuiA, job) == RemovalRequestOutcome::Created &&
                  job.state == RemovalState::Pending && job.cancellable && !job.result_unknown &&
                  job.affected_module_count == 2 && job.power_base_uuid == kEuiA && job.generation == grant.generation &&
                  job.requested_by_device_id == "dev1" && IsLowerHex(job.removal_id, 32),
              "a removal is created: pending, cancellable, bound to this Base and generation");
    API_CHECK(hub.registry.generation() > gen_before, "the Registry generation advances, so SSE clients get a new snapshot");
    const std::string id = job.removal_id;
    RemovalJob again;
    API_CHECK(RequestRemovalNow(*hub.management, "dev1", kEuiA, again) == RemovalRequestOutcome::Existing &&
                  again.removal_id == id,
              "the same request again: the same job");
    API_CHECK(RequestRemovalNow(*hub.management, "dev2", kEuiA, again) == RemovalRequestOutcome::Existing &&
                  again.removal_id == id && again.requested_by_device_id == "dev1",
              "the same Base from another device: still the same job");
    API_CHECK(hub.StateFile().find(id) != std::string::npos, "the job is in the state file the moment it is accepted");
    {
        ingest::RegistryView view = hub.registry.view();
        API_CHECK(HasBase(view, base_a) && HasNode(view, base_a, m1) && !hub.registry.is_power_base_blocked(base_a),
                  "a pending removal removes and blocks nothing: the Base is still enrolled");
    }

    // Cancel while pending, then ask again.
    API_CHECK(hub.management->CancelRemoval(id, again) == RemovalCancelOutcome::Cancelled &&
                  again.state == RemovalState::Cancelled,
              "a pending removal can be cancelled");
    API_CHECK(board.Poll() == PB_MGMT_POLL_IDLE && board.StoredState() == 10, "the board hears of nothing");
    API_CHECK(RequestRemovalNow(*hub.management, "dev2", kEuiA, job) == RemovalRequestOutcome::Created &&
                  job.removal_id != id,
              "a new request after the cancel is a new job");
    const std::string id2 = job.removal_id;

    // The command. The board has just restarted, so it holds no nonce of the
    // hub's: the first poll can only learn that a command waits. (A board
    // that kept polling echoes the nonce of its previous answer and gets the
    // command with its next poll -- see OfflineAndRestart.)
    API_CHECK(board.Boot(), "(the board restarts: its RAM, and with it the hub's last nonce, is gone)");
    board.events.clear();
    API_CHECK(board.Poll() == PB_MGMT_POLL_COMMAND_PENDING && StateOf(hub, id2) == RemovalState::Pending,
              "first poll: the hub only announces a command (its nonce must come back first); the job is still pending");
    API_CHECK(hub.management->GetRemoval(id2, again) && again.cancellable, "and still cancellable");
    // What the hub says at the moment the board starts erasing: by then it
    // has the board's "released" notice, and nothing more.
    RemovalState hub_at_erase = RemovalState::Pending;
    bool blocked_at_erase = false;
    board.on_erase = [&]() {
        hub_at_erase = StateOf(hub, id2);
        blocked_at_erase = hub.registry.is_power_base_blocked(base_a);
    };
    API_CHECK(board.Poll() == PB_MGMT_POLL_UNENROLLED, "second poll: the board carries the command out");
    board.on_erase = nullptr;
    API_CHECK(board.events == "PPQADEV",
              "board: poll, poll, stop services, accepted, released notice, network erase, store read back");
    API_CHECK(board.events.find('Z') == std::string::npos, "the board never put its receipt on the network");
    API_CHECK(board.StoredState() == 36, "board: final record -- erased, and the hub acknowledged the released notice");
    API_CHECK(hub_at_erase == RemovalState::Released && blocked_at_erase,
              "when the erase STARTED the hub said 'released' (and had blocked the Base) -- not 'completed'");

    // Released: everything the hub does about a Base that is gone is done --
    // and the removal is still not complete.
    API_CHECK(hub.management->GetRemoval(id2, again) && again.state == RemovalState::Released && !again.cancellable &&
                  !again.result_unknown && again.completed_at_unix_ms == 0,
              "hub: the job is RELEASED, not completed -- the board's stored network is not confirmed erased");
    API_CHECK(RemovalJobJson(again).find("\"state\":\"released\"") != std::string::npos &&
                  RemovalJobJson(again).find("\"completed_at_unix_ms\":null") != std::string::npos,
              "and the API says exactly that");
    ingest::RegistryView view = hub.registry.view();
    API_CHECK(!HasBase(view, base_a) && !HasNode(view, base_a, m1) && !HasNode(view, base_a, m2),
              "the Base and the modules registered under it left the Registry");
    API_CHECK(HasBase(view, base_b) && HasNode(view, base_b, m2) && HasNode(view, base_b, m3),
              "the module that had moved to another Base, and that Base, are untouched");
    API_CHECK(hub.registry.is_power_base_blocked(base_a) && hub.management->IsBlocked(kEuiA) &&
                  !hub.management->IsManaged(kEuiA),
              "the Base is blocked on the data path and no longer 'managed'");
    API_CHECK(hub.removed_modules.empty() && hub.management->PendingSettingsCleanups() == 1 && again.settings_cleanup_pending,
              "the settings clean-up is noted in the state, not run on the ingest thread");
    API_CHECK(hub.StateList("cleanups").find(FormatUuidHex(m1)) != std::string::npos &&
                  hub.StateList("cleanups").find(FormatUuidHex(m2)) == std::string::npos,
              "the state file's clean-up names the module that left with the Base -- and not the one that lives on another Base");
    API_CHECK(hub.StateList("members").find(FormatUuidHex(m1)) == std::string::npos &&
                  hub.StateList("members").find("{\"eui64\":\"" + std::string(kEuiB) + "\"") != std::string::npos &&
                  hub.StateList("members").find(FormatUuidHex(m2)) != std::string::npos &&
                  hub.StateList("members").find(kEuiA) == std::string::npos,
              "and its members: the released Base has none any more, the other Base has the module that lives on it");
    API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && hub.removed_modules.size() == 1 &&
                  hub.removed_modules[0] == std::vector<std::string>{FormatUuidHex(m1)},
              "the clean-up pass hands the settings exactly that module, and is done");
    API_CHECK(hub.management->GetRemoval(id2, again) && !again.settings_cleanup_pending &&
                  hub.StateFile().find("\"cleanups\":[]") != std::string::npos,
              "after which nothing is owed any more");
    API_CHECK(hub.StateFile().find("\"state\":\"released\"") != std::string::npos &&
                  hub.StateFile().find("\"completed\"") == std::string::npos &&
                  hub.StateFile().find("\"blocked\":[{") != std::string::npos && hub.StateFile().find(grant.key) != std::string::npos,
              "released job, the block, and the key (still needed to check the receipt) are in the state file");

    // The released notice is not a receipt, whoever brings it.
    std::vector<uint8_t> notice;
    for (const auto &request : board.sent) {
        if (request[1] == PB_MGMT_TYPE_UNENROLL_DONE) {
            notice = request;
        }
    }
    RemovalJob by_receipt;
    API_CHECK(notice.size() == PB_MGMT_UNENROLL_DONE_LEN && notice[PB_MGMT_HEADER_LEN + 16] == PB_MGMT_RESULT_RELEASED &&
                  hub.management->SubmitReleaseReceipt(notice, by_receipt) == ReceiptOutcome::Malformed &&
                  StateOf(hub, id2) == RemovalState::Released,
              "the notice (a genuine, correctly signed message) handed in as a receipt: refused, still released");
    {
        // What an earlier hub build took for completion: the same notice,
        // over the management resource, any number of times.
        const uint64_t before = hub.registry.generation();
        ingest::ManagementReply reply = hub.management->HandleDatagram(notice);
        API_CHECK(reply.code == ingest::kCoapCodeContent && reply.payload == board.answers.back() &&
                      StateOf(hub, id2) == RemovalState::Released && hub.registry.generation() == before,
                  "the notice again: the same acknowledgement, nothing happens twice, and it still completes nothing");
        pb_mgmt_msg_t ack;
        API_CHECK(pb_mgmt_parse(reply.payload.data(), reply.payload.size(), &ack) &&
                      ack.acked_type == PB_MGMT_TYPE_UNENROLL_DONE && ack.acked_result == PB_MGMT_RESULT_RELEASED,
                  "its acknowledgement names what it acknowledges: 'released'");
    }

    // The receipt: collected from the board (over BLE, by an app) and relayed.
    std::vector<uint8_t> receipt = board.Receipt();
    API_CHECK(receipt.size() == PB_MGMT_UNENROLL_DONE_LEN && receipt[1] == PB_MGMT_TYPE_UNENROLL_DONE &&
                  receipt[PB_MGMT_HEADER_LEN + 16] == PB_MGMT_RESULT_ERASED,
              "the board holds its receipt: UNENROLL_DONE with result ERASED");
    {
        std::vector<uint8_t> forged = receipt;
        forged.back() ^= 0x01;
        API_CHECK(hub.management->SubmitReleaseReceipt(forged, by_receipt) == ReceiptOutcome::Rejected,
                  "a receipt with a wrong MAC: rejected");
        forged = notice;
        forged[PB_MGMT_HEADER_LEN + 16] = PB_MGMT_RESULT_ERASED; // "released" relabelled as "erased", MAC unchanged
        API_CHECK(hub.management->SubmitReleaseReceipt(forged, by_receipt) == ReceiptOutcome::Rejected &&
                      StateOf(hub, id2) == RemovalState::Released,
                  "the released notice with its result byte changed to 'erased': rejected (the MAC covers the result)");
        forged = receipt;
        forged[12] ^= 0x01; // another hub's id
        API_CHECK(hub.management->SubmitReleaseReceipt(forged, by_receipt) == ReceiptOutcome::Rejected,
                  "a receipt naming another hub: rejected");
        API_CHECK(hub.management->SubmitReleaseReceipt(std::vector<uint8_t>(receipt.begin(), receipt.end() - 1), by_receipt) ==
                      ReceiptOutcome::Malformed,
                  "a truncated receipt: malformed");
    }
    g_now_ms += 5000;
    API_CHECK(hub.management->SubmitReleaseReceipt(receipt, by_receipt) == ReceiptOutcome::Completed &&
                  by_receipt.removal_id == id2 && by_receipt.state == RemovalState::Completed &&
                  by_receipt.completed_at_unix_ms == g_now_ms,
              "the genuine receipt completes the removal -- the only thing that does");
    API_CHECK(hub.StateFile().find("\"state\":\"completed\"") != std::string::npos &&
                  hub.StateFile().find(grant.key) == std::string::npos && hub.registry.is_power_base_blocked(base_a),
              "completed is stored, the key is dropped (it has done everything it was issued for), the block stays");
    API_CHECK(hub.management->SubmitReleaseReceipt(receipt, by_receipt) == ReceiptOutcome::AlreadyCompleted &&
                  by_receipt.state == RemovalState::Completed && by_receipt.completed_at_unix_ms == g_now_ms,
              "the same receipt again: recognised, nothing changes");
    {
        std::vector<uint8_t> other = receipt;
        other.back() ^= 0x01;
        API_CHECK(hub.management->SubmitReleaseReceipt(other, by_receipt) == ReceiptOutcome::Rejected,
                  "another 'receipt' for the completed job: rejected");
    }

    // Late data from the removed Base.
    const uint64_t gen_done = hub.registry.generation();
    const uint64_t hb_serial = hub.registry.heartbeat_serial();
    API_CHECK(SendHeartbeat(hub.registry, base_a) == ingest::kCoapCodeForbidden &&
                  SendTopology(hub.registry, base_a, m1, 2) == ingest::kCoapCodeForbidden &&
                  SendTopology(hub.registry, base_a, Module(9), 1) == ingest::kCoapCodeForbidden,
              "a late heartbeat and late topology reports from it are refused (4.03)");
    view = hub.registry.view();
    API_CHECK(!HasBase(view, base_a) && !HasNode(view, base_a, m1) && view.nodes.size() == 2 &&
                  hub.registry.generation() == gen_done && hub.registry.heartbeat_serial() == hb_serial,
              "and change nothing: no Base, no module, no generation bump, no heartbeat serial");
    API_CHECK(SendHeartbeat(hub.registry, base_b) == ingest::kCoapCodeChanged, "(the other Base's data is accepted as always)");

    // Nothing secret in what the API shows.
    std::string list = RemovalListJson(hub.management->ListRemovals());
    API_CHECK(list.find(grant.key) == std::string::npos && list.find("command") == std::string::npos &&
                  list.find("\"key\"") == std::string::npos && list.find(id2) != std::string::npos,
              "the removal list names the jobs and carries no key and no command id");
    API_CHECK(hub.management->CancelRemoval(id2, again) == RemovalCancelOutcome::NotCancellable,
              "a completed removal cannot be cancelled");
}

// Defect 1 as it was reported: the hub called a removal complete on a proof
// the board sent BEFORE it erased its stored network, so a failed erase was
// still "completed".
void EraseFailureIsNotCompletion(int *failures)
{
    std::printf("\n-- the board's erase fails, lies, or its final record cannot be written: never 'completed' --\n");
    for (int mode = 0; mode < 3; mode++) {
        Hub hub;
        hub.Open();
        ManagementGrant grant;
        hub.management->IssueCredential(kEuiA, grant);
        Board board;
        board.Enroll(hub, kEuiA, grant);
        board.Poll();
        RemovalJob job, out;
        RequestRemovalNow(*hub.management, "dev", kEuiA, job);
        const std::string id = job.removal_id;
        RemovalState seen_in_erase = RemovalState::Pending;
        board.on_erase = [&]() { seen_in_erase = StateOf(hub, id); };
        const char *what = mode == 0 ? "the erase returns an error" : (mode == 1 ? "the erase returns OK but the store is not empty" : "the final record cannot be written");
        board.erase_fails = mode == 0;
        board.erase_lies = mode == 1;
        if (mode == 2) {
            // journal, release record, (erase), final record: the third commit.
            // The NVS simulator fails the NEXT commit; arm it when the erase runs.
            board.on_erase = [&]() {
                seen_in_erase = StateOf(hub, id);
                nvs_sim_force_next_commit_error(ESP_FAIL);
            };
        }
        const pb_mgmt_poll_result_t result = board.Poll();
        board.on_erase = nullptr;
        std::printf("   (%s)\n", what);
        API_CHECK(result == PB_MGMT_POLL_RELEASED_NOT_ERASED, "the firmware reports 'released, NOT erased' -- not an unenroll");
        API_CHECK(seen_in_erase == RemovalState::Released, "inside the erase the hub said 'released': it had not completed anything");
        API_CHECK(StateOf(hub, id) == RemovalState::Released, "after the failure the hub still says 'released'");
        API_CHECK(board.events.find('Z') == std::string::npos, "no receipt was sent");
        if (mode != 2) {
            // (After a failed final commit the simulator's RAM copy is not
            // what a reboot would find; modes 0 and 1 wrote nothing.)
            API_CHECK(board.Receipt().empty() && board.StoredState() == 31, "and the board has none to hand out: erase pending");
        }
        g_now_ms += 10 * 60 * 1000;
        API_CHECK(hub.management->GetRemoval(id, out) && out.state == RemovalState::Released && out.completed_at_unix_ms == 0,
                  "ten minutes later: still 'released' -- time completes nothing");
        // Every boot on which it still fails: the same.
        if (mode != 2) {
            API_CHECK(board.FinishRelease() == PB_MGMT_RELEASE_INCOMPLETE && board.Receipt().empty() &&
                          StateOf(hub, id) == RemovalState::Released,
                      "a next boot on which it fails again: incomplete, no receipt, hub unchanged");
            board.erase_fails = false;
            board.erase_lies = false;
            API_CHECK(board.FinishRelease() == PB_MGMT_RELEASE_FINAL && !board.Receipt().empty(),
                      "the boot on which the erase works and reads back empty: final, and now there is a receipt");
            API_CHECK(StateOf(hub, id) == RemovalState::Released, "which the hub has not seen yet: still 'released'");
            API_CHECK(hub.management->SubmitReleaseReceipt(board.Receipt(), out) == ReceiptOutcome::Completed &&
                          out.state == RemovalState::Completed,
                      "relayed, it completes the removal");
        }
    }
}

void OfflineAndRestart(int *failures)
{
    std::printf("\n-- offline Base, hub restart, lost answers --\n");
    Hub hub;
    hub.Open();
    ManagementGrant grant;
    hub.management->IssueCredential(kEuiA, grant);
    Board board;
    board.Enroll(hub, kEuiA, grant);
    board.Poll();
    const ingest::Uuid base_a = Uuid(kEuiA);
    SendHeartbeat(hub.registry, base_a);
    SendTopology(hub.registry, base_a, Module(1), 1);

    // The board is off. The request is taken and waits.
    board.link_up = false;
    RemovalJob job, again;
    API_CHECK(RequestRemovalNow(*hub.management, "dev1", kEuiA, job) == RemovalRequestOutcome::Created &&
                  job.state == RemovalState::Pending,
              "the Base is offline: the removal is accepted and waits (pending)");
    const std::string id = job.removal_id;
    g_now_ms += 24 * 3600 * 1000;
    API_CHECK(hub.management->GetRemoval(id, again) && again.state == RemovalState::Pending && again.cancellable &&
                  !again.result_unknown,
              "a day later, still offline: still pending, still cancellable -- time alone decides nothing");

    // The hub restarts. (A new process has a new, empty Registry.)
    hub.management.reset();
    API_CHECK(hub.Open() && hub.management->GetRemoval(id, again) && again.state == RemovalState::Pending &&
                  hub.management->IsManaged(kEuiA),
              "hub restart: the pending job and the credential are restored from the state file");
    {
        SnapshotManagement extra;
        extra.managed_power_bases = hub.management->ManagedPowerBases();
        ingest::RegistryView empty;
        std::string snapshot = BuildSnapshotJson(empty, 1, 0, &extra);
        API_CHECK(snapshot.find(std::string("{\"uuid\":\"") + kEuiA +
                                "\",\"online\":false,\"module_count\":0,\"online_module_count\":0,"
                                "\"management\":\"supported\",\"erase_confirmation\":\"manual\"}") != std::string::npos,
                  "a managed Base that has been silent since the restart is still listed (offline), so it stays selectable");
    }

    // The board comes back; the answer that carries the command is lost.
    board.link_up = true;
    API_CHECK(board.Poll() == PB_MGMT_POLL_COMMAND_PENDING, "the board is back: its poll learns that a command waits");
    board.drop_answers = true;
    API_CHECK(board.Poll() == PB_MGMT_POLL_NO_ANSWER && board.StoredState() == 10, "the answer with the command is lost: the board is unchanged");
    API_CHECK(hub.management->GetRemoval(id, again) && again.state == RemovalState::Sent && !again.cancellable,
              "but the hub sent it: the job is 'sent' and can no longer be cancelled");
    API_CHECK(hub.management->CancelRemoval(id, again) == RemovalCancelOutcome::NotCancellable &&
                  again.state == RemovalState::Sent,
              "a cancel now is refused -- the command may be on the board");

    // Restart in 'sent'.
    hub.management.reset();
    API_CHECK(hub.Open() && StateOf(hub, id) == RemovalState::Sent, "hub restart: the job is still 'sent'");

    // No progress for longer than the limit: the display hint, not a state.
    g_now_ms += 6 * 60 * 1000;
    API_CHECK(hub.management->GetRemoval(id, again) && again.state == RemovalState::Sent && again.result_unknown,
              "no proof for more than five minutes: result_unknown is reported; the state is still 'sent', not 'completed'");

    // The board polls again and gets the SAME command.
    board.drop_answers = false;
    board.events.clear();
    API_CHECK(board.Poll() == PB_MGMT_POLL_COMMAND_PENDING && board.Poll() == PB_MGMT_POLL_UNENROLLED,
              "the board's next polls get the command again and carry it out");
    API_CHECK(hub.management->GetRemoval(id, again) && again.state == RemovalState::Released && !again.result_unknown,
              "a late released notice moves the job on -- to 'released', which is not 'result unknown' and not 'completed'");
    API_CHECK(hub.registry.is_power_base_blocked(base_a), "and the Base is blocked");
    const std::vector<uint8_t> receipt = board.Receipt();
    API_CHECK(!receipt.empty(), "(the board holds its receipt)");

    // The block, the released job and the key to check the receipt with all
    // survive a restart.
    hub.management.reset();
    ingest::Registry fresh; // what the new process starts with
    PowerBaseManagementConfig config;
    config.directory = hub.dir;
    config.hub_id = kHubId;
    config.now_unix_ms = []() { return g_now_ms; };
    PowerBaseManagement reopened(fresh, config);
    std::string error;
    API_CHECK(reopened.Load(error) && fresh.is_power_base_blocked(base_a) &&
                  SendHeartbeat(fresh, base_a) == ingest::kCoapCodeForbidden && fresh.view().power_bases.empty(),
              "hub restart after the release: the block is applied to the new Registry before any data arrives");
    g_now_ms += 7LL * 24 * 3600 * 1000;
    API_CHECK(reopened.GetRemoval(id, again) && again.state == RemovalState::Released && !again.result_unknown &&
                  reopened.ListRemovals().size() == 1,
              "a week later the job is still there and still 'released': it is neither completed nor dropped by time");
    API_CHECK(reopened.SubmitReleaseReceipt(receipt, again) == ReceiptOutcome::Completed &&
                  again.state == RemovalState::Completed && fresh.is_power_base_blocked(base_a),
              "the receipt, relayed after the restart, completes it");
}

void LostProof(int *failures)
{
    std::printf("\n-- the board's released notice never arrives --\n");
    Hub hub;
    hub.Open();
    ManagementGrant grant;
    hub.management->IssueCredential(kEuiA, grant);
    Board board;
    board.Enroll(hub, kEuiA, grant);
    board.Poll();
    SendHeartbeat(hub.registry, Uuid(kEuiA));
    RemovalJob job, again;
    RequestRemovalNow(*hub.management, "dev1", kEuiA, job);
    // The board gets the command with its next poll (it echoes the nonce of
    // the poll above); everything it sends afterwards is lost.
    struct LoseAfterCommand {
        static pb_mgmt_xfer_t Exchange(const pb_enroll_bridge_endpoint_t *ep, const uint8_t *req, size_t req_len,
                                       uint8_t *resp, size_t resp_cap, size_t *resp_len, void *ctx)
        {
            if (req[1] != PB_MGMT_TYPE_POLL) {
                g_board->events += req[1] == PB_MGMT_TYPE_UNENROLL_ACCEPTED ? 'a' : 'd';
                *resp_len = 0;
                return PB_MGMT_XFER_NO_ANSWER;
            }
            return Board::Exchange(ep, req, req_len, resp, resp_cap, resp_len, ctx);
        }
    };
    board.client.ops.exchange = LoseAfterCommand::Exchange;
    board.events.clear();
    API_CHECK(board.Poll() == PB_MGMT_POLL_UNENROLLED && board.StoredState() == 34,
              "the board releases itself and erases its network although no notice got through (bounded attempts)");
    API_CHECK(std::count(board.events.begin(), board.events.end(), 'd') == PB_MGMT_PROOF_ATTEMPTS,
              "after exactly the bounded number of notice attempts");
    const std::vector<uint8_t> unseen_receipt = board.Receipt();
    API_CHECK(!unseen_receipt.empty(), "(it holds its receipt)");
    API_CHECK(hub.management->GetRemoval(job.removal_id, again) && again.state == RemovalState::Sent,
              "the hub saw none of it: the job stays 'sent'");
    g_now_ms += 10 * 60 * 1000;
    API_CHECK(hub.management->GetRemoval(job.removal_id, again) && again.state == RemovalState::Sent &&
                  again.result_unknown && !hub.registry.is_power_base_blocked(Uuid(kEuiA)),
              "later: 'result unknown' -- never 'completed' by a timeout, and nothing is removed or blocked");
    ingest::RegistryView view = hub.registry.view();
    API_CHECK(HasBase(view, Uuid(kEuiA)), "the Base stays in the list (it goes offline by itself when it stays silent)");

    // The way out: the same board is enrolled again.
    ManagementGrant next;
    API_CHECK(hub.management->IssueCredential(kEuiA, next) && next.generation != grant.generation && next.key != grant.key,
              "a new enrollment session issues a NEW generation and key");
    API_CHECK(StateOf(hub, job.removal_id) == RemovalState::Sent, "issuing alone changes nothing");
    Board again_board;
    again_board.Enroll(hub, kEuiA, next);
    API_CHECK(again_board.Poll() == PB_MGMT_POLL_IDLE && StateOf(hub, job.removal_id) == RemovalState::Superseded &&
                  hub.management->IsManaged(kEuiA),
              "its first authenticated poll activates the new generation and ends the old job as superseded");
    API_CHECK(!hub.registry.is_power_base_blocked(Uuid(kEuiA)) && SendHeartbeat(hub.registry, Uuid(kEuiA)) == ingest::kCoapCodeChanged,
              "the Base is not blocked: it is enrolled");
    API_CHECK(again_board.Poll() == PB_MGMT_POLL_IDLE && again_board.StoredState() == 10,
              "and the old job's command is never given to the new enrollment");
    RemovalJob late;
    API_CHECK(hub.management->SubmitReleaseReceipt(unseen_receipt, late) == ReceiptOutcome::Rejected &&
                  StateOf(hub, job.removal_id) == RemovalState::Superseded && hub.management->IsManaged(kEuiA) &&
                  !hub.registry.is_power_base_blocked(Uuid(kEuiA)),
              "the old enrollment's receipt, arriving after the new one is active: rejected, the new enrollment untouched");
}

// The released notice was lost, but the receipt reaches the hub (an app read
// it from the board): it says more than the notice would have.
void ReceiptWithoutNotice(int *failures)
{
    std::printf("\n-- the receipt arrives although the released notice never did --\n");
    Hub hub;
    hub.Open();
    const ingest::Uuid base_a = Uuid(kEuiA);
    ManagementGrant grant;
    hub.management->IssueCredential(kEuiA, grant);
    Board board;
    board.Enroll(hub, kEuiA, grant);
    board.Poll();
    SendHeartbeat(hub.registry, base_a);
    SendTopology(hub.registry, base_a, Module(7), 1);
    RemovalJob job, out;
    RequestRemovalNow(*hub.management, "dev1", kEuiA, job);
    struct LoseAfterCommand {
        static pb_mgmt_xfer_t Exchange(const pb_enroll_bridge_endpoint_t *ep, const uint8_t *req, size_t req_len,
                                       uint8_t *resp, size_t resp_cap, size_t *resp_len, void *ctx)
        {
            if (req[1] != PB_MGMT_TYPE_POLL) {
                *resp_len = 0;
                return PB_MGMT_XFER_NO_ANSWER;
            }
            return Board::Exchange(ep, req, req_len, resp, resp_cap, resp_len, ctx);
        }
    };
    board.client.ops.exchange = LoseAfterCommand::Exchange;
    API_CHECK(board.Poll() == PB_MGMT_POLL_UNENROLLED && StateOf(hub, job.removal_id) == RemovalState::Sent &&
                  !hub.registry.is_power_base_blocked(base_a) && HasBase(hub.registry.view(), base_a),
              "(the board is done; the hub knows nothing: 'sent', nothing blocked or removed)");
    API_CHECK(hub.management->SubmitReleaseReceipt(board.Receipt(), out) == ReceiptOutcome::Completed &&
                  out.state == RemovalState::Completed,
              "the receipt completes the removal directly");
    API_CHECK(hub.registry.is_power_base_blocked(base_a) && !HasBase(hub.registry.view(), base_a) &&
                  hub.registry.view().nodes.empty() && !hub.management->IsManaged(kEuiA),
              "and everything the notice would have done is done with it: blocked, the Base and its module gone");
    API_CHECK(out.settings_cleanup_pending && hub.management->ProcessSettingsCleanups() == 0 &&
                  hub.removed_modules.size() == 1 && hub.removed_modules[0] == std::vector<std::string>{FormatUuidHex(Module(7))},
              "including the settings clean-up of its module");
    API_CHECK(hub.StateFile().find(grant.key) == std::string::npos, "the key is dropped");
}

void ReenrollAfterRemoval(int *failures)
{
    std::printf("\n-- after a removal: only a new authenticated generation brings the Base back --\n");
    Hub hub;
    hub.Open();
    const ingest::Uuid base_a = Uuid(kEuiA);
    ManagementGrant grant;
    hub.management->IssueCredential(kEuiA, grant);
    Board board;
    board.Enroll(hub, kEuiA, grant);
    board.Poll();
    SendHeartbeat(hub.registry, base_a);
    RemovalJob job;
    RequestRemovalNow(*hub.management, "dev1", kEuiA, job);
    board.Poll();
    board.Poll();
    API_CHECK(StateOf(hub, job.removal_id) == RemovalState::Released && hub.registry.is_power_base_blocked(base_a),
              "(released: blocked; the receipt has not been relayed)");
    const std::vector<uint8_t> old_receipt = board.Receipt();
    std::vector<uint8_t> old_poll, old_proof;
    for (const auto &request : board.sent) {
        if (request[1] == PB_MGMT_TYPE_POLL) {
            old_poll = request;
        } else if (request[1] == PB_MGMT_TYPE_UNENROLL_DONE) {
            old_proof = request;
        }
    }

    // Heartbeats, however many, and a replayed poll do not lift the block.
    for (int i = 0; i < 5; i++) {
        SendHeartbeat(hub.registry, base_a);
    }
    API_CHECK(hub.management->HandleDatagram(old_poll).code == ingest::kCoapCodeUnauthorized &&
                  hub.registry.is_power_base_blocked(base_a),
              "heartbeats and a replayed poll of the released generation do not lift the block");

    ManagementGrant next;
    API_CHECK(hub.management->IssueCredential(kEuiA, next) && hub.registry.is_power_base_blocked(base_a) &&
                  SendHeartbeat(hub.registry, base_a) == ingest::kCoapCodeForbidden,
              "a new credential is issued (provisional); the block stays until the board proves it has the key");
    // A poll with the right generation and a wrong key.
    ManagementGrant forged = next;
    forged.key = std::string(64, '1');
    API_CHECK(hub.management->HandleDatagram(PollFor(kEuiA, forged)).code == ingest::kCoapCodeUnauthorized &&
                  hub.registry.is_power_base_blocked(base_a),
              "a poll naming the new generation but signed with another key: refused, still blocked");

    Board renewed;
    renewed.Enroll(hub, kEuiA, next);
    API_CHECK(renewed.Poll() == PB_MGMT_POLL_IDLE && !hub.registry.is_power_base_blocked(base_a) &&
                  !hub.management->IsBlocked(kEuiA) && hub.management->IsManaged(kEuiA),
              "the re-enrolled board's authenticated poll lifts the block");
    API_CHECK(SendHeartbeat(hub.registry, base_a) == ingest::kCoapCodeChanged && HasBase(hub.registry.view(), base_a),
              "and its heartbeat is accepted again");
    API_CHECK(hub.StateFile().find(grant.key) == std::string::npos && hub.StateFile().find(next.key) != std::string::npos,
              "the old generation's key is gone from the state file; the new one is there");

    // The old enrollment's messages mean nothing now.
    const uint64_t generation = hub.registry.generation();
    API_CHECK(hub.management->HandleDatagram(old_proof).code == ingest::kCoapCodeUnauthorized &&
                  hub.management->HandleDatagram(old_poll).code == ingest::kCoapCodeUnauthorized &&
                  hub.registry.generation() == generation && !hub.registry.is_power_base_blocked(base_a) &&
                  StateOf(hub, job.removal_id) == RemovalState::Superseded,
              "the old generation's notice and poll are refused and change nothing in the new enrollment");
    RemovalJob stale;
    API_CHECK(hub.management->SubmitReleaseReceipt(old_receipt, stale) == ReceiptOutcome::Rejected &&
                  StateOf(hub, job.removal_id) == RemovalState::Superseded && !hub.registry.is_power_base_blocked(base_a),
              "the old removal ended 'superseded' (its erase was never confirmed) and its receipt can no longer change that");
    // And the new enrollment can be removed in its turn.
    RemovalJob job2;
    API_CHECK(RequestRemovalNow(*hub.management, "dev1", kEuiA, job2) == RemovalRequestOutcome::Created &&
                  job2.generation == next.generation && job2.removal_id != job.removal_id,
              "a removal of the new enrollment is a new job bound to the new generation");
}

// Defect 2 as it was reported: with 64 Power Bases blocked, the 65th removal
// dropped the oldest block, and the board removed first was accepted again.
void BlockCapacity(int *failures)
{
    std::printf("\n-- blocks are never dropped to make room: capacity is reserved when a removal is requested --\n");
    Hub hub;
    hub.Open();
    struct Removed {
        std::string eui;
        ManagementGrant grant;
        std::array<uint8_t, 16> command{};
        std::string id;
    };
    auto eui_of = [](unsigned i) {
        char text[17];
        std::snprintf(text, sizeof(text), "6055f9fffe01%04x", i);
        return std::string(text);
    };
    auto enroll = [&](unsigned i, ManagementGrant &grant) {
        const std::string eui = eui_of(i);
        return hub.management->IssueCredential(eui, grant) && ActivateDirect(*hub.management, eui, grant) &&
               SendHeartbeat(hub.registry, Uuid(eui)) == ingest::kCoapCodeChanged &&
               SendTopology(hub.registry, Uuid(eui), Module(static_cast<uint8_t>(i)), 1) == ingest::kCoapCodeChanged;
    };
    std::vector<Removed> removed;
    bool all_ok = true;
    for (unsigned i = 1; i <= 62; i++) {
        Removed r;
        r.eui = eui_of(i);
        RemovalJob job;
        all_ok = all_ok && enroll(i, r.grant) &&
                 RequestRemovalNow(*hub.management, "dev", r.eui, job) == RemovalRequestOutcome::Created &&
                 ReleaseDirect(*hub.management, r.eui, r.grant, r.command) == ingest::kCoapCodeContent;
        r.id = job.removal_id;
        all_ok = all_ok && StateOf(hub, r.id) == RemovalState::Released;
        removed.push_back(r);
        g_now_ms += 1000;
    }
    API_CHECK(all_ok && hub.management->FreeBlockSlots() == 2, "(62 Power Bases enrolled, removed and released: two block slots left)");

    // Several removals at the limit at once: each open job holds a slot.
    Removed r63, r64, r65;
    r63.eui = eui_of(63);
    r64.eui = eui_of(64);
    r65.eui = eui_of(65);
    RemovalJob j63, j64, j65, out;
    API_CHECK(enroll(63, r63.grant) && enroll(64, r64.grant) && enroll(65, r65.grant), "(three more enrolled)");
    API_CHECK(RequestRemovalNow(*hub.management, "dev", r63.eui, j63) == RemovalRequestOutcome::Created &&
                  hub.management->FreeBlockSlots() == 1,
              "a removal request reserves a slot the moment it is accepted, long before the board reports");
    API_CHECK(RequestRemovalNow(*hub.management, "dev", r64.eui, j64) == RemovalRequestOutcome::Created &&
                  hub.management->FreeBlockSlots() == 0,
              "the next one takes the last slot");
    const std::string before_refusal = hub.StateFile();
    API_CHECK(RequestRemovalNow(*hub.management, "dev", r65.eui, j65) == RemovalRequestOutcome::CapacityFull &&
                  j65.removal_id.empty() && hub.StateFile() == before_refusal,
              "a third at the same time is refused -- although only 62 Bases are blocked yet -- and nothing is stored");
    API_CHECK(RequestRemovalNow(*hub.management, "dev", r63.eui, out) == RemovalRequestOutcome::Existing &&
                  out.removal_id == j63.removal_id && hub.management->FreeBlockSlots() == 0,
              "asking again for a Base that already has a job is the same job and takes no second slot");
    // The refused board: no destructive command, ever.
    std::array<uint8_t, 16> none{};
    bool commanded = false;
    for (int i = 0; i < 4; i++) {
        commanded = commanded || FetchCommandDirect(*hub.management, r65.eui, r65.grant, none);
    }
    API_CHECK(!commanded && hub.management->IsManaged(r65.eui) && !hub.registry.is_power_base_blocked(Uuid(r65.eui)) &&
                  SendHeartbeat(hub.registry, Uuid(r65.eui)) == ingest::kCoapCodeChanged,
              "the refused board gets no command however often it polls; it stays enrolled and its data is accepted");

    // Both reserved removals can finish in any order; a storage failure in
    // between loses nothing.
    API_CHECK(FetchCommandDirect(*hub.management, r64.eui, r64.grant, r64.command), "(the 64th board has its command)");
    hub.fail_next_writes = 1;
    API_CHECK(hub.management->HandleDatagram(DoneFor(r64.eui, r64.grant, r64.command, PB_MGMT_RESULT_RELEASED)).code ==
                      ingest::kCoapCodeServiceUnavailable &&
                  StateOf(hub, j64.removal_id) == RemovalState::Sent && !hub.registry.is_power_base_blocked(Uuid(r64.eui)) &&
                  hub.management->FreeBlockSlots() == 0,
              "its released notice arrives while the state cannot be stored: 5.03, nothing changes, the slot stays reserved");
    API_CHECK(ReleaseDirect(*hub.management, r63.eui, r63.grant, r63.command) == ingest::kCoapCodeContent &&
                  hub.management->HandleDatagram(DoneFor(r64.eui, r64.grant, r64.command, PB_MGMT_RESULT_RELEASED)).code ==
                      ingest::kCoapCodeContent &&
                  StateOf(hub, j63.removal_id) == RemovalState::Released && StateOf(hub, j64.removal_id) == RemovalState::Released,
              "both boards report (the 64th again): both are released -- the reserved slots were there");
    r63.id = j63.removal_id;
    r64.id = j64.removal_id;
    removed.push_back(r63);
    removed.push_back(r64);

    auto all_blocked = [&](ingest::Registry &registry) {
        bool ok = true;
        for (size_t i = 0; i < removed.size(); i++) {
            const ingest::Uuid base = Uuid(removed[i].eui);
            ok = ok && registry.is_power_base_blocked(base) && SendHeartbeat(registry, base) == ingest::kCoapCodeForbidden &&
                 SendTopology(registry, base, Module(static_cast<uint8_t>(i + 1)), 9) == ingest::kCoapCodeForbidden &&
                 SendSample(registry, base, Module(static_cast<uint8_t>(i + 1)), 9) == ingest::kCoapCodeForbidden;
        }
        return ok;
    };
    API_CHECK(removed.size() == kMaxBlockedPowerBases && all_blocked(hub.registry) && hub.management->FreeBlockSlots() == 0,
              "64 Power Bases are blocked: heartbeat, topology and sample of every one of them are refused (4.03)");

    // The 65th.
    API_CHECK(RequestRemovalNow(*hub.management, "dev", r65.eui, j65) == RemovalRequestOutcome::CapacityFull &&
                  RequestRemovalNow(*hub.management, "other-device", r65.eui, j65) == RemovalRequestOutcome::CapacityFull,
              "the 65th removal is refused, whoever asks");
    API_CHECK(all_blocked(hub.registry) && hub.registry.is_power_base_blocked(Uuid(removed[0].eui)) &&
                  SendHeartbeat(hub.registry, Uuid(removed[0].eui)) == ingest::kCoapCodeForbidden,
              "and the Base removed FIRST is still blocked: its old heartbeat is still refused");
    API_CHECK(!FetchCommandDirect(*hub.management, r65.eui, r65.grant, none) && hub.management->IsManaged(r65.eui),
              "the 65th board still gets no command");

    // Neither time, nor a completed job, nor its pruning frees a slot.
    g_now_ms += 30LL * 24 * 3600 * 1000;
    API_CHECK(hub.management->SubmitReleaseReceipt(DoneFor(removed[1].eui, removed[1].grant, removed[1].command, PB_MGMT_RESULT_ERASED), out) ==
                      ReceiptOutcome::Completed &&
                  hub.management->FreeBlockSlots() == 0 && hub.registry.is_power_base_blocked(Uuid(removed[1].eui)),
              "a month later one removal is completed by its receipt: the block stays, no slot is freed");
    g_now_ms += 60 * 60 * 1000;
    {
        bool listed = false;
        for (const RemovalJob &j : hub.management->ListRemovals()) {
            listed = listed || j.removal_id == removed[1].id;
        }
        API_CHECK(RequestRemovalNow(*hub.management, "dev", r65.eui, j65) == RemovalRequestOutcome::CapacityFull && !listed &&
                      all_blocked(hub.registry),
                  "when that finished job is no longer in the list of removals: still blocked, still no slot");
    }

    // A restart changes nothing.
    {
        ingest::Registry fresh;
        PowerBaseManagementConfig config;
        config.directory = hub.dir;
        config.hub_id = kHubId;
        config.now_unix_ms = []() { return g_now_ms; };
        PowerBaseManagement reopened(fresh, config);
        std::string error;
        RemovalJob refused;
        API_CHECK(reopened.Load(error) && all_blocked(fresh) && reopened.FreeBlockSlots() == 0,
                  "hub restart: all 64 blocks are restored into the new Registry, the first one included");
        API_CHECK(RequestRemovalNow(reopened, "dev", r65.eui, refused) == RemovalRequestOutcome::CapacityFull,
                  "and the 65th removal is refused there as well");
    }

    // The one thing that frees a slot: the removed board is enrolled again.
    ManagementGrant renewed;
    API_CHECK(hub.management->IssueCredential(removed[0].eui, renewed) && hub.management->FreeBlockSlots() == 0 &&
                  hub.registry.is_power_base_blocked(Uuid(removed[0].eui)),
              "a new enrollment session for a removed board alone frees nothing");
    API_CHECK(ActivateDirect(*hub.management, removed[0].eui, renewed) && hub.management->FreeBlockSlots() == 1 &&
                  !hub.registry.is_power_base_blocked(Uuid(removed[0].eui)) &&
                  SendHeartbeat(hub.registry, Uuid(removed[0].eui)) == ingest::kCoapCodeChanged &&
                  StateOf(hub, removed[0].id) == RemovalState::Superseded,
              "its first poll authenticated with the NEW generation's key lifts its block and frees one slot");
    API_CHECK(hub.management->HandleDatagram(DoneFor(removed[0].eui, removed[0].grant, removed[0].command, PB_MGMT_RESULT_RELEASED)).code ==
                      ingest::kCoapCodeUnauthorized &&
                  hub.management->SubmitReleaseReceipt(DoneFor(removed[0].eui, removed[0].grant, removed[0].command, PB_MGMT_RESULT_ERASED), out) ==
                      ReceiptOutcome::Rejected &&
                  hub.management->HandleDatagram(PollFor(removed[0].eui, removed[0].grant)).code == ingest::kCoapCodeUnauthorized &&
                  !hub.registry.is_power_base_blocked(Uuid(removed[0].eui)) && hub.management->IsManaged(removed[0].eui),
              "the earlier generation's notice, receipt and poll are refused and do not touch the new enrollment");
    removed.erase(removed.begin());
    API_CHECK(all_blocked(hub.registry), "every other removed Base is still blocked");

    // The freed slot, a storage failure, a cancel.
    hub.fail_writes = true;
    API_CHECK(RequestRemovalNow(*hub.management, "dev", r65.eui, j65) == RemovalRequestOutcome::Unavailable &&
                  hub.management->FreeBlockSlots() == 1,
              "a request that cannot be stored reserves nothing");
    hub.fail_writes = false;
    {
        ingest::Registry fresh;
        PowerBaseManagementConfig config;
        config.directory = hub.dir;
        config.hub_id = kHubId;
        config.now_unix_ms = []() { return g_now_ms; };
        PowerBaseManagement reopened(fresh, config);
        std::string error;
        API_CHECK(reopened.Load(error) && reopened.FreeBlockSlots() == 1, "(and after a restart the count is the same)");
    }
    API_CHECK(RequestRemovalNow(*hub.management, "dev", r65.eui, j65) == RemovalRequestOutcome::Created &&
                  hub.management->FreeBlockSlots() == 0,
              "now the 65th removal is accepted, and holds the slot");
    Removed r66;
    r66.eui = eui_of(66);
    RemovalJob j66;
    API_CHECK(enroll(66, r66.grant) && RequestRemovalNow(*hub.management, "dev", r66.eui, j66) == RemovalRequestOutcome::CapacityFull,
              "so a 66th is refused while that job is only pending");
    API_CHECK(hub.management->CancelRemoval(j65.removal_id, out) == RemovalCancelOutcome::Cancelled &&
                  hub.management->FreeBlockSlots() == 1,
              "cancelling the pending job gives its slot back");
    API_CHECK(RequestRemovalNow(*hub.management, "dev", r66.eui, j66) == RemovalRequestOutcome::Created &&
                  RequestRemovalNow(*hub.management, "dev", r65.eui, j65) == RemovalRequestOutcome::CapacityFull,
              "which the 66th can then take -- and the 65th is refused again");
    API_CHECK(!FetchCommandDirect(*hub.management, r65.eui, r65.grant, none) &&
                  ReleaseDirect(*hub.management, r66.eui, r66.grant, r66.command) == ingest::kCoapCodeContent &&
                  hub.registry.is_power_base_blocked(Uuid(r66.eui)) && hub.management->FreeBlockSlots() == 0 && all_blocked(hub.registry),
              "the 66th is released into the reserved slot; nothing else was unblocked for it");
    // A repeated notice and a repeated receipt are the same answers.
    API_CHECK(hub.management->HandleDatagram(DoneFor(r66.eui, r66.grant, r66.command, PB_MGMT_RESULT_RELEASED)).code == ingest::kCoapCodeContent &&
                  hub.management->SubmitReleaseReceipt(DoneFor(r66.eui, r66.grant, r66.command, PB_MGMT_RESULT_ERASED), out) == ReceiptOutcome::Completed &&
                  hub.management->SubmitReleaseReceipt(DoneFor(r66.eui, r66.grant, r66.command, PB_MGMT_RESULT_ERASED), out) == ReceiptOutcome::AlreadyCompleted &&
                  hub.management->FreeBlockSlots() == 0 && hub.registry.is_power_base_blocked(Uuid(r66.eui)),
              "its notice again and its receipt twice: idempotent, the block and the slot count unchanged");

    // The credential limit (96) next to the block limit (64): credentials of
    // released removals are kept for their receipts and are never the ones
    // that give way.
    size_t issued = 0;
    ManagementGrant last;
    std::string last_eui;
    for (unsigned i = 100; i < 300; i++) {
        ManagementGrant g;
        if (!hub.management->IssueCredential(eui_of(i), g)) {
            break;
        }
        if (!ActivateDirect(*hub.management, eui_of(i), g)) {
            break;
        }
        issued++;
        last = g;
        last_eui = eui_of(i);
    }
    ManagementGrant over;
    API_CHECK(issued > 0 && issued < 200 && !hub.management->IssueCredential("6055f9fffe01ffff", over) && over.key.empty(),
              "enrolling more Bases stops at the credential limit: a session that cannot be stored is refused");
    API_CHECK(all_blocked(hub.registry) && hub.management->IsManaged(last_eui) && hub.management->IsManaged(r65.eui) &&
                  hub.management->SubmitReleaseReceipt(DoneFor(removed[5].eui, removed[5].grant, removed[5].command, PB_MGMT_RESULT_ERASED), out) ==
                      ReceiptOutcome::Completed,
              "no block, no active credential and no released credential was given up for it: a receipt still verifies");
    API_CHECK(hub.management->IssueCredential("6055f9fffe01ffff", over) && !over.key.empty() && all_blocked(hub.registry),
              "the credential that receipt no longer needs made room for one more enrollment; every block is still there");
}

// Defect 3 as it was reported: the room clean-up ran once, from RAM, and a
// failure or a process end before it was lost for good.
void SettingsCleanupDurability(int *failures)
{
    std::printf("\n-- the settings clean-up is stored with the release and retried until it is done --\n");
    const ingest::Uuid base_a = Uuid(kEuiA), base_b = Uuid(kEuiB);
    const ingest::Uuid m1 = Module(1), m2 = Module(2);
    // A Base with two modules, released; returns the job id.
    auto released_hub = [&](Hub &hub, ManagementGrant &grant, std::array<uint8_t, 16> &command) {
        hub.Open();
        hub.management->IssueCredential(kEuiA, grant);
        ActivateDirect(*hub.management, kEuiA, grant);
        SendHeartbeat(hub.registry, base_a);
        SendTopology(hub.registry, base_a, m1, 1);
        SendTopology(hub.registry, base_a, m2, 1);
        RemovalJob job;
        RequestRemovalNow(*hub.management, "dev", kEuiA, job);
        ReleaseDirect(*hub.management, kEuiA, grant, command);
        return job.removal_id;
    };
    const std::vector<std::string> both = {FormatUuidHex(m1), FormatUuidHex(m2)};
    auto sorted = [](std::vector<std::string> v) {
        std::sort(v.begin(), v.end());
        return v;
    };

    {
        std::printf("   (the first attempt fails, a later one succeeds)\n");
        Hub hub;
        ManagementGrant grant;
        std::array<uint8_t, 16> command{};
        const std::string id = released_hub(hub, grant, command);
        RemovalJob job;
        API_CHECK(hub.management->GetRemoval(id, job) && job.state == RemovalState::Released && job.settings_cleanup_pending &&
                      hub.removed_modules.empty(),
                  "released; the clean-up is owed and nothing ran under the lock or on the ingest thread");
        hub.unassign_ok = false;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 1 && hub.removed_modules.size() == 1 &&
                      sorted(hub.removed_modules[0]) == sorted(both),
                  "first attempt: the settings cannot be written -- the clean-up stays pending");
        API_CHECK(hub.management->GetRemoval(id, job) && job.state == RemovalState::Released && job.settings_cleanup_pending &&
                      RemovalJobJson(job).find("\"settings_cleanup_pending\":true") != std::string::npos,
                  "the job says so: released, clean-up pending -- two different things");
        API_CHECK(hub.management->ProcessSettingsCleanups() == 1 && hub.removed_modules.size() == 1,
                  "it is not hammered: nothing is tried before the retry time");
        g_now_ms += 999;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 1 && hub.removed_modules.size() == 1, "(not one millisecond early)");
        g_now_ms += 1;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 1 && hub.removed_modules.size() == 2,
                  "after the first backoff it is tried again (and fails again)");
        g_now_ms += 1999;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 1 && hub.removed_modules.size() == 2, "the backoff doubles");
        // A long outage: the wait is bounded.
        for (int i = 0; i < 12; i++) {
            g_now_ms += 8000;
            hub.management->ProcessSettingsCleanups();
        }
        API_CHECK(hub.removed_modules.size() == 14, "and is capped: one attempt per maximum backoff, for as long as it takes");
        hub.unassign_ok = true;
        g_now_ms += 8000;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && hub.management->GetRemoval(id, job) &&
                      !job.settings_cleanup_pending && hub.StateFile().find("\"cleanups\":[]") != std::string::npos,
                  "when the settings can be written again the clean-up is done and nothing is owed");
        g_now_ms += 100000;
        const size_t calls = hub.removed_modules.size();
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && hub.removed_modules.size() == calls, "and it is not done again");
        // A repeated notice, and the receipt, start no second clean-up.
        RemovalJob out;
        API_CHECK(hub.management->HandleDatagram(DoneFor(kEuiA, grant, command, PB_MGMT_RESULT_RELEASED)).code == ingest::kCoapCodeContent &&
                      hub.management->PendingSettingsCleanups() == 0 &&
                      hub.management->SubmitReleaseReceipt(DoneFor(kEuiA, grant, command, PB_MGMT_RESULT_ERASED), out) == ReceiptOutcome::Completed &&
                      hub.management->PendingSettingsCleanups() == 0 && hub.management->ProcessSettingsCleanups() == 0 &&
                      hub.removed_modules.size() == calls,
                  "the notice again and the receipt afterwards: no second clean-up");
    }

    {
        std::printf("   (the process ends right after the release was stored, before any clean-up ran)\n");
        Hub hub;
        ManagementGrant grant;
        std::array<uint8_t, 16> command{};
        const std::string id = released_hub(hub, grant, command);
        API_CHECK(hub.removed_modules.empty() && hub.StateFile().find(FormatUuidHex(m1)) != std::string::npos &&
                      hub.StateFile().find(FormatUuidHex(m2)) != std::string::npos,
                  "the modules are named in the state file by the same write that stored the release");
        // The process is gone. A new one: new manager, new (empty) Registry.
        hub.management.reset();
        hub.registry.remove_power_base(base_a);
        hub.cleanup_grace = std::chrono::milliseconds(60000);
        RemovalJob job;
        API_CHECK(hub.Open() && hub.management->PendingSettingsCleanups() == 1 && hub.management->GetRemoval(id, job) &&
                      job.settings_cleanup_pending,
                  "after the restart the clean-up is still owed");
        API_CHECK(hub.management->ProcessSettingsCleanups() == 1 && hub.removed_modules.empty(),
                  "it waits out the start-up grace: the Registry is empty until the Bases have reported again");
        // Meanwhile one of the two modules turns out to live on another Base.
        SendHeartbeat(hub.registry, base_b);
        SendTopology(hub.registry, base_b, m2, 1);
        g_now_ms += 60000;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && hub.removed_modules.size() == 1 &&
                      hub.removed_modules[0] == std::vector<std::string>{FormatUuidHex(m1)},
                  "then it runs: for the module that is registered nowhere -- not for the one that reported under another Base");
        // Restart after restart: nothing more.
        for (int i = 0; i < 3; i++) {
            hub.management.reset();
            hub.Open();
            g_now_ms += 120000;
            hub.management->ProcessSettingsCleanups();
        }
        API_CHECK(hub.removed_modules.size() == 1 && hub.management->PendingSettingsCleanups() == 0,
                  "three more restarts: the clean-up is not repeated");
    }

    {
        std::printf("   (the clean-up succeeds but that it did cannot be stored)\n");
        Hub hub;
        ManagementGrant grant;
        std::array<uint8_t, 16> command{};
        const std::string id = released_hub(hub, grant, command);
        hub.fail_next_writes = 1;
        RemovalJob job;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 1 && hub.removed_modules.size() == 1 &&
                      hub.management->GetRemoval(id, job) && job.settings_cleanup_pending,
                  "the rooms were cleared, the mark could not be written: still pending (it is not assumed done)");
        // And the process ends here: what the next one finds is the file.
        hub.management.reset();
        hub.registry.remove_power_base(base_a);
        API_CHECK(hub.Open() && hub.management->PendingSettingsCleanups() == 1, "a restart finds it pending in the file");
        g_now_ms += 10000;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && hub.removed_modules.size() == 2 &&
                      sorted(hub.removed_modules[1]) == sorted(both),
                  "it is done again for the same modules (the clean-up is idempotent) and this time the mark is stored");
    }

    {
        std::printf("   (a module moves to another Base, or its Base is enrolled again, before the retry)\n");
        Hub hub;
        ManagementGrant grant;
        std::array<uint8_t, 16> command{};
        const std::string id = released_hub(hub, grant, command);
        hub.unassign_ok = false;
        hub.management->ProcessSettingsCleanups();
        API_CHECK(hub.removed_modules.size() == 1 && sorted(hub.removed_modules[0]) == sorted(both), "(first attempt failed for both modules)");
        // m1 is docked onto Base B and given a room there by the user.
        SendHeartbeat(hub.registry, base_b);
        SendTopology(hub.registry, base_b, m1, 1);
        hub.unassign_ok = true;
        g_now_ms += 1000;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && hub.removed_modules.size() == 2 &&
                      hub.removed_modules[1] == std::vector<std::string>{FormatUuidHex(m2)},
                  "the retry asks where each module is NOW: the one registered under Base B keeps its room");
        // Later m1 leaves Base B again: its room there was the user's, and
        // this old clean-up must not come back for it.
        hub.registry.remove_power_base(base_b);
        g_now_ms += 100000;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && hub.removed_modules.size() == 2,
                  "and it has left the clean-up for good");

        // The same Base enrolled again with its modules before the retry.
        Hub again;
        ManagementGrant g1, g2;
        std::array<uint8_t, 16> c1{};
        released_hub(again, g1, c1);
        again.unassign_ok = false;
        again.management->ProcessSettingsCleanups();
        again.management->IssueCredential(kEuiA, g2);
        ActivateDirect(*again.management, kEuiA, g2);
        SendHeartbeat(again.registry, base_a);
        SendTopology(again.registry, base_a, m1, 5);
        SendTopology(again.registry, base_a, m2, 5);
        again.unassign_ok = true;
        g_now_ms += 1000;
        API_CHECK(again.management->ProcessSettingsCleanups() == 0 && again.removed_modules.size() == 1,
                  "a Base enrolled again (new generation) with its modules: the old removal's clean-up touches none of them");
    }

    {
        std::printf("   (the real settings store: only the removed modules' rooms change)\n");
        const std::string dir = test_support::MakeTempDir();
        bool settings_writable = true;
        auto store = std::make_shared<SettingsStore>(dir, [&](const std::string &path, const std::string &contents, std::string &error) {
            if (!settings_writable) {
                error = "injected settings write failure";
                return false;
            }
            return WriteFileAtomically(path, contents, error);
        });
        store->Load();
        const std::string room = "11111111-2222-4333-8444-555555555555", other_room = "22222222-2222-4333-8444-555555555555";
        auto module = [&](const ingest::Uuid &uuid, const std::string &name, const std::string &room_id) {
            ModuleSetting m;
            m.uuid = FormatUuidHex(uuid);
            m.has_name = !name.empty();
            m.name = name;
            m.has_room = !room_id.empty();
            m.room_id = room_id;
            return m;
        };
        HomeSettings settings;
        settings.has_home_name = true;
        settings.home_name = "Home";
        for (const std::string &r : {room, other_room}) {
            RoomSetting rs;
            rs.id = r;
            rs.name_kind = RoomNameKind::Custom;
            rs.name = r == room ? "Study" : "Hall";
            rs.symbol = "books.vertical";
            settings.rooms.push_back(rs);
        }
        const ingest::Uuid unrelated = Module(9);
        settings.modules = {module(m1, "Desk", room), module(m2, "", room), module(unrelated, "Door", other_room)};
        API_CHECK(store->Update("", settings, 1000).outcome == SettingsUpdateOutcome::Saved, "(settings: three modules in rooms)");

        Hub hub;
        hub.Open();
        // The production hook, around the real store; a competing writer can
        // be put between its read and its write.
        int competing_writes = 0;
        bool compete = false;
        auto real = MakeRoomUnassigner(store, []() { return g_now_ms; });
        PowerBaseManagementConfig config;
        config.directory = hub.dir;
        config.hub_id = kHubId;
        config.now_unix_ms = []() { return g_now_ms; };
        config.cleanup_startup_grace = std::chrono::milliseconds(0);
        config.cleanup_retry_min = std::chrono::milliseconds(1000);
        config.read_rooms = MakeRoomReader(store);
        int hook_calls = 0;
        config.unassign_rooms = [&](const std::vector<RoomCleanupTarget> &modules, const RoomCleanupGuard &still_owed) {
            hook_calls++;
            if (!compete) {
                return real(modules, still_owed);
            }
            // Other devices keep saving between this clean-up's read and its
            // write, every time it tries.
            RoomUnassignOutcome outcome = UnassignModuleRooms(*store, modules, g_now_ms, still_owed, [&]() {
                SettingsSnapshot current = store->Get();
                HomeSettings changed = current.settings;
                changed.home_name = "Home " + std::to_string(++competing_writes);
                store->Update(current.revision, changed, g_now_ms);
            });
            return outcome == RoomUnassignOutcome::Saved || outcome == RoomUnassignOutcome::Nothing;
        };
        hub.management = std::make_unique<PowerBaseManagement>(hub.registry, std::move(config));
        std::string error;
        hub.management->Load(error);
        ManagementGrant grant;
        hub.management->IssueCredential(kEuiA, grant);
        ActivateDirect(*hub.management, kEuiA, grant);
        SendHeartbeat(hub.registry, base_a);
        SendTopology(hub.registry, base_a, m1, 1);
        SendTopology(hub.registry, base_a, m2, 1);
        RemovalJob job;
        RequestRemovalNow(*hub.management, "dev", kEuiA, job);
        std::array<uint8_t, 16> command{};
        ReleaseDirect(*hub.management, kEuiA, grant, command);

        // 1. The settings file cannot be written.
        settings_writable = false;
        const std::string revision0 = store->Get().revision;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 1 && hook_calls == 1 && store->Get().revision == revision0,
                  "the settings write fails: reported as a failure (not swallowed), the clean-up stays pending");
        settings_writable = true;
        // 2. Other writers win every attempt of one pass.
        compete = true;
        g_now_ms += 1000;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 1 && hook_calls == 2 && competing_writes == 4,
                  "other writers win all four attempts of a pass (revision conflicts): still pending, tried again later");
        auto find = [&](const ingest::Uuid &uuid) -> const ModuleSetting * {
            static SettingsSnapshot snapshot;
            snapshot = store->Get();
            for (const ModuleSetting &m : snapshot.settings.modules) {
                if (m.uuid == FormatUuidHex(uuid)) {
                    return &m;
                }
            }
            return nullptr;
        };
        API_CHECK(find(m1) != nullptr && find(m1)->has_room && store->Get().settings.home_name == "Home 4",
                  "(the rooms are untouched so far; the other writers' changes are all there)");
        // 3. Meanwhile a user renames the unrelated module and moves it.
        {
            SettingsSnapshot current = store->Get();
            HomeSettings changed = current.settings;
            for (ModuleSetting &m : changed.modules) {
                if (m.uuid == FormatUuidHex(unrelated)) {
                    m.name = "Front door";
                    m.room_id = room;
                }
            }
            API_CHECK(store->Update(current.revision, changed, g_now_ms).outcome == SettingsUpdateOutcome::Saved,
                      "(a user edits an unrelated module in between)");
        }
        compete = false;
        const std::string revision_before = store->Get().revision;
        g_now_ms += 2000;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && hook_calls == 3, "the next pass gets through: done");
        API_CHECK(store->Get().revision != revision_before, "as one ordinary new revision");
        API_CHECK(find(m1) != nullptr && !find(m1)->has_room && find(m1)->name == "Desk",
                  "the removed module with a name: no room, name kept");
        API_CHECK(find(m2) == nullptr, "the removed module that had only a room: its entry is gone");
        API_CHECK(find(unrelated) != nullptr && find(unrelated)->has_room && find(unrelated)->room_id == room &&
                      find(unrelated)->name == "Front door",
                  "the unrelated module is exactly as the user left it a moment ago");
        API_CHECK(store->Get().settings.rooms.size() == 2 && store->Get().settings.home_name == "Home 4",
                  "rooms and the home name are untouched");
        const std::string revision_done = store->Get().revision;
        g_now_ms += 100000;
        hub.management.reset();
        API_CHECK(hook_calls == 3 && store->Get().revision == revision_done, "nothing is written again afterwards");
        test_support::RemoveTree(dir);
    }

    {
        std::printf("   (the worker thread)\n");
        Hub hub;
        ManagementGrant grant;
        std::array<uint8_t, 16> command{};
        hub.Open();
        hub.management->StartCleanupWorker(std::chrono::milliseconds(20));
        hub.management->IssueCredential(kEuiA, grant);
        ActivateDirect(*hub.management, kEuiA, grant);
        SendHeartbeat(hub.registry, base_a);
        SendTopology(hub.registry, base_a, m1, 1);
        RemovalJob job;
        RequestRemovalNow(*hub.management, "dev", kEuiA, job);
        const std::thread::id ingest_thread = std::this_thread::get_id();
        std::thread::id hook_thread;
        // While the settings are being written, another thread uses the
        // manager and the Registry. Were the write done under either mutex,
        // that thread could not answer before the hook returns.
        bool others_served = false;
        std::thread other;
        hub.on_unassign = [&]() {
            hook_thread = std::this_thread::get_id();
            auto answered = std::make_shared<std::promise<void>>();
            std::future<void> waiting = answered->get_future();
            other = std::thread([&, answered]() {
                RemovalJob seen;
                hub.management->GetRemoval(job.removal_id, seen);
                hub.management->ListRemovals();
                hub.management->IsBlocked(kEuiA);
                hub.registry.generation();
                SendHeartbeat(hub.registry, base_b);
                answered->set_value();
            });
            others_served = waiting.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        };
        ReleaseDirect(*hub.management, kEuiA, grant, command);
        bool done = false;
        for (int i = 0; i < 2000 && !done; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            done = hub.management->PendingSettingsCleanups() == 0;
        }
        if (other.joinable()) {
            other.join();
        }
        API_CHECK(done && hub.removed_modules.size() == 1, "with the worker started, a release is followed by its clean-up without anyone asking");
        API_CHECK(hook_thread != std::thread::id() && hook_thread != ingest_thread,
                  "and the settings write runs on the worker's thread, not on the thread that handled the board's message");
        API_CHECK(others_served,
                  "while it is being written, the manager and the Registry answer another thread: no mutex of theirs is held");
        hub.management.reset(); // joins the worker
        API_CHECK(true, "the worker stops with the manager");
    }
}

void RejectedDatagrams(int *failures)
{
    std::printf("\n-- what the management resource refuses --\n");
    Hub hub;
    hub.Open();
    ManagementGrant grant;
    hub.management->IssueCredential(kEuiA, grant);
    Board board;
    board.Enroll(hub, kEuiA, grant);
    board.Poll();
    SendHeartbeat(hub.registry, Uuid(kEuiA));
    RemovalJob job;
    RequestRemovalNow(*hub.management, "dev1", kEuiA, job);
    const std::string before = hub.StateFile();

    auto code = [&](std::vector<uint8_t> datagram) { return hub.management->HandleDatagram(datagram).code; };
    std::vector<uint8_t> good = PollFor(kEuiA, grant);
    API_CHECK(code({}) == ingest::kCoapCodeBadRequest && code({1, 2, 3}) == ingest::kCoapCodeBadRequest,
              "empty and short datagrams: 4.00");
    std::vector<uint8_t> t = good;
    t[0] = 2;
    API_CHECK(code(t) == ingest::kCoapCodeBadRequest, "another protocol version: 4.00");
    t = good;
    t.push_back(0);
    API_CHECK(code(t) == ingest::kCoapCodeBadRequest, "a trailing byte: 4.00");
    t = good;
    t[20] ^= 1;
    API_CHECK(code(t) == ingest::kCoapCodeUnauthorized, "another hub id: 4.01");
    t = good;
    t[5] ^= 1;
    API_CHECK(code(t) == ingest::kCoapCodeUnauthorized, "another board's EUI-64: 4.01");
    t = good;
    t[40] ^= 1;
    API_CHECK(code(t) == ingest::kCoapCodeUnauthorized, "an unknown generation: 4.01");
    t = good;
    t[50] ^= 1;
    API_CHECK(code(t) == ingest::kCoapCodeUnauthorized, "a changed nonce byte (MAC no longer fits): 4.01");
    t = good;
    t.back() ^= 1;
    API_CHECK(code(t) == ingest::kCoapCodeUnauthorized, "a changed MAC byte: 4.01");
    ManagementGrant other = grant;
    other.key = std::string(64, '7');
    API_CHECK(code(PollFor(kEuiA, other)) == ingest::kCoapCodeUnauthorized, "signed with another key: 4.01");
    API_CHECK(code(PollFor(kEuiB, grant)) == ingest::kCoapCodeUnauthorized,
              "this credential used for another board: 4.01");

    // Messages only a hub sends.
    pb_mgmt_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.type = PB_MGMT_TYPE_UNENROLL;
    auto eui = Bytes<8>(kEuiA);
    auto hub_id = Bytes<16>(kHubId);
    auto generation = Bytes<16>(grant.generation);
    auto key = Bytes<32>(grant.key);
    std::memcpy(m.id.eui64, eui.data(), 8);
    std::memcpy(m.id.hub_id, hub_id.data(), 16);
    std::memcpy(m.id.generation, generation.data(), 16);
    RAND_bytes(m.board_nonce, 16);
    RAND_bytes(m.command_id, 16);
    std::vector<uint8_t> wire(PB_MGMT_MAX_MESSAGE_LEN);
    wire.resize(pb_mgmt_pack(&m, key.data(), Hmac, nullptr, wire.data(), wire.size()));
    API_CHECK(code(wire) == ingest::kCoapCodeBadRequest, "a correctly signed UNENROLL sent TO the hub: 4.00");

    // A proof for a command the hub never issued.
    std::memset(&m.board_nonce, 0, sizeof(m.board_nonce));
    m.type = PB_MGMT_TYPE_UNENROLL_DONE;
    m.result = PB_MGMT_RESULT_RELEASED;
    wire.assign(PB_MGMT_MAX_MESSAGE_LEN, 0);
    wire.resize(pb_mgmt_pack(&m, key.data(), Hmac, nullptr, wire.data(), wire.size()));
    API_CHECK(code(wire) == ingest::kCoapCodeNotFound && StateOf(hub, job.removal_id) == RemovalState::Pending,
              "a signed released notice with a command id the hub never issued: 4.04, the job is untouched");
    m.result = PB_MGMT_RESULT_ERASED;
    wire.assign(PB_MGMT_MAX_MESSAGE_LEN, 0);
    wire.resize(pb_mgmt_pack(&m, key.data(), Hmac, nullptr, wire.data(), wire.size()));
    RemovalJob not_ours;
    API_CHECK(code(wire) == ingest::kCoapCodeNotFound &&
                  hub.management->SubmitReleaseReceipt(wire, not_ours) == ReceiptOutcome::Rejected &&
                  StateOf(hub, job.removal_id) == RemovalState::Pending,
              "a signed receipt with a command id the hub never issued: refused both ways, the job is untouched");
    API_CHECK(hub.StateFile() == before && !hub.registry.is_power_base_blocked(Uuid(kEuiA)),
              "none of these changed the stored state or blocked anything");

    // A captured poll cannot fetch the command.
    std::array<uint8_t, 16> nonce{};
    {
        ingest::ManagementReply idle = hub.management->HandleDatagram(PollFor(kEuiA, grant));
        pb_mgmt_msg_t a;
        API_CHECK(idle.code == ingest::kCoapCodeContent && pb_mgmt_parse(idle.payload.data(), idle.payload.size(), &a) &&
                      a.type == PB_MGMT_TYPE_IDLE && (a.flags & PB_MGMT_IDLE_F_COMMAND_PENDING) != 0,
                  "(a poll without the nonce gets IDLE with 'command pending')");
        std::memcpy(nonce.data(), a.hub_nonce, 16);
    }
    std::vector<uint8_t> echoing = PollFor(kEuiA, grant, nonce);
    ingest::ManagementReply first = hub.management->HandleDatagram(echoing);
    pb_mgmt_msg_t parsed;
    API_CHECK(first.code == ingest::kCoapCodeContent && pb_mgmt_parse(first.payload.data(), first.payload.size(), &parsed) &&
                  parsed.type == PB_MGMT_TYPE_UNENROLL,
              "the poll that echoes the hub's nonce gets the command");
    ingest::ManagementReply replay = hub.management->HandleDatagram(echoing);
    API_CHECK(replay.code == ingest::kCoapCodeContent && pb_mgmt_parse(replay.payload.data(), replay.payload.size(), &parsed) &&
                  parsed.type == PB_MGMT_TYPE_IDLE,
              "the very same poll played again gets IDLE: the nonce was good for one use");

    // Through the ingest dispatch: the resource exists only with a handler,
    // and never reaches the Registry.
    ingest::CoapMessage coap;
    coap.code = ingest::kCoapCodePost;
    coap.uri_path = ingest::kManagementUriPath;
    coap.payload = good;
    const uint64_t registry_generation = hub.registry.generation();
    API_CHECK(ingest::handle_request(hub.registry, coap, nullptr).code == ingest::kCoapCodeNotFound &&
                  ingest::handle_request(hub.registry, coap) == ingest::kCoapCodeBadRequest &&
                  hub.registry.generation() == registry_generation,
              "without a handler the resource is 4.04 (4.00 through the old entry point, as on an older hub)");
    coap.uri_path = "pogo/v1/heartbeat";
    API_CHECK(ingest::handle_request(hub.registry, coap) == ingest::kCoapCodeBadRequest,
              "a management message posted to a data resource is not a data message");
}

void LimitsAndStorage(int *failures)
{
    std::printf("\n-- limits, races and storage --\n");
    Hub hub;
    hub.Open();

    // An enrollment session for the same EUI-64 is running.
    ManagementGrant grant;
    hub.management->IssueCredential(kEuiA, grant);
    hub.management->HandleDatagram(PollFor(kEuiA, grant));
    hub.enrolling = true;
    RemovalJob job;
    API_CHECK(RequestRemovalNow(*hub.management, "dev1", kEuiA, job) == RemovalRequestOutcome::EnrollmentInProgress,
              "a removal while an enrollment session for the same EUI-64 runs: refused");
    hub.enrolling = false;

    // Storage failure: nothing half-done.
    hub.fail_writes = true;
    const std::string before = hub.StateFile();
    API_CHECK(RequestRemovalNow(*hub.management, "dev1", kEuiA, job) == RemovalRequestOutcome::Unavailable &&
                  hub.management->ListRemovals().empty() && hub.StateFile() == before,
              "the job cannot be stored: the request fails and no job exists (never accepted in RAM only)");
    ManagementGrant unused;
    API_CHECK(!hub.management->IssueCredential(kEuiB, unused) && unused.key.empty() && hub.StateFile() == before,
              "a credential that cannot be stored is not issued");
    hub.fail_writes = false;
    API_CHECK(RequestRemovalNow(*hub.management, "dev1", kEuiA, job) == RemovalRequestOutcome::Created, "(storage works again)");
    // 'sent' must be stored before the command leaves.
    std::array<uint8_t, 16> nonce{};
    {
        ingest::ManagementReply idle = hub.management->HandleDatagram(PollFor(kEuiA, grant));
        pb_mgmt_msg_t a;
        pb_mgmt_parse(idle.payload.data(), idle.payload.size(), &a);
        std::memcpy(nonce.data(), a.hub_nonce, 16);
    }
    hub.fail_writes = true;
    ingest::ManagementReply reply = hub.management->HandleDatagram(PollFor(kEuiA, grant, nonce));
    API_CHECK(reply.code == ingest::kCoapCodeServiceUnavailable && reply.payload.empty() &&
                  StateOf(hub, job.removal_id) == RemovalState::Pending,
              "'sent' cannot be stored: the command is NOT sent (5.03) and the job stays pending");
    hub.fail_writes = false;

    // Bounded number of open jobs.
    Hub many;
    many.Open();
    int created = 0;
    RemovalRequestOutcome last = RemovalRequestOutcome::Created;
    for (int i = 0; i < static_cast<int>(kMaxActiveRemovals) + 1; i++) {
        char eui[17];
        std::snprintf(eui, sizeof(eui), "6055f9fffe10%04x", i + 1);
        ManagementGrant g;
        many.management->IssueCredential(eui, g);
        many.management->HandleDatagram(PollFor(eui, g));
        last = RequestRemovalNow(*many.management, "dev1", eui, job);
        created += last == RemovalRequestOutcome::Created;
    }
    API_CHECK(created == static_cast<int>(kMaxActiveRemovals) && last == RemovalRequestOutcome::TooMany &&
                  many.management->ListRemovals().size() == kMaxActiveRemovals,
              "at most 32 removals wait at once; the 33rd is refused, not queued");

    // A damaged state file: the feature is off, the file is left alone.
    Hub damaged;
    const std::string path = damaged.dir + "/" + kPowerBaseMgmtFileName;
    const std::string garbage = "{\"schema_version\":1,\"credentials\":[{\"eui64\":\"zz\"}],\"removals\":[],\"blocked\":[]}";
    test_support::WriteFile(path, garbage);
    std::string text;
    API_CHECK(!damaged.Open() && !damaged.management->available() && test_support::ReadFile(path, text) && text == garbage,
              "a damaged state file: management is unavailable and the file is not replaced");
    ManagementGrant none;
    API_CHECK(!damaged.management->IssueCredential(kEuiA, none) &&
                  RequestRemovalNow(*damaged.management, "dev1", kEuiA, job) == RemovalRequestOutcome::Unavailable &&
                  damaged.management->HandleDatagram(PollFor(kEuiA, grant)).code == ingest::kCoapCodeServiceUnavailable &&
                  test_support::ReadFile(path, text) && text == garbage,
              "nothing is issued, accepted or answered while it is unavailable, and the file stays as it was");
    for (const char *bad : {"", "[]", "{\"schema_version\":2,\"credentials\":[],\"removals\":[],\"blocked\":[]}",
                            "{\"schema_version\":3,\"credentials\":[],\"removals\":[],\"blocked\":[],\"cleanups\":[]}",
                            "{\"schema_version\":1,\"credentials\":[],\"removals\":[]}"}) {
        Hub h;
        test_support::WriteFile(h.dir + "/" + kPowerBaseMgmtFileName, bad);
        API_CHECK(!h.Open(), "(another damaged form is refused as well)");
    }

    // A state file of the earlier schema, in which "completed" meant only
    // that the board had said "released": it is read as what it was.
    {
        Hub old;
        const std::string generation(32, '7'), key(64, '8'), command(32, '9'), removal(32, 'a');
        const std::string v1 = std::string("{\"schema_version\":1,\"credentials\":[{\"eui64\":\"") + kEuiA + "\",\"generation\":\"" +
                               generation + "\",\"key\":\"" + key +
                               "\",\"state\":\"released\",\"issued_at_unix_ms\":1,\"activated_at_unix_ms\":2}],"
                               "\"removals\":[{\"removal_id\":\"" +
                               removal + "\",\"eui64\":\"" + kEuiA + "\",\"generation\":\"" + generation + "\",\"command_id\":\"" +
                               command +
                               "\",\"state\":\"completed\",\"created_at_unix_ms\":3,\"updated_at_unix_ms\":" +
                               std::to_string(g_now_ms) + ",\"completed_at_unix_ms\":" + std::to_string(g_now_ms) +
                               ",\"progress_at_unix_ms\":4,\"requested_by_device_id\":\"dev\",\"affected_module_count\":1}],"
                               "\"blocked\":[{\"eui64\":\"" +
                               kEuiA + "\",\"since_unix_ms\":5}]}\n";
        test_support::WriteFile(old.dir + "/" + kPowerBaseMgmtFileName, v1);
        RemovalJob migrated;
        API_CHECK(old.Open() && old.management->GetRemoval(removal, migrated) && migrated.state == RemovalState::Released &&
                      migrated.completed_at_unix_ms == 0 && old.registry.is_power_base_blocked(Uuid(kEuiA)),
                  "an earlier-schema 'completed' job is read as 'released' (its erase was never confirmed); the block stays");
        ManagementGrant g;
        g.hub_id = kHubId;
        g.generation = generation;
        g.key = key;
        RemovalJob out;
        API_CHECK(old.management->SubmitReleaseReceipt(DoneFor(kEuiA, g, Bytes<16>(command), PB_MGMT_RESULT_ERASED), out) ==
                          ReceiptOutcome::Completed &&
                      old.StateFile().find("\"schema_version\":2") != std::string::npos,
                  "only the board's receipt completes it; the file is written in the current schema from then on");
    }
}

void NothingSecretLogged(int *failures)
{
    std::printf("\n-- no key in the hub's log output --\n");
    char path[] = "/tmp/pb_mgmt_hub_logXXXXXX";
    int fd = mkstemp(path);
    std::fflush(stdout);
    std::fflush(stderr);
    int saved_out = dup(STDOUT_FILENO), saved_err = dup(STDERR_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);

    std::string key, generation;
    {
        Hub hub;
        hub.Open();
        ManagementGrant grant;
        hub.management->IssueCredential(kEuiA, grant);
        key = grant.key;
        generation = grant.generation;
        Board board;
        board.Enroll(hub, kEuiA, grant);
        board.Poll();
        SendHeartbeat(hub.registry, Uuid(kEuiA));
        RemovalJob job;
        RequestRemovalNow(*hub.management, "dev1", kEuiA, job);
        ManagementGrant wrong = grant;
        wrong.key = std::string(64, '3');
        hub.management->HandleDatagram(PollFor(kEuiA, wrong));
        board.Poll();
        board.Poll();
        RemovalJob done;
        hub.management->ProcessSettingsCleanups();
        hub.management->SubmitReleaseReceipt(board.Receipt(), done);
        hub.fail_writes = true;
        hub.management->IssueCredential(kEuiB, wrong);
    }

    std::fflush(stdout);
    std::fflush(stderr);
    dup2(saved_out, STDOUT_FILENO);
    dup2(saved_err, STDERR_FILENO);
    close(saved_out);
    close(saved_err);
    std::string log;
    lseek(fd, 0, SEEK_SET);
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        log.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    unlink(path);
    bool leaked = false;
    for (size_t i = 0; i + 8 <= key.size(); i += 2) {
        leaked = leaked || log.find(key.substr(i, 8)) != std::string::npos;
    }
    API_CHECK(!log.empty() && log.find("released") != std::string::npos && log.find("completed") != std::string::npos,
              "the scenario logged its progress (released, then completed by the receipt)");
    API_CHECK(!leaked, "no run of four key bytes appears in it");
    API_CHECK(log.find(generation) == std::string::npos, "nor the generation");
}

void SettingsCleanup(int *failures)
{
    std::printf("\n-- shared settings: removed modules lose their room, keep their name --\n");
    const std::string dir = test_support::MakeTempDir();
    SettingsStore store(dir);
    store.Load();
    const std::string room = "11111111-2222-4333-8444-555555555555";
    auto module = [&](const std::string &uuid, bool name, bool in_room) {
        ModuleSetting m;
        m.uuid = uuid;
        m.has_name = name;
        m.name = name ? "Desk " + uuid.substr(14) : "";
        m.has_room = in_room;
        m.room_id = in_room ? room : "";
        return m;
    };
    HomeSettings settings;
    settings.has_home_name = true;
    settings.home_name = "Home";
    RoomSetting r;
    r.id = room;
    r.name_kind = RoomNameKind::Custom;
    r.name = "Study";
    r.symbol = "books.vertical";
    settings.rooms.push_back(r);
    settings.modules = {module("744dbdfffe650001", true, true), module("744dbdfffe650002", false, true),
                        module("744dbdfffe650003", true, true), module("744dbdfffe650004", true, false)};
    API_CHECK(UnassignModuleRooms(store, {Target("744dbdfffe650001")}, 1) == RoomUnassignOutcome::Nothing,
              "no settings yet: nothing to do, and none are created");
    SettingsUpdateResult created = store.Update("", settings, 1000);
    API_CHECK(created.outcome == SettingsUpdateOutcome::Saved, "(settings with four modules, three of them in a room)");
    const std::string revision = created.current.revision;
    const uint64_t counter = store.change_counter();

    API_CHECK(UnassignModuleRooms(store, {Target("744dbdfffe650001"), Target("744dbdfffe650002"), Target("744dbdfffe65ffff")}, 2000) ==
                  RoomUnassignOutcome::Saved,
              "two of the removed modules had a room: a new revision is saved");
    SettingsSnapshot now = store.Get();
    auto find = [&](const std::string &uuid) -> const ModuleSetting * {
        for (const ModuleSetting &m : now.settings.modules) {
            if (m.uuid == uuid) {
                return &m;
            }
        }
        return nullptr;
    };
    API_CHECK(now.revision != revision && store.change_counter() == counter + 1,
              "through the ordinary revision: every device sees one new revision");
    API_CHECK(find("744dbdfffe650001") != nullptr && !find("744dbdfffe650001")->has_room &&
                  find("744dbdfffe650001")->has_name && find("744dbdfffe650001")->name == "Desk 01",
              "a named module keeps its name and is in no room");
    API_CHECK(find("744dbdfffe650002") == nullptr, "an entry that had only the room is dropped");
    API_CHECK(find("744dbdfffe650003") != nullptr && find("744dbdfffe650003")->has_room &&
                  find("744dbdfffe650003")->room_id == room && find("744dbdfffe650004") != nullptr,
              "every other module is exactly as it was");
    API_CHECK(now.settings.rooms.size() == 1 && now.settings.rooms[0].name == "Study" && now.settings.home_name == "Home",
              "rooms and the home name are untouched");
    API_CHECK(UnassignModuleRooms(store, {Target("744dbdfffe650001")}, 3000) == RoomUnassignOutcome::Nothing &&
                  store.Get().revision == now.revision,
              "doing it again changes nothing and saves no revision");
    test_support::RemoveTree(dir);
}

void SnapshotFields(int *failures)
{
    std::printf("\n-- the snapshot: management fields are additive --\n");
    ingest::Registry registry;
    const ingest::Uuid base_a = Uuid(kEuiA), base_b = Uuid(kEuiB);
    SendHeartbeat(registry, base_a);
    SendHeartbeat(registry, base_b);
    SendTopology(registry, base_a, Module(1), 1);
    ingest::RegistryView view = registry.view();
    const std::string plain = BuildSnapshotJson(view, 7, 1000);
    API_CHECK(BuildSnapshotJson(view, 7, 1000, nullptr) == plain,
              "without management the snapshot is byte for byte what it was");
    SnapshotManagement extra;
    extra.managed_power_bases = {kEuiA};
    const std::string before_announcement = BuildSnapshotJson(view, 7, 1000, &extra);
    extra.self_confirming_power_bases = {kEuiA};
    API_CHECK(before_announcement.find(std::string("{\"uuid\":\"") + kEuiA +
                                       "\",\"online\":true,\"module_count\":1,\"online_module_count\":1,"
                                       "\"management\":\"supported\",\"erase_confirmation\":\"manual\"}") !=
                  std::string::npos,
              "a managed Base whose board announced nothing: a removal's erase is confirmed by hand");
    RemovalJob job;
    job.removal_id = std::string(32, 'a');
    job.power_base_uuid = kEuiA;
    job.generation = std::string(32, 'b');
    job.state = RemovalState::Pending;
    job.cancellable = true;
    job.requested_by_device_id = "dev1";
    job.affected_module_count = 1;
    job.erase_confirmation = EraseConfirmation::Automatic;
    extra.removals_json = "[" + RemovalJobJson(job) + "]";
    const std::string with = BuildSnapshotJson(view, 7, 1000, &extra);
    API_CHECK(with.find(std::string("{\"uuid\":\"") + kEuiA +
                        "\",\"online\":true,\"module_count\":1,\"online_module_count\":1,\"management\":\"supported\","
                        "\"erase_confirmation\":\"automatic\"}") != std::string::npos,
              "a Base with an active credential: \"management\":\"supported\", and how a removal's erase would be "
              "confirmed (its board announced that it sends its receipt itself)");
    API_CHECK(with.find(std::string("{\"uuid\":\"") + kEuiB +
                        "\",\"online\":true,\"module_count\":0,\"online_module_count\":0,\"management\":\"unsupported\"}") !=
                  std::string::npos,
              "a Base without one: \"management\":\"unsupported\"");
    API_CHECK(with.find("\"power_base_removals\":[{\"removal_id\":\"" + job.removal_id +
                        "\",\"power_base_uuid\":\"" + kEuiA + "\"") != std::string::npos &&
                  with.find("\"state\":\"pending\",\"result_unknown\":false,\"cancellable\":true") != std::string::npos &&
                  with.find("\"completed_at_unix_ms\":null") != std::string::npos &&
                  with.find("\"settings_cleanup_pending\":false,\"erase_confirmation\":\"automatic\"}]") != std::string::npos,
              "the removal jobs follow the modules, in the documented shape (a job that is not final says how its "
              "erase is expected to be confirmed, after every earlier field)");
    job.state = RemovalState::Completed;
    job.erase_confirmation = EraseConfirmation::None;
    API_CHECK(RemovalJobJson(job).find("erase_confirmation") == std::string::npos &&
                  RemovalJobJson(job).find("\"settings_cleanup_pending\":false}") != std::string::npos,
              "a final job has no such field: its JSON ends as it always did");
    // Removing only the added fields gives back the plain snapshot.
    std::string stripped = with;
    for (const char *field : {",\"erase_confirmation\":\"automatic\"", ",\"erase_confirmation\":\"manual\"",
                              ",\"management\":\"supported\"", ",\"management\":\"unsupported\""}) {
        size_t at;
        while ((at = stripped.find(field)) != std::string::npos) {
            stripped.erase(at, std::strlen(field));
        }
    }
    size_t removals = stripped.find(",\"power_base_removals\":");
    if (removals != std::string::npos) {
        stripped.erase(removals, stripped.size() - 1 - removals);
    }
    API_CHECK(stripped == plain, "every earlier field keeps its name, value and position");
}

// A signed HELLO made directly from a grant.
std::vector<uint8_t> HelloFor(const std::string &eui64, const ManagementGrant &grant, uint32_t capabilities)
{
    pb_mgmt_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.type = PB_MGMT_TYPE_HELLO;
    auto eui = Bytes<8>(eui64);
    auto hub_id = Bytes<16>(grant.hub_id);
    auto generation = Bytes<16>(grant.generation);
    auto key = Bytes<32>(grant.key);
    std::memcpy(m.id.eui64, eui.data(), 8);
    std::memcpy(m.id.hub_id, hub_id.data(), 16);
    std::memcpy(m.id.generation, generation.data(), 16);
    m.capabilities = capabilities;
    std::vector<uint8_t> wire(PB_MGMT_MAX_MESSAGE_LEN);
    wire.resize(pb_mgmt_pack(&m, key.data(), Hmac, nullptr, wire.data(), wire.size()));
    return wire;
}

bool JsonSays(const RemovalJob &job, const char *field)
{
    return RemovalJobJson(job).find(field) != std::string::npos;
}

void ThreadReceipt(int *failures)
{
    std::printf("\n-- the board sends its receipt itself: firmware client with the delivery session <-> hub --\n");
    const ingest::Uuid base_a = Uuid(kEuiA);
    {
        Hub hub;
        hub.Open();
        ManagementGrant grant;
        hub.management->IssueCredential(kEuiA, grant);
        Board board;
        board.session = true;
        API_CHECK(board.Enroll(hub, kEuiA, grant), "(a board whose firmware has the delivery session is enrolled)");
        API_CHECK(hub.management->HandleDatagram(HelloFor(kEuiA, grant, PB_MGMT_BOARD_CAP_THREAD_RECEIPT)).code ==
                          ingest::kCoapCodeUnauthorized &&
                      !hub.management->IsManaged(kEuiA),
                  "a HELLO before the first poll is refused: a generation becomes active by its poll, nothing else");
        API_CHECK(board.Poll() == PB_MGMT_POLL_IDLE && board.events == "PH" && board.client.hello_done &&
                      board.client.hub_capabilities == PB_MGMT_HUB_CAP_THREAD_RECEIPT,
                  "after its first answered poll the board announces that it sends its receipt itself, and the hub "
                  "answers with what it can do");
        API_CHECK(hub.StateFile().find("\"board_capabilities\":1") != std::string::npos,
                  "the hub stored that with the credential");
        SendHeartbeat(hub.registry, base_a);
        SendTopology(hub.registry, base_a, Module(1), 1);

        RemovalJob job, out;
        API_CHECK(RequestRemovalNow(*hub.management, "dev1", kEuiA, job) == RemovalRequestOutcome::Created &&
                      job.erase_confirmation == EraseConfirmation::Automatic &&
                      JsonSays(job, "\"erase_confirmation\":\"automatic\""),
                  "a removal of this Base reports: the erase will be confirmed automatically");
        const std::string id = job.removal_id;

        RemovalState hub_at_session = RemovalState::Pending;
        EraseConfirmation hint_at_session = EraseConfirmation::None;
        board.on_session = [&]() {
            RemovalJob at;
            hub.management->GetRemoval(id, at);
            hub_at_session = at.state;
            hint_at_session = at.erase_confirmation;
        };
        board.events.clear();
        API_CHECK(board.Poll() == PB_MGMT_POLL_UNENROLLED, "the next poll (it echoes the hub's nonce) carries the command out");
        board.on_session = nullptr;
        API_CHECK(board.events == "PQADEVSvZs",
                  "board: poll, stop services, accepted, released notice, erase, read back, session up, read back "
                  "again, receipt, session down");
        API_CHECK(hub_at_session == RemovalState::Released && hint_at_session == EraseConfirmation::Automatic,
                  "when the session STARTED the hub said 'released', automatic confirmation expected -- not "
                  "'completed'");
        API_CHECK(hub.management->GetRemoval(id, out) && out.state == RemovalState::Completed &&
                      out.completed_at_unix_ms == g_now_ms && out.erase_confirmation == EraseConfirmation::None &&
                      !JsonSays(out, "erase_confirmation"),
                  "hub: completed by the receipt that came in on the management resource; nothing is expected any "
                  "more");
        API_CHECK(board.client.receipt_acked && board.client.receipt_attempts == 1 && board.StoredState() == 36,
                  "board: the hub's acknowledgement of the receipt verified; final record");
        API_CHECK(hub.StateFile().find(grant.key) == std::string::npos && !hub.management->IsManaged(kEuiA) &&
                      hub.registry.is_power_base_blocked(base_a) && !HasBase(hub.registry.view(), base_a),
                  "the key is dropped; the Base is gone and stays blocked");
        // The receipt on the wire is the receipt the board hands out over BLE.
        const std::vector<uint8_t> receipt = board.Receipt();
        API_CHECK(!receipt.empty() && board.sent.back() == receipt,
                  "what the board sent is byte for byte the receipt a client reads from it over BLE");
        API_CHECK(hub.management->SubmitReleaseReceipt(receipt, out) == ReceiptOutcome::AlreadyCompleted,
                  "relayed by an app as well: the same receipt, nothing changes");

        // The same receipt on the management resource again.
        const std::vector<uint8_t> first_ack = board.answers.back();
        const std::string state_before = hub.StateFile();
        ingest::ManagementReply again = hub.management->HandleDatagram(receipt);
        API_CHECK(again.code == ingest::kCoapCodeContent && again.payload == first_ack && hub.StateFile() == state_before,
                  "the same receipt again gets the same acknowledgement, byte for byte, although the key is gone; "
                  "nothing is written");
        std::vector<uint8_t> forged = receipt;
        forged.back() ^= 0x01;
        API_CHECK(hub.management->HandleDatagram(forged).code == ingest::kCoapCodeUnauthorized,
                  "a receipt with another MAC is refused like any message without a key");
        forged = receipt;
        forged[PB_MGMT_HEADER_LEN] ^= 0x01; // another command id, the original MAC
        API_CHECK(hub.management->HandleDatagram(forged).code == ingest::kCoapCodeUnauthorized,
                  "and so is the MAC under another command id");
        std::array<uint8_t, 16> command{};
        std::memcpy(command.data(), &receipt[PB_MGMT_HEADER_LEN], 16);
        API_CHECK(hub.management->HandleDatagram(DoneFor(kEuiA, grant, command, PB_MGMT_RESULT_RELEASED)).code ==
                      ingest::kCoapCodeUnauthorized,
                  "the released notice of that removal is not answered any more");
        API_CHECK(hub.Open() && hub.management->HandleDatagram(receipt).payload == first_ack,
                  "after a hub restart the same receipt still gets the same acknowledgement (it is in the state file)");
        // Final jobs are kept for ten minutes, and for as long as their
        // settings clean-up is owed; the next release or activation after
        // that drops them.
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0, "(its settings clean-up is done)");
        g_now_ms += 11 * 60 * 1000;
        ManagementGrant other;
        hub.management->IssueCredential(kEuiB, other);
        ActivateDirect(*hub.management, kEuiB, other);
        API_CHECK(!hub.management->GetRemoval(id, out) &&
                      hub.management->HandleDatagram(receipt).code == ingest::kCoapCodeUnauthorized,
                  "once the finished job is no longer kept, the receipt is a message without a key like any other");
    }

    std::printf("\n-- the receipt's answer is lost: the hub has it, the board asks again --\n");
    {
        Hub hub;
        hub.Open();
        ManagementGrant grant;
        hub.management->IssueCredential(kEuiA, grant);
        Board board;
        board.session = true;
        board.Enroll(hub, kEuiA, grant);
        board.Poll();
        RemovalJob job, out;
        RequestRemovalNow(*hub.management, "dev1", kEuiA, job);
        struct LoseFirstReceiptAnswer {
            static pb_mgmt_xfer_t Exchange(const pb_enroll_bridge_endpoint_t *ep, const uint8_t *req, size_t req_len,
                                           uint8_t *resp, size_t resp_cap, size_t *resp_len, void *ctx)
            {
                static int receipts = 0;
                const bool receipt = req[1] == PB_MGMT_TYPE_UNENROLL_DONE &&
                                     req[PB_MGMT_HEADER_LEN + 16] == PB_MGMT_RESULT_ERASED;
                pb_mgmt_xfer_t x = Board::Exchange(ep, req, req_len, resp, resp_cap, resp_len, ctx);
                if (receipt && receipts++ == 0) {
                    *resp_len = 0;
                    return PB_MGMT_XFER_NO_ANSWER; // the hub took it; the answer never arrived
                }
                return x;
            }
        };
        board.client.ops.exchange = LoseFirstReceiptAnswer::Exchange;
        API_CHECK(board.Poll() == PB_MGMT_POLL_UNENROLLED && board.client.receipt_attempts == 2 &&
                      board.client.receipt_acked,
                  "the board sends the receipt a second time and that one is acknowledged");
        API_CHECK(hub.management->GetRemoval(job.removal_id, out) && out.state == RemovalState::Completed,
                  "the hub completed on the first and stayed completed on the second");
    }

    std::printf("\n-- a board without the session, and one whose receipt does not arrive --\n");
    {
        Hub hub;
        hub.Open();
        ManagementGrant grant;
        hub.management->IssueCredential(kEuiA, grant);
        Board board; // session = false: firmware that hands the receipt out over BLE only
        board.Enroll(hub, kEuiA, grant);
        API_CHECK(board.Poll() == PB_MGMT_POLL_IDLE && board.events == "P" &&
                      hub.StateFile().find("\"board_capabilities\":0") != std::string::npos,
                  "a board without the session announces nothing");
        RemovalJob job, out;
        API_CHECK(RequestRemovalNow(*hub.management, "dev1", kEuiA, job) == RemovalRequestOutcome::Created &&
                      job.erase_confirmation == EraseConfirmation::Manual &&
                      JsonSays(job, "\"erase_confirmation\":\"manual\""),
                  "its removal reports from the start: the erase has to be confirmed by hand");
        API_CHECK(board.Poll() == PB_MGMT_POLL_UNENROLLED && board.events.find('Z') == std::string::npos &&
                      hub.management->GetRemoval(job.removal_id, out) && out.state == RemovalState::Released &&
                      out.erase_confirmation == EraseConfirmation::Manual,
                  "released, nothing on the network after it, and the hub says so: manual");
        g_now_ms += 60 * 60 * 1000;
        API_CHECK(hub.management->GetRemoval(job.removal_id, out) && out.state == RemovalState::Released,
                  "time completes nothing");
        API_CHECK(hub.management->SubmitReleaseReceipt(board.Receipt(), out) == ReceiptOutcome::Completed,
                  "the receipt relayed by an app completes it, as before");
    }
    {
        Hub hub;
        hub.Open();
        ManagementGrant grant;
        hub.management->IssueCredential(kEuiA, grant);
        Board board;
        board.session = true;
        board.session_attaches = false; // the last session finds no parent
        board.Enroll(hub, kEuiA, grant);
        board.Poll();
        RemovalJob job, out;
        RequestRemovalNow(*hub.management, "dev1", kEuiA, job);
        const int64_t released_at = g_now_ms;
        API_CHECK(board.Poll() == PB_MGMT_POLL_UNENROLLED && board.events.find('Z') == std::string::npos &&
                      board.events.find("Ss") != std::string::npos && !board.client.receipt_acked,
                  "the board is done; its session never attached and nothing was sent");
        API_CHECK(hub.management->GetRemoval(job.removal_id, out) && out.state == RemovalState::Released &&
                      out.erase_confirmation == EraseConfirmation::Automatic,
                  "hub: released, and for now the receipt is still expected to come by itself");
        g_now_ms = released_at + 4 * 60 * 1000;
        API_CHECK(hub.management->GetRemoval(job.removal_id, out) &&
                      out.erase_confirmation == EraseConfirmation::Automatic,
                  "still at the end of the time a board needs for it");
        g_now_ms = released_at + 4 * 60 * 1000 + 1;
        API_CHECK(hub.management->GetRemoval(job.removal_id, out) && out.state == RemovalState::Released &&
                      out.erase_confirmation == EraseConfirmation::Manual && JsonSays(out, "\"erase_confirmation\":\"manual\""),
                  "after it: still released (time completes nothing), and now reported as needing the manual "
                  "confirmation");
        API_CHECK(hub.Open() && hub.management->GetRemoval(job.removal_id, out) &&
                      out.erase_confirmation == EraseConfirmation::Manual,
                  "the same after a hub restart");
        // Late, but genuine: on the management resource, exactly as the board
        // would have sent it.
        ingest::ManagementReply late = hub.management->HandleDatagram(board.Receipt());
        API_CHECK(late.code == ingest::kCoapCodeContent && hub.management->GetRemoval(job.removal_id, out) &&
                      out.state == RemovalState::Completed,
                  "a receipt that arrives after that still completes the removal");
    }

    // The two faults found in review (2026-10-08), with the real hub on the
    // other end: a delivery session that stored the network again.
    std::printf("\n-- a session that stored the network again: the hub completes nothing on it --\n");
    {
        Hub hub;
        hub.Open();
        ManagementGrant grant;
        hub.management->IssueCredential(kEuiA, grant);
        Board board;
        board.session = true;
        board.Enroll(hub, kEuiA, grant);
        board.Poll();
        RemovalJob job, out;
        RequestRemovalNow(*hub.management, "dev1", kEuiA, job);
        const std::string id = job.removal_id;
        const int64_t released_at = g_now_ms;
        board.session_stores_again = true;
        board.on_session_down = [&]() { board.erase_fails = true; };
        board.events.clear();
        API_CHECK(board.Poll() == PB_MGMT_POLL_RELEASED_NOT_ERASED,
                  "board: the store was written in the session and cannot be erased again -- not an unenroll");
        board.on_session_down = nullptr;
        API_CHECK(board.events == "PQADEVSvsE" && board.client.receipt_attempts == 0,
                  "it read the store back once the session was up, and sent nothing");
        API_CHECK(hub.management->GetRemoval(id, out) && out.state == RemovalState::Released &&
                      out.completed_at_unix_ms == 0,
                  "[fault review 1] hub: released -- NOT completed -- while the board's erase has not checked out");
        API_CHECK(board.StoredState() == 31 && board.dataset && board.Receipt().empty(),
                  "board: 'erase pending' again, its store holds the network, and it hands out no receipt");
        API_CHECK(hub.StateFile().find(grant.key) != std::string::npos,
                  "hub: the key is kept -- it is still owed a receipt it can verify");
        g_now_ms = released_at + 4 * 60 * 1000 + 1;
        API_CHECK(hub.management->GetRemoval(id, out) && out.state == RemovalState::Released &&
                      out.erase_confirmation == EraseConfirmation::Manual,
                  "hub: time completes nothing; after the board's time it asks for the manual confirmation");

        // The next boot of the board: the erase works.
        board.erase_fails = false;
        board.session_stores_again = false;
        board.events.clear();
        API_CHECK(board.FinishRelease() == PB_MGMT_RELEASE_FINAL && board.events == "DEVSvZs" && !board.dataset,
                  "board, next boot: notice, erase, read back, final record -- and a session that stores nothing");
        API_CHECK(hub.management->GetRemoval(id, out) && out.state == RemovalState::Completed &&
                      board.client.receipt_acked,
                  "hub: completed now, by the receipt that followed a verified erase");
    }
    {
        Hub hub;
        hub.Open();
        ManagementGrant grant;
        hub.management->IssueCredential(kEuiA, grant);
        Board board;
        board.session = true;
        board.Enroll(hub, kEuiA, grant);
        board.Poll();
        RemovalJob job, out;
        RequestRemovalNow(*hub.management, "dev1", kEuiA, job);
        const std::string id = job.removal_id;
        board.session_stores_again = true;
        board.events.clear();
        API_CHECK(board.Poll() == PB_MGMT_POLL_UNENROLLED && board.events == "PQADEVSvsEV" && !board.dataset,
                  "the same session with an erase that works: nothing sent, the store erased again and read back");
        API_CHECK(hub.management->GetRemoval(id, out) && out.state == RemovalState::Released,
                  "hub: released; no receipt came in over Thread");
        const std::vector<uint8_t> receipt = board.Receipt();
        API_CHECK(!receipt.empty() && hub.management->SubmitReleaseReceipt(receipt, out) == ReceiptOutcome::Completed,
                  "the manual confirmation is what completes it: the board hands out its receipt, an app relays it");
    }
    {
        // The second: the power goes in that session, before the store is
        // dealt with. The final record is already stored.
        Hub hub;
        hub.Open();
        ManagementGrant grant;
        hub.management->IssueCredential(kEuiA, grant);
        Board board;
        board.session = true;
        board.Enroll(hub, kEuiA, grant);
        board.Poll();
        RemovalJob job, out;
        RequestRemovalNow(*hub.management, "dev1", kEuiA, job);
        const std::string id = job.removal_id;
        board.session_stores_again = true;
        board.erase_lies = false;
        // "Power lost" = the board's flash as it is right after the attach:
        // the same record and store are reached by an erase that cannot be
        // made and a record that cannot be written back.
        board.on_session_down = [&]() {
            board.erase_fails = true;
            nvs_sim_inject(NVS_SIM_OP_SET_BLOB, nullptr, 1, ESP_FAIL);
        };
        API_CHECK(board.Poll() == PB_MGMT_POLL_RELEASED_NOT_ERASED && board.StoredState() == 36 && board.dataset,
                  "(a board left with a final record on a stored network; the hub was sent nothing)");
        board.on_session_down = nullptr;
        API_CHECK(board.Receipt().empty(),
                  "[fault review 2] board: a final record on a stored network hands out no receipt");
        API_CHECK(hub.management->GetRemoval(id, out) && out.state == RemovalState::Released,
                  "hub: released, not completed -- there is nothing an app could relay");
        API_CHECK(board.CheckFinalRelease() == PB_MGMT_FINAL_NOT_ERASED && board.Receipt().empty(),
                  "a boot whose erase fails: still none");
        board.erase_fails = false;
        API_CHECK(board.CheckFinalRelease() == PB_MGMT_FINAL_ERASED_AGAIN && !board.dataset,
                  "a boot whose erase works: the stored network is erased and the store read back");
        const std::vector<uint8_t> receipt = board.Receipt();
        API_CHECK(!receipt.empty() && hub.management->SubmitReleaseReceipt(receipt, out) == ReceiptOutcome::Completed &&
                      out.state == RemovalState::Completed,
                  "only then does the board hand out its receipt, and the hub complete on it");
        API_CHECK(board.CheckFinalRelease() == PB_MGMT_FINAL_VERIFIED, "later boots: verified, nothing to erase");
    }

    std::printf("\n-- what a board announces, and who believes it --\n");
    {
        Hub hub;
        hub.Open();
        ManagementGrant grant_a, grant_b;
        hub.management->IssueCredential(kEuiA, grant_a);
        hub.management->IssueCredential(kEuiB, grant_b);
        ActivateDirect(*hub.management, kEuiA, grant_a);
        ActivateDirect(*hub.management, kEuiB, grant_b);

        ingest::ManagementReply reply = hub.management->HandleDatagram(HelloFor(kEuiA, grant_a, 0x80000001u));
        pb_mgmt_msg_t ack;
        API_CHECK(reply.code == ingest::kCoapCodeContent && pb_mgmt_parse(reply.payload.data(), reply.payload.size(), &ack) &&
                      ack.type == PB_MGMT_TYPE_HELLO_ACK && ack.capabilities == kHubManagementCapabilities &&
                      pb_mgmt_verify(reply.payload.data(), reply.payload.size(), Bytes<32>(grant_a.key).data(), Hmac, nullptr),
                  "HELLO with a capability bit this hub does not know: answered, signed with that enrollment's key");
        RemovalJob job_a, job_b, out;
        RequestRemovalNow(*hub.management, "dev1", kEuiA, job_a);
        RequestRemovalNow(*hub.management, "dev1", kEuiB, job_b);
        API_CHECK(job_a.erase_confirmation == EraseConfirmation::Automatic &&
                      job_b.erase_confirmation == EraseConfirmation::Manual,
                  "the bit it knows counts for that board, and for that board only");

        std::vector<uint8_t> wrong_key = HelloFor(kEuiB, grant_a, PB_MGMT_BOARD_CAP_THREAD_RECEIPT);
        API_CHECK(hub.management->HandleDatagram(wrong_key).code == ingest::kCoapCodeUnauthorized &&
                      hub.management->GetRemoval(job_b.removal_id, out) &&
                      out.erase_confirmation == EraseConfirmation::Manual,
                  "a HELLO for one board signed with another board's key changes nothing");
        std::vector<uint8_t> tampered = HelloFor(kEuiB, grant_b, 0);
        tampered[PB_MGMT_HEADER_LEN + 3] = 1; // capabilities 0 -> 1, MAC untouched
        API_CHECK(hub.management->HandleDatagram(tampered).code == ingest::kCoapCodeUnauthorized &&
                      hub.management->GetRemoval(job_b.removal_id, out) &&
                      out.erase_confirmation == EraseConfirmation::Manual,
                  "nor does a capability bit set on the way");

        const std::string before = hub.StateFile();
        API_CHECK(hub.management->HandleDatagram(HelloFor(kEuiA, grant_a, 0x80000001u)).code == ingest::kCoapCodeContent &&
                      hub.StateFile() == before,
                  "the same HELLO again: answered, nothing written");
        hub.fail_next_writes = 1;
        API_CHECK(hub.management->HandleDatagram(HelloFor(kEuiB, grant_b, PB_MGMT_BOARD_CAP_THREAD_RECEIPT)).code ==
                          ingest::kCoapCodeServiceUnavailable &&
                      hub.management->GetRemoval(job_b.removal_id, out) &&
                      out.erase_confirmation == EraseConfirmation::Manual,
                  "a HELLO that cannot be stored is not acknowledged and not believed (the board asks again)");
        API_CHECK(hub.management->HandleDatagram(HelloFor(kEuiB, grant_b, PB_MGMT_BOARD_CAP_THREAD_RECEIPT)).code ==
                          ingest::kCoapCodeContent &&
                      hub.management->GetRemoval(job_b.removal_id, out) &&
                      out.erase_confirmation == EraseConfirmation::Automatic,
                  "stored: the pending removal of that board now expects an automatic confirmation");
        API_CHECK(hub.Open() && hub.management->GetRemoval(job_a.removal_id, out) &&
                      out.erase_confirmation == EraseConfirmation::Automatic &&
                      hub.management->GetRemoval(job_b.removal_id, out) &&
                      out.erase_confirmation == EraseConfirmation::Automatic,
                  "what was announced survives a hub restart");
        API_CHECK(hub.management->HandleDatagram(HelloFor(kEuiA, grant_a, 0)).code == ingest::kCoapCodeContent &&
                      hub.management->GetRemoval(job_a.removal_id, out) &&
                      out.erase_confirmation == EraseConfirmation::Manual,
                  "a board that later announces that it does NOT send its receipt (other firmware) is believed too");

        // result_unknown outranks the announcement.
        hub.management->HandleDatagram(HelloFor(kEuiA, grant_a, PB_MGMT_BOARD_CAP_THREAD_RECEIPT));
        std::array<uint8_t, 16> command{};
        FetchCommandDirect(*hub.management, kEuiA, grant_a, command);
        g_now_ms += 5 * 60 * 1000 + 1;
        API_CHECK(hub.management->GetRemoval(job_a.removal_id, out) && out.state == RemovalState::Sent &&
                      out.result_unknown && out.erase_confirmation == EraseConfirmation::Manual,
                  "a command without any sign of life for five minutes: result unknown, and the manual "
                  "confirmation is what is offered");

        // A released board announces nothing more.
        std::array<uint8_t, 16> command_b{};
        ReleaseDirect(*hub.management, kEuiB, grant_b, command_b);
        API_CHECK(hub.management->HandleDatagram(HelloFor(kEuiB, grant_b, 0)).code == ingest::kCoapCodeUnauthorized &&
                      hub.management->GetRemoval(job_b.removal_id, out) && out.state == RemovalState::Released &&
                      out.erase_confirmation == EraseConfirmation::Automatic,
                  "a HELLO after the release (a replay) is refused and changes nothing");
    }

    std::printf("\n-- a state file from before these fields --\n");
    {
        Hub hub;
        hub.Open();
        ManagementGrant grant;
        hub.management->IssueCredential(kEuiA, grant);
        Board board;
        board.session = true;
        board.Enroll(hub, kEuiA, grant);
        board.Poll();
        RemovalJob job, out;
        RequestRemovalNow(*hub.management, "dev1", kEuiA, job);
        board.Poll();
        API_CHECK(hub.management->GetRemoval(job.removal_id, out) && out.state == RemovalState::Completed,
                  "(a removal completed by a receipt over Thread)");
        // What a hub from before wrote: the same file without the two fields.
        std::string text = hub.StateFile();
        const std::string ack_field = ",\"receipt_ack_mac\":\"";
        size_t at = text.find(ack_field);
        API_CHECK(at != std::string::npos, "(the acknowledgement's MAC is in the state file)");
        text.erase(at, ack_field.size() + 64 + 1);
        ManagementGrant other;
        hub.management->IssueCredential(kEuiB, other);
        std::string with_credential = hub.StateFile();
        const std::string with_announcement = with_credential;
        const std::string caps_field = ",\"board_capabilities\":0";
        API_CHECK(with_credential.find(caps_field) != std::string::npos, "(and a credential entry says what its board announced)");
        std::string error;
        API_CHECK(WriteFileAtomically(hub.dir + "/" + kPowerBaseMgmtFileName, text, error) && hub.Open() &&
                      hub.management->GetRemoval(job.removal_id, out) && out.state == RemovalState::Completed,
                  "a file without the acknowledgement's MAC is read as it always was");
        API_CHECK(hub.management->HandleDatagram(board.Receipt()).code == ingest::kCoapCodeUnauthorized &&
                      hub.management->SubmitReleaseReceipt(board.Receipt(), out) == ReceiptOutcome::AlreadyCompleted,
                  "its receipt is not acknowledged again on the management resource (there is nothing to answer "
                  "with); relayed by an app it is still recognised");
        while ((at = with_credential.find(caps_field)) != std::string::npos) {
            with_credential.erase(at, caps_field.size());
        }
        API_CHECK(WriteFileAtomically(hub.dir + "/" + kPowerBaseMgmtFileName, with_credential, error) && hub.Open() &&
                      hub.management->available(),
                  "a file whose credentials carry no announcement is read: such a board announced nothing");
        std::string bad = with_announcement;
        at = bad.find("\"board_capabilities\":0");
        bad.replace(at, std::strlen("\"board_capabilities\":0"), "\"board_capabilities\":-1");
        Hub damaged;
        API_CHECK(WriteFileAtomically(damaged.dir + "/" + kPowerBaseMgmtFileName, bad, error) && !damaged.Open(),
                  "a file with a value HELLO cannot carry is damaged, like any other damaged file");
    }

    std::printf("\n-- an earlier removal's receipt after the board was enrolled again --\n");
    {
        Hub hub;
        hub.Open();
        ManagementGrant first, second;
        hub.management->IssueCredential(kEuiA, first);
        Board board;
        board.session = true;
        board.Enroll(hub, kEuiA, first);
        board.Poll();
        RemovalJob job, out;
        RequestRemovalNow(*hub.management, "dev1", kEuiA, job);
        board.Poll();
        const std::vector<uint8_t> old_receipt = board.Receipt();
        const std::vector<uint8_t> old_ack = board.answers.back();
        API_CHECK(hub.management->GetRemoval(job.removal_id, out) && out.state == RemovalState::Completed,
                  "(removed automatically)");
        // The same board, enrolled again: a new generation and key.
        hub.management->IssueCredential(kEuiA, second);
        Board again;
        again.session = true;
        again.Enroll(hub, kEuiA, second);
        API_CHECK(again.Poll() == PB_MGMT_POLL_IDLE && hub.management->IsManaged(kEuiA) &&
                      !hub.registry.is_power_base_blocked(base_a),
                  "(and enrolled again: a new generation is active, the block is lifted)");
        SendHeartbeat(hub.registry, base_a);
        const std::string before = hub.StateFile();
        ingest::ManagementReply replay = hub.management->HandleDatagram(old_receipt);
        API_CHECK(replay.code == ingest::kCoapCodeContent && replay.payload == old_ack && hub.StateFile() == before,
                  "the old removal's receipt arrives again: the old acknowledgement, and not one byte of state "
                  "changes");
        API_CHECK(hub.management->IsManaged(kEuiA) && !hub.registry.is_power_base_blocked(base_a) &&
                      HasBase(hub.registry.view(), base_a) && hub.management->ListRemovals().size() == 1,
                  "the new enrollment is untouched: still managed, not blocked, in the Registry, no new removal");
        // A removal of the NEW enrollment cannot be completed by the old receipt.
        RemovalJob job2;
        RequestRemovalNow(*hub.management, "dev1", kEuiA, job2);
        API_CHECK(job2.generation == second.generation &&
                      hub.management->SubmitReleaseReceipt(old_receipt, out) == ReceiptOutcome::AlreadyCompleted &&
                      out.removal_id == job.removal_id && StateOf(hub, job2.removal_id) == RemovalState::Pending,
                  "a removal of the new enrollment stays pending whatever is done with the old receipt");
        std::array<uint8_t, 16> old_command{};
        std::memcpy(old_command.data(), &old_receipt[PB_MGMT_HEADER_LEN], 16);
        API_CHECK(hub.management->HandleDatagram(DoneFor(kEuiA, second, old_command, PB_MGMT_RESULT_ERASED)).code ==
                          ingest::kCoapCodeNotFound &&
                      StateOf(hub, job2.removal_id) == RemovalState::Pending && hub.management->IsManaged(kEuiA),
                  "nor by a receipt for the OLD command signed with the new key: no such command in this "
                  "enrollment");
        API_CHECK(again.Poll() == PB_MGMT_POLL_UNENROLLED &&
                      StateOf(hub, job2.removal_id) == RemovalState::Completed && again.Receipt() != old_receipt,
                  "the new enrollment is removed by its own command and its own receipt");
    }
}


// ---- the same hub at another Thread address or port (docs/power-base-management.md, section 12) ----

std::vector<uint8_t> LocateFor(const std::string &eui64, const std::string &hub_id_hex, const std::string &generation_hex,
                               const std::string &key_hex, uint8_t nonce_first)
{
    pb_mgmt_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.type = PB_MGMT_TYPE_LOCATE;
    auto eui = Bytes<8>(eui64);
    auto hub_id = Bytes<16>(hub_id_hex);
    auto generation = Bytes<16>(generation_hex);
    auto key = Bytes<32>(key_hex);
    std::memcpy(m.id.eui64, eui.data(), 8);
    std::memcpy(m.id.hub_id, hub_id.data(), 16);
    std::memcpy(m.id.generation, generation.data(), 16);
    for (int i = 0; i < 16; i++) {
        m.board_nonce[i] = static_cast<uint8_t>(nonce_first + i);
    }
    std::vector<uint8_t> wire(PB_MGMT_MAX_MESSAGE_LEN);
    wire.resize(pb_mgmt_pack(&m, key.data(), Hmac, nullptr, wire.data(), wire.size()));
    return wire;
}

void EndpointLocate(int *failures)
{
    std::printf("\n-- LOCATE: the wire format's golden MACs, computed here with OpenSSL --\n");
    {
        pb_mgmt_msg_t m;
        uint8_t wire[PB_MGMT_MAX_MESSAGE_LEN];
        uint8_t key[32];
        for (int i = 0; i < 32; i++) {
            key[i] = static_cast<uint8_t>(0x30 + i);
        }
        const uint8_t addr[16] = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00, 0x8c, 0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79};
        for (uint8_t type : {static_cast<uint8_t>(PB_MGMT_TYPE_LOCATE), static_cast<uint8_t>(PB_MGMT_TYPE_LOCATE_ACK)}) {
            std::memset(&m, 0, sizeof(m));
            m.type = type;
            auto eui = Bytes<8>(kEuiA);
            std::memcpy(m.id.eui64, eui.data(), 8);
            for (int i = 0; i < 16; i++) {
                m.id.hub_id[i] = static_cast<uint8_t>(0x10 + i);
                m.id.generation[i] = static_cast<uint8_t>(0x20 + i);
                m.board_nonce[i] = static_cast<uint8_t>(0xb0 + i);
            }
            if (type == PB_MGMT_TYPE_LOCATE_ACK) {
                std::memcpy(m.endpoint_addr, addr, 16);
                m.endpoint_port = 5684;
            }
            size_t n = pb_mgmt_pack(&m, key, Hmac, nullptr, wire, sizeof(wire));
            if (type == PB_MGMT_TYPE_LOCATE) {
                API_CHECK(n == 96 && Hex(&wire[n - 32], 32) == "5ceec372200fa66bf2904adb8d47502e1b314bbee8ef110d19789272f4989231",
                          "LOCATE: 96 bytes, the firmware test's golden MAC");
            } else {
                API_CHECK(n == 112 && Hex(&wire[n - 32], 32) == "6fa4e266b7ef48249a301bb5639a81953cb0cd7494dcd29b69d3eaa271faf831",
                          "LOCATE_ACK: 112 bytes, the firmware test's golden MAC");
            }
        }
    }

    // The hub's host in the Thread network: its EID, its routing locator and
    // the port its ingest listener is on. The test moves them.
    std::array<uint8_t, 16> hub_eid = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00, 0x8c, 0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79};
    const std::array<uint8_t, 16> eid_old = hub_eid;
    const std::array<uint8_t, 16> eid_new = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00, 0x12, 0x9a, 0x77, 0x01, 0xbe, 0xef, 0x40, 0x02};
    std::array<uint8_t, 16> hub_rloc = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x30, 0x00};
    const std::array<uint8_t, 16> rloc_new = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x5c, 0x00};
    uint16_t hub_port = 5684;
    bool thread_interface_up = true;

    Hub hub;
    hub.ingest_endpoint = [&](std::array<uint8_t, 16> &address, uint16_t &port) {
        address = hub_eid;
        port = hub_port;
        return thread_interface_up;
    };
    hub.Open();
    ManagementGrant grant;
    hub.management->IssueCredential(kEuiA, grant);

    Board board;
    board.locate = true;
    board.route = [&](const pb_enroll_bridge_endpoint_t &to) {
        const bool at_host = std::memcmp(to.addr, hub_eid.data(), 16) == 0 || std::memcmp(to.addr, hub_rloc.data(), 16) == 0;
        if (!at_host) {
            return 0;
        }
        if (to.port == hub_port) {
            return 1;
        }
        return to.port == PB_MGMT_LOCATE_PORT ? 2 : 0; // the locate-only socket exists when the ingest port is another
    };
    auto endpoint_is = [&](const pb_enroll_bridge_endpoint_t &ep, const std::array<uint8_t, 16> &addr, uint16_t port) {
        return ep.port == port && std::memcmp(ep.addr, addr.data(), 16) == 0;
    };
    auto stored_endpoint = [&]() {
        pb_enroll_record_t rec;
        std::memset(&rec, 0, sizeof(rec));
        pb_enroll_record_load(board.eui.data(), &rec);
        return rec.bridge_endpoint;
    };
    auto poll_until_moved = [&](int limit) {
        for (int i = 1; i <= limit; i++) {
            if (board.Poll() == PB_MGMT_POLL_ENDPOINT_MOVED) {
                return i;
            }
        }
        return 0;
    };

    std::printf("\n-- a hub that says where it is, and a board that can ask --\n");
    API_CHECK(board.Enroll(hub, kEuiA, grant) && board.Poll() == PB_MGMT_POLL_IDLE && hub.management->IsManaged(kEuiA),
              "the board is enrolled at the hub's endpoint and its first poll activates the credential");
    API_CHECK((board.client.hub_capabilities & PB_MGMT_HUB_CAP_ENDPOINT_LOCATE) != 0 &&
                  (board.client.hub_capabilities & PB_MGMT_HUB_CAP_THREAD_RECEIPT) != 0,
              "HELLO_ACK: this hub answers LOCATE (and still takes a receipt over Thread)");
    API_CHECK(hub.StateFile().find("\"board_capabilities\":3") != std::string::npos,
              "the hub noted what the board announced: it sends its receipt itself and it follows its hub");
    for (int i = 0; i < 50; i++) {
        board.Poll();
    }
    API_CHECK(board.events.find('L') == std::string::npos && board.moved == 0,
              "50 answered polls: the board never asks where its hub is");

    std::printf("\n-- the hub's Thread address changes (same hub id, same credential, same network) --\n");
    const std::string file_before = hub.StateFile();
    hub_eid = eid_new;
    hub_rloc = rloc_new;
    board.hints = {rloc_new}; // what the board's own Network Data would name
    int took = poll_until_moved(20);
    API_CHECK(took == static_cast<int>(PB_MGMT_LOCATE_AFTER_SILENT_POLLS),
              "six polls go unanswered at the old address; the round that follows finds the hub");
    API_CHECK(board.moved == 1 && endpoint_is(board.moved_to, eid_new, 5684) && endpoint_is(stored_endpoint(), eid_new, 5684),
              "the board stored the endpoint the REAL hub named and confirmed, and moved its platform there");
    {
        // The last two LOCATEs: to the locator (the hint), then to the EID the hub named.
        std::vector<pb_enroll_bridge_endpoint_t> locates;
        for (size_t i = 0; i < board.sent.size(); i++) {
            if (board.sent[i][1] == PB_MGMT_TYPE_LOCATE) {
                locates.push_back(board.sent_to[i]);
            }
        }
        API_CHECK(locates.size() == 3 && endpoint_is(locates[0], eid_old, 5684) && endpoint_is(locates[1], rloc_new, 5684) &&
                      endpoint_is(locates[2], eid_new, 5684),
                  "asked at the stored address (silent), at the hint, and once more at the endpoint the hub named");
    }
    API_CHECK(hub.StateFile() == file_before, "answering LOCATE changed nothing in the hub's state file");
    API_CHECK(board.Poll() == PB_MGMT_POLL_IDLE && endpoint_is(board.sent_to.back(), eid_new, 5684) &&
                  hub.management->IsManaged(kEuiA),
              "the next poll goes to the new address and is answered: the same credential, still active");

    std::printf("\n-- the hub's ingest port changes (the locate port stays open, LOCATE only) --\n");
    hub_port = 5700;
    board.hints.clear(); // no hint needed: the stored address still answers on the locate port
    size_t polls_before = board.sent.size();
    took = poll_until_moved(20);
    API_CHECK(took == static_cast<int>(PB_MGMT_LOCATE_AFTER_SILENT_POLLS) && endpoint_is(stored_endpoint(), eid_new, 5700) &&
                  board.moved == 2,
              "polls on the old port get no answer (the locate socket answers LOCATE only); the board finds the new port");
    {
        bool poll_answered_on_locate_port = false;
        for (size_t i = polls_before; i < board.sent.size(); i++) {
            (void)i;
        }
        // A POLL put to the locate-only socket: silent.
        std::vector<uint8_t> poll = board.sent[polls_before];
        ingest::ManagementReply silent = hub.management->HandleLocateDatagram(poll);
        poll_answered_on_locate_port = !silent.silent;
        API_CHECK(poll[1] == PB_MGMT_TYPE_POLL && !poll_answered_on_locate_port && silent.payload.empty(),
                  "the hub's locate socket gives a POLL no answer at all");
        ingest::ManagementReply garbage = hub.management->HandleLocateDatagram(std::vector<uint8_t>(96, 0x5a));
        ingest::ManagementReply empty = hub.management->HandleLocateDatagram({});
        API_CHECK(garbage.silent && empty.silent, "nor bytes that are no message, nor an empty request");
    }
    API_CHECK(board.Poll() == PB_MGMT_POLL_IDLE && endpoint_is(board.sent_to.back(), eid_new, 5700),
              "the next poll goes to the new port and is answered");

    std::printf("\n-- a removal requested after the move runs at the new endpoint --\n");
    {
        RemovalJob job, out;
        SendHeartbeat(hub.registry, Uuid(kEuiA));
        API_CHECK(RequestRemovalNow(*hub.management, "dev1", kEuiA, job) == RemovalRequestOutcome::Created,
                  "a removal is requested");
        // The board holds the nonce of the hub's last answer, so the very
        // next poll carries the command.
        const pb_mgmt_poll_result_t removal_poll = board.Poll();
        const int final_release = static_cast<int>(PB_ENROLL_MGMT_RELEASED) * 10 +
                                  (PB_ENROLL_RELEASE_F_ERASED | PB_ENROLL_RELEASE_F_NOTICE_ACKED);
        API_CHECK(removal_poll == PB_MGMT_POLL_UNENROLLED && board.Poll() == PB_MGMT_POLL_NOT_ARMED &&
                      hub.management->GetRemoval(job.removal_id, out) &&
                      out.state == RemovalState::Completed && board.StoredState() == final_release,
                  "the board takes the command at the new endpoint, releases, erases and delivers its receipt there: "
                  "completed");
        size_t locates = std::count(board.events.begin(), board.events.end(), 'L');
        hub_eid = eid_old; // the hub moves once more
        for (int i = 0; i < 40; i++) {
            board.Poll();
        }
        API_CHECK(std::count(board.events.begin(), board.events.end(), 'L') == static_cast<long>(locates) &&
                      board.StoredState() == final_release,
                  "a released board does not look for the hub and its record stays a final release");
        hub_eid = eid_new;
    }

    std::printf("\n-- who gets an answer to LOCATE --\n");
    {
        Hub h2;
        std::array<uint8_t, 16> eid = eid_old;
        bool up = true;
        h2.ingest_endpoint = [&](std::array<uint8_t, 16> &address, uint16_t &port) {
            address = eid;
            port = 5684;
            return up;
        };
        h2.Open();
        ManagementGrant active, provisional, other;
        h2.management->IssueCredential(kEuiA, active);
        ActivateDirect(*h2.management, kEuiA, active);
        h2.management->IssueCredential(kEuiB, provisional);
        const std::string before = h2.StateFile();

        auto answered_with = [&](const ingest::ManagementReply &r, const std::string &key_hex, uint8_t nonce_first) {
            pb_mgmt_msg_t a;
            return r.code == ingest::kCoapCodeContent && r.payload.size() == PB_MGMT_LOCATE_ACK_LEN &&
                   pb_mgmt_parse(r.payload.data(), r.payload.size(), &a) && a.type == PB_MGMT_TYPE_LOCATE_ACK &&
                   pb_mgmt_verify(r.payload.data(), r.payload.size(), Bytes<32>(key_hex).data(), Hmac, nullptr) &&
                   a.board_nonce[0] == nonce_first && a.board_nonce[15] == static_cast<uint8_t>(nonce_first + 15) &&
                   std::memcmp(a.endpoint_addr, eid.data(), 16) == 0 && a.endpoint_port == 5684;
        };
        ingest::ManagementReply r = h2.management->HandleDatagram(LocateFor(kEuiA, active.hub_id, active.generation, active.key, 0x41));
        API_CHECK(answered_with(r, active.key, 0x41),
                  "an active credential: LOCATE_ACK with this request's nonce and the hub's endpoint, under that key");
        ingest::ManagementReply again = h2.management->HandleDatagram(LocateFor(kEuiA, active.hub_id, active.generation, active.key, 0x41));
        API_CHECK(again.payload == r.payload, "the same request again: the same answer (nothing is counted or remembered)");
        r = h2.management->HandleDatagram(LocateFor(kEuiB, provisional.hub_id, provisional.generation, provisional.key, 0x42));
        API_CHECK(answered_with(r, provisional.key, 0x42) && !h2.management->IsManaged(kEuiB),
                  "a provisional credential is answered too, and LOCATE does not activate it");

        r = h2.management->HandleDatagram(LocateFor(kEuiA, active.hub_id, active.generation, provisional.key, 0x43));
        API_CHECK(r.code == ingest::kCoapCodeUnauthorized && r.payload.empty(), "another key: 4.01, no payload");
        r = h2.management->HandleDatagram(LocateFor(kEuiA, active.hub_id, provisional.generation, active.key, 0x44));
        API_CHECK(r.code == ingest::kCoapCodeUnauthorized, "a generation this hub has no credential for: 4.01");
        r = h2.management->HandleDatagram(LocateFor(kEuiA, "909192939495969798999a9b9c9d9e9f", active.generation, active.key, 0x45));
        API_CHECK(r.code == ingest::kCoapCodeUnauthorized, "addressed to another hub id: 4.01");
        std::vector<uint8_t> bad = LocateFor(kEuiA, active.hub_id, active.generation, active.key, 0x46);
        bad[PB_MGMT_HEADER_LEN + 17] = 1;
        r = h2.management->HandleDatagram(bad);
        API_CHECK(r.code == ingest::kCoapCodeBadRequest, "a reserved byte set: 4.00");

        up = false;
        r = h2.management->HandleDatagram(LocateFor(kEuiA, active.hub_id, active.generation, active.key, 0x47));
        API_CHECK(r.code == ingest::kCoapCodeServiceUnavailable && r.payload.empty(),
                  "the hub cannot tell where it is (its Thread interface has no such address): 5.03, nothing is guessed");
        up = true;

        // A superseded generation: the board is enrolled again and the new
        // generation becomes active; the old key is gone.
        h2.management->IssueCredential(kEuiA, other);
        ActivateDirect(*h2.management, kEuiA, other);
        r = h2.management->HandleDatagram(LocateFor(kEuiA, active.hub_id, active.generation, active.key, 0x48));
        API_CHECK(r.code == ingest::kCoapCodeUnauthorized, "the previous enrollment generation of the same board: 4.01");

        // A released credential (its key is kept for the receipt only).
        RemovalJob job;
        std::array<uint8_t, 16> command{};
        SendHeartbeat(h2.registry, Uuid(kEuiA));
        RequestRemovalNow(*h2.management, "dev1", kEuiA, job);
        bool fetched = FetchCommandDirect(*h2.management, kEuiA, other, command);
        pb_mgmt_msg_t done;
        std::memset(&done, 0, sizeof(done));
        done.type = PB_MGMT_TYPE_UNENROLL_DONE;
        auto eui = Bytes<8>(kEuiA);
        std::memcpy(done.id.eui64, eui.data(), 8);
        std::memcpy(done.id.hub_id, Bytes<16>(other.hub_id).data(), 16);
        std::memcpy(done.id.generation, Bytes<16>(other.generation).data(), 16);
        std::memcpy(done.command_id, command.data(), 16);
        done.result = PB_MGMT_RESULT_RELEASED;
        std::vector<uint8_t> wire(PB_MGMT_MAX_MESSAGE_LEN);
        wire.resize(pb_mgmt_pack(&done, Bytes<32>(other.key).data(), Hmac, nullptr, wire.data(), wire.size()));
        ingest::ManagementReply released = h2.management->HandleDatagram(wire);
        r = h2.management->HandleDatagram(LocateFor(kEuiA, other.hub_id, other.generation, other.key, 0x49));
        API_CHECK(fetched && released.code == ingest::kCoapCodeContent && r.code == ingest::kCoapCodeUnauthorized,
                  "a board that said 'released': its key still verifies, and LOCATE is refused all the same (4.01)");
        (void)before;
    }
    {
        Hub plain; // no ingest endpoint was configured
        plain.Open();
        ManagementGrant g;
        plain.management->IssueCredential(kEuiA, g);
        ActivateDirect(*plain.management, kEuiA, g);
        ingest::ManagementReply r = plain.management->HandleDatagram(LocateFor(kEuiA, g.hub_id, g.generation, g.key, 0x51));
        API_CHECK(r.code == ingest::kCoapCodeServiceUnavailable && r.payload.empty(),
                  "a hub that was never told where its listener is: 5.03 (and it does not announce the capability)");
    }
}

} // namespace

// ---- taking an offline Power Base out of this hub's list (docs/power-base-management.md, section 13) ----

std::string RequestId(unsigned n)
{
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%024x%08x", 0x4d0000u, n);
    return buf;
}

// The record this hub lists for `eui64` ("" when it lists none).
std::string RecordOf(Hub &hub, const std::string &eui64)
{
    auto records = hub.management->PowerBaseRecords({eui64});
    return records.empty() ? std::string() : records[0].second;
}

// Ten minutes pass for "is this Base heard from?": nothing sent so far counts.
void NothingHeardAnyMore(Hub &hub)
{
    hub.steady_shift = std::chrono::seconds(600);
}

// A signed UNENROLL_ACCEPTED for `command`.
std::vector<uint8_t> AcceptedFor(const std::string &eui64, const ManagementGrant &grant, const std::array<uint8_t, 16> &command)
{
    pb_mgmt_msg_t m;
    std::memset(&m, 0, sizeof(m));
    m.type = PB_MGMT_TYPE_UNENROLL_ACCEPTED;
    auto eui = Bytes<8>(eui64);
    auto hub_id = Bytes<16>(grant.hub_id);
    auto generation = Bytes<16>(grant.generation);
    auto key = Bytes<32>(grant.key);
    std::memcpy(m.id.eui64, eui.data(), 8);
    std::memcpy(m.id.hub_id, hub_id.data(), 16);
    std::memcpy(m.id.generation, generation.data(), 16);
    std::memcpy(m.command_id, command.data(), 16);
    std::vector<uint8_t> wire(PB_MGMT_MAX_MESSAGE_LEN);
    wire.resize(pb_mgmt_pack(&m, key.data(), Hmac, nullptr, wire.data(), wire.size()));
    return wire;
}

size_t CountOf(const std::string &text, const std::string &what)
{
    size_t count = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) {
        count++;
    }
    return count;
}

void ListRemoval(int *failures)
{
    std::printf("\n-- an offline Power Base is taken out of the hub's list (not a removal: the board is not erased) --\n");
    const ingest::Uuid base_a = Uuid(kEuiA), base_b = Uuid(kEuiB);
    const ingest::Uuid m1 = Module(1), m2 = Module(2), m3 = Module(3);
    const std::string m1_hex = "744dbdfffe650001";
    Hub hub;
    hub.Open();
    ManagementGrant ga, gb;
    hub.management->IssueCredential(kEuiA, ga);
    hub.management->IssueCredential(kEuiB, gb);
    Board board;
    API_CHECK(board.Enroll(hub, kEuiA, ga) && board.Poll() == PB_MGMT_POLL_IDLE && ActivateDirect(*hub.management, kEuiB, gb),
              "(two Power Bases with a management credential: A is the real firmware client)");
    SendHeartbeat(hub.registry, base_a);
    SendHeartbeat(hub.registry, base_b);
    // m1 under A; m2 under A and, since, under B (it moved); m3 under B.
    SendTopology(hub.registry, base_a, m1, 1);
    SendTopology(hub.registry, base_a, m2, 1);
    SendTopology(hub.registry, base_b, m2, 1);
    SendTopology(hub.registry, base_b, m3, 1);

    API_CHECK(RecordOf(hub, kEuiA) == ga.generation && RecordOf(hub, kEuiB) == gb.generation,
              "the record of a Base with a credential is its registration's generation (no secret: the key is not in it)");
    const std::string before = hub.StateFile();
    ForgetRecord done, again;

    // 1. Heard from: refused, whatever the app showed.
    API_CHECK(hub.management->ForgetPowerBase("dev1", kEuiA, ga.generation, RequestId(1), done) == ForgetOutcome::Online &&
                  hub.StateFile() == before && HasBase(hub.registry.view(), base_a) && !hub.management->IsBlocked(kEuiA) &&
                  hub.management->IsManaged(kEuiA),
              "while the hub hears the Base the request is refused (online) and nothing changes -- the hub decides, not "
              "what an app displayed");

    NothingHeardAnyMore(hub);
    // 2. Requests that do not name this registration, or nothing this hub lists.
    API_CHECK(hub.management->ForgetPowerBase("dev1", kEuiA, gb.generation, RequestId(2), done) == ForgetOutcome::StaleRecord &&
                  hub.management->ForgetPowerBase("dev1", kEuiA, std::string(32, '0'), RequestId(2), done) ==
                      ForgetOutcome::StaleRecord &&
                  hub.management->ForgetPowerBase("dev1", kEuiA, "zz", RequestId(2), done) == ForgetOutcome::StaleRecord &&
                  hub.StateFile() == before,
              "offline, but the record is another registration's, all zeros or not a record: refused (stale), nothing changes");
    API_CHECK(hub.management->ForgetPowerBase("dev1", "6055f9fffe00ffff", ga.generation, RequestId(2), done) ==
                      ForgetOutcome::UnknownPowerBase &&
                  hub.management->ForgetPowerBase("dev1", "not-an-eui", ga.generation, RequestId(2), done) ==
                      ForgetOutcome::UnknownPowerBase &&
                  hub.management->ForgetPowerBase("dev1", kEuiA, ga.generation, "short", done) == ForgetOutcome::RequestConflict &&
                  hub.StateFile() == before,
              "a Base this hub does not list, a malformed EUI-64, a malformed request id: refused, nothing changes");
    hub.enrolling = true;
    API_CHECK(hub.management->ForgetPowerBase("dev1", kEuiA, ga.generation, RequestId(2), done) ==
                      ForgetOutcome::EnrollmentInProgress &&
                  hub.StateFile() == before,
              "while a registration of that board is running: refused, nothing changes");
    hub.enrolling = false;

    // 3. The state cannot be stored: no effect anywhere.
    hub.fail_next_writes = 1;
    API_CHECK(hub.management->ForgetPowerBase("dev1", kEuiA, ga.generation, RequestId(3), done) == ForgetOutcome::Unavailable &&
                  done.forget_id.empty(),
              "the state file cannot be written: the request fails (unavailable)");
    {
        ingest::RegistryView view = hub.registry.view();
        API_CHECK(hub.StateFile() == before && HasBase(view, base_a) && HasNode(view, base_a, m1) &&
                      !hub.registry.is_power_base_blocked(base_a) && !hub.management->IsBlocked(kEuiA) &&
                      hub.management->IsManaged(kEuiA) && hub.removed_modules.empty() &&
                      hub.management->PendingSettingsCleanups() == 0 && hub.management->ListForgets().empty(),
                  "and nothing took effect first: the file, the Registry (Base, modules, no block), the credential and "
                  "the settings are as before");
        API_CHECK(SendHeartbeat(hub.registry, base_a) == ingest::kCoapCodeChanged ||
                      SendHeartbeat(hub.registry, base_a) == ingest::kCoapCodeValid,
                  "the Base's data is still accepted");
        API_CHECK(hub.management->HandleDatagram(PollFor(kEuiA, ga)).code == ingest::kCoapCodeContent,
                  "and so is its authenticated poll");
    }

    // 4. Carried out.
    NothingHeardAnyMore(hub);
    hub.steady_shift += std::chrono::seconds(600); // the heartbeat of the check above is old too
    const std::string before_ok = hub.StateFile();
    API_CHECK(hub.management->ForgetPowerBase("dev1", kEuiA, ga.generation, RequestId(4), done) == ForgetOutcome::Removed,
              "offline, the listed registration, storable: the Base is taken out of the list");
    const std::string forget_id = done.forget_id;
    API_CHECK(done.forget_id.size() == 32 && done.request_id == RequestId(4) && done.power_base_uuid == kEuiA &&
                  done.record == ga.generation && done.managed && done.affected_module_count == 2 &&
                  done.cancelled_removal_id.empty() && done.kept_removal_id.empty() && done.settings_cleanup_pending &&
                  done.requested_by_device_id == "dev1",
              "its record: the Base, the registration, who asked, two modules, no removal touched, the settings clean-up owed");
    API_CHECK(ForgetRecordJson(done).find("\"state\":\"removed_from_hub\"") != std::string::npos &&
                  ForgetRecordJson(done).find("completed") == std::string::npos &&
                  ForgetRecordJson(done).find("erased") == std::string::npos,
              "it is reported as 'removed_from_hub' -- never as completed, never as erased");
    {
        ingest::RegistryView view = hub.registry.view();
        API_CHECK(!HasBase(view, base_a) && !HasNode(view, base_a, m1) && !HasNode(view, base_a, m2),
                  "the Registry: the Base and the nodes registered under it are gone");
        API_CHECK(HasBase(view, base_b) && HasNode(view, base_b, m2) && HasNode(view, base_b, m3),
                  "the other Base, its module and the module that had moved to it are untouched");
    }
    API_CHECK(hub.management->IsBlocked(kEuiA) && hub.registry.is_power_base_blocked(base_a) &&
                  !hub.management->IsManaged(kEuiA) &&
                  hub.management->ManagedPowerBases() == std::vector<std::string>{kEuiB} && RecordOf(hub, kEuiA).empty() &&
                  RecordOf(hub, kEuiB) == gb.generation && hub.management->IsManaged(kEuiB),
              "the Base is blocked and no longer listed; the other Base is listed with its registration as before");
    API_CHECK(hub.StateFile().find(ga.key) == std::string::npos && hub.StateFile().find(gb.key) != std::string::npos &&
                  hub.StateFile().find("\"schema_version\":2,") != std::string::npos &&
                  hub.StateFile().find("\"forgets\":[{\"forget_id\":\"" + forget_id + "\"") != std::string::npos,
              "the state file: that registration's key is gone, the other Base's is there, the list removal is recorded "
              "(schema 2, an added list)");
    API_CHECK(hub.StateFile().find("\"modules\":[\"" + m1_hex + "\"],") != std::string::npos,
              "the clean-up noted with it lists the module that left -- and not the one that is registered under the "
              "other Base");
    RemovalJob no_job;
    API_CHECK(hub.management->ListRemovals().empty() && !hub.management->GetRemoval(forget_id, no_job),
              "no removal job was made of it: nothing is 'pending', 'released' or 'completed'");

    // The rooms: only of the module that left with the Base.
    API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && hub.removed_modules.size() == 1 &&
                  hub.removed_modules[0] == std::vector<std::string>{m1_hex},
              "the settings clean-up clears the room of the module that left with it -- not of the one registered under "
              "the other Base");
    API_CHECK(hub.management->GetForget(forget_id, again) && !again.settings_cleanup_pending,
              "and the record says the clean-up is done");

    // 5. What arrives afterwards from that registration revives nothing.
    const std::string after = hub.StateFile();
    API_CHECK(SendHeartbeat(hub.registry, base_a) == ingest::kCoapCodeForbidden &&
                  SendTopology(hub.registry, base_a, m1, 2) == ingest::kCoapCodeForbidden &&
                  SendSample(hub.registry, base_a, m1, 9) == ingest::kCoapCodeForbidden &&
                  !HasBase(hub.registry.view(), base_a) && !HasNode(hub.registry.view(), base_a, m1),
              "a late heartbeat, topology report and sample of that Base are refused (4.03) and list nothing");
    std::array<uint8_t, 16> some_command{};
    some_command[0] = 7;
    RemovalJob stale_job;
    API_CHECK(hub.management->HandleDatagram(PollFor(kEuiA, ga)).code == ingest::kCoapCodeUnauthorized &&
                  hub.management->HandleDatagram(HelloFor(kEuiA, ga, PB_MGMT_BOARD_CAP_THREAD_RECEIPT)).code ==
                      ingest::kCoapCodeUnauthorized &&
                  hub.management->HandleDatagram(LocateFor(kEuiA, ga.hub_id, ga.generation, ga.key, 1)).code ==
                      ingest::kCoapCodeUnauthorized &&
                  hub.management->HandleLocateDatagram(LocateFor(kEuiA, ga.hub_id, ga.generation, ga.key, 2)).code ==
                      ingest::kCoapCodeUnauthorized &&
                  hub.management->HandleDatagram(AcceptedFor(kEuiA, ga, some_command)).code == ingest::kCoapCodeUnauthorized &&
                  hub.management->HandleDatagram(DoneFor(kEuiA, ga, some_command, PB_MGMT_RESULT_RELEASED)).code ==
                      ingest::kCoapCodeUnauthorized &&
                  hub.management->HandleDatagram(DoneFor(kEuiA, ga, some_command, PB_MGMT_RESULT_ERASED)).code ==
                      ingest::kCoapCodeUnauthorized &&
                  hub.management->SubmitReleaseReceipt(DoneFor(kEuiA, ga, some_command, PB_MGMT_RESULT_ERASED), stale_job) ==
                      ReceiptOutcome::Rejected,
              "its poll, HELLO, LOCATE (either port), accepted, released notice and receipt are refused: the key is not "
              "this hub's any more");
    API_CHECK(board.Poll() != PB_MGMT_POLL_IDLE && board.Poll() != PB_MGMT_POLL_UNENROLLED && board.StoredState() == 10 &&
                  board.dataset,
              "the real board: its polls get no answer it accepts, and it is exactly as it was -- enrolled, its network "
              "stored. The hub's list changed; the board was not erased");
    API_CHECK(hub.StateFile() == after && hub.management->IsBlocked(kEuiA) && !hub.management->IsManaged(kEuiA) &&
                  hub.management->ListRemovals().empty(),
              "none of that changed anything on the hub");

    // 6. The same request again, and a later one.
    API_CHECK(hub.management->ForgetPowerBase("dev2", kEuiA, ga.generation, RequestId(4), again) == ForgetOutcome::Existing &&
                  again.forget_id == forget_id && again.requested_by_device_id == "dev1" && hub.StateFile() == after,
              "the same request id again (the answer was lost): the same record, nothing done twice");
    API_CHECK(hub.management->ForgetPowerBase("dev1", kEuiB, gb.generation, RequestId(4), again) == ForgetOutcome::RequestConflict &&
                  hub.management->IsManaged(kEuiB) && hub.StateFile() == after,
              "that request id for another Base: refused, the other Base untouched");
    API_CHECK(hub.management->ForgetPowerBase("dev2", kEuiA, ga.generation, RequestId(5), again) ==
                      ForgetOutcome::UnknownPowerBase &&
                  hub.management->ListForgets().size() == 1 && hub.StateFile() == after,
              "a second device asking for the same Base later: it is not listed any more (unknown) -- one list removal, "
              "not two");

    // 7. A restart: a new process with an empty Registry.
    Hub restarted(hub);
    restarted.steady_shift = hub.steady_shift;
    API_CHECK(restarted.Open() && restarted.management->IsBlocked(kEuiA) &&
                  restarted.registry.is_power_base_blocked(base_a) && !restarted.management->IsManaged(kEuiA) &&
                  restarted.management->ManagedPowerBases() == std::vector<std::string>{kEuiB} &&
                  restarted.management->GetForget(forget_id, again) && again.record == ga.generation &&
                  restarted.management->ListForgets().size() == 1,
              "hub restart: the block, the dropped registration and the record are restored from the state file");
    API_CHECK(SendHeartbeat(restarted.registry, base_a) == ingest::kCoapCodeForbidden &&
                  restarted.management->HandleDatagram(PollFor(kEuiA, ga)).code == ingest::kCoapCodeUnauthorized &&
                  !HasBase(restarted.registry.view(), base_a),
              "after the restart its data and its poll are still refused");
    {
        SnapshotManagement extra;
        extra.managed_power_bases = restarted.management->ManagedPowerBases();
        extra.forget_supported = true;
        extra.records = restarted.management->PowerBaseRecords(extra.managed_power_bases);
        std::string snapshot = BuildSnapshotJson(restarted.registry.view(), 1, 0, &extra);
        API_CHECK(snapshot.find(kEuiA) == std::string::npos &&
                      snapshot.find(std::string("{\"uuid\":\"") + kEuiB +
                                    "\",\"online\":false,\"module_count\":0,\"online_module_count\":0,"
                                    "\"management\":\"supported\",\"erase_confirmation\":\"manual\",\"record\":\"" +
                                    gb.generation + "\"}") != std::string::npos,
                  "the snapshot after the restart: the removed Base is not listed (not even as a silent managed one); "
                  "the other is, with its record");
    }

    // 8. Only a NEW authenticated registration brings the Base back.
    ManagementGrant ga2;
    API_CHECK(restarted.management->IssueCredential(kEuiA, ga2) && restarted.management->IsBlocked(kEuiA) &&
                  SendHeartbeat(restarted.registry, base_a) == ingest::kCoapCodeForbidden,
              "a new registration is started: issuing its credential alone unblocks nothing");
    API_CHECK(ActivateDirect(*restarted.management, kEuiA, ga2) && !restarted.management->IsBlocked(kEuiA) &&
                  restarted.management->IsManaged(kEuiA) && SendHeartbeat(restarted.registry, base_a) == ingest::kCoapCodeChanged &&
                  SendTopology(restarted.registry, base_a, m1, 1) == ingest::kCoapCodeChanged &&
                  RecordOf(restarted, kEuiA) == ga2.generation,
              "the board's first authenticated poll with the new key: unblocked, listed, its data accepted, a new record");
    API_CHECK(restarted.management->HandleDatagram(PollFor(kEuiA, ga)).code == ingest::kCoapCodeUnauthorized,
              "the earlier registration's key stays refused");
    // A late copy of the old request, and an old confirmation, against the new registration.
    restarted.steady_shift += std::chrono::seconds(600);
    const std::string with_new = restarted.StateFile();
    API_CHECK(restarted.management->ForgetPowerBase("dev1", kEuiA, ga.generation, RequestId(4), again) == ForgetOutcome::Existing &&
                  restarted.management->ForgetPowerBase("dev1", kEuiA, ga.generation, RequestId(6), again) ==
                      ForgetOutcome::StaleRecord &&
                  restarted.management->IsManaged(kEuiA) && !restarted.management->IsBlocked(kEuiA) &&
                  HasNode(restarted.registry.view(), base_a, m1) && restarted.StateFile() == with_new,
              "the old request again, and a new request that still names the old registration: neither touches the new "
              "registration, its module or its data");

    // 9. The record is kept as long as a finished removal, then dropped from the list.
    g_now_ms += 11 * 60 * 1000;
    API_CHECK(restarted.management->ListForgets().empty(), "eleven minutes later the record is no longer listed");
}

void ListRemovalOfUnmanagedBase(int *failures)
{
    std::printf("\n-- list removal of a Base without a management credential (an earlier way of registering) --\n");
    const char kEuiC[] = "6055f9fffe009abc";
    const ingest::Uuid base_c = Uuid(kEuiC);
    const ingest::Uuid m5 = Module(5);
    Hub hub;
    hub.Open();
    SendHeartbeat(hub.registry, base_c);
    SendTopology(hub.registry, base_c, m5, 1);
    const std::string record = RecordOf(hub, kEuiC);
    API_CHECK(record.size() == 32 && RecordOf(hub, kEuiC) == record && !hub.management->IsManaged(kEuiC),
              "a Base known from its data only has a record too: made up by the hub, the same every time it is asked");
    RemovalJob job;
    API_CHECK(RequestRemovalNow(*hub.management, "dev1", kEuiC, job) == RemovalRequestOutcome::Unsupported,
              "its removal is still unsupported: nothing is forced on a board that cannot prove anything");
    ForgetRecord done;
    const std::string before = hub.StateFile();
    API_CHECK(hub.management->ForgetPowerBase("dev1", kEuiC, record, RequestId(20), done) == ForgetOutcome::Online &&
                  hub.StateFile() == before,
              "while it is heard: refused (online)");

    // It went silent, the confirmation was opened -- and then it came back.
    NothingHeardAnyMore(hub);
    SendHeartbeat(hub.registry, base_c);
    hub.steady_shift = std::chrono::seconds(0);
    API_CHECK(hub.management->ForgetPowerBase("dev1", kEuiC, record, RequestId(20), done) == ForgetOutcome::Online &&
                  HasBase(hub.registry.view(), base_c) && !hub.management->IsBlocked(kEuiC) && hub.StateFile() == before,
              "it reconnected after the confirmation was opened: the old request is refused and the Base stays");

    // Silent again -- and registered anew before the request arrives.
    NothingHeardAnyMore(hub);
    ManagementGrant gc;
    API_CHECK(hub.management->IssueCredential(kEuiC, gc) && RecordOf(hub, kEuiC) == gc.generation && gc.generation != record,
              "(a registration of that board is started: the listed record is now that registration's)");
    const std::string with_new = hub.StateFile();
    API_CHECK(hub.management->ForgetPowerBase("dev1", kEuiC, record, RequestId(21), done) == ForgetOutcome::StaleRecord &&
                  hub.StateFile() == with_new && hub.StateFile().find(gc.key) != std::string::npos,
              "the old confirmation names the earlier record: refused (stale); the new registration's credential is untouched");
    API_CHECK(ActivateDirect(*hub.management, kEuiC, gc) && hub.management->IsManaged(kEuiC),
              "and that registration completes as if nothing had been asked");

    // A Base that stays unmanaged and silent: taken out of the list.
    const char kEuiD[] = "6055f9fffe00def0";
    const ingest::Uuid base_d = Uuid(kEuiD);
    const ingest::Uuid m6 = Module(6);
    Hub old_hub;
    old_hub.Open();
    SendHeartbeat(old_hub.registry, base_d);
    SendTopology(old_hub.registry, base_d, m6, 1);
    const std::string record_d = RecordOf(old_hub, kEuiD);
    NothingHeardAnyMore(old_hub);
    API_CHECK(old_hub.management->ForgetPowerBase("dev1", kEuiD, record_d, RequestId(22), done) == ForgetOutcome::Removed &&
                  !done.managed && done.affected_module_count == 1 && done.settings_cleanup_pending,
              "silent and unmanaged: taken out of the list (recorded as not managed)");
    API_CHECK(!HasBase(old_hub.registry.view(), base_d) && !HasNode(old_hub.registry.view(), base_d, m6) &&
                  old_hub.management->IsBlocked(kEuiD) && RecordOf(old_hub, kEuiD).empty() &&
                  SendHeartbeat(old_hub.registry, base_d) == ingest::kCoapCodeForbidden &&
                  SendTopology(old_hub.registry, base_d, m6, 2) == ingest::kCoapCodeForbidden,
              "it and its module are gone, it is blocked, and its late data lists nothing");
    API_CHECK(old_hub.management->ProcessSettingsCleanups() == 0 && old_hub.removed_modules.size() == 1 &&
                  old_hub.removed_modules[0] == std::vector<std::string>{"744dbdfffe650006"},
              "its module's room is cleared by the same clean-up");
    Hub restarted(old_hub);
    API_CHECK(restarted.Open() && restarted.management->IsBlocked(kEuiD) &&
                  SendHeartbeat(restarted.registry, base_d) == ingest::kCoapCodeForbidden &&
                  RecordOf(restarted, kEuiD).empty(),
              "hub restart: still blocked -- an unmanaged Base does not come back by being heard");
    ManagementGrant gd;
    API_CHECK(restarted.management->IssueCredential(kEuiD, gd) && ActivateDirect(*restarted.management, kEuiD, gd) &&
                  !restarted.management->IsBlocked(kEuiD) && SendHeartbeat(restarted.registry, base_d) == ingest::kCoapCodeChanged,
              "it comes back by being registered through the app (a credential, and the board's authenticated poll)");

    // A record made up by one process means nothing to the next.
    const char kEuiE[] = "6055f9fffe00e111";
    const ingest::Uuid base_e = Uuid(kEuiE);
    Hub first;
    first.Open();
    SendHeartbeat(first.registry, base_e);
    const std::string record_e = RecordOf(first, kEuiE);
    Hub second(first);
    second.Open();
    SendHeartbeat(second.registry, base_e);
    const std::string record_e2 = RecordOf(second, kEuiE);
    NothingHeardAnyMore(second);
    API_CHECK(record_e.size() == 32 && record_e2.size() == 32 && record_e2 != record_e &&
                  second.management->ForgetPowerBase("dev1", kEuiE, record_e, RequestId(23), done) == ForgetOutcome::StaleRecord &&
                  HasBase(second.registry.view(), base_e) && !second.management->IsBlocked(kEuiE),
              "a confirmation opened before a hub restart names a record the restarted hub never gave: refused (stale)");
    API_CHECK(second.management->ForgetPowerBase("dev1", kEuiE, record_e2, RequestId(24), done) == ForgetOutcome::Removed,
              "with the record read after the restart it is carried out");
}

void ListRemovalAndRemovalJobs(int *failures)
{
    std::printf("\n-- list removal meets a removal job: pending is cancelled, a command that may be with the board is left alone --\n");
    Hub hub;
    hub.Open();
    ForgetRecord done;
    RemovalJob job, now;

    // Pending: the command was never given.
    ManagementGrant ga;
    hub.management->IssueCredential(kEuiA, ga);
    ActivateDirect(*hub.management, kEuiA, ga);
    NothingHeardAnyMore(hub);
    API_CHECK(RequestRemovalNow(*hub.management, "dev1", kEuiA, job) == RemovalRequestOutcome::Created &&
                  job.state == RemovalState::Pending,
              "(an offline Base with a pending removal: 'when it comes back, erase the board')");
    const size_t slots_before = hub.management->FreeBlockSlots();
    API_CHECK(hub.management->ForgetPowerBase("dev1", kEuiA, ga.generation, RequestId(40), done) == ForgetOutcome::Removed &&
                  done.cancelled_removal_id == job.removal_id && done.kept_removal_id.empty() &&
                  StateOf(hub, job.removal_id) == RemovalState::Cancelled,
              "list removal of it: the pending removal ends as cancelled in the same stored change, and the record says so");
    API_CHECK(hub.management->FreeBlockSlots() == slots_before && hub.StateFile().find(ga.key) == std::string::npos &&
                  hub.management->HandleDatagram(PollFor(kEuiA, ga)).code == ingest::kCoapCodeUnauthorized,
              "the block uses the slot that removal had reserved; the key is gone, so the command can never be given");

    // Sent: the command may be with the board.
    ManagementGrant gb;
    hub.management->IssueCredential(kEuiB, gb);
    ActivateDirect(*hub.management, kEuiB, gb);
    RequestRemovalNow(*hub.management, "dev1", kEuiB, job);
    std::array<uint8_t, 16> command{};
    API_CHECK(FetchCommandDirect(*hub.management, kEuiB, gb, command) && StateOf(hub, job.removal_id) == RemovalState::Sent,
              "(a second Base: its removal's command went out -- 'sent')");
    const std::string sent_id = job.removal_id;
    API_CHECK(hub.management->ForgetPowerBase("dev1", kEuiB, gb.generation, RequestId(41), done) == ForgetOutcome::Removed &&
                  done.kept_removal_id == sent_id && done.cancelled_removal_id.empty(),
              "list removal of it is carried out, and the record names the removal that was left as it is");
    API_CHECK(hub.management->GetRemoval(sent_id, now) && now.state == RemovalState::Sent && !now.cancellable &&
                  hub.StateFile().find(gb.key) != std::string::npos &&
                  hub.management->CancelRemoval(sent_id, now) == RemovalCancelOutcome::NotCancellable,
              "the job is still 'sent' -- not cancelled, not completed -- and its key is kept for it");
    RemovalJob cancelled_one;
    API_CHECK(now.power_base_list_removed && RemovalJobJson(now).find("\"list_removed\":true") != std::string::npos &&
                  RemovalJobJson(now).find("\"state\":\"sent\"") != std::string::npos &&
                  RemovalJobJson(now).find("\"completed_at_unix_ms\":null") != std::string::npos,
              "and it says what happened to the LIST, apart from its own state: the Base was taken out of it (list_removed) "
              "-- sent, not completed");
    API_CHECK(hub.management->GetRemoval(done.kept_removal_id, cancelled_one) && cancelled_one.removal_id == sent_id &&
                  hub.management->ListRemovals().size() >= 2,
              "(the record names that job)");
    for (const RemovalJob &j : hub.management->ListRemovals()) {
        if (j.removal_id != sent_id) {
            API_CHECK(!j.power_base_list_removed && RemovalJobJson(j).find("list_removed") == std::string::npos,
                      "a job that ended as cancelled with the list removal has no such field");
        }
    }
    API_CHECK(hub.management->IsBlocked(kEuiB) && !hub.management->IsManaged(kEuiB) &&
                  hub.management->ManagedPowerBases().empty() && RecordOf(hub, kEuiB).empty() &&
                  SendHeartbeat(hub.registry, Uuid(kEuiB)) == ingest::kCoapCodeForbidden,
              "the Base itself is out of the list and blocked: its data lists nothing");
    ForgetRecord twice;
    API_CHECK(hub.management->ForgetPowerBase("dev2", kEuiB, gb.generation, RequestId(44), twice) ==
                      ForgetOutcome::UnknownPowerBase &&
                  hub.management->ListForgets().size() == 2 && StateOf(hub, sent_id) == RemovalState::Sent,
              "another request for it: the Base is not listed (unknown) although its key is kept -- nothing is done twice");
    g_now_ms += 3 * 24 * 3600 * 1000LL;
    API_CHECK(hub.management->GetRemoval(sent_id, now) && now.state == RemovalState::Sent && now.result_unknown,
              "days of silence later the job is 'sent, result unknown' -- silence completes nothing");
    // The board did get the command, and reports.
    API_CHECK(hub.management->HandleDatagram(AcceptedFor(kEuiB, gb, command)).code == ingest::kCoapCodeContent &&
                  StateOf(hub, sent_id) == RemovalState::Accepted,
              "the board's authenticated 'accepted' still moves that job on");
    API_CHECK(hub.management->GetRemoval(sent_id, now) && now.power_base_list_removed,
              "(accepted, of a Base that is out of the list: still marked list_removed)");
    API_CHECK(hub.management->HandleDatagram(DoneFor(kEuiB, gb, command, PB_MGMT_RESULT_RELEASED)).code ==
                      ingest::kCoapCodeContent &&
                  StateOf(hub, sent_id) == RemovalState::Released && hub.management->IsBlocked(kEuiB),
              "its 'released' notice too -- released, not completed");
    API_CHECK(hub.management->GetRemoval(sent_id, now) && !now.power_base_list_removed &&
                  RemovalJobJson(now).find("list_removed") == std::string::npos,
              "from 'released' on the job speaks for itself: the mark is gone");
    RemovalJob completed;
    API_CHECK(hub.management->SubmitReleaseReceipt(DoneFor(kEuiB, gb, command, PB_MGMT_RESULT_ERASED), completed) ==
                      ReceiptOutcome::Completed &&
                  completed.state == RemovalState::Completed && hub.StateFile().find(gb.key) == std::string::npos &&
                  hub.management->IsBlocked(kEuiB),
              "and only its authenticated receipt (erased and checked) completes the removal; then the key is dropped. "
              "The Base stays blocked");

    // Released: the Base is not listed any more.
    const char kEuiC[] = "6055f9fffe009abc";
    ManagementGrant gc;
    hub.management->IssueCredential(kEuiC, gc);
    ActivateDirect(*hub.management, kEuiC, gc);
    RequestRemovalNow(*hub.management, "dev1", kEuiC, job);
    std::array<uint8_t, 16> command_c{};
    API_CHECK(ReleaseDirect(*hub.management, kEuiC, gc, command_c) == ingest::kCoapCodeContent &&
                  StateOf(hub, job.removal_id) == RemovalState::Released,
              "(a third Base: its board said 'released')");
    const std::string released_file = hub.StateFile();
    API_CHECK(hub.management->ForgetPowerBase("dev1", kEuiC, gc.generation, RequestId(42), done) ==
                      ForgetOutcome::UnknownPowerBase &&
                  StateOf(hub, job.removal_id) == RemovalState::Released && hub.StateFile() == released_file,
              "list removal of a released Base: it is not listed (unknown); the job waits for its receipt as before");
    API_CHECK(hub.management->SubmitReleaseReceipt(DoneFor(kEuiC, gc, command_c, PB_MGMT_RESULT_ERASED), completed) ==
                  ReceiptOutcome::Completed,
              "and its receipt still completes it");

    // Clock changes prove nothing.
    const char kEuiF[] = "6055f9fffe00f222";
    ManagementGrant gf;
    hub.management->IssueCredential(kEuiF, gf);
    ActivateDirect(*hub.management, kEuiF, gf);
    RequestRemovalNow(*hub.management, "dev1", kEuiF, job);
    std::array<uint8_t, 16> command_f{};
    FetchCommandDirect(*hub.management, kEuiF, gf, command_f);
    const int64_t real_now = g_now_ms;
    g_now_ms -= 400LL * 24 * 3600 * 1000; // the hub's clock jumps back more than a year
    API_CHECK(hub.management->ForgetPowerBase("dev1", kEuiF, gf.generation, RequestId(43), done) == ForgetOutcome::Removed &&
                  StateOf(hub, job.removal_id) == RemovalState::Sent,
              "with the clock set back a year: carried out; the sent removal is still 'sent'");
    g_now_ms = real_now + 400LL * 24 * 3600 * 1000; // and then far forward
    ForgetRecord again;
    API_CHECK(StateOf(hub, job.removal_id) == RemovalState::Sent &&
                  hub.management->ForgetPowerBase("dev1", kEuiF, gf.generation, RequestId(43), again) != ForgetOutcome::Removed &&
                  hub.management->IsBlocked(kEuiF) && hub.management->IsBlocked(kEuiA) && hub.management->IsBlocked(kEuiB),
              "with the clock far ahead: still 'sent', nothing done twice, and no block was given up for being old");
    g_now_ms = real_now;

    // The board is registered again while that job is still 'sent'.
    const std::string kept_id = job.removal_id;
    ManagementGrant gf2;
    RemovalJob fresh;
    API_CHECK(hub.management->IssueCredential(kEuiF, gf2) && ActivateDirect(*hub.management, kEuiF, gf2) &&
                  hub.management->GetRemoval(kept_id, now) && now.state == RemovalState::Superseded &&
                  !now.power_base_list_removed && hub.StateFile().find(gf.key) == std::string::npos,
              "a new registration of that board: the kept job ends as superseded (never completed) and its key is dropped");
    API_CHECK(hub.management->IsManaged(kEuiF) && RecordOf(hub, kEuiF) == gf2.generation &&
                  RequestRemovalNow(*hub.management, "dev1", kEuiF, fresh) == RemovalRequestOutcome::Created &&
                  fresh.removal_id != kept_id && fresh.generation == gf2.generation && !fresh.power_base_list_removed,
              "and the old job is not in the way: the new registration is listed, managed, and can be removed with a new job");
}

void ListRemovalLimitsAndStorage(int *failures)
{
    std::printf("\n-- list removal: block capacity, record capacity, damaged files, a clean-up that fails --\n");
    ForgetRecord done;
    {
        // 63 Bases blocked by released removals, one pending removal: every block slot is in use.
        Hub hub;
        hub.Open();
        bool all_ok = true;
        for (int i = 0; i < 63; i++) {
            char eui[17];
            std::snprintf(eui, sizeof(eui), "6055f9fffe10%04x", i + 1);
            ManagementGrant g;
            RemovalJob job;
            std::array<uint8_t, 16> command{};
            all_ok = all_ok && hub.management->IssueCredential(eui, g) && ActivateDirect(*hub.management, eui, g) &&
                     RequestRemovalNow(*hub.management, "dev1", eui, job) == RemovalRequestOutcome::Created &&
                     ReleaseDirect(*hub.management, eui, g, command) == ingest::kCoapCodeContent;
        }
        ManagementGrant gp;
        RemovalJob pending;
        all_ok = all_ok && hub.management->IssueCredential(kEuiA, gp) && ActivateDirect(*hub.management, kEuiA, gp) &&
                 RequestRemovalNow(*hub.management, "dev1", kEuiA, pending) == RemovalRequestOutcome::Created;
        API_CHECK(all_ok && hub.management->FreeBlockSlots() == 0,
                  "(63 Power Bases blocked by removals and one pending removal: no block slot is free)");
        const ingest::Uuid base_b = Uuid(kEuiB);
        SendHeartbeat(hub.registry, base_b);
        const std::string record_b = RecordOf(hub, kEuiB);
        NothingHeardAnyMore(hub);
        const std::string before = hub.StateFile();
        API_CHECK(hub.management->ForgetPowerBase("dev1", kEuiB, record_b, RequestId(60), done) == ForgetOutcome::CapacityFull &&
                      hub.StateFile() == before && HasBase(hub.registry.view(), base_b) && !hub.management->IsBlocked(kEuiB) &&
                      CountOf(before, "\"since_unix_ms\"") == 63,
                  "list removal of one more Base: refused (capacity full) -- nothing changed and none of the 63 blocks "
                  "was given up");
        API_CHECK(hub.management->ForgetPowerBase("dev1", kEuiA, gp.generation, RequestId(61), done) == ForgetOutcome::Removed &&
                      done.cancelled_removal_id == pending.removal_id &&
                      CountOf(hub.StateFile(), "\"since_unix_ms\"") == 64 && hub.management->FreeBlockSlots() == 0,
                  "the Base with the pending removal can be taken out: its block is the slot that removal had reserved");
    }
    {
        // The records are bounded too; none that is recent is dropped to make room.
        Hub hub;
        hub.Open();
        NothingHeardAnyMore(hub);
        bool all_ok = true;
        for (unsigned i = 0; i < kMaxForgetRecords; i++) {
            ManagementGrant g;
            all_ok = all_ok && hub.management->IssueCredential(kEuiA, g) && ActivateDirect(*hub.management, kEuiA, g) &&
                     hub.management->ForgetPowerBase("dev1", kEuiA, g.generation, RequestId(100 + i), done) ==
                         ForgetOutcome::Removed;
        }
        ManagementGrant g;
        hub.management->IssueCredential(kEuiA, g);
        ActivateDirect(*hub.management, kEuiA, g);
        const std::string before = hub.StateFile();
        API_CHECK(all_ok && hub.management->ListForgets().size() == kMaxForgetRecords &&
                      hub.management->ForgetPowerBase("dev1", kEuiA, g.generation, RequestId(900), done) == ForgetOutcome::TooMany &&
                      hub.StateFile() == before && hub.management->IsManaged(kEuiA),
                  "the same Base registered and taken out 64 times within minutes: the 65th is refused (too many), "
                  "nothing changed");
        g_now_ms += 11 * 60 * 1000;
        API_CHECK(hub.management->ForgetPowerBase("dev1", kEuiA, g.generation, RequestId(900), done) == ForgetOutcome::Removed &&
                      hub.management->ListForgets().size() == 1,
                  "once the earlier records are older than the retention it is carried out");
    }
    {
        // Files this hub cannot read are never replaced.
        const std::string good_prefix = "{\"schema_version\":2,\"credentials\":[],\"removals\":[],\"blocked\":[],\"cleanups\":[]";
        struct Damaged {
            const char *what;
            std::string contents;
        };
        const std::vector<Damaged> files = {
            {"not JSON", "not json"},
            {"a file cut off in the middle", good_prefix.substr(0, 40)},
            {"an unknown schema_version", "{\"schema_version\":3,\"credentials\":[],\"removals\":[],\"blocked\":[],\"cleanups\":[],\"forgets\":[]}"},
            {"a forgets list that is not a list", good_prefix + ",\"forgets\":{}}"},
            {"a forget entry with a malformed id",
             good_prefix + ",\"forgets\":[{\"forget_id\":\"xyz\",\"request_id\":\"" + RequestId(1) + "\",\"eui64\":\"" + kEuiA +
                 "\",\"record\":\"" + RequestId(2) +
                 "\",\"managed\":1,\"created_at_unix_ms\":1,\"requested_by_device_id\":\"d\",\"affected_module_count\":0,"
                 "\"cancelled_removal_id\":\"\",\"kept_removal_id\":\"\"}]}"},
            {"a forget entry that names both a cancelled and a kept removal",
             good_prefix + ",\"forgets\":[{\"forget_id\":\"" + RequestId(3) + "\",\"request_id\":\"" + RequestId(1) +
                 "\",\"eui64\":\"" + kEuiA + "\",\"record\":\"" + RequestId(2) +
                 "\",\"managed\":1,\"created_at_unix_ms\":1,\"requested_by_device_id\":\"d\",\"affected_module_count\":0,"
                 "\"cancelled_removal_id\":\"" + RequestId(4) + "\",\"kept_removal_id\":\"" + RequestId(5) + "\"}]}"},
        };
        for (const Damaged &d : files) {
            Hub hub;
            const std::string path = hub.dir + "/" + kPowerBaseMgmtFileName;
            test_support::WriteFile(path, d.contents);
            const bool loaded = hub.Open();
            SendHeartbeat(hub.registry, Uuid(kEuiA));
            NothingHeardAnyMore(hub);
            const ForgetOutcome outcome =
                hub.management->ForgetPowerBase("dev1", kEuiA, RequestId(7), RequestId(8), done);
            std::string msg = std::string(d.what) + ": management does not start, a list removal is refused "
                                                    "(unavailable), the Base stays, and the file is left as it is";
            API_CHECK(!loaded && outcome == ForgetOutcome::Unavailable && hub.StateFile() == d.contents &&
                          HasBase(hub.registry.view(), Uuid(kEuiA)) && hub.management->PowerBaseRecords({kEuiA}).empty(),
                      msg.c_str());
        }
        // A file of a hub that did not know the list (no "forgets") is an ordinary file.
        Hub hub;
        test_support::WriteFile(hub.dir + "/" + kPowerBaseMgmtFileName, good_prefix + "}\n");
        API_CHECK(hub.Open() && hub.management->ListForgets().empty(), "a state file without the forgets list loads: it has none");
    }
    {
        // The settings clean-up fails, the hub restarts, the Base is registered again meanwhile.
        const ingest::Uuid base_a = Uuid(kEuiA);
        const ingest::Uuid m1 = Module(1), m2 = Module(2);
        Hub hub;
        hub.Open();
        ManagementGrant ga;
        hub.management->IssueCredential(kEuiA, ga);
        ActivateDirect(*hub.management, kEuiA, ga);
        SendHeartbeat(hub.registry, base_a);
        SendTopology(hub.registry, base_a, m1, 1);
        SendTopology(hub.registry, base_a, m2, 1);
        NothingHeardAnyMore(hub);
        hub.unassign_ok = false;
        API_CHECK(hub.management->ForgetPowerBase("dev1", kEuiA, ga.generation, RequestId(70), done) == ForgetOutcome::Removed &&
                      hub.management->ProcessSettingsCleanups() == 1 && hub.removed_modules.size() == 1 &&
                      hub.removed_modules[0].size() == 2,
                  "(a Base with two modules is taken out; the settings cannot be written: the clean-up stays pending)");
        ForgetRecord record;
        API_CHECK(hub.management->GetForget(done.forget_id, record) && record.settings_cleanup_pending &&
                      hub.management->IsBlocked(kEuiA),
                  "the record says the clean-up is owed; the list removal itself is not undone by that");
        g_now_ms += 11 * 60 * 1000;
        API_CHECK(hub.management->ListForgets().size() == 1,
                  "a record that still owes its clean-up stays listed past the retention");

        Hub restarted(hub);
        restarted.steady_shift = hub.steady_shift;
        restarted.unassign_ok = true;
        API_CHECK(restarted.Open() && restarted.management->PendingSettingsCleanups() == 1 &&
                      restarted.management->GetForget(done.forget_id, record) && record.settings_cleanup_pending,
                  "hub restart: the owed clean-up is restored from the state file");
        // Before it is retried the board is registered again and one module is back, in a new room.
        ManagementGrant ga2;
        restarted.management->IssueCredential(kEuiA, ga2);
        API_CHECK(ActivateDirect(*restarted.management, kEuiA, ga2) &&
                      SendHeartbeat(restarted.registry, base_a) == ingest::kCoapCodeChanged &&
                      SendTopology(restarted.registry, base_a, m1, 1) == ingest::kCoapCodeChanged,
                  "(the board is registered again and one of the two modules is docked on it once more)");
        g_now_ms += 60 * 1000;
        API_CHECK(restarted.management->ProcessSettingsCleanups() == 0 && restarted.removed_modules.size() == 1 &&
                      restarted.removed_modules[0] == std::vector<std::string>{"744dbdfffe650002"},
                  "the retried clean-up clears only the module that is still gone: the room given under the new "
                  "registration is not the old clean-up's to clear");
        API_CHECK(restarted.management->IsManaged(kEuiA) && !restarted.management->IsBlocked(kEuiA) &&
                      HasNode(restarted.registry.view(), base_a, m1),
                  "and the new registration is untouched by it");
    }
}

void ListRemovalOnAnotherHub(int *failures)
{
    std::printf("\n-- a board that moved to another hub: the earlier hub's record is removed there, and only there --\n");
    const ingest::Uuid base_a = Uuid(kEuiA);
    const ingest::Uuid m1 = Module(1);
    Hub old_hub;
    old_hub.hub_id = "0f0e0d0c0b0a09080706050403020100";
    old_hub.Open();
    Hub new_hub;
    new_hub.Open();
    ManagementGrant at_old, at_new;
    old_hub.management->IssueCredential(kEuiA, at_old);
    ActivateDirect(*old_hub.management, kEuiA, at_old);
    SendHeartbeat(old_hub.registry, base_a);
    SendTopology(old_hub.registry, base_a, m1, 1);
    // The board is reset by its button and registered at the other hub.
    new_hub.management->IssueCredential(kEuiA, at_new);
    API_CHECK(at_old.hub_id != at_new.hub_id && ActivateDirect(*new_hub.management, kEuiA, at_new) &&
                  SendHeartbeat(new_hub.registry, base_a) == ingest::kCoapCodeChanged &&
                  SendTopology(new_hub.registry, base_a, m1, 1) == ingest::kCoapCodeChanged,
              "(the board is registered at a second hub and reports there; the first hub still lists it)");
    const std::string new_hub_file = new_hub.StateFile();
    ForgetRecord done;
    NothingHeardAnyMore(old_hub);
    NothingHeardAnyMore(new_hub);
    // A request for the wrong hub is one that hub cannot match.
    API_CHECK(new_hub.management->ForgetPowerBase("dev1", kEuiA, at_old.generation, RequestId(80), done) ==
                      ForgetOutcome::StaleRecord &&
                  new_hub.StateFile() == new_hub_file && new_hub.management->IsManaged(kEuiA),
              "the earlier hub's record, sent to the new hub: refused (stale) -- the new registration stays");
    API_CHECK(old_hub.management->ForgetPowerBase("dev1", kEuiA, at_old.generation, RequestId(81), done) == ForgetOutcome::Removed &&
                  old_hub.management->IsBlocked(kEuiA) && !HasBase(old_hub.registry.view(), base_a),
              "at the earlier hub: taken out of its list");
    API_CHECK(new_hub.StateFile() == new_hub_file && new_hub.management->IsManaged(kEuiA) &&
                  !new_hub.management->IsBlocked(kEuiA) && HasNode(new_hub.registry.view(), base_a, m1) &&
                  new_hub.management->HandleDatagram(PollFor(kEuiA, at_new)).code == ingest::kCoapCodeContent,
              "the new hub is untouched: the registration, the module and the board's poll there are as before");
    API_CHECK(old_hub.management->HandleDatagram(PollFor(kEuiA, at_new)).code == ingest::kCoapCodeUnauthorized &&
                  SendHeartbeat(old_hub.registry, base_a) == ingest::kCoapCodeForbidden,
              "the board's messages for the new hub mean nothing to the earlier one, and its data stays refused there");
}


// ---- the shared settings in the room scenarios ----

const char kRoomA[] = "11111111-2222-4333-8444-555555555555";
const char kRoomB[] = "22222222-2222-4333-8444-555555555555";

// A settings store in <hub dir>/settings with the two rooms above, where each
// of `modules` is {uuid, name ("" = none), room ("" = none)}.
std::shared_ptr<SettingsStore> NewSettings(const Hub &hub, const std::vector<std::array<std::string, 3>> &modules)
{
    const std::string dir = hub.dir + "/settings";
    mkdir(dir.c_str(), 0700);
    auto store = std::make_shared<SettingsStore>(dir);
    store->Load();
    HomeSettings settings;
    settings.has_home_name = true;
    settings.home_name = "Home";
    for (int i = 0; i < 2; i++) {
        RoomSetting room;
        room.id = i == 0 ? kRoomA : kRoomB;
        room.name_kind = RoomNameKind::Custom;
        room.name = i == 0 ? "Study" : "Hall";
        room.symbol = "books.vertical";
        settings.rooms.push_back(room);
    }
    for (const auto &m : modules) {
        ModuleSetting module;
        module.uuid = m[0];
        module.has_name = !m[1].empty();
        module.name = m[1];
        module.has_room = !m[2].empty();
        module.room_id = m[2];
        settings.modules.push_back(module);
    }
    store->Update("", settings, g_now_ms);
    return store;
}

// The room `module` is in ("" when it has none, or no entry at all).
std::string RoomOf(SettingsStore &store, const ingest::Uuid &module)
{
    const SettingsSnapshot snapshot = store.Get();
    for (const ModuleSetting &m : snapshot.settings.modules) {
        if (m.uuid == FormatUuidHex(module)) {
            return m.has_room ? m.room_id : std::string();
        }
    }
    return std::string();
}

std::string NameOf(SettingsStore &store, const ingest::Uuid &module)
{
    const SettingsSnapshot snapshot = store.Get();
    for (const ModuleSetting &m : snapshot.settings.modules) {
        if (m.uuid == FormatUuidHex(module)) {
            return m.has_name ? m.name : std::string();
        }
    }
    return std::string();
}

// A device saves: `module` is in `room` now. Based on the current revision,
// like every writer.
bool Assign(SettingsStore &store, const ingest::Uuid &module, const std::string &room)
{
    const SettingsSnapshot current = store.Get();
    HomeSettings changed = current.settings;
    bool found = false;
    for (ModuleSetting &m : changed.modules) {
        if (m.uuid == FormatUuidHex(module)) {
            m.has_room = !room.empty();
            m.room_id = room;
            found = true;
        }
    }
    if (!found) {
        ModuleSetting m;
        m.uuid = FormatUuidHex(module);
        m.has_room = !room.empty();
        m.room_id = room;
        changed.modules.push_back(m);
    }
    return store.Update(current.revision, changed, g_now_ms).outcome == SettingsUpdateOutcome::Saved;
}

// Base A, registered with a credential (`grant`), with modules 1 and 2 -- both
// named and in room A -- and an unrelated module 9 in room B that is on no
// Base of this hub. The hooks are the production ones around a real store.
void RoomWorld(Hub &hub, ManagementGrant &grant)
{
    hub.settings = NewSettings(hub, {{FormatUuidHex(Module(1)), "Desk", kRoomA},
                                     {FormatUuidHex(Module(2)), "Shelf", kRoomA},
                                     {FormatUuidHex(Module(9)), "Door", kRoomB}});
    hub.Open();
    hub.management->IssueCredential(kEuiA, grant);
    ActivateDirect(*hub.management, kEuiA, grant);
    SendHeartbeat(hub.registry, Uuid(kEuiA));
    SendTopology(hub.registry, Uuid(kEuiA), Module(1), 1);
    SendTopology(hub.registry, Uuid(kEuiA), Module(2), 1);
}

// `module` is docked onto Base B, which this hub hears.
void DockOnB(Hub &hub, const ingest::Uuid &module, uint32_t seq = 1)
{
    SendHeartbeat(hub.registry, Uuid(kEuiB));
    SendTopology(hub.registry, Uuid(kEuiB), module, seq);
}

bool ForgetA(Hub &hub, const ManagementGrant &grant, unsigned request, ForgetRecord *out = nullptr)
{
    NothingHeardAnyMore(hub);
    ForgetRecord done;
    const bool removed = hub.management->ForgetPowerBase("dev", kEuiA, grant.generation, RequestId(request), done) ==
                         ForgetOutcome::Removed;
    if (out != nullptr) {
        *out = done;
    }
    return removed;
}

// Everything of the settings that a clean-up of Base A's modules must leave
// alone: the home name, the rooms, the unrelated module, and every name.
bool RestUntouched(SettingsStore &store)
{
    const SettingsSnapshot snapshot = store.Get();
    return snapshot.settings.home_name == "Home" && snapshot.settings.rooms.size() == 2 &&
           RoomOf(store, Module(9)) == kRoomB && NameOf(store, Module(9)) == "Door" &&
           NameOf(store, Module(1)) == "Desk" && NameOf(store, Module(2)) == "Shelf";
}

void RoomCleanupBinding(int *failures)
{
    std::printf("\n-- a clean-up clears the assignment a module had under the Base that left -- never one made since --\n");
    const ingest::Uuid m1 = Module(1), m2 = Module(2);

    {
        std::printf("   (a list removal; before the settings are written the module docks elsewhere and gets a new room)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        ForgetRecord done;
        API_CHECK(ForgetA(hub, grant, 100, &done) && done.settings_cleanup_pending && done.affected_module_count == 2,
                  "Base A is taken out of the list; the clean-up of its two modules is owed");
        const std::string noted = hub.StateList("cleanups");
        API_CHECK(noted.find(FormatUuidHex(m1)) != std::string::npos && noted.find(FormatUuidHex(m2)) != std::string::npos &&
                      CountOf(noted, kRoomA) == 2 && noted.find("\"generation\":\"" + grant.generation + "\"") != std::string::npos &&
                      noted.find("\"eui64\":\"" + std::string(kEuiA) + "\"") != std::string::npos,
                  "it is written down for that registration, with the room each module is in at this moment");
        // After the pass decided what is due, and before the settings hook
        // reads or writes anything: module 1 is docked onto Base B, and a
        // device puts it into room B there.
        hub.on_unassign = [&]() {
            DockOnB(hub, m1);
            API_CHECK(Assign(*hub.settings, m1, kRoomB), "(module 1 on Base B now; a device saves: room B)");
        };
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && hub.removed_modules.size() == 1,
                  "the clean-up runs once and is done");
        API_CHECK(hub.removed_targets[0].size() == 2 && hub.removed_targets[0][0].room_known &&
                      hub.removed_targets[0][0].room_id == kRoomA && hub.removed_targets[0][1].room_known,
                  "the settings were handed both modules, each bound to room A");
        API_CHECK(RoomOf(*hub.settings, m1) == kRoomB, "module 1 keeps the room it was given on Base B");
        API_CHECK(RoomOf(*hub.settings, m2).empty(), "module 2, which went with Base A, has no room now");
        API_CHECK(RestUntouched(*hub.settings), "names, rooms, the home name and the unrelated module are as they were");
        const std::string revision = hub.settings->Get().revision;
        g_now_ms += 100000;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && hub.removed_modules.size() == 1 &&
                      hub.settings->Get().revision == revision && hub.StateList("cleanups") == "\"cleanups\":[]",
                  "run again later: nothing is owed, nothing is asked, nothing is written");
        // Module 1 leaves Base B again: its room there was given on B.
        hub.registry.remove_power_base(Uuid(kEuiB));
        g_now_ms += 100000;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && RoomOf(*hub.settings, m1) == kRoomB,
                  "and when module 1 is on no Base again, the old clean-up does not come back for it");
    }

    {
        std::printf("   (the same order for a clean-up that does not know the rooms, as a hub before 0.4.1 stored it)\n");
        Hub hub;
        hub.bind_rooms = false;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        API_CHECK(ForgetA(hub, grant, 101) && hub.StateList("cleanups").find("\"rooms\"") == std::string::npos,
                  "(the clean-up names the modules only)");
        hub.on_unassign = [&]() {
            DockOnB(hub, m1);
            Assign(*hub.settings, m1, kRoomB);
        };
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && !hub.removed_targets[0][0].room_known,
                  "(it runs, and is done)");
        API_CHECK(RoomOf(*hub.settings, m1) == kRoomB && RoomOf(*hub.settings, m2).empty() && RestUntouched(*hub.settings),
                  "what is asked at the moment of the write protects module 1 all the same: it is listed under Base B");
    }

    {
        std::printf("   (the other order: the clean-up first, the new room after it)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        ForgetA(hub, grant, 102);
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && RoomOf(*hub.settings, m1).empty() &&
                      RoomOf(*hub.settings, m2).empty(),
                  "the clean-up ran: both modules without a room");
        DockOnB(hub, m1);
        API_CHECK(Assign(*hub.settings, m1, kRoomB), "(then module 1 docks onto Base B and is given room B)");
        g_now_ms += 100000;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && hub.removed_modules.size() == 1 &&
                      RoomOf(*hub.settings, m1) == kRoomB && RestUntouched(*hub.settings),
                  "nothing comes after it: the room stays");
    }

    {
        std::printf("   (between the read and the write: the write loses, and the second attempt asks again)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        ForgetA(hub, grant, 103);
        int between = 0;
        hub.before_settings_write = [&]() {
            if (between++ == 0) {
                // The settings were read and both modules were found to be
                // owed. Now, before the write: module 1 docks onto Base B and
                // a device saves room B for it.
                DockOnB(hub, m1);
                Assign(*hub.settings, m1, kRoomB);
            }
        };
        const std::string before = hub.settings->Get().revision;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && between == 2,
                  "the first write is a revision conflict; the second attempt is made from the device's settings and gets through");
        API_CHECK(RoomOf(*hub.settings, m1) == kRoomB && RoomOf(*hub.settings, m2).empty() && RestUntouched(*hub.settings) &&
                      hub.settings->Get().revision != before,
                  "module 1 keeps room B, module 2 is cleared: the device's save was seen, not overwritten");
    }

    {
        std::printf("   (the same, for a clean-up that does not know the rooms: only the question protects)\n");
        Hub hub;
        hub.bind_rooms = false;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        ForgetA(hub, grant, 104);
        int between = 0;
        hub.before_settings_write = [&]() {
            if (between++ == 0) {
                DockOnB(hub, m1);
                Assign(*hub.settings, m1, kRoomB);
            }
        };
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && between == 2 && RoomOf(*hub.settings, m1) == kRoomB &&
                      RoomOf(*hub.settings, m2).empty(),
                  "conflict, second attempt, and the question is asked again there: module 1 is listed under Base B");
    }

    {
        std::printf("   (a module that moved keeps the room it had, also when nobody touched its room)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        ForgetA(hub, grant, 105);
        hub.on_unassign = [&]() { DockOnB(hub, m1); };
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && RoomOf(*hub.settings, m1) == kRoomA &&
                      RoomOf(*hub.settings, m2).empty(),
                  "module 1 docked onto Base B before the write: it is that Base's, and room A stays its room");
        API_CHECK(hub.StateList("members").find("{\"eui64\":\"" + std::string(kEuiB) + "\",\"modules\":[\"" +
                                               FormatUuidHex(m1) + "\"]}") != std::string::npos,
                  "the state file has it under Base B");
    }

    {
        std::printf("   (a room given since, to a module that is on no Base: another assignment, not this clean-up's)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        ForgetA(hub, grant, 106);
        API_CHECK(Assign(*hub.settings, m1, kRoomB), "(a device still showing the old list moves module 1 to room B)");
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && RoomOf(*hub.settings, m1) == kRoomB &&
                      RoomOf(*hub.settings, m2).empty() && RestUntouched(*hub.settings),
                  "the clean-up clears the assignment to room A it was noted for; room B was given after it and stays");
    }

    {
        std::printf("   (the automatic removal uses the same clean-up: released, then the same race)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        RemovalJob job;
        std::array<uint8_t, 16> command{};
        API_CHECK(RequestRemovalNow(*hub.management, "dev", kEuiA, job) == RemovalRequestOutcome::Created &&
                      ReleaseDirect(*hub.management, kEuiA, grant, command) == ingest::kCoapCodeContent &&
                      hub.management->GetRemoval(job.removal_id, job) && job.state == RemovalState::Released &&
                      job.settings_cleanup_pending && CountOf(hub.StateList("cleanups"), kRoomA) == 2,
                  "Base A is unenrolled: released, and its clean-up is noted with the rooms");
        hub.on_unassign = [&]() {
            DockOnB(hub, m1);
            Assign(*hub.settings, m1, kRoomB);
        };
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && RoomOf(*hub.settings, m1) == kRoomB &&
                      RoomOf(*hub.settings, m2).empty() && RestUntouched(*hub.settings),
                  "module 1, docked onto Base B and given room B before the write, keeps it; module 2 is cleared");

        Hub same_room;
        ManagementGrant g2;
        RoomWorld(same_room, g2);
        RequestRemovalNow(*same_room.management, "dev", kEuiA, job);
        ReleaseDirect(*same_room.management, kEuiA, g2, command);
        int between = 0;
        same_room.before_settings_write = [&]() {
            if (between++ == 0) {
                DockOnB(same_room, m1);
                Assign(*same_room.settings, m1, kRoomB);
            }
        };
        API_CHECK(same_room.management->ProcessSettingsCleanups() == 0 && between == 2 &&
                      RoomOf(*same_room.settings, m1) == kRoomB && RoomOf(*same_room.settings, m2).empty(),
                  "and between its read and its write: conflict, asked again, module 1 keeps room B");
    }

    {
        std::printf("   (the Base is registered again with its modules before the clean-up ran)\n");
        Hub hub;
        ManagementGrant grant, again;
        RoomWorld(hub, grant);
        ForgetA(hub, grant, 107);
        hub.on_unassign = [&]() {
            // A new registration of the same board; module 1 reports under it
            // and is given room B. Module 2 stays away.
            hub.management->IssueCredential(kEuiA, again);
            ActivateDirect(*hub.management, kEuiA, again);
            SendHeartbeat(hub.registry, Uuid(kEuiA));
            SendTopology(hub.registry, Uuid(kEuiA), m1, 5);
            Assign(*hub.settings, m1, kRoomB);
        };
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && RoomOf(*hub.settings, m1) == kRoomB &&
                      RoomOf(*hub.settings, m2).empty(),
                  "the earlier registration's clean-up leaves the module of the new registration alone, and clears the other");
        API_CHECK(RecordOf(hub, kEuiA) == again.generation &&
                      hub.StateList("members").find(FormatUuidHex(m1)) != std::string::npos,
                  "(Base A is listed under its new registration, with module 1)");
    }

    {
        std::printf("   (a module without a room when its Base leaves: nothing is owed, and nothing is touched later)\n");
        Hub hub;
        hub.settings = NewSettings(hub, {{FormatUuidHex(m1), "Desk", ""}});
        hub.Open();
        ManagementGrant grant;
        hub.management->IssueCredential(kEuiA, grant);
        ActivateDirect(*hub.management, kEuiA, grant);
        SendHeartbeat(hub.registry, Uuid(kEuiA));
        SendTopology(hub.registry, Uuid(kEuiA), m1, 1);
        ForgetRecord done;
        API_CHECK(ForgetA(hub, grant, 108, &done) && !done.settings_cleanup_pending && done.affected_module_count == 1 &&
                      hub.management->PendingSettingsCleanups() == 0,
                  "taken out of the list with one module that has no room: no clean-up is noted");
        Assign(*hub.settings, m1, kRoomA);
        g_now_ms += 100000;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && hub.removed_modules.empty() &&
                      RoomOf(*hub.settings, m1) == kRoomA,
                  "a room it is given afterwards is never this removal's to clear");
    }

    {
        std::printf("   (the settings cannot be read when the Base leaves: the clean-up waits, and still asks)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        // The settings file is damaged; the store finds out when it is read.
        const std::string good;
        std::string contents;
        test_support::ReadFile(hub.settings->path(), contents);
        test_support::WriteFile(hub.settings->path(), "not json");
        hub.settings->Load();
        API_CHECK(hub.settings->Get().state == SettingsState::Unavailable, "(the settings file is damaged)");
        ForgetRecord done;
        API_CHECK(ForgetA(hub, grant, 109, &done) && done.settings_cleanup_pending &&
                      hub.StateList("cleanups").find("\"rooms\"") == std::string::npos &&
                      hub.StateList("cleanups").find(FormatUuidHex(m2)) != std::string::npos,
                  "the list removal is carried out; its clean-up names the modules, the rooms are not known");
        API_CHECK(hub.management->ProcessSettingsCleanups() == 1 && hub.removed_modules.size() == 1,
                  "the settings cannot be written either: the clean-up stays pending");
        // Repaired by hand, as the documentation says. Meanwhile module 1
        // went to Base B.
        test_support::WriteFile(hub.settings->path(), contents);
        hub.settings->Load();
        DockOnB(hub, m1);
        g_now_ms += 1000;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && RoomOf(*hub.settings, m1) == kRoomA &&
                      RoomOf(*hub.settings, m2).empty() && RestUntouched(*hub.settings),
                  "after the repair: module 2 is cleared, module 1 -- on Base B by now -- keeps its room");
    }

    {
        std::printf("   (the hook as a test sees it: what is handed over, and what the section lets a write have)\n");
        Hub hub;
        hub.read_rooms = [&](const std::vector<std::string> &modules, std::vector<RoomCleanupTarget> &out) {
            out.clear();
            for (const std::string &m : modules) {
                if (m == FormatUuidHex(m1)) {
                    out.push_back(Target(m, kRoomA)); // module 2 has no room
                }
            }
            return true;
        };
        hub.Open();
        ManagementGrant grant;
        hub.management->IssueCredential(kEuiA, grant);
        ActivateDirect(*hub.management, kEuiA, grant);
        SendHeartbeat(hub.registry, Uuid(kEuiA));
        SendTopology(hub.registry, Uuid(kEuiA), m1, 1);
        SendTopology(hub.registry, Uuid(kEuiA), m2, 1);
        ForgetRecord done;
        API_CHECK(ForgetA(hub, grant, 110, &done) && done.affected_module_count == 2, "(two modules, one of them in a room)");
        hub.on_unassign = [&]() { DockOnB(hub, m1); };
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && hub.removed_targets.size() == 1 &&
                      hub.removed_targets[0].size() == 1 && hub.removed_targets[0][0].module_uuid == FormatUuidHex(m1) &&
                      hub.removed_targets[0][0].room_id == kRoomA,
                  "only the module that had a room is handed to the settings, with that room");
        API_CHECK(hub.cleared_modules.size() == 1 && hub.cleared_modules[0].empty(),
                  "and when it is listed under another Base by the time the hook asks, the section gives the write nothing");
    }
}

void MembersAcrossRestart(int *failures)
{
    std::printf("\n-- what is known, after a restart, of a Base that has not reported again: its members, as written down --\n");
    const ingest::Uuid base_a = Uuid(kEuiA);
    const ingest::Uuid m1 = Module(1), m2 = Module(2);
    const std::string members_a = "{\"eui64\":\"" + std::string(kEuiA) + "\",\"modules\":[\"" + FormatUuidHex(m1) + "\",\"" +
                                  FormatUuidHex(m2) + "\"]}";
    // The process ends; the next one starts over the same directory with a
    // new, empty Registry and the same settings.
    auto restart = [](Hub &previous, Hub &next, int grace_ms) {
        previous.management.reset();
        next.settings = previous.settings;
        next.bind_rooms = previous.bind_rooms;
        next.cleanup_grace = std::chrono::milliseconds(grace_ms);
        return next.Open();
    };

    {
        std::printf("   (the Base never reports again and is taken out of the list)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        API_CHECK(hub.StateList("members").empty() || hub.StateList("members") == "\"members\":[]",
                  "(nothing was looked at yet: no members are written down)");
        hub.management->ProcessSettingsCleanups(); // the worker's tick
        API_CHECK(hub.StateList("members") == "\"members\":[" + members_a + "]",
                  "the worker's tick writes down where every module is: Base A has modules 1 and 2");
        const int writes = hub.writes_tried;
        hub.management->ProcessSettingsCleanups();
        hub.management->ProcessSettingsCleanups();
        API_CHECK(hub.writes_tried == writes, "and writes nothing while nothing changes");

        Hub next(hub);
        API_CHECK(restart(hub, next, 60000) && next.registry.view().nodes.empty() && next.management->IsManaged(kEuiA) &&
                      RecordOf(next, kEuiA) == grant.generation,
                  "after the restart the Registry is empty; Base A is listed from its credential, silent");
        ForgetRecord done;
        API_CHECK(ForgetA(next, grant, 120, &done) && done.affected_module_count == 2 && done.settings_cleanup_pending,
                  "taking it out of the list knows its two modules although the Registry has none of them");
        const std::string noted = next.StateList("cleanups");
        API_CHECK(noted.find(FormatUuidHex(m1)) != std::string::npos && noted.find(FormatUuidHex(m2)) != std::string::npos &&
                      CountOf(noted, kRoomA) == 2 && next.StateList("members") == "\"members\":[]",
                  "the clean-up names them, bound to their rooms; Base A has no members any more");
        API_CHECK(next.management->ProcessSettingsCleanups() == 1 && next.removed_modules.empty() &&
                      RoomOf(*next.settings, m1) == kRoomA,
                  "it rests on what the file said: it waits out the start-up grace, like a clean-up read from the file");
        g_now_ms += 59999;
        API_CHECK(next.management->ProcessSettingsCleanups() == 1 && next.removed_modules.empty(), "(not one millisecond early)");
        g_now_ms += 1;
        API_CHECK(next.management->ProcessSettingsCleanups() == 0 && RoomOf(*next.settings, m1).empty() &&
                      RoomOf(*next.settings, m2).empty() && RestUntouched(*next.settings),
                  "then the rooms of both modules are cleared -- and nothing else");
    }

    {
        std::printf("   (a module had moved to another Base before the restart)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        hub.management->ProcessSettingsCleanups();
        DockOnB(hub, m2);
        hub.management->ProcessSettingsCleanups();
        API_CHECK(hub.StateList("members").find("{\"eui64\":\"" + std::string(kEuiA) + "\",\"modules\":[\"" + FormatUuidHex(m1) +
                                               "\"]}") != std::string::npos &&
                      hub.StateList("members").find("{\"eui64\":\"" + std::string(kEuiB) + "\",\"modules\":[\"" +
                                                    FormatUuidHex(m2) + "\"]}") != std::string::npos,
                  "module 2 was reported by Base B: written down under B, and no longer under A");
        Hub next(hub);
        restart(hub, next, 0);
        ForgetRecord done;
        API_CHECK(ForgetA(next, grant, 121, &done) && done.affected_module_count == 1 &&
                      next.StateList("cleanups").find(FormatUuidHex(m2)) == std::string::npos,
                  "after the restart, taking Base A out of the list is about module 1 only");
        g_now_ms += 1;
        API_CHECK(next.management->ProcessSettingsCleanups() == 0 && RoomOf(*next.settings, m1).empty() &&
                      RoomOf(*next.settings, m2) == kRoomA && RestUntouched(*next.settings),
                  "module 1 is cleared; module 2, on Base B, keeps its room although B has not reported since the restart");
    }

    {
        std::printf("   (a module turns up under another Base after the restart, before the grace is over)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        hub.management->ProcessSettingsCleanups();
        Hub next(hub);
        restart(hub, next, 60000);
        API_CHECK(ForgetA(next, grant, 122) && next.management->ProcessSettingsCleanups() == 1, "(taken out of the list; the clean-up waits)");
        DockOnB(next, m2);
        API_CHECK(Assign(*next.settings, m2, kRoomB), "(module 2 reports under Base B and is given room B)");
        g_now_ms += 60000;
        API_CHECK(next.management->ProcessSettingsCleanups() == 0 && next.removed_modules.size() == 1 &&
                      next.removed_modules[0] == std::vector<std::string>{FormatUuidHex(m1)} &&
                      RoomOf(*next.settings, m1).empty() && RoomOf(*next.settings, m2) == kRoomB,
                  "after the grace the clean-up is for module 1 alone; module 2 keeps the room it has on Base B");
    }

    {
        std::printf("   (a module was detached from the Base before the restart)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        hub.management->ProcessSettingsCleanups();
        API_CHECK(hub.registry.apply_removal(base_a, m2) == ingest::ApplyResult::AppliedNew, "(Base A reports module 2 as detached)");
        hub.management->ProcessSettingsCleanups();
        API_CHECK(hub.StateList("members") == "\"members\":[{\"eui64\":\"" + std::string(kEuiA) + "\",\"modules\":[\"" +
                                                  FormatUuidHex(m1) + "\"]}]",
                  "it is no member of Base A any more");
        Hub next(hub);
        restart(hub, next, 0);
        ForgetRecord done;
        API_CHECK(ForgetA(next, grant, 123, &done) && done.affected_module_count == 1, "(after the restart: one module)");
        g_now_ms += 1;
        API_CHECK(next.management->ProcessSettingsCleanups() == 0 && RoomOf(*next.settings, m1).empty() &&
                      RoomOf(*next.settings, m2) == kRoomA,
                  "the detached module's room is not touched: it did not leave with the Base");
    }

    {
        std::printf("   (the unenrolling of a Base whose modules this process never saw)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        hub.management->ProcessSettingsCleanups();
        Hub next(hub);
        restart(hub, next, 30000);
        RemovalJob job;
        std::array<uint8_t, 16> command{};
        API_CHECK(RequestRemovalNow(*next.management, "dev", kEuiA, job) == RemovalRequestOutcome::Created &&
                      ReleaseDirect(*next.management, kEuiA, grant, command) == ingest::kCoapCodeContent &&
                      next.management->GetRemoval(job.removal_id, job) && job.state == RemovalState::Released &&
                      job.settings_cleanup_pending && CountOf(next.StateList("cleanups"), kRoomA) == 2,
                  "the board polls and releases before any of its modules was reported: the clean-up names the members");
        API_CHECK(next.management->ProcessSettingsCleanups() == 1 && RoomOf(*next.settings, m1) == kRoomA, "(it waits out the grace)");
        g_now_ms += 30000;
        API_CHECK(next.management->ProcessSettingsCleanups() == 0 && RoomOf(*next.settings, m1).empty() &&
                      RoomOf(*next.settings, m2).empty(),
                  "and then clears both rooms");
    }

    {
        std::printf("   (a state file without the list -- written by an earlier hub: nothing is made up)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        hub.management->ProcessSettingsCleanups();
        std::string text = hub.StateFile();
        const std::string list = hub.StateList("members");
        const size_t at = text.find("," + list);
        API_CHECK(at != std::string::npos, "(the list is in the file)");
        text.erase(at, list.size() + 1);
        hub.management.reset();
        test_support::WriteFile(hub.dir + "/" + kPowerBaseMgmtFileName, text);
        Hub next(hub);
        API_CHECK(restart(hub, next, 0) && next.management->IsManaged(kEuiA), "the file without \"members\" is read like any other");
        ForgetRecord done;
        API_CHECK(ForgetA(next, grant, 124, &done) && done.affected_module_count == 0 && !done.settings_cleanup_pending &&
                      next.management->PendingSettingsCleanups() == 0,
                  "taking the silent Base out of the list works; no module is known under it, so no clean-up is noted");
        g_now_ms += 200000;
        API_CHECK(next.management->ProcessSettingsCleanups() == 0 && next.removed_modules.empty() &&
                      RoomOf(*next.settings, m1) == kRoomA && RoomOf(*next.settings, m2) == kRoomA,
                  "and no room is cleared on a guess: the rooms stay as they are");
        API_CHECK(next.StateFile().find("\"members\":[]") != std::string::npos, "(the next write has the list, empty)");
    }

    {
        std::printf("   (the members cannot be written)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        const std::string before = hub.StateFile();
        hub.fail_writes = true;
        int tried = hub.writes_tried;
        hub.management->ProcessSettingsCleanups();
        API_CHECK(hub.writes_tried == tried + 1 && hub.StateFile() == before, "the tick tries once; the file is as it was");
        tried = hub.writes_tried;
        hub.management->ProcessSettingsCleanups();
        hub.management->ProcessSettingsCleanups();
        API_CHECK(hub.writes_tried == tried, "a disk that just refused is not asked again at once");
        g_now_ms += 999;
        hub.management->ProcessSettingsCleanups();
        API_CHECK(hub.writes_tried == tried, "(not one millisecond before the retry time)");
        g_now_ms += 1;
        hub.management->ProcessSettingsCleanups();
        API_CHECK(hub.writes_tried == tried + 1 && hub.StateFile() == before, "then once more -- and it fails again");
        g_now_ms += 1999;
        hub.management->ProcessSettingsCleanups();
        API_CHECK(hub.writes_tried == tried + 1, "the wait doubles, like a clean-up's");
        g_now_ms += 1;
        hub.management->ProcessSettingsCleanups();
        API_CHECK(hub.writes_tried == tried + 2, "(and is tried again when it is over)");
        tried = hub.writes_tried;
        ForgetRecord done;
        NothingHeardAnyMore(hub);
        API_CHECK(hub.management->ForgetPowerBase("dev", kEuiA, grant.generation, RequestId(125), done) == ForgetOutcome::Unavailable &&
                      hub.management->IsManaged(kEuiA) && hub.StateFile() == before && RoomOf(*hub.settings, m1) == kRoomA,
                  "a list removal that cannot be stored is refused and changes nothing");
        hub.steady_shift = std::chrono::seconds(0);
        hub.fail_writes = false;
        g_now_ms += 8000;
        hub.management->ProcessSettingsCleanups();
        API_CHECK(hub.StateList("members") == "\"members\":[" + members_a + "]",
                  "when the disk works again the members are written down");

        // And when they never could be written before the process ended.
        Hub lost;
        ManagementGrant g2;
        RoomWorld(lost, g2);
        lost.fail_writes = true;
        lost.management->ProcessSettingsCleanups();
        Hub next(lost);
        restart(lost, next, 0);
        API_CHECK(ForgetA(next, g2, 126, &done) && done.affected_module_count == 0 && !done.settings_cleanup_pending,
                  "a restart before they were ever written: the Base can be taken out of the list, no module is known");
        g_now_ms += 200000;
        API_CHECK(next.management->ProcessSettingsCleanups() == 0 && RoomOf(*next.settings, m1) == kRoomA,
                  "what was not written down is not cleaned");
    }

    {
        std::printf("   (a Base registered the earlier way -- without a credential -- that is heard again only for a moment)\n");
        Hub hub;
        hub.settings = NewSettings(hub, {{FormatUuidHex(m1), "Desk", kRoomA}, {FormatUuidHex(m2), "Shelf", kRoomA},
                                         {FormatUuidHex(Module(9)), "Door", kRoomB}});
        hub.Open();
        SendHeartbeat(hub.registry, base_a);
        SendTopology(hub.registry, base_a, m1, 1);
        SendTopology(hub.registry, base_a, m2, 1);
        hub.management->ProcessSettingsCleanups();
        API_CHECK(hub.StateList("members") == "\"members\":[" + members_a + "]", "(its modules are written down like any Base's)");
        Hub next(hub);
        restart(hub, next, 60000);
        API_CHECK(!HasBase(next.registry.view(), base_a) && !next.management->IsManaged(kEuiA),
                  "after the restart it is not listed: nothing but its data ever listed it");
        // Its heartbeat arrives -- and nothing else before it falls silent.
        SendHeartbeat(next.registry, base_a);
        const std::string record = RecordOf(next, kEuiA);
        NothingHeardAnyMore(next);
        ForgetRecord done;
        API_CHECK(record.size() == 32 &&
                      next.management->ForgetPowerBase("dev", kEuiA, record, RequestId(128), done) == ForgetOutcome::Removed &&
                      done.affected_module_count == 2 && done.settings_cleanup_pending,
                  "taking it out of the list names the two modules the earlier process had written down under it");
        API_CHECK(next.management->ProcessSettingsCleanups() == 1 && RoomOf(*next.settings, m1) == kRoomA, "(the grace applies)");
        g_now_ms += 60000;
        API_CHECK(next.management->ProcessSettingsCleanups() == 0 && RoomOf(*next.settings, m1).empty() &&
                      RoomOf(*next.settings, m2).empty() && RestUntouched(*next.settings),
                  "and their rooms are cleared");
    }

    {
        std::printf("   (a state that would not fit the file this hub reads back is not written)\n");
        Hub hub;
        // Every module the hub asks about is in room A (no settings store:
        // far more modules than a settings document holds).
        hub.read_rooms = [](const std::vector<std::string> &modules, std::vector<RoomCleanupTarget> &out) {
            out.clear();
            for (const std::string &m : modules) {
                out.push_back(Target(m, kRoomA));
            }
            return true;
        };
        hub.unassign_ok = false; // the settings cannot be written: every clean-up stays owed
        hub.Open();
        NothingHeardAnyMore(hub);
        size_t carried_out = 0;
        ForgetOutcome last = ForgetOutcome::Removed;
        std::string before_refusal;
        for (unsigned b = 0; b < kMaxBlockedPowerBases && last == ForgetOutcome::Removed; b++) {
            const ingest::Uuid base{0x60, 0x55, 0xf9, 0xff, 0xfe, 0x10, 0x00, static_cast<uint8_t>(b + 1)};
            SendHeartbeat(hub.registry, base);
            for (unsigned m = 0; m < kMaxCleanupModules; m++) {
                const ingest::Uuid module{0x74, 0x4d, 0xbd, 0xff, 0xfe, static_cast<uint8_t>(b + 1), 0x00,
                                          static_cast<uint8_t>(m + 1)};
                SendTopology(hub.registry, base, module, 1);
            }
            const std::string eui = FormatUuidHex(base);
            const std::string record = RecordOf(hub, eui);
            before_refusal = hub.StateFile();
            ForgetRecord done;
            last = hub.management->ForgetPowerBase("dev", eui, record, RequestId(200 + b), done);
            if (last == ForgetOutcome::Removed) {
                carried_out++;
            } else {
                API_CHECK(last == ForgetOutcome::Unavailable && !hub.management->IsBlocked(eui) && RecordOf(hub, eui) == record &&
                              hub.registry.view().nodes.size() == kMaxCleanupModules,
                          "the list removal whose state would be too large is refused like a failed write: the Base is "
                          "still listed, with its modules");
            }
        }
        API_CHECK(carried_out > 40 && carried_out < kMaxBlockedPowerBases && last == ForgetOutcome::Unavailable,
                  "(dozens of list removals, each with a full clean-up that stays owed, until one no longer fits)");
        API_CHECK(hub.StateFile() == before_refusal && hub.StateFile().size() <= kMaxPowerBaseMgmtFileBytes,
                  "the file is as it was before that request, and within the size this hub reads");
        hub.management.reset();
        Hub next(hub);
        API_CHECK(next.Open() && next.management->PendingSettingsCleanups() == carried_out,
                  "a restart reads it: every list removal that was carried out, with its clean-up");
    }

    {
        std::printf("   (members of a Base this hub blocks are not kept)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        hub.management->ProcessSettingsCleanups();
        const std::string with_members = hub.StateFile();
        ForgetA(hub, grant, 129);
        // A file that still names members of the Base that left (this hub
        // never writes one): they are dropped when it is read.
        std::string text = hub.StateFile();
        const size_t at = text.find("\"members\":[]");
        API_CHECK(at != std::string::npos && with_members.find(members_a) != std::string::npos, "(after the list removal: no members)");
        text.replace(at, std::strlen("\"members\":[]"), "\"members\":[" + members_a + "]");
        hub.management.reset();
        test_support::WriteFile(hub.dir + "/" + kPowerBaseMgmtFileName, text);
        Hub next(hub);
        restart(hub, next, 0);
        ManagementGrant again;
        API_CHECK(next.management->IsBlocked(kEuiA) && next.management->IssueCredential(kEuiA, again) &&
                      next.StateList("members") == "\"members\":[]",
                  "the file is read, the Base is blocked, and the next write has no members for it");
    }

    {
        std::printf("   (what a snapshot shows is written down first, without waiting for the worker)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        ingest::RegistryView view = hub.registry.view();
        PowerBaseManagement::Description described;
        API_CHECK(hub.management->DescribeForSnapshot(view, described) &&
                      hub.StateList("members") == "\"members\":[" + members_a + "]",
                  "describing the state for a snapshot writes the members of what that snapshot lists");
    }

    {
        std::printf("   (a file whose members or rooms are not what this hub writes)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        DockOnB(hub, Module(3));
        hub.management->ProcessSettingsCleanups();
        ForgetRecord done;
        NothingHeardAnyMore(hub);
        hub.management->ForgetPowerBase("dev", kEuiA, grant.generation, RequestId(127), done);
        const std::string good = hub.StateFile();
        hub.management.reset();
        const std::string member_b = "{\"eui64\":\"" + std::string(kEuiB) + "\",\"modules\":[\"" + FormatUuidHex(Module(3)) + "\"]}";
        API_CHECK(good.find("\"members\":[" + member_b + "]") != std::string::npos && CountOf(good, kRoomA) == 2,
                  "(the file: Base B with module 3 in members, a clean-up with two rooms)");
        auto opens = [&](const std::string &text) {
            test_support::WriteFile(hub.dir + "/" + kPowerBaseMgmtFileName, text);
            Hub probe(hub);
            return probe.Open();
        };
        auto with = [&](const std::string &what, const std::string &replacement) {
            std::string text = good;
            const size_t at = text.find(what);
            if (at != std::string::npos) {
                text.replace(at, what.size(), replacement);
            }
            return text;
        };
        API_CHECK(opens(good), "the file as it was written is read");
        API_CHECK(!opens(with(member_b, member_b + "," + member_b)), "a Base twice in members: damaged");
        API_CHECK(!opens(with(member_b, member_b + ",{\"eui64\":\"6055f9fffe00cccc\",\"modules\":[\"" + FormatUuidHex(Module(3)) + "\"]}")),
                  "a module under two Bases: damaged");
        API_CHECK(!opens(with(member_b, "{\"eui64\":\"" + std::string(kEuiB) + "\",\"modules\":[]}")), "a Base without modules: damaged");
        API_CHECK(!opens(with(member_b, "{\"eui64\":\"" + std::string(kEuiB) + "\",\"modules\":[\"zz\"]}")), "a module that is no EUI-64: damaged");
        API_CHECK(!opens(with("\"members\":[", "\"members\":7,\"x\":[")), "members that are not a list: damaged");
        std::string lower = kRoomA;
        lower[0] = 'a';
        API_CHECK(!opens(with(std::string("\"rooms\":[\"") + kRoomA, "\"rooms\":[\"" + lower + "aaaaaaa")), "a room that is no canonical id: damaged");
        API_CHECK(!opens(with(std::string("\"rooms\":[\"") + kRoomA + "\",", "\"rooms\":[")), "fewer rooms than modules: damaged");
        test_support::WriteFile(hub.dir + "/" + kPowerBaseMgmtFileName, good);
    }
}

void SnapshotDescription(int *failures)
{
    std::printf("\n-- a snapshot never shows a Base next to the record that took it out of the list --\n");
    const ingest::Uuid base_a = Uuid(kEuiA), base_b = Uuid(kEuiB);
    const ingest::Uuid m1 = Module(1);
    PowerBaseManagement::Description described;
    auto has_record = [&](const std::string &eui) {
        return std::any_of(described.records.begin(), described.records.end(),
                           [&](const std::pair<std::string, std::string> &r) { return r.first == eui; });
    };

    {
        std::printf("   (a list removal after the Registry was copied)\n");
        Hub hub;
        hub.Open();
        SendHeartbeat(hub.registry, base_a);
        SendTopology(hub.registry, base_a, m1, 1);
        const std::string record = RecordOf(hub, kEuiA);
        // GET /api/v1/snapshot and the SSE loop copy the Registry first ...
        ingest::RegistryView copy = hub.registry.view();
        API_CHECK(HasBase(copy, base_a) && HasNode(copy, base_a, m1) && copy.generation == hub.registry.generation(),
                  "(the copy lists Base A with its module, and says which generation it was made at)");
        // ... and then another request is carried out before management is read.
        NothingHeardAnyMore(hub);
        ForgetRecord done;
        API_CHECK(hub.management->ForgetPowerBase("dev", kEuiA, record, RequestId(140), done) == ForgetOutcome::Removed,
                  "(another device's request takes Base A out of the list in between)");
        API_CHECK(hub.management->DescribeForSnapshot(copy, described), "management is described for the snapshot");
        API_CHECK(described.forgets.size() == 1 && described.forgets[0].forget_id == done.forget_id,
                  "it lists the list removal");
        API_CHECK(!HasBase(copy, base_a) && !HasNode(copy, base_a, m1) && !has_record(kEuiA),
                  "and the copy it belongs with does not list Base A or its module any more: it was made again");
        API_CHECK(copy.generation == hub.registry.generation(), "(at the Registry's present generation)");
    }

    {
        std::printf("   (a copy made after the last change is used as it is; an older one than the last seen is not)\n");
        Hub hub;
        hub.Open();
        SendHeartbeat(hub.registry, base_a);
        const std::string record = RecordOf(hub, kEuiA);
        NothingHeardAnyMore(hub);
        ForgetRecord done;
        hub.management->ForgetPowerBase("dev", kEuiA, record, RequestId(141), done);
        ingest::RegistryView after = hub.registry.view();
        // The Registry moves on by itself: a Base is heard for the first time.
        SendHeartbeat(hub.registry, base_b);
        ingest::RegistryView copy = after;
        API_CHECK(hub.management->DescribeForSnapshot(copy, described) && !HasBase(copy, base_b) &&
                      copy.generation == after.generation && !has_record(kEuiB),
                  "the copy from after the list removal is kept: what the Registry learned by itself since is the next snapshot's");
        ingest::RegistryView newest = hub.registry.view();
        API_CHECK(hub.management->DescribeForSnapshot(newest, described) && HasBase(newest, base_b) && has_record(kEuiB),
                  "(a newer copy is described: Base B is listed, with a record)");
        copy = after;
        API_CHECK(hub.management->DescribeForSnapshot(copy, described) && HasBase(copy, base_b) && has_record(kEuiB),
                  "a copy older than one that was already described is made again: a snapshot never goes back");
    }

    {
        std::printf("   (a release after the Registry was copied)\n");
        Hub hub;
        hub.Open();
        ManagementGrant grant;
        hub.management->IssueCredential(kEuiA, grant);
        ActivateDirect(*hub.management, kEuiA, grant);
        SendHeartbeat(hub.registry, base_a);
        SendTopology(hub.registry, base_a, m1, 1);
        RemovalJob job;
        RequestRemovalNow(*hub.management, "dev", kEuiA, job);
        ingest::RegistryView copy = hub.registry.view();
        std::array<uint8_t, 16> command{};
        API_CHECK(ReleaseDirect(*hub.management, kEuiA, grant, command) == ingest::kCoapCodeContent,
                  "(the board's 'released' notice arrives after the copy was made)");
        API_CHECK(hub.management->DescribeForSnapshot(copy, described) && described.removals.size() == 1 &&
                      described.removals[0].state == RemovalState::Released && described.managed_power_bases.empty() &&
                      !HasBase(copy, base_a) && !HasNode(copy, base_a, m1) && !has_record(kEuiA),
                  "the job is released and the Base and its module are not listed: one picture, not two halves");
    }

    {
        std::printf("   (the same when the copy is as new as it can be: only the release itself came after it)\n");
        // The command went out earlier; nothing has happened in the Registry
        // since. A copy made now and the copy the release itself looks at are
        // at the same generation -- only the release's own mark tells them apart.
        for (int by_receipt = 0; by_receipt < 2; by_receipt++) {
            Hub hub;
            hub.Open();
            ManagementGrant grant;
            hub.management->IssueCredential(kEuiA, grant);
            ActivateDirect(*hub.management, kEuiA, grant);
            SendHeartbeat(hub.registry, base_a);
            SendTopology(hub.registry, base_a, m1, 1);
            RemovalJob job;
            RequestRemovalNow(*hub.management, "dev", kEuiA, job);
            std::array<uint8_t, 16> command{};
            API_CHECK(FetchCommandDirect(*hub.management, kEuiA, grant, command), "(the command had gone out)");
            ingest::RegistryView copy = hub.registry.view();
            API_CHECK(copy.generation == hub.registry.generation() && HasNode(copy, base_a, m1),
                      "(the copy is at the Registry's present generation and lists the Base's module)");
            if (by_receipt == 0) {
                API_CHECK(hub.management->HandleDatagram(DoneFor(kEuiA, grant, command, PB_MGMT_RESULT_RELEASED)).code ==
                              ingest::kCoapCodeContent,
                          "(then the board's 'released' notice arrives)");
            } else {
                RemovalJob completed;
                API_CHECK(hub.management->SubmitReleaseReceipt(DoneFor(kEuiA, grant, command, PB_MGMT_RESULT_ERASED), completed) ==
                              ReceiptOutcome::Completed,
                          "(then the board's receipt arrives, the notice never did)");
            }
            API_CHECK(hub.management->DescribeForSnapshot(copy, described) && described.removals.size() == 1 &&
                          described.removals[0].state == (by_receipt == 0 ? RemovalState::Released : RemovalState::Completed) &&
                          !HasBase(copy, base_a) && !HasNode(copy, base_a, m1) && !has_record(kEuiA),
                      by_receipt == 0 ? "released, and the copy does not list the Base any more: it was made again"
                                      : "completed, and the copy does not list the Base any more: it was made again");
        }
    }

    {
        std::printf("   (taken out of the list AND registered again after the Registry was copied)\n");
        Hub hub;
        hub.Open();
        ManagementGrant first, second;
        hub.management->IssueCredential(kEuiA, first);
        ActivateDirect(*hub.management, kEuiA, first);
        SendHeartbeat(hub.registry, base_a);
        SendTopology(hub.registry, base_a, m1, 1);
        ingest::RegistryView copy = hub.registry.view();
        NothingHeardAnyMore(hub);
        ForgetRecord done;
        hub.management->ForgetPowerBase("dev", kEuiA, first.generation, RequestId(142), done);
        hub.management->IssueCredential(kEuiA, second);
        API_CHECK(ActivateDirect(*hub.management, kEuiA, second), "(list removal, then a new registration becomes active)");
        API_CHECK(hub.management->DescribeForSnapshot(copy, described) && described.forgets.size() == 1 &&
                      described.forgets[0].record == first.generation && described.managed_power_bases.size() == 1,
                  "the description has the list removal of the first registration and Base A as managed again");
        API_CHECK(described.records.size() == 1 && described.records[0].second == second.generation &&
                      !HasNode(copy, base_a, m1) && !HasBase(copy, base_a),
                  "under the NEW registration's record -- and the module the first registration had is not shown under it");
    }

    {
        std::printf("   (a device that was away: the record that took a Base out stays for as long as the Base stays out)\n");
        Hub hub;
        hub.Open();
        ManagementGrant grant, other, again;
        hub.management->IssueCredential(kEuiA, grant);
        ActivateDirect(*hub.management, kEuiA, grant);
        NothingHeardAnyMore(hub);
        ForgetRecord done;
        API_CHECK(hub.management->ForgetPowerBase("dev", kEuiA, grant.generation, RequestId(150), done) == ForgetOutcome::Removed &&
                      !done.settings_cleanup_pending,
                  "(Base A is taken out of the list; nothing is owed)");
        // Hours later -- far beyond the time a finished job is kept -- and
        // after the file was written again and again.
        g_now_ms += 5 * 3600 * 1000LL;
        // (Another board's registration becoming active is a stored change
        // that drops whatever is old enough to be dropped.)
        hub.management->IssueCredential(kEuiB, other);
        API_CHECK(ActivateDirect(*hub.management, kEuiB, other), "(hours later the state is stored again, and old records are dropped)");
        ingest::RegistryView copy = hub.registry.view();
        API_CHECK(hub.management->DescribeForSnapshot(copy, described) && described.forgets.size() == 1 &&
                      described.forgets[0].forget_id == done.forget_id && described.forgets[0].record == grant.generation &&
                      hub.management->ListForgets().size() == 1 && hub.StateList("forgets").find(done.forget_id) != std::string::npos,
                  "hours later the snapshot still carries the record: a device that was away finds why the Base is not listed");
        ForgetRecord same;
        API_CHECK(hub.management->ForgetPowerBase("dev", kEuiA, grant.generation, RequestId(150), same) == ForgetOutcome::Existing &&
                      same.forget_id == done.forget_id,
                  "and the same request, sent again that late, still gets its record");
        hub.management.reset();
        Hub next(hub);
        API_CHECK(next.Open() && next.management->ListForgets().size() == 1 && next.management->IsBlocked(kEuiA),
                  "a restart keeps it, with the block");
        // The board is registered again: the record is history from then on.
        API_CHECK(next.management->IssueCredential(kEuiA, again) && next.management->ListForgets().size() == 1,
                  "(a registration that was only started changes nothing: the Base is still out of the list)");
        API_CHECK(ActivateDirect(*next.management, kEuiA, again) && !next.management->IsBlocked(kEuiA) &&
                      next.management->ListForgets().empty() && next.StateList("forgets") == "\"forgets\":[]",
                  "when the new registration becomes active the old record is dropped: it is hours old and explains nothing any more");

        // Taken out, registered again and taken out again within minutes:
        // two records of one Base, both recent. Only the last one is kept
        // for the block.
        Hub twice;
        twice.Open();
        ManagementGrant g1, g2, unrelated;
        ForgetRecord first, second;
        twice.management->IssueCredential(kEuiA, g1);
        ActivateDirect(*twice.management, kEuiA, g1);
        NothingHeardAnyMore(twice);
        twice.management->ForgetPowerBase("dev", kEuiA, g1.generation, RequestId(152), first);
        twice.management->IssueCredential(kEuiA, g2);
        ActivateDirect(*twice.management, kEuiA, g2);
        g_now_ms += 60 * 1000;
        API_CHECK(twice.management->ForgetPowerBase("dev", kEuiA, g2.generation, RequestId(153), second) == ForgetOutcome::Removed &&
                      twice.management->ListForgets().size() == 2,
                  "(taken out, registered again and taken out again within a minute: both records are listed while they are recent)");
        g_now_ms += 5 * 3600 * 1000LL;
        twice.management->IssueCredential(kEuiB, unrelated);
        ActivateDirect(*twice.management, kEuiB, unrelated);
        API_CHECK(twice.management->ListForgets().size() == 1 && twice.management->ListForgets()[0].forget_id == second.forget_id &&
                      twice.management->ListForgets()[0].record == g2.generation &&
                      twice.StateList("forgets").find(second.forget_id) != std::string::npos &&
                      twice.StateList("forgets").find(first.forget_id) == std::string::npos,
                  "hours later: exactly the record of the registration that was taken out last -- listed and stored; the earlier one is gone");
    }

    {
        std::printf("   (management unavailable)\n");
        Hub hub;
        test_support::WriteFile(hub.dir + "/" + kPowerBaseMgmtFileName, "not json");
        API_CHECK(!hub.Open(), "(a damaged state file)");
        SendHeartbeat(hub.registry, base_a);
        ingest::RegistryView copy = hub.registry.view();
        const uint64_t generation = copy.generation;
        API_CHECK(!hub.management->DescribeForSnapshot(copy, described) && copy.generation == generation && HasBase(copy, base_a),
                  "nothing is described and the copy is left alone: the snapshot is the one of a hub without management");
    }
}

// ---- the settings write of a clean-up is made in the section that decides ----

// Whether the settings document `contents` still gives `module` a room.
bool ContentsKeepRoom(const std::string &contents, const ingest::Uuid &module)
{
    const size_t at = contents.find("\"" + FormatUuidHex(module) + "\"");
    if (at == std::string::npos) {
        return false;
    }
    const size_t end = contents.find('}', at);
    return contents.substr(at, end == std::string::npos ? std::string::npos : end - at).find("\"room_id\"") != std::string::npos;
}

void RoomCleanupWriteSection(int *failures)
{
    std::printf("\n-- the room clean-up decides and writes in one section: a module that moves in between keeps its room --\n");
    const ingest::Uuid m1 = Module(1), m2 = Module(2);
    auto members_b_has = [&](Hub &hub, const ingest::Uuid &module) {
        const std::string members = hub.StateList("members");
        const size_t at = members.find("{\"eui64\":\"" + std::string(kEuiB) + "\"");
        return at != std::string::npos && members.find(FormatUuidHex(module), at) != std::string::npos;
    };

    {
        std::printf("   (a list removal; after the settings were read, before the write: the module docks elsewhere, nobody saves)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        ForgetA(hub, grant, 300);
        int between = 0;
        hub.before_settings_write = [&]() {
            // The settings were read and both modules were found to be
            // candidates: at this moment they are the clean-up's. Only now
            // is module 1 listed under Base B. No device saves anything, so
            // the write that follows is no revision conflict.
            if (between++ == 0) {
                DockOnB(hub, m1);
            }
        };
        const std::string before = hub.settings->Get().revision;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && between == 1,
                  "the clean-up runs once and is done in one attempt (no conflict was needed)");
        API_CHECK(RoomOf(*hub.settings, m1) == kRoomA,
                  "module 1, listed under Base B after the read and before the write, keeps the room it has");
        API_CHECK(RoomOf(*hub.settings, m2).empty() && RestUntouched(*hub.settings) && hub.settings->Get().revision != before,
                  "module 2, which went with Base A, is cleared by that write; names, rooms and the rest are as they were");
        API_CHECK(hub.StateList("cleanups") == "\"cleanups\":[]" && members_b_has(hub, m1),
                  "nothing is owed any more, and the state file has module 1 under Base B");
        const std::string revision = hub.settings->Get().revision;
        hub.registry.remove_power_base(Uuid(kEuiB));
        g_now_ms += 100000;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && RoomOf(*hub.settings, m1) == kRoomA &&
                      hub.settings->Get().revision == revision,
                  "and when module 1 is on no Base again later, the old clean-up does not come back for it");
    }

    {
        std::printf("   (every module of the clean-up moves in that gap: nothing is written at all)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        ForgetA(hub, grant, 301);
        int between = 0;
        hub.before_settings_write = [&]() {
            if (between++ == 0) {
                DockOnB(hub, m1, 1);
                DockOnB(hub, m2, 1);
            }
        };
        const std::string before = hub.settings->Get().revision;
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && between == 1 && hub.settings->Get().revision == before,
                  "the section finds no module left for the clean-up: the settings are not written");
        API_CHECK(RoomOf(*hub.settings, m1) == kRoomA && RoomOf(*hub.settings, m2) == kRoomA && RestUntouched(*hub.settings) &&
                      hub.StateList("cleanups") == "\"cleanups\":[]",
                  "both modules keep room A, and the clean-up has ended");
    }

    {
        std::printf("   (the same gap for a clean-up that does not know the rooms)\n");
        Hub hub;
        hub.bind_rooms = false;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        ForgetA(hub, grant, 302);
        int between = 0;
        hub.before_settings_write = [&]() {
            if (between++ == 0) {
                DockOnB(hub, m1);
            }
        };
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && between == 1 && RoomOf(*hub.settings, m1) == kRoomA &&
                      RoomOf(*hub.settings, m2).empty() && RestUntouched(*hub.settings),
                  "module 1 keeps its room, module 2 is cleared");
    }

    {
        std::printf("   (the automatic removal: released, then the same gap)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        RemovalJob job;
        std::array<uint8_t, 16> command{};
        API_CHECK(RequestRemovalNow(*hub.management, "dev", kEuiA, job) == RemovalRequestOutcome::Created &&
                      ReleaseDirect(*hub.management, kEuiA, grant, command) == ingest::kCoapCodeContent &&
                      hub.management->GetRemoval(job.removal_id, job) && job.state == RemovalState::Released &&
                      job.settings_cleanup_pending,
                  "(Base A is unenrolled: released, its clean-up owed)");
        int between = 0;
        hub.before_settings_write = [&]() {
            if (between++ == 0) {
                DockOnB(hub, m1);
            }
        };
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && between == 1 && RoomOf(*hub.settings, m1) == kRoomA &&
                      RoomOf(*hub.settings, m2).empty() && RestUntouched(*hub.settings),
                  "module 1, listed under Base B in the gap, keeps its room; module 2 is cleared");
        API_CHECK(hub.management->GetRemoval(job.removal_id, job) && !job.settings_cleanup_pending &&
                      job.state == RemovalState::Released,
                  "the job owes no clean-up any more and is otherwise as it was");
    }

    {
        std::printf("   (docked and detached again in the gap: listed under no Base at the write, and still not the clean-up's)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        ForgetA(hub, grant, 303);
        int between = 0;
        hub.before_settings_write = [&]() {
            if (between++ == 0) {
                DockOnB(hub, m1);
                API_CHECK(hub.registry.apply_removal(Uuid(kEuiB), m1) == ingest::ApplyResult::AppliedNew &&
                              !HasNode(hub.registry.view(), Uuid(kEuiB), m1),
                          "(module 1 was docked onto Base B and Base B confirmed it detached again)");
            }
        };
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && RoomOf(*hub.settings, m1) == kRoomA &&
                      RoomOf(*hub.settings, m2).empty() && RestUntouched(*hub.settings),
                  "it was another registration's in between: it keeps its room although no copy of the Registry showed it there");
    }

    {
        std::printf("   (docked and detached again before the pass, with nobody looking in between)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        ForgetA(hub, grant, 304);
        DockOnB(hub, m1);
        hub.registry.apply_removal(Uuid(kEuiB), m1);
        API_CHECK(hub.registry.view().nodes.empty(), "(the Registry lists no module now)");
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && RoomOf(*hub.settings, m1) == kRoomA &&
                      RoomOf(*hub.settings, m2).empty(),
                  "module 1 keeps its room: the Registry remembers that it was listed after the clean-up was noted");
        // The same Base, registered again with module 2 docked for a moment.
        Hub again;
        ManagementGrant g1, g2;
        RoomWorld(again, g1);
        ForgetA(again, g1, 305);
        again.management->IssueCredential(kEuiA, g2);
        ActivateDirect(*again.management, kEuiA, g2);
        SendHeartbeat(again.registry, Uuid(kEuiA));
        SendTopology(again.registry, Uuid(kEuiA), m2, 7);
        again.registry.apply_removal(Uuid(kEuiA), m2);
        API_CHECK(again.management->ProcessSettingsCleanups() == 0 && RoomOf(*again.settings, m2) == kRoomA &&
                      RoomOf(*again.settings, m1).empty(),
                  "likewise under the Base's new registration: module 2 was listed there, module 1 never came back");
    }

    {
        std::printf("   (a clean-up read from the file; the module was reported by another Base before this manager started)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        ForgetA(hub, grant, 310);
        // The manager is gone before its clean-up ran. Base B reports module 1
        // -- and only then a new manager reads the clean-up from the file. The
        // listing is older than the moment this manager took the clean-up on:
        // what protects the module is that it is listed under a Base NOW.
        hub.management.reset();
        DockOnB(hub, m1);
        API_CHECK(hub.Open() && hub.management->PendingSettingsCleanups() == 1, "(a new manager finds the clean-up owed)");
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && RoomOf(*hub.settings, m1) == kRoomA &&
                      RoomOf(*hub.settings, m2).empty() && RestUntouched(*hub.settings),
                  "module 1 is listed under Base B at this moment and keeps its room; module 2 is cleared");
    }

    {
        std::printf("   (a conflict first, then the move: every attempt goes through the section)\n");
        Hub hub;
        ManagementGrant grant;
        RoomWorld(hub, grant);
        ForgetA(hub, grant, 306);
        int between = 0;
        hub.before_settings_write = [&]() {
            if (between == 0) {
                // An unrelated save: the first write is a revision conflict.
                API_CHECK(Assign(*hub.settings, Module(8), kRoomB), "(a device saves something unrelated)");
            } else if (between == 1) {
                DockOnB(hub, m1); // and now the move, with no save
            }
            between++;
        };
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0 && between == 2,
                  "first attempt: conflict; second attempt: written");
        API_CHECK(RoomOf(*hub.settings, m1) == kRoomA && RoomOf(*hub.settings, m2).empty() &&
                      RoomOf(*hub.settings, Module(8)) == kRoomB && RestUntouched(*hub.settings),
                  "the device's save is kept, module 1 (moved before the second write) keeps its room, module 2 is cleared");
    }

    {
        std::printf("   (while the settings are being written a report waits: it cannot fall between the decision and the write)\n");
        Hub hub;
        ManagementGrant grant;
        // The real store, with a writer that takes its time on request.
        (void)NewSettings(hub, {{FormatUuidHex(m1), "Desk", kRoomA}, {FormatUuidHex(m2), "Shelf", kRoomA},
                                {FormatUuidHex(Module(9)), "Door", kRoomB}});
        std::atomic<bool> slow{false}, in_write{false}, started{false}, applied{false}, applied_during_write{false};
        hub.settings = std::make_shared<SettingsStore>(
            hub.dir + "/settings", [&](const std::string &path, const std::string &contents, std::string &error) {
                if (slow.load()) {
                    in_write = true;
                    for (int i = 0; i < 2000 && !started.load(); i++) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    // The reporter is running: give it every chance to get
                    // through, if it could.
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                    applied_during_write = applied.load();
                }
                return WriteFileAtomically(path, contents, error);
            });
        hub.settings->Load();
        hub.Open();
        hub.management->IssueCredential(kEuiA, grant);
        ActivateDirect(*hub.management, kEuiA, grant);
        SendHeartbeat(hub.registry, Uuid(kEuiA));
        SendTopology(hub.registry, Uuid(kEuiA), m1, 1);
        SendTopology(hub.registry, Uuid(kEuiA), m2, 1);
        ForgetA(hub, grant, 307);
        std::thread reporter([&]() {
            for (int i = 0; i < 10000 && !in_write.load(); i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            started = true;
            DockOnB(hub, m1); // Base B reports module 1 while the settings are being written
            applied = true;
        });
        slow = true;
        const size_t pending = hub.management->ProcessSettingsCleanups();
        slow = false;
        reporter.join();
        API_CHECK(pending == 0 && in_write.load() && started.load(), "(the clean-up wrote the settings while a report was on its way)");
        API_CHECK(!applied_during_write.load() && applied.load(),
                  "the report was not applied while the write ran; it was applied right after");
        API_CHECK(RoomOf(*hub.settings, m1).empty() && RoomOf(*hub.settings, m2).empty() &&
                      HasNode(hub.registry.view(), Uuid(kEuiB), m1),
                  "so the order is one of the two whole ones: the clean-up first (module 1 was on no Base), the listing after it");
    }

    {
        std::printf("   (the Registry's table of listings is bounded: what it no longer knows is not assumed)\n");
        // Three hundred other modules are listed and detached between two
        // looks: the table had to drop listings made after the last look.
        auto churn = [](Hub &hub, unsigned rounds, const std::function<void()> &after_round) {
            const ingest::Uuid base_c{0x60, 0x55, 0xf9, 0xff, 0xfe, 0x00, 0xcc, 0x01};
            for (unsigned r = 0; r < rounds; r++) {
                SendHeartbeat(hub.registry, base_c);
                for (unsigned m = 0; m < 60; m++) {
                    const ingest::Uuid module{0x74, 0x4d, 0xbd, 0xff, 0xfe, static_cast<uint8_t>(0x80 + r), 0x01,
                                              static_cast<uint8_t>(m + 1)};
                    SendTopology(hub.registry, base_c, module, 1);
                }
                hub.registry.remove_power_base(base_c);
                if (after_round) {
                    after_round();
                }
            }
        };
        Hub unseen;
        ManagementGrant g1;
        RoomWorld(unseen, g1);
        ForgetA(unseen, g1, 308);
        churn(unseen, 6, nullptr);
        const ingest::RegistryView full = unseen.registry.view();
        API_CHECK(full.listings.size() == ingest::Registry::kMaxListings && full.listings_dropped_through > 0,
                  "(the table is full and has dropped listings)");
        API_CHECK(unseen.management->ProcessSettingsCleanups() == 0 && RoomOf(*unseen.settings, m1) == kRoomA &&
                      RoomOf(*unseen.settings, m2) == kRoomA && RestUntouched(*unseen.settings),
                  "listings made since the last look were dropped: the modules are not assumed to have stayed away, and keep their rooms");

        // The same churn with a look after every round (a device reading
        // snapshots, the worker's pass): only listings that were looked at
        // are dropped, and nothing is lost.
        Hub seen;
        ManagementGrant g2;
        RoomWorld(seen, g2);
        ForgetA(seen, g2, 309);
        PowerBaseManagement::Description described;
        churn(seen, 6, [&]() {
            ingest::RegistryView view = seen.registry.view();
            seen.management->DescribeForSnapshot(view, described);
        });
        API_CHECK(seen.registry.view().listings_dropped_through > 0 && seen.management->PendingSettingsCleanups() == 1,
                  "(listings were dropped here too; the clean-up is still owed)");
        API_CHECK(seen.management->ProcessSettingsCleanups() == 0 && RoomOf(*seen.settings, m1).empty() &&
                      RoomOf(*seen.settings, m2).empty() && RestUntouched(*seen.settings),
                  "the clean-up clears both rooms: neither module was listed since it was noted, and that is still known");
    }

    {
        std::printf("   (reports, device saves, snapshots and clean-up passes at once: no order of locks stops them)\n");
        int rounds_done = 0;
        int wrong_writes = 0;
        bool deadlock = false;
        for (int round = 0; round < 12 && !deadlock; round++) {
            Hub hub;
            ManagementGrant grant;
            (void)NewSettings(hub, {{FormatUuidHex(m1), "Desk", kRoomA}, {FormatUuidHex(m2), "Shelf", kRoomA},
                                    {FormatUuidHex(Module(9)), "Door", kRoomB}});
            // Set once the report that lists module 1 under Base B has been
            // applied. A settings write that starts after that and takes
            // module 1's room away was decided from a Registry that is gone.
            std::atomic<bool> docked{false}, had_room{true};
            std::atomic<int> wrong{0};
            hub.settings = std::make_shared<SettingsStore>(
                hub.dir + "/settings", [&](const std::string &path, const std::string &contents, std::string &error) {
                    // (The store calls its writer one at a time.)
                    const bool has_room = ContentsKeepRoom(contents, m1);
                    if (docked.load() && had_room.load() && !has_room) {
                        wrong++;
                    }
                    had_room = has_room;
                    return WriteFileAtomically(path, contents, error);
                });
            hub.settings->Load();
            hub.Open();
            hub.management->IssueCredential(kEuiA, grant);
            ActivateDirect(*hub.management, kEuiA, grant);
            SendHeartbeat(hub.registry, Uuid(kEuiA));
            SendTopology(hub.registry, Uuid(kEuiA), m1, 1);
            SendTopology(hub.registry, Uuid(kEuiA), m2, 1);
            ForgetA(hub, grant, 320 + static_cast<unsigned>(round));
            std::atomic<bool> go{false}, stop{false};
            std::atomic<int> finished{0};
            auto wait_go = [&]() {
                while (!go.load()) {
                    std::this_thread::yield();
                }
            };
            std::vector<std::thread> threads;
            threads.emplace_back([&]() { // the clean-up worker
                wait_go();
                for (int i = 0; i < 20; i++) {
                    hub.management->ProcessSettingsCleanups();
                }
                finished++;
            });
            threads.emplace_back([&]() { // the ingest thread
                wait_go();
                if (round % 3 != 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(50 * (round % 5)));
                }
                DockOnB(hub, m1);
                docked = true;
                for (uint32_t seq = 2; seq < 40 && !stop.load(); seq++) {
                    SendTopology(hub.registry, Uuid(kEuiB), Module(static_cast<uint8_t>(20 + seq)), 1);
                    SendHeartbeat(hub.registry, Uuid(kEuiB));
                }
                finished++;
            });
            threads.emplace_back([&]() { // a device saving settings
                wait_go();
                for (int i = 0; i < 20 && !stop.load(); i++) {
                    (void)Assign(*hub.settings, Module(8), i % 2 == 0 ? kRoomB : kRoomA);
                }
                finished++;
            });
            threads.emplace_back([&]() { // devices reading snapshots, and asking for management
                wait_go();
                PowerBaseManagement::Description described;
                for (int i = 0; i < 40 && !stop.load(); i++) {
                    ingest::RegistryView view = hub.registry.view();
                    hub.management->DescribeForSnapshot(view, described);
                    (void)hub.management->ListRemovals();
                    (void)hub.management->PowerBaseRecords({kEuiB});
                }
                finished++;
            });
            go = true;
            for (int waited = 0; waited < 30000 && finished.load() < 4; waited++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (finished.load() < 4) {
                deadlock = true;
                stop = true;
                API_CHECK(false, "the threads did not finish within 30 s: a deadlock (the test stops here)");
                std::fflush(stdout);
                std::_Exit(3);
            }
            for (std::thread &t : threads) {
                t.join();
            }
            wrong_writes += wrong.load();
            // Whatever the order was, it was one of the two whole ones. (When
            // the saving device won four writes in a row the clean-up waits
            // for its retry: let that time pass, with nobody else writing.)
            g_now_ms += 100000;
            hub.management->ProcessSettingsCleanups();
            const bool kept = RoomOf(*hub.settings, m1) == kRoomA;
            const bool cleared = RoomOf(*hub.settings, m1).empty();
            if ((kept || cleared) && RoomOf(*hub.settings, m2).empty() && hub.management->PendingSettingsCleanups() == 0 &&
                NameOf(*hub.settings, m1) == "Desk" && RoomOf(*hub.settings, Module(9)) == kRoomB) {
                rounds_done++;
            } else {
                std::printf("   round %d: module 1 room '%s', module 2 room '%s', clean-ups owed %zu, name '%s', module 9 room '%s'\n",
                            round, RoomOf(*hub.settings, m1).c_str(), RoomOf(*hub.settings, m2).c_str(),
                            hub.management->PendingSettingsCleanups(), NameOf(*hub.settings, m1).c_str(),
                            RoomOf(*hub.settings, Module(9)).c_str());
            }
        }
        API_CHECK(!deadlock && rounds_done == 12,
                  "twelve rounds finish: module 2 is cleared, module 1 kept or cleared as a whole, names and the rest untouched");
        API_CHECK(wrong_writes == 0,
                  "no settings write that began after module 1 was listed under Base B took its room away");
    }
}

// ---- a Base that is out of the list says so for as long as it is ----

void Departures(int *failures)
{
    std::printf("\n-- a Base whose registration ended is named as out of the list until it is registered again --\n");
    const ingest::Uuid base_a = Uuid(kEuiA), base_b = Uuid(kEuiB);
    const ingest::Uuid m1 = Module(1);
    PowerBaseManagement::Description described;
    auto describe = [&](Hub &hub) {
        ingest::RegistryView view = hub.registry.view();
        described = PowerBaseManagement::Description();
        return hub.management->DescribeForSnapshot(view, described) ? view : ingest::RegistryView();
    };
    auto departure_of = [&](const std::string &eui) -> const PowerBaseDeparture * {
        for (const PowerBaseDeparture &d : described.departures) {
            if (d.power_base_uuid == eui) {
                return &d;
            }
        }
        return nullptr;
    };
    auto listed_or_managed = [&](const ingest::RegistryView &view, const std::string &eui) {
        return HasBase(view, Uuid(eui)) ||
               std::find(described.managed_power_bases.begin(), described.managed_power_bases.end(), eui) !=
                   described.managed_power_bases.end() ||
               std::any_of(view.nodes.begin(), view.nodes.end(),
                           [&](const ingest::NodeState &n) { return n.power_base_uuid == Uuid(eui); });
    };
    // Base A, registered and heard, with module 1; Base B likewise with module 2.
    auto two_bases = [&](Hub &hub, ManagementGrant &ga, ManagementGrant &gb) {
        hub.Open();
        hub.management->IssueCredential(kEuiA, ga);
        ActivateDirect(*hub.management, kEuiA, ga);
        hub.management->IssueCredential(kEuiB, gb);
        ActivateDirect(*hub.management, kEuiB, gb);
        SendHeartbeat(hub.registry, base_a);
        SendTopology(hub.registry, base_a, m1, 1);
        SendHeartbeat(hub.registry, base_b);
        SendTopology(hub.registry, base_b, Module(2), 1);
    };
    // The ordinary removal of Base A, to the board's receipt: completed.
    auto remove_a = [&](Hub &hub, const ManagementGrant &grant, RemovalJob &job) {
        std::array<uint8_t, 16> command{};
        return RequestRemovalNow(*hub.management, "dev-a", kEuiA, job) == RemovalRequestOutcome::Created &&
               ReleaseDirect(*hub.management, kEuiA, grant, command) == ingest::kCoapCodeContent &&
               hub.management->HandleDatagram(DoneFor(kEuiA, grant, command, PB_MGMT_RESULT_ERASED)).code ==
                   ingest::kCoapCodeContent &&
               hub.management->GetRemoval(job.removal_id, job) && job.state == RemovalState::Completed;
    };

    {
        std::printf("   (an ordinary removal; the job is shown for a while, the departure for as long as it is true)\n");
        Hub hub;
        ManagementGrant ga, gb;
        two_bases(hub, ga, gb);
        ingest::RegistryView view = describe(hub);
        API_CHECK(described.departures.empty() && hub.management->ListDepartures().empty() && listed_or_managed(view, kEuiA),
                  "while both Bases are registered nothing is named as out of the list");
        RemovalJob job;
        API_CHECK(remove_a(hub, ga, job), "Base A is removed: the board released it and its receipt completed the job");
        view = describe(hub);
        const PowerBaseDeparture *gone = departure_of(kEuiA);
        API_CHECK(described.departures.size() == 1 && gone != nullptr && gone->record == ga.generation &&
                      gone->reason == "removed" && gone->since_unix_ms == g_now_ms,
                  "the snapshot names Base A as out of the list, with the registration that ended and how");
        API_CHECK(!listed_or_managed(view, kEuiA) && listed_or_managed(view, kEuiB) && departure_of(kEuiB) == nullptr &&
                      described.removals.size() == 1 && described.removals[0].state == RemovalState::Completed,
                  "Base A is not listed, Base B is, and the finished job is still shown");
        const std::string blocked = hub.StateList("blocked");
        API_CHECK(blocked.find("\"eui64\":\"" + std::string(kEuiA) + "\"") != std::string::npos &&
                      blocked.find("\"reason\":\"released\"") != std::string::npos &&
                      blocked.find("\"record\":\"" + ga.generation + "\"") != std::string::npos && CountOf(blocked, "\"eui64\"") == 1,
                  "the block in the state file says which registration ended and how (the same write as the block)");

        // Fifteen minutes later (the clock is the test's): the job is no
        // longer shown or stored. That the registration is over is.
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0, "(the room clean-up of the removal is done)");
        g_now_ms += 16 * 60 * 1000;
        ManagementGrant other;
        hub.management->IssueCredential("6055f9fffe00aaaa", other);
        ActivateDirect(*hub.management, "6055f9fffe00aaaa", other); // a stored change that drops what is old
        view = describe(hub);
        gone = departure_of(kEuiA);
        API_CHECK(described.removals.empty() && hub.management->ListRemovals().empty() &&
                      hub.StateFile().find(job.removal_id) == std::string::npos,
                  "sixteen minutes on, the completed job is neither listed nor stored any more");
        API_CHECK(gone != nullptr && gone->record == ga.generation && gone->reason == "removed" && !listed_or_managed(view, kEuiA),
                  "the snapshot still names Base A as out of the list: a device that saw none of it finds why A is missing");
        g_now_ms += 5LL * 60 * 60 * 1000;
        view = describe(hub);
        API_CHECK(departure_of(kEuiA) != nullptr && described.departures.size() == 1, "five hours on: the same");

        // A new process over the same directory, with an empty Registry.
        hub.management.reset();
        Hub restarted(hub);
        API_CHECK(restarted.Open(), "(the hub starts again)");
        view = describe(restarted);
        gone = departure_of(kEuiA);
        API_CHECK(view.nodes.empty() && view.power_bases.empty() && gone != nullptr && gone->record == ga.generation &&
                      gone->reason == "removed" && departure_of(kEuiB) == nullptr,
                  "after a restart the Registry is empty for a while: Base A is named as out of the list, Base B is not");
        API_CHECK(std::find(described.managed_power_bases.begin(), described.managed_power_bases.end(), kEuiB) !=
                          described.managed_power_bases.end() &&
                      std::find(described.managed_power_bases.begin(), described.managed_power_bases.end(), kEuiA) ==
                          described.managed_power_bases.end(),
                  "(Base B is listed from its registration alone; Base A is not)");

        // Registered again: the departure ends with the block.
        ManagementGrant ga2;
        API_CHECK(restarted.management->IssueCredential(kEuiA, ga2) && ga2.generation != ga.generation,
                  "(a new registration of Base A is started)");
        view = describe(restarted);
        API_CHECK(departure_of(kEuiA) != nullptr && restarted.management->IsBlocked(kEuiA),
                  "while that registration is only started the Base is still out of the list, and named so");
        API_CHECK(ActivateDirect(*restarted.management, kEuiA, ga2), "(the board answers with the new registration)");
        view = describe(restarted);
        API_CHECK(described.departures.empty() && restarted.management->ListDepartures().empty() &&
                      listed_or_managed(view, kEuiA) && RecordOf(restarted, kEuiA) == ga2.generation &&
                      restarted.StateList("blocked") == "\"blocked\":[]",
                  "registered again: Base A is listed under the new registration and nothing names it as out of the list");
        // And the hub starts once more right after: an empty Registry, and
        // nothing that could be taken for "Base A was removed".
        restarted.management.reset();
        Hub once_more(restarted);
        API_CHECK(once_more.Open(), "(the hub starts again)");
        view = describe(once_more);
        API_CHECK(view.nodes.empty() && described.departures.empty() && listed_or_managed(view, kEuiA) &&
                      RecordOf(once_more, kEuiA) == ga2.generation,
                  "after a restart that follows the new registration: an empty Registry, Base A listed, no departure");
    }

    {
        std::printf("   (the last Base of the hub; a removal that is only released; a write that fails)\n");
        Hub hub;
        ManagementGrant ga;
        hub.Open();
        hub.management->IssueCredential(kEuiA, ga);
        ActivateDirect(*hub.management, kEuiA, ga);
        SendHeartbeat(hub.registry, base_a);
        SendTopology(hub.registry, base_a, m1, 1);
        RemovalJob job;
        std::array<uint8_t, 16> command{};
        API_CHECK(RequestRemovalNow(*hub.management, "dev", kEuiA, job) == RemovalRequestOutcome::Created &&
                      FetchCommandDirect(*hub.management, kEuiA, ga, command),
                  "(the removal is requested and the command is with the board: sent)");
        ingest::RegistryView view = describe(hub);
        API_CHECK(described.departures.empty() && listed_or_managed(view, kEuiA),
                  "a command that is only sent ends nothing: the Base is listed and not named as out of the list");
        // The notice cannot be stored: nothing is changed, nothing is claimed.
        hub.fail_writes = true;
        const uint8_t refused = hub.management->HandleDatagram(DoneFor(kEuiA, ga, command, PB_MGMT_RESULT_RELEASED)).code;
        view = describe(hub);
        API_CHECK(refused != ingest::kCoapCodeContent && described.departures.empty() && !hub.management->IsBlocked(kEuiA) &&
                      listed_or_managed(view, kEuiA),
                  "the board's notice while the state cannot be written: not accepted, no block, no departure, the Base still listed");
        hub.fail_writes = false;
        API_CHECK(hub.management->HandleDatagram(DoneFor(kEuiA, ga, command, PB_MGMT_RESULT_RELEASED)).code ==
                          ingest::kCoapCodeContent &&
                      hub.management->GetRemoval(job.removal_id, job) && job.state == RemovalState::Released,
                  "(the notice again, stored this time: released, not completed)");
        view = describe(hub);
        const PowerBaseDeparture *gone = departure_of(kEuiA);
        API_CHECK(view.nodes.empty() && view.power_bases.empty() && described.managed_power_bases.empty() && gone != nullptr &&
                      gone->record == ga.generation && gone->reason == "removed",
                  "the hub lists nothing at all now -- and says of its last Base that it is out of the list");
        API_CHECK(described.removals.size() == 1 && described.removals[0].state == RemovalState::Released &&
                      hub.StateFile().find(ga.key) != std::string::npos,
                  "the job stays released (it is not made completed) and its key is kept for the receipt");
        g_now_ms += 60LL * 60 * 1000;
        view = describe(hub);
        API_CHECK(departure_of(kEuiA) != nullptr && described.removals.size() == 1 &&
                      described.removals[0].state == RemovalState::Released && hub.StateFile().find(ga.key) != std::string::npos,
                  "an hour later: still released, key kept, and the departure named");
    }

    {
        std::printf("   (a list removal, the Base registered again, then an ordinary removal)\n");
        Hub hub;
        ManagementGrant g1;
        hub.Open();
        hub.management->IssueCredential(kEuiA, g1);
        ActivateDirect(*hub.management, kEuiA, g1);
        SendHeartbeat(hub.registry, base_a);
        SendTopology(hub.registry, base_a, m1, 1);
        ForgetRecord forgotten;
        API_CHECK(ForgetA(hub, g1, 400, &forgotten), "(Base A is taken out of the list)");
        (void)describe(hub);
        const PowerBaseDeparture *gone = departure_of(kEuiA);
        API_CHECK(gone != nullptr && gone->record == g1.generation && gone->reason == "forgotten" &&
                      hub.StateList("blocked").find("\"reason\":\"forgotten\"") != std::string::npos,
                  "the departure names the registration that was taken out, as forgotten -- not as removed");
        API_CHECK(hub.management->ListForgets().size() == 1 && hub.management->ListForgets()[0].forget_id == forgotten.forget_id,
                  "(its record is listed: it is why the Base is out of the list)");
        // Registered again, two minutes later (the record of the list removal
        // is still in the hub's list and in its file)...
        g_now_ms += 2 * 60 * 1000;
        hub.steady_shift = std::chrono::seconds(0);
        ManagementGrant g2;
        hub.management->IssueCredential(kEuiA, g2);
        API_CHECK(ActivateDirect(*hub.management, kEuiA, g2), "(Base A is registered again)");
        SendHeartbeat(hub.registry, base_a);
        SendTopology(hub.registry, base_a, m1, 5);
        ingest::RegistryView view = describe(hub);
        API_CHECK(described.departures.empty() && listed_or_managed(view, kEuiA) && RecordOf(hub, kEuiA) == g2.generation,
                  "listed under the new registration; nothing names it as out of the list");
        API_CHECK(hub.StateFile().find(forgotten.forget_id) != std::string::npos && hub.management->ListForgets().size() == 1,
                  "(the earlier list-removal record is still there: it is not ten minutes old)");
        // ...and removed the ordinary way.
        RemovalJob job;
        API_CHECK(remove_a(hub, g2, job), "(the new registration is removed and completed)");
        view = describe(hub);
        gone = departure_of(kEuiA);
        API_CHECK(described.departures.size() == 1 && gone != nullptr && gone->record == g2.generation && gone->reason == "removed",
                  "the departure is the registration that ended LAST, as removed");
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0, "(the room clean-ups are done)");
        g_now_ms += 16 * 60 * 1000;
        ManagementGrant other;
        hub.management->IssueCredential("6055f9fffe00aaaa", other);
        ActivateDirect(*hub.management, "6055f9fffe00aaaa", other);
        view = describe(hub);
        gone = departure_of(kEuiA);
        API_CHECK(described.removals.empty() && described.forgets.empty() && hub.management->ListForgets().empty() &&
                      hub.StateFile().find(forgotten.forget_id) == std::string::npos,
                  "sixteen minutes on: neither the job nor the earlier list-removal record is shown or kept -- that record is "
                  "not why the Base is out of the list now");
        API_CHECK(gone != nullptr && gone->record == g2.generation && gone->reason == "removed" && !listed_or_managed(view, kEuiA),
                  "and the departure is still there");
    }

    {
        std::printf("   (a list removal that leaves a sent job; its receipt later)\n");
        Hub hub;
        ManagementGrant ga;
        hub.Open();
        hub.management->IssueCredential(kEuiA, ga);
        ActivateDirect(*hub.management, kEuiA, ga);
        SendHeartbeat(hub.registry, base_a);
        RemovalJob job;
        std::array<uint8_t, 16> command{};
        RequestRemovalNow(*hub.management, "dev", kEuiA, job);
        FetchCommandDirect(*hub.management, kEuiA, ga, command);
        ForgetRecord forgotten;
        API_CHECK(ForgetA(hub, ga, 401, &forgotten) && forgotten.kept_removal_id == job.removal_id,
                  "(the command is with the board; the Base is taken out of the list and the job is left as it is)");
        (void)describe(hub);
        const PowerBaseDeparture *gone = departure_of(kEuiA);
        API_CHECK(gone != nullptr && gone->reason == "forgotten" && gone->record == ga.generation &&
                      described.removals.size() == 1 && described.removals[0].state == RemovalState::Sent &&
                      described.removals[0].power_base_list_removed && hub.StateFile().find(ga.key) != std::string::npos,
                  "out of the list as forgotten; the job is still sent (not completed), marked, and its key is kept");
        // The board did carry the command out, and says so.
        API_CHECK(hub.management->HandleDatagram(DoneFor(kEuiA, ga, command, PB_MGMT_RESULT_ERASED)).code ==
                          ingest::kCoapCodeContent &&
                      hub.management->GetRemoval(job.removal_id, job) && job.state == RemovalState::Completed,
                  "(the board's receipt arrives: the job is completed by proof)");
        (void)describe(hub);
        gone = departure_of(kEuiA);
        API_CHECK(described.departures.size() == 1 && gone != nullptr && gone->reason == "forgotten" &&
                      gone->record == ga.generation && hub.management->IsBlocked(kEuiA),
                  "the Base is out of the list as before, for the reason it left it");
    }

    {
        std::printf("   (a state file an earlier hub wrote: blocks that do not say why)\n");
        Hub hub;
        ManagementGrant ga, gb;
        two_bases(hub, ga, gb);
        RemovalJob job;
        remove_a(hub, ga, job);
        hub.steady_shift = std::chrono::seconds(600);
        // Base B: taken out of the list, registered again, taken out again.
        ForgetRecord earlier, forgotten;
        ManagementGrant gb2;
        API_CHECK(hub.management->ForgetPowerBase("dev", kEuiB, gb.generation, RequestId(402), earlier) == ForgetOutcome::Removed &&
                      hub.management->IssueCredential(kEuiB, gb2) && ActivateDirect(*hub.management, kEuiB, gb2) &&
                      hub.management->ForgetPowerBase("dev", kEuiB, gb2.generation, RequestId(403), forgotten) ==
                          ForgetOutcome::Removed &&
                      earlier.forget_id != forgotten.forget_id,
                  "(Base A removed; Base B taken out of the list, registered again and taken out again)");
        API_CHECK(hub.management->ProcessSettingsCleanups() == 0, "(the room clean-ups are done: no record is kept for one)");
        hub.management.reset();
        // What a hub before 0.4.2 wrote: the same file without the two keys.
        std::string text = hub.StateFile();
        auto strip = [&](const std::string &key) {
            for (size_t at = text.find("\"" + key + "\":\""); at != std::string::npos; at = text.find("\"" + key + "\":\"")) {
                // Only inside the list of blocks.
                const size_t list = text.find("\"blocked\":[");
                const size_t end = text.find(']', list);
                at = text.find("\"" + key + "\":\"", list);
                if (at == std::string::npos || at > end) {
                    break;
                }
                const size_t close = text.find('"', at + key.size() + 4);
                text.erase(at, close - at + 2); // through the comma after it
            }
        };
        strip("reason");
        strip("record");
        const size_t list = text.find("\"blocked\":[");
        const std::string blocks = text.substr(list, text.find(']', list) - list);
        API_CHECK(blocks.find("reason") == std::string::npos && blocks.find("record") == std::string::npos &&
                      CountOf(blocks, "\"since_unix_ms\"") == 2,
                  "(the two blocks as such a hub stored them)");
        test_support::WriteFile(hub.dir + "/" + kPowerBaseMgmtFileName, text);
        Hub old_file(hub);
        API_CHECK(old_file.Open(), "this hub reads that file");
        g_now_ms += 5LL * 60 * 60 * 1000;
        ingest::RegistryView view = describe(old_file);
        const PowerBaseDeparture *a = departure_of(kEuiA);
        const PowerBaseDeparture *b = departure_of(kEuiB);
        API_CHECK(described.departures.size() == 2 && a != nullptr && b != nullptr && a->record.empty() && a->reason.empty() &&
                      b->record.empty() && b->reason.empty() && !listed_or_managed(view, kEuiA) && !listed_or_managed(view, kEuiB),
                  "both Bases are named as out of the list -- that much the block itself says -- without a registration or a reason");
        API_CHECK(DeparturesJson(described.departures).find("\"record\"") == std::string::npos &&
                      DeparturesJson(described.departures).find("\"reason\"") == std::string::npos &&
                      DeparturesJson(described.departures).find("null") == std::string::npos,
                  "what is not known is left out of the snapshot, not made up");
        API_CHECK(old_file.management->ListForgets().size() == 1 && old_file.management->ListForgets()[0].forget_id == forgotten.forget_id,
                  "the list-removal record of such a block is kept as before: the last one stored for that Base, and not the "
                  "earlier one of the same Base");
        // The next stored change keeps them as they are (nothing is invented).
        ManagementGrant other;
        old_file.management->IssueCredential("6055f9fffe00aaaa", other);
        API_CHECK(old_file.StateList("blocked").find("reason") == std::string::npos &&
                      CountOf(old_file.StateList("blocked"), "\"since_unix_ms\"") == 2,
                  "and the file this hub writes next still has the two blocks, still without a reason it does not know");
    }

    {
        std::printf("   (a block that says something this hub cannot have written)\n");
        const char *const cases[][2] = {
            {"\"reason\":\"released\"", "\"reason\":\"erased\""},
            {"\"reason\":\"released\"", "\"reason\":7"},
            {"\"record\":\"", "\"record\":\"XYZ"},
        };
        int refused = 0;
        for (const auto &c : cases) {
            Hub hub;
            ManagementGrant ga, gb;
            two_bases(hub, ga, gb);
            RemovalJob job;
            remove_a(hub, ga, job);
            hub.management.reset();
            std::string text = hub.StateFile();
            const size_t list = text.find("\"blocked\":[");
            const size_t at = text.find(c[0], list);
            if (at == std::string::npos) {
                continue;
            }
            text.replace(at, std::strlen(c[0]), c[1]);
            test_support::WriteFile(hub.dir + "/" + kPowerBaseMgmtFileName, text);
            Hub damaged(hub);
            if (!damaged.Open() && !damaged.management->available()) {
                refused++;
            }
        }
        API_CHECK(refused == 3, "an unknown reason, a reason that is no string, a record that is no record: management does not start");
    }

    {
        std::printf("   (as many departures as blocks, never more)\n");
        Hub hub;
        hub.Open();
        size_t removed = 0;
        for (unsigned i = 0; i < 8; i++) {
            const ingest::Uuid base{0x60, 0x55, 0xf9, 0xff, 0xfe, 0x20, 0x00, static_cast<uint8_t>(i + 1)};
            const std::string eui = FormatUuidHex(base);
            ManagementGrant g;
            hub.management->IssueCredential(eui, g);
            ActivateDirect(*hub.management, eui, g);
            SendHeartbeat(hub.registry, base);
            RemovalJob job;
            std::array<uint8_t, 16> command{};
            if (RequestRemovalNow(*hub.management, "dev", eui, job) == RemovalRequestOutcome::Created &&
                ReleaseDirect(*hub.management, eui, g, command) == ingest::kCoapCodeContent) {
                removed++;
            }
        }
        (void)describe(hub);
        API_CHECK(removed == 8 && described.departures.size() == 8 && CountOf(hub.StateList("blocked"), "\"record\"") == 8 &&
                      hub.management->ListDepartures().size() <= kMaxBlockedPowerBases,
                  "eight Bases removed: eight blocks, eight departures (one per block; the blocks are capped)");
        const std::string json = DeparturesJson(described.departures);
        API_CHECK(json.front() == '[' && json.back() == ']' && CountOf(json, "\"power_base_uuid\"") == 8 &&
                      CountOf(json, "\"reason\":\"removed\"") == 8 && CountOf(json, "\"since_unix_ms\"") == 8,
                  "the snapshot's list has one object per Base with its id, registration, reason and time");
    }
}

// ---- a removal is asked of ONE registration (section 16) ----

// A removal request as one device made it: what it sends, and sends again
// unchanged when it got no answer.
struct AskedRemoval {
    std::string record;
    uint32_t serial = 0;
    std::string request_id;
};

// The device reads the hub's list now and the user confirms the removal of
// what is shown: the registration and its removal serial, and a new id.
AskedRemoval ConfirmRemoval(PowerBaseManagement &management, const std::string &eui)
{
    AskedRemoval asked;
    management.RemovalBinding(eui, asked.record, asked.serial);
    asked.request_id = NextRemovalRequestId();
    return asked;
}

RemovalRequestOutcome SendRemoval(PowerBaseManagement &management, const std::string &device, const std::string &eui,
                                  const AskedRemoval &asked, RemovalJob &out)
{
    return management.RequestRemoval(device, eui, asked.record, asked.serial, asked.request_id, out);
}

// The jobs the hub has for `eui` that are not final, and all of them.
size_t OpenJobsOf(PowerBaseManagement &management, const std::string &eui)
{
    size_t count = 0;
    for (const RemovalJob &job : management.ListRemovals()) {
        count += job.power_base_uuid == eui && !IsFinalRemovalState(job.state) ? 1 : 0;
    }
    return count;
}

void RemovalRequestBinding(int *failures)
{
    std::printf("\n-- a removal request is about one registration: it never reaches another, and makes at most one job --\n");
    const int64_t sixteen_minutes = 16 * 60 * 1000;
    // A stored change of the hub that has nothing to do with Base A: what
    // drops the finished jobs that are older than they are kept for.
    unsigned extra = 0;
    auto unrelated_change = [&](Hub &hub) {
        char eui[17];
        std::snprintf(eui, sizeof(eui), "6055f9fffe0f%04x", ++extra);
        ManagementGrant other;
        return hub.management->IssueCredential(eui, other) && ActivateDirect(*hub.management, eui, other);
    };
    auto register_a = [&](Hub &hub, ManagementGrant &grant) {
        return hub.management->IssueCredential(kEuiA, grant) && ActivateDirect(*hub.management, kEuiA, grant);
    };
    // The removal of Base A, carried out by its board to the receipt.
    auto board_completes = [&](Hub &hub, const ManagementGrant &grant, const std::string &removal_id) {
        std::array<uint8_t, 16> command{};
        RemovalJob job;
        return ReleaseDirect(*hub.management, kEuiA, grant, command) == ingest::kCoapCodeContent &&
               hub.management->HandleDatagram(DoneFor(kEuiA, grant, command, PB_MGMT_RESULT_ERASED)).code ==
                   ingest::kCoapCodeContent &&
               hub.management->GetRemoval(removal_id, job) && job.state == RemovalState::Completed;
    };
    auto has_command = [&](Hub &hub, const ManagementGrant &grant) {
        std::array<uint8_t, 16> command{};
        return FetchCommandDirect(*hub.management, kEuiA, grant, command);
    };
    auto binding = [&](Hub &hub, std::string &record, uint32_t &serial) {
        return hub.management->RemovalBinding(kEuiA, record, serial);
    };

    {
        std::printf("   (the reviewed order: the answer is lost, the removal completes, another device registers the Base "
                    "again, the first device sends its request once more)\n");
        Hub hub;
        hub.Open();
        ManagementGrant old_grant;
        API_CHECK(register_a(hub, old_grant), "(Base A is registered)");
        std::string record;
        uint32_t serial = 9;
        API_CHECK(binding(hub, record, serial) && record == old_grant.generation && serial == 0,
                  "a removal request for Base A names its registration and removal serial 0");
        API_CHECK(hub.StateFile().find("removal_serial") == std::string::npos && hub.StateFile().find("request_id") == std::string::npos,
                  "the state file has neither key before a removal is asked (it is the file an earlier hub writes)");
        const AskedRemoval phone_a = ConfirmRemoval(*hub.management, kEuiA);
        RemovalJob first;
        API_CHECK(SendRemoval(*hub.management, "phone-A", kEuiA, phone_a, first) == RemovalRequestOutcome::Created &&
                      first.generation == old_grant.generation && first.request_id == phone_a.request_id &&
                      first.state == RemovalState::Pending,
                  "phone A's request creates the job, for the registration it names (the answer is lost)");
        API_CHECK(binding(hub, record, serial) && record == old_grant.generation && serial == 1,
                  "the registration's removal serial moved on with the job");
        const std::string file = hub.StateFile();
        API_CHECK(file.find("\"removal_serial\":1") != std::string::npos &&
                      file.find("\"request_id\":\"" + phone_a.request_id + "\"") != std::string::npos,
                  "the serial and the request id are in the state file, in the same write as the job");
        API_CHECK(board_completes(hub, old_grant, first.removal_id), "the board carries it out: released, then its receipt -- completed");

        ManagementGrant next_grant;
        API_CHECK(register_a(hub, next_grant) && next_grant.generation != old_grant.generation && hub.management->IsManaged(kEuiA),
                  "phone B registers the same Base again: another registration");
        API_CHECK(binding(hub, record, serial) && record == next_grant.generation && serial == 0,
                  "the new registration has its own removal serial, 0");

        RemovalJob repeated;
        RemovalRequestOutcome outcome = SendRemoval(*hub.management, "phone-A", kEuiA, phone_a, repeated);
        API_CHECK(outcome == RemovalRequestOutcome::Existing && repeated.removal_id == first.removal_id &&
                      repeated.state == RemovalState::Completed && repeated.generation == old_grant.generation,
                  "phone A sends the same request again: it is answered with the job it created -- the completed "
                  "removal of the earlier registration");
        API_CHECK(hub.management->ListRemovals().size() == 1 && OpenJobsOf(*hub.management, kEuiA) == 0,
                  "no job was created for the new registration");
        API_CHECK(!has_command(hub, next_grant), "the new registration's board polls: no UNENROLL, and none pending");
        API_CHECK(hub.management->IsManaged(kEuiA) && !hub.management->IsBlocked(kEuiA) && binding(hub, record, serial) &&
                      record == next_grant.generation && serial == 0,
                  "the new registration is as it was: listed, managed, serial 0");

        // The finished job leaves the hub's list and its file.
        g_now_ms += sixteen_minutes;
        API_CHECK(unrelated_change(hub) && hub.management->ListRemovals().empty() &&
                      hub.StateFile().find(phone_a.request_id) == std::string::npos,
                  "sixteen minutes on, the finished job is neither listed nor stored: nothing of that request is "
                  "remembered here");
        outcome = SendRemoval(*hub.management, "phone-A", kEuiA, phone_a, repeated);
        API_CHECK(outcome == RemovalRequestOutcome::StaleRecord && repeated.removal_id.empty(),
                  "the same request once more: refused as not about what is listed (stale_record) -- being forgotten "
                  "did not make it a request for the new registration");
        API_CHECK(hub.management->ListRemovals().empty() && !has_command(hub, next_grant),
                  "nothing was created and the new registration's board is given no command");

        // The next process over the same file.
        hub.management.reset();
        Hub restarted(hub);
        API_CHECK(restarted.Open(), "(the hub starts again)");
        outcome = SendRemoval(*restarted.management, "phone-A", kEuiA, phone_a, repeated);
        API_CHECK(outcome == RemovalRequestOutcome::StaleRecord && restarted.management->ListRemovals().empty() &&
                      !has_command(restarted, next_grant),
                  "after a restart: the same answer, no job, no command");

        // What stays possible: the user looks at the list now and confirms.
        const AskedRemoval fresh = ConfirmRemoval(*restarted.management, kEuiA);
        RemovalJob wanted;
        API_CHECK(fresh.record == next_grant.generation && fresh.serial == 0 &&
                      SendRemoval(*restarted.management, "phone-A", kEuiA, fresh, wanted) == RemovalRequestOutcome::Created &&
                      wanted.generation == next_grant.generation && has_command(restarted, next_grant),
                  "a NEW confirmation of what is listed now is carried out: a job for the new registration, and its command");
    }

    {
        std::printf("   (a request that arrives for the first time after the Base was removed and registered again; and "
                    "one that was confirmed before and is sent after)\n");
        Hub hub;
        hub.Open();
        ManagementGrant old_grant;
        register_a(hub, old_grant);
        // Phone A confirms; its request is on its way. A dialog is open on a
        // tablet that shows the same registration.
        const AskedRemoval delayed = ConfirmRemoval(*hub.management, kEuiA);
        std::string dialog_record;
        uint32_t dialog_serial = 0;
        binding(hub, dialog_record, dialog_serial);
        // Another device removes the Base...
        const AskedRemoval phone_b = ConfirmRemoval(*hub.management, kEuiA);
        RemovalJob by_b;
        API_CHECK(SendRemoval(*hub.management, "phone-B", kEuiA, phone_b, by_b) == RemovalRequestOutcome::Created &&
                      board_completes(hub, old_grant, by_b.removal_id),
                  "(another device removes Base A; completed)");
        RemovalJob late;
        API_CHECK(SendRemoval(*hub.management, "phone-A", kEuiA, delayed, late) == RemovalRequestOutcome::UnknownPowerBase,
                  "while the Base is out of the list a request for its earlier registration finds no such Base");
        // ...and a registration of it is started: not active yet.
        ManagementGrant next_grant;
        API_CHECK(hub.management->IssueCredential(kEuiA, next_grant), "(a new registration is started)");
        std::string started_record = "x";
        uint32_t started_serial = 7;
        API_CHECK(!hub.management->RemovalBinding(kEuiA, started_record, started_serial) && started_record.empty() &&
                      started_serial == 0,
                  "a registration that is only started is nothing a removal could name: no binding is given for it");
        API_CHECK(SendRemoval(*hub.management, "phone-A", kEuiA, delayed, late) != RemovalRequestOutcome::Created &&
                      hub.management->ListRemovals().size() == 1,
                  "while the new registration is only started: still nothing is created");
        API_CHECK(ActivateDirect(*hub.management, kEuiA, next_grant), "(its board polls: the new registration is active)");

        API_CHECK(SendRemoval(*hub.management, "phone-A", kEuiA, delayed, late) == RemovalRequestOutcome::StaleRecord &&
                      late.removal_id.empty(),
                  "phone A's request arrives now, for the first time: stale_record");
        AskedRemoval from_dialog;
        from_dialog.record = dialog_record;
        from_dialog.serial = dialog_serial;
        from_dialog.request_id = NextRemovalRequestId();
        API_CHECK(SendRemoval(*hub.management, "tablet", kEuiA, from_dialog, late) == RemovalRequestOutcome::StaleRecord,
                  "the dialog that was open all the while is confirmed now: it asks for the registration it showed -- "
                  "stale_record");
        // The same two with the serial the new registration has (0, as the
        // old one had when they were made): the registration decides.
        API_CHECK(delayed.serial == 0 && dialog_serial == 0, "(both carry serial 0, which is also the new registration's)");
        API_CHECK(hub.management->ListRemovals().size() == 1 && OpenJobsOf(*hub.management, kEuiA) == 0 &&
                      !has_command(hub, next_grant),
                  "the only job is the completed one of the earlier registration; the new registration's board is given "
                  "no command");
        std::string record;
        uint32_t serial = 9;
        API_CHECK(binding(hub, record, serial) && record == next_grant.generation && serial == 0,
                  "and its removal serial is untouched");
    }

    {
        std::printf("   (a request that was cancelled is not carried out when it comes again; a new confirmation is)\n");
        Hub hub;
        hub.Open();
        ManagementGrant grant;
        register_a(hub, grant);
        const AskedRemoval asked = ConfirmRemoval(*hub.management, kEuiA);
        RemovalJob job, cancelled, again;
        API_CHECK(SendRemoval(*hub.management, "phone-A", kEuiA, asked, job) == RemovalRequestOutcome::Created,
                  "a removal is asked (the board is away: the job waits)");
        API_CHECK(hub.management->CancelRemoval(job.removal_id, cancelled) == RemovalCancelOutcome::Cancelled,
                  "the user cancels it");
        API_CHECK(SendRemoval(*hub.management, "phone-A", kEuiA, asked, again) == RemovalRequestOutcome::Existing &&
                      again.removal_id == job.removal_id && again.state == RemovalState::Cancelled,
                  "the cancelled request comes again (a late copy of it, or a retry): the same job, cancelled as it is");
        API_CHECK(hub.management->ListRemovals().size() == 1 && OpenJobsOf(*hub.management, kEuiA) == 0 && !has_command(hub, grant),
                  "no second job, and the board is given no command");
        std::string record;
        uint32_t serial = 0;
        API_CHECK(binding(hub, record, serial) && record == grant.generation && serial == 1,
                  "the registration is the same one; its removal serial is 1 (one removal was asked of it)");

        // A restart while the cancelled job is still kept: the request is still known by its id.
        hub.management.reset();
        Hub second(hub);
        API_CHECK(second.Open() && SendRemoval(*second.management, "phone-A", kEuiA, asked, again) == RemovalRequestOutcome::Existing &&
                      again.removal_id == job.removal_id && again.state == RemovalState::Cancelled &&
                      again.request_id == asked.request_id && second.management->ListRemovals().size() == 1,
                  "after a restart the cancelled request is answered with its cancelled job (the id was read from the file)");
        second.management.reset();
        hub.Open();

        g_now_ms += sixteen_minutes;
        API_CHECK(unrelated_change(hub) && hub.management->ListRemovals().empty() &&
                      hub.StateFile().find(asked.request_id) == std::string::npos,
                  "sixteen minutes on, the cancelled job is neither listed nor stored");
        API_CHECK(SendRemoval(*hub.management, "phone-A", kEuiA, asked, again) == RemovalRequestOutcome::StaleRecord &&
                      hub.management->ListRemovals().empty() && !has_command(hub, grant),
                  "the cancelled request comes again now: stale_record -- the serial it carries was spent by its own job, "
                  "and that is kept with the registration, not with the job");
        hub.management.reset();
        Hub restarted(hub);
        API_CHECK(restarted.Open() && SendRemoval(*restarted.management, "phone-A", kEuiA, asked, again) ==
                                          RemovalRequestOutcome::StaleRecord &&
                      restarted.management->ListRemovals().empty(),
                  "after a restart: the same");

        const AskedRemoval fresh = ConfirmRemoval(*restarted.management, kEuiA);
        RemovalJob wanted;
        API_CHECK(fresh.record == grant.generation && fresh.serial == 1 && fresh.request_id != asked.request_id,
                  "the user confirms the removal anew: the same registration, the present serial, another request id");
        API_CHECK(SendRemoval(*restarted.management, "phone-A", kEuiA, fresh, wanted) == RemovalRequestOutcome::Created &&
                      wanted.removal_id != job.removal_id && has_command(restarted, grant),
                  "that one is carried out: a new job, and its command for the board");
    }

    {
        std::printf("   (two devices ask at the same moment; each request is delivered twice)\n");
        Hub hub;
        hub.Open();
        ManagementGrant grant;
        register_a(hub, grant);
        const AskedRemoval phone_a = ConfirmRemoval(*hub.management, kEuiA);
        const AskedRemoval phone_b = ConfirmRemoval(*hub.management, kEuiA);
        API_CHECK(phone_a.record == phone_b.record && phone_a.serial == phone_b.serial && phone_a.request_id != phone_b.request_id,
                  "(both saw the same registration and serial; each has its own request id)");
        RemovalJob ja, jb, ja2, jb2;
        API_CHECK(SendRemoval(*hub.management, "phone-A", kEuiA, phone_a, ja) == RemovalRequestOutcome::Created,
                  "the first request creates the job");
        API_CHECK(SendRemoval(*hub.management, "phone-B", kEuiA, phone_b, jb) == RemovalRequestOutcome::Existing &&
                      jb.removal_id == ja.removal_id,
                  "the second is about the same registration, whose removal is under way: that job");
        API_CHECK(SendRemoval(*hub.management, "phone-A", kEuiA, phone_a, ja2) == RemovalRequestOutcome::Existing &&
                      ja2.removal_id == ja.removal_id &&
                      SendRemoval(*hub.management, "phone-B", kEuiA, phone_b, jb2) == RemovalRequestOutcome::Existing &&
                      jb2.removal_id == ja.removal_id,
                  "each of them again: the same job");
        std::string record;
        uint32_t serial = 0;
        API_CHECK(hub.management->ListRemovals().size() == 1 && binding(hub, record, serial) && serial == 1,
                  "one job, and one removal counted for the registration");
        std::array<uint8_t, 16> command{}, repeated{};
        API_CHECK(FetchCommandDirect(*hub.management, kEuiA, grant, command) &&
                      FetchCommandDirect(*hub.management, kEuiA, grant, repeated) && repeated == command,
                  "the board is given one command (asked again, it is the same command)");
        API_CHECK(SendRemoval(*hub.management, "phone-B", kEuiA, phone_b, jb2) == RemovalRequestOutcome::Existing &&
                      jb2.state == RemovalState::Sent,
                  "a request after the command went out is still that job");

        // The same race for real: two threads, many Bases.
        const int rounds = 24;
        std::vector<std::string> euis;
        std::vector<ManagementGrant> grants(rounds);
        bool prepared = true;
        for (int i = 0; i < rounds; i++) {
            char eui[17];
            std::snprintf(eui, sizeof(eui), "6055f9fffe0e%04x", i + 1);
            euis.push_back(eui);
            prepared = prepared && hub.management->IssueCredential(eui, grants[i]) && ActivateDirect(*hub.management, eui, grants[i]);
        }
        int one_created = 0, same_job = 0;
        for (int i = 0; i < rounds && prepared; i++) {
            const AskedRemoval first = ConfirmRemoval(*hub.management, euis[i]);
            const AskedRemoval second = ConfirmRemoval(*hub.management, euis[i]);
            std::atomic<bool> go{false};
            RemovalJob j1, j2;
            RemovalRequestOutcome o1 = RemovalRequestOutcome::Unavailable, o2 = RemovalRequestOutcome::Unavailable;
            std::thread t1([&]() {
                while (!go.load()) {
                }
                o1 = SendRemoval(*hub.management, "phone-A", euis[i], first, j1);
            });
            std::thread t2([&]() {
                while (!go.load()) {
                }
                o2 = SendRemoval(*hub.management, "phone-B", euis[i], second, j2);
            });
            go.store(true);
            t1.join();
            t2.join();
            const bool created_once = (o1 == RemovalRequestOutcome::Created) != (o2 == RemovalRequestOutcome::Created);
            const bool other_existing = o1 == RemovalRequestOutcome::Existing || o2 == RemovalRequestOutcome::Existing;
            one_created += created_once && other_existing ? 1 : 0;
            same_job += j1.removal_id == j2.removal_id && !j1.removal_id.empty() && OpenJobsOf(*hub.management, euis[i]) == 1 ? 1 : 0;
        }
        API_CHECK(prepared && one_created == rounds && same_job == rounds,
                  "two threads asking at once, 24 times: every time one request creates the job and the other gets that job");
    }

    {
        std::printf("   (the state file cannot be written)\n");
        Hub hub;
        hub.Open();
        ManagementGrant old_grant;
        register_a(hub, old_grant);
        const AskedRemoval asked = ConfirmRemoval(*hub.management, kEuiA);
        RemovalJob job;
        hub.fail_writes = true;
        API_CHECK(SendRemoval(*hub.management, "phone-A", kEuiA, asked, job) == RemovalRequestOutcome::Unavailable &&
                      job.removal_id.empty() && hub.management->ListRemovals().empty() && !has_command(hub, old_grant),
                  "a request that cannot be stored: unavailable, no job, and no command for the board");
        std::string record;
        uint32_t serial = 9;
        API_CHECK(binding(hub, record, serial) && serial == 0, "the removal serial did not move (the job and the serial are one write)");
        hub.fail_writes = false;
        API_CHECK(SendRemoval(*hub.management, "phone-A", kEuiA, asked, job) == RemovalRequestOutcome::Created,
                  "the same request once writing works again: carried out (it had created nothing)");
        API_CHECK(board_completes(hub, old_grant, job.removal_id), "(the removal completes)");
        ManagementGrant next_grant;
        API_CHECK(register_a(hub, next_grant), "(the Base is registered again)");
        g_now_ms += sixteen_minutes;
        API_CHECK(unrelated_change(hub) && hub.management->ListRemovals().empty(), "(the finished job has left the hub's list)");

        hub.fail_writes = true;
        RemovalJob none;
        API_CHECK(SendRemoval(*hub.management, "phone-A", kEuiA, asked, none) == RemovalRequestOutcome::StaleRecord &&
                      hub.management->ListRemovals().empty() && !has_command(hub, next_grant),
                  "while writing fails, the earlier request again: stale_record, nothing for the new registration");
        const AskedRemoval fresh = ConfirmRemoval(*hub.management, kEuiA);
        API_CHECK(SendRemoval(*hub.management, "phone-B", kEuiA, fresh, none) == RemovalRequestOutcome::Unavailable &&
                      hub.management->ListRemovals().empty() && !has_command(hub, next_grant),
                  "while writing fails, a new confirmation of the new registration: unavailable -- no job is kept in "
                  "memory only, and the board is given no command");
        API_CHECK(binding(hub, record, serial) && record == next_grant.generation && serial == 0,
                  "the new registration's serial did not move");
        hub.fail_writes = false;
        API_CHECK(SendRemoval(*hub.management, "phone-B", kEuiA, fresh, none) == RemovalRequestOutcome::Created &&
                      has_command(hub, next_grant),
                  "once writing works: that confirmation is carried out");
    }

    {
        std::printf("   (a request id belongs to one Base; a released removal; a Base this hub cannot remove)\n");
        Hub hub;
        hub.Open();
        ManagementGrant ga, gb;
        register_a(hub, ga);
        hub.management->IssueCredential(kEuiB, gb);
        ActivateDirect(*hub.management, kEuiB, gb);
        const AskedRemoval for_a = ConfirmRemoval(*hub.management, kEuiA);
        RemovalJob job, other;
        API_CHECK(SendRemoval(*hub.management, "phone-A", kEuiA, for_a, job) == RemovalRequestOutcome::Created, "(a removal of Base A)");
        AskedRemoval for_b = ConfirmRemoval(*hub.management, kEuiB);
        for_b.request_id = for_a.request_id;
        API_CHECK(SendRemoval(*hub.management, "phone-A", kEuiB, for_b, other) == RemovalRequestOutcome::RequestConflict &&
                      OpenJobsOf(*hub.management, kEuiB) == 0,
                  "the same request id for another Base: request_id_conflict, and nothing is created for that Base");
        API_CHECK(hub.management->RequestRemoval("phone-A", kEuiB, for_b.record, for_b.serial, "not-an-id", other) ==
                          RemovalRequestOutcome::RequestConflict &&
                      OpenJobsOf(*hub.management, kEuiB) == 0,
                  "a request id that is none creates nothing");

        std::array<uint8_t, 16> command{};
        API_CHECK(ReleaseDirect(*hub.management, kEuiA, ga, command) == ingest::kCoapCodeContent && hub.management->IsBlocked(kEuiA),
                  "(Base A's board releases the registration: out of the list, the job is released)");
        std::string record;
        uint32_t serial = 0;
        API_CHECK(!binding(hub, record, serial) && record.empty(), "a Base that is out of the list has nothing a request could name");
        AskedRemoval still_on_screen;
        still_on_screen.record = ga.generation;
        still_on_screen.serial = 0;
        still_on_screen.request_id = NextRemovalRequestId();
        API_CHECK(SendRemoval(*hub.management, "tablet", kEuiA, still_on_screen, other) == RemovalRequestOutcome::Existing &&
                      other.removal_id == job.removal_id && other.state == RemovalState::Released,
                  "a device that still shows that registration asks: its removal is the released job");
        still_on_screen.record = std::string(32, 'c');
        still_on_screen.request_id = NextRemovalRequestId();
        API_CHECK(SendRemoval(*hub.management, "tablet", kEuiA, still_on_screen, other) == RemovalRequestOutcome::UnknownPowerBase &&
                      hub.management->ListRemovals().size() == 1,
                  "a request that names another registration of that Base: not listed, nothing created");

        // A Base this hub only hears: no credential, nothing to name.
        const char kHeardOnly[] = "6055f9fffe009abc";
        SendHeartbeat(hub.registry, Uuid(kHeardOnly));
        AskedRemoval unmanaged = ConfirmRemoval(*hub.management, kHeardOnly);
        API_CHECK(unmanaged.record.empty(), "a Base without a credential has no removal binding");
        unmanaged.record = std::string(32, 'd');
        API_CHECK(SendRemoval(*hub.management, "phone-A", kHeardOnly, unmanaged, other) == RemovalRequestOutcome::Unsupported,
                  "asking all the same: unsupported, as before");
    }

    {
        std::printf("   (what a snapshot says, and the state file across hubs)\n");
        Hub hub;
        hub.Open();
        ManagementGrant grant;
        register_a(hub, grant);
        PowerBaseManagement::Description described;
        auto serial_in_snapshot = [&](Hub &h, const std::string &eui) -> int64_t {
            ingest::RegistryView view = h.registry.view();
            described = PowerBaseManagement::Description();
            if (!h.management->DescribeForSnapshot(view, described)) {
                return -2;
            }
            for (const auto &entry : described.removal_serials) {
                if (entry.first == eui) {
                    return entry.second;
                }
            }
            return -1;
        };
        API_CHECK(serial_in_snapshot(hub, kEuiA) == 0, "the snapshot gives a managed Base its removal serial: 0");
        const AskedRemoval asked = ConfirmRemoval(*hub.management, kEuiA);
        RemovalJob job;
        SendRemoval(*hub.management, "phone-A", kEuiA, asked, job);
        API_CHECK(serial_in_snapshot(hub, kEuiA) == 1 && described.removals.size() == 1 &&
                      described.removals[0].request_id == asked.request_id &&
                      RemovalJobJson(described.removals[0]).find("\"request_id\":\"" + asked.request_id + "\"}") != std::string::npos,
                  "after the request: serial 1, and the job says which request created it (last in its JSON)");

        // The file as a hub before this one reads and rewrites it: without
        // the two keys.
        const std::string written = hub.StateFile();
        std::string older = written;
        const std::string serial_key = ",\"removal_serial\":1";
        const std::string id_key = ",\"request_id\":\"" + asked.request_id + "\"";
        API_CHECK(older.find(serial_key) != std::string::npos && older.find(id_key) != std::string::npos,
                  "(the file has the two optional keys)");
        older.erase(older.find(serial_key), serial_key.size());
        older.erase(older.find(id_key), id_key.size());
        hub.management.reset();
        std::string error;
        Hub from_older(hub);
        API_CHECK(WriteFileAtomically(from_older.dir + "/" + kPowerBaseMgmtFileName, older, error) && from_older.Open(),
                  "a file without them (as an earlier hub writes it) is read");
        std::string record;
        uint32_t serial = 9;
        RemovalJob read_back, again;
        API_CHECK(from_older.management->RemovalBinding(kEuiA, record, serial) && serial == 0 &&
                      from_older.management->GetRemoval(job.removal_id, read_back) && read_back.request_id.empty() &&
                      RemovalJobJson(read_back).find("request_id") == std::string::npos,
                  "the serial reads 0 and the job has no request id: nothing is made up");
        API_CHECK(SendRemoval(*from_older.management, "phone-A", kEuiA, asked, again) == RemovalRequestOutcome::Existing &&
                      again.removal_id == job.removal_id && from_older.management->ListRemovals().size() == 1,
                  "the request that made that job is still answered with it: the registration it names is being removed");
        from_older.management.reset();

        struct Damage {
            const char *what;
            std::string from, to;
        };
        const Damage damages[] = {
            {"a removal_serial that is negative", serial_key, ",\"removal_serial\":-1"},
            {"a removal_serial that is a string", serial_key, ",\"removal_serial\":\"1\""},
            {"a removal_serial beyond 32 bits", serial_key, ",\"removal_serial\":4294967296"},
            {"a request_id that is no id", id_key, ",\"request_id\":\"nope\""},
            {"a request_id that is a number", id_key, ",\"request_id\":7"},
        };
        for (const Damage &d : damages) {
            std::string bad = written;
            bad.replace(bad.find(d.from), d.from.size(), d.to);
            Hub damaged;
            const bool wrote = WriteFileAtomically(damaged.dir + "/" + kPowerBaseMgmtFileName, bad, error);
            API_CHECK(wrote && !damaged.Open() && !damaged.management->available(), d.what);
        }
    }
}

void RunPowerBaseManagementTests(int *failures)
{
    GoldenVectors(failures);
    OnlineRemoval(failures);
    EraseFailureIsNotCompletion(failures);
    OfflineAndRestart(failures);
    LostProof(failures);
    ReceiptWithoutNotice(failures);
    ThreadReceipt(failures);
    ReenrollAfterRemoval(failures);
    BlockCapacity(failures);
    SettingsCleanupDurability(failures);
    RejectedDatagrams(failures);
    LimitsAndStorage(failures);
    NothingSecretLogged(failures);
    SettingsCleanup(failures);
    SnapshotFields(failures);
    EndpointLocate(failures);
    ListRemoval(failures);
    ListRemovalOfUnmanagedBase(failures);
    ListRemovalAndRemovalJobs(failures);
    ListRemovalLimitsAndStorage(failures);
    ListRemovalOnAnotherHub(failures);
    RoomCleanupBinding(failures);
    MembersAcrossRestart(failures);
    SnapshotDescription(failures);
    RoomCleanupWriteSection(failures);
    Departures(failures);
    RemovalRequestBinding(failures);
}
