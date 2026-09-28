#include "pogohome_api/tls_identity.hpp"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <unistd.h>

#include "pogohome_api/atomic_file.hpp"
#include "pogohome_api/crypto_util.hpp"

namespace pogohome_api {

namespace {

constexpr size_t kMaxPemBytes = 16 * 1024;
constexpr long kCertificateValidityDays = 3650;

std::string OpenSslError(const char *what)
{
    unsigned long code = ERR_get_error();
    char buf[256] = {};
    if (code != 0) {
        ERR_error_string_n(code, buf, sizeof(buf));
    }
    ERR_clear_error();
    return std::string(what) + (code != 0 ? std::string(": ") + buf : std::string());
}

template <typename T, void (*Free)(T *)> struct OsslPtr {
    T *p = nullptr;
    OsslPtr() = default;
    explicit OsslPtr(T *ptr) : p(ptr) {}
    ~OsslPtr()
    {
        if (p != nullptr) {
            Free(p);
        }
    }
    OsslPtr(const OsslPtr &) = delete;
    OsslPtr &operator=(const OsslPtr &) = delete;
    T *release()
    {
        T *out = p;
        p = nullptr;
        return out;
    }
};

void FreeBio(BIO *b)
{
    BIO_free(b);
}

using PkeyPtr = OsslPtr<EVP_PKEY, EVP_PKEY_free>;
using X509Ptr = OsslPtr<X509, X509_free>;
using PkeyCtxPtr = OsslPtr<EVP_PKEY_CTX, EVP_PKEY_CTX_free>;
using BioPtr = OsslPtr<BIO, FreeBio>;
using BnPtr = OsslPtr<BIGNUM, BN_free>;

std::string BioToString(BIO *bio)
{
    char *data = nullptr;
    long len = BIO_get_mem_data(bio, &data);
    return len > 0 ? std::string(data, static_cast<size_t>(len)) : std::string();
}

bool GenerateIdentity(const std::string &common_name, std::string &key_pem, std::string &cert_pem, std::string &error)
{
    PkeyCtxPtr pctx(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr));
    PkeyPtr key;
    if (pctx.p == nullptr || EVP_PKEY_keygen_init(pctx.p) != 1 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx.p, NID_X9_62_prime256v1) != 1 ||
        EVP_PKEY_keygen(pctx.p, &key.p) != 1) {
        error = OpenSslError("cannot generate the TLS key");
        return false;
    }

    X509Ptr cert(X509_new());
    unsigned char serial_bytes[16];
    if (cert.p == nullptr || !SecureRandomBytes(serial_bytes, sizeof(serial_bytes))) {
        error = OpenSslError("cannot create the TLS certificate");
        return false;
    }
    serial_bytes[0] &= 0x7F; // a positive serial number
    BnPtr serial(BN_bin2bn(serial_bytes, sizeof(serial_bytes), nullptr));
    X509_NAME *name = X509_get_subject_name(cert.p);
    if (serial.p == nullptr || X509_set_version(cert.p, 2) != 1 ||
        BN_to_ASN1_INTEGER(serial.p, X509_get_serialNumber(cert.p)) == nullptr ||
        X509_time_adj_ex(X509_getm_notBefore(cert.p), -1, 0, nullptr) == nullptr ||
        X509_time_adj_ex(X509_getm_notAfter(cert.p), kCertificateValidityDays, 0, nullptr) == nullptr ||
        X509_set_pubkey(cert.p, key.p) != 1 ||
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
                                   reinterpret_cast<const unsigned char *>(common_name.c_str()), -1, -1, 0) != 1 ||
        X509_set_issuer_name(cert.p, name) != 1 || X509_sign(cert.p, key.p, EVP_sha256()) <= 0) {
        error = OpenSslError("cannot create the TLS certificate");
        return false;
    }

    BioPtr key_bio(BIO_new(BIO_s_mem()));
    BioPtr cert_bio(BIO_new(BIO_s_mem()));
    if (key_bio.p == nullptr || cert_bio.p == nullptr ||
        PEM_write_bio_PrivateKey(key_bio.p, key.p, nullptr, nullptr, 0, nullptr, nullptr) != 1 ||
        PEM_write_bio_X509(cert_bio.p, cert.p) != 1) {
        error = OpenSslError("cannot encode the TLS identity");
        return false;
    }
    key_pem = BioToString(key_bio.p);
    cert_pem = BioToString(cert_bio.p);
    return !key_pem.empty() && !cert_pem.empty();
}

