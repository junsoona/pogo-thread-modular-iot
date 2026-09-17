// Bounded, in-memory, UUID-keyed registry of everything this ingest program
// has learned from a Power Base's bridge_export_proto stream -- the future
// Matter Bridge's own read model (this follow-up deliberately stops short
// of building that Bridge itself; see docs/matter-bridge.md). Mirrors the
// firmware's own topology_registry_t/sensor_root_aggregator design
// deliberately: bounded array, mutex-protected, dedup by an exact-match key
// -- never an unbounded/dynamically-growing table.
//
// Correction pass (this follow-up):
//   - apply_topology() now mirrors components/topology_registry's own
//     generation/sequence ordering policy EXACTLY (see that component's own
//     header comment) instead of only detecting byte-for-byte exact
//     duplicates: within the SAME boot_session_generation, only a
//     STRICTLY LARGER last_seq is applied; a smaller one is rejected as
//     STALE (a reordered/delayed packet must never roll back an already
//     newer state, including resurrecting an online flag a later report
//     already turned offline). A generation change is accepted as a new
//     session UNLESS it exactly matches the just-superseded generation (a
//     delayed packet from the immediately-preceding session).
//   - apply_sample() now requires this (power_base_uuid, module_uuid) pair
//     to already have known, ONLINE topology with a sensor_bitmap that is a
//     superset of the sample's own bitmap, and requires every channel's
//     type to actually be a bit set in the sample's declared bitmap --
//     mirroring components/power_base_transport's own "/s" handler gating.
#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
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
    // Mirrors components/topology_registry's own bookkeeping exactly (see
    // that component's header comment) -- the generation just before
    // boot_session_generation, so a delayed packet from that exact old
    // session is recognized and rejected regardless of how its numeric
    // value compares to the current one.
    uint32_t superseded_generation = 0;
    bool has_superseded_generation = false;

    bool has_sample = false;
    uint32_t last_sample_seq = 0;
    uint32_t last_sample_uptime_ms = 0;
    std::vector<ChannelReading> last_channels;
};

enum class ApplyResult {
    AppliedNew,           // a genuine new state (2.04 Changed)
    DuplicateExact,       // byte-for-byte identical to what's already stored (2.03 Valid)
    StorageFull,          // a brand-new (power_base_uuid, module_uuid) pair, but the table is full (5.03)
    TopologyUnknown,      // apply_sample only: no topology at all for this (power_base_uuid, module_uuid) (4.04)
    PreconditionFailed,   // apply_sample only: known topology but offline, or a bitmap mismatch (4.12)
    StaleSession,         // apply_topology only: an old-generation/reordered-seq snapshot, rejected (4.12)
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
    // sample_sensor_bitmap is the SAMPLE's own declared bitmap (distinct
    // from the topology's stored sensor_bitmap, which is checked against
    // it -- see this header's own top comment).
    ApplyResult apply_sample(const Uuid &power_base_uuid, const Uuid &module_uuid, uint32_t sample_seq,
                              uint32_t uptime_ms, uint16_t sample_sensor_bitmap,
                              const std::vector<ChannelReading> &channels);

    // Heartbeats are pure liveness signals with no meaningful "duplicate"
    // concept (this follow-up's own design: never authoritative) -- always
    // AppliedNew, and updates node_count-adjacent bookkeeping only if the
    // power_base_uuid is already known from at least one topology node.
    void apply_heartbeat(const Uuid &power_base_uuid, uint32_t uptime_ms, uint16_t node_count);

    size_t node_count() const;
    bool get_node(size_t index, NodeState &out) const;

    // --- Read-model additions for a future Matter Bridge (this follow-up)
    // -- see docs/matter-bridge.md. All three are safe to call from any
    // thread OTHER than the CoAP-handling one (which never calls them):
    // each takes mutex_ only long enough to copy data out, never while a
    // caller might block on Matter SDK calls or file I/O (this header's own
    // top comment's rule applies to callers just as much as it does to
    // apply_topology()/apply_sample() themselves).

    // Monotonically increasing, bumped exactly once per AppliedNew result
    // from apply_topology()/apply_sample() (never for DuplicateExact or any
    // rejection) -- a cheap "did anything actually change" signal so a
    // reader never has to diff full snapshots to notice nothing happened.
    uint64_t generation() const;

    // A full, consistent copy of every currently-tracked node, taken under
    // one mutex_ acquisition (never a NodeState half-updated mid-copy).
    std::vector<NodeState> snapshot() const;

    // Blocks the calling thread until generation() differs from
    // `last_seen_generation` or `timeout` elapses -- a condition_variable
    // wait, never a busy/polling loop. On a real change, updates
    // *last_seen_generation to the new generation and returns true; on
    // timeout, leaves *last_seen_generation untouched and returns false.
    bool wait_for_change(uint64_t *last_seen_generation, std::chrono::milliseconds timeout) const;

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::vector<NodeState> nodes_;
    uint64_t generation_ = 0;

    // Caller must hold mutex_. Returns nullptr if not found.
    NodeState *find_locked(const Uuid &power_base_uuid, const Uuid &module_uuid);

    // Caller must hold mutex_. Bumps generation_ and wakes every
    // wait_for_change() waiter -- called exactly once per AppliedNew return,
    // from inside the same critical section that just applied the change
    // (so a waiter that wakes and then calls snapshot() always sees AT
    // LEAST the change that woke it, never a stale pre-change table).
    void note_applied_locked();
};

} // namespace ingest
