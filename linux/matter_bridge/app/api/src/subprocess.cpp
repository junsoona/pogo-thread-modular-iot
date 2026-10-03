#include "pogohome_api/subprocess.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

#include "pogohome_api/otbr_helper_protocol.hpp"

namespace pogohome_api {

namespace {

using Clock = std::chrono::steady_clock;

bool MakePipe(int fds[2])
{
#if defined(__linux__)
    return pipe2(fds, O_CLOEXEC) == 0;
#else
    if (pipe(fds) != 0) {
        return false;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
#endif
}

void CloseFd(int &fd)
{
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

void SetNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

int MillisUntil(Clock::time_point deadline)
{
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (left <= 0) {
        return 0;
    }
    return left > 100 ? 100 : static_cast<int>(left);
}

// Reaps `pid` if it has exited; true once it has.
bool TryReap(pid_t pid, SubprocessResult &result)
{
    int status = 0;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
        if (WIFEXITED(status)) {
            result.exited = true;
            result.exit_code = WEXITSTATUS(status);
        }
        return true;
    }
    return r < 0 && errno == ECHILD;
}

void KillAndReap(pid_t pid, SubprocessResult &result)
{
    if (TryReap(pid, result)) {
        return;
    }
    kill(pid, SIGTERM);
    const auto soft = Clock::now() + std::chrono::milliseconds(300);
    while (Clock::now() < soft) {
        if (TryReap(pid, result)) {
            return;
        }
        usleep(10 * 1000);
    }
    kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

} // namespace

SubprocessResult RunSubprocess(const std::vector<std::string> &argv, const std::vector<std::string> &env,
                               std::string &stdin_data, const SubprocessOptions &options)
{
    SubprocessResult result;
    if (argv.empty() || argv[0].empty() || argv[0][0] != '/') {
        SecureWipe(stdin_data);
        return result;
    }

    sigset_t sigpipe;
    sigemptyset(&sigpipe);
    sigaddset(&sigpipe, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &sigpipe, nullptr);

    // Everything the child needs is prepared before fork(): after it, only
    // async-signal-safe calls run in the child.
    std::vector<char *> c_argv;
    for (const auto &a : argv) {
        c_argv.push_back(const_cast<char *>(a.c_str()));
    }
    c_argv.push_back(nullptr);
    std::vector<char *> c_env;
    for (const auto &e : env) {
        c_env.push_back(const_cast<char *>(e.c_str()));
    }
    c_env.push_back(nullptr);
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536) {
        max_fd = 65536;
    }

    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (!MakePipe(in_pipe) || !MakePipe(out_pipe) || !MakePipe(err_pipe)) {
        CloseFd(in_pipe[0]);
        CloseFd(in_pipe[1]);
        CloseFd(out_pipe[0]);
        CloseFd(out_pipe[1]);
        CloseFd(err_pipe[0]);
        CloseFd(err_pipe[1]);
        SecureWipe(stdin_data);
        return result;
    }

    pid_t pid = fork();
    if (pid == 0) {
        // Move the three child ends above 2 first, so dup2() below can never
        // clobber one of them.
        int in_fd = fcntl(in_pipe[0], F_DUPFD, 3);
        int out_fd = fcntl(out_pipe[1], F_DUPFD, 3);
        int err_fd = fcntl(err_pipe[1], F_DUPFD, 3);
        if (in_fd < 0 || out_fd < 0 || err_fd < 0 || dup2(in_fd, 0) < 0 || dup2(out_fd, 1) < 0 ||
            dup2(err_fd, 2) < 0) {
            _exit(127);
        }
#if defined(__linux__) && defined(SYS_close_range)
        if (syscall(SYS_close_range, 3U, ~0U, 0U) != 0)
#endif
        {
            for (long fd = 3; fd < max_fd; fd++) {
                close(static_cast<int>(fd));
            }
        }
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        signal(SIGPIPE, SIG_DFL);
        execve(c_argv[0], c_argv.data(), c_env.data());
        _exit(127);
    }

    CloseFd(in_pipe[0]);
    CloseFd(out_pipe[1]);
    CloseFd(err_pipe[1]);
    int in_w = in_pipe[1];
    int out_r = out_pipe[0];
    int err_r = err_pipe[0];
    if (pid < 0) {
        CloseFd(in_w);
        CloseFd(out_r);
        CloseFd(err_r);
        SecureWipe(stdin_data);
        return result;
    }
    result.spawned = true;
    SetNonBlocking(in_w);
    SetNonBlocking(out_r);
    SetNonBlocking(err_r);

    const auto deadline = Clock::now() + options.timeout;
    auto exit_deadline = deadline;
    size_t written = 0;
    bool reaped = false;
    bool kill_now = false;

    if (stdin_data.empty() && !options.close_stdin_when) {
        CloseFd(in_w);
    }

    while (!kill_now) {
        if (out_r < 0 && err_r < 0) {
            if (TryReap(pid, result)) {
                reaped = true;
                break;
            }
        }
        const auto now = Clock::now();
        if (now >= exit_deadline) {
            result.timed_out = !result.stdin_accepted;
            kill_now = true;
            break;
        }

        pollfd fds[3];
        nfds_t n = 0;
        int in_idx = -1, out_idx = -1, err_idx = -1;
        if (in_w >= 0 && written < stdin_data.size()) {
            fds[n] = {in_w, POLLOUT, 0};
            in_idx = static_cast<int>(n++);
        }
        if (out_r >= 0) {
            fds[n] = {out_r, POLLIN, 0};
            out_idx = static_cast<int>(n++);
        }
        if (err_r >= 0) {
            fds[n] = {err_r, POLLIN, 0};
            err_idx = static_cast<int>(n++);
        }
        int rc = n == 0 ? (usleep(10 * 1000), 0) : poll(fds, n, MillisUntil(exit_deadline));
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            kill_now = true;
            break;
        }

        if (in_idx >= 0 && (fds[in_idx].revents & (POLLOUT | POLLERR | POLLHUP))) {
            ssize_t w = write(in_w, stdin_data.data() + written, stdin_data.size() - written);
            if (w > 0) {
                written += static_cast<size_t>(w);
            } else if (w < 0 && errno != EAGAIN && errno != EINTR) {
                CloseFd(in_w); // EPIPE: the child stopped reading
            }
            if (written == stdin_data.size()) {
                SecureWipe(stdin_data);
                written = 0;
                if (!options.close_stdin_when) {
                    CloseFd(in_w);
                }
            }
        }

        auto drain = [&](int &fd, std::string &buf, size_t limit) {
            char chunk[1024];
            ssize_t r = read(fd, chunk, sizeof(chunk));
            if (r > 0) {
                if (buf.size() + static_cast<size_t>(r) > limit) {
                    result.output_overflow = true;
                    kill_now = true;
                    return;
                }
                buf.append(chunk, static_cast<size_t>(r));
                return;
            }
            if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
                CloseFd(fd);
            }
        };
        if (out_idx >= 0 && (fds[out_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            drain(out_r, result.out, options.max_stdout_bytes);
            if (!kill_now && !result.stdin_accepted && options.close_stdin_when &&
                options.close_stdin_when(result.out)) {
                result.stdin_accepted = true;
                CloseFd(in_w);
                auto grace = Clock::now() + options.exit_grace;
                if (grace < exit_deadline) {
                    exit_deadline = grace;
                }
            }
        }
        if (!kill_now && err_idx >= 0 && (fds[err_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            drain(err_r, result.err, options.max_stderr_bytes);
        }
    }

    SecureWipe(stdin_data);
    CloseFd(in_w);
    CloseFd(out_r);
    CloseFd(err_r);
    if (!reaped) {
        KillAndReap(pid, result);
    }
    return result;
}

} // namespace pogohome_api
