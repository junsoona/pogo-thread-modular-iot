// Resolves a Linux network interface name to the LAN address this API's
// HTTP server should bind to -- there is no equivalent code anywhere else
// in this repo to reuse (linux/matter_bridge/ingest/src/server.cpp binds
// its CoAP socket to in6addr_any and instead filters incoming datagrams by
// ifindex via IPV6_PKTINFO, see that file's own comment; it never needs an
// actual address). This API's own bind policy is different and stricter by
// requirement (see docs/pogohome-api-v1.md's own "bind policy" section):
// bind directly to the interface's own address, never 0.0.0.0/:: or
// "listen everywhere".
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pogohome_api {

struct ResolvedAddress {
    std::string numeric; // a bare numeric literal, e.g. "192.168.1.20" or "2001:db8::1"
    int family = 0;      // AF_INET or AF_INET6
};

// Picks ONE address for `ifname`: this interface's first configured global
// IPv4 address if it has one, else its first configured non-link-local,
// non-loopback global IPv6 address, else failure. IPv4 is preferred because
// this API's intended interface (see the follow-up spec's own example,
// `--api-interface wlp5x0`) is a Wi-Fi/LAN interface an iOS app on the same
// home network reaches by its ordinary IPv4 LAN address; link-local IPv6
// (fe80::/10) is deliberately skipped because binding to it correctly
// requires an explicit scope id, which a bare numeric-literal HTTP bind
// address cannot carry -- a real, if narrower, limitation this repo's own
// docs/pogohome-api-v1.md documents rather than silently working around.
//
// Returns false (with `error` set to a caller-loggable, human-readable
// reason) if: `ifname` is empty, the interface does not exist
// (if_nametoindex() fails), or getifaddrs() finds no usable address on it
// at all. Never falls back to any OTHER interface or to an "any interface"
// bind -- same explicit-required-interface policy as
// ingest::ServerConfig::interface_name (see ingest/include/ingest/server.hpp's
// own SECURITY NOTE).
bool ResolveInterfaceAddress(const std::string &ifname, ResolvedAddress &out, std::string &error);

// Whether `ifname` currently carries `address` (a ResolvedAddress's numeric
// form, of `family`). The API server stays bound to the address it resolved at
// startup; once that address leaves the interface (DHCP renumbering, the link
// going away) the server can no longer be reached there, which is what the
// Bonjour advertiser uses this for (see mdns_advertiser.hpp).
bool InterfaceHasAddress(const std::string &ifname, const std::string &address, int family);

// The Thread Mesh-Local EID among the IPv6 addresses of ONE Thread interface
// (the OTBR's wpan0), told apart by the shape of the addresses alone -- no
// OTBR control socket, no helper, no privilege:
//
//   - a Thread interface carries its routing locator, and usually anycast
//     locators, all in the Mesh-Local Prefix and all with the interface
//     identifier 0000:00ff:fe00:xxxx. A /64 inside fd00::/8 that holds such an
//     address is the Mesh-Local Prefix; no other prefix on the interface (the
//     OMR prefix, a domain prefix, link-local) ever holds one;
//   - the Mesh-Local EID is the one address in that /64 that is NOT a
//     locator.
//
// False -- nothing is guessed -- when no /64 holds a locator (the interface
// is not attached to a Thread network), when more than one does, or when the
// Mesh-Local /64 holds no or several non-locator addresses. This is the
// address `ot-ctl ipaddr mleid` prints and the one an enrollment hands to a
// Power Base (commissioner.hpp); it is what a Power Base's LOCATE is answered
// with (power_base_management.hpp), read at that moment, so an OTBR whose
// EID changed is described as it is now.
bool PickThreadMeshLocalEid(const std::vector<std::array<uint8_t, 16>> &addresses, std::array<uint8_t, 16> &out);
// The same for the live interface `ifname` (getifaddrs()).
bool ResolveThreadMeshLocalEid(const std::string &ifname, std::array<uint8_t, 16> &out);

} // namespace pogohome_api
