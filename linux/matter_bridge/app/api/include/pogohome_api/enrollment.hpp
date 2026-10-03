// Power Base enrollment sessions (docs/pogohome-api-v1.md "Power Base
// enrollment"): a paired device asks the hub to let ONE new Power Base,
// named by its exact EUI-64, join the Thread network with a PSKd the device
// chose, for a short time.
//
// One session at a time. A session:
//   1. makes sure the OTBR Commissioner is active (never stopping one that
//      already was);
//   2. allows exactly that EUI-64 as a Joiner, for the rest of the window
//      (default 120 s), so the OTBR closes the entry by itself even if this
//      process dies;
//   3. waits until the Registry receives a heartbeat from a Power Base with
//      that EUI-64 as its uuid -- a heartbeat that arrived AFTER the session
//      started (Registry::heartbeat_serial()), never a cached online state;
//   4. removes that one Joiner entry when it ends for any reason (success,
//      cancel, expiry, failure, shutdown); "NotFound" counts as removed.
//
// "heartbeat_received" means this Linux bridge heard the Base. It does not
// mean every sensor behind it works.
//
// Threading: all session state is under this class's own mutex. Commissioner
// calls (which run a subprocess for seconds) happen on the session's worker
// thread with NO lock held; the Registry is only read with its own short
// getters. The first transition to a final state wins; after that nothing
// changes the state again (only the Joiner entry cleanup progresses).
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "ingest/registry.hpp"
#include "pogohome_api/commissioner.hpp"

namespace pogohome_api {

enum class EnrollmentState {
    Preparing,           // starting the Commissioner / adding the Joiner entry
    WaitingForHeartbeat, // the Joiner entry is open; no new heartbeat yet
    HeartbeatReceived,   // final: the bridge received a new heartbeat from that EUI-64
    Cancelled,           // final: the device that started it cancelled it
    Expired,             // final: the window ended without a new heartbeat
    Failed,              // final: see EnrollmentSession::error
};

enum class JoinerEntry {
    None,         // never added
    Adding,       // the add command is running
    Allowed,      // open on the OTBR
    Removing,     // the remove command is running
    Removed,      // removed, or already gone (NotFound)
    RemoveFailed, // could not remove; the OTBR closes it at its own timeout
};

const char *EnrollmentStateName(EnrollmentState state);
const char *JoinerEntryName(JoinerEntry entry);
bool IsFinalState(EnrollmentState state);

struct EnrollmentSession {
    std::string session_id; // 32 lowercase hex (128 random bits)
    std::string device_id;  // the paired device that started it
    std::string eui64;      // 16 lowercase hex
    EnrollmentState state = EnrollmentState::Preparing;
    JoinerEntry joiner_entry = JoinerEntry::None;
    std::string error; // Failed only, a fixed code
    int window_seconds = 0;
    int64_t created_at_unix_ms = 0;
    int64_t expires_at_unix_ms = 0; // informational; the deadline is monotonic
    int64_t expires_in_ms = 0;      // at the time of the copy; 0 once final
    int64_t heartbeat_received_at_unix_ms = 0; // 0 = not received
    // Final and the Joiner entry is settled (not Adding/Allowed/Removing).
    bool finished = false;
};

struct EnrollmentConfig {
    std::chrono::seconds window{120};
    // How often the worker checks the Registry for the heartbeat.
    std::chrono::milliseconds poll_interval{250};
    // How long a finished session stays readable by its device.
    std::chrono::milliseconds retain_finished{10 * 60 * 1000};
    // Joiner entries shorter than this are not worth opening.
    std::chrono::seconds min_joiner_timeout{10};
    std::function<int64_t()> now_unix_ms;
    std::function<std::chrono::steady_clock::time_point()> now_steady;
};

enum class EnrollmentStartOutcome {
    Started,
    Busy,             // another session is running or still cleaning up
    AlreadyConnected, // a Base with this EUI-64 is online right now
    Stopping,         // the bridge is shutting down
    InternalError,    // no randomness for the session id
};

enum class EnrollmentCancelOutcome {
    Cancelled,
    AlreadyFinal, // it had already ended (e.g. the heartbeat came first)
    NotFound,     // no such session for this device
};

// Accepts 16 hex digits in either case; `out` is lowercase. Rejects '*',
// separators, and all-zero. `uuid` (optional) gets the 8 bytes.
bool NormalizeEui64(const std::string &input, std::string &out, ingest::Uuid *uuid = nullptr);

class EnrollmentManager {
public:
    EnrollmentManager(ingest::Registry &registry, std::shared_ptr<Commissioner> commissioner,
                      EnrollmentConfig config = {});
    ~EnrollmentManager();
    EnrollmentManager(const EnrollmentManager &) = delete;
    EnrollmentManager &operator=(const EnrollmentManager &) = delete;

    // `eui64` normalized, `pskd` valid (the caller checked both); `pskd` is
    // wiped before this returns. On Busy, `out` is the running session only
    // if `device_id` started it (otherwise left empty).
    EnrollmentStartOutcome Start(const std::string &device_id, const std::string &eui64, std::string &pskd,
                                 EnrollmentSession &out);
    // False when there is no such session for this device (another device's
    // session is indistinguishable from a missing one).
    bool Get(const std::string &device_id, const std::string &session_id, EnrollmentSession &out);
    EnrollmentCancelOutcome Cancel(const std::string &device_id, const std::string &session_id,
                                   EnrollmentSession &out);

    // Ends a running session (Failed, "bridge_stopping"), waits for its
    // Joiner entry cleanup, joins the worker. Idempotent. After this, Start()
    // answers Stopping.
    void Stop();

private:
    struct Session : EnrollmentSession {
        std::chrono::steady_clock::time_point deadline;
        std::chrono::steady_clock::time_point finished_at;
        uint64_t heartbeat_baseline = 0;
        ingest::Uuid uuid{};
    };

    // `pskd` is wiped once the Joiner add has used it.
    void Worker(std::string &pskd);
    // Caller holds mutex_. True if a heartbeat from the session's Base has
    // arrived since it started; then the session becomes HeartbeatReceived.
    bool CheckHeartbeatLocked();
    // Caller holds mutex_. Moves to a final state unless already final.
    void FinishLocked(EnrollmentState state, const char *error = "");
    EnrollmentSession CopyLocked() const;
    bool OwnedByLocked(const std::string &device_id, const std::string &session_id) const;
    std::chrono::steady_clock::time_point NowSteady() const;
    int64_t NowUnixMs() const;

    ingest::Registry &registry_;
    const std::shared_ptr<Commissioner> commissioner_;
    const EnrollmentConfig config_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool has_session_ = false;
    Session session_;
    bool worker_running_ = false; // the session's worker has not finished its cleanup
    bool stopping_ = false;
    std::thread worker_;
};

} // namespace pogohome_api
