#include "pogohome_api/enrollment.hpp"

#include <algorithm>
#include <cstdio>

#include "pogohome_api/crypto_util.hpp"
#include "pogohome_api/otbr_helper_protocol.hpp"

namespace pogohome_api {

namespace {

int HexValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

// A failed Commissioner call's session error code.
const char *CommissionerError(CommissionerStatus status, bool adding_joiner)
{
    switch (status) {
    case CommissionerStatus::Unavailable:
        return "otbr_unavailable";
    case CommissionerStatus::Timeout:
        return "otbr_timeout";
    case CommissionerStatus::Rejected:
    case CommissionerStatus::InvalidState:
    case CommissionerStatus::NotFound:
        return adding_joiner ? "joiner_rejected" : "commissioner_rejected";
    case CommissionerStatus::Ok:
    case CommissionerStatus::Failed:
    case CommissionerStatus::Unsupported:
        break;
    }
    return "helper_failed";
}

// Why a session that asked for a bridge endpoint did not get one.
const char *BridgeEndpointError(CommissionerStatus status)
{
    switch (status) {
    case CommissionerStatus::Timeout:
        return "bridge_endpoint_timeout";
    case CommissionerStatus::Unsupported:
        // The installed root helper predates the mesh-local-eid operation.
        return "bridge_endpoint_unsupported";
    case CommissionerStatus::Unavailable:
    case CommissionerStatus::Rejected:
    case CommissionerStatus::InvalidState:
    case CommissionerStatus::NotFound:
        return "bridge_endpoint_unavailable";
    case CommissionerStatus::Ok:
    case CommissionerStatus::Failed:
        break;
    }
    // The helper answered, but not with one usable Mesh-Local EID.
    return "bridge_endpoint_invalid";
}

std::string ShortId(const std::string &session_id)
{
    return session_id.substr(0, 8);
}

} // namespace

const char *EnrollmentStateName(EnrollmentState state)
{
    switch (state) {
    case EnrollmentState::Preparing:
        return "preparing";
    case EnrollmentState::WaitingForHeartbeat:
        return "waiting_for_heartbeat";
    case EnrollmentState::HeartbeatReceived:
        return "heartbeat_received";
    case EnrollmentState::Cancelled:
        return "cancelled";
    case EnrollmentState::Expired:
        return "expired";
    case EnrollmentState::Failed:
        return "failed";
    }
    return "failed";
}

const char *JoinerEntryName(JoinerEntry entry)
{
    switch (entry) {
    case JoinerEntry::None:
        return "none";
    case JoinerEntry::Adding:
        return "adding";
    case JoinerEntry::Allowed:
        return "allowed";
    case JoinerEntry::Removing:
        return "removing";
    case JoinerEntry::Removed:
        return "removed";
    case JoinerEntry::RemoveFailed:
        return "remove_failed";
    }
    return "none";
}

bool IsFinalState(EnrollmentState state)
{
    return state == EnrollmentState::HeartbeatReceived || state == EnrollmentState::Cancelled ||
           state == EnrollmentState::Expired || state == EnrollmentState::Failed;
}

bool NormalizeEui64(const std::string &input, std::string &out, ingest::Uuid *uuid)
{
    out.clear();
    if (input.size() != 16) {
        return false;
    }
    std::string lower = input;
    for (char &c : lower) {
        if (c >= 'A' && c <= 'F') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    if (!IsValidEui64(lower)) {
        return false;
    }
    if (uuid != nullptr) {
        for (size_t i = 0; i < 8; i++) {
            (*uuid)[i] = static_cast<uint8_t>(HexValue(lower[2 * i]) * 16 + HexValue(lower[2 * i + 1]));
        }
    }
    out = lower;
    return true;
}

EnrollmentManager::EnrollmentManager(ingest::Registry &registry, std::shared_ptr<Commissioner> commissioner,
                                     EnrollmentConfig config)
    : registry_(registry), commissioner_(std::move(commissioner)), config_(std::move(config))
{
}

EnrollmentManager::~EnrollmentManager()
{
    Stop();
}

std::chrono::steady_clock::time_point EnrollmentManager::NowSteady() const
{
    return config_.now_steady ? config_.now_steady() : std::chrono::steady_clock::now();
}

int64_t EnrollmentManager::NowUnixMs() const
{
    if (config_.now_unix_ms) {
        return config_.now_unix_ms();
    }
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

EnrollmentStartOutcome EnrollmentManager::Start(const std::string &device_id, const std::string &eui64,
                                                std::string &pskd, EnrollmentSession &out,
                                                const EnrollmentStartOptions &options)
{
    // The PSKd lives on the heap in one place until the worker has used it.
    auto secret = std::make_unique<std::string>(pskd);
    SecureWipe(pskd);
    out = EnrollmentSession();

    ingest::Uuid uuid{};
    std::string normalized;
    if (!NormalizeEui64(eui64, normalized, &uuid) || !IsValidPskd(*secret) ||
        (options.bridge_endpoint_version != 0 && options.bridge_endpoint_version != kBridgeEndpointVersion)) {
        SecureWipe(*secret);
        return EnrollmentStartOutcome::InternalError; // the HTTP layer validates first
    }
    const bool deliver_endpoint = options.bridge_endpoint_version == kBridgeEndpointVersion;

    std::thread previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            SecureWipe(*secret);
            return EnrollmentStartOutcome::Stopping;
        }
        if (worker_running_) {
            if (session_.device_id == device_id) {
                out = CopyLocked();
            }
            SecureWipe(*secret);
            return EnrollmentStartOutcome::Busy;
        }
        if (deliver_endpoint && config_.bridge_port == 0) {
            SecureWipe(*secret);
            return EnrollmentStartOutcome::BridgeEndpointUnavailable;
        }
        const auto now = NowSteady();
        ingest::PowerBaseState base;
        if (registry_.get_power_base(uuid, base, now) && base.online) {
            SecureWipe(*secret);
            return EnrollmentStartOutcome::AlreadyConnected;
        }
        std::string session_id = RandomHex(16);
        if (session_id.empty()) {
            SecureWipe(*secret);
            return EnrollmentStartOutcome::InternalError;
        }

        previous = std::move(worker_); // finished its cleanup already
        session_ = Session();
        session_.session_id = session_id;
        session_.device_id = device_id;
        session_.eui64 = normalized;
        session_.uuid = uuid;
        session_.state = EnrollmentState::Preparing;
        session_.joiner_entry = JoinerEntry::None;
        session_.bridge_endpoint_requested = deliver_endpoint;
        session_.window_seconds = static_cast<int>(config_.window.count());
        session_.created_at_unix_ms = NowUnixMs();
        session_.expires_at_unix_ms =
            session_.created_at_unix_ms + std::chrono::duration_cast<std::chrono::milliseconds>(config_.window).count();
        session_.deadline = now + config_.window;
        // Any heartbeat after this read is new; everything before it -- a
        // cached online state, an old heartbeat with the same timestamp --
        // is not.
        session_.heartbeat_baseline = registry_.heartbeat_serial();
        has_session_ = true;
        worker_running_ = true;
        worker_ = std::thread([this, s = std::move(secret)]() mutable {
            Worker(*s);
            SecureWipe(*s);
        });
        out = CopyLocked();
        std::fprintf(stdout, "pogohome_api: enrollment %s: device %s started it for Power Base %s (%d s window%s)\n",
                     ShortId(session_id).c_str(), device_id.c_str(), normalized.c_str(), session_.window_seconds,
                     deliver_endpoint ? ", with bridge endpoint delivery" : "");
    }
    if (previous.joinable()) {
        previous.join();
    }
    return EnrollmentStartOutcome::Started;
}

void EnrollmentManager::Worker(std::string &pskd)
{
    std::string eui64;
    std::string id;
    bool deliver_endpoint = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        eui64 = session_.eui64;
        id = ShortId(session_.session_id);
        deliver_endpoint = session_.bridge_endpoint_requested;
    }

