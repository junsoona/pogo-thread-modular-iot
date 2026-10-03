#include "pogohome_api/commissioner.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>

#include "pogohome_api/otbr_helper_protocol.hpp"
#include "pogohome_api/subprocess.hpp"

namespace pogohome_api {

namespace {

// sudo's and the helper's own messages never contain the PSKd (it only ever
// travels on stdin), but only a short, printable first line is logged.
std::string FirstLineForLog(const std::string &text)
{
    std::string out;
    for (char c : text) {
        if (c == '\n' || out.size() >= 160) {
            break;
        }
        out += (c >= 0x20 && c <= 0x7e) ? c : '?';
    }
    return out;
}

CommissionerStatus StatusFromErrorCode(const std::string &code)
{
    if (code == "rejected") {
        return CommissionerStatus::Rejected;
    }
    if (code == "invalid_state") {
        return CommissionerStatus::InvalidState;
    }
    if (code == "otbr_unavailable") {
        return CommissionerStatus::Unavailable;
    }
    if (code == "timeout") {
        return CommissionerStatus::Timeout;
    }
    return CommissionerStatus::Failed;
}

} // namespace

const char *CommissionerStatusName(CommissionerStatus status)
{
    switch (status) {
    case CommissionerStatus::Ok:
        return "ok";
    case CommissionerStatus::NotFound:
        return "not_found";
    case CommissionerStatus::Rejected:
        return "rejected";
    case CommissionerStatus::InvalidState:
        return "invalid_state";
    case CommissionerStatus::Unavailable:
        return "unavailable";
    case CommissionerStatus::Timeout:
        return "timeout";
    case CommissionerStatus::Failed:
        return "failed";
    }
    return "failed";
}

CommissionerStatus HelperCommissioner::Call(std::string &request, std::chrono::milliseconds timeout,
                                            std::vector<std::string> &words)
{
    static std::mutex log_mutex;
    words.clear();
    if (request.empty() || config_.command.empty()) {
        SecureWipe(request);
        return CommissionerStatus::Failed;
    }
    const std::vector<std::string> env = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C"};
    SubprocessOptions options;
    options.timeout = timeout;
    options.max_stdout_bytes = kMaxHelperResponseBytes + 1;
    options.max_stderr_bytes = 1024;
    SubprocessResult run = RunSubprocess(config_.command, env, request, options);
    SecureWipe(request);

    if (!run.spawned) {
        std::fprintf(stderr, "pogohome_api: enrollment: could not start the OTBR helper\n");
        return CommissionerStatus::Unavailable;
    }
    if (run.timed_out) {
        std::fprintf(stderr, "pogohome_api: enrollment: the OTBR helper did not answer within %lld ms\n",
                     static_cast<long long>(timeout.count()));
        return CommissionerStatus::Timeout;
    }
    bool ok = false;
    if (run.output_overflow || !ParseHelperResponse(run.out, ok, words)) {
        std::lock_guard<std::mutex> lock(log_mutex);
        std::fprintf(stderr, "pogohome_api: enrollment: unexpected OTBR helper answer (exit %d)%s%s\n",
                     run.exited ? run.exit_code : -1, run.err.empty() ? "" : ": ",
                     FirstLineForLog(run.err).c_str());
        // sudo refusing (no rule, NoNewPrivileges, a password prompt) ends
        // here too: nothing ran as root.
        return run.exited && run.exit_code == 1 && run.out.empty() ? CommissionerStatus::Unavailable
                                                                   : CommissionerStatus::Failed;
    }
    if (!ok) {
        CommissionerStatus status = words.empty() ? CommissionerStatus::Failed : StatusFromErrorCode(words[0]);
        words.clear();
        return status;
    }
    return CommissionerStatus::Ok;
}

CommissionerStatus HelperCommissioner::QueryState(std::string &state)
{
    HelperRequest r;
    r.op = HelperOp::CommissionerState;
    std::string request = BuildHelperRequest(r);
    std::vector<std::string> words;
    CommissionerStatus status = Call(request, config_.call_timeout, words);
    if (status != CommissionerStatus::Ok) {
        return status;
    }
    if (words.size() != 1 || (words[0] != "disabled" && words[0] != "petitioning" && words[0] != "active")) {
        return CommissionerStatus::Failed;
    }
    state = words[0];
    return CommissionerStatus::Ok;
}

CommissionerStatus HelperCommissioner::EnsureActive(bool &started)
{
    started = false;
    HelperRequest r;
    r.op = HelperOp::CommissionerStart;
    std::string request = BuildHelperRequest(r);
    std::vector<std::string> words;
    CommissionerStatus status = Call(request, config_.start_timeout, words);
    if (status != CommissionerStatus::Ok) {
        return status;
    }
    if (words.size() != 2 || words[0] != "active" || (words[1] != "started" && words[1] != "already")) {
        return CommissionerStatus::Failed;
    }
    started = words[1] == "started";
    return CommissionerStatus::Ok;
}

CommissionerStatus HelperCommissioner::AddJoiner(const std::string &eui64, std::string &pskd, int timeout_s)
{
    HelperRequest r;
    r.op = HelperOp::JoinerAdd;
    r.eui64 = eui64;
    r.timeout_s = timeout_s;
    r.pskd = pskd;
    SecureWipe(pskd);
    std::string request = BuildHelperRequest(r);
    SecureWipe(r.pskd);
    std::vector<std::string> words;
    CommissionerStatus status = Call(request, config_.call_timeout, words);
    if (status != CommissionerStatus::Ok) {
        return status;
    }
    return words.size() == 1 && words[0] == "added" ? CommissionerStatus::Ok : CommissionerStatus::Failed;
}

CommissionerStatus HelperCommissioner::RemoveJoiner(const std::string &eui64)
{
    HelperRequest r;
    r.op = HelperOp::JoinerRemove;
    r.eui64 = eui64;
    std::string request = BuildHelperRequest(r);
    std::vector<std::string> words;
    CommissionerStatus status = Call(request, config_.call_timeout, words);
    if (status != CommissionerStatus::Ok) {
        return status;
    }
    if (words.size() == 1 && words[0] == "removed") {
        return CommissionerStatus::Ok;
    }
    if (words.size() == 1 && words[0] == "not_found") {
        return CommissionerStatus::NotFound;
    }
    return CommissionerStatus::Failed;
}

} // namespace pogohome_api
