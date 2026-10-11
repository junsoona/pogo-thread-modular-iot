#include "pogohome_api/power_base_management.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "pogohome_api/atomic_file.hpp"
#include "pogohome_api/crypto_util.hpp"
#include "pogohome_api/hex.hpp"
#include "pogohome_api/home_settings.hpp"
#include "pogohome_api/json_snapshot.hpp"
#include "pogohome_api/json_value.hpp"

namespace pogohome_api {

namespace {

// HMAC-SHA-256 from OpenSSL, in the shape pb_mgmt_proto.h asks for.
bool OpenSslHmac(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t out[PB_MGMT_MAC_LEN],
                 void * /*ctx*/)
{
    unsigned int mac_len = 0;
    unsigned char mac[EVP_MAX_MD_SIZE];
    if (HMAC(EVP_sha256(), key, static_cast<int>(key_len), data, data_len, mac, &mac_len) == nullptr ||
        mac_len != PB_MGMT_MAC_LEN) {
        return false;
    }
    std::memcpy(out, mac, PB_MGMT_MAC_LEN);
    OPENSSL_cleanse(mac, sizeof(mac));
    return true;
}

int HexNibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

// Exactly 2 * N lowercase hex digits.
template <size_t N> bool ParseHex(const std::string &hex, std::array<uint8_t, N> &out)
{
    if (hex.size() != 2 * N) {
        return false;
    }
    for (size_t i = 0; i < N; i++) {
        int hi = HexNibble(hex[2 * i]);
        int lo = HexNibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>(hi * 16 + lo);
    }
    return true;
}

template <size_t N> std::string ToHex(const std::array<uint8_t, N> &bytes)
{
    return LowerHex(bytes.data(), bytes.size());
}

template <size_t N> bool IsZero(const std::array<uint8_t, N> &bytes)
{
    return std::all_of(bytes.begin(), bytes.end(), [](uint8_t b) { return b == 0; });
}

const char *CredentialStateName(int state)
{
    switch (state) {
    case 0:
        return "provisional";
    case 1:
        return "active";
    default:
        return "released";
    }
}

bool ParseRemovalState(const std::string &name, RemovalState &out)
{
    static const RemovalState all[] = {RemovalState::Pending,  RemovalState::Sent,      RemovalState::Accepted,
                                       RemovalState::Released, RemovalState::Completed, RemovalState::Cancelled,
                                       RemovalState::Superseded};
    for (RemovalState s : all) {
        if (name == RemovalStateName(s)) {
            out = s;
            return true;
        }
    }
    return false;
}

bool GetString(const JsonValue &object, const char *key, std::string &out)
{
    const JsonValue *v = object.Find(key);
    if (v == nullptr || !v->IsString()) {
        return false;
    }
    out = v->string_value;
    return true;
}

bool GetInt(const JsonValue &object, const char *key, int64_t &out)
{
    const JsonValue *v = object.Find(key);
    if (v == nullptr || !v->IsInteger()) {
        return false;
    }
    out = v->int_value;
    return true;
}

void AppendKeyString(std::string &out, const char *key, const std::string &value, bool comma = true)
{
    out += '"';
    out += key;
    out += "\":";
    AppendJsonStringLiteral(out, value);
    if (comma) {
        out += ',';
    }
}

void AppendKeyInt(std::string &out, const char *key, int64_t value, bool comma = true)
{
    out += '"';
    out += key;
    out += "\":";
    out += std::to_string(value);
    if (comma) {
        out += ',';
    }
}

std::string ShortId(const std::string &id)
{
    return id.substr(0, 8);
}

ingest::ManagementReply Refusal(uint8_t code)
{
    ingest::ManagementReply reply;
    reply.code = code;
    return reply;
}

} // namespace

const char *RemovalStateName(RemovalState state)
{
    switch (state) {
    case RemovalState::Pending:
        return "pending";
    case RemovalState::Sent:
        return "sent";
    case RemovalState::Accepted:
        return "accepted";
    case RemovalState::Released:
        return "released";
    case RemovalState::Completed:
        return "completed";
    case RemovalState::Cancelled:
        return "cancelled";
    case RemovalState::Superseded:
        return "superseded";
    }
    return "pending";
}

bool IsFinalRemovalState(RemovalState state)
{
    return state == RemovalState::Completed || state == RemovalState::Cancelled || state == RemovalState::Superseded;
}

const char *EraseConfirmationName(EraseConfirmation confirmation)
{
    switch (confirmation) {
    case EraseConfirmation::Automatic:
        return "automatic";
    case EraseConfirmation::Manual:
        return "manual";
    case EraseConfirmation::None:
        break;
    }
    return "none";
}

PowerBaseManagement::SigningCopy::~SigningCopy()
{
    OPENSSL_cleanse(credential.key.data(), credential.key.size());
}

PowerBaseManagement::PowerBaseManagement(ingest::Registry &registry, PowerBaseManagementConfig config)
    : registry_(registry), config_(std::move(config))
{
    hub_id_ok_ = ParseHex(config_.hub_id, hub_id_) && !IsZero(hub_id_);
}

PowerBaseManagement::~PowerBaseManagement()
{
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        worker_stop_ = true;
    }
    worker_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

int64_t PowerBaseManagement::NowUnixMs() const
{
    if (config_.now_unix_ms) {
        return config_.now_unix_ms();
    }
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool PowerBaseManagement::available() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return available_;
}

// ---- the state file ----

std::string PowerBaseManagement::Serialize(const State &state)
{
    std::string out = "{";
    AppendKeyInt(out, "schema_version", kPowerBaseMgmtFileSchema);
    out += "\"credentials\":[";
    for (size_t i = 0; i < state.credentials.size(); i++) {
        const Credential &c = state.credentials[i];
        out += i == 0 ? "{" : ",{";
        AppendKeyString(out, "eui64", FormatUuidHex(c.eui));
        AppendKeyString(out, "generation", ToHex(c.generation));
        AppendKeyString(out, "key", ToHex(c.key));
        AppendKeyString(out, "state", CredentialStateName(static_cast<int>(c.state)));
        AppendKeyInt(out, "issued_at_unix_ms", c.issued_at_unix_ms);
        AppendKeyInt(out, "activated_at_unix_ms", c.activated_at_unix_ms);
        // Added later; a file without it means "never announced".
        AppendKeyInt(out, "board_capabilities", static_cast<int64_t>(c.board_capabilities), c.removal_serial != 0);
        // Added later: how many removals were asked of this registration.
        // Written only once one was; a hub that does not know the key ignores
        // it (and, on its next write, drops it).
        if (c.removal_serial != 0) {
            AppendKeyInt(out, "removal_serial", static_cast<int64_t>(c.removal_serial), false);
        }
        out += '}';
    }
    out += "],\"removals\":[";
    for (size_t i = 0; i < state.jobs.size(); i++) {
        const Job &j = state.jobs[i];
        out += i == 0 ? "{" : ",{";
        AppendKeyString(out, "removal_id", j.job.removal_id);
        AppendKeyString(out, "eui64", j.job.power_base_uuid);
        AppendKeyString(out, "generation", j.job.generation);
        AppendKeyString(out, "command_id", ToHex(j.command_id));
        AppendKeyString(out, "state", RemovalStateName(j.job.state));
        AppendKeyInt(out, "created_at_unix_ms", j.job.created_at_unix_ms);
        AppendKeyInt(out, "updated_at_unix_ms", j.job.updated_at_unix_ms);
        AppendKeyInt(out, "completed_at_unix_ms", j.job.completed_at_unix_ms);
        AppendKeyInt(out, "progress_at_unix_ms", j.progress_at_unix_ms);
        AppendKeyString(out, "requested_by_device_id", j.job.requested_by_device_id);
        AppendKeyInt(out, "affected_module_count", j.job.affected_module_count);
        // Zeros unless completed. A MAC, not a secret.
        AppendKeyString(out, "receipt_mac", ToHex(j.receipt_mac));
        // Added later; a file without it has none (zeros).
        AppendKeyString(out, "receipt_ack_mac", ToHex(j.receipt_ack_mac), !j.job.request_id.empty());
        // Added later: the request that created the job. Written only when
        // known; a hub that does not know the key ignores it.
        if (!j.job.request_id.empty()) {
            AppendKeyString(out, "request_id", j.job.request_id, false);
        }
        out += '}';
    }
    out += "],\"blocked\":[";
    for (size_t i = 0; i < state.blocked.size(); i++) {
        const Blocked &b = state.blocked[i];
        out += i == 0 ? "{" : ",{";
        AppendKeyString(out, "eui64", FormatUuidHex(b.eui));
        // Added later: which registration ended, and how. Written only when
        // known; a hub that does not know the two keys ignores them (and, on
        // its next write, drops them -- the block itself it keeps).
        if (b.reason != BlockReason::Unknown) {
            AppendKeyString(out, "reason", b.reason == BlockReason::Released ? "released" : "forgotten");
        }
        if (!b.record.empty()) {
            AppendKeyString(out, "record", b.record);
        }
        AppendKeyInt(out, "since_unix_ms", b.since_unix_ms, false);
        out += '}';
    }
    out += "],\"cleanups\":[";
    for (size_t i = 0; i < state.cleanups.size(); i++) {
        const Cleanup &c = state.cleanups[i];
        out += i == 0 ? "{" : ",{";
        AppendKeyString(out, "removal_id", c.removal_id);
        AppendKeyString(out, "eui64", FormatUuidHex(c.eui));
        AppendKeyString(out, "generation", ToHex(c.generation));
        out += "\"modules\":[";
        for (size_t m = 0; m < c.modules.size(); m++) {
            if (m != 0) {
                out += ',';
            }
            AppendJsonStringLiteral(out, FormatUuidHex(c.modules[m]));
        }
        out += "],";
        // Added later: the room each module was in when the Base left the
        // list, in the order of "modules". Written only when it was read; a
        // hub that does not know the list ignores it (and then clears
        // whatever room a module has, as it always did).
        if (!c.rooms.empty()) {
            out += "\"rooms\":[";
            for (size_t m = 0; m < c.rooms.size(); m++) {
                if (m != 0) {
                    out += ',';
                }
                AppendJsonStringLiteral(out, c.rooms[m]);
            }
            out += "],";
        }
        AppendKeyInt(out, "attempts", c.attempts);
        AppendKeyInt(out, "next_attempt_unix_ms", c.next_attempt_unix_ms, false);
        out += '}';
    }
    // Added later (the list removals); a file without it has none. A hub that
    // does not know the list ignores it and, on its next write, drops it: the
    // effects of a list removal are the block, the credentials and the
    // clean-up above, which such a hub keeps.
    out += "],\"forgets\":[";
    for (size_t i = 0; i < state.forgets.size(); i++) {
        const ForgetRecord &f = state.forgets[i].record;
        out += i == 0 ? "{" : ",{";
        AppendKeyString(out, "forget_id", f.forget_id);
        AppendKeyString(out, "request_id", f.request_id);
        AppendKeyString(out, "eui64", f.power_base_uuid);
        AppendKeyString(out, "record", f.record);
        AppendKeyInt(out, "managed", f.managed ? 1 : 0);
        AppendKeyInt(out, "created_at_unix_ms", f.created_at_unix_ms);
        AppendKeyString(out, "requested_by_device_id", f.requested_by_device_id);
        AppendKeyInt(out, "affected_module_count", f.affected_module_count);
        AppendKeyString(out, "cancelled_removal_id", f.cancelled_removal_id);
        AppendKeyString(out, "kept_removal_id", f.kept_removal_id, false);
        out += '}';
    }
    // Added later (where every module belongs); a file without it has none. A
    // hub that does not know the list ignores it and, on its next write,
    // drops it: it is evidence for a later clean-up, and a hub without it
    // cleans only what it sees in its Registry.
    out += "],\"members\":[";
    for (size_t i = 0; i < state.members.size(); i++) {
        const Member &member = state.members[i];
        out += i == 0 ? "{" : ",{";
        AppendKeyString(out, "eui64", FormatUuidHex(member.eui));
        out += "\"modules\":[";
        for (size_t m = 0; m < member.modules.size(); m++) {
            if (m != 0) {
                out += ',';
            }
            AppendJsonStringLiteral(out, FormatUuidHex(member.modules[m]));
        }
        out += "]}";
    }
    out += "]}\n";
    return out;
}

bool PowerBaseManagement::Parse(const std::string &text, State &out, std::string &error)
{
    JsonValue root;
    JsonParseLimits limits;
    limits.max_values = 16384;
    std::string parse_error;
    if (!ParseJson(text, root, parse_error, limits) || !root.IsObject()) {
        error = "corrupt: not a JSON object";
        return false;
    }
    int64_t schema = 0;
    if (!GetInt(root, "schema_version", schema) || (schema != kPowerBaseMgmtFileSchema && schema != 1)) {
        error = "corrupt: unknown schema_version";
        return false;
    }
    // Schema 1 called a removal "completed" when the board had only said
    // "released" (its stored network was not confirmed erased). Such a job is
    // read as what it really was. Schema 1 had no receipts and no clean-ups.
    const bool legacy = schema == 1;
    const JsonValue *credentials = root.Find("credentials");
    const JsonValue *removals = root.Find("removals");
    const JsonValue *blocked = root.Find("blocked");
    if (credentials == nullptr || !credentials->IsArray() || removals == nullptr || !removals->IsArray() ||
        blocked == nullptr || !blocked->IsArray()) {
        error = "corrupt: a list is missing";
        return false;
    }
    const JsonValue *cleanups = root.Find("cleanups");
    if (!legacy && (cleanups == nullptr || !cleanups->IsArray())) {
        error = "corrupt: a list is missing";
        return false;
    }
    if (credentials->array_items.size() > kMaxManagementCredentials ||
        removals->array_items.size() > kMaxActiveRemovals + kMaxBlockedPowerBases + kMaxFinishedRemovals ||
        blocked->array_items.size() > kMaxBlockedPowerBases ||
        (cleanups != nullptr && cleanups->IsArray() && cleanups->array_items.size() > kMaxBlockedPowerBases)) {
        error = "corrupt: a list is longer than this hub ever writes";
        return false;
    }

    State state;
    for (const JsonValue &item : credentials->array_items) {
        Credential c;
        std::string eui, generation, key, st;
        if (!item.IsObject() || !GetString(item, "eui64", eui) || !GetString(item, "generation", generation) ||
            !GetString(item, "key", key) || !GetString(item, "state", st) ||
            !GetInt(item, "issued_at_unix_ms", c.issued_at_unix_ms) ||
            !GetInt(item, "activated_at_unix_ms", c.activated_at_unix_ms) || !ParseHex(eui, c.eui) ||
            !ParseHex(generation, c.generation) || !ParseHex(key, c.key) || IsZero(c.eui) || IsZero(c.generation) ||
            IsZero(c.key)) {
            error = "corrupt: a credential entry";
            return false;
        }
        if (st == "provisional") {
            c.state = CredentialState::Provisional;
        } else if (st == "active") {
            c.state = CredentialState::Active;
        } else if (st == "released") {
            c.state = CredentialState::Released;
        } else {
            error = "corrupt: a credential state";
            return false;
        }
        // Optional: written since HELLO exists. Present, it must be a value
        // HELLO can carry.
        if (item.Find("board_capabilities") != nullptr) {
            int64_t caps = 0;
            if (!GetInt(item, "board_capabilities", caps) || caps < 0 || caps > 0xffffffffLL) {
                error = "corrupt: a credential entry";
                return false;
            }
            c.board_capabilities = static_cast<uint32_t>(caps);
        }
        // Optional: written since a removal request names it. Present, it
        // must be a count.
        if (item.Find("removal_serial") != nullptr) {
            int64_t serial = 0;
            if (!GetInt(item, "removal_serial", serial) || serial < 0 || serial > 0xffffffffLL) {
                error = "corrupt: a credential entry";
                return false;
            }
            c.removal_serial = static_cast<uint32_t>(serial);
        }
        state.credentials.push_back(c);
        OPENSSL_cleanse(&key[0], key.size());
    }
    for (const JsonValue &item : removals->array_items) {
        Job j;
        std::string st, command;
        int64_t affected = 0;
        if (!item.IsObject() || !GetString(item, "removal_id", j.job.removal_id) ||
            !GetString(item, "eui64", j.job.power_base_uuid) || !GetString(item, "generation", j.job.generation) ||
            !GetString(item, "command_id", command) || !GetString(item, "state", st) ||
            !GetInt(item, "created_at_unix_ms", j.job.created_at_unix_ms) ||
            !GetInt(item, "updated_at_unix_ms", j.job.updated_at_unix_ms) ||
            !GetInt(item, "completed_at_unix_ms", j.job.completed_at_unix_ms) ||
            !GetInt(item, "progress_at_unix_ms", j.progress_at_unix_ms) ||
            !GetString(item, "requested_by_device_id", j.job.requested_by_device_id) ||
            !GetInt(item, "affected_module_count", affected) || !IsLowerHex(j.job.removal_id, 32) ||
            !ParseHex(j.job.power_base_uuid, j.eui) || !ParseHex(j.job.generation, j.generation) ||
            !ParseHex(command, j.command_id) || IsZero(j.eui) || IsZero(j.generation) || IsZero(j.command_id) ||
            !ParseRemovalState(st, j.job.state) || affected < 0 || affected > 1000 ||
            j.job.requested_by_device_id.size() > 64) {
            error = "corrupt: a removal entry";
            return false;
        }
        j.job.affected_module_count = static_cast<int>(affected);
        if (legacy) {
            if (j.job.state == RemovalState::Completed) {
                j.job.state = RemovalState::Released;
                j.job.completed_at_unix_ms = 0;
            }
        } else {
            std::string receipt_mac;
            if (!GetString(item, "receipt_mac", receipt_mac) || !ParseHex(receipt_mac, j.receipt_mac) ||
                (j.job.state == RemovalState::Completed) == IsZero(j.receipt_mac)) {
                // A completed job without the receipt that completed it, or a
                // receipt on a job that is not completed, is not this hub's.
                error = "corrupt: a removal entry";
                return false;
            }
            // Optional: written since a repeated receipt is acknowledged
            // again. Only a completed job can have one.
            if (item.Find("receipt_ack_mac") != nullptr) {
                std::string ack_mac;
                if (!GetString(item, "receipt_ack_mac", ack_mac) || !ParseHex(ack_mac, j.receipt_ack_mac) ||
                    (j.job.state != RemovalState::Completed && !IsZero(j.receipt_ack_mac))) {
                    error = "corrupt: a removal entry";
                    return false;
                }
            }
        }
        // Optional: written since a request has an id. Present, it is one.
        if (const JsonValue *request_id = item.Find("request_id")) {
            if (!request_id->IsString() || !IsLowerHex(request_id->string_value, 32)) {
                error = "corrupt: a removal entry";
                return false;
            }
            j.job.request_id = request_id->string_value;
        }
        state.jobs.push_back(j);
    }
    for (const JsonValue &item : blocked->array_items) {
        Blocked b;
        std::string eui;
        if (!item.IsObject() || !GetString(item, "eui64", eui) || !GetInt(item, "since_unix_ms", b.since_unix_ms) ||
            !ParseHex(eui, b.eui) || IsZero(b.eui)) {
            error = "corrupt: a blocked entry";
            return false;
        }
        // Optional, but never half-true: a reason this hub does not know or
        // a record that is no record means the file is not what this hub (or
        // an earlier one) wrote.
        if (const JsonValue *reason = item.Find("reason")) {
            if (!reason->IsString() || (reason->string_value != "released" && reason->string_value != "forgotten")) {
                error = "corrupt: a blocked entry's reason";
                return false;
            }
            b.reason = reason->string_value == "released" ? BlockReason::Released : BlockReason::Forgotten;
        }
        if (const JsonValue *record = item.Find("record")) {
            if (!record->IsString() || !IsLowerHex(record->string_value, 32)) {
                error = "corrupt: a blocked entry's record";
                return false;
            }
            b.record = record->string_value;
        }
        state.blocked.push_back(b);
    }
    if (cleanups != nullptr && cleanups->IsArray()) {
        for (const JsonValue &item : cleanups->array_items) {
            Cleanup c;
            std::string eui, generation;
            int64_t attempts = 0;
            const JsonValue *modules = item.IsObject() ? item.Find("modules") : nullptr;
            if (!item.IsObject() || !GetString(item, "removal_id", c.removal_id) || !GetString(item, "eui64", eui) ||
                !GetString(item, "generation", generation) || !GetInt(item, "attempts", attempts) ||
                !GetInt(item, "next_attempt_unix_ms", c.next_attempt_unix_ms) || !IsLowerHex(c.removal_id, 32) ||
                !ParseHex(eui, c.eui) || !ParseHex(generation, c.generation) || IsZero(c.eui) ||
                IsZero(c.generation) || attempts < 0 || attempts > 1000000 || modules == nullptr ||
                !modules->IsArray() || modules->array_items.empty() ||
                modules->array_items.size() > kMaxCleanupModules) {
                error = "corrupt: a cleanup entry";
                return false;
            }
            c.attempts = static_cast<int>(attempts);
            for (const JsonValue &m : modules->array_items) {
                ingest::Uuid module{};
                if (!m.IsString() || !ParseHex(m.string_value, module) || IsZero(module)) {
                    error = "corrupt: a cleanup entry";
                    return false;
                }
                c.modules.push_back(module);
            }
            // Optional: written since a clean-up is bound to the room each
            // module was in. Present, it names one room per module.
            const JsonValue *rooms = item.Find("rooms");
            if (rooms != nullptr) {
                if (!rooms->IsArray() || rooms->array_items.size() != c.modules.size()) {
                    error = "corrupt: a cleanup entry";
                    return false;
                }
                for (const JsonValue &room : rooms->array_items) {
                    if (!room.IsString() || room.string_value.empty() || CanonicalRoomId(room.string_value) != room.string_value) {
                        error = "corrupt: a cleanup entry";
                        return false;
                    }
                    c.rooms.push_back(room.string_value);
                }
            }
            state.cleanups.push_back(std::move(c));
        }
    }
    // Optional: written since a Base can be taken out of the list. Present,
    // every entry must be one this hub writes.
    const JsonValue *forgets = root.Find("forgets");
    if (forgets != nullptr) {
        if (legacy || !forgets->IsArray() || forgets->array_items.size() > kMaxForgetRecords) {
            error = "corrupt: the forgets list";
            return false;
        }
        for (const JsonValue &item : forgets->array_items) {
            Forget f;
            int64_t managed = 0;
            int64_t affected = 0;
            auto id_or_empty = [](const std::string &id) { return id.empty() || IsLowerHex(id, 32); };
            if (!item.IsObject() || !GetString(item, "forget_id", f.record.forget_id) ||
                !GetString(item, "request_id", f.record.request_id) ||
                !GetString(item, "eui64", f.record.power_base_uuid) || !GetString(item, "record", f.record.record) ||
                !GetInt(item, "managed", managed) || !GetInt(item, "created_at_unix_ms", f.record.created_at_unix_ms) ||
                !GetString(item, "requested_by_device_id", f.record.requested_by_device_id) ||
                !GetInt(item, "affected_module_count", affected) ||
                !GetString(item, "cancelled_removal_id", f.record.cancelled_removal_id) ||
                !GetString(item, "kept_removal_id", f.record.kept_removal_id) ||
                !IsLowerHex(f.record.forget_id, 32) || !IsLowerHex(f.record.request_id, 32) ||
                !IsLowerHex(f.record.record, 32) || !ParseHex(f.record.power_base_uuid, f.eui) || IsZero(f.eui) ||
                (managed != 0 && managed != 1) || affected < 0 || affected > 1000 ||
                f.record.requested_by_device_id.size() > 64 || !id_or_empty(f.record.cancelled_removal_id) ||
                !id_or_empty(f.record.kept_removal_id) ||
                (!f.record.cancelled_removal_id.empty() && !f.record.kept_removal_id.empty())) {
                error = "corrupt: a forget entry";
                return false;
            }
            f.record.managed = managed == 1;
            f.record.affected_module_count = static_cast<int>(affected);
            state.forgets.push_back(std::move(f));
        }
    }
    // Optional: written since the modules of every listed Base are noted.
    // Present, every entry must be one this hub writes: one entry per Base,
    // every module under one Base only.
    const JsonValue *members = root.Find("members");
    if (members != nullptr) {
        if (legacy || !members->IsArray() || members->array_items.size() > kMaxMemberPowerBases) {
            error = "corrupt: the members list";
            return false;
        }
        size_t total = 0;
        for (const JsonValue &item : members->array_items) {
            Member member;
            std::string eui;
            const JsonValue *modules = item.IsObject() ? item.Find("modules") : nullptr;
            if (!item.IsObject() || !GetString(item, "eui64", eui) || !ParseHex(eui, member.eui) || IsZero(member.eui) ||
                modules == nullptr || !modules->IsArray() || modules->array_items.empty() ||
                modules->array_items.size() > kMaxCleanupModules ||
                std::any_of(state.members.begin(), state.members.end(),
                            [&](const Member &m) { return m.eui == member.eui; })) {
                error = "corrupt: a members entry";
                return false;
            }
            for (const JsonValue &m : modules->array_items) {
                ingest::Uuid module{};
                const bool known_elsewhere =
                    m.IsString() && ParseHex(m.string_value, module) &&
                    (std::find(member.modules.begin(), member.modules.end(), module) != member.modules.end() ||
                     std::any_of(state.members.begin(), state.members.end(), [&](const Member &other) {
                         return std::find(other.modules.begin(), other.modules.end(), module) != other.modules.end();
                     }));
                if (!m.IsString() || !ParseHex(m.string_value, module) || IsZero(module) || known_elsewhere) {
                    error = "corrupt: a members entry";
                    return false;
                }
                member.modules.push_back(module);
            }
            total += member.modules.size();
            state.members.push_back(std::move(member));
        }
        if (total > kMaxMemberModules) {
            error = "corrupt: the members list";
            return false;
        }
    }
    // What this hub never writes: more block slots in use than there are.
    if (UsedBlockSlots(state) > kMaxBlockedPowerBases) {
        error = "corrupt: more removals than block slots";
        return false;
    }
    out = std::move(state);
    return true;
}

bool PowerBaseManagement::Load(std::string &error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    available_ = false;
    if (!hub_id_ok_) {
        error = "no usable hub_id";
        return false;
    }
    std::string text;
    std::string read_error;
    ReadFileResult result = ReadWholeFile(path(), kMaxPowerBaseMgmtFileBytes, text, read_error);
    if (result == ReadFileResult::Error) {
        error = "io: " + read_error;
        return false;
    }
    State state;
    if (result == ReadFileResult::Ok) {
        bool ok = Parse(text, state, error);
        OPENSSL_cleanse(&text[0], text.size());
        if (!ok) {
            return false;
        }
    }
    // A clean-up read from the file is taken on NOW: a module the Registry
    // newly lists under a Power Base from here on is that registration's
    // (in the bridge the Registry is as new as this object; whatever it
    // listed before this moment is covered by what it lists now).
    const uint64_t taken_on_at = registry_.generation();
    for (Cleanup &c : state.cleanups) {
        c.from_file = true;
        c.noted_generation = taken_on_at;
    }
    // What was written down of where the modules are is what an earlier
    // process saw: none of it has been seen by THIS process. A Base that is
    // blocked is not listed and has no members (this hub never writes any for
    // one).
    state.members.erase(std::remove_if(state.members.begin(), state.members.end(),
                                       [&](const Member &member) { return IsBlockedLocked(state, member.eui); }),
                        state.members.end());
    for (Member &member : state.members) {
        member.unconfirmed = member.modules;
    }
    state_ = std::move(state);
    observed_any_ = false;
    observed_generation_ = 0;
    observed_nodes_.clear();
    observation_dirty_ = false;
    observation_store_failed_at_unix_ms_ = 0;
    observation_store_failures_ = 0;
    list_changed_generation_ = 0;
    for (const Blocked &b : state_.blocked) {
        registry_.set_power_base_blocked(b.eui, true);
    }
    loaded_at_unix_ms_ = NowUnixMs();
    available_ = true;
    if (!state_.cleanups.empty()) {
        std::fprintf(stdout,
                     "pogohome_api: power base management: %zu settings clean-up(s) of earlier removals are still "
                     "pending and will be retried\n",
                     state_.cleanups.size());
    }
    return true;
}

bool PowerBaseManagement::StoreLocked(const State &state)
{
    std::string contents = Serialize(state);
    std::string write_error;
    bool ok = false;
    if (contents.size() > kMaxPowerBaseMgmtFileBytes) {
        // Load() would call such a file damaged. It is never written: the
        // change that needed it fails like any other failed write.
        write_error = "the state would not fit the file this hub reads back";
    } else {
        ok = config_.writer ? config_.writer(path(), contents, write_error)
                            : WriteFileAtomically(path(), contents, write_error);
    }
    OPENSSL_cleanse(&contents[0], contents.size());
    if (!ok) {
        // The reason only; never the contents.
        std::fprintf(stderr, "pogohome_api: power base management: could not store state (%s)\n", write_error.c_str());
    } else {
        // Every state that is stored was copied from state_ under this lock:
        // what ObserveLocked() noted there is in the file now.
        observation_dirty_ = false;
        observation_store_failed_at_unix_ms_ = 0;
        observation_store_failures_ = 0;
    }
    return ok;
}

void PowerBaseManagement::PruneFinishedLocked(State &state, int64_t now_unix_ms, int64_t retain_ms)
{
    // Final jobs older than the retention, then the oldest final ones beyond
    // the cap. A job that is not final is never dropped (a released job stays
    // until its receipt arrives or a new enrollment supersedes it), and
    // neither is one whose settings clean-up is still owed.
    auto cleanup_owed = [&](const Job &j) {
        return std::any_of(state.cleanups.begin(), state.cleanups.end(),
                           [&](const Cleanup &c) { return c.removal_id == j.job.removal_id; });
    };
    state.jobs.erase(std::remove_if(state.jobs.begin(), state.jobs.end(),
                                    [&](const Job &j) {
                                        return IsFinalRemovalState(j.job.state) && !cleanup_owed(j) &&
                                               now_unix_ms - j.job.updated_at_unix_ms > retain_ms;
                                    }),
                     state.jobs.end());
    auto finished = static_cast<size_t>(std::count_if(
        state.jobs.begin(), state.jobs.end(), [](const Job &j) { return IsFinalRemovalState(j.job.state); }));
    while (finished > kMaxFinishedRemovals) {
        auto oldest = state.jobs.end();
        for (auto it = state.jobs.begin(); it != state.jobs.end(); ++it) {
            if (IsFinalRemovalState(it->job.state) && !cleanup_owed(*it) &&
                (oldest == state.jobs.end() || it->job.updated_at_unix_ms < oldest->job.updated_at_unix_ms)) {
                oldest = it;
            }
        }
        if (oldest == state.jobs.end()) {
            break; // every remaining final job still owes its clean-up
        }
        state.jobs.erase(oldest);
        finished--;
    }
    // A list removal is final from the start. Kept as long as a finished job,
    // longer while its settings clean-up is owed -- and for as long as it is
    // the reason its Base is not listed (ForgetExplainsBlockLocked()).
    state.forgets.erase(std::remove_if(state.forgets.begin(), state.forgets.end(),
                                       [&](const Forget &f) {
                                           const bool owed = std::any_of(
                                               state.cleanups.begin(), state.cleanups.end(),
                                               [&](const Cleanup &c) { return c.removal_id == f.record.forget_id; });
                                           return !owed && !ForgetExplainsBlockLocked(state, f) &&
                                                  now_unix_ms - f.record.created_at_unix_ms > retain_ms;
                                       }),
                        state.forgets.end());
}

bool PowerBaseManagement::ForgetExplainsBlockLocked(const State &state, const Forget &forget)
{
    auto block = std::find_if(state.blocked.begin(), state.blocked.end(),
                              [&](const Blocked &b) { return b.eui == forget.eui; });
    if (block == state.blocked.end()) {
        return false; // registered again since: that record is history
    }
    if (block->reason == BlockReason::Released) {
        return false; // the Base was registered again after it, and then removed
    }
    if (block->reason == BlockReason::Forgotten && !block->record.empty() && block->record != forget.record.record) {
        return false; // a list removal of another registration made this block
    }
    // The last one stored for that Base (the list is in the order of the
    // requests; no clock is asked).
    for (auto it = state.forgets.rbegin(); it != state.forgets.rend(); ++it) {
        if (it->eui == forget.eui) {
            return it->record.forget_id == forget.record.forget_id;
        }
    }
    return false;
}

size_t PowerBaseManagement::UsedBlockSlots(const State &state)
{
    size_t used = state.blocked.size();
    for (const Job &j : state.jobs) {
        const bool may_block = j.job.state == RemovalState::Pending || j.job.state == RemovalState::Sent ||
                               j.job.state == RemovalState::Accepted;
        if (may_block && std::none_of(state.blocked.begin(), state.blocked.end(),
                                      [&](const Blocked &b) { return b.eui == j.eui; })) {
            used++;
        }
    }
    return used;
}

size_t PowerBaseManagement::FreeBlockSlots() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t used = UsedBlockSlots(state_);
    return used >= kMaxBlockedPowerBases ? 0 : kMaxBlockedPowerBases - used;
}

