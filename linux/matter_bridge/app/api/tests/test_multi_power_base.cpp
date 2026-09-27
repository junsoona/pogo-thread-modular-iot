// Multi-Power-Base milestone, API side: ownership resolution (one entry per
// module), the additive `power_bases`/`power_base_uuid`/`parent_uuid` fields,
// a byte-exact single-Base legacy comparison, and HTTP/SSE end to end with
// two Bases (one going offline and back, and an SSE client reconnecting).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <regex>
#include <string>
#include <thread>

#include "ingest/registry.hpp"
#include "pogohome_api/http_server.hpp"
#include "pogohome_api/json_snapshot.hpp"
#include "test_check.hpp"

using ingest::NodeState;
using ingest::PowerBaseState;
using ingest::Registry;
using ingest::RegistryView;
using ingest::Uuid;

namespace {

constexpr uint16_t kSht45Bitmap = 0x0006;

Uuid MakeUuid(uint8_t tag)
{
    Uuid u{};
    u.fill(tag);
    return u;
}

NodeState MakeNode(const Uuid &pb, const Uuid &mod, bool online, uint64_t attach_order)
{
    NodeState n;
    n.power_base_uuid = pb;
    n.module_uuid = mod;
    n.parent_uuid = pb;
    n.has_topology = true;
    n.online = online;
    n.depth = 1;
    n.role = 3;
    n.sensor_bitmap = kSht45Bitmap;
    n.boot_session_generation = 1;
    n.last_seq = 1;
    n.attach_order = attach_order;
    return n;
}

PowerBaseState MakeBase(const Uuid &pb, bool online)
{
    PowerBaseState b;
    b.power_base_uuid = pb;
    b.online = online;
    return b;
}

size_t CountOccurrences(const std::string &haystack, const std::string &needle)
{
    size_t count = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + 1)) {
        count++;
    }
    return count;
}

// Opens GET /api/v1/events and returns the connected fd (or -1).
int OpenEvents(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    timeval tv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    std::string req = "GET /api/v1/events HTTP/1.1\r\nHost: test\r\n\r\n";
    send(fd, req.data(), req.size(), 0);
    return fd;
}

// Reads SSE frames until one complete `event: snapshot` data line satisfies
// `pred`, or ~3s pass. Returns that data line (empty if none matched).
template <typename Pred>
std::string ReadSnapshotUntil(int fd, Pred pred)
{
    std::string buf;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        size_t start;
        while ((start = buf.find("event: snapshot\ndata: ")) != std::string::npos) {
            size_t data_start = start + std::strlen("event: snapshot\ndata: ");
            size_t end = buf.find("\n\n", data_start);
            if (end == std::string::npos) {
                break;
            }
            std::string data = buf.substr(data_start, end - data_start);
            buf.erase(0, end + 2);
            if (pred(data)) {
                return data;
            }
        }
        char chunk[4096];
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            break;
        }
        buf.append(chunk, static_cast<size_t>(n));
    }
    return "";
}

pogohome_api::ServerConfig FastConfig()
{
    pogohome_api::ServerConfig cfg;
    cfg.bind_address = "127.0.0.1";
    cfg.address_family = AF_INET;
    cfg.port = 0;
    cfg.sse_heartbeat_interval = std::chrono::milliseconds(250);
    cfg.sse_poll_interval = std::chrono::milliseconds(30);
    return cfg;
}

} // namespace

