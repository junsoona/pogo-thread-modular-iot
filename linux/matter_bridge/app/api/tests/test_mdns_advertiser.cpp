// Drives the REAL pogohome_api::MdnsAdvertiser engine (mdns_advertiser.cpp)
// through a fake AvahiApi that behaves like libavahi-client: a poll lock,
// client/group state callbacks fired with that lock held (avahi's event
// thread), synchronous callbacks from inside avahi_client_new(), and local
// name collisions from add_service. What the fake cannot show -- real mDNS
// packets on a LAN -- is verified on the Linux host with avahi-browse and a
// real iPhone/Mac (see docs/pogohome-api-v1.md "Bonjour").
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "pogohome_api/mdns_advertiser.hpp"
#include "test_check.hpp"

using pogohome_api::AdvertState;
using pogohome_api::AvahiApi;
using pogohome_api::MdnsAdvertiser;

namespace {

struct AddedService {
    int interface_index;
    int protocol;
    std::string name;
    std::string type;
    uint16_t port;
    std::vector<std::string> txt;
};

struct FakeAvahi {
    std::recursive_mutex poll_lock; // avahi's poll mutex (the fake fires callbacks while holding it)
    int client_state_on_new = AvahiApi::kClientRunning;
    bool client_new_fails = false;
    AvahiApi::ClientCallback client_cb = nullptr;
    void *client_ud = nullptr;
    AvahiApi::GroupCallback group_cb = nullptr;
    void *group_ud = nullptr;
    void *live_group = nullptr;
    bool group_empty = true;
    std::set<std::string> taken_local_names;
    int add_result = 0; // forced add_service result when non-zero
    std::atomic<int> clients_created{0};
    std::atomic<int> clients_freed{0};
    std::atomic<int> groups_created{0};
    std::atomic<int> groups_freed{0};
    std::atomic<int> resets{0};
    std::atomic<int> commits{0};
    std::atomic<int> poll_stopped{0};
    std::atomic<int> poll_freed{0};
    std::vector<AddedService> added;
    int client_token = 0;
    int group_tokens[64] = {};
    int next_group = 0;
};

FakeAvahi *g_fake = nullptr;

void *FakePollNew() { return g_fake; }
void FakePollFree(void *) { g_fake->poll_freed++; }
const void *FakePollGet(void *) { return g_fake; }
int FakePollStart(void *) { return 0; }
int FakePollStop(void *) { g_fake->poll_stopped++; return 0; }
void FakePollLock(void *) { g_fake->poll_lock.lock(); }
void FakePollUnlock(void *) { g_fake->poll_lock.unlock(); }

void *FakeClientNew(const void *, int flags, AvahiApi::ClientCallback cb, void *ud, int *error)
{
    if (g_fake->client_new_fails) {
        *error = -1;
        return nullptr;
    }
    (void)flags;
    g_fake->clients_created++;
    g_fake->client_cb = cb;
    g_fake->client_ud = ud;
    // Like avahi: the first state callback arrives before avahi_client_new() returns.
    cb(&g_fake->client_token, g_fake->client_state_on_new, ud);
    return &g_fake->client_token;
}
void FakeClientFree(void *) { g_fake->clients_freed++; }
int FakeClientErrno(void *) { return -1; }

void *FakeGroupNew(void *, AvahiApi::GroupCallback cb, void *ud)
{
    g_fake->groups_created++;
    g_fake->group_cb = cb;
    g_fake->group_ud = ud;
    g_fake->group_empty = true;
    g_fake->live_group = &g_fake->group_tokens[g_fake->next_group++ % 64];
    return g_fake->live_group;
}
int FakeGroupFree(void *)
{
    g_fake->groups_freed++;
    g_fake->group_empty = true;
    g_fake->live_group = nullptr;
    return 0;
}
int FakeGroupReset(void *)
{
    g_fake->resets++;
    g_fake->group_empty = true;
    return 0;
}
int FakeGroupIsEmpty(void *) { return g_fake->group_empty ? 1 : 0; }
int FakeGroupCommit(void *)
{
    g_fake->commits++;
    return 0;
}
int FakeAddService(void *, int iface, int proto, int, const char *name, const char *type, const char *, const char *,
                   uint16_t port, void *txt)
{
    if (g_fake->add_result != 0) {
        return g_fake->add_result;
    }
    if (g_fake->taken_local_names.count(name) != 0) {
        return AvahiApi::kErrCollision;
    }
    AddedService s{iface, proto, name, type, port, {}};
    if (txt != nullptr) {
        s.txt = *static_cast<std::vector<std::string> *>(txt);
    }
    g_fake->added.push_back(s);
    g_fake->group_empty = false;
    return 0;
}
void *FakeStringListAdd(void *list, const char *text)
{
    auto *v = list != nullptr ? static_cast<std::vector<std::string> *>(list) : new std::vector<std::string>();
    v->push_back(text);
    return v;
}
void FakeStringListFree(void *list) { delete static_cast<std::vector<std::string> *>(list); }
char *FakeAlternativeName(const char *name)
{
    // "X" -> "X #2" -> "X #3", like avahi_alternative_service_name().
    std::string n = name;
    size_t hash = n.rfind(" #");
    std::string next = hash == std::string::npos ? n + " #2"
                                                 : n.substr(0, hash) + " #" + std::to_string(atoi(n.c_str() + hash + 2) + 1);
    return strdup(next.c_str());
}
void FakeFree(void *p) { free(p); }
const char *FakeStrerror(int) { return "fake avahi error"; }

AvahiApi FakeApi()
{
    AvahiApi api;
    api.threaded_poll_new = FakePollNew;
    api.threaded_poll_free = FakePollFree;
    api.threaded_poll_get = FakePollGet;
    api.threaded_poll_start = FakePollStart;
    api.threaded_poll_stop = FakePollStop;
    api.threaded_poll_lock = FakePollLock;
    api.threaded_poll_unlock = FakePollUnlock;
    api.client_new = FakeClientNew;
    api.client_free = FakeClientFree;
    api.client_errno = FakeClientErrno;
    api.entry_group_new = FakeGroupNew;
    api.entry_group_free = FakeGroupFree;
    api.entry_group_reset = FakeGroupReset;
    api.entry_group_is_empty = FakeGroupIsEmpty;
    api.entry_group_commit = FakeGroupCommit;
    api.entry_group_add_service_strlst = FakeAddService;
    api.string_list_add = FakeStringListAdd;
    api.string_list_free = FakeStringListFree;
    api.alternative_service_name = FakeAlternativeName;
    api.free = FakeFree;
    api.strerror = FakeStrerror;
    return api;
}

// avahi's event thread delivering a state change.
void FireClient(int state)
{
    std::lock_guard<std::recursive_mutex> lock(g_fake->poll_lock);
    g_fake->client_cb(&g_fake->client_token, state, g_fake->client_ud);
}

void FireGroup(int state)
{
    std::lock_guard<std::recursive_mutex> lock(g_fake->poll_lock);
    if (g_fake->group_cb != nullptr && g_fake->live_group != nullptr) {
        g_fake->group_cb(g_fake->live_group, state, g_fake->group_ud);
    }
}

size_t AddedCount()
{
    std::lock_guard<std::recursive_mutex> lock(g_fake->poll_lock);
    return g_fake->added.size();
}

AddedService LastAdded()
{
    std::lock_guard<std::recursive_mutex> lock(g_fake->poll_lock);
    return g_fake->added.empty() ? AddedService{} : g_fake->added.back();
}

template <typename Cond> bool WaitFor(Cond cond, int timeout_ms = 2000)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

struct Harness {
    FakeAvahi fake;
    std::atomic<bool> available{true};
    std::atomic<unsigned> ifindex{3};
    pogohome_api::AdvertiserConfig config;

