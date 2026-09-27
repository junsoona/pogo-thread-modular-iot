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
#include <utility>
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

    // Multi-Power-Base milestone. True only while `online` is false because
    // the stale sweep (sweep_stale_locked()) demoted it -- never because a
    // report itself said online=false. Lets an unchanged periodic resync
    // (same generation+seq, online=true -- DuplicateExact) restore online
    // once that Power Base's link to this host comes back; without it such a
    // node stayed offline until the module happened to send a report with a
    // new seq.
    bool stale_demoted = false;

    // Multi-Power-Base milestone. Registry-wide counter value taken when this
    // node last became online *by a report* (first sighting with online=true,
    // an online=false -> online=true report, or a new boot session reporting
    // online=true) -- never by recovering from stale_demoted. 0 until then.
    // A module moved from one Power Base to another is briefly present under
    // both (the old Base only reports it offline after its own stale sweep);
    // a reader that needs exactly one owner per module_uuid picks the online
    // entry with the largest attach_order, i.e. the most recent physical
    // attach. See docs/pogohome-api-v1.md "Power Base ownership".
    uint64_t attach_order = 0;
};

// Multi-Power-Base milestone: what this host knows about one Power Base
// itself, independent of its modules. power_base_uuid is the Base's own
// eFuse EUI-64 (power_base/main/app_main.c), which every bridge_export_proto
// message already carries -- no new wire field.
struct PowerBaseState {
    Uuid power_base_uuid{};
    // Heard from (heartbeat or any accepted topology/sample/removal) within
    // the Registry's stale timeout. Demoted by the same sweep as nodes.
    bool online = false;
    std::chrono::steady_clock::time_point first_seen_at{};
    std::chrono::steady_clock::time_point last_seen_at{};
};

// One consistent copy of nodes and Power Bases taken under a single lock.
struct RegistryView {
    std::vector<NodeState> nodes;
    std::vector<PowerBaseState> power_bases;
};

enum class ApplyResult {
    AppliedNew,           // a genuine new state (2.04 Changed)
    DuplicateExact,       // byte-for-byte identical to what's already stored (2.03 Valid)
    StorageFull,          // a brand-new (power_base_uuid, module_uuid) pair, but the table is full (5.03)
    TopologyUnknown,      // apply_sample only: no topology at all for this (power_base_uuid, module_uuid) (4.04)
    PreconditionFailed,   // apply_sample only: known topology but offline, or a bitmap mismatch (4.12); also
                           // apply_topology, only while module_uuid is on the removed list (see apply_removal())
                           // and this report does not itself carry online=true -- see that method's own doc
                           // comment
    StaleSession,         // apply_topology only: an old-generation/reordered-seq snapshot, rejected (4.12)
};

// Thread-safe (one mutex guards both the writer/CoAP-handling thread and any
// reader, e.g. a future Matter Bridge poller or this program's own smoke
// stdout printer) -- see this header's own top comment.
class Registry {
public:
    static constexpr size_t kMaxNodes = 64;
    // Bounded like nodes_. When full, a new Base evicts the oldest offline
    // Base that has no nodes left; if there is none, the new Base is simply
    // not tracked (its nodes are still applied -- the API then derives that
    // Base's entry from its modules, see pogohome_api::BuildSnapshotJson()).
    static constexpr size_t kMaxPowerBases = 16;

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