RemovalJob PowerBaseManagement::CopyJobLocked(const Job &job) const
{
    RemovalJob out = job.job;
    out.cancellable = job.job.state == RemovalState::Pending;
    out.result_unknown = (job.job.state == RemovalState::Sent || job.job.state == RemovalState::Accepted) &&
                         NowUnixMs() - job.progress_at_unix_ms > config_.result_unknown_after.count();
    out.settings_cleanup_pending =
        std::any_of(state_.cleanups.begin(), state_.cleanups.end(),
                    [&](const Cleanup &c) { return c.removal_id == job.job.removal_id; });
    out.erase_confirmation = EraseConfirmationLocked(job, out.result_unknown);
    // A job whose command may be with the board, of a Base this hub blocks:
    // only a list removal leaves that behind (a release moves the job on).
    out.power_base_list_removed =
        (job.job.state == RemovalState::Sent || job.job.state == RemovalState::Accepted) &&
        IsBlockedLocked(state_, job.eui);
    return out;
}

EraseConfirmation PowerBaseManagement::EraseConfirmationLocked(const Job &job, bool result_unknown) const
{
    if (IsFinalRemovalState(job.job.state)) {
        return EraseConfirmation::None;
    }
    // The credential of a job that is not final is still held (active, or
    // released and kept for the receipt): what the board announced is there.
    bool board_sends_receipt = false;
    for (const Credential &c : state_.credentials) {
        if (c.eui == job.eui && c.generation == job.generation) {
            board_sends_receipt = (c.board_capabilities & PB_MGMT_BOARD_CAP_THREAD_RECEIPT) != 0;
        }
    }
    if (!board_sends_receipt || result_unknown) {
        return EraseConfirmation::Manual;
    }
    if (job.job.state == RemovalState::Released &&
        NowUnixMs() - job.progress_at_unix_ms > config_.automatic_confirmation_window.count()) {
        // It said it would, and the time it needs for that is over.
        return EraseConfirmation::Manual;
    }
    return EraseConfirmation::Automatic;
}

