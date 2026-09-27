#include "pogohome_api/mdns_advertiser.hpp"

#include <cstdio>

#include <dlfcn.h>
#include <sys/socket.h>

namespace pogohome_api {

namespace {

constexpr size_t kMaxDnsLabelBytes = 63;
// Renames tried in one pass when avahi reports a local name collision.
constexpr int kMaxRenamesPerAttempt = 8;

template <typename Fn> bool LoadSymbol(void *library, const char *name, Fn &out, std::string &error)
{
    void *symbol = dlsym(library, name);
    if (symbol == nullptr) {
        error = std::string("symbol ") + name + " not found";
        return false;
    }
    out = reinterpret_cast<Fn>(symbol);
    return true;
}

} // namespace

std::vector<std::string> BuildBonjourTxt(const std::string &hub_id)
{
    return {
        "txtvers=1",
        std::string("api=") + kBonjourApiVersion,
        std::string("path=") + kBonjourApiPath,
        "hub_id=" + hub_id,
    };
}

std::string DefaultBonjourServiceName(const std::string &hostname)
{
    std::string host = hostname.substr(0, hostname.find('.'));
    std::string name = host.empty() ? std::string("Jogakbo Hub") : "Jogakbo Hub (" + host + ")";
    if (name.size() <= kMaxDnsLabelBytes) {
        return name;
    }
    size_t cut = kMaxDnsLabelBytes;
    // Never split a UTF-8 sequence: back up over continuation bytes.
    while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) {
        cut--;
    }
    return name.substr(0, cut);
}

bool LoadSystemAvahiApi(AvahiApi &api, std::string &error)
{
    // Deliberately never dlclose()d: the advertiser lives as long as the
    // process, and connectedhomeip's own Linux mDNS may share these libraries.
    void *common = dlopen("libavahi-common.so.3", RTLD_NOW | RTLD_LOCAL);
    if (common == nullptr) {
        const char *reason = dlerror();
        error = std::string("libavahi-common.so.3: ") + (reason != nullptr ? reason : "not loadable");
        return false;
    }
    void *client = dlopen("libavahi-client.so.3", RTLD_NOW | RTLD_LOCAL);
    if (client == nullptr) {
        const char *reason = dlerror();
        error = std::string("libavahi-client.so.3: ") + (reason != nullptr ? reason : "not loadable");
        return false;
    }
    AvahiApi loaded;
    bool ok = LoadSymbol(common, "avahi_threaded_poll_new", loaded.threaded_poll_new, error) &&
              LoadSymbol(common, "avahi_threaded_poll_free", loaded.threaded_poll_free, error) &&
              LoadSymbol(common, "avahi_threaded_poll_get", loaded.threaded_poll_get, error) &&
              LoadSymbol(common, "avahi_threaded_poll_start", loaded.threaded_poll_start, error) &&
              LoadSymbol(common, "avahi_threaded_poll_stop", loaded.threaded_poll_stop, error) &&
              LoadSymbol(common, "avahi_threaded_poll_lock", loaded.threaded_poll_lock, error) &&
              LoadSymbol(common, "avahi_threaded_poll_unlock", loaded.threaded_poll_unlock, error) &&
              LoadSymbol(common, "avahi_string_list_add", loaded.string_list_add, error) &&
              LoadSymbol(common, "avahi_string_list_free", loaded.string_list_free, error) &&
              LoadSymbol(common, "avahi_alternative_service_name", loaded.alternative_service_name, error) &&
              LoadSymbol(common, "avahi_free", loaded.free, error) &&
              LoadSymbol(common, "avahi_strerror", loaded.strerror, error) &&
              LoadSymbol(client, "avahi_client_new", loaded.client_new, error) &&
              LoadSymbol(client, "avahi_client_free", loaded.client_free, error) &&
              LoadSymbol(client, "avahi_client_errno", loaded.client_errno, error) &&
              LoadSymbol(client, "avahi_entry_group_new", loaded.entry_group_new, error) &&
              LoadSymbol(client, "avahi_entry_group_free", loaded.entry_group_free, error) &&
              LoadSymbol(client, "avahi_entry_group_reset", loaded.entry_group_reset, error) &&
              LoadSymbol(client, "avahi_entry_group_is_empty", loaded.entry_group_is_empty, error) &&
              LoadSymbol(client, "avahi_entry_group_commit", loaded.entry_group_commit, error) &&
              LoadSymbol(client, "avahi_entry_group_add_service_strlst", loaded.entry_group_add_service_strlst,
                         error);
    if (!ok) {
        return false;
    }
    api = loaded;
    return true;
}

