#include "matter_bridge/bridge_manager.hpp"

#include <unordered_map>

extern "C" {
#include "sensor_channel/sensor_channel.h"
}

namespace matter_bridge {

BridgeManager::BridgeManager(EndpointStore &endpoint_store) : endpoint_store_(endpoint_store) {}

std::optional<ModuleUpdate> BridgeManager::ComputeUpdateForModule(const Uuid &module_uuid,
                                                                    const std::vector<const ingest::NodeState *> &entries,
                                                                    bool *out_conflicted)
{
    if (out_conflicted != nullptr) {
        *out_conflicted = false;
    }

    std::vector<const ingest::NodeState *> online_entries;
    for (const auto *n : entries) {
        if (n->online) {
            online_entries.push_back(n);
        }
    }

    // Registry keys every node by (power_base_uuid, module_uuid), so more
    // than one entry here for the SAME module_uuid can only come from
    // DIFFERENT power_base_uuids -- more than one of them online at once is
    // exactly the conflict docs/matter-bridge.md and this repo's test list
    // item 9 describe. Per that spec, this module's update is withheld
    // entirely (not "pick one arbitrarily") until it resolves back down to
    // at most one online source; whatever was last applied to Matter for it
    // is left exactly as-is.
    if (online_entries.size() > 1) {
        if (out_conflicted != nullptr) {
            *out_conflicted = true;
        }
        return std::nullopt;
    }

    const ingest::NodeState *effective = online_entries.empty() ? entries.front() : online_entries.front();
    bool reachable = !online_entries.empty();

    // Peek is_new WITHOUT holding mutex_ across the (file-I/O-performing)
    // get_or_assign() call below -- mutex_ is never held across file I/O or
    // a Matter SDK call, see this class's own header comment. A narrow
    // TOCTOU where this peek goes stale before the authoritative re-check
    // below is harmless: get_or_assign() is idempotent (the same
    // module_uuid always yields the same endpoint id no matter how many
    // times or from how many vantage points it's called), and the real
    // decision uses the re-checked value taken under the lock right before
    // ModuleUpdate is built.
    bool peeked_is_new;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        peeked_is_new = (last_applied_.find(module_uuid) == last_applied_.end());
    }

    if (peeked_is_new) {
        // BH1750/SGP40/SGP41-only modules never get a Matter endpoint in
        // this MVP (docs/matter-bridge.md section 3.1 and this repo's own
        // scope) -- a module whose declared sensor_bitmap has none of the
        // five bridged channel bits set is skipped BEFORE ever consuming an
        // EndpointStore slot, so it can never exhaust the bounded endpoint
        // id space either. A module's sensor_bitmap never changes after its
        // first sighting (same as module_config's own fixed-at-flash-time
        // bitmap), so this check only ever needs to run once per
        // module_uuid, right here.
        bool bridgeable = (effective->sensor_bitmap & (1u << SENSOR_CHANNEL_TEMPERATURE)) != 0 ||
                           (effective->sensor_bitmap & (1u << SENSOR_CHANNEL_HUMIDITY)) != 0 ||
                           (effective->sensor_bitmap & (1u << SENSOR_CHANNEL_PM1_0)) != 0 ||
                           (effective->sensor_bitmap & (1u << SENSOR_CHANNEL_PM2_5)) != 0 ||
                           (effective->sensor_bitmap & (1u << SENSOR_CHANNEL_PM10)) != 0;
        if (!bridgeable) {
            return std::nullopt;
        }
    }