PowerBaseManagement::Credential *PowerBaseManagement::FindCredentialLocked(const ingest::Uuid &eui,
                                                                         const std::array<uint8_t, 16> &generation)
{
    for (Credential &c : state_.credentials) {
        if (c.eui == eui && c.generation == generation) {
            return &c;
        }
    }
    return nullptr;
}

PowerBaseManagement::Job *PowerBaseManagement::FindOpenJobLocked(const ingest::Uuid &eui)
{
    for (Job &j : state_.jobs) {
        if (j.eui == eui && !IsFinalRemovalState(j.job.state)) {
            return &j;
        }
    }
    return nullptr;
}

// ---- enrollment sessions ----

bool PowerBaseManagement::IssueCredential(const std::string &eui64, ManagementGrant &out)
{
    out = ManagementGrant();
    Credential cred;
    if (!ParseHex(eui64, cred.eui) || IsZero(cred.eui)) {
        return false;
    }
    if (!SecureRandomBytes(cred.generation.data(), cred.generation.size()) ||
        !SecureRandomBytes(cred.key.data(), cred.key.size()) || IsZero(cred.generation) || IsZero(cred.key)) {
        return false;
    }
    cred.state = CredentialState::Provisional;
    cred.issued_at_unix_ms = NowUnixMs();

    std::lock_guard<std::mutex> lock(mutex_);
    if (!available_) {
        OPENSSL_cleanse(cred.key.data(), cred.key.size());
        return false;
    }
    State next = state_;
    // One provisional credential per board: a new session replaces the one
    // of an earlier session that never led to a poll.
    next.credentials.erase(std::remove_if(next.credentials.begin(), next.credentials.end(),
                                          [&](const Credential &c) {
                                              return c.eui == cred.eui && c.state == CredentialState::Provisional;
                                          }),
                           next.credentials.end());
    if (next.credentials.size() >= kMaxManagementCredentials) {
        // Make room with the oldest provisional credential of any board: it
        // was never used. Active and released ones are never dropped here.
        auto oldest = next.credentials.end();
        for (auto it = next.credentials.begin(); it != next.credentials.end(); ++it) {
            if (it->state == CredentialState::Provisional &&
                (oldest == next.credentials.end() || it->issued_at_unix_ms < oldest->issued_at_unix_ms)) {
                oldest = it;
            }
        }
        if (oldest == next.credentials.end()) {
            OPENSSL_cleanse(cred.key.data(), cred.key.size());
            return false;
        }
        next.credentials.erase(oldest);
    }
    next.credentials.push_back(cred);
    if (!StoreLocked(next)) {
        OPENSSL_cleanse(cred.key.data(), cred.key.size());
        return false;
    }
    state_ = std::move(next);
    out.version = kPowerBaseManagementVersion;
    out.hub_id = config_.hub_id;
    out.generation = ToHex(cred.generation);
    out.key = ToHex(cred.key);
    OPENSSL_cleanse(cred.key.data(), cred.key.size());
    std::fprintf(stdout, "pogohome_api: power base management: credential issued for Power Base %s (provisional)\n",
                 eui64.c_str());
    return true;
}

bool PowerBaseManagement::IsBlocked(const std::string &eui64) const
{
    ingest::Uuid eui{};
    if (!ParseHex(eui64, eui)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(state_.blocked.begin(), state_.blocked.end(), [&](const Blocked &b) { return b.eui == eui; });
}

std::vector<std::string> PowerBaseManagement::ManagedLocked(bool self_confirming_only) const
{
    std::vector<std::string> out;
    for (const Credential &c : state_.credentials) {
        if (c.state != CredentialState::Active || IsBlockedLocked(state_, c.eui)) {
            continue;
        }
        if (!self_confirming_only || (c.board_capabilities & PB_MGMT_BOARD_CAP_THREAD_RECEIPT) != 0) {
            out.push_back(FormatUuidHex(c.eui));
        }
    }
    return out;
}

std::vector<std::string> PowerBaseManagement::ManagedPowerBases() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ManagedLocked(false);
}

std::vector<std::string> PowerBaseManagement::SelfConfirmingPowerBases() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ManagedLocked(true);
}

bool PowerBaseManagement::IsManaged(const std::string &eui64) const
{
    ingest::Uuid eui{};
    if (!ParseHex(eui64, eui)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return !IsBlockedLocked(state_, eui) &&
           std::any_of(state_.credentials.begin(), state_.credentials.end(),
                       [&](const Credential &c) { return c.eui == eui && c.state == CredentialState::Active; });
}

bool PowerBaseManagement::IsBlockedLocked(const State &state, const ingest::Uuid &eui)
{
    return std::any_of(state.blocked.begin(), state.blocked.end(), [&](const Blocked &b) { return b.eui == eui; });
}

// ---- the management resource ----

ingest::ManagementReply PowerBaseManagement::SignedReplyLocked(pb_mgmt_msg_t &answer, const Credential &cred) const
{
    std::memcpy(answer.id.eui64, cred.eui.data(), PB_MGMT_EUI64_LEN);
    std::memcpy(answer.id.hub_id, hub_id_.data(), PB_MGMT_HUB_ID_LEN);
    std::memcpy(answer.id.generation, cred.generation.data(), PB_MGMT_GENERATION_LEN);
    ingest::ManagementReply reply;
    reply.payload.resize(PB_MGMT_MAX_MESSAGE_LEN);
    size_t n = pb_mgmt_pack(&answer, cred.key.data(), OpenSslHmac, nullptr, reply.payload.data(), reply.payload.size());
    if (n == 0) {
        return Refusal(ingest::kCoapCodeServiceUnavailable);
    }
    reply.payload.resize(n);
    reply.code = ingest::kCoapCodeContent;
    return reply;
}

ingest::ManagementReply PowerBaseManagement::HandleDatagram(const std::vector<uint8_t> &request)
{
    pb_mgmt_msg_t msg;
    if (request.empty() || !pb_mgmt_parse(request.data(), request.size(), &msg)) {
        return Refusal(ingest::kCoapCodeBadRequest);
    }
    // Only what a board sends.
    if (msg.type != PB_MGMT_TYPE_POLL && msg.type != PB_MGMT_TYPE_UNENROLL_ACCEPTED &&
        msg.type != PB_MGMT_TYPE_UNENROLL_DONE && msg.type != PB_MGMT_TYPE_HELLO &&
        msg.type != PB_MGMT_TYPE_LOCATE) {
        return Refusal(ingest::kCoapCodeBadRequest);
    }

    ingest::Uuid eui{};
    std::array<uint8_t, 16> generation{};
    std::memcpy(eui.data(), msg.id.eui64, eui.size());
    std::memcpy(generation.data(), msg.id.generation, generation.size());

    bool released = false;
    ingest::ManagementReply reply;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!available_) {
            return Refusal(ingest::kCoapCodeServiceUnavailable);
        }
        // One answer for "another hub", "unknown board or generation" and
        // "wrong key": the sender learns nothing about which it was.
        Credential *cred = FindCredentialLocked(eui, generation);
        const bool for_this_hub = CRYPTO_memcmp(msg.id.hub_id, hub_id_.data(), hub_id_.size()) == 0;
        if (cred == nullptr && for_this_hub && msg.type == PB_MGMT_TYPE_UNENROLL_DONE &&
            msg.result == PB_MGMT_RESULT_ERASED) {
            // No key for this enrollment any more. The one message that can
            // still be answered is the receipt that completed its removal.
            return RepeatedReceiptLocked(msg, request.data(), request.size());
        }
        if (cred == nullptr || !for_this_hub ||
            !pb_mgmt_verify(request.data(), request.size(), cred->key.data(), OpenSslHmac, nullptr)) {
            return Refusal(ingest::kCoapCodeUnauthorized);
        }
        switch (msg.type) {
        case PB_MGMT_TYPE_POLL:
            reply = HandlePollLocked(msg, *cred);
            break;
        case PB_MGMT_TYPE_UNENROLL_ACCEPTED:
            reply = HandleAcceptedLocked(msg, *cred);
            break;
        case PB_MGMT_TYPE_HELLO:
            reply = HandleHelloLocked(msg, *cred);
            break;
        case PB_MGMT_TYPE_LOCATE:
            reply = HandleLocateLocked(msg, *cred);
            break;
        default:
            reply = HandleDoneLocked(msg, request.data(), request.size(), *cred, released);
            break;
        }
    }
    if (released) {
        // The settings clean-up is stored with the release; its first
        // attempt is made by the worker, never on this (ingest) thread and
        // never under the lock.
        WakeCleanupWorker();
    }
    return reply;
}

ingest::ManagementReply PowerBaseManagement::HandleLocateDatagram(const std::vector<uint8_t> &request)
{
    pb_mgmt_msg_t msg;
    if (request.empty() || !pb_mgmt_parse(request.data(), request.size(), &msg) || msg.type != PB_MGMT_TYPE_LOCATE) {
        ingest::ManagementReply reply;
        reply.silent = true;
        return reply;
    }
    return HandleDatagram(request);
}