    // 0. The bridge endpoint, for a session that asked for one -- BEFORE
    // anything is changed on the OTBR. Read-only, and like every
    // Commissioner call made with no lock held. A session that fails here,
    // or that was cancelled, expired or stopped meanwhile, goes on to
    // neither the Commissioner nor the Joiner entry.
    bool proceed = true;
    if (deliver_endpoint) {
        std::string address;
        CommissionerStatus endpoint_status = commissioner_->QueryMeshLocalEid(address);
        std::string canonical;
        // The Commissioner's word is checked again here: this is the value
        // that leaves the hub.
        const bool usable =
            endpoint_status == CommissionerStatus::Ok && ParseMeshLocalEid(address, nullptr, &canonical);
        std::lock_guard<std::mutex> lock(mutex_);
        if (IsFinalState(session_.state)) {
            proceed = false; // it ended while the address was being read: nothing more happens
        } else if (!usable) {
            std::fprintf(stderr, "pogohome_api: enrollment %s: no bridge endpoint (%s) -- nothing was added to the OTBR\n",
                         id.c_str(), CommissionerStatusName(endpoint_status));
            FinishLocked(EnrollmentState::Failed, BridgeEndpointError(endpoint_status));
            proceed = false;
        } else if (NowSteady() >= session_.deadline) {
            FinishLocked(EnrollmentState::Expired);
            proceed = false;
        } else {
            session_.bridge_endpoint.version = kBridgeEndpointVersion;
            session_.bridge_endpoint.address = canonical;
            session_.bridge_endpoint.port = config_.bridge_port;
            session_.has_bridge_endpoint = true;
            // The address is not logged; the port is the program's own --port.
            std::fprintf(stdout, "pogohome_api: enrollment %s: bridge endpoint fixed for this session (port %u)\n",
                         id.c_str(), static_cast<unsigned>(config_.bridge_port));
        }
    }

