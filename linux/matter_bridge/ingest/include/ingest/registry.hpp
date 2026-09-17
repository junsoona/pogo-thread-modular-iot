// Bounded, in-memory, UUID-keyed registry of everything this ingest program
// has learned from a Power Base's bridge_export_proto stream -- the future
// Matter Bridge's own read model (this follow-up deliberately stops short
// of building that Bridge itself; see docs/matter-bridge.md). Mirrors the
// firmware's own topology_registry_t/sensor_root_aggregator design
// deliberately: bounded array, mutex-protected, dedup by an exact-match key
// -- never an unbounded/dynamically-growing table.
#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

extern "C" {
#include "sensor_channel/sensor_channel.h"
}

namespace ingest {

using Uuid = std::array<uint8_t, 8>;

struct ChannelReading {
    uint8_t type = 0;
    uint8_t scale = 0;
    uint8_t status = 0;
    int32_t value = 0;
};

struct NodeState {
    Uuid power_base_uuid{};
    Uuid module_uuid{};
    Uuid parent_uuid{};

    bool has_topology = false;
    bool online = false;
    uint8_t depth = 0;
    uint8_t role = 0;
    uint16_t sensor_bitmap = 0;
    bool downstream_connected = false;
    uint32_t boot_session_generation = 0;
    uint32_t last_seq = 0; // topology report sequence

    bool has_sample = false;
    uint32_t last_sample_seq = 0;
    uint32_t last_sample_uptime_ms = 0;
    std::vector<ChannelReading> last_channels;
};

enum class ApplyResult {
    AppliedNew,    // a genuine new state (2.04 Changed)
    DuplicateExact, // byte-for-byte identical to what's already stored (2.03 Valid)
    StorageFull,   // a brand-new (power_base_uuid, module_uuid) pair, but the table is full (5.03)
};

// Thread-safe (one mutex guards both the writer/CoAP-handling thread and any
// reader, e.g. a future Matter Bridge poller or this program's own smoke
// stdout printer) -- see this header's own top comment.
class Registry {
public:
    static constexpr size_t kMaxNodes = 64;

    ApplyResult apply_topology(const Uuid &power_base_uuid, const Uuid &module_uuid, const Uuid &parent_uuid,
                                bool online, uint8_t depth, uint8_t role, uint16_t sensor_bitmap,
                                bool downstream_connected, uint32_t boot_session_generation, uint32_t last_seq);

    // sample_seq/uptime_ms/channels are this sample's own decoded fields
    // (via the real sensor_channel_unpack(), never reinterpreted here).
    ApplyResult apply_sample(const Uuid &power_base_uuid, const Uuid &module_uuid, uint32_t sample_seq,
                              uint32_t uptime_ms, const std::vector<ChannelReading> &channels);

    // Heartbeats are pure liveness signals with no meaningful "duplicate"
    // concept (this follow-up's own design: never authoritative) -- always
    // AppliedNew, and updates node_count-adjacent bookkeeping only if the
    // power_base_uuid is already known from at least one topology node.
    void apply_heartbeat(const Uuid &power_base_uuid, uint32_t uptime_ms, uint16_t node_count);

    size_t node_count() const;
    bool get_node(size_t index, NodeState &out) const;

private:
    mutable std::mutex mutex_;
    std::vector<NodeState> nodes_;

    // Caller must hold mutex_. Returns nullptr if not found.
    NodeState *find_locked(const Uuid &power_base_uuid, const Uuid &module_uuid);
};

} // namespace ingest