    std::optional<uint16_t> ep_id = endpoint_store_.get_or_assign(module_uuid);
    if (!ep_id.has_value()) {
        // The persistent store is full or a write failed -- see
        // EndpointStore::get_or_assign()'s own doc comment: this
        // module_uuid is skipped entirely this round rather than ever
        // creating a Matter endpoint whose ID isn't durably recorded.
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = last_applied_.find(module_uuid);
    bool is_new = (it == last_applied_.end());
    static const LastApplied kEmpty{};
    const LastApplied &la = is_new ? kEmpty : it->second;

    ModuleUpdate upd;
    upd.module_uuid = module_uuid;
    upd.endpoint_id = *ep_id;
    upd.is_new_endpoint = is_new;

    if (is_new) {
        upd.has_temperature_channel = (effective->sensor_bitmap & (1u << SENSOR_CHANNEL_TEMPERATURE)) != 0;
        upd.has_humidity_channel = (effective->sensor_bitmap & (1u << SENSOR_CHANNEL_HUMIDITY)) != 0;
        upd.has_pm1_channel = (effective->sensor_bitmap & (1u << SENSOR_CHANNEL_PM1_0)) != 0;
        upd.has_pm25_channel = (effective->sensor_bitmap & (1u << SENSOR_CHANNEL_PM2_5)) != 0;
        upd.has_pm10_channel = (effective->sensor_bitmap & (1u << SENSOR_CHANNEL_PM10)) != 0;
    }

    if (is_new || !la.has_reachable || la.reachable != reachable) {
        upd.reachable = reachable;
    }

    if (reachable && effective->has_sample) {
        for (const auto &ch : effective->last_channels) {
            switch (ch.type) {
            case SENSOR_CHANNEL_TEMPERATURE: {
                TemperatureCentiC v = ConvertTemperature(ch);
                if (is_new || !la.has_temperature || la.temperature != v) {
                    upd.temperature = v;
                }
                break;
            }
            case SENSOR_CHANNEL_HUMIDITY: {
                HumidityCentiPercent v = ConvertHumidity(ch);
                if (is_new || !la.has_humidity || la.humidity != v) {
                    upd.humidity = v;
                }
                break;
            }
            case SENSOR_CHANNEL_PM1_0: {
                PmConcentrationUgm3 v = ConvertPm(ch);
                if (is_new || !la.has_pm1 || la.pm1 != v) {
                    upd.pm1 = v;
                }
                break;
            }
            case SENSOR_CHANNEL_PM2_5: {
                PmConcentrationUgm3 v = ConvertPm(ch);
                if (is_new || !la.has_pm25 || la.pm25 != v) {
                    upd.pm25 = v;
                }
                break;
            }
            case SENSOR_CHANNEL_PM10: {
                PmConcentrationUgm3 v = ConvertPm(ch);
                if (is_new || !la.has_pm10 || la.pm10 != v) {
                    upd.pm10 = v;
                }
                break;
            }
            default:
                break; // BH1750/SGP40/SGP41 channels -- not bridged in this MVP, see docs/matter-bridge.md
            }
        }
    }

    bool has_any_field = upd.reachable.has_value() || upd.temperature.has_value() || upd.humidity.has_value() ||
                          upd.pm1.has_value() || upd.pm25.has_value() || upd.pm10.has_value();
    if (is_new || has_any_field) {
        return upd;
    }
    return std::nullopt;
}

std::vector<ModuleUpdate> BridgeManager::Reconcile(const std::vector<ingest::NodeState> &snapshot)
{
    std::unordered_map<Uuid, std::vector<const ingest::NodeState *>, UuidHash> by_module;
    for (const auto &n : snapshot) {
        if (!n.has_topology) {
            continue; // nothing to bridge yet -- see registry.hpp: has_topology is the first-sighting marker
        }
        by_module[n.module_uuid].push_back(&n);
    }

    std::vector<ModuleUpdate> updates;
    std::vector<Uuid> still_conflicted;

    for (const auto &kv : by_module) {
        bool conflicted = false;
        std::optional<ModuleUpdate> upd = ComputeUpdateForModule(kv.first, kv.second, &conflicted);
        if (conflicted) {
            still_conflicted.push_back(kv.first);
            continue;
        }
        if (upd.has_value()) {
            updates.push_back(std::move(*upd));
        }
    }

    conflicted_ = std::move(still_conflicted);
    return updates;
}

std::optional<ModuleUpdate> BridgeManager::ReconcileOne(const std::vector<ingest::NodeState> &snapshot,
                                                          const Uuid &module_uuid)
{
    std::vector<const ingest::NodeState *> entries;
    for (const auto &n : snapshot) {
        if (n.has_topology && n.module_uuid == module_uuid) {
            entries.push_back(&n);
        }
    }
    if (entries.empty()) {
        return std::nullopt; // no longer (or never) present in this snapshot -- nothing to apply
    }
    bool conflicted = false;
    std::optional<ModuleUpdate> upd = ComputeUpdateForModule(module_uuid, entries, &conflicted);
    // Deliberately does NOT touch conflicted_ here -- see this method's own
    // header doc comment: that list reflects only the last whole-registry
    // Reconcile() pass. A conflicted module_uuid simply yields no update.
    if (conflicted) {
        return std::nullopt;
    }
    return upd;
}

void BridgeManager::CommitApplied(const Uuid &module_uuid, const ModuleUpdate &update, const ApplyOutcome &outcome)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = last_applied_.find(module_uuid);
    if (it == last_applied_.end()) {
        if (!(update.is_new_endpoint && outcome.endpoint_created)) {
            // Endpoint creation was never confirmed successful -- create NO
            // entry at all, so the next Reconcile()/ReconcileOne() still
            // sees is_new==true and retries full creation from scratch.
            // See this class's own header comment on why this replaces the
            // old unconditional-eager-write behavior.
            return;
        }
        it = last_applied_.emplace(module_uuid, LastApplied{}).first;
    }
    LastApplied &la = it->second;

