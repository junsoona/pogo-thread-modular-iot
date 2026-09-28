// Thin wrappers over OpenSSL's libcrypto for the settings-sharing revision
// (device pairing, bearer tokens, the settings store's id). OpenSSL is not a
// new dependency of the production bridge: connectedhomeip's Linux build
// already links it as its default crypto backend (see
// tools/build_matter_bridge.sh, "libssl-dev").
#pragma once

#include <cstddef>
#include <string>

namespace pogohome_api {

// Cryptographically secure random bytes (RAND_bytes). False if the RNG fails.
bool SecureRandomBytes(unsigned char *out, size_t len);

// `bytes` random bytes as lowercase hex, or "" when the RNG fails.
std::string RandomHex(size_t bytes);

std::string LowerHex(const unsigned char *data, size_t len);

// SHA-256 as 64 lowercase hex digits.
std::string Sha256Hex(const std::string &data);
std::string Sha256Hex(const unsigned char *data, size_t len);

// HMAC-SHA-256 as 64 lowercase hex digits.
std::string HmacSha256Hex(const std::string &key, const std::string &message);

// Compares two strings in time that depends only on their lengths.
bool ConstantTimeEquals(const std::string &a, const std::string &b);

// RFC 4648 base64url without padding.
std::string Base64UrlNoPad(const unsigned char *data, size_t len);

// Whether `s` has exactly `length` lowercase hex digits.
bool IsLowerHex(const std::string &s, size_t length);

} // namespace pogohome_api
