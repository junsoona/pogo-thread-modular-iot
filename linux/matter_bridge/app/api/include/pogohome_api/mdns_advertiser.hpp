// Bonjour (DNS-SD over mDNS) advertisement of the PogoHome Local API v1 as
// `_jogakbo._tcp`, so a client on the same LAN can find this Linux sensor hub
// without being told its address (see docs/pogohome-api-v1.md "Bonjour").
//
// Only the HTTP API is advertised -- never a Power Base or a sensor module,
// and nothing about Thread or Matter. The records are published through the
// host's existing avahi-daemon (the same daemon connectedhomeip's Linux
// platform mDNS already talks to) as one entry group owned by a separate
// avahi client connection. That group only ever holds this one service
// record: this process never registers a host name, never touches the
// Matter `_matter._tcp`/`_matterc._udp` records or OTBR's
// `_meshcop._udp`/SRP records, and never publishes on any interface but the
// API's own (--api-interface).
//
// Lifecycle:
//   - Published only while the API server is accepting connections AND its
//     bound address is still present on --api-interface (`available`,
//     re-checked every `recheck_interval`); withdrawn as soon as either stops
//     being true, and republished when it is true again.
//   - Stop() (bridge shutdown, run before the API server itself stops)
//     withdraws the record; if the process dies without Stop(), avahi-daemon
//     drops everything this client registered when its D-Bus connection
//     closes, so a dead bridge never leaves a stale advertisement behind.
//   - avahi-daemon restarting, or not running yet at startup, is survived:
//     the client is created with AVAHI_CLIENT_NO_FAIL and the record is
//     registered again once the daemon is back.
//   - A service name conflict on the LAN (another host already announcing
//     the same instance name) is resolved with avahi's own alternative name
//     ("Jogakbo Hub (host) #2"). Only the display name changes; clients
//     identify the hub by the `hub_id` TXT key, never by the name.
//
// Threads: one avahi threaded poll (avahi's own event thread, which runs the
// client/group callbacks) plus one supervisor thread owned here. Both are
// joined by Stop(); nothing is detached and no child process is spawned.
//
// libavahi-client is loaded with dlopen() (LoadSystemAvahiApi) rather than
// linked, so this adds no build dependency and a host without Avahi still
// runs the bridge and its API -- it only logs that it is not advertising.
// The host tests drive the same engine through a fake AvahiApi.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pogohome_api {

constexpr const char kBonjourServiceType[] = "_jogakbo._tcp";
// TXT `api`: the API major version the advertised server speaks, i.e. its
// health/snapshot `schema_version`.
constexpr const char kBonjourApiVersion[] = "1";
// TXT `path`: where the API's routes live under the advertised host:port.
constexpr const char kBonjourApiPath[] = "/api/v1";

// txtvers=1, api=<kBonjourApiVersion>, path=<kBonjourApiPath>, hub_id=<hub_id>.
std::vector<std::string> BuildBonjourTxt(const std::string &hub_id);

// "Jogakbo Hub (<short host name>)", cut to the 63-byte DNS label limit on a
// UTF-8 character boundary. A display name only -- see hub_identity.hpp for
// how a hub is identified.
std::string DefaultBonjourServiceName(const std::string &hostname);

// The subset of libavahi-client/libavahi-common this advertiser uses, as
// plain function pointers over opaque handles. Values mirror avahi's stable
// public ABI (libavahi-client.so.3), see the constants below.
struct AvahiApi {
    using ClientCallback = void (*)(void *client, int state, void *userdata);
    using GroupCallback = void (*)(void *group, int state, void *userdata);

    void *(*threaded_poll_new)() = nullptr;
    void (*threaded_poll_free)(void *poll) = nullptr;
    const void *(*threaded_poll_get)(void *poll) = nullptr;
    int (*threaded_poll_start)(void *poll) = nullptr;
    int (*threaded_poll_stop)(void *poll) = nullptr;
    void (*threaded_poll_lock)(void *poll) = nullptr;
    void (*threaded_poll_unlock)(void *poll) = nullptr;

    void *(*client_new)(const void *poll_api, int flags, ClientCallback callback, void *userdata,
                        int *error) = nullptr;
    void (*client_free)(void *client) = nullptr;
    int (*client_errno)(void *client) = nullptr;

    void *(*entry_group_new)(void *client, GroupCallback callback, void *userdata) = nullptr;
    int (*entry_group_free)(void *group) = nullptr;
    int (*entry_group_reset)(void *group) = nullptr;
    int (*entry_group_is_empty)(void *group) = nullptr;
    int (*entry_group_commit)(void *group) = nullptr;
    int (*entry_group_add_service_strlst)(void *group, int interface_index, int protocol, int flags,
                                          const char *name, const char *type, const char *domain,
                                          const char *host, uint16_t port, void *txt) = nullptr;

    void *(*string_list_add)(void *list, const char *text) = nullptr;
    void (*string_list_free)(void *list) = nullptr;
    char *(*alternative_service_name)(const char *name) = nullptr;
    void (*free)(void *p) = nullptr;
    const char *(*strerror)(int error) = nullptr;

