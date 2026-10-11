#include "pogohome_api/interface_address.hpp"

#include <cstring>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

namespace pogohome_api {

bool ResolveInterfaceAddress(const std::string &ifname, ResolvedAddress &out, std::string &error)
{
    if (ifname.empty()) {
        error = "no interface name given";
        return false;
    }
    if (if_nametoindex(ifname.c_str()) == 0) {
        error = "interface '" + ifname + "' does not exist";
        return false;
    }

    struct ifaddrs *addrs = nullptr;
    if (getifaddrs(&addrs) != 0) {
        error = std::string("getifaddrs() failed: ") + std::strerror(errno);
        return false;
    }

    std::string ipv4_found;
    std::string ipv6_found;

    for (struct ifaddrs *a = addrs; a != nullptr; a = a->ifa_next) {
        if (a->ifa_addr == nullptr || a->ifa_name == nullptr) {
            continue;
        }
        if (ifname != a->ifa_name) {
            continue;
        }
        if (a->ifa_addr->sa_family == AF_INET && ipv4_found.empty()) {
            char buf[INET_ADDRSTRLEN];
            const auto *sin = reinterpret_cast<const struct sockaddr_in *>(a->ifa_addr);
            if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) != nullptr) {
                ipv4_found = buf;
            }
        } else if (a->ifa_addr->sa_family == AF_INET6 && ipv6_found.empty()) {
            const auto *sin6 = reinterpret_cast<const struct sockaddr_in6 *>(a->ifa_addr);
            if (!IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr) && !IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr)) {
                char buf[INET6_ADDRSTRLEN];
                if (inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf)) != nullptr) {
                    ipv6_found = buf;
                }
            }
        }
    }
    freeifaddrs(addrs);

    if (!ipv4_found.empty()) {
        out.numeric = ipv4_found;
        out.family = AF_INET;
        return true;
    }
    if (!ipv6_found.empty()) {
        out.numeric = ipv6_found;
        out.family = AF_INET6;
        return true;
    }

    error = "interface '" + ifname +
            "' has no usable IPv4 address and no non-link-local IPv6 address to bind this API's HTTP server to";
    return false;
}

bool InterfaceHasAddress(const std::string &ifname, const std::string &address, int family)
{
    if (ifname.empty() || address.empty() || if_nametoindex(ifname.c_str()) == 0) {
        return false;
    }
    struct ifaddrs *addrs = nullptr;
    if (getifaddrs(&addrs) != 0) {
        return false;
    }
    bool found = false;
    for (struct ifaddrs *a = addrs; a != nullptr && !found; a = a->ifa_next) {
        if (a->ifa_addr == nullptr || a->ifa_name == nullptr || ifname != a->ifa_name ||
            a->ifa_addr->sa_family != family) {
            continue;
        }
        char buf[INET6_ADDRSTRLEN];
        const void *raw = family == AF_INET
                              ? static_cast<const void *>(&reinterpret_cast<const struct sockaddr_in *>(a->ifa_addr)->sin_addr)
                              : static_cast<const void *>(&reinterpret_cast<const struct sockaddr_in6 *>(a->ifa_addr)->sin6_addr);
        if (inet_ntop(family, raw, buf, sizeof(buf)) != nullptr && address == buf) {
            found = true;
        }
    }
    freeifaddrs(addrs);
    return found;
}

namespace {

// 0000:00ff:fe00:xxxx -- a Thread routing or anycast locator.
bool IsThreadLocatorIid(const std::array<uint8_t, 16> &a)
{
    return a[8] == 0x00 && a[9] == 0x00 && a[10] == 0x00 && a[11] == 0xff && a[12] == 0xfe && a[13] == 0x00;
}

} // namespace

bool PickThreadMeshLocalEid(const std::vector<std::array<uint8_t, 16>> &addresses, std::array<uint8_t, 16> &out)
{
    // The /64 that holds a locator. Two different ones: not one Thread
    // interface as this function knows it.
    const std::array<uint8_t, 16> *prefix_of = nullptr;
    for (const auto &a : addresses) {
        if (a[0] != 0xfd || !IsThreadLocatorIid(a)) {
            continue;
        }
        if (prefix_of != nullptr && std::memcmp(prefix_of->data(), a.data(), 8) != 0) {
            return false;
        }
        prefix_of = &a;
    }
    if (prefix_of == nullptr) {
        return false;
    }
    const std::array<uint8_t, 16> *eid = nullptr;
    for (const auto &a : addresses) {
        if (std::memcmp(prefix_of->data(), a.data(), 8) != 0 || IsThreadLocatorIid(a)) {
            continue;
        }
        if (eid != nullptr && *eid != a) {
            return false; // two candidates: not decided here
        }
        eid = &a;
    }
    if (eid == nullptr) {
        return false;
    }
    out = *eid;
    return true;
}

bool ResolveThreadMeshLocalEid(const std::string &ifname, std::array<uint8_t, 16> &out)
{
    if (ifname.empty()) {
        return false;
    }
    struct ifaddrs *addrs = nullptr;
    if (getifaddrs(&addrs) != 0) {
        return false;
    }
    std::vector<std::array<uint8_t, 16>> found;
    for (struct ifaddrs *a = addrs; a != nullptr; a = a->ifa_next) {
        if (a->ifa_addr == nullptr || a->ifa_name == nullptr || ifname != a->ifa_name ||
            a->ifa_addr->sa_family != AF_INET6) {
            continue;
        }
        const auto *sin6 = reinterpret_cast<const struct sockaddr_in6 *>(a->ifa_addr);
        std::array<uint8_t, 16> bytes{};
        std::memcpy(bytes.data(), sin6->sin6_addr.s6_addr, bytes.size());
        found.push_back(bytes);
    }
    freeifaddrs(addrs);
    return PickThreadMeshLocalEid(found, out);
}

} // namespace pogohome_api
