// Turns an ingest::Registry snapshot into the minimal set of Matter-facing
// changes that actually need to happen -- endpoint creation (once, ever,
// per module_uuid), Reachable transitions, and attribute value pushes --
// without ever touching the Matter SDK itself (see value_mapping.hpp's own
// top comment for why linux/matter_bridge/app/core/ has zero
// connectedhomeip dependency).
//
// Threading contract (revised this pass -- transport-load reduction, Linux
// Registry -> Matter): TWO threads now touch a BridgeManager instance --
// see linux/matter_bridge/app/chip/main.cpp:
//   1. The "registry watcher" thread calls Reconcile()/ReconcileOne() (pure
//      reads against last_applied_/endpoint_store_) and
//      TryBeginSchedule()/AbortSchedule() to decide, per module_uuid,
//      whether and how often to actually call
//      chip::DeviceLayer::PlatformMgr().ScheduleWork() -- see
//      TryBeginSchedule()'s own doc comment for the coalescing+rate-limit
//      policy this implements.
//   2. The Matter event-loop thread, from inside the ScheduleWork()
//      callback (after actually applying a ModuleUpdate to the real SDK),
//      calls CommitApplied() (to record which fields, if any, truly
//      succeeded -- see that method's own doc comment on why this is no
//      longer done inside Reconcile() itself) and EndSchedule().
// mutex_ guards every access to last_applied_ and schedule_state_ so these
// two threads never race; it is NEVER held across a Matter SDK call or file
// I/O (endpoint_store_.get_or_assign() included) -- see each method's own
// comment for exactly where the lock is (and is not) held. conflicted_ is
// touched only by Reconcile(), i.e. only ever from the registry watcher
// thread, so it needs no lock of its own.
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "ingest/registry.hpp"
#include "matter_bridge/endpoint_store.hpp"
#include "matter_bridge/uuid_hash.hpp"
#include "matter_bridge/value_mapping.hpp"

namespace matter_bridge {

// Everything Reconcile() decided needs to be pushed to Matter for one
// module_uuid this round. Every std::optional<...> channel field is empty
// unless that channel's value actually needs to change (either because it
// differs from the last value Reconcile() ever decided for this
// module_uuid, or because is_new_endpoint means nothing has ever been
// pushed at all) -- this IS this program's attribute-update dedup (see
// docs/matter-bridge.md and this repo's test list item 5): a caller that
// only calls SetMeasuredValue() when the corresponding optional is set
// never issues a redundant Matter attribute update for unchanged data.
struct ModuleUpdate {
    Uuid module_uuid{};
    uint16_t endpoint_id = 0;

    // True exactly once per module_uuid, the first time Reconcile() ever
    // emits an update for it -- the caller must create the Matter dynamic
    // endpoint (Descriptor + Bridged Device Basic Information + whichever
    // of has_*_channel below are set) before applying anything else in
    // this struct.
    bool is_new_endpoint = false;

    // This module's fixed capability set for its ENTIRE lifetime (derived
    // from topology's sensor_bitmap the first time this module_uuid was
    // ever seen) -- which clusters the endpoint has at all. Only
    // meaningful (and only ever set) when is_new_endpoint is true; a
    // module's declared sensor set never changes after first sight, same
    // as the firmware's own module_config is fixed at flash time.
    bool has_temperature_channel = false;
    bool has_humidity_channel = false;
    bool has_pm1_channel = false;
    bool has_pm25_channel = false;
    bool has_pm10_channel = false;
    bool has_illuminance_channel = false;

    // This module's bridgeable sensor_bitmap bits (illuminance | temperature
    // | humidity | pm1 | pm25 | pm10) AT THE MOMENT this endpoint was
    // created -- only meaningful (and only ever set) when is_new_endpoint is
    // true. BridgeManager persists this (see CommitApplied()) so a LATER
    // Reconcile() pass can detect a module_uuid whose topology-reported
    // sensor_bitmap no longer matches what its already-created endpoint's
    // cluster set reflects (see SensorBitmapMismatches()'s own doc comment)
    // -- a real scenario during development when the SAME physical test
    // board is reflashed/reconfigured with a different sensor while this
    // bridge process keeps running, never expected for a real deployed
    // module (see docs/matter-bridge.md's own restart-for-sensor-swap
    // section).
    uint16_t declared_sensor_bitmap = 0;