ingest::ManagementReply PowerBaseManagement::HandleLocateLocked(const pb_mgmt_msg_t &msg, const Credential &cred) const
{
    if (cred.state == CredentialState::Released) {
        // That board said it is out of this enrollment for good; its key is
        // kept only to check its receipt.
        return Refusal(ingest::kCoapCodeUnauthorized);
    }
    std::array<uint8_t, 16> address{};
    uint16_t port = 0;
    if (!config_.ingest_endpoint || !config_.ingest_endpoint(address, port) || port == 0 || IsZero(address)) {
        return Refusal(ingest::kCoapCodeServiceUnavailable);
    }
    // Once per request, and a board asks at most a dozen times a round: who
    // asked is worth a line to whoever watches the hub while it is moved.
    std::fprintf(stdout, "pogohome_api: power base management: Power Base %s asked where this hub is (LOCATE) -- "
                         "answered with the present ingest endpoint, port %u\n",
                 FormatUuidHex(cred.eui).c_str(), (unsigned)port);
    pb_mgmt_msg_t answer;
    std::memset(&answer, 0, sizeof(answer));
    answer.type = PB_MGMT_TYPE_LOCATE_ACK;
    std::memcpy(answer.board_nonce, msg.board_nonce, PB_MGMT_NONCE_LEN);
    std::memcpy(answer.endpoint_addr, address.data(), PB_MGMT_ENDPOINT_ADDR_LEN);
    answer.endpoint_port = port;
    return SignedReplyLocked(answer, cred);
}

ingest::ManagementReply PowerBaseManagement::RepeatedReceiptLocked(const pb_mgmt_msg_t &msg, const uint8_t *wire,
                                                                   size_t wire_len) const
{
    // The caller checked: for this hub, UNENROLL_DONE with result ERASED, well
    // formed. Everything before the MAC is then fixed by the board, the hub,
    // the generation and the command id, so a message for the same four with
    // the same MAC as the verified receipt IS that receipt, byte for byte.
    // One refusal for everything else, as for any message without a key.
    for (const Job &j : state_.jobs) {
        if (j.job.state != RemovalState::Completed || IsZero(j.receipt_ack_mac) ||
            std::memcmp(j.eui.data(), msg.id.eui64, j.eui.size()) != 0 ||
            CRYPTO_memcmp(j.generation.data(), msg.id.generation, j.generation.size()) != 0 ||
            CRYPTO_memcmp(j.command_id.data(), msg.command_id, j.command_id.size()) != 0 ||
            CRYPTO_memcmp(j.receipt_mac.data(), pb_mgmt_mac_of(wire, wire_len), j.receipt_mac.size()) != 0) {
            continue;
        }
        pb_mgmt_msg_t answer;
        std::memset(&answer, 0, sizeof(answer));
        answer.type = PB_MGMT_TYPE_RESULT_ACK;
        std::memcpy(answer.id.eui64, j.eui.data(), PB_MGMT_EUI64_LEN);
        std::memcpy(answer.id.hub_id, hub_id_.data(), PB_MGMT_HUB_ID_LEN);
        std::memcpy(answer.id.generation, j.generation.data(), PB_MGMT_GENERATION_LEN);
        std::memcpy(answer.command_id, j.command_id.data(), PB_MGMT_COMMAND_ID_LEN);
        answer.acked_type = PB_MGMT_TYPE_UNENROLL_DONE;
        answer.acked_result = PB_MGMT_RESULT_ERASED;
        ingest::ManagementReply reply;
        reply.payload.resize(PB_MGMT_MAX_MESSAGE_LEN);
        size_t n = pb_mgmt_pack_with_mac(&answer, j.receipt_ack_mac.data(), reply.payload.data(), reply.payload.size());
        if (n == 0) {
            return Refusal(ingest::kCoapCodeServiceUnavailable);
        }
        reply.payload.resize(n);
        reply.code = ingest::kCoapCodeContent;
        return reply;
    }
    return Refusal(ingest::kCoapCodeUnauthorized);
}

ingest::ManagementReply PowerBaseManagement::HandleHelloLocked(const pb_mgmt_msg_t &msg, Credential &cred)
{
    if (cred.state != CredentialState::Active) {
        // A generation becomes active by its first poll, and a released board
        // has nothing more to announce (this is a replay). Like a poll then.
        return Refusal(ingest::kCoapCodeUnauthorized);
    }
    const SigningCopy signer(cred); // see HandlePollLocked()
    if (cred.board_capabilities != msg.capabilities) {
        State next = state_;
        for (Credential &c : next.credentials) {
            if (c.eui == signer.credential.eui && c.generation == signer.credential.generation) {
                c.board_capabilities = msg.capabilities;
            }
        }
        if (!StoreLocked(next)) {
            return Refusal(ingest::kCoapCodeServiceUnavailable);
        }
        state_ = std::move(next);
        // What a removal of this Base reports (erase_confirmation) changed.
        registry_.notify_external_change();
        std::fprintf(stdout, "pogohome_api: power base management: Power Base %s announced that it %s\n",
                     FormatUuidHex(signer.credential.eui).c_str(),
                     (msg.capabilities & PB_MGMT_BOARD_CAP_THREAD_RECEIPT) != 0
                         ? "sends its release receipt itself"
                         : "does not send its release receipt itself");
    }
    pb_mgmt_msg_t answer;
    std::memset(&answer, 0, sizeof(answer));
    answer.type = PB_MGMT_TYPE_HELLO_ACK;
    answer.capabilities = kHubManagementCapabilities | (config_.ingest_endpoint ? PB_MGMT_HUB_CAP_ENDPOINT_LOCATE : 0u);
    return SignedReplyLocked(answer, signer.credential);
}

ingest::ManagementReply PowerBaseManagement::HandlePollLocked(const pb_mgmt_msg_t &msg, Credential &cred)
{
    if (cred.state == CredentialState::Released) {
        // A released board does not poll; this is a poll from before its
        // release, played again. It gets no command and no nonce.
        return Refusal(ingest::kCoapCodeUnauthorized);
    }
    const int64_t now_ms = NowUnixMs();
    const std::string eui_hex = FormatUuidHex(cred.eui);

    if (cred.state == CredentialState::Provisional) {
        // The board proved it holds this enrollment's key: the generation is
        // active. Everything of an earlier generation of this board ends
        // here -- its credentials, its open removal job, its block.
        State next = state_;
        const ingest::Uuid eui = cred.eui;
        const std::array<uint8_t, 16> generation = cred.generation;
        next.credentials.erase(std::remove_if(next.credentials.begin(), next.credentials.end(),
                                              [&](const Credential &c) {
                                                  return c.eui == eui && c.generation != generation;
                                              }),
                               next.credentials.end());
        for (Credential &c : next.credentials) {
            if (c.eui == eui && c.generation == generation) {
                c.state = CredentialState::Active;
                c.activated_at_unix_ms = now_ms;
            }
        }
        for (Job &j : next.jobs) {
            if (j.eui == eui && !IsFinalRemovalState(j.job.state) && j.generation != generation) {
                j.job.state = RemovalState::Superseded;
                j.job.updated_at_unix_ms = now_ms;
                j.progress_at_unix_ms = now_ms;
            }
        }
        const bool was_blocked =
            std::any_of(next.blocked.begin(), next.blocked.end(), [&](const Blocked &b) { return b.eui == eui; });
        next.blocked.erase(std::remove_if(next.blocked.begin(), next.blocked.end(),
                                          [&](const Blocked &b) { return b.eui == eui; }),
                           next.blocked.end());
        PruneFinishedLocked(next, now_ms, config_.retain_finished.count());
        if (!StoreLocked(next)) {
            return Refusal(ingest::kCoapCodeServiceUnavailable);
        }
        state_ = std::move(next);
        if (was_blocked) {
            registry_.set_power_base_blocked(eui, false);
        }
        // Nothing leaves the Registry here (a Base that was blocked has no
        // entry in it): a copy made before this moment is still what the
        // Registry holds of that Base.
        registry_.notify_external_change();
        std::fprintf(stdout,
                     "pogohome_api: power base management: Power Base %s: enrollment generation active (first "
                     "authenticated poll)%s\n",
                     eui_hex.c_str(), was_blocked ? "; its data is accepted again" : "");
        // `cred` pointed into the old vector.
        Credential *active = FindCredentialLocked(eui, generation);
        if (active == nullptr) {
            return Refusal(ingest::kCoapCodeServiceUnavailable);
        }
        return HandlePollLocked(msg, *active);
    }

    // `cred` points into state_, which a stored change below replaces: the
    // answer is signed with this copy (wiped when it goes out of scope).
    const SigningCopy signer(cred);

    // The nonce this hub gave in its previous answer to this board and
    // generation, if the poll echoes it and it has not expired. One use.
    bool echo_ok = false;
    const auto now_steady = std::chrono::steady_clock::now();
    for (auto it = nonces_.begin(); it != nonces_.end();) {
        if (it->eui == signer.credential.eui && it->generation == signer.credential.generation) {
            echo_ok = now_steady - it->issued_at <= config_.hub_nonce_ttl &&
                      CRYPTO_memcmp(msg.hub_nonce, it->nonce.data(), it->nonce.size()) == 0;
            it = nonces_.erase(it);
        } else {
            ++it;
        }
    }

    Job *job = nullptr;
    for (Job &j : state_.jobs) {
        if (j.eui == signer.credential.eui && j.generation == signer.credential.generation &&
            !IsFinalRemovalState(j.job.state)) {
            job = &j;
        }
    }

    pb_mgmt_msg_t answer;
    std::memset(&answer, 0, sizeof(answer));
    std::memcpy(answer.board_nonce, msg.board_nonce, PB_MGMT_NONCE_LEN);

    if (job != nullptr && echo_ok) {
        // The command goes out. That it MAY have arrived is stored before it
        // is sent: from here on the job cannot be cancelled.
        if (job->job.state == RemovalState::Pending) {
            State next = state_;
            for (Job &j : next.jobs) {
                if (j.job.removal_id == job->job.removal_id) {
                    j.job.state = RemovalState::Sent;
                    j.job.updated_at_unix_ms = now_ms;
                    j.progress_at_unix_ms = now_ms;
                }
            }
            if (!StoreLocked(next)) {
                return Refusal(ingest::kCoapCodeServiceUnavailable);
            }
            const std::string id = job->job.removal_id;
            state_ = std::move(next);
            registry_.notify_external_change();
            std::fprintf(stdout, "pogohome_api: power base removal %s: command sent to Power Base %s\n",
                         ShortId(id).c_str(), eui_hex.c_str());
            for (Job &j : state_.jobs) {
                if (j.job.removal_id == id) {
                    job = &j;
                }
            }
        }
        answer.type = PB_MGMT_TYPE_UNENROLL;
        std::memcpy(answer.command_id, job->command_id.data(), PB_MGMT_COMMAND_ID_LEN);
        return SignedReplyLocked(answer, signer.credential);
    }

    // No command in this answer: a fresh nonce for the next poll.
    HubNonce nonce;
    nonce.eui = signer.credential.eui;
    nonce.generation = signer.credential.generation;
    nonce.issued_at = now_steady;
    if (!SecureRandomBytes(nonce.nonce.data(), nonce.nonce.size()) || IsZero(nonce.nonce)) {
        return Refusal(ingest::kCoapCodeServiceUnavailable);
    }
    // Bounded: one per (board, generation), and never more than there are
    // credentials.
    if (nonces_.size() >= kMaxManagementCredentials) {
        nonces_.erase(nonces_.begin());
    }
    nonces_.push_back(nonce);
    answer.type = PB_MGMT_TYPE_IDLE;
    answer.flags = job != nullptr ? PB_MGMT_IDLE_F_COMMAND_PENDING : 0;
    std::memcpy(answer.hub_nonce, nonce.nonce.data(), PB_MGMT_NONCE_LEN);
    return SignedReplyLocked(answer, signer.credential);
}

ingest::ManagementReply PowerBaseManagement::HandleAcceptedLocked(const pb_mgmt_msg_t &msg, Credential &cred)
{
    const SigningCopy signer(cred); // see HandlePollLocked()
    const int64_t now_ms = NowUnixMs();
    Job *job = nullptr;
    for (Job &j : state_.jobs) {
        if (j.eui == cred.eui && j.generation == cred.generation &&
            CRYPTO_memcmp(j.command_id.data(), msg.command_id, j.command_id.size()) == 0) {
            job = &j;
        }
    }
    if (job == nullptr || job->job.state == RemovalState::Cancelled || job->job.state == RemovalState::Superseded) {
        return Refusal(ingest::kCoapCodeNotFound);
    }
    if (job->job.state == RemovalState::Pending || job->job.state == RemovalState::Sent) {
        State next = state_;
        const std::string id = job->job.removal_id;
        for (Job &j : next.jobs) {
            if (j.job.removal_id == id) {
                j.job.state = RemovalState::Accepted;
                j.job.updated_at_unix_ms = now_ms;
                j.progress_at_unix_ms = now_ms;
            }
        }
        if (!StoreLocked(next)) {
            return Refusal(ingest::kCoapCodeServiceUnavailable);
        }
        state_ = std::move(next);
        registry_.notify_external_change();
        std::fprintf(stdout, "pogohome_api: power base removal %s: the board accepted the command (not complete yet)\n",
                     ShortId(id).c_str());
    }
    // Accepted, released or completed already: the same answer, nothing
    // changes.
    pb_mgmt_msg_t answer;
    std::memset(&answer, 0, sizeof(answer));
    answer.type = PB_MGMT_TYPE_RESULT_ACK;
    std::memcpy(answer.command_id, msg.command_id, PB_MGMT_COMMAND_ID_LEN);
    answer.acked_type = PB_MGMT_TYPE_UNENROLL_ACCEPTED;
    return SignedReplyLocked(answer, signer.credential);
}

bool PowerBaseManagement::PrepareReleaseLocked(State &next, const Job &job, int64_t now_unix_ms,
                                               const ingest::RegistryView &view)
{
    const ingest::Uuid eui = job.eui;
    for (Credential &c : next.credentials) {
        if (c.eui == eui && c.generation == job.generation) {
            c.state = CredentialState::Released;
        }
    }
    if (std::none_of(next.blocked.begin(), next.blocked.end(), [&](const Blocked &b) { return b.eui == eui; })) {
        // The slot was reserved when the removal was requested. Should the
        // list be full all the same, nothing is evicted and nothing is
        // changed: an existing block is never given up for a new one.
        if (next.blocked.size() >= kMaxBlockedPowerBases) {
            return false;
        }
        Blocked b;
        b.eui = eui;
        b.since_unix_ms = now_unix_ms;
        // Which registration ended, and how: what a snapshot says of this
        // Base for as long as it stays out of the list (ListDeparturesLocked()),
        // long after the job itself is no longer shown.
        b.reason = BlockReason::Released;
        b.record = job.job.generation;
        next.blocked.push_back(b);
    }
    // A Base that is blocked already was taken out of the list before (a list
    // removal that left this job as it was): the block keeps saying that.

    // The modules that leave with this Base and are registered under no other
    // Base: their room assignment is cleared later, outside the lock. They
    // are written down HERE, in the same file write as the release, so that
    // a process that ends right after it still knows what is owed.
    NoteCleanupLocked(next, job.job.removal_id, eui, job.generation, view, now_unix_ms, nullptr);
    return true;
}

void PowerBaseManagement::ApplyReleaseToRegistryLocked(const ingest::Uuid &eui)
{
    // From now on nothing of this Base is applied; then it and the modules
    // registered under it leave the Registry (the Matter watcher removes
    // their endpoints through its usual Reconcile()).
    registry_.set_power_base_blocked(eui, true);
    (void)registry_.remove_power_base(eui);
    NoteListChangeLocked();
}