    Harness()
    {
        g_fake = &fake;
        config.interface_name = "wlp-test";
        config.address_family = AF_INET;
        config.port = 8787;
        config.service_name = "Jogakbo Hub (hub)";
        config.txt = pogohome_api::BuildBonjourTxt("00112233445566778899aabbccddeeff");
        config.available = [this] { return available.load(); };
        config.interface_index = [this] { return ifindex.load(); };
        config.recheck_interval = std::chrono::milliseconds(20);
        config.retry_delay = std::chrono::milliseconds(60);
    }
};

} // namespace

void RunMdnsAdvertiserTests(int *failures)
{
    printf("\n-- scenario: TXT record and default service name --\n");
    {
        auto txt = pogohome_api::BuildBonjourTxt("00112233445566778899aabbccddeeff");
        API_CHECK(txt.size() == 4 && txt[0] == "txtvers=1" && txt[1] == "api=1" && txt[2] == "path=/api/v1" &&
                      txt[3] == "hub_id=00112233445566778899aabbccddeeff",
                  "TXT carries txtvers, api version, base path and hub_id");
        API_CHECK(std::string(pogohome_api::kBonjourServiceType) == "_jogakbo._tcp", "the service type is _jogakbo._tcp");
        API_CHECK(pogohome_api::DefaultBonjourServiceName("pogo-hub.lan") == "Jogakbo Hub (pogo-hub)",
                  "the name uses the short host name");
        std::string long_name = pogohome_api::DefaultBonjourServiceName(std::string(80, 'h'));
        API_CHECK(long_name.size() <= 63, "a long host name is cut to the 63-byte DNS label limit");
        std::string utf8 = pogohome_api::DefaultBonjourServiceName(std::string(47, 'a') + "\xEC\xA1\xB0\xEA\xB0\x81");
        API_CHECK(utf8.size() <= 63 && (static_cast<unsigned char>(utf8.back()) & 0xC0) != 0xC0,
                  "the cut never ends inside a UTF-8 character");
    }

    printf("\n-- scenario: available API is advertised once, on its interface, with the real port and TXT --\n");
    {
        Harness h;
        MdnsAdvertiser adv(FakeApi(), h.config);
        API_CHECK(adv.Start(), "the advertiser starts");
        API_CHECK(WaitFor([] { return AddedCount() == 1; }), "one service record is registered");
        AddedService s = LastAdded();
        API_CHECK(s.type == "_jogakbo._tcp" && s.port == 8787 && s.interface_index == 3 &&
                      s.protocol == AvahiApi::kProtoInet,
                  "type, API port, API interface index and IPv4 protocol");
        API_CHECK(s.txt.size() == 4 && s.txt[3] == "hub_id=00112233445566778899aabbccddeeff", "TXT is attached");
        API_CHECK(adv.state() == AdvertState::Registering, "registering until avahi confirms");
        FireGroup(AvahiApi::kGroupEstablished);
        API_CHECK(WaitFor([&] { return adv.state() == AdvertState::Established; }), "established once avahi confirms");
        for (int i = 0; i < 5; i++) {
            adv.Poke();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        API_CHECK(AddedCount() == 1, "re-checks never register a duplicate record");
        API_CHECK(h.fake.clients_created == 1, "exactly one avahi client");
        adv.Stop();
        API_CHECK(h.fake.groups_freed == 1 && h.fake.clients_freed == 1 && h.fake.poll_stopped == 1 &&
                      h.fake.poll_freed == 1,
                  "Stop() withdraws the record and releases the client and the event thread");
        API_CHECK(adv.state() == AdvertState::Stopped, "stopped");
        adv.Stop();
        API_CHECK(h.fake.clients_freed == 1, "a second Stop() is a no-op");
    }

    printf("\n-- scenario: the API becoming unreachable withdraws the record; recovery republishes --\n");
    {
        Harness h;
        MdnsAdvertiser adv(FakeApi(), h.config);
        adv.Start();
        WaitFor([] { return AddedCount() == 1; });
        h.available = false;
        adv.Poke();
        API_CHECK(WaitFor([&] { return adv.state() == AdvertState::Withdrawn; }), "withdrawn while unreachable");
        API_CHECK(h.fake.group_empty, "nothing stays published");
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        API_CHECK(AddedCount() == 1, "nothing is re-added while unreachable");
        h.available = true;
        adv.Poke();
        API_CHECK(WaitFor([] { return AddedCount() == 2; }), "republished when the API is reachable again");
        adv.Stop();
    }

    printf("\n-- scenario: API unreachable at start -> never advertised until it is reachable --\n");
    {
        Harness h;
        h.available = false;
        MdnsAdvertiser adv(FakeApi(), h.config);
        adv.Start();
        API_CHECK(WaitFor([&] { return adv.state() == AdvertState::Withdrawn; }), "withdrawn from the start");
        API_CHECK(AddedCount() == 0, "no record for an API that cannot take requests");
        adv.Stop();
    }

    printf("\n-- scenario: a name taken on the LAN is renamed with avahi's alternative name --\n");
    {
        Harness h;
        MdnsAdvertiser adv(FakeApi(), h.config);
        adv.Start();
        WaitFor([] { return AddedCount() == 1; });
        FireGroup(AvahiApi::kGroupCollision);
        API_CHECK(WaitFor([] { return AddedCount() == 2; }), "re-registered after the collision");
        API_CHECK(LastAdded().name == "Jogakbo Hub (hub) #2", "under the alternative name");
        API_CHECK(adv.current_name() == "Jogakbo Hub (hub) #2", "current_name() follows the rename");
        API_CHECK(LastAdded().txt.size() == 4 && LastAdded().txt[3] == "hub_id=00112233445566778899aabbccddeeff",
                  "the hub_id is unchanged by the rename");
        adv.Stop();
    }

    printf("\n-- scenario: a name already registered on this host is renamed before committing --\n");
    {
        Harness h;
        h.fake.taken_local_names = {"Jogakbo Hub (hub)"};
        MdnsAdvertiser adv(FakeApi(), h.config);
        adv.Start();
        API_CHECK(WaitFor([] { return AddedCount() == 1; }), "registered");
        API_CHECK(LastAdded().name == "Jogakbo Hub (hub) #2", "under the next free name");
        adv.Stop();
    }

    printf("\n-- scenario: avahi-daemon restarting -> record dropped, then registered again --\n");
    {
        Harness h;
        MdnsAdvertiser adv(FakeApi(), h.config);
        adv.Start();
        WaitFor([] { return AddedCount() == 1; });
        FireClient(AvahiApi::kClientConnecting);
        API_CHECK(WaitFor([&] { return adv.state() == AdvertState::WaitingForDaemon; }), "waiting for the daemon");
        API_CHECK(h.fake.groups_freed == 1, "the old group is released");
        FireClient(AvahiApi::kClientRunning);
        API_CHECK(WaitFor([] { return AddedCount() == 2; }), "registered again when the daemon is back");
        API_CHECK(h.fake.clients_created == 1, "the same NO_FAIL client reconnects, no second client");
        adv.Stop();
    }

    printf("\n-- scenario: avahi-daemon absent at start -> no record until it appears --\n");
    {
        Harness h;
        h.fake.client_state_on_new = AvahiApi::kClientConnecting;
        MdnsAdvertiser adv(FakeApi(), h.config);
        adv.Start();
        API_CHECK(WaitFor([&] { return adv.state() == AdvertState::WaitingForDaemon; }), "waiting for the daemon");
        API_CHECK(AddedCount() == 0 && h.fake.groups_created == 0, "nothing registered without a daemon");
        FireClient(AvahiApi::kClientRunning);
        API_CHECK(WaitFor([] { return AddedCount() == 1; }), "registered once the daemon is running");
        adv.Stop();
    }

    printf("\n-- scenario: host name re-registration (client COLLISION) drops the record until RUNNING --\n");
    {
        Harness h;
        MdnsAdvertiser adv(FakeApi(), h.config);
        adv.Start();
        WaitFor([] { return AddedCount() == 1; });
        FireClient(AvahiApi::kClientCollision);
        API_CHECK(WaitFor([&] { return h.fake.groups_freed == 1; }), "the record is dropped");
        FireClient(AvahiApi::kClientRunning);
        API_CHECK(WaitFor([] { return AddedCount() == 2; }), "and registered again");
        adv.Stop();
    }

    printf("\n-- scenario: client FAILURE -> a new client after retry_delay, never a tight loop --\n");
    {
        Harness h;
        MdnsAdvertiser adv(FakeApi(), h.config);
        adv.Start();
        WaitFor([] { return AddedCount() == 1; });
        FireClient(AvahiApi::kClientFailure);
        API_CHECK(WaitFor([&] { return h.fake.clients_freed == 1; }), "the failed client is freed");
        API_CHECK(h.fake.clients_created == 1, "not recreated immediately");
        API_CHECK(WaitFor([&] { return h.fake.clients_created == 2; }), "recreated after the retry delay");
        API_CHECK(WaitFor([] { return AddedCount() == 2; }), "and the record registered again");
        adv.Stop();
    }

    printf("\n-- scenario: registration failure retries after retry_delay --\n");
    {
        Harness h;
        h.fake.add_result = -1;
        MdnsAdvertiser adv(FakeApi(), h.config);
        adv.Start();
        API_CHECK(WaitFor([&] { return adv.state() == AdvertState::Failed; }), "failed");
        int resets_after_first = h.fake.resets;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        API_CHECK(h.fake.resets - resets_after_first <= 1, "no tight retry loop");
        {
            std::lock_guard<std::recursive_mutex> lock(h.fake.poll_lock);
            h.fake.add_result = 0;
        }
        API_CHECK(WaitFor([] { return AddedCount() == 1; }), "registered once the failure clears");
        adv.Stop();
    }

    printf("\n-- scenario: the interface is re-created under a new index -> republished there --\n");
    {
        Harness h;
        MdnsAdvertiser adv(FakeApi(), h.config);
        adv.Start();
        WaitFor([] { return AddedCount() == 1; });
        h.ifindex = 7;
        adv.Poke();
        API_CHECK(WaitFor([] { return AddedCount() == 2; }), "registered again");
        API_CHECK(LastAdded().interface_index == 7, "on the new interface index");
        adv.Stop();
    }

    printf("\n-- scenario: IPv6-bound API is advertised over IPv6 --\n");
    {
        Harness h;
        h.config.address_family = AF_INET6;
        MdnsAdvertiser adv(FakeApi(), h.config);
        adv.Start();
        WaitFor([] { return AddedCount() == 1; });
        API_CHECK(LastAdded().protocol == AvahiApi::kProtoInet6, "AVAHI_PROTO_INET6");
        adv.Stop();
    }

    printf("\n-- scenario: without libavahi the loader fails cleanly (the bridge keeps running) --\n");
    {
        AvahiApi api;
        std::string error;
        bool loaded = pogohome_api::LoadSystemAvahiApi(api, error);
        API_CHECK(loaded || !error.empty(), "either loads every symbol or explains what is missing");
    }
    g_fake = nullptr;
}
