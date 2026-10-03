// The OTBR Commissioner operations Power Base enrollment needs, behind an
// interface so the session logic (enrollment.hpp) is tested with a fake.
//
// HelperCommissioner is the real one: every call runs pogohome-otbr-helper
// once (normally as `/usr/bin/sudo -n -- <helper>`) with the request on its
// stdin, under a hard timeout and an output limit, and maps its one-line
// answer. Calls block the calling thread for up to their timeout; callers
// must not hold any lock the API or the Registry needs while calling.
#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace pogohome_api {

enum class CommissionerStatus {
    Ok,
    NotFound,     // RemoveJoiner only: no such entry (already expired or auto-removed)
    Rejected,     // the OTBR refused the command
    InvalidState, // e.g. the Commissioner is not active
    Unavailable,  // no OTBR to talk to, sudo refused, the helper is missing
    Timeout,      // the helper (or ot-ctl under it) ran out of time
    Failed,       // anything else: a malformed answer, an unexpected exit
};

const char *CommissionerStatusName(CommissionerStatus status);

class Commissioner {
public:
    virtual ~Commissioner() = default;

    // "disabled", "petitioning" or "active".
    virtual CommissionerStatus QueryState(std::string &state) = 0;
    // Ok once the Commissioner is active. Never stops or restarts one that
    // is already running; `started` says whether this call started it.
    virtual CommissionerStatus EnsureActive(bool &started) = 0;
    // Allows exactly `eui64` (16 lowercase hex) for `timeout_s` seconds.
    // `pskd` is wiped before this returns, whatever the outcome.
    virtual CommissionerStatus AddJoiner(const std::string &eui64, std::string &pskd, int timeout_s) = 0;
    // Removes exactly `eui64`'s entry. NotFound means it was already gone.
    virtual CommissionerStatus RemoveJoiner(const std::string &eui64) = 0;
};

struct HelperCommissionerConfig {
    // Run as-is, then the request on stdin. Production:
    // {"/usr/bin/sudo", "-n", "--", "<absolute helper path>"}.
    std::vector<std::string> command;
    std::chrono::milliseconds start_timeout{20000}; // includes the petition wait
    std::chrono::milliseconds call_timeout{8000};
};

class HelperCommissioner : public Commissioner {
public:
    explicit HelperCommissioner(HelperCommissionerConfig config) : config_(std::move(config)) {}

    CommissionerStatus QueryState(std::string &state) override;
    CommissionerStatus EnsureActive(bool &started) override;
    CommissionerStatus AddJoiner(const std::string &eui64, std::string &pskd, int timeout_s) override;
    CommissionerStatus RemoveJoiner(const std::string &eui64) override;

private:
    // Runs the helper with `request` (wiped). On an "ok" answer fills
    // `words` (after "ok") and returns Ok.
    CommissionerStatus Call(std::string &request, std::chrono::milliseconds timeout, std::vector<std::string> &words);

    const HelperCommissionerConfig config_;
};

} // namespace pogohome_api
