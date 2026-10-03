// pogohome-otbr-helper: the only part of Power Base enrollment that runs as
// root. pogo_matter_bridge runs as a normal user and cannot open the OTBR
// control socket, so it runs this program through ONE exact sudoers rule
// (no arguments allowed; see docs/pogohome-api-v1.md "Power Base
// enrollment" > "Installing the OTBR helper") and writes one request line
// to its stdin. The protocol and the input checks are in
// include/pogohome_api/otbr_helper_protocol.hpp.
//
// What it can do, and nothing more:
//   - read the Commissioner state;
//   - start the Commissioner if it is not already active (it never stops
//     one: a Commissioner someone else started keeps running);
//   - allow one exact EUI-64 as a Joiner with a PSKd, for 10..300 seconds
//     (the OTBR removes the entry by itself when that time runs out, even if
//     the bridge never comes back to remove it);
//   - remove that one EUI-64's entry ("NotFound" -- already expired or
//     auto-removed after a successful join -- counts as removed).
//
// How it talks to the OTBR: it runs ot-ctl (a fixed, root-owned path; no
// PATH lookup, no shell) with NO command arguments and writes the single
// CLI command to ot-ctl's stdin, so the PSKd never appears in any process's
// argument list. It keeps that stdin open until ot-ctl has printed "Done" or
// "Error ...", then closes it. The helper never prints, logs or stores the
// PSKd or ot-ctl's raw output; its stdout is one fixed-vocabulary line.
//
// Build-time settings (defaults match the operating host):
//   POGOHOME_OT_CTL_PATH       "/usr/sbin/ot-ctl"
//   POGOHOME_OTBR_INTERFACE    "wpan0" (passed as -I only when different)
//   POGOHOME_OTBR_CLI_TIMEOUT_MS  5000 (one ot-ctl command)
//   POGOHOME_OTBR_HELPER_TEST_BUILD  host tests only: skips the check that
//                              ot-ctl is root-owned and not writable by others
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include "pogohome_api/otbr_helper_protocol.hpp"
#include "pogohome_api/subprocess.hpp"

#ifndef POGOHOME_OT_CTL_PATH
#define POGOHOME_OT_CTL_PATH "/usr/sbin/ot-ctl"
#endif
#ifndef POGOHOME_OTBR_INTERFACE
#define POGOHOME_OTBR_INTERFACE "wpan0"
#endif
#ifndef POGOHOME_OTBR_CLI_TIMEOUT_MS
#define POGOHOME_OTBR_CLI_TIMEOUT_MS 5000
#endif

using namespace pogohome_api;