    // While (power_base_uuid, module_uuid) is on the removed list (see
    // apply_removal() below),
    // this rejects the report as PreconditionFailed UNLESS it carries
    // online==true -- see apply_removal()'s own doc comment for why an
    // online==true report is the one signal treated as a genuine, automatic
    // re-approval (POGO auto-pairing already only ever produces one after a
    // real physical re-dock; see docs/pogo-lifecycle.md), clearing the
    // removed marker and falling through to the normal first-sighting apply
    // below (a fresh AppliedNew, same as any other brand-new module_uuid).
    // An online==false report for a removed module_uuid is exactly what a
    // stale firmware-side periodic topology resync of the long-since-offline
    // entry looks like (the Power Base's own topology_registry never forgets
    // a slot -- see that component's own header) -- rejecting it, rather
    // than resurrecting the node, is what keeps a removed module excluded
    // from snapshot()/HTTP/SSE until it is ACTUALLY re-approved.
    ApplyResult apply_topology(const Uuid &power_base_uuid, const Uuid &module_uuid, const Uuid &parent_uuid,
                                bool online, uint8_t depth, uint8_t role, uint16_t sensor_bitmap,
                                bool downstream_connected, uint32_t boot_session_generation, uint32_t last_seq,
                                std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    // Applies a confirmed-detach tombstone for module_uuid (see
    // BRIDGE_EXPORT_MSG_TOPOLOGY_REMOVED's own wire-format doc comment) --
    // NEVER for an ordinary offline/stale-timeout transition, which stays
    // online=false within nodes_ and must never reach this method (see this
    // header's own top comment: "online=false와 removed를 혼용하지 마라").
    //
    // Erases the (power_base_uuid, module_uuid) node from nodes_ if present
    // -- scoped to the reporting Power Base since the multi-Power-Base
    // milestone: a detach confirmed by Base A says nothing about the same
    // module now docked to Base B (with a single Base this is identical to
    // the earlier module-wide behavior) -- so
    // node_count()/get_node()/snapshot() (and therefore the HTTP/SSE API)
    // exclude it from the very next read, with no separate "removed" flag
    // for a consumer to have to check. Adds (power_base_uuid, module_uuid)
    // to a bounded removed list that apply_topology() consults so a stray
    // report for it from that same Base is never silently re-applied before
    // a genuine re-approval (see apply_topology()'s own doc comment).
    //
    // Idempotent: a SECOND apply_removal() for a module_uuid already on the
    // removed list is a DuplicateExact no-op (no second erase, no second
    // generation() bump) -- this is what makes a retried/duplicate tombstone
    // (e.g. the firmware side's own bounded retry, or two independent
    // confirmation paths for the same physical event) safe to apply more
    // than once. Returns AppliedNew (bumping generation() exactly once) the
    // first time this module_uuid is newly added to the removed list --
    // this is true even if module_uuid had no active node in nodes_ at all
    // (e.g. Linux never saw a topology report for it before the tombstone
    // arrived): the removed list itself is the durable fact being recorded,
    // independent of whatever nodes_ happened to already contain. The
    // removed list is bounded at kMaxNodes distinct entries; when full, the
    // OLDEST marker is evicted -- never a StorageFull refusal, which the
    // Power Base would retry forever while Linux kept the module (see
    // registry.cpp).
    ApplyResult apply_removal(const Uuid &power_base_uuid, const Uuid &module_uuid,
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
    // concept -- never authoritative for any module. Since the
    // multi-Power-Base milestone they mark the Power Base itself as heard
    // from (see PowerBaseState), so a Base with no modules docked yet is
    // still visible. generation() advances only when that Base is first seen
    // or comes back online -- a steady 10 s heartbeat never wakes SSE.
    void apply_heartbeat(const Uuid &power_base_uuid, uint32_t uptime_ms, uint16_t node_count,
                         std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

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

    // Nodes (exactly as snapshot() returns them) plus every tracked Power
    // Base, both taken under one mutex_ acquisition, with the same read-time
    // staleness correction applied to both.
    RegistryView view(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;

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
    // Bounded set of module_uuids a confirmed apply_removal() tombstone has
    // been applied for and that have not yet been re-approved by a fresh
    // online==true apply_topology() -- see both methods' own doc comments.
    // Deliberately separate from nodes_ (which never holds a removed
    // module_uuid at all): a removed module_uuid must keep being rejected
    // even though it has no entry in nodes_ to check against.
    std::vector<std::pair<Uuid, Uuid>> removed_; // (power_base_uuid, module_uuid)
    std::vector<PowerBaseState> power_bases_;
    uint64_t attach_counter_ = 0;
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

    // Caller must hold mutex_. Records that power_base_uuid was just heard
    // from. Returns true if that Base is new or was offline -- a visible
    // change the caller must cover with note_applied_locked(), folded into
    // the same single bump when the message also changed a node, so one
    // received message never advances generation() twice. See
    // kMaxPowerBases for the full-table policy.
    bool note_power_base_seen_locked(const Uuid &power_base_uuid, std::chrono::steady_clock::time_point now);

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
    PowerBaseState with_staleness_applied(PowerBaseState b, std::chrono::steady_clock::time_point now) const;

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
