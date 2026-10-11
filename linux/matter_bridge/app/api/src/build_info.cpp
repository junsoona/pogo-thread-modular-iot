#include "pogohome_api/build_info.hpp"

#include <cstring>

#if defined(__has_include)
#if __has_include("pogo_build_info_gen.h")
#include "pogo_build_info_gen.h"
#define POGOHOME_HAS_BUILD_STAMP 1
#endif
#endif

#ifndef POGOHOME_HAS_BUILD_STAMP
#define POGOHOME_HAS_BUILD_STAMP 0
#define POGO_BUILD_COMPONENT "linux_bridge"
#define POGO_BUILD_VERSION "unknown"
#define POGO_BUILD_ID "unknown"
#define POGO_BUILD_KIND "unknown"
#define POGO_BUILD_VARIANT "unknown"
#define POGO_BUILD_COMMIT "unknown"
#define POGO_BUILD_DIRTY (-1)
#define POGO_BUILD_SOURCE_FINGERPRINT "unknown"
#define POGO_BUILD_INFO_STRING "POGO_BUILD_INFO{component=linux_bridge;version=unknown;build=unknown;kind=unknown}"
#endif

namespace pogohome_api {

namespace {

// Referenced by BuildInfoString(), which every binary that prints a version
// calls: the string stays in the executable.
const char kBuildInfoString[] = POGO_BUILD_INFO_STRING;

// The values are generated from fixed alphabets (hex, digits, dots, a few
// words); anything else would be a broken stamp and is not put into JSON.
bool JsonSafe(const std::string &value)
{
    if (value.empty() || value.size() > 80) {
        return false;
    }
    for (char c : value) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '+' || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

} // namespace

const BuildInfo &CurrentBuildInfo()
{
    static const BuildInfo info = {
        POGO_BUILD_COMPONENT,
        POGO_BUILD_VERSION,
        POGO_BUILD_ID,
        POGO_BUILD_KIND,
        POGO_BUILD_VARIANT,
        POGO_BUILD_COMMIT,
        (POGO_BUILD_DIRTY) == 1 ? "true" : ((POGO_BUILD_DIRTY) == 0 ? "false" : "unknown"),
        POGO_BUILD_SOURCE_FINGERPRINT,
        POGOHOME_HAS_BUILD_STAMP != 0,
    };
    return info;
}

std::string VersionLine(const std::string &program)
{
    // Every binary that can print its version thereby also keeps the
    // POGO_BUILD_INFO string through the linker's section garbage collection.
    const char *volatile keep = kBuildInfoString;
    (void)keep;
    const BuildInfo &info = CurrentBuildInfo();
    return program + " " + info.version + " (" + info.component + ", build " + info.build_id + ", " + info.kind + ")";
}

std::string BuildInfoJson()
{
    const BuildInfo &info = CurrentBuildInfo();
    if (!info.known || !JsonSafe(info.component) || !JsonSafe(info.version) || !JsonSafe(info.build_id) ||
        !JsonSafe(info.kind)) {
        return std::string();
    }
    return "{\"component\":\"" + info.component + "\",\"version\":\"" + info.version + "\",\"build_id\":\"" +
           info.build_id + "\",\"build_kind\":\"" + info.kind + "\"}";
}

const char *BuildInfoString()
{
    return kBuildInfoString;
}

bool WantsVersion(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (argv[i] != nullptr && std::strcmp(argv[i], "--version") == 0) {
            return true;
        }
    }
    return false;
}

} // namespace pogohome_api
