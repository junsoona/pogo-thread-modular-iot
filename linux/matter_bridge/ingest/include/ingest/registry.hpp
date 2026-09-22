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
//
// Correction pass 2 (bounded online-expiry, this follow-up): this Registry
// previously had NO concept of "no message in a long time" at all -- an
// `online=true` node stayed that way forever unless a NEW topology report
// ever arrived saying otherwise. If the Power Base's own
// topology_registry_sweep_stale()-driven offline export was ever lost (a
// crash, an unreachable Linux host -- see linux_bridge_exporter's own
// "delivery unconfirmed" warning, observed repeatedly for extended periods
// in this repo's own power-base-bridge.log), this Registry's `online` --
// the value any consumer (this program's own stdout printer, the Matter
// Bridge watcher, or any external API built on this Registry) reads as
// authoritative -- stayed stuck at true with no path back to correctness.
// See kDefaultStaleTimeout's own doc comment for the bound chosen and why.
// This is now a genuinely ACTIVE, not merely read-computed, correction: a
// dedicated bounded-timer thread (never a busy loop -- see sweep_thread_
// below) actually mutates a newly-stale node's stored `online` to false,
// which goes through the EXACT SAME note_applied_locked() every other real
// change does -- so generation() advances, wait_for_change() wakers wake
// up, and (in linux/matter_bridge/app/chip/main.cpp's unmodified
// RegistryWatcherThreadMain) BridgeManager::Reconcile() sees a real
// `reachable=false` update on its very next pass, exactly like any other
// registry change. get_node()/snapshot() additionally still apply the same
// timeout correction to their own returned copy (with_staleness_applied())
// as a read-time safety net, so a reader can never observe a stale `online`
// even in the (at most sweep_interval_-wide) window before the background
// thread's next tick actually catches up.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
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

    // Monotonic timestamp of the last message actually ACCEPTED for this
    // node (AppliedNew or DuplicateExact from apply_topology()/apply_sample()
    // -- never a rejection, which proves nothing about current liveness; see
    // e.g. StaleSession, a delayed packet that says nothing about "now").
    // This is what lets Registry itself (see kDefaultStaleTimeout below)
    // demote `online` to false after a bounded silence, independent of
    // whether the Power Base ever manages to export an explicit offline
    // topology report -- see that constant's own doc comment for why this
    // exists as a second, independent line of defense rather than relying
    // solely on the firmware's own topology_registry_sweep_stale().
    std::chrono::steady_clock::time_point last_seen_at{};
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

    // Default bounded timeout used to demote a node's reported `online`
    // to false, purely from elapsed wall-clock time since the last message
    // this Registry actually accepted for it -- with NO fresh
    // topology/sample report (of any kind, including a sudden power loss
    // with no detach message at all), `online` must never stay true
    // forever (see this header's own top comment / correction-pass note:
    // this is what previously made the API's `online` "authoritative"
    // value get stuck at true).
    //
    // Chosen to EXACTLY mirror the firmware's own already-vetted
    // topology_stale_timeout_ms (configs/power_base.json, 120000ms --
    // components/topology_registry_sweep_stale()'s bound, enforced every
    // 5s by power_base's stale_sweep_task): this Registry's own timeout is
    // a SECOND, INDEPENDENT line of defense against exactly the same
    // condition, not a competing policy -- if the Power Base's own sweep
    // and its (reliable, retried) offline export both work, that explicit
    // offline report arrives and applies here well before this timeout
    // would ever fire on its own. It only matters when that final export
    // is itself lost (crash, reboot, a dropped UDP datagram with retries
    // exhausted) -- otherwise this Registry would echo a stale "online"
    // forever with no path back to true except a fresh report.
    //
    // 120s is safe against false positives on a healthy module: the
    // exporter's own periodic full topology+latest-sample resync runs
    // every LINUX_BRIDGE_EXPORTER_FULL_RESYNC_INTERVAL_MS=5000ms
    // (components/linux_bridge_exporter/Kconfig) REGARDLESS of that
    // module's actual sensor measurement/reporting period, so a healthy
    // node refreshes last_seen_at at least every 5s -- 120s is a ~24x
    // margin over that, tolerating many consecutive dropped resync
    // datagrams before ever misclassifying a live module as offline.
    static constexpr std::chrono::milliseconds kDefaultStaleTimeout{120000};

    // How often the background sweep thread (see sweep_thread_) wakes to
    // check for newly-stale nodes -- a bounded condition_variable timed
    // wait, never a busy loop (see sweep_thread_main()'s own doc comment).
    // 5000ms mirrors LINUX_BRIDGE_EXPORTER_FULL_RESYNC_INTERVAL_MS (the
    // same cadence a healthy node's liveness already gets refreshed at),
    // so worst-case detection latency past kDefaultStaleTimeout is bounded
    // by this same ~5s, not an unbounded/arbitrary poll period. Negligible
    // cost: one scan of at most kMaxNodes=64 small structs per tick.
    static constexpr std::chrono::milliseconds kDefaultSweepInterval{5000};

    explicit Registry(std::chrono::milliseconds stale_timeout = kDefaultStaleTimeout,
                       std::chrono::milliseconds sweep_interval = kDefaultSweepInterval);
    ~Registry();
    Registry(const Registry &) = delete;
    Registry &operator=(const Registry &) = delete;

    ApplyResult apply_topology(const Uuid &power_base_uuid, const Uuid &module_uuid, const Uuid &parent_uuid,
                                bool online, uint8_t depth, uint8_t role, uint16_t sensor_bitmap,
                                bool downstream_connected, uint32_t boot_session_generation, uint32_t last_seq,
                                std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    // sample_seq/uptime_ms/channels are this sample's own decoded fields
    // (via the real sensor_channel_unpack(), never reinterpreted here).
    // sample_sensor_bitmap is the SAMPLE's own declared bitmap (distinct
    // from the topology's stored sensor_bitmap, which is checked against
    // it -- see this header's own top comment).
    ApplyResult apply_sample(const Uuid &power_base_uuid, const Uuid &module_uuid, uint32_t sample_seq,
                              uint32_t uptime_ms, uint16_t sample_sensor_bitmap,
                              const std::vector<ChannelReading> &channels,
                              std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    // Heartbeats are pure liveness signals with no meaningful "duplicate"
    // concept (this follow-up's own design: never authoritative) -- always
    // AppliedNew, and updates node_count-adjacent bookkeeping only if the
    // power_base_uuid is already known from at least one topology node.
    void apply_heartbeat(const Uuid &power_base_uuid, uint32_t uptime_ms, uint16_t node_count);

    size_t node_count() const;
    // `now` lets a caller (or a test) evaluate staleness at a specific
    // instant; defaults to the real clock for every production call site.
    // The returned NodeState's `online` is the EFFECTIVE value (raw stored
    // online AND-ed with "still within kDefaultStaleTimeout/stale_timeout_
    // of last_seen_at") -- never the raw stored flag by itself -- so every
    // reader (this program's own stdout printer, BridgeManager::Reconcile(),
    // and any JSON API built on top of this Registry) gets the
    // timeout-corrected value for free, with no wire/API shape change.
    bool get_node(size_t index, NodeState &out,
                  std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;

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
    // See get_node()'s own doc comment: `online` on each returned NodeState
    // is the timeout-corrected EFFECTIVE value, not the raw stored flag.
    std::vector<NodeState> snapshot(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;

    // Blocks the calling thread until generation() differs from
    // `last_seen_generation` or `timeout` elapses -- a condition_variable
    // wait, never a busy/polling loop. On a real change, updates
    // *last_seen_generation to the new generation and returns true; on
    // timeout, leaves *last_seen_generation untouched and returns false.
    //
    // A node crossing stale_timeout_ with no fresh message at all is a REAL
    // change by this same definition: the background sweep thread (see
    // sweep_thread_) mutates it and calls note_applied_locked() exactly
    // like apply_topology()/apply_sample() do, so a waiter blocked here
    // wakes promptly (within kDefaultSweepInterval/sweep_interval_) on an
    // offline transition with no new registry traffic at all, not only on
    // one driven by a fresh message.
    bool wait_for_change(uint64_t *last_seen_generation, std::chrono::milliseconds timeout) const;

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::vector<NodeState> nodes_;
    uint64_t generation_ = 0;
    std::chrono::milliseconds stale_timeout_;
    std::chrono::milliseconds sweep_interval_;
    std::atomic<bool> stop_sweep_{false};
    // Started in the constructor, stopped+joined in the destructor -- the
    // ACTIVE half of bounded online-expiry (see this header's own top
    // comment). Never a busy loop: sweep_thread_main() blocks in
    // cv_.wait_for() the entire time between ticks, woken early by any real
    // apply_topology()/apply_sample() change (harmless -- it just finds
    // nothing new to sweep and waits again) or by the destructor's own
    // stop_sweep_ + notify_all().
    std::thread sweep_thread_;

    // Caller must hold mutex_. Returns nullptr if not found.
    NodeState *find_locked(const Uuid &power_base_uuid, const Uuid &module_uuid);

    // Caller must hold mutex_. Bumps generation_ and wakes every
    // wait_for_change() waiter -- called exactly once per AppliedNew return,
    // from inside the same critical section that just applied the change
    // (so a waiter that wakes and then calls snapshot() always sees AT
    // LEAST the change that woke it, never a stale pre-change table), AND
    // exactly once per node the background sweep thread newly demotes to
    // offline (see sweep_stale_locked()).
    void note_applied_locked();

    // Returns `n` with `online` demoted to false if it's still marked
    // online but stale_timeout_ has elapsed since last_seen_at -- a pure,
    // read-only computation (never mutates stored state) applied to
    // get_node()/snapshot()'s own returned copy as a safety net between
    // sweep_thread_ ticks -- see this header's own top comment.
    NodeState with_staleness_applied(NodeState n, std::chrono::steady_clock::time_point now) const;

    // Caller must hold mutex_. Mutates any currently-online node whose
    // stale_timeout_ has elapsed since last_seen_at to `online = false`,
    // logging the transition exactly once (never repeated on a later sweep
    // that finds it already offline) and calling note_applied_locked() once
    // per node actually demoted this pass -- the ACTIVE counterpart to
    // with_staleness_applied() above. Called only from sweep_thread_main().
    void sweep_stale_locked(std::chrono::steady_clock::time_point now);

    // Runs for this Registry's entire lifetime once started (constructor)
    // until stop_sweep_ is set (destructor). Never a busy loop: the ONLY
    // work per iteration is one sweep_stale_locked() call (a bounded scan
    // of at most kMaxNodes nodes) bracketing a cv_.wait_for(sweep_interval_)
    // that blocks the rest of the time.
    void sweep_thread_main();
};

} // namespace ingest
