// Covers pogohome_api::LoadOrCreateHubId() on a real temporary directory, and
// the additive `hub_id` field of GET /api/v1/health (BuildHealthJson and the
// real Server over a loopback socket).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "ingest/registry.hpp"
#include "pogohome_api/http_server.hpp"
#include "pogohome_api/hub_identity.hpp"
#include "pogohome_api/json_snapshot.hpp"
#include "test_check.hpp"

namespace {

std::string MakeTempDir()
{
    char tmpl[] = "/tmp/pogo_hub_id_test.XXXXXX";
    char *dir = mkdtemp(tmpl);
    return dir != nullptr ? std::string(dir) : std::string();
}

std::string ReadFile(const std::string &path)
{
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void WriteFile(const std::string &path, const std::string &contents)
{
    std::ofstream out(path, std::ios::trunc);
    out << contents;
}

std::string HealthBody(uint16_t port)
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

} // namespace

void RunHubIdentityTests(int *failures)
{
    printf("\n-- scenario: hub_id format --\n");
    {
        API_CHECK(pogohome_api::IsValidHubId("0123456789abcdef0123456789abcdef"), "32 lowercase hex digits are valid");
        API_CHECK(!pogohome_api::IsValidHubId("0123456789ABCDEF0123456789ABCDEF"), "uppercase is rejected");
        API_CHECK(!pogohome_api::IsValidHubId("0123456789abcdef"), "a short id is rejected");
        API_CHECK(!pogohome_api::IsValidHubId("192.168.50.129"), "an address is not an id");
        API_CHECK(!pogohome_api::IsValidHubId(""), "empty is rejected");
    }

    printf("\n-- scenario: first start creates hub_id in the state dir; later starts read the same id --\n");
    {
        std::string dir = MakeTempDir();
        API_CHECK(!dir.empty(), "a temporary state dir was created");
        std::string first, error;
        auto created = pogohome_api::LoadOrCreateHubId(dir, first, error);
        API_CHECK(created == pogohome_api::HubIdLoadResult::Created, "a missing hub_id file is created");
        API_CHECK(pogohome_api::IsValidHubId(first), "the new hub_id is 32 lowercase hex digits");
        std::string path = dir + "/" + pogohome_api::kHubIdFileName;
        API_CHECK(ReadFile(path) == first + "\n", "the file holds exactly the id and a newline");
        struct stat st{};
        API_CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "the file is private to the bridge user");

        std::string second;
        auto loaded = pogohome_api::LoadOrCreateHubId(dir, second, error);
        API_CHECK(loaded == pogohome_api::HubIdLoadResult::Loaded, "a restart reads the existing file");
        API_CHECK(second == first, "the id survives a restart unchanged");

        std::string other_dir = MakeTempDir();
        std::string other;
        pogohome_api::LoadOrCreateHubId(other_dir, other, error);
        API_CHECK(other != first, "another hub (another state dir) gets its own id");

        unlink(path.c_str());
        rmdir(dir.c_str());
        unlink((other_dir + "/" + pogohome_api::kHubIdFileName).c_str());
        rmdir(other_dir.c_str());
    }

    printf("\n-- scenario: a damaged hub_id file is an error, never silently replaced --\n");
    {
        std::string dir = MakeTempDir();
        std::string path = dir + "/" + pogohome_api::kHubIdFileName;
        WriteFile(path, "not-a-hub-id\n");
        std::string id, error;
        auto result = pogohome_api::LoadOrCreateHubId(dir, id, error);
        API_CHECK(result == pogohome_api::HubIdLoadResult::Error, "a malformed file is reported as an error");
        API_CHECK(id.empty(), "no id is returned for a malformed file");
        API_CHECK(!error.empty(), "the error explains what to do");
        API_CHECK(ReadFile(path) == "not-a-hub-id\n", "the damaged file is left exactly as it was");
        unlink(path.c_str());
        rmdir(dir.c_str());
    }

    printf("\n-- scenario: a missing state dir is an error --\n");
    {
        std::string id, error;
        auto result = pogohome_api::LoadOrCreateHubId("/nonexistent/pogo-hub-id-test", id, error);
        API_CHECK(result == pogohome_api::HubIdLoadResult::Error, "an unusable state dir fails cleanly");
        API_CHECK(!error.empty(), "with a readable error");
    }

    printf("\n-- scenario: /api/v1/health adds hub_id after every earlier field --\n");
    {
        const std::string id = "00112233445566778899aabbccddeeff";
        API_CHECK(pogohome_api::BuildHealthJson(42, 2, 1, id) ==
                      "{\"schema_version\":1,\"service\":\"pogohome\",\"registry_generation\":42,\"module_count\":2,"
                      "\"power_base_count\":1,\"hub_id\":\"00112233445566778899aabbccddeeff\"}",
                  "the Bonjour revision only appends hub_id");
        API_CHECK(pogohome_api::BuildHealthJson(42, 2, 1, "") == pogohome_api::BuildHealthJson(42, 2, 1),
                  "without a hub_id the served shape is byte-for-byte the previous one");

        ingest::Registry registry;
        pogohome_api::ServerConfig config;
        config.bind_address = "127.0.0.1";
        config.address_family = AF_INET;
        config.port = 0;
        config.hub_id = id;
        pogohome_api::Server server(registry, config);
        API_CHECK(server.Start(), "the server starts on loopback");
        API_CHECK(server.accepting(), "a started server reports it is accepting");
        std::string body = HealthBody(server.bound_port());
        API_CHECK(body.find("\"hub_id\":\"00112233445566778899aabbccddeeff\"") != std::string::npos,
                  "GET /api/v1/health over a real socket carries the configured hub_id");
        API_CHECK(body.find("\"service\":\"pogohome\"") != std::string::npos &&
                      body.find("\"schema_version\":1") != std::string::npos,
                  "schema_version and service are unchanged");
        server.Stop();
        API_CHECK(!server.accepting(), "a stopped server no longer reports accepting");
    }
}