    // Present iff the Reachable attribute needs to change (or this is a
    // brand-new endpoint, which always needs an initial value).
    std::optional<bool> reachable;

    std::optional<TemperatureCentiC> temperature;
    std::optional<HumidityCentiPercent> humidity;
    std::optional<PmConcentrationUgm3> pm1;
    std::optional<PmConcentrationUgm3> pm25;
    std::optional<PmConcentrationUgm3> pm10;
    std::optional<IlluminanceMeasuredValue> illuminance;
};

// What actually happened when a caller tried to apply one ModuleUpdate to
// the real Matter SDK -- passed to CommitApplied() so this bridge's own
// change-dedup state (last_applied_) only ever advances for a field that
// genuinely reached Matter, never optimistically. Every field defaults to
// "did not happen"; a caller sets only the ones that actually succeeded
// (see chip/chip_endpoint.hpp's own ApplyOutcome-producing Apply()).
struct ApplyOutcome {
    bool endpoint_created = false;
    bool reachable_applied = false;
    bool temperature_applied = false;
    bool humidity_applied = false;
    bool pm1_applied = false;
    bool pm25_applied = false;
    bool pm10_applied = false;
    bool illuminance_applied = false;
};

class BridgeManager {
public:
    explicit BridgeManager(EndpointStore &endpoint_store);

    // Default per-uuid minimum spacing between two SAMPLE-only-triggered
    // ScheduleWork() calls for the same module_uuid -- see
    // TryBeginSchedule()'s own doc comment. 1000ms to match this pass's
    // firmware-side default (CONFIG_LINUX_BRIDGE_EXPORTER_SAMPLE_MIN_INTERVAL_MS,
    // components/linux_bridge_exporter/Kconfig) for the same reasoning
    // spelled out there, even though this side has no Kconfig of its own.
    static constexpr std::chrono::milliseconds kDefaultSampleUpdateInterval{1000};

    // Groups `snapshot` by module_uuid (a module_uuid can appear more than
    // once if it has been seen via more than one power_base_uuid -- see
    // ingest::Registry's own (power_base_uuid, module_uuid) keying),
    // detects the same-module-uuid-online-under-two-power-bases conflict
    // (see this header's own doc comment on ConflictedModules()), and
    // returns one ModuleUpdate per module_uuid that needs a Matter-facing
    // change this round, diffed against the last state CommitApplied()
    // actually confirmed (NOT mutated by this call -- see CommitApplied()'s
    // own doc comment for why this is now purely a read). A module_uuid
    // with nothing new to say (already fully reflected in Matter, not newly
    // conflicted or resolved) never appears in the returned vector at all.
    //
    // Never calls into the Matter SDK. Does call EndpointStore::get_or_assign()
    // (file I/O) for a module_uuid seen for the first time -- never while
    // holding mutex_, see this header's own top comment.
    std::vector<ModuleUpdate> Reconcile(const std::vector<ingest::NodeState> &snapshot);

    // Single-module_uuid variant of Reconcile(), same rules (including the
    // online-under-two-power-bases conflict check, which here simply
    // withholds the update -- it does NOT update ConflictedModules()'s own
    // list, which reflects only the last whole-registry Reconcile() pass).
    // Intended to be called with a FRESH snapshot taken right before
    // actually applying to Matter (see chip/main.cpp's own
    // ApplyUpdateOnMatterThread()) -- this is what makes "re-read the
    // registry's latest snapshot at the moment a scheduled work item
    // actually executes" true, rather than reusing a possibly-now-stale
    // snapshot from whenever ScheduleWork() was originally called.
    std::optional<ModuleUpdate> ReconcileOne(const std::vector<ingest::NodeState> &snapshot, const Uuid &module_uuid);