void PowerBaseManagement::NoteListChangeLocked()
{
    registry_.notify_external_change();
    // Read after the Registry moved on: a copy made before this change has a
    // smaller generation, one made after it does not.
    list_changed_generation_ = registry_.generation();
}

// ---- where every module belongs ----

bool PowerBaseManagement::ObserveLocked(const ingest::RegistryView &view)
{
    if (observed_any_ && view.generation < observed_generation_) {
        return false; // older than what was taken in already
    }
    bool changed = false;
    bool cleanup_ended = false;
    auto contains = [](const std::vector<ingest::Uuid> &list, const ingest::Uuid &uuid) {
        return std::find(list.begin(), list.end(), uuid) != list.end();
    };
    auto erase_from = [&](std::vector<ingest::Uuid> &list, const ingest::Uuid &uuid) {
        auto it = std::find(list.begin(), list.end(), uuid);
        if (it == list.end()) {
            return false;
        }
        list.erase(it);
        return true;
    };
    size_t total = 0;
    for (const Member &member : state_.members) {
        total += member.modules.size();
    }

    // One Base per module: the one a snapshot shows it under.
    for (const ingest::NodeState &node : ResolveModuleOwners(view.nodes)) {
        const ingest::Uuid &base = node.power_base_uuid;
        const ingest::Uuid &module = node.module_uuid;
        if (IsBlockedLocked(state_, base)) {
            continue; // the Registry holds nothing of a Base that left the list
        }
        // It is that Base's now, and no other's.
        Member *home = nullptr;
        for (Member &member : state_.members) {
            if (member.eui == base) {
                home = &member;
            } else if (erase_from(member.modules, module)) {
                (void)erase_from(member.unconfirmed, module);
                total--;
                changed = true;
            }
        }
        if (home == nullptr && state_.members.size() < kMaxMemberPowerBases && total < kMaxMemberModules) {
            Member member;
            member.eui = base;
            state_.members.push_back(std::move(member));
            home = &state_.members.back();
        }
        if (home != nullptr) {
            if (!contains(home->modules, module) && home->modules.size() < kMaxCleanupModules &&
                total < kMaxMemberModules) {
                home->modules.push_back(module);
                total++;
                changed = true;
            }
            (void)erase_from(home->unconfirmed, module); // seen for itself now
        }
        // And no clean-up's: the room it has is this registration's. It
        // leaves every clean-up that names it, for good.
        for (Cleanup &c : state_.cleanups) {
            auto it = std::find(c.modules.begin(), c.modules.end(), module);
            if (it == c.modules.end()) {
                continue;
            }
            const size_t index = static_cast<size_t>(it - c.modules.begin());
            c.modules.erase(it);
            if (index < c.rooms.size()) {
                c.rooms.erase(c.rooms.begin() + static_cast<std::ptrdiff_t>(index));
            }
            changed = true;
        }
    }

    // Listed under a Power Base at some time after a clean-up was noted, even
    // though no copy this class took in showed it there (docked and detached
    // again between two looks): that listing was another registration's, and
    // the room the module has is not that clean-up's any more. The Registry
    // remembers when each module was last newly listed. Its table is bounded:
    // a module without an entry was either never listed, or its entry was
    // dropped -- and an entry made after this class last looked can only have
    // been dropped already when the table says it dropped one that late.
    // Only then is "no entry" not taken for "stayed away".
    const uint64_t looked_through = observed_any_ ? observed_generation_ : 0;
    for (Cleanup &c : state_.cleanups) {
        for (size_t i = c.modules.size(); i-- > 0;) {
            const ingest::Uuid &module = c.modules[i];
            auto listing = std::find_if(view.listings.begin(), view.listings.end(),
                                        [&](const ingest::ModuleListing &l) { return l.module_uuid == module; });
            const bool listed_since =
                listing != view.listings.end()
                    ? listing->generation > c.noted_generation
                    : view.listings_dropped_through > std::max(c.noted_generation, looked_through);
            if (!listed_since) {
                continue;
            }
            c.modules.erase(c.modules.begin() + static_cast<std::ptrdiff_t>(i));
            if (i < c.rooms.size()) {
                c.rooms.erase(c.rooms.begin() + static_cast<std::ptrdiff_t>(i));
            }
            changed = true;
        }
    }

    // A node this process saw under a Base that is still listed, and that is
    // gone now: the Base said the module was detached (only that erases a
    // node of a listed Base). It is no member of that Base any more.
    for (const auto &seen : observed_nodes_) {
        const bool still = std::any_of(view.nodes.begin(), view.nodes.end(), [&](const ingest::NodeState &n) {
            return n.power_base_uuid == seen.first && n.module_uuid == seen.second;
        });
        if (still || IsBlockedLocked(state_, seen.first)) {
            continue;
        }
        for (Member &member : state_.members) {
            if (member.eui == seen.first && erase_from(member.modules, seen.second)) {
                (void)erase_from(member.unconfirmed, seen.second);
                changed = true;
            }
        }
    }

    state_.members.erase(std::remove_if(state_.members.begin(), state_.members.end(),
                                        [](const Member &member) { return member.modules.empty(); }),
                         state_.members.end());
    // A clean-up none of whose modules is left has nothing to clear.
    for (auto it = state_.cleanups.begin(); it != state_.cleanups.end();) {
        if (it->modules.empty()) {
            std::fprintf(stdout,
                         "pogohome_api: power base removal %s: settings clean-up ended -- every module it named "
                         "was listed under a Power Base again and keeps its room\n",
                         ShortId(it->removal_id).c_str());
            it = state_.cleanups.erase(it);
            cleanup_ended = true;
        } else {
            ++it;
        }
    }

    observed_nodes_.clear();
    for (const ingest::NodeState &n : view.nodes) {
        observed_nodes_.emplace_back(n.power_base_uuid, n.module_uuid);
    }
    observed_generation_ = view.generation;
    observed_any_ = true;
    if (changed) {
        observation_dirty_ = true;
    }
    return cleanup_ended;
}

void PowerBaseManagement::FlushObservationLocked()
{
    if (!observation_dirty_) {
        return;
    }
    const int64_t now_ms = NowUnixMs();
    if (observation_store_failures_ > 0) {
        // A disk that just refused a write is not asked again at once: the
        // wait doubles like a clean-up's, up to the same maximum.
        int64_t wait = config_.cleanup_retry_min.count();
        for (int i = 1; i < observation_store_failures_ && wait < config_.cleanup_retry_max.count(); i++) {
            wait *= 2;
        }
        wait = std::min<int64_t>(wait, config_.cleanup_retry_max.count());
        if (now_ms >= observation_store_failed_at_unix_ms_ && now_ms - observation_store_failed_at_unix_ms_ < wait) {
            return;
        }
    }
    if (!StoreLocked(state_)) {
        // Nothing is lost: state_ keeps what was seen, the next write of any
        // kind stores it, and until then every decision asks the Registry.
        observation_store_failed_at_unix_ms_ = now_ms;
        if (observation_store_failures_ < 1000) {
            observation_store_failures_++;
        }
    }
}

void PowerBaseManagement::NoteCleanupLocked(State &next, const std::string &removal_id, const ingest::Uuid &eui,
                                            const std::array<uint8_t, 16> &generation, const ingest::RegistryView &view,
                                            int64_t now_unix_ms, size_t *known_modules)
{
    auto listed_elsewhere = [&](const ingest::Uuid &module) {
        return std::any_of(view.nodes.begin(), view.nodes.end(), [&](const ingest::NodeState &o) {
            return o.module_uuid == module && o.power_base_uuid != eui;
        });
    };
    std::vector<ingest::Uuid> modules;
    auto add = [&](const ingest::Uuid &module) {
        if (listed_elsewhere(module) || std::find(modules.begin(), modules.end(), module) != modules.end() ||
            modules.size() >= kMaxCleanupModules) {
            return false;
        }
        modules.push_back(module);
        return true;
    };
    // As the Registry has them at this moment.
    for (const ingest::NodeState &n : view.nodes) {
        if (n.power_base_uuid == eui) {
            (void)add(n.module_uuid);
        }
    }
    // And as they were written down: all that is known of a Base that has not
    // reported since this process started. A member another Base lists now is
    // that Base's; one that is written down nowhere is not made up.
    bool from_file = false;
    for (const Member &member : next.members) {
        if (member.eui != eui) {
            continue;
        }
        for (const ingest::Uuid &module : member.modules) {
            const bool unconfirmed =
                std::find(member.unconfirmed.begin(), member.unconfirmed.end(), module) != member.unconfirmed.end();
            if (add(module) && unconfirmed) {
                from_file = true;
            }
        }
    }
    // The Base leaves the list: it has no members any more.
    next.members.erase(std::remove_if(next.members.begin(), next.members.end(),
                                      [&](const Member &member) { return member.eui == eui; }),
                       next.members.end());
    if (known_modules != nullptr) {
        *known_modules = modules.size();
    }

    Cleanup cleanup;
    cleanup.removal_id = removal_id;
    cleanup.eui = eui;
    cleanup.generation = generation;
    cleanup.next_attempt_unix_ms = now_unix_ms;
    // What only the file says waits out the start-up grace, like a clean-up
    // read from the file: a module that moved while this hub was not looking
    // must be seen under its new Base before its room is touched.
    cleanup.from_file = from_file;
    // A module the Registry newly lists under a Power Base from here on is
    // that registration's (ObserveLocked()).
    cleanup.noted_generation = view.generation;
    // The room each of them is in NOW: that assignment, and no later one, is
    // what this clean-up is for. A module without a room has nothing to clear.
    std::vector<RoomCleanupTarget> rooms;
    std::vector<std::string> hex;
    for (const ingest::Uuid &module : modules) {
        hex.push_back(FormatUuidHex(module));
    }
    if (!modules.empty() && config_.read_rooms && config_.read_rooms(hex, rooms)) {
        for (size_t i = 0; i < modules.size(); i++) {
            auto room = std::find_if(rooms.begin(), rooms.end(),
                                     [&](const RoomCleanupTarget &t) { return t.module_uuid == hex[i]; });
            if (room != rooms.end() && room->room_known && !room->room_id.empty() &&
                CanonicalRoomId(room->room_id) == room->room_id) {
                cleanup.modules.push_back(modules[i]);
                cleanup.rooms.push_back(room->room_id);
            }
        }
    } else {
        cleanup.modules = modules; // the rooms are not known
    }
    next.cleanups.erase(std::remove_if(next.cleanups.begin(), next.cleanups.end(),
                                       [&](const Cleanup &c) { return c.removal_id == removal_id; }),
                        next.cleanups.end());
    if (!cleanup.modules.empty()) {
        next.cleanups.push_back(std::move(cleanup));
    }
}

ingest::ManagementReply PowerBaseManagement::CompleteWithReceiptLocked(const std::string &removal_id,
                                                                       const std::array<uint8_t, 32> &receipt_mac,
                                                                       const Credential &cred, bool &completed)
{
    // A copy: the caller's string may live in state_, which is replaced below.
    const std::string job_id = removal_id;
    completed = false;
    const int64_t now_ms = NowUnixMs();
    // Signed with a copy: the credential is dropped by this very change.
    const SigningCopy signer(cred);
    const Job *job = nullptr;
    for (const Job &j : state_.jobs) {
        if (j.job.removal_id == job_id) {
            job = &j;
        }
    }
    if (job == nullptr) {
        return Refusal(ingest::kCoapCodeNotFound);
    }
    pb_mgmt_msg_t answer;
    std::memset(&answer, 0, sizeof(answer));
    answer.type = PB_MGMT_TYPE_RESULT_ACK;
    std::memcpy(answer.command_id, job->command_id.data(), PB_MGMT_COMMAND_ID_LEN);
    answer.acked_type = PB_MGMT_TYPE_UNENROLL_DONE;
    answer.acked_result = PB_MGMT_RESULT_ERASED;

    // The acknowledgement is made now, while the key exists, and its MAC is
    // stored with the completion: the same receipt arriving again (the board
    // did not get this answer) is then answered with the same bytes.
    ingest::ManagementReply acknowledgement = SignedReplyLocked(answer, signer.credential);
    if (acknowledgement.code != ingest::kCoapCodeContent || acknowledgement.payload.size() != PB_MGMT_RESULT_ACK_LEN) {
        return Refusal(ingest::kCoapCodeServiceUnavailable);
    }
    std::array<uint8_t, 32> ack_mac{};
    std::memcpy(ack_mac.data(), pb_mgmt_mac_of(acknowledgement.payload.data(), acknowledgement.payload.size()),
                ack_mac.size());

    const bool was_released = job->job.state == RemovalState::Released;
    const ingest::Uuid eui = job->eui;
    const std::array<uint8_t, 16> generation = job->generation;
    // Where every module is at this moment (taken in before `next` is made:
    // an observation is state_'s, a decision is `next`'s).
    const ingest::RegistryView view = registry_.view();
    if (!was_released && ObserveLocked(view)) {
        registry_.notify_external_change();
    }
    State next = state_;
    if (!was_released) {
        // The released notice never arrived (lost, or the hub was away): the
        // receipt says more than the notice would have. Everything the
        // notice does is done first.
        if (!PrepareReleaseLocked(next, *job, now_ms, view)) {
            std::fprintf(stderr, "pogohome_api: power base removal %s: no block slot left -- nothing changed\n",
                         ShortId(job_id).c_str());
            return Refusal(ingest::kCoapCodeServiceUnavailable);
        }
    }
    for (Job &j : next.jobs) {
        if (j.job.removal_id == job_id) {
            j.job.state = RemovalState::Completed;
            j.job.updated_at_unix_ms = now_ms;
            j.job.completed_at_unix_ms = now_ms;
            j.progress_at_unix_ms = now_ms;
            j.receipt_mac = receipt_mac;
            j.receipt_ack_mac = ack_mac;
        }
    }
    // The key has done everything it was issued for.
    for (Credential &c : next.credentials) {
        if (c.eui == eui && c.generation == generation) {
            OPENSSL_cleanse(c.key.data(), c.key.size());
        }
    }
    next.credentials.erase(std::remove_if(next.credentials.begin(), next.credentials.end(),
                                          [&](const Credential &c) { return c.eui == eui && c.generation == generation; }),
                           next.credentials.end());
    PruneFinishedLocked(next, now_ms, config_.retain_finished.count());
    if (!StoreLocked(next)) {
        return Refusal(ingest::kCoapCodeServiceUnavailable);
    }
    state_ = std::move(next);
    if (!was_released) {
        ApplyReleaseToRegistryLocked(eui);
    } else {
        registry_.notify_external_change();
    }
    completed = true;
    std::fprintf(stdout,
                 "pogohome_api: power base removal %s: completed -- Power Base %s's receipt verified: its stored "
                 "network was erased and checked\n",
                 ShortId(job_id).c_str(), FormatUuidHex(eui).c_str());
    return acknowledgement;
}

