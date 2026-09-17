// std::unordered_map/unordered_set hash for ingest::Uuid (std::array<uint8_t,
// 8>) -- std::array has operator== but no std::hash specialization, and
// this bridge needs uuid-keyed maps in more than one place (bridge_manager,
// and its own tests), so this is factored out rather than repeated.
#pragma once

#include <cstddef>

#include "ingest/registry.hpp"

namespace matter_bridge {

struct UuidHash {
    size_t operator()(const ingest::Uuid &uuid) const
    {
        // FNV-1a over the 8 raw bytes -- this is a hash-table key, not a
        // security or wire-format checksum, so a simple, well-distributed,
        // dependency-free hash is all that's needed here.
        size_t h = 1469598103934665603ull;
        for (uint8_t b : uuid) {
            h ^= b;
            h *= 1099511628211ull;
        }
        return h;
    }
};

} // namespace matter_bridge