    // 1. The Commissioner. No lock is held during any Commissioner call.
    bool started = false;
    CommissionerStatus status = CommissionerStatus::Ok;
    if (proceed) {
        status = commissioner_->EnsureActive(started);
    }
    bool add_joiner = false;
    int timeout_s = 0;
    if (proceed) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsFinalState(session_.state)) {
            if (status != CommissionerStatus::Ok) {
                std::fprintf(stderr, "pogohome_api: enrollment %s: Commissioner not available (%s)\n", id.c_str(),
                             CommissionerStatusName(status));
                FinishLocked(EnrollmentState::Failed, CommissionerError(status, false));
            } else {
                if (started) {
                    std::fprintf(stdout, "pogohome_api: enrollment %s: started the OTBR Commissioner\n", id.c_str());
                }
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(session_.deadline - NowSteady());
                if (session_.bridge_endpoint_requested && !session_.has_bridge_endpoint) {
                    // Cannot happen (step 0 either fixed it or ended the
                    // session); a Joiner entry is never opened without it.
                    FinishLocked(EnrollmentState::Failed, "bridge_endpoint_unavailable");
                } else if (remaining < config_.min_joiner_timeout) {
                    FinishLocked(EnrollmentState::Failed, "window_too_short");
                } else {
                    // The OTBR's own timeout ends with this session's window.
                    timeout_s = static_cast<int>((remaining.count() + 999) / 1000);
                    timeout_s = std::min(timeout_s, kMaxJoinerTimeoutSeconds);
                    session_.joiner_entry = JoinerEntry::Adding;
                    add_joiner = true;
                }
            }
        }
    }

    // 2. The Joiner entry for exactly this EUI-64.
    if (add_joiner) {
        status = commissioner_->AddJoiner(eui64, pskd, timeout_s);
        std::lock_guard<std::mutex> lock(mutex_);
        if (status == CommissionerStatus::Ok) {
            session_.joiner_entry = JoinerEntry::Allowed;
            if (!IsFinalState(session_.state)) {
                session_.state = EnrollmentState::WaitingForHeartbeat;
                std::fprintf(stdout, "pogohome_api: enrollment %s: Joiner %s allowed for %d s\n", id.c_str(),
                             eui64.c_str(), timeout_s);
            }
        } else if (!IsFinalState(session_.state)) {
            std::fprintf(stderr, "pogohome_api: enrollment %s: adding the Joiner failed (%s)\n", id.c_str(),
                         CommissionerStatusName(status));
            FinishLocked(EnrollmentState::Failed, CommissionerError(status, true));
        }
    }
    SecureWipe(pskd);

    // 3. Wait for the heartbeat, a cancel, the deadline or Stop().
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!IsFinalState(session_.state)) {
            if (CheckHeartbeatLocked()) {
                break;
            }
            const auto now = NowSteady();
            if (now >= session_.deadline) {
                FinishLocked(EnrollmentState::Expired);
                break;
            }
            auto wait = std::min<std::chrono::steady_clock::duration>(config_.poll_interval, session_.deadline - now);
            cv_.wait_for(lock, wait);
        }
        std::fprintf(stdout, "pogohome_api: enrollment %s: %s\n", id.c_str(), EnrollmentStateName(session_.state));
        if (add_joiner) {
            session_.joiner_entry = JoinerEntry::Removing;
        }
    }

    // 4. Close exactly this EUI-64's entry, whatever the outcome. Even a
    // failed or timed-out add is followed by a remove: the entry may exist.
    if (add_joiner) {
        status = commissioner_->RemoveJoiner(eui64);
        std::lock_guard<std::mutex> lock(mutex_);
        if (status == CommissionerStatus::Ok || status == CommissionerStatus::NotFound) {
            session_.joiner_entry = JoinerEntry::Removed;
        } else {
            session_.joiner_entry = JoinerEntry::RemoveFailed;
            std::fprintf(stderr,
                         "pogohome_api: enrollment %s: could not remove Joiner %s (%s); the OTBR closes it by itself "
                         "within %d s\n",
                         id.c_str(), eui64.c_str(), CommissionerStatusName(status), timeout_s);
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    session_.finished_at = NowSteady();
    worker_running_ = false;
    cv_.notify_all();
}

bool EnrollmentManager::CheckHeartbeatLocked()
{
    ingest::PowerBaseState base;
    if (registry_.get_power_base(session_.uuid, base, NowSteady()) &&
        base.last_heartbeat_serial > session_.heartbeat_baseline) {
        session_.heartbeat_received_at_unix_ms = NowUnixMs();
        FinishLocked(EnrollmentState::HeartbeatReceived);
        return true;
    }
    return false;
}

void EnrollmentManager::FinishLocked(EnrollmentState state, const char *error)
{
    if (IsFinalState(session_.state)) {
        return;
    }
    session_.state = state;
    session_.error = error;
    cv_.notify_all();
}

EnrollmentSession EnrollmentManager::CopyLocked() const
{
    EnrollmentSession out = session_;
    const bool final_state = IsFinalState(session_.state);
    if (final_state) {
        out.expires_in_ms = 0;
    } else {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(session_.deadline - NowSteady()).count();
        out.expires_in_ms = left > 0 ? left : 0;
    }
    out.finished = final_state && !worker_running_;
    return out;
}

bool EnrollmentManager::OwnedByLocked(const std::string &device_id, const std::string &session_id) const
{
    return has_session_ && !device_id.empty() && session_.device_id == device_id &&
           ConstantTimeEquals(session_.session_id, session_id);
}

bool EnrollmentManager::Get(const std::string &device_id, const std::string &session_id, EnrollmentSession &out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (has_session_ && !worker_running_ && NowSteady() - session_.finished_at > config_.retain_finished) {
        has_session_ = false;
    }
    if (!OwnedByLocked(device_id, session_id)) {
        return false;
    }
    out = CopyLocked();
    return true;
}

EnrollmentCancelOutcome EnrollmentManager::Cancel(const std::string &device_id, const std::string &session_id,
                                                  EnrollmentSession &out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!OwnedByLocked(device_id, session_id)) {
        return EnrollmentCancelOutcome::NotFound;
    }
    // A heartbeat that already reached the Registry wins over the cancel: the
    // Base has joined and the bridge heard it, which a cancel cannot undo.
    if (IsFinalState(session_.state) || CheckHeartbeatLocked()) {
        out = CopyLocked();
        return EnrollmentCancelOutcome::AlreadyFinal;
    }
    FinishLocked(EnrollmentState::Cancelled);
    std::fprintf(stdout, "pogohome_api: enrollment %s: cancelled by device %s\n", ShortId(session_.session_id).c_str(),
                 device_id.c_str());
    out = CopyLocked();
    return EnrollmentCancelOutcome::Cancelled;
}

void EnrollmentManager::Stop()
{
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        if (has_session_ && !IsFinalState(session_.state)) {
            FinishLocked(EnrollmentState::Failed, "bridge_stopping");
        }
        cv_.notify_all();
        worker = std::move(worker_);
    }
    // The worker finishes its current Commissioner call (bounded by its
    // timeout), then removes the Joiner entry, then exits.
    if (worker.joinable()) {
        worker.join();
    }
}

} // namespace pogohome_api
