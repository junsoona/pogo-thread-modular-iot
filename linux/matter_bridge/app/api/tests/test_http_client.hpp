// A minimal loopback HTTP / HTTPS client for the API host tests: one request
// per connection, like every route but SSE. The TLS client accepts the hub's
// self-signed certificate (as the app does while pairing) and reports its
// SHA-256.
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "pogohome_api/crypto_util.hpp"

namespace test_support {

struct HttpResponse {
    bool ok = false;
    int status = 0;
    std::string headers;
    std::string body;
    std::string peer_fp; // TLS only
};

inline int ConnectLoopback(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    timeval tv{};
    tv.tv_sec = 5;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

inline HttpResponse ParseHttpResponse(const std::string &raw)
{
    HttpResponse r;
    size_t end = raw.find("\r\n\r\n");
    if (raw.rfind("HTTP/1.1 ", 0) != 0 || end == std::string::npos) {
        return r;
    }
    r.ok = true;
    r.status = std::atoi(raw.c_str() + 9);
    r.headers = raw.substr(0, end);
    r.body = raw.substr(end + 4);
    return r;
}

inline HttpResponse PlainHttpRequest(uint16_t port, const std::string &request)
{
    int fd = ConnectLoopback(port);
    if (fd < 0) {
        return HttpResponse();
    }
    send(fd, request.data(), request.size(), 0);
    std::string raw;
    char buf[4096];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        raw.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    return ParseHttpResponse(raw);
}

inline HttpResponse TlsHttpRequest(uint16_t port, const std::string &request)
{
    HttpResponse r;
    int fd = ConnectLoopback(port);
    if (fd < 0) {
        return r;
    }
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) == 1) {
        X509 *peer = SSL_get_peer_certificate(ssl);
        std::string fp;
        if (peer != nullptr) {
            unsigned char *der = nullptr;
            int len = i2d_X509(peer, &der);
            if (len > 0) {
                fp = pogohome_api::Sha256Hex(der, static_cast<size_t>(len));
                OPENSSL_free(der);
            }
            X509_free(peer);
        }
        SSL_write(ssl, request.data(), static_cast<int>(request.size()));
        std::string raw;
        char buf[4096];
        int n;
        while ((n = SSL_read(ssl, buf, sizeof(buf))) > 0) {
            raw.append(buf, static_cast<size_t>(n));
        }
        r = ParseHttpResponse(raw);
        r.peer_fp = fp;
        SSL_shutdown(ssl);
    }
    ERR_clear_error();
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);
    return r;
}

inline std::string HttpRequestText(const std::string &method, const std::string &path, const std::string &token = "",
                                   const std::string *body = nullptr)
{
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: hub\r\n";
    if (!token.empty()) {
        req += "Authorization: Bearer " + token + "\r\n";
    }
    if (body != nullptr) {
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body->size()) + "\r\n\r\n" +
               *body;
        return req;
    }
    return req + "\r\n";
}

inline std::string JsonStringField(const std::string &json, const std::string &key)
{
    std::string needle = "\"" + key + "\":\"";
    size_t start = json.find(needle);
    if (start == std::string::npos) {
        return std::string();
    }
    start += needle.size();
    return json.substr(start, json.find('"', start) - start);
}

} // namespace test_support
