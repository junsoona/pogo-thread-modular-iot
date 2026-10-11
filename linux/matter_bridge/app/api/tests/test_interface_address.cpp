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

#include <array>
#include <string>

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

    printf("\n-- scenario: which address of a Thread interface is its Mesh-Local EID (LOCATE's answer) --\n");
    {
        using Addr = std::array<uint8_t, 16>;
        // What `ip -6 addr show dev wpan0` lists on an OTBR that is a leader
        // with border routing: link-local, the Mesh-Local EID, its routing
        // locator, two anycast locators, an OMR address (another fd.. /64).
        const Addr link_local = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0xb4, 0x3c, 0x32, 0xf7, 0xab, 0xe0, 0xa1, 0xea};
        const Addr eid = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 0x8c, 0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79};
        const Addr rloc = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 0, 0, 0, 0xff, 0xfe, 0, 0x30, 0x00};
        const Addr leader_aloc = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 0, 0, 0, 0xff, 0xfe, 0, 0xfc, 0x00};
        const Addr service_aloc = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 0, 0, 0, 0xff, 0xfe, 0, 0xfc, 0x10};
        const Addr omr = {0xfd, 0x7a, 0x9b, 0x01, 0x02, 0x03, 0, 1, 0x5e, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
        const Addr global = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 1, 0x5e, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
        Addr out{};
        API_CHECK(pogohome_api::PickThreadMeshLocalEid({link_local, omr, eid, rloc, leader_aloc, service_aloc, global}, out) &&
                      out == eid,
                  "among link-local, OMR, global, locators and the EID: the EID, whatever the order");
        API_CHECK(pogohome_api::PickThreadMeshLocalEid({rloc, eid}, out) && out == eid, "EID and locator alone: the EID");
        API_CHECK(pogohome_api::PickThreadMeshLocalEid({eid, eid, rloc}, out) && out == eid,
                  "the same address listed twice is one address");
        out = Addr{};
        API_CHECK(!pogohome_api::PickThreadMeshLocalEid({link_local, omr, eid, global}, out) && out == Addr{},
                  "no locator anywhere (not attached to a Thread network): nothing is picked -- an fd.. address "
                  "alone is not taken for the EID");
        API_CHECK(!pogohome_api::PickThreadMeshLocalEid({rloc, leader_aloc}, out), "locators only: nothing");
        const Addr second = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
        API_CHECK(!pogohome_api::PickThreadMeshLocalEid({rloc, eid, second}, out),
                  "two non-locator addresses in the Mesh-Local Prefix: not decided here");
        const Addr other_rloc = {0xfd, 0x7a, 0x9b, 0x01, 0x02, 0x03, 0, 1, 0, 0, 0, 0xff, 0xfe, 0, 0x44, 0x00};
        API_CHECK(!pogohome_api::PickThreadMeshLocalEid({rloc, eid, other_rloc, omr}, out),
                  "locators in two different /64s: not one Thread interface, nothing is picked");
        const Addr not_ula_locator = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 1, 0, 0, 0, 0xff, 0xfe, 0, 0x30, 0x00};
        API_CHECK(!pogohome_api::PickThreadMeshLocalEid({not_ula_locator, global}, out),
                  "a locator-shaped address outside fd00::/8 does not make its prefix a Mesh-Local Prefix");
        API_CHECK(!pogohome_api::PickThreadMeshLocalEid({}, out), "no addresses: nothing");

        std::string loopback_name = FindLoopbackInterfaceName();
        API_CHECK(!pogohome_api::ResolveThreadMeshLocalEid(loopback_name, out) &&
                      !pogohome_api::ResolveThreadMeshLocalEid("pogo-test-iface-does-not-exist-xyz", out) &&
                      !pogohome_api::ResolveThreadMeshLocalEid("", out),
                  "a real interface that is no Thread interface (loopback), a missing one, an empty name: nothing");
    }
}
