// pogohome_api's build identity when the build did NOT generate the stamp
// header (docs/versioning.md): everything is "unknown" and nothing is
// invented. Compiled without the generated include directory on purpose.
#include <cstdio>
#include <string>

#include "pogohome_api/build_info.hpp"

int main()
{
    int failures = 0;
    auto check = [&failures](bool cond, const char *message) {
        std::printf("%s %s\n", cond ? "ok:  " : "FAIL:", message);
        if (!cond) {
            failures++;
        }
    };
    const pogohome_api::BuildInfo &info = pogohome_api::CurrentBuildInfo();
    check(!info.known, "a build without the stamp knows that it has none");
    check(info.component == "linux_bridge", "it is still the linux_bridge component");
    check(info.version == "unknown" && info.build_id == "unknown" && info.commit == "unknown" && info.kind == "unknown" &&
              info.dirty == "unknown" && info.fingerprint == "unknown",
          "version, build id, commit, kind, dirty and fingerprint are all 'unknown'");
    check(pogohome_api::BuildInfoJson().empty(), "the health build field is left out");
    check(pogohome_api::VersionLine("pogo_matter_bridge") == "pogo_matter_bridge unknown (linux_bridge, build unknown, unknown)",
          "--version says unknown rather than a version");
    check(std::string(pogohome_api::BuildInfoString()).find("version=unknown") != std::string::npos, "and so does the embedded string");
    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
