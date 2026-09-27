#include "pogohome_api/hub_identity.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pogohome_api {

namespace {

std::string HubIdPath(const std::string &state_dir)
{
    return state_dir + "/" + kHubIdFileName;
}

// 0 = read, 1 = no such file, -1 = error (errno-based message in `error`).
int ReadSmallFile(const std::string &path, std::string &out, std::string &error)
{
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            return 1;
        }
        error = "cannot open " + path + ": " + std::strerror(errno);
        return -1;
    }
    out.clear();
    char buf[128];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = "cannot read " + path + ": " + std::strerror(errno);
            close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        out.append(buf, static_cast<size_t>(n));
        if (out.size() > 256) {
            break; // far longer than any valid id -- reported as malformed below
        }
    }
    close(fd);
    return 0;
}

HubIdLoadResult ParseExisting(const std::string &path, const std::string &contents, std::string &hub_id,
                              std::string &error)
{
    std::string trimmed = contents;
    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r' || trimmed.back() == ' ')) {
        trimmed.pop_back();
    }
    if (!IsValidHubId(trimmed)) {
        error = path + " exists but does not hold a valid hub_id (" + std::to_string(kHubIdHexLength) +
                " lowercase hex digits) -- refusing to replace it with a new id, since clients that remember this "
                "hub would stop recognizing it. Inspect or move this file aside by hand, then restart.";
        return HubIdLoadResult::Error;
    }
    hub_id = trimmed;
    return HubIdLoadResult::Loaded;
}

bool RandomBytes(unsigned char *out, size_t len, std::string &error)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = std::string("cannot open /dev/urandom: ") + std::strerror(errno);
        return false;
    }
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, out + got, len - got);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            error = "short read from /dev/urandom";
            close(fd);
            return false;
        }
        got += static_cast<size_t>(n);
    }
    close(fd);
    return true;
}

bool WriteAllAndSync(int fd, const std::string &data)
{
    size_t written = 0;
    while (written < data.size()) {
        ssize_t n = write(fd, data.data() + written, data.size() - written);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return fsync(fd) == 0;
}

void SyncDirectory(const std::string &dir)
{
    int fd = open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

} // namespace

bool IsValidHubId(const std::string &hub_id)
{
    if (hub_id.size() != kHubIdHexLength) {
        return false;
    }
    for (char c : hub_id) {
        bool digit = c >= '0' && c <= '9';
        bool lower_hex = c >= 'a' && c <= 'f';
        if (!digit && !lower_hex) {
            return false;
        }
    }
    return true;
}

HubIdLoadResult LoadOrCreateHubId(const std::string &state_dir, std::string &hub_id, std::string &error)
{
    hub_id.clear();
    const std::string path = HubIdPath(state_dir);

    std::string contents;
    int rc = ReadSmallFile(path, contents, error);
    if (rc < 0) {
        return HubIdLoadResult::Error;
    }
    if (rc == 0) {
        return ParseExisting(path, contents, hub_id, error);
    }

    unsigned char bytes[kHubIdHexLength / 2];
    if (!RandomBytes(bytes, sizeof(bytes), error)) {
        return HubIdLoadResult::Error;
    }
    static const char kHexDigits[] = "0123456789abcdef";
    std::string fresh;
    for (unsigned char b : bytes) {
        fresh += kHexDigits[b >> 4];
        fresh += kHexDigits[b & 0x0F];
    }

    const std::string tmp_path = path + ".tmp." + std::to_string(static_cast<long>(getpid()));
    unlink(tmp_path.c_str()); // a leftover from a crashed start of this same pid number
    int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = "cannot create " + tmp_path + ": " + std::strerror(errno);
        return HubIdLoadResult::Error;
    }
    if (!WriteAllAndSync(fd, fresh + "\n")) {
        error = "cannot write " + tmp_path + ": " + std::strerror(errno);
        close(fd);
        unlink(tmp_path.c_str());
        return HubIdLoadResult::Error;
    }
    close(fd);

    // link(2), not rename(2): never replaces a hub_id another start wrote in
    // the meantime.
    int link_rc = link(tmp_path.c_str(), path.c_str());
    int link_errno = errno;
    unlink(tmp_path.c_str());
    if (link_rc != 0) {
        if (link_errno == EEXIST) {
            if (ReadSmallFile(path, contents, error) != 0) {
                if (error.empty()) {
                    error = path + " disappeared while it was being created";
                }
                return HubIdLoadResult::Error;
            }
            return ParseExisting(path, contents, hub_id, error);
        }
        error = "cannot create " + path + ": " + std::strerror(link_errno);
        return HubIdLoadResult::Error;
    }
    SyncDirectory(state_dir);

    hub_id = fresh;
    return HubIdLoadResult::Created;
}

} // namespace pogohome_api
