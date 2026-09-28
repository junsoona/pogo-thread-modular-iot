#include "pogohome_api/device_auth.hpp"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "pogohome_api/atomic_file.hpp"
#include "pogohome_api/crypto_util.hpp"
#include "pogohome_api/home_settings.hpp"
#include "pogohome_api/json_value.hpp"

namespace pogohome_api {

namespace {

constexpr const char kCodeAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr size_t kCodeLength = 12;
constexpr size_t kMaxAuthFileBytes = 64 * 1024;
constexpr size_t kTokenBytes = 32;
// A token's last-use time is written back at most this often, so normal use
// does not rewrite devices.json on every request.
constexpr int64_t kLastUsedWriteIntervalMs = 10 * 60 * 1000;
constexpr const char kPairProtocol[] = "pogohome-pair-v1";

class FileLock {
public:
    FileLock(const std::string &path, bool exclusive)
    {
        fd_ = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ < 0) {
            error_ = "cannot open " + path + ": " + std::strerror(errno);
            return;
        }
        while (flock(fd_, exclusive ? LOCK_EX : LOCK_SH) != 0) {
            if (errno == EINTR) {
                continue;
            }
            error_ = "cannot lock " + path + ": " + std::strerror(errno);
            close(fd_);
            fd_ = -1;
            return;
        }
    }
    ~FileLock()
    {
        if (fd_ >= 0) {
            flock(fd_, LOCK_UN);
            close(fd_);
        }
    }
    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;

