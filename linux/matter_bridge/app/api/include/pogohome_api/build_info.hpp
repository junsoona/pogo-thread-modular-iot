// What this build of the Linux bridge is: component, product version and
// build identity (docs/versioning.md). The values come from the repository's
// versions.json and from tools/pogo_version.py at build time -- generated
// into the build directory (CMake) or api/generated/ (the GN build, by
// tools/build_matter_bridge.sh); nothing here is typed in.
//
// A build made without that step (the header is simply absent) says
// "unknown" for everything: it never pretends to be a version.
//
// This is identification, not negotiation. No client decides what this hub
// supports from these strings; the API's own fields and capability values
// (schema_version, settings_api, power_base_management, ...) say that, as
// before.
#pragma once

#include <string>

namespace pogohome_api {

struct BuildInfo {
    std::string component;   // "linux_bridge"
    std::string version;     // MAJOR.MINOR.PATCH, or "unknown"
    std::string build_id;    // <commit12>[+dirty].<fingerprint12>, or "unknown"
    std::string kind;        // "development" or "release", or "unknown"
    std::string variant;
    std::string commit;      // full SHA, or "unknown"
    std::string dirty;       // "true", "false" or "unknown"
    std::string fingerprint; // SHA-256 of the sources, or "unknown"
    bool known;              // false: built without the version stamp
};

const BuildInfo &CurrentBuildInfo();

// "<program> 0.1.0 (linux_bridge, build 590e2a11d2cb+dirty.0123456789ab, development)"
std::string VersionLine(const std::string &program);

// The value of GET /api/v1/health's optional `build` field:
// {"component":"linux_bridge","version":"0.1.0","build_id":"...","build_kind":"development"}
// Empty when this build has no stamp -- the field is then left out, and the
// health body is byte for byte what it was before this field existed.
std::string BuildInfoJson();

// "POGO_BUILD_INFO{...}": in the binary so that it can be read back out of
// it (tools/pogo_version.py manifest-create / manifest-verify).
const char *BuildInfoString();

// True when argv has "--version": the caller prints VersionLine() and exits
// 0 before it touches anything else (no state directory, no socket).
bool WantsVersion(int argc, char **argv);

} // namespace pogohome_api