const char *AdvertStateName(AdvertState state)
{
    switch (state) {
    case AdvertState::Stopped:
        return "stopped";
    case AdvertState::WaitingForDaemon:
        return "waiting for avahi-daemon";
    case AdvertState::Withdrawn:
        return "withdrawn (API not reachable)";
    case AdvertState::Registering:
        return "registering";
    case AdvertState::Established:
        return "advertised";
    case AdvertState::Failed:
        return "failed";
    }
    return "?";
}

MdnsAdvertiser::MdnsAdvertiser(AvahiApi api, AdvertiserConfig config)
    : api_(api), config_(std::move(config)), name_(config_.service_name)
{
}

MdnsAdvertiser::~MdnsAdvertiser()
{
    Stop();
}

bool MdnsAdvertiser::Start()
{
    if (started_) {
        return false;
    }
    started_ = true;
    poll_ = api_.threaded_poll_new();
    if (poll_ == nullptr) {
        std::fprintf(stderr, "pogohome_api: Bonjour: avahi_threaded_poll_new() failed -- not advertising\n");
        stopped_ = true;
        return false;
    }
    if (api_.threaded_poll_start(poll_) < 0) {
        std::fprintf(stderr, "pogohome_api: Bonjour: avahi_threaded_poll_start() failed -- not advertising\n");
        api_.threaded_poll_free(poll_);
        poll_ = nullptr;
        stopped_ = true;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        wake_ = true; // reconcile immediately, not after the first recheck_interval
    }
    supervisor_ = std::thread([this] { Run(); });
    return true;
}

void MdnsAdvertiser::Stop()
{
    if (!started_ || stopped_) {
        return;
    }
    stopped_ = true;
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        stop_ = true;
    }
    wake_cv_.notify_all();
    if (supervisor_.joinable()) {
        supervisor_.join();
    }
    // With avahi's event thread stopped, nothing else touches the client or
    // group; freeing the group withdraws the record right away (a blocking
    // D-Bus call that does not need the event loop).
    api_.threaded_poll_stop(poll_);
    bool was_published = group_ != nullptr && api_.entry_group_is_empty(group_) == 0;
    if (group_ != nullptr) {
        api_.entry_group_free(group_);
        group_ = nullptr;
    }
    if (client_ != nullptr) {
        api_.client_free(client_);
        client_ = nullptr;
    }
    api_.threaded_poll_free(poll_);
    poll_ = nullptr;
    if (was_published) {
        std::fprintf(stdout, "pogohome_api: Bonjour: withdrew '%s' (%s)\n", current_name().c_str(),
                     kBonjourServiceType);
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_ = AdvertState::Stopped;
}

void MdnsAdvertiser::Poke()
{
    Wake();
}

AdvertState MdnsAdvertiser::state() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

std::string MdnsAdvertiser::current_name() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return name_;
}

void MdnsAdvertiser::Wake()
{
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        wake_ = true;
    }
    wake_cv_.notify_all();
}

// Runs on avahi's event thread (or synchronously inside avahi_client_new()
// on the supervisor thread), with the poll lock held: only records the new
// state and wakes the supervisor, which does the actual work.
void MdnsAdvertiser::OnClientState(void *, int state, void *userdata)
{
    auto *self = static_cast<MdnsAdvertiser *>(userdata);
    self->client_state_ = state;
    self->Wake();
}

