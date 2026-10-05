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
    if (code == "invalid_request") {
        // The helper refused the request line itself. The bridge only ever
        // sends lines BuildHelperRequest() made, so this is a helper that
        // does not know the operation.
        return CommissionerStatus::Unsupported;
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
    case CommissionerStatus::Unsupported:
        return "unsupported";
    }
    return "failed";
}

CommissionerStatus HelperCommissioner::Run(std::string &request, std::chrono::milliseconds timeout, HelperRun &out)
{
    out = HelperRun();
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
    out.ran = true;
    out.exited = run.exited;
    out.exit_code = run.exit_code;
    out.output_overflow = run.output_overflow;
    out.line = run.out;
    out.err_text = run.err;
    return CommissionerStatus::Ok;
}

CommissionerStatus HelperCommissioner::UnexpectedAnswer(const HelperRun &run)
{
    static std::mutex log_mutex;
    std::lock_guard<std::mutex> lock(log_mutex);
    std::fprintf(stderr, "pogohome_api: enrollment: unexpected OTBR helper answer (exit %d)%s%s\n",
                 run.exited ? run.exit_code : -1, run.err_text.empty() ? "" : ": ",
                 FirstLineForLog(run.err_text).c_str());
    // sudo refusing (no rule, NoNewPrivileges, a password prompt) ends
    // here too: nothing ran as root.
    return run.exited && run.exit_code == 1 && run.line.empty() ? CommissionerStatus::Unavailable
                                                                : CommissionerStatus::Failed;
}

CommissionerStatus HelperCommissioner::Call(std::string &request, std::chrono::milliseconds timeout,
                                            std::vector<std::string> &words)
{
    words.clear();
    HelperRun run;
    CommissionerStatus status = Run(request, timeout, run);
    if (status != CommissionerStatus::Ok) {
        return status;
    }
    bool ok = false;
    if (run.output_overflow || !ParseHelperResponse(run.line, ok, words)) {
        return UnexpectedAnswer(run);
    }
    if (!ok) {
        status = words.empty() ? CommissionerStatus::Failed : StatusFromErrorCode(words[0]);
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

CommissionerStatus HelperCommissioner::QueryMeshLocalEid(std::string &address)
{
    address.clear();
    HelperRequest r;
    r.op = HelperOp::MeshLocalEid;
    std::string request = BuildHelperRequest(r);
    HelperRun run;
    CommissionerStatus status = Run(request, config_.call_timeout, run);
    if (status != CommissionerStatus::Ok) {
        return status;
    }
    if (run.output_overflow) {
        return UnexpectedAnswer(run);
    }
    // The one answer that carries a value has its own strict parser; every
    // other answer still has to be a fixed-vocabulary line.
    if (ParseHelperMleidResponse(run.line, address)) {
        return CommissionerStatus::Ok;
    }
    bool ok = false;
    std::vector<std::string> words;
    if (!ParseHelperResponse(run.line, ok, words)) {
        return UnexpectedAnswer(run);
    }
    if (ok || words.empty()) {
        return CommissionerStatus::Failed; // "ok <something else>" is not an address
    }
    return StatusFromErrorCode(words[0]);
}

} // namespace pogohome_api
