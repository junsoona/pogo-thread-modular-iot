#include "matter_bridge/bridge_manager.hpp"

#include <unordered_map>

extern "C" {
#include "sensor_channel/sensor_channel.h"
}

namespace matter_bridge {

BridgeManager::BridgeManager(EndpointStore &endpoint_store) : endpoint_store_(endpoint_store) {}

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
        const Uuid &module_uuid = kv.first;
        const std::vector<const ingest::NodeState *> &entries = kv.second;

        std::vector<const ingest::NodeState *> online_entries;
        for (const auto *n : entries) {
            if (n->online) {
                online_entries.push_back(n);
            }
        }

        // Registry keys every node by (power_base_uuid, module_uuid), so
        // more than one entry here for the SAME module_uuid can only come
        // from DIFFERENT power_base_uuids -- more than one of them online
        // at once is exactly the conflict docs/matter-bridge.md and this
        // repo's test list item 9 describe. Per that spec, this module's
        // update is withheld entirely (not "pick one arbitrarily") until
        // it resolves back down to at most one online source; whatever was
        // last applied to Matter for it is left exactly as-is.
        if (online_entries.size() > 1) {
            still_conflicted.push_back(module_uuid);
            continue;
        }

        const ingest::NodeState *effective = online_entries.empty() ? entries.front() : online_entries.front();
        bool reachable = !online_entries.empty();

        bool is_new = last_applied_.find(module_uuid) == last_applied_.end();

        if (is_new) {
            // BH1750/SGP40/SGP41-only modules never get a Matter endpoint
            // in this MVP (docs/matter-bridge.md section 3.1 and this
            // repo's own scope) -- a module whose declared sensor_bitmap
            // has none of the five bridged channel bits set is skipped
            // BEFORE ever consuming an EndpointStore slot, so it can never
            // exhaust the bounded endpoint id space either. A module's
            // sensor_bitmap never changes after its first sighting (same
            // as module_config's own fixed-at-flash-time bitmap), so this
            // check only ever needs to run once per module_uuid, right here.
            bool bridgeable = (effective->sensor_bitmap & (1u << SENSOR_CHANNEL_TEMPERATURE)) != 0 ||
                               (effective->sensor_bitmap & (1u << SENSOR_CHANNEL_HUMIDITY)) != 0 ||
                               (effective->sensor_bitmap & (1u << SENSOR_CHANNEL_PM1_0)) != 0 ||
                               (effective->sensor_bitmap & (1u << SENSOR_CHANNEL_PM2_5)) != 0 ||
                               (effective->sensor_bitmap & (1u << SENSOR_CHANNEL_PM10)) != 0;
            if (!bridgeable) {
                continue;
            }
        }

        std::optional<uint16_t> ep_id = endpoint_store_.get_or_assign(module_uuid);
        if (!ep_id.has_value()) {
            // The persistent store is full or a write failed -- see
            // EndpointStore::get_or_assign()'s own doc comment: this
            // module_uuid is skipped entirely this round rather than ever
            // creating a Matter endpoint whose ID isn't durably recorded.
            continue;
        }

        ModuleUpdate upd;
        upd.module_uuid = module_uuid;
        upd.endpoint_id = *ep_id;
        upd.is_new_endpoint = is_new;

        LastApplied &la = last_applied_[module_uuid];

        if (is_new) {
            upd.has_temperature_channel = (effective->sensor_bitmap & (1u << SENSOR_CHANNEL_TEMPERATURE)) != 0;
            upd.has_humidity_channel = (effective->sensor_bitmap & (1u << SENSOR_CHANNEL_HUMIDITY)) != 0;
            upd.has_pm1_channel = (effective->sensor_bitmap & (1u << SENSOR_CHANNEL_PM1_0)) != 0;
            upd.has_pm25_channel = (effective->sensor_bitmap & (1u << SENSOR_CHANNEL_PM2_5)) != 0;
            upd.has_pm10_channel = (effective->sensor_bitmap & (1u << SENSOR_CHANNEL_PM10)) != 0;
        }

        if (is_new || !la.has_reachable || la.reachable != reachable) {
            upd.reachable = reachable;
            la.has_reachable = true;
            la.reachable = reachable;
        }

        if (reachable && effective->has_sample) {
            for (const auto &ch : effective->last_channels) {
                switch (ch.type) {
                case SENSOR_CHANNEL_TEMPERATURE: {
                    TemperatureCentiC v = ConvertTemperature(ch);
                    if (is_new || !la.has_temperature || la.temperature != v) {
                        upd.temperature = v;
                        la.has_temperature = true;
                        la.temperature = v;
                    }
                    break;
                }
                case SENSOR_CHANNEL_HUMIDITY: {
                    HumidityCentiPercent v = ConvertHumidity(ch);
                    if (is_new || !la.has_humidity || la.humidity != v) {
                        upd.humidity = v;
                        la.has_humidity = true;
                        la.humidity = v;
                    }
                    break;
                }
                case SENSOR_CHANNEL_PM1_0: {
                    PmConcentrationUgm3 v = ConvertPm(ch);
                    if (is_new || !la.has_pm1 || la.pm1 != v) {
                        upd.pm1 = v;
                        la.has_pm1 = true;
                        la.pm1 = v;
                    }
                    break;
                }
                case SENSOR_CHANNEL_PM2_5: {
                    PmConcentrationUgm3 v = ConvertPm(ch);
                    if (is_new || !la.has_pm25 || la.pm25 != v) {
                        upd.pm25 = v;
                        la.has_pm25 = true;
                        la.pm25 = v;
                    }
                    break;
                }
                case SENSOR_CHANNEL_PM10: {
                    PmConcentrationUgm3 v = ConvertPm(ch);
                    if (is_new || !la.has_pm10 || la.pm10 != v) {
                        upd.pm10 = v;
                        la.has_pm10 = true;
                        la.pm10 = v;
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
            updates.push_back(std::move(upd));
        }
    }

    conflicted_ = std::move(still_conflicted);
    return updates;
}

std::vector<Uuid> BridgeManager::ConflictedModules() const
{
    return conflicted_;
}

} // namespace matter_bridge