    // Commits `update`'s fields into this bridge's own last-applied dedup
    // state (last_applied_), but ONLY for the fields `outcome` reports as
    // having genuinely succeeded against the real Matter SDK. A field
    // update.X has_value() with outcome.X_applied == false (a failed
    // SetMeasuredValue()/SetReachable(), or update.is_new_endpoint with
    // outcome.endpoint_created == false) is left completely uncommitted --
    // the corresponding stored value (or, for a failed endpoint creation,
    // the ENTIRE last_applied_ entry) is simply never created/updated, so
    // the next Reconcile()/ReconcileOne() call still sees a diff against
    // the true last-known-good state and re-emits exactly that field for
    // another attempt. This is this bridge's whole retry mechanism for a
    // failed ScheduleWork()/attribute setter -- see this header's own top
    // comment on why this used to happen (unconditionally, inside
    // Reconcile() itself) and no longer does.
    void CommitApplied(const Uuid &module_uuid, const ModuleUpdate &update, const ApplyOutcome &outcome);

    // Returns true iff the caller (chip/main.cpp's registry watcher thread)
    // should proceed to call ScheduleWork() for module_uuid right now, and
    // records that a work item for it is now in flight. This is the
    // coalescing+rate-limit gate this pass adds: at most one ScheduleWork()
    // may be in flight for a given module_uuid at a time (TryBeginSchedule()
    // returns false while a previous one hasn't finished, see EndSchedule()),
    // and -- UNLESS `urgent` is true -- a non-urgent (SAMPLE-value-only)
    // update additionally waits at least `min_interval` since the last time
    // this uuid was actually scheduled. `urgent` (a brand-new endpoint or a
    // Reachable/online<->offline transition) bypasses `min_interval`
    // entirely, so topology-driven changes are never held back by the
    // SAMPLE rate limit -- they can still see the small, unavoidable
    // single-in-flight-per-uuid delay above, which exists purely to prevent
    // two concurrent Matter-thread applications for the same endpoint
    // racing each other, not to throttle anything.
    //
    // A false return is never a lost update: the caller simply does nothing
    // this round, and the NEXT Reconcile()/ReconcileOne() call (the
    // registry watcher thread's own poll loop keeps re-checking on a bound
    // well under min_interval) re-derives the diff fresh from the registry
    // and, once TryBeginSchedule() finally allows it, applies whatever is
    // CURRENT at that later moment -- never a replay of an older value.
    bool TryBeginSchedule(const Uuid &module_uuid, bool urgent,
                          std::chrono::milliseconds min_interval = kDefaultSampleUpdateInterval);

    // Must be called by the caller if TryBeginSchedule() returned true but
    // the caller's own ScheduleWork() call itself failed synchronously
    // (never actually enqueued onto the Matter event loop) -- releases the
    // in-flight gate immediately so this uuid is not left permanently
    // blocked. Called from the registry watcher thread.
    void AbortSchedule(const Uuid &module_uuid);

    // Called once a scheduled work item has actually finished running on
    // the Matter event-loop thread (regardless of whether any individual
    // field within it succeeded) -- releases the in-flight gate so a future
    // TryBeginSchedule() for this uuid can succeed again. Called from the
    // Matter event-loop thread.
    void EndSchedule(const Uuid &module_uuid);

    // module_uuids currently withheld because more than one power_base_uuid
    // reports them online AT THE SAME TIME (see docs/matter-bridge.md and
    // this repo's test list item 9) -- Reconcile() never emits a
    // ModuleUpdate for one of these while the conflict persists (the
    // endpoint, if it already exists, is simply left exactly as last
    // known-good; see this header's own top comment). Cleared automatically
    // once a later Reconcile() sees at most one online source again.
    std::vector<Uuid> ConflictedModules() const;