    if (outcome.reachable_applied && update.reachable.has_value()) {
        la.has_reachable = true;
        la.reachable = *update.reachable;
    }
    if (outcome.temperature_applied && update.temperature.has_value()) {
        la.has_temperature = true;
        la.temperature = *update.temperature;
    }
    if (outcome.humidity_applied && update.humidity.has_value()) {
        la.has_humidity = true;
        la.humidity = *update.humidity;
    }
    if (outcome.pm1_applied && update.pm1.has_value()) {
        la.has_pm1 = true;
        la.pm1 = *update.pm1;
    }
    if (outcome.pm25_applied && update.pm25.has_value()) {
        la.has_pm25 = true;
        la.pm25 = *update.pm25;
    }
    if (outcome.pm10_applied && update.pm10.has_value()) {
        la.has_pm10 = true;
        la.pm10 = *update.pm10;
    }
}

bool BridgeManager::TryBeginSchedule(const Uuid &module_uuid, bool urgent, std::chrono::milliseconds min_interval)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ScheduleState &st = schedule_state_[module_uuid];
    if (st.in_flight) {
        return false; // at most one pending Matter work item per uuid -- see this method's own doc comment
    }
    if (!urgent && st.has_scheduled) {
        auto now = std::chrono::steady_clock::now();
        if (now - st.last_scheduled < min_interval) {
            return false; // still inside this uuid's own SAMPLE rate-limit window
        }
    }
    st.in_flight = true;
    st.has_scheduled = true;
    st.last_scheduled = std::chrono::steady_clock::now();
    return true;
}

void BridgeManager::EndSchedule(const Uuid &module_uuid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = schedule_state_.find(module_uuid);
    if (it != schedule_state_.end()) {
        it->second.in_flight = false;
    }
}

void BridgeManager::AbortSchedule(const Uuid &module_uuid)
{
    // Same release as EndSchedule() -- kept as a separate method purely so
    // call sites document WHY the gate is being released (a synchronous
    // ScheduleWork() failure vs. a normal work-item completion); see this
    // class's own header comment for each one's contract.
    EndSchedule(module_uuid);
}

std::vector<Uuid> BridgeManager::ConflictedModules() const
{
    return conflicted_;
}

} // namespace matter_bridge
