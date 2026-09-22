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

#include <string>

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

} // namespace pogohome_api