ingest::ManagementReply PowerBaseManagement::HandleDoneLocked(const pb_mgmt_msg_t &msg, const uint8_t *wire,
                                                              size_t wire_len, Credential &cred, bool &released)
{
    released = false;
    const int64_t now_ms = NowUnixMs();
    Job *job = nullptr;
    for (Job &j : state_.jobs) {
        if (j.eui == cred.eui && j.generation == cred.generation &&
            CRYPTO_memcmp(j.command_id.data(), msg.command_id, j.command_id.size()) == 0) {
            job = &j;
        }
    }
    if (job == nullptr || job->job.state == RemovalState::Cancelled || job->job.state == RemovalState::Superseded) {
        // No such command for this enrollment, or one that never was (or no
        // longer is) to be carried out.
        return Refusal(ingest::kCoapCodeNotFound);
    }

    if (msg.result == PB_MGMT_RESULT_ERASED) {
        // The receipt, sent by the board itself over its last Thread session
        // (it also comes in through SubmitReleaseReceipt(), relayed by an
        // app). The same message either way.
        if (job->job.state == RemovalState::Completed) {
            pb_mgmt_msg_t answer;
            std::memset(&answer, 0, sizeof(answer));
            answer.type = PB_MGMT_TYPE_RESULT_ACK;
            std::memcpy(answer.command_id, msg.command_id, PB_MGMT_COMMAND_ID_LEN);
            answer.acked_type = PB_MGMT_TYPE_UNENROLL_DONE;
            answer.acked_result = PB_MGMT_RESULT_ERASED;
            return SignedReplyLocked(answer, cred);
        }
        std::array<uint8_t, 32> receipt_mac{};
        std::memcpy(receipt_mac.data(), pb_mgmt_mac_of(wire, wire_len), receipt_mac.size());
        const bool was_released = job->job.state == RemovalState::Released;
        const std::string id = job->job.removal_id;
        bool completed = false;
        ingest::ManagementReply reply = CompleteWithReceiptLocked(id, receipt_mac, cred, completed);
        released = completed && !was_released;
        return reply;
    }

    // PB_MGMT_RESULT_RELEASED: a notice. The enrollment is out of use for
    // good; the board's stored network is NOT confirmed erased. This never
    // completes a removal.
    pb_mgmt_msg_t answer;
    std::memset(&answer, 0, sizeof(answer));
    answer.type = PB_MGMT_TYPE_RESULT_ACK;
    std::memcpy(answer.command_id, msg.command_id, PB_MGMT_COMMAND_ID_LEN);
    answer.acked_type = PB_MGMT_TYPE_UNENROLL_DONE;
    answer.acked_result = PB_MGMT_RESULT_RELEASED;
    if (job->job.state == RemovalState::Released || job->job.state == RemovalState::Completed) {
        return SignedReplyLocked(answer, cred); // a repeated notice: nothing changes
    }

    // Released, credential marked, Base blocked, clean-up noted -- stored as
    // one file write BEFORE the Registry is touched and before the board is
    // told, so a crash here leaves a job the next identical notice releases.
    const ingest::Uuid eui = cred.eui;
    const std::array<uint8_t, 16> generation = cred.generation;
    const std::string id = job->job.removal_id;
    // Where every module is at this moment (see CompleteWithReceiptLocked()).
    const ingest::RegistryView view = registry_.view();
    if (ObserveLocked(view)) {
        registry_.notify_external_change();
    }
    State next = state_;
    if (!PrepareReleaseLocked(next, *job, now_ms, view)) {
        std::fprintf(stderr, "pogohome_api: power base removal %s: no block slot left -- nothing changed\n",
                     ShortId(id).c_str());
        return Refusal(ingest::kCoapCodeServiceUnavailable);
    }
    size_t noted = 0;
    for (const Cleanup &c : next.cleanups) {
        if (c.removal_id == id) {
            noted = c.modules.size();
        }
    }
    for (Job &j : next.jobs) {
        if (j.job.removal_id == id) {
            j.job.state = RemovalState::Released;
            j.job.updated_at_unix_ms = now_ms;
            j.progress_at_unix_ms = now_ms;
        }
    }
    PruneFinishedLocked(next, now_ms, config_.retain_finished.count());
    if (!StoreLocked(next)) {
        return Refusal(ingest::kCoapCodeServiceUnavailable);
    }
    state_ = std::move(next);
    ApplyReleaseToRegistryLocked(eui);
    released = true;
    std::fprintf(stdout,
                 "pogohome_api: power base removal %s: released -- Power Base %s stored that its enrollment is out "
                 "of use (NOT complete: its stored network is not confirmed erased); %zu module(s) noted for the "
                 "settings clean-up\n",
                 ShortId(id).c_str(), FormatUuidHex(eui).c_str(), noted);

    Credential *kept = FindCredentialLocked(eui, generation);
    if (kept == nullptr) {
        return Refusal(ingest::kCoapCodeServiceUnavailable);
    }
    return SignedReplyLocked(answer, *kept);
}

ReceiptOutcome PowerBaseManagement::SubmitReleaseReceipt(const std::vector<uint8_t> &receipt, RemovalJob &out)
{
    out = RemovalJob();
    pb_mgmt_msg_t msg;
    if (receipt.size() != PB_MGMT_UNENROLL_DONE_LEN || !pb_mgmt_parse(receipt.data(), receipt.size(), &msg) ||
        msg.type != PB_MGMT_TYPE_UNENROLL_DONE || msg.result != PB_MGMT_RESULT_ERASED) {
        // In particular the released notice: it is not a receipt.
        return ReceiptOutcome::Malformed;
    }
    ingest::Uuid eui{};
    std::array<uint8_t, 16> generation{};
    std::array<uint8_t, 32> mac{};
    std::memcpy(eui.data(), msg.id.eui64, eui.size());
    std::memcpy(generation.data(), msg.id.generation, generation.size());
    std::memcpy(mac.data(), pb_mgmt_mac_of(receipt.data(), receipt.size()), mac.size());

    bool completed_now = false;
    bool released_now = false;
    ReceiptOutcome outcome = ReceiptOutcome::Rejected;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!available_) {
            return ReceiptOutcome::Unavailable;
        }
        const bool for_this_hub = CRYPTO_memcmp(msg.id.hub_id, hub_id_.data(), hub_id_.size()) == 0;
        Job *job = nullptr;
        for (Job &j : state_.jobs) {
            if (j.eui == eui && j.generation == generation &&
                CRYPTO_memcmp(j.command_id.data(), msg.command_id, j.command_id.size()) == 0) {
                job = &j;
            }
        }
        if (!for_this_hub || job == nullptr) {
            return ReceiptOutcome::Rejected;
        }
        if (job->job.state == RemovalState::Completed) {
            // The key is gone; the receipt that completed the job is known by
            // its MAC. The same receipt again changes nothing.
            if (CRYPTO_memcmp(job->receipt_mac.data(), mac.data(), mac.size()) != 0) {
                return ReceiptOutcome::Rejected;
            }
            out = CopyJobLocked(*job);
            return ReceiptOutcome::AlreadyCompleted;
        }
        if (job->job.state == RemovalState::Cancelled || job->job.state == RemovalState::Superseded) {
            return ReceiptOutcome::Rejected;
        }
        Credential *cred = FindCredentialLocked(eui, generation);
        if (cred == nullptr ||
            !pb_mgmt_verify(receipt.data(), receipt.size(), cred->key.data(), OpenSslHmac, nullptr)) {
            return ReceiptOutcome::Rejected;
        }
        const std::string id = job->job.removal_id;
        const bool was_released = job->job.state == RemovalState::Released;
        ingest::ManagementReply reply = CompleteWithReceiptLocked(id, mac, *cred, completed_now);
        if (!completed_now) {
            return reply.code == ingest::kCoapCodeServiceUnavailable ? ReceiptOutcome::Unavailable
                                                                     : ReceiptOutcome::Rejected;
        }
        released_now = !was_released;
        for (const Job &j : state_.jobs) {
            if (j.job.removal_id == id) {
                out = CopyJobLocked(j);
            }
        }
        outcome = ReceiptOutcome::Completed;
    }
    if (released_now) {
        WakeCleanupWorker();
    }
    return outcome;
}

// ---- removing a Power Base from this hub's list ----

std::string PowerBaseManagement::RecordLocked(const ingest::Uuid &eui, bool issue)
{
    if (IsBlockedLocked(state_, eui)) {
        return std::string(); // not listed
    }
    // The newest registration this hub issued for the board: one that was
    // started (provisional) counts, so that a request made before it names
    // an earlier one.
    const Credential *newest = nullptr;
    for (const Credential &c : state_.credentials) {
        if (c.eui != eui || c.state == CredentialState::Released) {
            continue;
        }
        if (newest == nullptr || (c.state == CredentialState::Provisional && newest->state != CredentialState::Provisional)) {
            newest = &c;
        }
    }
    if (newest != nullptr) {
        return ToHex(newest->generation);
    }
    for (const auto &entry : unmanaged_records_) {
        if (entry.first == eui) {
            return entry.second;
        }
    }
    if (!issue) {
        return std::string();
    }
    std::string record = RandomHex(16);
    if (!IsLowerHex(record, 32)) {
        return std::string();
    }
    // Bounded: far more than the Registry ever lists.
    if (unmanaged_records_.size() >= 2 * ingest::Registry::kMaxNodes) {
        unmanaged_records_.erase(unmanaged_records_.begin());
    }
    unmanaged_records_.emplace_back(eui, record);
    return record;
}

std::vector<std::pair<std::string, std::string>>
PowerBaseManagement::PowerBaseRecords(const std::vector<std::string> &eui64s)
{
    std::vector<std::pair<std::string, std::string>> out;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!available_) {
        return out;
    }
    for (const std::string &hex : eui64s) {
        ingest::Uuid eui{};
        if (!ParseHex(hex, eui) || IsZero(eui)) {
            continue;
        }
        std::string record = RecordLocked(eui, true);
        if (!record.empty()) {
            out.emplace_back(hex, std::move(record));
        }
    }
    return out;
}

ForgetRecord PowerBaseManagement::CopyForgetLocked(const Forget &forget) const
{
    ForgetRecord out = forget.record;
    out.settings_cleanup_pending =
        std::any_of(state_.cleanups.begin(), state_.cleanups.end(),
                    [&](const Cleanup &c) { return c.removal_id == forget.record.forget_id; });
    return out;
}

ForgetOutcome PowerBaseManagement::ForgetPowerBase(const std::string &device_id, const std::string &eui64,
                                                   const std::string &record, const std::string &request_id,
                                                   ForgetRecord &out)
{
    out = ForgetRecord();
    ingest::Uuid eui{};
    if (!ParseHex(eui64, eui) || IsZero(eui)) {
        return ForgetOutcome::UnknownPowerBase;
    }
    if (!IsLowerHex(request_id, 32)) {
        return ForgetOutcome::RequestConflict;
    }
    // Asked before the lock is taken, like for a removal. A registration that
    // is started after this is not lost to this request: it issues a new
    // credential, and from then on `record` names an earlier registration.
    const bool enrolling = config_.enrollment_in_progress && config_.enrollment_in_progress(eui64);

    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!available_) {
            return ForgetOutcome::Unavailable;
        }
        // The same request again (its answer was lost): the same record.
        for (const Forget &f : state_.forgets) {
            if (f.record.request_id == request_id) {
                if (f.eui != eui) {
                    return ForgetOutcome::RequestConflict;
                }
                out = CopyForgetLocked(f);
                return ForgetOutcome::Existing;
            }
        }
        if (enrolling) {
            return ForgetOutcome::EnrollmentInProgress;
        }
        if (IsBlockedLocked(state_, eui)) {
            // Released by a removal, or taken out of the list already.
            return ForgetOutcome::UnknownPowerBase;
        }
        // Where the Base and its modules are, as the Registry has them now.
        // Whether the Base is heard from is NOT decided from this copy: that
        // is asked again, under the Registry's lock, by the step below.
        const ingest::RegistryView view = registry_.view();
        if (ObserveLocked(view)) {
            registry_.notify_external_change();
        }
        const bool has_credential =
            std::any_of(state_.credentials.begin(), state_.credentials.end(), [&](const Credential &c) {
                return c.eui == eui && c.state != CredentialState::Released;
            });
        int module_count = 0;
        bool in_registry = std::any_of(view.power_bases.begin(), view.power_bases.end(),
                                       [&](const ingest::PowerBaseState &b) { return b.power_base_uuid == eui; });
        for (const ingest::NodeState &n : view.nodes) {
            if (n.power_base_uuid == eui) {
                module_count++;
                in_registry = true;
            }
        }
        const bool has_active =
            std::any_of(state_.credentials.begin(), state_.credentials.end(), [&](const Credential &c) {
                return c.eui == eui && c.state == CredentialState::Active;
            });
        if (!has_active && !in_registry) {
            // Nothing of it is listed (a credential that was never used is
            // not an entry of the list).
            return ForgetOutcome::UnknownPowerBase;
        }
        // The registration the device confirmed must be the one listed now.
        const std::string listed = RecordLocked(eui, false);
        if (listed.empty() || !IsLowerHex(record, 32) || !ConstantTimeEquals(listed, record)) {
            return ForgetOutcome::StaleRecord;
        }

        const int64_t now_ms = NowUnixMs();
        const Job *open = FindOpenJobLocked(eui);
        // A job that is not final has reserved the block this ends in.
        if (open == nullptr && UsedBlockSlots(state_) >= kMaxBlockedPowerBases) {
            std::fprintf(stderr,
                         "pogohome_api: power base list removal refused for %s: all %zu block slots are in use\n",
                         eui64.c_str(), kMaxBlockedPowerBases);
            return ForgetOutcome::CapacityFull;
        }

        Forget forget;
        forget.eui = eui;
        forget.record.forget_id = RandomHex(16);
        if (!IsLowerHex(forget.record.forget_id, 32)) {
            return ForgetOutcome::Unavailable;
        }
        forget.record.request_id = request_id;
        forget.record.power_base_uuid = eui64;
        forget.record.record = record;
        forget.record.managed = has_credential;
        forget.record.created_at_unix_ms = now_ms;
        forget.record.requested_by_device_id = device_id;
        forget.record.affected_module_count = module_count;

        State next = state_;
        PruneFinishedLocked(next, now_ms, config_.retain_finished.count());
        if (next.forgets.size() >= kMaxForgetRecords) {
            return ForgetOutcome::TooMany; // every one of them still owes its clean-up, or is recent
        }
        // The key of a job whose command may be with the board stays, for
        // that job alone. Every other credential of the board ends here.
        std::array<uint8_t, 16> kept_generation{};
        bool keep = false;
        for (Job &j : next.jobs) {
            if (j.eui != eui || IsFinalRemovalState(j.job.state)) {
                continue;
            }
            if (j.job.state == RemovalState::Pending) {
                // Never given to the board (Sent is stored before the command
                // goes out, under this same lock): this is the cancel.
                j.job.state = RemovalState::Cancelled;
                j.job.updated_at_unix_ms = now_ms;
                j.progress_at_unix_ms = now_ms;
                forget.record.cancelled_removal_id = j.job.removal_id;
            } else {
                forget.record.kept_removal_id = j.job.removal_id;
                kept_generation = j.generation;
                keep = true;
            }
        }
        for (Credential &c : next.credentials) {
            if (c.eui == eui && !(keep && c.generation == kept_generation)) {
                OPENSSL_cleanse(c.key.data(), c.key.size());
            }
        }
        next.credentials.erase(std::remove_if(next.credentials.begin(), next.credentials.end(),
                                              [&](const Credential &c) {
                                                  return c.eui == eui && !(keep && c.generation == kept_generation);
                                              }),
                               next.credentials.end());
        Blocked block;
        block.eui = eui;
        block.since_unix_ms = now_ms;
        block.reason = BlockReason::Forgotten;
        block.record = record;
        next.blocked.push_back(block);

        // The modules that leave with the Base and are registered under no
        // other one: their rooms are cleared later, by the same clean-up as
        // after a release -- written down in this very file write. A
        // clean-up is filed under a generation; a list removal has its
        // record, which is one for a Base with a credential.
        std::array<uint8_t, 16> cleanup_generation{};
        ParseHex(record, cleanup_generation);
        size_t known_modules = 0;
        NoteCleanupLocked(next, forget.record.forget_id, eui, cleanup_generation, view, now_ms, &known_modules);
        size_t noted = 0;
        for (const Cleanup &c : next.cleanups) {
            if (c.removal_id == forget.record.forget_id) {
                noted = c.modules.size();
            }
        }
        // What was known under it: seen in the Registry, or written down for
        // a Base that has not reported since this process started.
        forget.record.affected_module_count = std::max(module_count, static_cast<int>(known_modules));
        next.forgets.push_back(forget);

        // Decided, stored and applied as one step of the Registry: when the
        // Base is heard from at this moment nothing is written; when the
        // write fails nothing is blocked or erased.
        const auto steady = config_.steady_now ? config_.steady_now() : std::chrono::steady_clock::now();
        const auto result = registry_.block_and_remove_power_base_if_offline(
            eui, [&]() { return StoreLocked(next); }, nullptr, steady);
        if (result == ingest::Registry::OfflineRemoval::Online) {
            return ForgetOutcome::Online;
        }
        if (result == ingest::Registry::OfflineRemoval::CommitFailed) {
            return ForgetOutcome::Unavailable;
        }
        state_ = std::move(next);
        unmanaged_records_.erase(std::remove_if(unmanaged_records_.begin(), unmanaged_records_.end(),
                                                [&](const std::pair<ingest::Uuid, std::string> &e) {
                                                    return e.first == eui;
                                                }),
                                 unmanaged_records_.end());
        nonces_.erase(std::remove_if(nonces_.begin(), nonces_.end(),
                                     [&](const HubNonce &n) {
                                         return n.eui == eui && !(keep && n.generation == kept_generation);
                                     }),
                      nonces_.end());
        // A Base that was listed from its credential alone left no trace in
        // the Registry: its readers are told all the same.
        NoteListChangeLocked();
        std::fprintf(stdout,
                     "pogohome_api: power base list removal %s: device %s took Power Base %s out of this hub's list "
                     "(offline; the board was NOT erased and nothing says it was); %zu module(s) noted for the "
                     "settings clean-up%s%s\n",
                     ShortId(forget.record.forget_id).c_str(), device_id.c_str(), eui64.c_str(), noted,
                     forget.record.cancelled_removal_id.empty() ? "" : "; its pending removal was cancelled",
                     forget.record.kept_removal_id.empty() ? ""
                                                           : "; a removal whose command may be with the board was left "
                                                             "as it is");
        for (const Forget &f : state_.forgets) {
            if (f.record.forget_id == forget.record.forget_id) {
                out = CopyForgetLocked(f);
            }
        }
        removed = true;
    }
    if (removed) {
        WakeCleanupWorker();
    }
    return ForgetOutcome::Removed;
}