void MdnsAdvertiser::OnGroupState(void *group, int state, void *userdata)
{
    auto *self = static_cast<MdnsAdvertiser *>(userdata);
    if (group != self->group_ && self->group_ != nullptr) {
        return; // a group this advertiser already freed
    }
    self->group_state_ = state;
    if (state == AvahiApi::kGroupEstablished) {
        self->SetStateLocked(AdvertState::Established);
    } else if (state == AvahiApi::kGroupFailure) {
        // Retried after retry_delay, never in a tight loop.
        self->group_retry_at_ = std::chrono::steady_clock::now() + self->config_.retry_delay;
        self->SetStateLocked(AdvertState::Failed,
                             std::string("entry group failed: ") +
                                 self->api_.strerror(self->api_.client_errno(self->client_)));
    }
    self->Wake();
}

void MdnsAdvertiser::Run()
{
    while (true) {
        {
            std::unique_lock<std::mutex> lock(wake_mutex_);
            wake_cv_.wait_for(lock, config_.recheck_interval, [this] { return wake_ || stop_; });
            if (stop_) {
                return;
            }
            wake_ = false;
        }
        // Evaluated outside avahi's lock: these look at sockets and interfaces.
        bool available = config_.available ? config_.available() : true;
        unsigned interface_index = config_.interface_index ? config_.interface_index() : 0;

        api_.threaded_poll_lock(poll_);
        ReconcileLocked(available, interface_index, std::chrono::steady_clock::now());
        api_.threaded_poll_unlock(poll_);
    }
}

void MdnsAdvertiser::ReconcileLocked(bool available, unsigned interface_index,
                                     std::chrono::steady_clock::time_point now)
{
    if (client_ == nullptr) {
        if (now < client_retry_at_) {
            return;
        }
        int error = 0;
        client_state_ = AvahiApi::kClientConnecting;
        client_ = api_.client_new(api_.threaded_poll_get(poll_), AvahiApi::kClientFlagNoFail, &OnClientState, this,
                                  &error);
        if (client_ == nullptr) {
            client_retry_at_ = now + config_.retry_delay;
            SetStateLocked(AdvertState::WaitingForDaemon,
                           std::string("avahi_client_new: ") + api_.strerror(error));
            return;
        }
    }

    if (client_state_ == AvahiApi::kClientFailure) {
        std::string reason = api_.strerror(api_.client_errno(client_));
        DropGroupLocked();
        api_.client_free(client_);
        client_ = nullptr;
        client_retry_at_ = now + config_.retry_delay;
        SetStateLocked(AdvertState::WaitingForDaemon, "avahi client failed: " + reason);
        return;
    }
    if (client_state_ != AvahiApi::kClientRunning) {
        // Daemon absent or restarting (CONNECTING), or it is re-registering
        // the host name (REGISTERING/COLLISION): per avahi's rules nothing may
        // stay registered until it is RUNNING again.
        DropGroupLocked();
        SetStateLocked(AdvertState::WaitingForDaemon);
        return;
    }

    if (!available || interface_index == 0) {
        if (group_ != nullptr && api_.entry_group_is_empty(group_) == 0) {
            api_.entry_group_reset(group_);
            group_state_ = AvahiApi::kGroupUncommitted;
        }
        published_interface_ = 0;
        SetStateLocked(AdvertState::Withdrawn);
        return;
    }

    if (group_ != nullptr) {
        if (group_state_ == AvahiApi::kGroupCollision) {
            // Another host on the LAN already announces this instance name.
            std::string old_name = current_name();
            RenameLocked();
            std::fprintf(stderr, "pogohome_api: Bonjour: service name '%s' is taken on the network, renaming to '%s'\n",
                         old_name.c_str(), current_name().c_str());
            api_.entry_group_reset(group_);
            group_state_ = AvahiApi::kGroupUncommitted;
        } else if (group_state_ == AvahiApi::kGroupFailure) {
            if (now < group_retry_at_) {
                return;
            }
            api_.entry_group_reset(group_);
            group_state_ = AvahiApi::kGroupUncommitted;
        } else if (published_interface_ != interface_index && api_.entry_group_is_empty(group_) == 0) {
            // The interface was re-created under a new index.
            api_.entry_group_reset(group_);
            group_state_ = AvahiApi::kGroupUncommitted;
        }
    }

    if (group_ == nullptr) {
        group_state_ = AvahiApi::kGroupUncommitted;
        group_ = api_.entry_group_new(client_, &OnGroupState, this);
        if (group_ == nullptr) {
            group_retry_at_ = now + config_.retry_delay;
            SetStateLocked(AdvertState::Failed,
                           std::string("avahi_entry_group_new: ") + api_.strerror(api_.client_errno(client_)));
            return;
        }
    }

    if (api_.entry_group_is_empty(group_) != 0) {
        AddServiceLocked(interface_index, now);
    }
}

