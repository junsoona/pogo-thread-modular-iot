// Durable module_uuid -> Matter dynamic endpoint ID mapping, stored under
// this program's --state-dir (see linux/matter_bridge/app's own README).
// This is the ONLY thing that makes "the same module_uuid gets the same
// Matter endpoint after a restart" true -- Matter fabric/ACL state is kept
// durable by the CHIP SDK's own persistent storage delegate (also under
// --state-dir, see linux/matter_bridge/app/chip/), but the SDK has no
// concept of "this application-specific 8-byte UUID picks this endpoint
// ID"; that mapping is this program's own responsibility, hence this file.
//
// CHIP-SDK-independent on purpose (see value_mapping.hpp's own top comment
// for why linux/matter_bridge/app/core/ has zero connectedhomeip
// dependency) -- linux/matter_bridge/app/chip/ is the only caller.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ingest/registry.hpp"

namespace matter_bridge {

using Uuid = ingest::Uuid;

enum class LoadResult {
    Ok,      // an existing, valid mapping file was loaded
    NotFound, // no mapping file exists yet (first run) -- an empty store, not an error
    Corrupt,  // a mapping file exists but failed its integrity check
};

// module_uuid -> Matter dynamic endpoint ID, persisted under
// "<state_dir>/endpoint_map.bin" via a temp-file + fflush + fsync + rename()
// sequence (POSIX rename() onto an existing path is atomic -- a reader can
// only ever observe a fully-old or fully-new file, never a torn write, so
// this program crashing or being killed mid-persist can never corrupt the
// existing mapping; see persist_locked() in the .cpp for the exact
// sequence). A CRC16 trailer (the same pogo_crc16_ccitt() the rest of this
// codebase already uses for wire-format integrity, see
// components/pogo_proto/pogo_crc16.h) additionally guards against disk-level
// bit rot between runs, independent of the write-time atomicity guarantee
// above.
class EndpointStore {
public:
    // 0 and 1 are the bridge's own fixed root/aggregator endpoints, and 2 is
    // the fixed "dummy" endpoint the .zap file reserves purely so ZAP
    // generates code for every cluster type this bridge ever puts on a
    // dynamic endpoint (see linux/matter_bridge/app/bridge-common/README.md)
    // -- the first ID this store ever hands out is the next one after that.
    static constexpr uint16_t kFirstDynamicEndpointId = 3;
    // Mirrors ingest::Registry::kMaxNodes -- this store never needs to hold
    // more distinct module_uuids than the ingest registry can ever track in
    // the first place.
    static constexpr size_t kMaxEndpoints = ingest::Registry::kMaxNodes;

    explicit EndpointStore(std::string state_dir);

    // Reads and validates the mapping file, if any. Must be called exactly
    // once before get_or_assign()/lookup()/reverse_lookup(). Corrupt is
    // deliberately NOT treated as "start over with an empty store" by this
    // class -- see this header's own top comment on why a caller (main.cpp)
    // must treat Corrupt as fatal (refuse to start) rather than silently
    // reassigning endpoint IDs.
    LoadResult load();

    // Returns the endpoint ID already assigned to module_uuid. If
    // module_uuid has never been seen before, assigns the smallest unused
    // ID (>= kFirstDynamicEndpointId) and durably persists the ENTIRE
    // updated table before returning -- an ID is never handed to a caller
    // that isn't already safely on disk, so a crash immediately after this
    // call can never leave a Matter endpoint alive whose ID a restart would
    // then reassign to a different module_uuid. Returns std::nullopt if the
    // store is already at kMaxEndpoints capacity, or if persisting the new
    // entry fails (a full or read-only disk) -- either way, the caller must
    // NOT create a Matter endpoint for this module_uuid this round (see
    // this header's own top comment).
    std::optional<uint16_t> get_or_assign(const Uuid &module_uuid);

    std::optional<uint16_t> lookup(const Uuid &module_uuid) const;
    std::optional<Uuid> reverse_lookup(uint16_t endpoint_id) const;

    size_t size() const { return entries_.size(); }

private:
    struct Entry {
        Uuid uuid{};
        uint16_t endpoint_id = 0;
    };

    std::string state_dir_;
    std::vector<Entry> entries_;
    bool loaded_ = false;

    std::string path() const;
    uint16_t next_free_endpoint_id_locked() const;
    bool persist_locked() const;
};

} // namespace matter_bridge