bool PowerBaseManagement::GetForget(const std::string &forget_id, ForgetRecord &out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Forget &f : state_.forgets) {
        if (ConstantTimeEquals(f.record.forget_id, forget_id)) {
            out = CopyForgetLocked(f);
            return true;
        }
    }
    return false;
}

std::vector<ForgetRecord> PowerBaseManagement::ListForgets()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ListForgetsLocked();
}

std::vector<ForgetRecord> PowerBaseManagement::ListForgetsLocked() const
{
    const int64_t now_ms = NowUnixMs();
    std::vector<ForgetRecord> out;
    for (const Forget &f : state_.forgets) {
        ForgetRecord copy = CopyForgetLocked(f);
        if (copy.settings_cleanup_pending || ForgetExplainsBlockLocked(state_, f) ||
            now_ms - f.record.created_at_unix_ms <= config_.retain_finished.count()) {
            out.push_back(std::move(copy));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const ForgetRecord &a, const ForgetRecord &b) {
        return a.created_at_unix_ms > b.created_at_unix_ms;
    });
    return out;
}

std::vector<PowerBaseDeparture> PowerBaseManagement::ListDepartures() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return available_ ? ListDeparturesLocked() : std::vector<PowerBaseDeparture>();
}

std::vector<PowerBaseDeparture> PowerBaseManagement::ListDeparturesLocked() const
{
    // The blocks themselves: a Base is blocked from the moment its
    // registration ended until a new one becomes active, and never listed in
    // between. No retention applies and none is needed -- there is at most
    // one entry per blocked Base (kMaxBlockedPowerBases), and it leaves with
    // the block.
    std::vector<PowerBaseDeparture> out;
    out.reserve(state_.blocked.size());
    for (const Blocked &b : state_.blocked) {
        PowerBaseDeparture d;
        d.power_base_uuid = FormatUuidHex(b.eui);
        d.record = b.record;
        d.reason = b.reason == BlockReason::Released ? "removed" : b.reason == BlockReason::Forgotten ? "forgotten" : "";
        d.since_unix_ms = b.since_unix_ms;
        out.push_back(std::move(d));
    }
    return out;
}

// ---- clean-ups of the shared settings ----

size_t PowerBaseManagement::PendingSettingsCleanups() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.cleanups.size();
}

size_t PowerBaseManagement::ProcessSettingsCleanups()
{
    // One pass at a time: the worker and a direct caller never interleave.
    std::lock_guard<std::mutex> run(cleanup_run_mutex_);

    struct Due {
        std::string removal_id;
        std::vector<RoomCleanupTarget> targets;
        int attempts = 0;
    };
    std::vector<Due> due;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!available_) {
            return 0;
        }
        // Where every module is NOW. One that is listed under some Base at
        // this moment -- it moved before the removal, was docked elsewhere
        // since, or its Base was enrolled again -- keeps its room: that
        // assignment is not the removed Base's any more. It leaves every
        // clean-up for good (ObserveLocked()). Also what keeps the members
        // list current while no device is reading snapshots.
        if (ObserveLocked(registry_.view())) {
            registry_.notify_external_change();
        }
        FlushObservationLocked();
        const int64_t now_ms = NowUnixMs();
        for (const Cleanup &c : state_.cleanups) {
            if (c.next_attempt_unix_ms > now_ms) {
                continue;
            }
            // After a restart the Registry starts empty: wait until the Power
            // Bases have had time to report where every module is now.
            if (c.from_file && now_ms - loaded_at_unix_ms_ < config_.cleanup_startup_grace.count()) {
                continue;
            }
            Due d;
            d.removal_id = c.removal_id;
            d.attempts = c.attempts;
            for (size_t i = 0; i < c.modules.size(); i++) {
                RoomCleanupTarget target;
                target.module_uuid = FormatUuidHex(c.modules[i]);
                target.room_known = i < c.rooms.size();
                if (target.room_known) {
                    target.room_id = c.rooms[i];
                }
                d.targets.push_back(std::move(target));
            }
            due.push_back(std::move(d));
        }
    }

    for (const Due &d : due) {
        // What the settings write is tied to (room_cleanup.hpp): the section
        // in which the write itself is made. This class's lock and then the
        // Registry's are taken and KEPT; the Registry copy is
        // made under its lock, the modules of this attempt are decided from
        // that copy, and the settings are written before either lock is given
        // up. No report can be applied in between, so a module that is the
        // clean-up's when the write starts is listed under no Power Base
        // until the write has returned -- whether or not anyone saved the
        // settings meanwhile. (A room a device gives it later makes no
        // difference to this write: it is conditional on the revision that
        // was read.) Order of the locks: this class, the Registry, the
        // settings store -- the same as everywhere else; nothing here is
        // called back from under the settings store's lock.
        // There is no separate question before it: an answer given outside
        // the section could only be out of date by the time of the write.
        const RoomCleanupGuard still_owed(
            nullptr, [this, &d](const std::vector<std::string> &modules, const RoomCleanupGuard::Write &write) {
                std::lock_guard<std::mutex> lock(mutex_);
                bool cleanup_ended = false;
                registry_.hold([&](const ingest::RegistryView &view) {
                    // Nothing in here may call the Registry: its lock is held.
                    cleanup_ended = ObserveLocked(view);
                    std::vector<std::string> owed;
                    for (const Cleanup &c : state_.cleanups) {
                        if (c.removal_id != d.removal_id) {
                            continue;
                        }
                        for (const std::string &hex : modules) {
                            ingest::Uuid module{};
                            if (ParseHex(hex, module) &&
                                std::find(c.modules.begin(), c.modules.end(), module) != c.modules.end()) {
                                owed.push_back(hex);
                            }
                        }
                    }
                    if (!owed.empty() && write) {
                        write(owed);
                    }
                });
                if (cleanup_ended) {
                    registry_.notify_external_change(); // after the Registry's lock was given up
                }
            });
        bool done = false;
        if (config_.unassign_rooms) {
            // The slow part (a settings file write, possibly several tries
            // against other writers): no lock of this class is held.
            done = config_.unassign_rooms(d.targets, still_owed);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t now_ms = NowUnixMs();
        State next = state_;
        auto it = std::find_if(next.cleanups.begin(), next.cleanups.end(),
                               [&](const Cleanup &c) { return c.removal_id == d.removal_id; });
        if (it == next.cleanups.end()) {
            // Every module it named was listed under a Base again while the
            // settings were being written: there was nothing left to clear,
            // and ObserveLocked() ended it.
            FlushObservationLocked();
            registry_.notify_external_change();
            continue;
        }
        const size_t cleared = it->modules.size();
        if (done) {
            next.cleanups.erase(it);
        } else {
            it->attempts = d.attempts + 1;
            int64_t wait = config_.cleanup_retry_min.count();
            for (int i = 1; i < it->attempts && wait < config_.cleanup_retry_max.count(); i++) {
                wait *= 2;
            }
            it->next_attempt_unix_ms = now_ms + std::min<int64_t>(wait, config_.cleanup_retry_max.count());
        }
        PruneFinishedLocked(next, now_ms, config_.retain_finished.count());
        if (!StoreLocked(next)) {
            // The clean-up may have been carried out, but that it was is not
            // on disk: it stays pending and is done again (it is idempotent).
            // The retry time moves on in RAM so a broken disk is not hammered.
            for (Cleanup &c : state_.cleanups) {
                if (c.removal_id == d.removal_id) {
                    c.attempts = d.attempts + 1;
                    int64_t wait = config_.cleanup_retry_min.count();
                    for (int i = 1; i < c.attempts && wait < config_.cleanup_retry_max.count(); i++) {
                        wait *= 2;
                    }
                    c.next_attempt_unix_ms = now_ms + std::min<int64_t>(wait, config_.cleanup_retry_max.count());
                }
            }
            std::fprintf(stderr,
                         "pogohome_api: power base removal %s: settings clean-up %s, but that could not be stored -- "
                         "it stays pending\n",
                         ShortId(d.removal_id).c_str(), done ? "done" : "failed");
            continue;
        }
        state_ = std::move(next);
        registry_.notify_external_change();
        if (done) {
            std::fprintf(stdout,
                         "pogohome_api: power base removal %s: settings clean-up done (%zu module(s) without a room "
                         "now)\n",
                         ShortId(d.removal_id).c_str(), cleared);
        } else {
            std::fprintf(stderr,
                         "pogohome_api: power base removal %s: settings clean-up failed (attempt %d) -- retried later; "
                         "the removal itself is not affected\n",
                         ShortId(d.removal_id).c_str(), d.attempts + 1);
        }
    }
    return PendingSettingsCleanups();
}

// ---- what a snapshot says of management ----

bool PowerBaseManagement::DescribeForSnapshot(ingest::RegistryView &view, Description &out)
{
    out = Description();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!available_) {
        return false;
    }
    // The caller copied the Registry before it came here. Between that copy
    // and this lock another request may have taken a Base out of the list --
    // a release, a list removal: the copy would then show that Base next to
    // the record that took it out. Such a copy (and one older than the last
    // copy that was taken in) is replaced by one made now. A Base only leaves
    // the Registry under this lock, so from here to the end of this function
    // the copy and state_ belong together.
    if (view.generation < list_changed_generation_ || (observed_any_ && view.generation < observed_generation_)) {
        view = registry_.view();
    }
    // What a snapshot shows is written down first: a module is never shown
    // under a Base that the members list (and every clean-up) does not know
    // of.
    if (ObserveLocked(view)) {
        registry_.notify_external_change();
    }
    FlushObservationLocked();

    out.managed_power_bases = ManagedLocked(false);
    out.self_confirming_power_bases = ManagedLocked(true);
    out.removals = ListRemovalsLocked();
    out.forgets = ListForgetsLocked();
    out.departures = ListDeparturesLocked();
    // The registration behind every listed Base.
    std::vector<std::string> listed = out.managed_power_bases;
    auto add = [&](const ingest::Uuid &uuid) {
        std::string hex = FormatUuidHex(uuid);
        if (std::find(listed.begin(), listed.end(), hex) == listed.end()) {
            listed.push_back(std::move(hex));
        }
    };
    for (const ingest::PowerBaseState &b : view.power_bases) {
        add(b.power_base_uuid);
    }
    for (const ingest::NodeState &n : view.nodes) {
        add(n.power_base_uuid);
    }
    for (const std::string &hex : listed) {
        ingest::Uuid eui{};
        if (!ParseHex(hex, eui) || IsZero(eui)) {
            continue;
        }
        std::string record = RecordLocked(eui, true);
        if (!record.empty()) {
            out.records.emplace_back(hex, std::move(record));
        }
    }
    // What a removal request for each managed Base has to name besides.
    for (const Credential &c : state_.credentials) {
        if (c.state == CredentialState::Active && !IsBlockedLocked(state_, c.eui)) {
            out.removal_serials.emplace_back(FormatUuidHex(c.eui), c.removal_serial);
        }
    }
    return true;
}

void PowerBaseManagement::WakeCleanupWorker()
{
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        worker_wake_ = true;
    }
    worker_cv_.notify_all();
}

