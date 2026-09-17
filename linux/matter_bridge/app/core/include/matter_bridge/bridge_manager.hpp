// Turns an ingest::Registry snapshot into the minimal set of Matter-facing
// changes that actually need to happen -- endpoint creation (once, ever,
// per module_uuid), Reachable transitions, and attribute value pushes --
// without ever touching the Matter SDK itself (see value_mapping.hpp's own
// top comment for why linux/matter_bridge/app/core/ has zero
// connectedhomeip dependency).
//
// Threading contract: exactly one thread (the "registry watcher" thread --
// see linux/matter_bridge/app/chip/main.cpp) owns a BridgeManager instance
// and calls Reconcile() on it, always right after taking a fresh
// ingest::Registry::snapshot() under that registry's own mutex (never
// while holding it, see registry.hpp's own top comment). BridgeManager
// itself does no locking because it is never touched from more than one
// thread.
#pragma once

#include <cstdint>
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

    // Present iff the Reachable attribute needs to change (or this is a
    // brand-new endpoint, which always needs an initial value).
    std::optional<bool> reachable;

    std::optional<TemperatureCentiC> temperature;
    std::optional<HumidityCentiPercent> humidity;
    std::optional<PmConcentrationUgm3> pm1;
    std::optional<PmConcentrationUgm3> pm25;
    std::optional<PmConcentrationUgm3> pm10;
};

class BridgeManager {
public:
    explicit BridgeManager(EndpointStore &endpoint_store);

    // Groups `snapshot` by module_uuid (a module_uuid can appear more than
    // once if it has been seen via more than one power_base_uuid -- see
    // ingest::Registry's own (power_base_uuid, module_uuid) keying),
    // detects the same-module-uuid-online-under-two-power-bases conflict
    // (see this header's own doc comment on ConflictedModules()), and
    // returns one ModuleUpdate per module_uuid that needs a Matter-facing
    // change this round. A module_uuid with nothing new to say (already
    // fully reflected in Matter, not newly conflicted or resolved) never
    // appears in the returned vector at all.
    //
    // Never calls into the Matter SDK, never does file I/O directly (only
    // EndpointStore::get_or_assign() does, and only for a module_uuid seen
    // for the first time).
    std::vector<ModuleUpdate> Reconcile(const std::vector<ingest::NodeState> &snapshot);

    // module_uuids currently withheld because more than one power_base_uuid
    // reports them online AT THE SAME TIME (see docs/matter-bridge.md and
    // this repo's test list item 9) -- Reconcile() never emits a
    // ModuleUpdate for one of these while the conflict persists (the
    // endpoint, if it already exists, is simply left exactly as last
    // known-good; see this header's own top comment). Cleared automatically
    // once a later Reconcile() sees at most one online source again.
    std::vector<Uuid> ConflictedModules() const;

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
    };

    EndpointStore &endpoint_store_;
    std::unordered_map<Uuid, LastApplied, UuidHash> last_applied_;
    std::vector<Uuid> conflicted_;
};

} // namespace matter_bridge