namespace {

constexpr int kBackstopSeconds = 30;
constexpr auto kStdinTimeout = std::chrono::milliseconds(2000);
constexpr auto kCliTimeout = std::chrono::milliseconds(POGOHOME_OTBR_CLI_TIMEOUT_MS);
constexpr auto kPetitionWait = std::chrono::milliseconds(10000);
constexpr auto kPetitionPoll = std::chrono::milliseconds(300);

enum class CliStatus { Done, Error, Unavailable, Timeout };

struct CliResult {
    CliStatus status = CliStatus::Unavailable;
    OtCliOutput output;
};

int Respond(const char *line, int exit_code)
{
    std::printf("%s\n", line);
    std::fflush(stdout);
    return exit_code;
}

bool OtCtlLooksSafe()
{
#ifdef POGOHOME_OTBR_HELPER_TEST_BUILD
    return access(POGOHOME_OT_CTL_PATH, X_OK) == 0;
#else
    struct stat st{};
    if (stat(POGOHOME_OT_CTL_PATH, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    // Run as root, so it must not be replaceable by anyone but root.
    return st.st_uid == 0 && (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
#endif
}

// One CLI command through ot-ctl's stdin. `command` is wiped.
CliResult RunCli(std::string command)
{
    CliResult result;
    std::vector<std::string> argv = {POGOHOME_OT_CTL_PATH};
    if (std::strcmp(POGOHOME_OTBR_INTERFACE, "wpan0") != 0) {
        argv.push_back("-I");
        argv.push_back(POGOHOME_OTBR_INTERFACE);
    }
    const std::vector<std::string> env = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C"};
    command += '\n';
    SubprocessOptions options;
    options.timeout = kCliTimeout;
    options.max_stdout_bytes = 4096;
    options.max_stderr_bytes = 1024;
    options.close_stdin_when = [](const std::string &out) { return OtCliOutputComplete(out); };
    SubprocessResult run = RunSubprocess(argv, env, command, options);
    SecureWipe(command);

    result.output = ParseOtCliOutput(run.out);
    // ot-ctl may echo the command (which can hold the PSKd): wipe everything.
    SecureWipe(run.out);
    SecureWipe(run.err);
    if (!run.spawned) {
        result.status = CliStatus::Unavailable;
    } else if (result.output.kind == OtCliOutput::Kind::Done) {
        result.status = CliStatus::Done;
    } else if (result.output.kind == OtCliOutput::Kind::Error) {
        result.status = CliStatus::Error;
    } else if (run.timed_out) {
        result.status = CliStatus::Timeout;
    } else {
        // e.g. "connect session failed" on stderr: no OTBR to talk to.
        result.status = CliStatus::Unavailable;
    }
    for (auto &line : result.output.lines) {
        // Only the Commissioner state word is ever looked at.
        if (line != "disabled" && line != "petition" && line != "petitioning" && line != "active") {
            SecureWipe(line);
        }
    }
    return result;
}

const char *CliFailure(const CliResult &r)
{
    switch (r.status) {
    case CliStatus::Timeout:
        return "err timeout";
    case CliStatus::Unavailable:
        return "err otbr_unavailable";
    case CliStatus::Error:
        return r.output.error_name == "InvalidState" ? "err invalid_state" : "err rejected";
    case CliStatus::Done:
        break;
    }
    return "err internal";
}

// "disabled", "petitioning", "active", or "" (with `failure` set).
std::string QueryState(const char *&failure)
{
    CliResult r = RunCli("commissioner state");
    if (r.status != CliStatus::Done) {
        failure = CliFailure(r);
        return std::string();
    }
    for (const auto &line : r.output.lines) {
        if (line == "disabled" || line == "active") {
            return line;
        }
        if (line == "petition" || line == "petitioning") {
            return "petitioning";
        }
    }
    failure = "err internal";
    return std::string();
}

int DoState()
{
    const char *failure = nullptr;
    std::string state = QueryState(failure);
    if (state.empty()) {
        return Respond(failure, 1);
    }
    return Respond(("ok " + state).c_str(), 0);
}

int DoStart()
{
    const char *failure = nullptr;
    std::string state = QueryState(failure);
    if (state.empty()) {
        return Respond(failure, 1);
    }
    if (state == "active") {
        return Respond("ok active already", 0);
    }
    bool started_here = false;
    if (state == "disabled") {
        CliResult r = RunCli("commissioner start");
        if (r.status != CliStatus::Done) {
            return Respond(CliFailure(r), 1);
        }
        started_here = true;
    }
    // Petitioning (ours or someone else's): wait, bounded, for the Leader's
    // answer.
    const auto deadline = std::chrono::steady_clock::now() + kPetitionWait;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(kPetitionPoll);
        state = QueryState(failure);
        if (state.empty()) {
            return Respond(failure, 1);
        }
        if (state == "active") {
            return Respond(started_here ? "ok active started" : "ok active already", 0);
        }
        if (state == "disabled") {
            return Respond("err rejected", 1); // the petition was refused
        }
    }
    return Respond("err timeout", 1);
}

int DoJoinerAdd(HelperRequest &request)
{
    std::string command = "commissioner joiner add " + request.eui64 + " " + request.pskd + " " +
                          std::to_string(request.timeout_s);
    SecureWipe(request.pskd);
    CliResult r = RunCli(std::move(command));
    if (r.status != CliStatus::Done) {
        return Respond(CliFailure(r), 1);
    }
    return Respond("ok added", 0);
}

int DoJoinerRemove(const HelperRequest &request)
{
    CliResult r = RunCli("commissioner joiner remove " + request.eui64);
    if (r.status == CliStatus::Done) {
        return Respond("ok removed", 0);
    }
    if (r.status == CliStatus::Error && r.output.error_name == "NotFound") {
        // Already gone: expired, or removed by OpenThread after the join.
        return Respond("ok not_found", 0);
    }
    return Respond(CliFailure(r), 1);
}

// Reads the whole request (up to EOF), at most kMaxHelperRequestBytes + 1.
bool ReadRequest(std::string &raw)
{
    const auto deadline = std::chrono::steady_clock::now() + kStdinTimeout;
    char buf[64];
    while (raw.size() <= kMaxHelperRequestBytes) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            return false;
        }
        pollfd pfd{STDIN_FILENO, POLLIN, 0};
        int rc = poll(&pfd, 1, static_cast<int>(left.count()));
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc <= 0) {
            return false;
        }
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n == 0) {
            return true;
        }
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return false;
        }
        raw.append(buf, static_cast<size_t>(n));
        std::memset(buf, 0, sizeof(buf));
    }
    return false; // too long
}