void MdnsAdvertiser::AddServiceLocked(unsigned interface_index, std::chrono::steady_clock::time_point now)
{
    void *txt = nullptr;
    for (const auto &entry : config_.txt) {
        txt = api_.string_list_add(txt, entry.c_str());
    }
    int protocol = config_.address_family == AF_INET6 ? AvahiApi::kProtoInet6 : AvahiApi::kProtoInet;

    int rc = 0;
    for (int attempt = 0; attempt < kMaxRenamesPerAttempt; attempt++) {
        std::string name = current_name();
        rc = api_.entry_group_add_service_strlst(group_, static_cast<int>(interface_index), protocol, 0, name.c_str(),
                                                 kBonjourServiceType, nullptr, nullptr, config_.port, txt);
        if (rc != AvahiApi::kErrCollision) {
            break;
        }
        // Another client of this same avahi-daemon already registered it.
        RenameLocked();
        std::fprintf(stderr, "pogohome_api: Bonjour: service name '%s' is taken on this host, trying '%s'\n",
                     name.c_str(), current_name().c_str());
    }
    if (txt != nullptr) {
        api_.string_list_free(txt);
    }
    if (rc >= 0) {
        rc = api_.entry_group_commit(group_);
    }
    if (rc < 0) {
        api_.entry_group_reset(group_);
        group_state_ = AvahiApi::kGroupFailure;
        group_retry_at_ = now + config_.retry_delay;
        SetStateLocked(AdvertState::Failed, std::string("registering the service: ") + api_.strerror(rc));
        return;
    }
    published_interface_ = interface_index;
    if (group_state_ != AvahiApi::kGroupEstablished) {
        SetStateLocked(AdvertState::Registering);
    }
}

void MdnsAdvertiser::DropGroupLocked()
{
    if (group_ != nullptr) {
        void *group = group_;
        group_ = nullptr;
        api_.entry_group_free(group);
    }
    group_state_ = AvahiApi::kGroupUncommitted;
    published_interface_ = 0;
}

void MdnsAdvertiser::RenameLocked()
{
    std::string current = current_name();
    char *alternative = api_.alternative_service_name(current.c_str());
    if (alternative == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    name_ = alternative;
    api_.free(alternative);
}

void MdnsAdvertiser::SetStateLocked(AdvertState state, const std::string &detail)
{
    AdvertState previous;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        previous = state_;
        state_ = state;
        name = name_;
    }
    if (previous == state) {
        return; // log transitions only, never once per recheck
    }
    FILE *out = (state == AdvertState::Failed || state == AdvertState::WaitingForDaemon) ? stderr : stdout;
    if (state == AdvertState::Established) {
        std::fprintf(out, "pogohome_api: Bonjour: advertising '%s' (%s) on %s, port %u\n", name.c_str(),
                     kBonjourServiceType, config_.interface_name.c_str(), static_cast<unsigned>(config_.port));
    } else {
        std::fprintf(out, "pogohome_api: Bonjour: %s%s%s\n", AdvertStateName(state), detail.empty() ? "" : " -- ",
                     detail.c_str());
    }
}

} // namespace pogohome_api
