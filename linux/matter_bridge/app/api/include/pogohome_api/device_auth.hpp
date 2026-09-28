// Which PogoHome devices may read and change the shared home settings
// (settings_store.hpp). Finding the hub over Bonjour grants nothing: a device
// is registered only by proving it knows a one-time pairing code the
// operator created on the hub (`pogohome-hubctl pair`), and from then on it
// sends its own bearer token.
//
// Pairing (docs/pogohome-api-v1.md "Pairing"), all over the API's TLS:
// - the code (12 Crockford base32 characters, 60 bits, shown as
//   XXXX-XXXX-XXXX) is never sent. The device sends
//   HMAC-SHA256(code, "client" message) over the hub_id, the SHA-256 of the
//   TLS certificate it actually saw, a fresh nonce and its name. The hub
//   recomputes it with its OWN certificate, so a device talking to an
//   impostor (a different certificate) is rejected and learns nothing;
// - the hub answers with a token and HMAC-SHA256(code, "server" message)
//   over the same values plus the new device id and the token's hash. The
//   device stores the token and pins the certificate only after that
//   proof checks out: only something that knows the code could produce it;
// - a code expires (default 10 minutes), works once, and is destroyed after
//   kMaxPairingFailures wrong proofs, so it cannot be guessed online, and
//   60 bits cannot be searched offline within its lifetime.
//
// Storage: <--state-dir>/pogohome/{devices.json,pairing.json} (0600, in a
// 0700 directory), shared with the pogohome-hubctl tool; every
// read-modify-write holds an flock on auth.lock. Only SHA-256 hashes of
// tokens are stored. Nothing here ever logs a token or a code.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pogohome_api {

constexpr const char kDevicesFileName[] = "devices.json";
constexpr const char kPairingFileName[] = "pairing.json";
constexpr const char kAuthLockFileName[] = "auth.lock";
constexpr int kMaxPairingFailures = 5;
constexpr size_t kMaxRegisteredDevices = 32;
constexpr int64_t kDefaultPairingTtlMs = 10 * 60 * 1000;
constexpr int64_t kMaxPairingTtlMs = 60 * 60 * 1000;
constexpr size_t kMaxDeviceNameCodePoints = 64;
constexpr size_t kMaxDeviceNameBytes = 256;

// ---- pairing code and proofs (pure functions, shared with the app's spec) ----

// A new random code, 12 characters of "0123456789ABCDEFGHJKMNPQRSTVWXYZ".
// "" if the RNG fails.
std::string GeneratePairingCode();
// "XXXX-XXXX-XXXX".
std::string FormatPairingCode(const std::string &code);
// Accepts what a person types: any case, spaces and hyphens, O for 0, I/L
// for 1. False unless that leaves exactly 12 code characters.
bool NormalizePairingCode(const std::string &input, std::string &code);

std::string PairingClientProof(const std::string &code, const std::string &hub_id, const std::string &cert_sha256_hex,
                               const std::string &client_nonce_hex, const std::string &device_name);
std::string PairingServerProof(const std::string &code, const std::string &hub_id, const std::string &cert_sha256_hex,
                               const std::string &client_nonce_hex, const std::string &device_id,
                               const std::string &token);

// ---- registered devices ----

struct DeviceRecord {
    std::string id; // 16 lowercase hex
    std::string name;
    std::string token_sha256; // 64 lowercase hex
    int64_t created_at_unix_ms = 0;
    int64_t last_used_at_unix_ms = 0;
};

struct PairingStatus {
    bool active = false;
    int64_t expires_at_unix_ms = 0;
    int failed_attempts = 0;
};

struct ClaimRequest {
    std::string device_name;
    std::string client_nonce_hex; // 32 lowercase hex (16 bytes)
    std::string proof_hex;        // 64 lowercase hex
};

struct ClaimResponse {
    std::string device_id;
    std::string token;
    std::string server_proof_hex;
};

enum class ClaimOutcome {
    Paired,
    InvalidRequest,
    NoActiveCode, // none was created, it was used, or it was cancelled
    Expired,
    BadProof,       // wrong code, or the device saw a different certificate
    TooManyDevices, // kMaxRegisteredDevices already registered
    StorageError,
};

enum class AuthOutcome { Ok, Invalid, StorageError };

class DeviceAuth {
public:
    // `directory` is <--state-dir>/pogohome; it must already exist
    // (EnsurePrivateDirectory).
    explicit DeviceAuth(std::string directory);

    // -- operator side (pogohome-hubctl) --
    // Replaces any earlier code.
    bool CreatePairingCode(int64_t now_unix_ms, int64_t ttl_ms, std::string &code, int64_t &expires_at_unix_ms,
                           std::string &error);
    bool CancelPairing(std::string &error);
    bool GetPairingStatus(int64_t now_unix_ms, PairingStatus &status, std::string &error);
    bool ListDevices(std::vector<DeviceRecord> &devices, std::string &error);
    // `found` false when no device has that id (nothing changes).
    bool RevokeDevice(const std::string &device_id, bool &found, std::string &error);
    bool RevokeAllDevices(size_t &revoked, std::string &error);

    // -- server side --
    ClaimOutcome Claim(const ClaimRequest &request, const std::string &hub_id, const std::string &cert_sha256_hex,
                       int64_t now_unix_ms, ClaimResponse &response, std::string &error);
    // `authorization` is the raw Authorization header value ("Bearer …").
    AuthOutcome Authenticate(const std::string &authorization, int64_t now_unix_ms, DeviceRecord &device,
                             std::string &error);

private:
    std::string PathOf(const char *name) const { return directory_ + "/" + name; }

    const std::string directory_;
};

} // namespace pogohome_api