const char *OpName(HelperOp op)
{
    switch (op) {
    case HelperOp::CommissionerState:
        return "commissioner-state";
    case HelperOp::CommissionerStart:
        return "commissioner-start";
    case HelperOp::JoinerAdd:
        return "joiner-add";
    case HelperOp::JoinerRemove:
        return "joiner-remove";
    }
    return "?";
}

} // namespace

int main(int argc, char **)
{
    // Whatever happens below, this process ends within kBackstopSeconds.
    alarm(kBackstopSeconds);
    signal(SIGPIPE, SIG_IGN);
    umask(077);
    // Make sure descriptors 0-2 exist so nothing we open lands on them.
    for (int fd = 0; fd <= 2; fd++) {
        if (fcntl(fd, F_GETFD) < 0) {
            int null_fd = open("/dev/null", O_RDWR);
            if (null_fd < 0 || null_fd > 2) {
                return 1;
            }
        }
    }
    openlog("pogohome-otbr-helper", LOG_PID, LOG_DAEMON);

    if (argc != 1) {
        return Respond("err invalid_request", 2);
    }
    std::string raw;
    HelperRequest request;
    const bool read_ok = ReadRequest(raw);
    const bool parsed = read_ok && ParseHelperRequest(raw, request);
    SecureWipe(raw);
    if (!parsed) {
        syslog(LOG_WARNING, "rejected a malformed request");
        return Respond("err invalid_request", 2);
    }
    if (!OtCtlLooksSafe()) {
        SecureWipe(request.pskd);
        syslog(LOG_ERR, "%s is missing, or is not a root-owned file only root can write -- refusing",
               POGOHOME_OT_CTL_PATH);
        return Respond("err otbr_unavailable", 1);
    }

    // The audit line names the operation and the EUI-64, never the PSKd.
    if (request.eui64.empty()) {
        syslog(LOG_NOTICE, "%s", OpName(request.op));
    } else if (request.op == HelperOp::JoinerAdd) {
        syslog(LOG_NOTICE, "%s eui64=%s timeout=%ds", OpName(request.op), request.eui64.c_str(), request.timeout_s);
    } else {
        syslog(LOG_NOTICE, "%s eui64=%s", OpName(request.op), request.eui64.c_str());
    }

    switch (request.op) {
    case HelperOp::CommissionerState:
        return DoState();
    case HelperOp::CommissionerStart:
        return DoStart();
    case HelperOp::JoinerAdd:
        return DoJoinerAdd(request);
    case HelperOp::JoinerRemove:
        return DoJoinerRemove(request);
    }
    return Respond("err internal", 1);
}
