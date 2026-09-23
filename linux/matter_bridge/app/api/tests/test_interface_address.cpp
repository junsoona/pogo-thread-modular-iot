// Covers pogohome_api::ResolveInterfaceAddress() against REAL interfaces on
// the host running this test. The loopback interface always carries
// 127.0.0.1 on every Linux/macOS/BSD host this repo targets, but its NAME
// is platform-specific ("lo" on Linux, "lo0" on macOS/BSD) -- correction
// pass: this test discovers the real name via getifaddrs()'s own
// IFF_LOOPBACK flag instead of hardcoding "lo" (which made this scenario
// fail outright on every macOS/BSD dev host, never actually exercising
// ResolveInterfaceAddress()'s success path there at all). This is a
// TEST-ONLY discovery mechanism -- pogohome_api::ResolveInterfaceAddress()
// itself is completely unchanged and still requires an explicit,
// caller-given interface name (this repo's own bind-policy requirement:
// never guess or default to "the loopback" or "all interfaces" in
// production, see interface_address.hpp's own top comment); this test
// simply needs a REAL name valid on whichever platform it runs on.
// The nonexistent-interface failure path is exercised with a name chosen
// to never collide with a real device.
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include "pogohome_api/interface_address.hpp"
#include "test_check.hpp"

// Returns the first interface name getifaddrs() reports with IFF_LOOPBACK
// set (there is always at least one on Linux/macOS/BSD -- "lo" or "lo0"),
// or "" if genuinely none exists (defensive; should not happen on any real
// host this repo targets).
static std::string FindLoopbackInterfaceName()
{
    struct ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) {
        return "";
    }
    std::string name;
    for (struct ifaddrs *ifa = list; ifa != nullptr; ifa = ifa->ifa_next) {
        if ((ifa->ifa_flags & IFF_LOOPBACK) != 0 && ifa->ifa_name != nullptr) {
            name = ifa->ifa_name;
            break;
        }
    }
    freeifaddrs(list);
    return name;
}

void RunInterfaceAddressTests(int *failures)
{
    printf("\n-- scenario: a nonexistent interface name fails cleanly, with a clear error, no silent fallback --\n");
    {
        pogohome_api::ResolvedAddress out;
        std::string error;
        bool ok = pogohome_api::ResolveInterfaceAddress("pogo-test-iface-does-not-exist-xyz", out, error);
        API_CHECK(!ok, "ResolveInterfaceAddress() returns false for a nonexistent interface");
        API_CHECK(!error.empty(), "a human-readable error string is set on failure");
    }

    printf("\n-- scenario: an empty interface name fails cleanly (never resolves to \"listen everywhere\") --\n");
    {
        pogohome_api::ResolvedAddress out;
        std::string error;
        bool ok = pogohome_api::ResolveInterfaceAddress("", out, error);
        API_CHECK(!ok, "ResolveInterfaceAddress() returns false for an empty interface name");
    }

    printf("\n-- scenario: the real loopback interface resolves to its own IPv4 address (correction pass: "
           "discovered by IFF_LOOPBACK rather than assuming the Linux name \"lo\", so this is meaningful on "
           "macOS/BSD too, where it's \"lo0\") --\n");
    {
        std::string loopback_name = FindLoopbackInterfaceName();
        API_CHECK(!loopback_name.empty(), "a loopback interface was found on this host (getifaddrs()/IFF_LOOPBACK)");
        if (!loopback_name.empty()) {
            pogohome_api::ResolvedAddress out;
            std::string error;
            bool ok = pogohome_api::ResolveInterfaceAddress(loopback_name, out, error);
            API_CHECK(ok, ("ResolveInterfaceAddress(\"" + loopback_name + "\") succeeds on a real host").c_str());
            if (ok) {
                API_CHECK(out.family == AF_INET, "the loopback interface resolves to an IPv4 address (127.0.0.1) "
                                                  "by this API's own IPv4-preferred policy");
                API_CHECK(out.numeric == "127.0.0.1", "the loopback interface's resolved address is exactly "
                                                       "127.0.0.1");
            }
        }
    }
}