bool BuildContext(const std::string &key_pem, const std::string &cert_pem, std::shared_ptr<TlsContext> &out,
                  std::string &error)
{
    BioPtr key_bio(BIO_new_mem_buf(key_pem.data(), static_cast<int>(key_pem.size())));
    BioPtr cert_bio(BIO_new_mem_buf(cert_pem.data(), static_cast<int>(cert_pem.size())));
    if (key_bio.p == nullptr || cert_bio.p == nullptr) {
        error = OpenSslError("out of memory");
        return false;
    }
    PkeyPtr key(PEM_read_bio_PrivateKey(key_bio.p, nullptr, nullptr, nullptr));
    X509Ptr cert(PEM_read_bio_X509(cert_bio.p, nullptr, nullptr, nullptr));
    if (key.p == nullptr || cert.p == nullptr) {
        error = OpenSslError("the TLS key or certificate file is damaged");
        return false;
    }
    if (X509_check_private_key(cert.p, key.p) != 1) {
        error = OpenSslError("the TLS certificate does not belong to the TLS key");
        return false;
    }

    unsigned char *der = nullptr;
    int der_len = i2d_X509(cert.p, &der);
    if (der_len <= 0 || der == nullptr) {
        error = OpenSslError("cannot encode the TLS certificate");
        return false;
    }
    std::string fingerprint = Sha256Hex(der, static_cast<size_t>(der_len));
    OPENSSL_free(der);

    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == nullptr) {
        error = OpenSslError("cannot create the TLS context");
        return false;
    }
    long options = SSL_OP_NO_COMPRESSION | SSL_OP_NO_TICKET | SSL_OP_CIPHER_SERVER_PREFERENCE;
#ifdef SSL_OP_NO_RENEGOTIATION
    options |= SSL_OP_NO_RENEGOTIATION;
#endif
    SSL_CTX_set_options(ctx, options);
    // TLS 1.2 is kept for older clients, with forward-secret AEAD suites only.
    if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_cipher_list(ctx, "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:"
                                     "ECDHE-ECDSA-CHACHA20-POLY1305") != 1 ||
        SSL_CTX_use_certificate(ctx, cert.p) != 1 || SSL_CTX_use_PrivateKey(ctx, key.p) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        error = OpenSslError("cannot configure the TLS context");
        SSL_CTX_free(ctx);
        return false;
    }
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    out = std::make_shared<TlsContext>(ctx, fingerprint);
    return true;
}

} // namespace

TlsContext::~TlsContext()
{
    SSL_CTX_free(ctx_);
}

TlsIdentityResult LoadOrCreateTlsIdentity(const std::string &directory, const std::string &common_name,
                                          std::shared_ptr<TlsContext> &out, std::string &error)
{
    out.reset();
    const std::string key_path = directory + "/" + kTlsKeyFileName;
    const std::string cert_path = directory + "/" + kTlsCertFileName;

    std::string key_pem, cert_pem;
    ReadFileResult key_read = ReadWholeFile(key_path, kMaxPemBytes, key_pem, error);
    if (key_read == ReadFileResult::Error) {
        return TlsIdentityResult::Error;
    }
    ReadFileResult cert_read = ReadWholeFile(cert_path, kMaxPemBytes, cert_pem, error);
    if (cert_read == ReadFileResult::Error) {
        return TlsIdentityResult::Error;
    }

    if (key_read == ReadFileResult::Ok && cert_read == ReadFileResult::Ok) {
        if (!BuildContext(key_pem, cert_pem, out, error)) {
            error = key_path + " / " + cert_path + ": " + error +
                    " -- not replaced (paired devices pin this certificate). Move both files aside on purpose to "
                    "create a new identity; every device must then pair again.";
            return TlsIdentityResult::Error;
        }
        return TlsIdentityResult::Loaded;
    }
    if (key_read == ReadFileResult::Ok || cert_read == ReadFileResult::Ok) {
        error = "only one of " + key_path + " and " + cert_path +
                " exists -- not replaced. Restore the missing file, or move both aside on purpose to create a new "
                "identity (every device must then pair again).";
        return TlsIdentityResult::Error;
    }

    if (!GenerateIdentity(common_name, key_pem, cert_pem, error)) {
        return TlsIdentityResult::Error;
    }
    // Key first: a crash between the two writes leaves only the key, which
    // the next start reports instead of pairing devices to a half identity.
    if (!WriteFileAtomically(key_path, key_pem, error)) {
        return TlsIdentityResult::Error;
    }
    if (!WriteFileAtomically(cert_path, cert_pem, error)) {
        // Nobody has seen this key yet, so it is safe to drop it again.
        unlink(key_path.c_str());
        return TlsIdentityResult::Error;
    }
    if (!BuildContext(key_pem, cert_pem, out, error)) {
        return TlsIdentityResult::Error;
    }
    return TlsIdentityResult::Created;
}

} // namespace pogohome_api
