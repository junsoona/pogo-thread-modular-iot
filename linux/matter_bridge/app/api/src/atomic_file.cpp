#include "pogohome_api/atomic_file.hpp"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pogohome_api {

namespace {

std::string DirectoryOf(const std::string &path)
{
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return ".";
    }
    if (slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

std::string Describe(const char *what, const std::string &path, int err)
{
    return std::string(what) + " " + path + ": " + std::strerror(err);
}

} // namespace

bool WriteFileAtomically(const std::string &path, const std::string &contents, std::string &error)
{
    const std::string tmp_path = path + ".tmp";
    unlink(tmp_path.c_str()); // a leftover from an interrupted write
    int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = Describe("cannot create", tmp_path, errno);
        return false;
    }
    size_t written = 0;
    while (written < contents.size()) {
        ssize_t n = write(fd, contents.data() + written, contents.size() - written);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            int err = n < 0 ? errno : EIO;
            error = Describe("cannot write", tmp_path, err);
            close(fd);
            unlink(tmp_path.c_str());
            return false;
        }
        written += static_cast<size_t>(n);
    }
    if (fsync(fd) != 0) {
        int err = errno;
        error = Describe("cannot fsync", tmp_path, err);
        close(fd);
        unlink(tmp_path.c_str());
        return false;
    }
    if (close(fd) != 0) {
        int err = errno;
        error = Describe("cannot close", tmp_path, err);
        unlink(tmp_path.c_str());
        return false;
    }
    if (rename(tmp_path.c_str(), path.c_str()) != 0) {
        int err = errno;
        error = Describe("cannot replace", path, err);
        unlink(tmp_path.c_str());
        return false;
    }
    const std::string dir = DirectoryOf(path);
    int dir_fd = open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (dir_fd >= 0) {
        // The new contents are already in place; a failed directory fsync
        // only weakens durability across a power cut, so it is not reported
        // as a failed write (the caller would otherwise report "not saved"
        // for data that is on disk).
        fsync(dir_fd);
        close(dir_fd);
    }
    return true;
}

ReadFileResult ReadWholeFile(const std::string &path, size_t max_bytes, std::string &out, std::string &error)
{
    out.clear();
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            return ReadFileResult::Missing;
        }
        error = Describe("cannot open", path, errno);
        return ReadFileResult::Error;
    }
    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = Describe("cannot read", path, errno);
            close(fd);
            return ReadFileResult::Error;
        }
        if (n == 0) {
            break;
        }
        out.append(buf, static_cast<size_t>(n));
        if (out.size() > max_bytes) {
            error = path + " is larger than " + std::to_string(max_bytes) + " bytes";
            close(fd);
            out.clear();
            return ReadFileResult::Error;
        }
    }
    close(fd);
    return ReadFileResult::Ok;
}

bool EnsurePrivateDirectory(const std::string &dir, std::string &error)
{
    if (mkdir(dir.c_str(), 0700) == 0) {
        return true;
    }
    if (errno != EEXIST) {
        error = Describe("cannot create directory", dir, errno);
        return false;
    }
    struct stat st{};
    if (stat(dir.c_str(), &st) != 0) {
        error = Describe("cannot stat", dir, errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        error = dir + " exists but is not a directory";
        return false;
    }
    if (st.st_uid == geteuid() && (st.st_mode & 0077) != 0) {
        chmod(dir.c_str(), 0700);
    }
    return true;
}

} // namespace pogohome_api
