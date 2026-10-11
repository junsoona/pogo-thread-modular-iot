// Covers pogohome_api's build identity (docs/versioning.md): the values the
// build stamped into this test binary, `--version`, and the optional `build`
// object of GET /api/v1/health served by the real Server over a loopback
// socket. What the stamp must contain for a given tree is checked by
// tools/test_pogo_version.py; whether the version here is the one in
// versions.json is checked by CTest (pogohome_build_info_matches_versions).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "ingest/registry.hpp"
#include "pogohome_api/build_info.hpp"
#include "pogohome_api/http_server.hpp"
#include "pogohome_api/json_snapshot.hpp"
#include "test_check.hpp"

namespace {

std::string Health(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    timeval tv{};
    tv.tv_sec = 2;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return "";
    }
    const char req[] = "GET /api/v1/health HTTP/1.1\r\nHost: x\r\n\r\n";
    send(fd, req, sizeof(req) - 1, 0);
    std::string response;
    char buf[1024];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        response.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    size_t body = response.find("\r\n\r\n");
    return body == std::string::npos ? "" : response.substr(body + 4);
}

bool IsSemver(const std::string &v)
{
    int dots = 0;
    bool digit_seen = false;
    for (char c : v) {
        if (c == '.') {
            if (!digit_seen) {
                return false;
            }
            dots++;
            digit_seen = false;
        } else if (c >= '0' && c <= '9') {
            digit_seen = true;
        } else {
            return false;
        }
    }
    return dots == 2 && digit_seen;
}

} // namespace

void RunBuildInfoTests(int *failures)
{
    printf("\n-- scenario: the build identity stamped into this binary --\n");
    const pogohome_api::BuildInfo &info = pogohome_api::CurrentBuildInfo();
    {
        API_CHECK(info.known, "this (CMake) build was stamped");
        API_CHECK(info.component == "linux_bridge", "the component is linux_bridge");
        API_CHECK(IsSemver(info.version), "the version is MAJOR.MINOR.PATCH with no suffix");
        API_CHECK(info.kind == "development" || info.kind == "release", "the build kind is development or release");
        API_CHECK(info.commit == "unknown" || info.commit.size() == 40, "the commit is a full SHA or unknown");
        API_CHECK(info.fingerprint.size() == 64, "the source fingerprint is a SHA-256");
        API_CHECK(info.dirty == "true" || info.dirty == "false" || info.dirty == "unknown", "dirty is true, false or unknown");
        API_CHECK(!(info.kind == "release" && info.dirty != "false"), "a release build is never from a dirty or unknown tree");
        API_CHECK(info.build_id.size() <= 31 && info.build_id.find('.') != std::string::npos, "the build id fits 31 characters");
        API_CHECK(info.build_id.find(info.fingerprint.substr(0, 12)) != std::string::npos,
                  "the build id ends in the first 12 digits of the fingerprint");
        API_CHECK((info.dirty == "true") == (info.build_id.find("+dirty") != std::string::npos), "the build id says +dirty exactly when the tree was dirty");
    }

    printf("\n-- scenario: --version --\n");
    {
        char program[] = "pogo_matter_bridge";
        char flag[] = "--version";
        char other[] = "--interface";
        char value[] = "wpan0";
        char *with[] = {program, other, value, flag, nullptr};
        char *without[] = {program, other, value, nullptr};
        API_CHECK(pogohome_api::WantsVersion(4, with), "--version is found anywhere among the arguments");
        API_CHECK(!pogohome_api::WantsVersion(3, without), "and not found when it is not there");
        API_CHECK(!pogohome_api::WantsVersion(1, with), "the program name is not an argument");
        const std::string line = pogohome_api::VersionLine("pogo_matter_bridge");
        API_CHECK(line == "pogo_matter_bridge " + info.version + " (linux_bridge, build " + info.build_id + ", " + info.kind + ")",
                  "the version line names the program, the version, the component, the build id and the kind");
        const std::string embedded = pogohome_api::BuildInfoString();
        API_CHECK(embedded.rfind("POGO_BUILD_INFO{", 0) == 0 && embedded.back() == '}', "the binary carries the POGO_BUILD_INFO string");
        API_CHECK(embedded.find("component=linux_bridge;") != std::string::npos &&
                      embedded.find("version=" + info.version + ";") != std::string::npos &&
                      embedded.find("build=" + info.build_id + ";") != std::string::npos &&
                      embedded.find("fingerprint=" + info.fingerprint) != std::string::npos,
                  "with the same component, version, build id and fingerprint");
    }

    printf("\n-- scenario: GET /api/v1/health gains an optional build object, after every earlier field --\n");
    {
        const std::string expected = "{\"component\":\"linux_bridge\",\"version\":\"" + info.version + "\",\"build_id\":\"" +
                                     info.build_id + "\",\"build_kind\":\"" + info.kind + "\"}";
        API_CHECK(pogohome_api::BuildInfoJson() == expected, "the build object has component, version, build_id and build_kind");
        API_CHECK(pogohome_api::BuildHealthJson(7, 1, 1).find("build") == std::string::npos,
                  "BuildHealthJson itself is unchanged (the field is appended by the server)");

        const std::string before = pogohome_api::BuildHealthJson(0, 0, 0, "");
        {
            // A server that was not asked for it: exactly the health of before.
            ingest::Registry registry;
            pogohome_api::ServerConfig config;
            config.bind_address = "127.0.0.1";
            config.address_family = AF_INET;
            config.port = 0;
            pogohome_api::Server server(registry, config);
            API_CHECK(server.Start(), "a server without report_build starts on loopback");
            API_CHECK(Health(server.bound_port()) == before, "its health is byte for byte the previous body (no build field)");
            server.Stop();
        }
        ingest::Registry registry;
        pogohome_api::ServerConfig config;
        config.bind_address = "127.0.0.1";
        config.address_family = AF_INET;
        config.port = 0;
        config.report_build = true; // what chip/main.cpp sets
        pogohome_api::Server server(registry, config);
        API_CHECK(server.Start(), "the server starts on loopback");
        const std::string body = Health(server.bound_port());
        API_CHECK(body == before.substr(0, before.size() - 1) + ",\"build\":" + expected + "}",
                  "with report_build the served health is the previous body with only ,\"build\":{...} appended");
        API_CHECK(body.find("\"schema_version\":1") == 1, "schema_version is still the first field and still 1");
        API_CHECK(body.find("setup") == std::string::npos && body.find("key") == std::string::npos, "nothing secret-shaped is in it");
        server.Stop();
    }
}
