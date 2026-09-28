// The TLS identity the API uses for settings sharing: an ECDSA P-256 key and
// a self-signed certificate, created once in <--state-dir>/pogohome/ and
// kept like hub_id. There is no certificate authority on a home LAN: a
// device trusts this certificate because pairing proved it (the pairing
// proof covers the certificate's SHA-256, see device_auth.hpp) and pins that
// SHA-256 from then on. Nothing ever accepts "any certificate".
//
// The same TCP port serves plain HTTP (the unchanged read-only sensor API)
// and TLS: http_server.cpp looks at the first byte of each connection (0x16,
// a TLS handshake record, or an HTTP method letter). Settings, pairing and
// device routes are only answered over TLS.
//
// Damage policy (same as hub_id): a key or certificate file that exists but
// cannot be loaded -- or only one of the two existing -- is an error, never
// silently replaced; a new identity would make every paired device refuse
// the hub until it pairs again. An operator who wants a new identity moves
// both files aside on purpose (docs/pogohome-api-v1.md).
#pragma once

#include <memory>
#include <string>

typedef struct ssl_ctx_st SSL_CTX;

namespace pogohome_api {

constexpr const char kTlsKeyFileName[] = "tls_key.pem";
constexpr const char kTlsCertFileName[] = "tls_cert.pem";

class TlsContext {
public:
    TlsContext(SSL_CTX *ctx, std::string certificate_sha256) : ctx_(ctx), certificate_sha256_(std::move(certificate_sha256)) {}
    ~TlsContext();
    TlsContext(const TlsContext &) = delete;
    TlsContext &operator=(const TlsContext &) = delete;

    SSL_CTX *get() const { return ctx_; }
    // SHA-256 of the certificate's DER encoding, 64 lowercase hex digits:
    // what devices pin, and what the pairing proofs cover.
    const std::string &certificate_sha256() const { return certificate_sha256_; }

private:
    SSL_CTX *ctx_;
    std::string certificate_sha256_;
};

enum class TlsIdentityResult { Loaded, Created, Error };

// `directory` must exist (EnsurePrivateDirectory). `common_name` only labels
// the certificate.
TlsIdentityResult LoadOrCreateTlsIdentity(const std::string &directory, const std::string &common_name,
                                          std::shared_ptr<TlsContext> &out, std::string &error);

} // namespace pogohome_api
