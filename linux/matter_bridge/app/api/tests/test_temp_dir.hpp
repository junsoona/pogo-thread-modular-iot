// A scratch directory per scenario for the settings/credential host tests.
#pragma once

#include <cstdlib>
#include <string>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace test_support {

inline std::string MakeTempDir()
{
    const char *base = std::getenv("TMPDIR");
    std::string pattern = std::string(base != nullptr && *base != '\0' ? base : "/tmp");
    if (pattern.back() != '/') {
        pattern += '/';
    }
    pattern += "pogohome_test.XXXXXX";
    std::string buf = pattern;
    if (mkdtemp(&buf[0]) == nullptr) {
        return std::string();
    }
    return buf;
}

inline void RemoveTree(const std::string &path)
{
    DIR *dir = opendir(path.c_str());
    if (dir != nullptr) {
        while (dirent *entry = readdir(dir)) {
            std::string name = entry->d_name;
            if (name == "." || name == "..") {
                continue;
            }
            std::string child = path + "/" + name;
            struct stat st{};
            if (lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                chmod(child.c_str(), 0700);
                RemoveTree(child);
            } else {
                unlink(child.c_str());
            }
        }
        closedir(dir);
    }
    rmdir(path.c_str());
}

inline bool ReadFile(const std::string &path, std::string &out)
{
    out.clear();
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return true;
}

inline bool WriteFile(const std::string &path, const std::string &contents)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    bool ok = std::fwrite(contents.data(), 1, contents.size(), f) == contents.size();
    std::fclose(f);
    return ok;
}

inline int FileMode(const std::string &path)
{
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) {
        return -1;
    }
    return static_cast<int>(st.st_mode & 0777);
}

} // namespace test_support