    // module_uuids whose CURRENT topology-reported sensor_bitmap (masked to
    // the bits this bridge acts on: illuminance/temperature/humidity/pm1/
    // pm25/pm10) no longer matches the bitmap this module's Matter endpoint
    // was actually CREATED with (ModuleUpdate::declared_sensor_bitmap at the
    // last confirmed CommitApplied() with endpoint_created==true). This can
    // only happen if the same physical module_uuid's sensor hardware is
    // reconfigured while this bridge process keeps running -- see this
    // header's own top comment on ModuleUpdate::declared_sensor_bitmap and
    // docs/matter-bridge.md's restart-for-sensor-swap section. Recomputed
    // fresh on every whole-registry Reconcile() pass (same style as
    // ConflictedModules()) from every module_uuid currently present in the
    // snapshot with a known last_applied_ entry -- unlike a conflict, this
    // does NOT self-resolve (the endpoint's actual cluster set never changes
    // without a process restart), so a module_uuid here stays here for the
    // remainder of this process's lifetime once its sensor genuinely
    // changes. While a module_uuid is in this list, Reconcile()/
    // ReconcileOne() never emit a value for a channel type that is not part
    // of its ORIGINAL declared_sensor_bitmap (see ComputeUpdateForModule()'s
    // own top comment) -- this bridge never silently keeps trying (and
    // silently failing) to push a reading onto a cluster its endpoint was
    // never actually given, and never fabricates a new cluster for it
    // in-place either (this bridge's fixed-sensor-module-identity policy for
    // a real deployed module is unchanged -- see docs/matter-bridge.md).
    std::vector<Uuid> SensorBitmapMismatches() const;

private:
    struct LastApplied {
        bool has_reachable = false;
        bool reachable = false;
        bool has_temperature = false;
        TemperatureCentiC temperature;
        bool has_humidity = false;
        HumidityCentiPercent humidity;
        bool has_pm1 = false;
        PmConcentrationUgm3 pm1;
        bool has_pm25 = false;
        PmConcentrationUgm3 pm25;
        bool has_pm10 = false;
        PmConcentrationUgm3 pm10;
        bool has_illuminance = false;
        IlluminanceMeasuredValue illuminance;
        // The sensor_bitmap (masked) this module's endpoint was actually
        // created with -- see ModuleUpdate::declared_sensor_bitmap's own doc
        // comment. Set once, when this LastApplied entry is first created
        // (endpoint_created confirmed), never updated afterward -- this IS
        // the "original declaration" SensorBitmapMismatches() compares
        // against.
        uint16_t declared_sensor_bitmap = 0;
    };

    struct ScheduleState {
        bool in_flight = false;
        bool has_scheduled = false;
        std::chrono::steady_clock::time_point last_scheduled{};
    };

    // Shared by Reconcile()/ReconcileOne() -- computes what (if anything)
    // needs to change for one module_uuid's `entries` (all NodeState
    // entries in the snapshot for that uuid, i.e. one per power_base_uuid
    // that has ever reported it), against the CURRENT last_applied_ state,
    // WITHOUT mutating last_applied_ (see this header's own top comment --
    // that now only ever happens in CommitApplied()). Sets *out_conflicted
    // and returns std::nullopt if more than one entry is online at once.
    // Sets *out_mismatched (independent of *out_conflicted) if this is an
    // EXISTING module_uuid whose current sensor_bitmap no longer matches
    // last_applied_'s own declared_sensor_bitmap -- see
    // SensorBitmapMismatches()'s own doc comment; a mismatched module still
    // gets a normal (possibly empty) update for its ORIGINALLY-declared
    // channels, it simply never gains a value for a channel type outside
    // that original declaration.
    std::optional<ModuleUpdate> ComputeUpdateForModule(const Uuid &module_uuid,
                                                         const std::vector<const ingest::NodeState *> &entries,
                                                         bool *out_conflicted, bool *out_mismatched);

    EndpointStore &endpoint_store_;
    mutable std::mutex mutex_;
    std::unordered_map<Uuid, LastApplied, UuidHash> last_applied_;
    std::unordered_map<Uuid, ScheduleState, UuidHash> schedule_state_;
    std::vector<Uuid> conflicted_;
    std::vector<Uuid> sensor_bitmap_mismatches_;
};

} // namespace matter_bridge