void RunMultiPowerBaseTests(int *failures)
{
    const Uuid pb_a = MakeUuid(0xA0);
    const Uuid pb_b = MakeUuid(0xB0);
    const Uuid mod1 = MakeUuid(0x11);
    const Uuid mod2 = MakeUuid(0x22);

    printf("\n-- scenario: ResolveModuleOwners -- one entry per module, most recent online attach wins --\n");
    {
        auto one = pogohome_api::ResolveModuleOwners({MakeNode(pb_a, mod1, true, 1), MakeNode(pb_a, mod2, true, 2)});
        API_CHECK(one.size() == 2 && one[0].module_uuid == mod1 && one[1].module_uuid == mod2,
                  "single Base: every module passes through unchanged, in order");

        auto moved = pogohome_api::ResolveModuleOwners({MakeNode(pb_a, mod1, true, 1), MakeNode(pb_b, mod1, true, 5)});
        API_CHECK(moved.size() == 1 && moved[0].power_base_uuid == pb_b,
                  "both Bases list it online: the newer attach (Base B) owns it");

        auto old_online = pogohome_api::ResolveModuleOwners({MakeNode(pb_a, mod1, true, 1), MakeNode(pb_b, mod1, false, 5)});
        API_CHECK(old_online.size() == 1 && old_online[0].power_base_uuid == pb_a,
                  "an online entry always beats an offline one");

        auto both_off = pogohome_api::ResolveModuleOwners({MakeNode(pb_b, mod1, false, 5), MakeNode(pb_a, mod1, false, 1)});
        API_CHECK(both_off.size() == 1 && both_off[0].power_base_uuid == pb_b,
                  "all offline: the most recently attached Base keeps it");
    }

    printf("\n-- scenario: snapshot JSON with two Bases and the same sensor kind on both --\n");
    {
        RegistryView view;
        view.power_bases = {MakeBase(pb_a, true), MakeBase(pb_b, false)};
        view.nodes = {MakeNode(pb_a, mod1, true, 1), MakeNode(pb_b, mod2, false, 2)};
        std::string json = pogohome_api::BuildSnapshotJson(view, 7, 1732000000000);

        API_CHECK(json.find("\"power_bases\":[{\"uuid\":\"a0a0a0a0a0a0a0a0\",\"online\":true,\"module_count\":1,"
                            "\"online_module_count\":1},{\"uuid\":\"b0b0b0b0b0b0b0b0\",\"online\":false,"
                            "\"module_count\":1,\"online_module_count\":0}]") != std::string::npos,
                  "power_bases lists both Bases with their own state and counts");
        API_CHECK(json.find("{\"uuid\":\"1111111111111111\",\"power_base_uuid\":\"a0a0a0a0a0a0a0a0\","
                            "\"parent_uuid\":\"a0a0a0a0a0a0a0a0\",\"online\":true") != std::string::npos,
                  "module 1 names Base A");
        API_CHECK(json.find("{\"uuid\":\"2222222222222222\",\"power_base_uuid\":\"b0b0b0b0b0b0b0b0\","
                            "\"parent_uuid\":\"b0b0b0b0b0b0b0b0\",\"online\":false") != std::string::npos,
                  "module 2 (same SHT45 kind) names Base B and keeps its own state");
        API_CHECK(CountOccurrences(json, "\"sensor_bitmap\":6") == 2, "both same-kind modules are listed, unmerged");
    }

    printf("\n-- scenario: a module under two Bases is listed exactly once --\n");
    {
        RegistryView view;
        view.power_bases = {MakeBase(pb_a, true), MakeBase(pb_b, true)};
        view.nodes = {MakeNode(pb_a, mod1, true, 1), MakeNode(pb_b, mod1, true, 3)};
        std::string json = pogohome_api::BuildSnapshotJson(view, 1, 0);
        API_CHECK(CountOccurrences(json, "\"uuid\":\"1111111111111111\"") == 1, "the module uuid appears once");
        API_CHECK(json.find("\"module_count\":0") != std::string::npos &&
                      json.find("\"uuid\":\"b0b0b0b0b0b0b0b0\",\"online\":true,\"module_count\":1") !=
                          std::string::npos,
                  "it is counted under its owner (Base B) only");
    }

    printf("\n-- scenario: a Base the Registry is not tracking is derived from its modules --\n");
    {
        std::string json = pogohome_api::BuildSnapshotJson(
            std::vector<NodeState>{MakeNode(pb_a, mod1, true, 1), MakeNode(pb_b, mod2, false, 2)}, 1, 0);
        API_CHECK(json.find("{\"uuid\":\"a0a0a0a0a0a0a0a0\",\"online\":true,\"module_count\":1") != std::string::npos,
                  "an untracked Base with an online module is online");
        API_CHECK(json.find("{\"uuid\":\"b0b0b0b0b0b0b0b0\",\"online\":false,\"module_count\":1") != std::string::npos,
                  "an untracked Base whose modules are all offline is not claimed online");
    }

    printf("\n-- scenario: single-Base compatibility -- removing only the additive fields gives the exact "
           "pre-milestone JSON --\n");
    {
        NodeState n = MakeNode(MakeUuid(0xB0), Uuid{0x74, 0x4d, 0xbd, 0xff, 0xfe, 0x65, 0xd3, 0x57}, true, 1);
        n.role = 2;
        n.last_seq = 123;
        n.has_sample = true;
        n.last_sample_seq = 123;
        n.last_channels = {ingest::ChannelReading{1, 1, 0, 23400}};
        RegistryView view;
        view.power_bases = {MakeBase(MakeUuid(0xB0), true)};
        view.nodes = {n};
        std::string json = pogohome_api::BuildSnapshotJson(view, 42, 1732000000000);

        std::string legacy = std::regex_replace(json, std::regex("\"power_bases\":\\[[^\\]]*\\],"), "");
        legacy = std::regex_replace(legacy, std::regex("\"(power_base|parent)_uuid\":\"[0-9a-f]{16}\","), "");
        const std::string expected =
            "{\"schema_version\":1,\"generated_at_unix_ms\":1732000000000,\"registry_generation\":42,\"modules\":["
            "{\"uuid\":\"744dbdfffe65d357\",\"online\":true,\"depth\":1,\"role\":\"intermediate\",\"sensor_bitmap\":6,"
            "\"generation\":1,\"last_seq\":123,\"readings\":[{\"kind\":\"temperature\",\"raw_value\":23400,\"scale\":1,"
            "\"value\":23.400,\"unit\":\"celsius\",\"status\":\"ok\",\"sample_seq\":123}]}]}";
        API_CHECK(legacy == expected, "every pre-existing field, value and order is unchanged");
        if (legacy != expected) {
            printf("      got:      %s\n      expected: %s\n", legacy.c_str(), expected.c_str());
        }
        API_CHECK(json.find("\"schema_version\":1,") == 1, "schema_version stays 1 (additive change only)");

        std::string health = pogohome_api::BuildHealthJson(42, 2);
        API_CHECK(health == "{\"schema_version\":1,\"service\":\"pogohome\",\"registry_generation\":42,\"module_count\":2}",
                  "the original health shape is unchanged");
        API_CHECK(pogohome_api::BuildHealthJson(42, 2, 1) ==
                      "{\"schema_version\":1,\"service\":\"pogohome\",\"registry_generation\":42,\"module_count\":2,"
                      "\"power_base_count\":1}",
                  "the served health shape only appends power_base_count");
    }

    printf("\n-- scenario: HTTP + SSE with two Bases; Base A goes offline and back; the SSE client reconnects --\n");
    {
        Registry registry(std::chrono::milliseconds(200), std::chrono::milliseconds(10));
        registry.apply_topology(pb_a, mod1, pb_a, true, 1, 3, kSht45Bitmap, false, 1, 1);
        registry.apply_topology(pb_b, mod2, pb_b, true, 1, 3, kSht45Bitmap, false, 1, 1);
        pogohome_api::Server server(registry, FastConfig());
        API_CHECK(server.Start(), "Server::Start() succeeds");

        int fd = OpenEvents(server.bound_port());
        API_CHECK(fd >= 0, "SSE client connected");
        std::string first = ReadSnapshotUntil(fd, [](const std::string &) { return true; });
        API_CHECK(first.find("\"uuid\":\"a0a0a0a0a0a0a0a0\",\"online\":true") != std::string::npos &&
                      first.find("\"uuid\":\"b0b0b0b0b0b0b0b0\",\"online\":true") != std::string::npos,
                  "the first SSE snapshot has both Bases online");

        // Keep only Base B alive past A's timeout.
        std::thread keep_b([&] {
            auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
            while (std::chrono::steady_clock::now() < until) {
                registry.apply_heartbeat(pb_b, 0, 1);
                registry.apply_topology(pb_b, mod2, pb_b, true, 1, 3, kSht45Bitmap, false, 1, 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
        std::string offline = ReadSnapshotUntil(fd, [](const std::string &d) {
            return d.find("\"uuid\":\"a0a0a0a0a0a0a0a0\",\"online\":false") != std::string::npos;
        });
        API_CHECK(!offline.empty(), "an SSE snapshot reports Base A offline without any new message from A");
        API_CHECK(offline.find("\"uuid\":\"1111111111111111\",\"power_base_uuid\":\"a0a0a0a0a0a0a0a0\","
                               "\"parent_uuid\":\"a0a0a0a0a0a0a0a0\",\"online\":false") != std::string::npos,
                  "Base A's module is offline in that same snapshot");
        API_CHECK(offline.find("\"uuid\":\"b0b0b0b0b0b0b0b0\",\"online\":true") != std::string::npos &&
                      offline.find("\"uuid\":\"2222222222222222\",\"power_base_uuid\":\"b0b0b0b0b0b0b0b0\","
                                   "\"parent_uuid\":\"b0b0b0b0b0b0b0b0\",\"online\":true") != std::string::npos,
                  "Base B and its module stay online in that same snapshot");
        keep_b.join();

        // The app's SSE connection drops; while it is gone, Base A comes back.
        close(fd);
        registry.apply_heartbeat(pb_a, 0, 1);
        registry.apply_topology(pb_a, mod1, pb_a, true, 1, 3, kSht45Bitmap, false, 1, 1);
        registry.apply_heartbeat(pb_b, 0, 1);
        registry.apply_topology(pb_b, mod2, pb_b, true, 1, 3, kSht45Bitmap, false, 1, 1);

        int fd2 = OpenEvents(server.bound_port());
        std::string reconnected = ReadSnapshotUntil(fd2, [](const std::string &) { return true; });
        API_CHECK(reconnected.find("\"uuid\":\"a0a0a0a0a0a0a0a0\",\"online\":true") != std::string::npos &&
                      reconnected.find("\"uuid\":\"1111111111111111\",\"power_base_uuid\":\"a0a0a0a0a0a0a0a0\","
                                       "\"parent_uuid\":\"a0a0a0a0a0a0a0a0\",\"online\":true") != std::string::npos,
                  "a reconnecting SSE client immediately gets Base A and its module back online");
        API_CHECK(CountOccurrences(reconnected, "\"power_base_uuid\":") == 2, "still exactly two modules, no duplicates");
        close(fd2);
        server.Stop();
    }
}
