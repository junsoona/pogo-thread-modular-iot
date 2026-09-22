// Covers pogohome_api::ResolveInterfaceAddress() against REAL interfaces on
// the host running this test -- "lo" (loopback) exists and always carries
// 127.0.0.1 on every Linux/macOS/BSD host this repo targets, so its
// success path is exercised deterministically without needing to fake
// /proc or getifaddrs() itself. The nonexistent-interface failure path is
// exercised with a name chosen to never collide with a real device.
#include <netinet/in.h>

#include "pogohome_api/interface_address.hpp"
#include "test_check.hpp"

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

    printf("\n-- scenario: the real loopback interface resolves to its own IPv4 address --\n");
    {
        pogohome_api::ResolvedAddress out;
        std::string error;
        bool ok = pogohome_api::ResolveInterfaceAddress("lo", out, error);
        API_CHECK(ok, "ResolveInterfaceAddress(\"lo\") succeeds on a real host");
        if (ok) {
            API_CHECK(out.family == AF_INET, "\"lo\" resolves to an IPv4 address (127.0.0.1) by this API's own "
                                              "IPv4-preferred policy");
            API_CHECK(out.numeric == "127.0.0.1", "\"lo\"'s resolved address is exactly 127.0.0.1");
        }
    }
}
