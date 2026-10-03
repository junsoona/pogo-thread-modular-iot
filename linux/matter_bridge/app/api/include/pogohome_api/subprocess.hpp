// Runs one program with a hard time limit and bounded output, never through
// a shell: pogo_matter_bridge uses it to call pogohome-otbr-helper (through
// sudo), and the helper uses it to call ot-ctl.
//
// - argv[0] is an absolute path, exec'd directly (execve, no PATH search,
//   no /bin/sh). `env` is the child's whole environment.
// - `stdin_data` is written to the child's stdin and then WIPED in the
//   caller's copy (it may carry a PSKd). With `close_stdin_when` set, stdin
//   stays open until that predicate accepts what stdout printed so far --
//   for ot-ctl, which may exit on stdin EOF before printing its answer.
// - stdout/stderr beyond their limits, or the deadline passing, kills the
//   child (SIGTERM, then SIGKILL) and says so; the child is always reaped.
// - Every other descriptor is closed in the child, and its signal mask and
//   SIGPIPE disposition are reset.
//
// The calling thread gets SIGPIPE blocked (and it stays blocked): a write
// to a child that already exited must fail with EPIPE, not kill the process.
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace pogohome_api {

struct SubprocessOptions {
    std::chrono::milliseconds timeout{5000};
    size_t max_stdout_bytes = 4096;
    size_t max_stderr_bytes = 1024;
    std::function<bool(const std::string &stdout_so_far)> close_stdin_when;
    // After close_stdin_when accepted the output, how long the child gets to
    // exit by itself before it is killed.
    std::chrono::milliseconds exit_grace{1000};
};

struct SubprocessResult {
    bool spawned = false;
    bool exited = false;   // exited normally; exit_code is valid
    int exit_code = -1;
    bool timed_out = false;
    bool output_overflow = false;
    bool stdin_accepted = false; // close_stdin_when returned true
    std::string out;
    std::string err;
};

SubprocessResult RunSubprocess(const std::vector<std::string> &argv, const std::vector<std::string> &env,
                               std::string &stdin_data, const SubprocessOptions &options);

} // namespace pogohome_api