    bool ok() const { return fd_ >= 0; }
    const std::string &error() const { return error_; }

private:
    int fd_ = -1;
    std::string error_;
};

struct PairingRecord {
    std::string code;
    int64_t created_at_unix_ms = 0;
    int64_t expires_at_unix_ms = 0;
    int failed_attempts = 0;
};

enum class LoadResult { Ok, Missing, Error };

bool ReadInt(const JsonValue &object, const char *key, int64_t &out)
{
    const JsonValue *v = object.Find(key);
    if (v == nullptr || !v->IsInteger()) {
        return false;
    }
    out = v->int_value;
    return true;
}

bool ReadString(const JsonValue &object, const char *key, std::string &out)
{
    const JsonValue *v = object.Find(key);
    if (v == nullptr || !v->IsString()) {
        return false;
    }
    out = v->string_value;
    return true;
}

bool CheckHeader(const JsonValue &root, const char *format)
{
    std::string f;
    int64_t schema = 0;
    return ReadString(root, "format", f) && f == format && ReadInt(root, "schema_version", schema) && schema == 1;
}

LoadResult LoadPairing(const std::string &path, PairingRecord &record, std::string &error)
{
    std::string contents;
    ReadFileResult read = ReadWholeFile(path, kMaxAuthFileBytes, contents, error);
    if (read == ReadFileResult::Missing) {
        return LoadResult::Missing;
    }
    if (read == ReadFileResult::Error) {
        return LoadResult::Error;
    }
    JsonValue root;
    std::string parse_error;
    int64_t failed = 0;
    if (!ParseJson(contents, root, parse_error) || !CheckHeader(root, "pogohome-pairing") ||
        !ReadString(root, "code", record.code) || !ReadInt(root, "created_at_unix_ms", record.created_at_unix_ms) ||
        !ReadInt(root, "expires_at_unix_ms", record.expires_at_unix_ms) ||
        !ReadInt(root, "failed_attempts", failed) || failed < 0 || failed > kMaxPairingFailures) {
        error = path + " is damaged (cancel pairing to remove it)";
        return LoadResult::Error;
    }
    std::string normalized;
    if (!NormalizePairingCode(record.code, normalized) || normalized != record.code) {
        error = path + " is damaged (cancel pairing to remove it)";
        return LoadResult::Error;
    }
    record.failed_attempts = static_cast<int>(failed);
    return LoadResult::Ok;
}

bool SavePairing(const std::string &path, const PairingRecord &record, std::string &error)
{
    std::string out = "{\"format\":\"pogohome-pairing\",\"schema_version\":1,\"code\":";
    AppendJsonStringLiteral(out, record.code);
    out += ",\"created_at_unix_ms\":" + std::to_string(record.created_at_unix_ms);
    out += ",\"expires_at_unix_ms\":" + std::to_string(record.expires_at_unix_ms);
    out += ",\"failed_attempts\":" + std::to_string(record.failed_attempts);
    out += "}\n";
    return WriteFileAtomically(path, out, error);
}

bool RemoveFile(const std::string &path, std::string &error)
{
    if (unlink(path.c_str()) != 0 && errno != ENOENT) {
        error = "cannot remove " + path + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

LoadResult LoadDevices(const std::string &path, std::vector<DeviceRecord> &devices, std::string &error)
{
    devices.clear();
    std::string contents;
    ReadFileResult read = ReadWholeFile(path, kMaxAuthFileBytes, contents, error);
    if (read == ReadFileResult::Missing) {
        return LoadResult::Missing;
    }
    if (read == ReadFileResult::Error) {
        return LoadResult::Error;
    }
    JsonValue root;
    std::string parse_error;
    const std::string damaged = path + " is damaged; registered devices cannot be checked until it is repaired";
    if (!ParseJson(contents, root, parse_error) || !CheckHeader(root, "pogohome-devices")) {
        error = damaged;
        return LoadResult::Error;
    }
    const JsonValue *list = root.Find("devices");
    if (list == nullptr || !list->IsArray() || list->array_items.size() > kMaxRegisteredDevices) {
        error = damaged;
        return LoadResult::Error;
    }
    for (const auto &item : list->array_items) {
        DeviceRecord d;
        if (!ReadString(item, "id", d.id) || !IsLowerHex(d.id, 16) || !ReadString(item, "name", d.name) ||
            !ReadString(item, "token_sha256", d.token_sha256) || !IsLowerHex(d.token_sha256, 64) ||
            !ReadInt(item, "created_at_unix_ms", d.created_at_unix_ms) ||
            !ReadInt(item, "last_used_at_unix_ms", d.last_used_at_unix_ms)) {
            error = damaged;
            devices.clear();
            return LoadResult::Error;
        }
        devices.push_back(std::move(d));
    }
    return LoadResult::Ok;
}

bool SaveDevices(const std::string &path, const std::vector<DeviceRecord> &devices, std::string &error)
{
    std::string out = "{\"format\":\"pogohome-devices\",\"schema_version\":1,\"devices\":[";
    for (size_t i = 0; i < devices.size(); i++) {
        const DeviceRecord &d = devices[i];
        if (i > 0) {
            out += ',';
        }
        out += "{\"id\":";
        AppendJsonStringLiteral(out, d.id);
        out += ",\"name\":";
        AppendJsonStringLiteral(out, d.name);
        out += ",\"token_sha256\":";
        AppendJsonStringLiteral(out, d.token_sha256);
        out += ",\"created_at_unix_ms\":" + std::to_string(d.created_at_unix_ms);
        out += ",\"last_used_at_unix_ms\":" + std::to_string(d.last_used_at_unix_ms);
        out += '}';
    }
    out += "]}\n";
    return WriteFileAtomically(path, out, error);
}

bool IsBase64Url(const std::string &s)
{
    for (char c : s) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string GeneratePairingCode()
{
    unsigned char bytes[8];
    if (!SecureRandomBytes(bytes, sizeof(bytes))) {
        return std::string();
    }
    uint64_t bits = 0;
    for (unsigned char b : bytes) {
        bits = (bits << 8) | b;
    }
    std::string code;
    for (size_t i = 0; i < kCodeLength; i++) {
        code += kCodeAlphabet[bits & 0x1F];
        bits >>= 5;
    }
    return code;
}

std::string FormatPairingCode(const std::string &code)
{
    if (code.size() != kCodeLength) {
        return code;
    }
    return code.substr(0, 4) + "-" + code.substr(4, 4) + "-" + code.substr(8, 4);
}

bool NormalizePairingCode(const std::string &input, std::string &code)
{
    code.clear();
    for (char c : input) {
        if (c == '-' || c == ' ') {
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        if (c == 'O') {
            c = '0';
        } else if (c == 'I' || c == 'L') {
            c = '1';
        }
        if (std::strchr(kCodeAlphabet, c) == nullptr || c == '\0') {
            code.clear();
            return false;
        }
        code += c;
        if (code.size() > kCodeLength) {
            code.clear();
            return false;
        }
    }
    if (code.size() != kCodeLength) {
        code.clear();
        return false;
    }
    return true;
}

std::string PairingClientProof(const std::string &code, const std::string &hub_id, const std::string &cert_sha256_hex,
                               const std::string &client_nonce_hex, const std::string &device_name)
{
    std::string message = std::string(kPairProtocol) + "\nclient\n" + hub_id + "\n" + cert_sha256_hex + "\n" +
                          client_nonce_hex + "\n" + Sha256Hex(device_name);
    return HmacSha256Hex(code, message);
}

std::string PairingServerProof(const std::string &code, const std::string &hub_id, const std::string &cert_sha256_hex,
                               const std::string &client_nonce_hex, const std::string &device_id,
                               const std::string &token)
{
    std::string message = std::string(kPairProtocol) + "\nserver\n" + hub_id + "\n" + cert_sha256_hex + "\n" +
                          client_nonce_hex + "\n" + device_id + "\n" + Sha256Hex(token);
    return HmacSha256Hex(code, message);
}

DeviceAuth::DeviceAuth(std::string directory) : directory_(std::move(directory)) {}

bool DeviceAuth::CreatePairingCode(int64_t now_unix_ms, int64_t ttl_ms, std::string &code,
                                   int64_t &expires_at_unix_ms, std::string &error)
{
    if (ttl_ms <= 0 || ttl_ms > kMaxPairingTtlMs) {
        error = "pairing code lifetime must be between 1 ms and 60 minutes";
        return false;
    }
    FileLock lock(PathOf(kAuthLockFileName), true);
    if (!lock.ok()) {
        error = lock.error();
        return false;
    }
    PairingRecord record;
    record.code = GeneratePairingCode();
    if (record.code.empty()) {
        error = "random number generator failed";
        return false;
    }
    record.created_at_unix_ms = now_unix_ms;
    record.expires_at_unix_ms = now_unix_ms + ttl_ms;
    if (!SavePairing(PathOf(kPairingFileName), record, error)) {
        return false;
    }
    code = record.code;
    expires_at_unix_ms = record.expires_at_unix_ms;
    return true;
}

bool DeviceAuth::CancelPairing(std::string &error)
{
    FileLock lock(PathOf(kAuthLockFileName), true);
    if (!lock.ok()) {
        error = lock.error();
        return false;
    }
    return RemoveFile(PathOf(kPairingFileName), error);
}

bool DeviceAuth::GetPairingStatus(int64_t now_unix_ms, PairingStatus &status, std::string &error)
{
    status = PairingStatus();
    FileLock lock(PathOf(kAuthLockFileName), false);
    if (!lock.ok()) {
        error = lock.error();
        return false;
    }
    PairingRecord record;
    LoadResult loaded = LoadPairing(PathOf(kPairingFileName), record, error);
    if (loaded == LoadResult::Error) {
        return false;
    }
    if (loaded == LoadResult::Ok && record.expires_at_unix_ms > now_unix_ms) {
        status.active = true;
        status.expires_at_unix_ms = record.expires_at_unix_ms;
        status.failed_attempts = record.failed_attempts;
    }
    return true;
}

bool DeviceAuth::ListDevices(std::vector<DeviceRecord> &devices, std::string &error)
{
    FileLock lock(PathOf(kAuthLockFileName), false);
    if (!lock.ok()) {
        error = lock.error();
        return false;
    }
    return LoadDevices(PathOf(kDevicesFileName), devices, error) != LoadResult::Error;
}

bool DeviceAuth::RevokeDevice(const std::string &device_id, bool &found, std::string &error)
{
    found = false;
    FileLock lock(PathOf(kAuthLockFileName), true);
    if (!lock.ok()) {
        error = lock.error();
        return false;
    }
    std::vector<DeviceRecord> devices;
    if (LoadDevices(PathOf(kDevicesFileName), devices, error) == LoadResult::Error) {
        return false;
    }
    std::vector<DeviceRecord> kept;
    for (auto &d : devices) {
        if (d.id == device_id) {
            found = true;
        } else {
            kept.push_back(std::move(d));
        }
    }
    if (!found) {
        return true;
    }
    return SaveDevices(PathOf(kDevicesFileName), kept, error);
}

bool DeviceAuth::RevokeAllDevices(size_t &revoked, std::string &error)
{
    revoked = 0;
    FileLock lock(PathOf(kAuthLockFileName), true);
    if (!lock.ok()) {
        error = lock.error();
        return false;
    }
    std::vector<DeviceRecord> devices;
    if (LoadDevices(PathOf(kDevicesFileName), devices, error) == LoadResult::Error) {
        return false;
    }
    revoked = devices.size();
    return SaveDevices(PathOf(kDevicesFileName), {}, error);
}

ClaimOutcome DeviceAuth::Claim(const ClaimRequest &request, const std::string &hub_id,
                               const std::string &cert_sha256_hex, int64_t now_unix_ms, ClaimResponse &response,
                               std::string &error)
{
    response = ClaimResponse();
    if (!IsValidDisplayName(request.device_name, kMaxDeviceNameCodePoints, kMaxDeviceNameBytes) ||
        !IsLowerHex(request.client_nonce_hex, 32) || !IsLowerHex(request.proof_hex, 64)) {
        return ClaimOutcome::InvalidRequest;
    }

    FileLock lock(PathOf(kAuthLockFileName), true);
    if (!lock.ok()) {
        error = lock.error();
        return ClaimOutcome::StorageError;
    }

    const std::string pairing_path = PathOf(kPairingFileName);
    PairingRecord record;
    LoadResult loaded = LoadPairing(pairing_path, record, error);
    if (loaded == LoadResult::Missing) {
        return ClaimOutcome::NoActiveCode;
    }
    if (loaded == LoadResult::Error) {
        return ClaimOutcome::StorageError;
    }
    if (record.expires_at_unix_ms <= now_unix_ms) {
        RemoveFile(pairing_path, error);
        return ClaimOutcome::Expired;
    }

    std::string expected = PairingClientProof(record.code, hub_id, cert_sha256_hex, request.client_nonce_hex,
                                              request.device_name);
    if (expected.empty() || !ConstantTimeEquals(expected, request.proof_hex)) {
        record.failed_attempts++;
        if (record.failed_attempts >= kMaxPairingFailures) {
            // Too many wrong guesses: the code is gone; the operator makes a
            // new one.
            if (!RemoveFile(pairing_path, error)) {
                return ClaimOutcome::StorageError;
            }
        } else if (!SavePairing(pairing_path, record, error)) {
            // Could not record the failure: refuse rather than allow
            // unlimited guesses.
            RemoveFile(pairing_path, error);
            return ClaimOutcome::StorageError;
        }
        return ClaimOutcome::BadProof;
    }

    std::vector<DeviceRecord> devices;
    if (LoadDevices(PathOf(kDevicesFileName), devices, error) == LoadResult::Error) {
        return ClaimOutcome::StorageError;
    }
    if (devices.size() >= kMaxRegisteredDevices) {
        return ClaimOutcome::TooManyDevices;
    }

    // The code is used up before the device is written: if the write below
    // fails the operator makes a new code, but a code can never register two
    // devices.
    if (!RemoveFile(pairing_path, error)) {
        return ClaimOutcome::StorageError;
    }

    unsigned char token_bytes[kTokenBytes];
    DeviceRecord device;
    device.id = RandomHex(8);
    if (device.id.empty() || !SecureRandomBytes(token_bytes, sizeof(token_bytes))) {
        error = "random number generator failed";
        return ClaimOutcome::StorageError;
    }
    const std::string token = Base64UrlNoPad(token_bytes, sizeof(token_bytes));
    device.name = request.device_name;
    device.token_sha256 = Sha256Hex(token);
    device.created_at_unix_ms = now_unix_ms;
    device.last_used_at_unix_ms = now_unix_ms;
    devices.push_back(device);
    if (!SaveDevices(PathOf(kDevicesFileName), devices, error)) {
        return ClaimOutcome::StorageError;
    }

    response.device_id = device.id;
    response.token = token;
    response.server_proof_hex = PairingServerProof(record.code, hub_id, cert_sha256_hex, request.client_nonce_hex,
                                                   device.id, token);
    return ClaimOutcome::Paired;
}

AuthOutcome DeviceAuth::Authenticate(const std::string &authorization, int64_t now_unix_ms, DeviceRecord &device,
                                     std::string &error)
{
    static const char kPrefix[] = "Bearer ";
    const size_t prefix_len = sizeof(kPrefix) - 1;
    if (authorization.size() <= prefix_len || authorization.compare(0, prefix_len, kPrefix) != 0) {
        return AuthOutcome::Invalid;
    }
    const std::string token = authorization.substr(prefix_len);
    if (token.size() != 43 || !IsBase64Url(token)) {
        return AuthOutcome::Invalid;
    }
    const std::string hash = Sha256Hex(token);

    bool needs_touch = false;
    {
        FileLock lock(PathOf(kAuthLockFileName), false);
        if (!lock.ok()) {
            error = lock.error();
            return AuthOutcome::StorageError;
        }
        std::vector<DeviceRecord> devices;
        if (LoadDevices(PathOf(kDevicesFileName), devices, error) == LoadResult::Error) {
            return AuthOutcome::StorageError;
        }
        bool matched = false;
        for (const auto &d : devices) {
            // Every entry is compared, so timing does not reveal which one matched.
            if (ConstantTimeEquals(d.token_sha256, hash)) {
                device = d;
                matched = true;
            }
        }
        if (!matched) {
            return AuthOutcome::Invalid;
        }
        needs_touch = now_unix_ms - device.last_used_at_unix_ms >= kLastUsedWriteIntervalMs;
    }

    if (needs_touch) {
        // Best effort: a failure here never fails the request.
        FileLock lock(PathOf(kAuthLockFileName), true);
        std::vector<DeviceRecord> devices;
        std::string touch_error;
        if (lock.ok() && LoadDevices(PathOf(kDevicesFileName), devices, touch_error) == LoadResult::Ok) {
            for (auto &d : devices) {
                if (d.id == device.id) {
                    d.last_used_at_unix_ms = now_unix_ms;
                }
            }
            SaveDevices(PathOf(kDevicesFileName), devices, touch_error);
        }
    }
    return AuthOutcome::Ok;
}

} // namespace pogohome_api