void PowerBaseManagement::CleanupWorkerLoop(std::chrono::milliseconds interval)
{
    std::unique_lock<std::mutex> lock(worker_mutex_);
    while (!worker_stop_) {
        worker_cv_.wait_for(lock, interval, [&] { return worker_stop_ || worker_wake_; });
        if (worker_stop_) {
            return;
        }
        worker_wake_ = false;
        lock.unlock();
        (void)ProcessSettingsCleanups();
        lock.lock();
    }
}

void PowerBaseManagement::StartCleanupWorker(std::chrono::milliseconds interval)
{
    std::lock_guard<std::mutex> lock(worker_mutex_);
    if (worker_.joinable() || worker_stop_) {
        return;
    }
    worker_ = std::thread([this, interval] { CleanupWorkerLoop(interval); });
}

// ---- the HTTP API ----

bool PowerBaseManagement::RemovalBinding(const std::string &eui64, std::string &record, uint32_t &removal_serial)
{
    record.clear();
    removal_serial = 0;
    ingest::Uuid eui{};
    if (!ParseHex(eui64, eui) || IsZero(eui)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!available_) {
        return false;
    }
    for (const Credential &c : state_.credentials) {
        if (c.eui == eui && c.state == CredentialState::Active) {
            record = RecordLocked(eui, false);
            removal_serial = c.removal_serial;
            return !record.empty();
        }
    }
    return false;
}

RemovalRequestOutcome PowerBaseManagement::RequestRemoval(const std::string &device_id, const std::string &eui64,
                                                          const std::string &record, uint32_t removal_serial,
                                                          const std::string &request_id, RemovalJob &out)
{
    out = RemovalJob();
    ingest::Uuid eui{};
    if (!ParseHex(eui64, eui) || IsZero(eui)) {
        return RemovalRequestOutcome::UnknownPowerBase;
    }
    if (!IsLowerHex(request_id, 32)) {
        return RemovalRequestOutcome::RequestConflict;
    }
    // Asked before the lock is taken (see the header): an enrollment that
    // starts right after this still ends consistently -- its new generation
    // supersedes this job when it becomes active.
    const bool enrolling = config_.enrollment_in_progress && config_.enrollment_in_progress(eui64);
    // Also before the lock: the Registry has its own.
    ingest::RegistryView view = registry_.view();
    int module_count = 0;
    bool known = std::any_of(view.power_bases.begin(), view.power_bases.end(),
                             [&](const ingest::PowerBaseState &b) { return b.power_base_uuid == eui; });
    for (const ingest::NodeState &n : view.nodes) {
        if (n.power_base_uuid == eui) {
            module_count++;
            known = true;
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!available_) {
        return RemovalRequestOutcome::Unavailable;
    }
    const int64_t now_ms = NowUnixMs();
    // The same request again -- its answer was lost, or it arrives twice or
    // late: the job it created, as that is now. Never a second one, and never
    // one of another registration.
    for (const Job &j : state_.jobs) {
        if (!j.job.request_id.empty() && j.job.request_id == request_id) {
            if (j.eui != eui) {
                return RemovalRequestOutcome::RequestConflict;
            }
            out = CopyJobLocked(j);
            return RemovalRequestOutcome::Existing;
        }
    }
    // A removal of the very registration the request names is under way,
    // whoever asked for it: that one. (Also when it was released already and
    // the Base has left the list.)
    Job *open = FindOpenJobLocked(eui);
    if (open != nullptr && IsLowerHex(record, 32) && ConstantTimeEquals(open->job.generation, record)) {
        out = CopyJobLocked(*open);
        return RemovalRequestOutcome::Existing;
    }
    Credential *active = nullptr;
    for (Credential &c : state_.credentials) {
        if (c.eui == eui && c.state == CredentialState::Active) {
            active = &c;
        }
    }
    if (active == nullptr) {
        return known ? RemovalRequestOutcome::Unsupported : RemovalRequestOutcome::UnknownPowerBase;
    }
    // The registration the device confirmed must be the one listed now. A
    // request made for an earlier registration -- the Base was removed and
    // registered again while the request was on its way, or before it was sent
    // once more -- is not about this one, and nothing here makes it so.
    const std::string listed = RecordLocked(eui, false);
    if (listed.empty() || !IsLowerHex(record, 32) || !ConstantTimeEquals(listed, record)) {
        return RemovalRequestOutcome::StaleRecord;
    }
    if (open != nullptr) {
        // What is listed is what the device confirmed, and its removal is
        // under way already.
        out = CopyJobLocked(*open);
        return RemovalRequestOutcome::Existing;
    }
    if (enrolling) {
        return RemovalRequestOutcome::EnrollmentInProgress;
    }
    // A removal was asked of this registration since the device read the
    // list, and that job is over (it would have been found above): this
    // request was made before it. A request that created a job once is here,
    // too, when it comes again after that job was dropped from the list -- the
    // serial it carries was spent by its own job.
    if (removal_serial != active->removal_serial) {
        return RemovalRequestOutcome::StaleRecord;
    }
    if (active->removal_serial == UINT32_MAX) {
        return RemovalRequestOutcome::Unavailable; // never reached in practice; nothing wraps around
    }
    const auto waiting_jobs = static_cast<size_t>(std::count_if(state_.jobs.begin(), state_.jobs.end(), [](const Job &j) {
        return j.job.state == RemovalState::Pending || j.job.state == RemovalState::Sent ||
               j.job.state == RemovalState::Accepted;
    }));
    if (waiting_jobs >= kMaxActiveRemovals) {
        return RemovalRequestOutcome::TooMany;
    }
    // The block this removal ends in is reserved now. Without a free slot the
    // request is refused before anything is stored: no job, and therefore no
    // command for the board, ever comes of it. Nothing is evicted to make
    // room -- an existing block only ends with a new enrollment of its board.
    if (UsedBlockSlots(state_) >= kMaxBlockedPowerBases) {
        std::fprintf(stderr,
                     "pogohome_api: power base removal refused for %s: all %zu block slots are in use (blocked Power "
                     "Bases and removals under way)\n",
                     eui64.c_str(), kMaxBlockedPowerBases);
        return RemovalRequestOutcome::CapacityFull;
    }

    Job job;
    job.eui = eui;
    job.generation = active->generation;
    job.job.removal_id = RandomHex(16);
    if (job.job.removal_id.empty() || !SecureRandomBytes(job.command_id.data(), job.command_id.size()) ||
        IsZero(job.command_id)) {
        return RemovalRequestOutcome::Unavailable;
    }
    job.job.power_base_uuid = eui64;
    job.job.generation = ToHex(active->generation);
    job.job.state = RemovalState::Pending;
    job.job.created_at_unix_ms = now_ms;
    job.job.updated_at_unix_ms = now_ms;
    job.job.requested_by_device_id = device_id;
    job.job.affected_module_count = module_count;
    job.job.request_id = request_id;
    job.progress_at_unix_ms = now_ms;

    State next = state_;
    PruneFinishedLocked(next, now_ms, config_.retain_finished.count());
    next.jobs.push_back(job);
    // The job and the serial it spent, in one stored change: either both are
    // in the file or neither is.
    for (Credential &c : next.credentials) {
        if (c.eui == eui && c.state == CredentialState::Active) {
            c.removal_serial++;
        }
    }
    if (!StoreLocked(next)) {
        return RemovalRequestOutcome::Unavailable;
    }
    state_ = std::move(next);
    registry_.notify_external_change();
    std::fprintf(stdout, "pogohome_api: power base removal %s: device %s asked to unenroll Power Base %s (stored)\n",
                 ShortId(job.job.removal_id).c_str(), device_id.c_str(), eui64.c_str());
    out = CopyJobLocked(job);
    return RemovalRequestOutcome::Created;
}

bool PowerBaseManagement::GetRemoval(const std::string &removal_id, RemovalJob &out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Job &j : state_.jobs) {
        if (ConstantTimeEquals(j.job.removal_id, removal_id)) {
            out = CopyJobLocked(j);
            return true;
        }
    }
    return false;
}

std::vector<RemovalJob> PowerBaseManagement::ListRemovals()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ListRemovalsLocked();
}

std::vector<RemovalJob> PowerBaseManagement::ListRemovalsLocked() const
{
    const int64_t now_ms = NowUnixMs();
    std::vector<RemovalJob> open;
    std::vector<RemovalJob> finished;
    for (const Job &j : state_.jobs) {
        if (!IsFinalRemovalState(j.job.state)) {
            open.push_back(CopyJobLocked(j));
        } else if (now_ms - j.job.updated_at_unix_ms <= config_.retain_finished.count()) {
            finished.push_back(CopyJobLocked(j));
        }
    }
    std::stable_sort(open.begin(), open.end(), [](const RemovalJob &a, const RemovalJob &b) {
        return a.created_at_unix_ms < b.created_at_unix_ms;
    });
    std::stable_sort(finished.begin(), finished.end(), [](const RemovalJob &a, const RemovalJob &b) {
        return a.updated_at_unix_ms > b.updated_at_unix_ms;
    });
    open.insert(open.end(), finished.begin(), finished.end());
    return open;
}

RemovalCancelOutcome PowerBaseManagement::CancelRemoval(const std::string &removal_id, RemovalJob &out)
{
    out = RemovalJob();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!available_) {
        return RemovalCancelOutcome::Unavailable;
    }
    Job *job = nullptr;
    for (Job &j : state_.jobs) {
        if (ConstantTimeEquals(j.job.removal_id, removal_id)) {
            job = &j;
        }
    }
    if (job == nullptr) {
        return RemovalCancelOutcome::NotFound;
    }
    if (job->job.state != RemovalState::Pending) {
        // Sent: the command may be on the board already. Nothing a cancel
        // could promise.
        out = CopyJobLocked(*job);
        return RemovalCancelOutcome::NotCancellable;
    }
    const int64_t now_ms = NowUnixMs();
    State next = state_;
    for (Job &j : next.jobs) {
        if (j.job.removal_id == job->job.removal_id) {
            j.job.state = RemovalState::Cancelled;
            j.job.updated_at_unix_ms = now_ms;
            j.progress_at_unix_ms = now_ms;
        }
    }
    const std::string id = job->job.removal_id;
    if (!StoreLocked(next)) {
        return RemovalCancelOutcome::Unavailable;
    }
    state_ = std::move(next);
    registry_.notify_external_change();
    std::fprintf(stdout, "pogohome_api: power base removal %s: cancelled before the command was sent\n",
                 ShortId(id).c_str());
    for (const Job &j : state_.jobs) {
        if (j.job.removal_id == id) {
            out = CopyJobLocked(j);
        }
    }
    return RemovalCancelOutcome::Cancelled;
}

std::string RemovalJobJson(const RemovalJob &job)
{
    std::string out = "{";
    AppendKeyString(out, "removal_id", job.removal_id);
    AppendKeyString(out, "power_base_uuid", job.power_base_uuid);
    AppendKeyString(out, "generation", job.generation);
    AppendKeyString(out, "state", RemovalStateName(job.state));
    out += "\"result_unknown\":";
    out += job.result_unknown ? "true," : "false,";
    out += "\"cancellable\":";
    out += job.cancellable ? "true," : "false,";
    AppendKeyInt(out, "created_at_unix_ms", job.created_at_unix_ms);
    AppendKeyInt(out, "updated_at_unix_ms", job.updated_at_unix_ms);
    out += "\"completed_at_unix_ms\":";
    out += job.state == RemovalState::Completed ? std::to_string(job.completed_at_unix_ms) : std::string("null");
    out += ',';
    AppendKeyString(out, "requested_by_device_id", job.requested_by_device_id);
    AppendKeyInt(out, "affected_module_count", job.affected_module_count);
    out += "\"settings_cleanup_pending\":";
    out += job.settings_cleanup_pending ? "true" : "false";
    // Only while something is still expected: a final job has no such field.
    if (job.erase_confirmation != EraseConfirmation::None) {
        out += ',';
        AppendKeyString(out, "erase_confirmation", EraseConfirmationName(job.erase_confirmation), false);
    }
    // Only on a job whose Base was taken out of this hub's list while its
    // command may be with the board: the list is settled, the board is not.
    // Every other job has no such field.
    if (job.power_base_list_removed) {
        out += ",\"list_removed\":true";
    }
    // The request that created it, when this hub knows it: how the device
    // that asked recognises its own request's job. No secret -- repeating it
    // only ever gets this job again.
    if (!job.request_id.empty()) {
        out += ',';
        AppendKeyString(out, "request_id", job.request_id, false);
    }
    out += '}';
    return out;
}

std::string ForgetRecordJson(const ForgetRecord &record)
{
    auto id_or_null = [](std::string &out, const char *key, const std::string &id) {
        out += '"';
        out += key;
        out += "\":";
        if (id.empty()) {
            out += "null";
        } else {
            AppendJsonStringLiteral(out, id);
        }
        out += ',';
    };
    std::string out = "{";
    AppendKeyString(out, "forget_id", record.forget_id);
    AppendKeyString(out, "request_id", record.request_id);
    AppendKeyString(out, "power_base_uuid", record.power_base_uuid);
    AppendKeyString(out, "record", record.record);
    // What this is, in so many words: the hub's list changed. The board was
    // not erased, and no field of this object ever says it was.
    AppendKeyString(out, "state", "removed_from_hub");
    out += "\"managed\":";
    out += record.managed ? "true," : "false,";
    AppendKeyInt(out, "created_at_unix_ms", record.created_at_unix_ms);
    AppendKeyString(out, "requested_by_device_id", record.requested_by_device_id);
    AppendKeyInt(out, "affected_module_count", record.affected_module_count);
    id_or_null(out, "cancelled_removal_id", record.cancelled_removal_id);
    id_or_null(out, "kept_removal_id", record.kept_removal_id);
    out += "\"settings_cleanup_pending\":";
    out += record.settings_cleanup_pending ? "true" : "false";
    out += '}';
    return out;
}

std::string DeparturesJson(const std::vector<PowerBaseDeparture> &departures)
{
    std::string out = "[";
    for (size_t i = 0; i < departures.size(); i++) {
        const PowerBaseDeparture &d = departures[i];
        out += i == 0 ? "{" : ",{";
        AppendKeyString(out, "power_base_uuid", d.power_base_uuid);
        // Left out, not null, when this hub does not know them.
        if (!d.record.empty()) {
            AppendKeyString(out, "record", d.record);
        }
        if (!d.reason.empty()) {
            AppendKeyString(out, "reason", d.reason);
        }
        AppendKeyInt(out, "since_unix_ms", d.since_unix_ms, false);
        out += '}';
    }
    out += ']';
    return out;
}

std::string ForgetListJson(const std::vector<ForgetRecord> &records)
{
    std::string out = "{\"forgets\":[";
    for (size_t i = 0; i < records.size(); i++) {
        if (i != 0) {
            out += ',';
        }
        out += ForgetRecordJson(records[i]);
    }
    out += "]}";
    return out;
}

std::string RemovalListJson(const std::vector<RemovalJob> &jobs)
{
    std::string out = "{\"removals\":[";
    for (size_t i = 0; i < jobs.size(); i++) {
        if (i != 0) {
            out += ',';
        }
        out += RemovalJobJson(jobs[i]);
    }
    out += "]}";
    return out;
}

} // namespace pogohome_api
