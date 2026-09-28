#include "pogohome_api/crypto_util.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace pogohome_api {

bool SecureRandomBytes(unsigned char *out, size_t len)
{
    return len == 0 || RAND_bytes(out, static_cast<int>(len)) == 1;
}

std::string LowerHex(const unsigned char *data, size_t len)
{
    static const char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = kHexDigits[data[i] >> 4];
        out[i * 2 + 1] = kHexDigits[data[i] & 0x0F];
    }
    return out;
}

std::string RandomHex(size_t bytes)
{
    std::string raw(bytes, '\0');
    if (!SecureRandomBytes(reinterpret_cast<unsigned char *>(&raw[0]), bytes)) {
        return std::string();
    }
    std::string hex = LowerHex(reinterpret_cast<const unsigned char *>(raw.data()), raw.size());
    OPENSSL_cleanse(&raw[0], raw.size());
    return hex;
}

std::string Sha256Hex(const unsigned char *data, size_t len)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(data, len, digest);
    return LowerHex(digest, sizeof(digest));
}

std::string Sha256Hex(const std::string &data)
{
    return Sha256Hex(reinterpret_cast<const unsigned char *>(data.data()), data.size());
}

std::string HmacSha256Hex(const std::string &key, const std::string &message)
{
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_len = 0;
    if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char *>(message.data()), message.size(), mac, &mac_len) == nullptr) {
        return std::string();
    }
    return LowerHex(mac, mac_len);
}

bool ConstantTimeEquals(const std::string &a, const std::string &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    return a.empty() || CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string Base64UrlNoPad(const unsigned char *data, size_t len)
{
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out += kAlphabet[(v >> 18) & 0x3F];
        out += kAlphabet[(v >> 12) & 0x3F];
        out += kAlphabet[(v >> 6) & 0x3F];
        out += kAlphabet[v & 0x3F];
        i += 3;
    }
    if (len - i == 1) {
        uint32_t v = uint32_t(data[i]) << 16;
        out += kAlphabet[(v >> 18) & 0x3F];
        out += kAlphabet[(v >> 12) & 0x3F];
    } else if (len - i == 2) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out += kAlphabet[(v >> 18) & 0x3F];
        out += kAlphabet[(v >> 12) & 0x3F];
        out += kAlphabet[(v >> 6) & 0x3F];
    }
    return out;
}

bool IsLowerHex(const std::string &s, size_t length)
{
    if (s.size() != length) {
        return false;
    }
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

} // namespace pogohome_api