    // avahi-client/client.h, avahi-client/publish.h, avahi-common/*.h
    static constexpr int kClientRegistering = 1; // AVAHI_CLIENT_S_REGISTERING
    static constexpr int kClientRunning = 2;     // AVAHI_CLIENT_S_RUNNING
    static constexpr int kClientCollision = 3;   // AVAHI_CLIENT_S_COLLISION
    static constexpr int kClientFailure = 100;   // AVAHI_CLIENT_FAILURE
    static constexpr int kClientConnecting = 101; // AVAHI_CLIENT_CONNECTING
    static constexpr int kClientFlagNoFail = 2;  // AVAHI_CLIENT_NO_FAIL

    static constexpr int kGroupUncommitted = 0; // AVAHI_ENTRY_GROUP_UNCOMMITED
    static constexpr int kGroupRegistering = 1; // AVAHI_ENTRY_GROUP_REGISTERING
    static constexpr int kGroupEstablished = 2; // AVAHI_ENTRY_GROUP_ESTABLISHED
    static constexpr int kGroupCollision = 3;   // AVAHI_ENTRY_GROUP_COLLISION
    static constexpr int kGroupFailure = 4;     // AVAHI_ENTRY_GROUP_FAILURE

    static constexpr int kProtoInet = 0;  // AVAHI_PROTO_INET
    static constexpr int kProtoInet6 = 1; // AVAHI_PROTO_INET6

    static constexpr int kErrCollision = -8; // AVAHI_ERR_COLLISION
};

// Loads libavahi-common.so.3 and libavahi-client.so.3 with dlopen() and fills
// `api`. False (with `error` set) when either library or any symbol is
// missing.
bool LoadSystemAvahiApi(AvahiApi &api, std::string &error);

struct AdvertiserConfig {
    std::string interface_name; // --api-interface; logged only
    int address_family = 0;     // AF_INET or AF_INET6 of the API's bound address
    uint16_t port = 0;          // the API's real bound port
    std::string service_name;   // preferred instance name (see DefaultBonjourServiceName)
    std::vector<std::string> txt;

    // Whether the API can take requests right now (server accepting, bound
    // address still on the interface). Called from the supervisor thread.
    std::function<bool()> available;
    // The interface's current index (0 = gone). Called from the supervisor
    // thread; an index change republishes on the new index.
    std::function<unsigned()> interface_index;

    std::chrono::milliseconds recheck_interval = std::chrono::seconds(5);
    // Wait before recreating a failed client or retrying a failed group.
    std::chrono::milliseconds retry_delay = std::chrono::seconds(5);
};

enum class AdvertState {
    Stopped,
    WaitingForDaemon, // no running avahi client (daemon absent/restarting, host name re-registration)
    Withdrawn,        // the API is not reachable; nothing is published
    Registering,      // committed, waiting for avahi to probe the name
    Established,      // announced on the LAN
    Failed,           // the last registration attempt failed; retrying after retry_delay
};

const char *AdvertStateName(AdvertState state);

class MdnsAdvertiser {
public:
    MdnsAdvertiser(AvahiApi api, AdvertiserConfig config);
    ~MdnsAdvertiser();

    MdnsAdvertiser(const MdnsAdvertiser &) = delete;
    MdnsAdvertiser &operator=(const MdnsAdvertiser &) = delete;

    // Creates and starts avahi's threaded poll and the supervisor thread.
    // False if the poll could not be created or started (nothing is left
    // running then). Call at most once.
    bool Start();

    // Withdraws the record and joins both threads. Idempotent.
    void Stop();

    // Re-evaluates now instead of at the next recheck_interval tick.
    void Poke();

    AdvertState state() const;
    // The instance name currently registered or being registered (it can
    // differ from config.service_name after a collision).
    std::string current_name() const;

private:
    static void OnClientState(void *client, int state, void *userdata);
    static void OnGroupState(void *group, int state, void *userdata);

    void Run();
    void Wake();
    // All of the below run with avahi's poll lock held.
    void ReconcileLocked(bool available, unsigned interface_index, std::chrono::steady_clock::time_point now);
    void AddServiceLocked(unsigned interface_index, std::chrono::steady_clock::time_point now);
    void DropGroupLocked();
    void RenameLocked();
    void SetStateLocked(AdvertState state, const std::string &detail = std::string());

    AvahiApi api_;
    AdvertiserConfig config_;

    void *poll_ = nullptr;
    void *client_ = nullptr;
    void *group_ = nullptr;
    int client_state_ = AvahiApi::kClientConnecting;
    int group_state_ = AvahiApi::kGroupUncommitted;
    unsigned published_interface_ = 0;
    std::string name_;
    std::chrono::steady_clock::time_point client_retry_at_{};
    std::chrono::steady_clock::time_point group_retry_at_{};

    mutable std::mutex state_mutex_; // guards state_ and name_ for readers
    AdvertState state_ = AdvertState::Stopped;

    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    bool wake_ = false;
    bool stop_ = false;
    std::thread supervisor_;
    bool started_ = false;
    bool stopped_ = false;
};

} // namespace pogohome_api
