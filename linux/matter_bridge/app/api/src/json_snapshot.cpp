#include "pogohome_api/json_snapshot.hpp"

#include <algorithm>
#include <cstdio>

#include "pogohome_api/hex.hpp"
#include "pogohome_api/sensor_kind.hpp"

namespace pogohome_api {

namespace {

// Appends `s` to `out` as a JSON string literal (including the surrounding
// quotes), escaping exactly what RFC 8259 requires (quote, backslash, and
// every control character < 0x20 -- the common named escapes plus \u00XX
// for anything else in that range) -- every string value this API ever
// emits (uuid hex, kind/status/role names) is plain ASCII today, but this
// stays fully correct for any future free-text field (e.g. a later
// milestone's module/room name) rather than assuming that forever.
void AppendJsonString(std::string &out, const char *s)
{
    out += '"';
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s); *p != '\0'; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    out += '"';
}

void AppendJsonString(std::string &out, const std::string &s)
{
    AppendJsonString(out, s.c_str());
}

void AppendReadingJson(std::string &out, const ingest::ChannelReading &ch, uint32_t sample_seq)
{
    const SensorKindInfo *kind_info = KindInfoForChannelType(ch.type);
    if (kind_info == nullptr) {
        // Not one of the 10 known channel types -- see this file's own
        // header comment: should never happen for a channel that reached
        // ingest::Registry via a real sensor_channel_unpack(), skipped
        // rather than emitted with a fabricated kind string.
        return;
    }
    const char *status_name = ChannelStatusName(ch.status);

    out += '{';
    out += "\"kind\":";
    AppendJsonString(out, kind_info->kind);
    out += ",\"raw_value\":";
    out += std::to_string(ch.value);
    out += ",\"scale\":";
    out += std::to_string(static_cast<unsigned>(ch.scale));
    out += ",\"value\":";
    out += FormatScaledValue(ch.value, ch.scale);
    out += ",\"unit\":";
    if (kind_info->unit != nullptr) {
        AppendJsonString(out, kind_info->unit);
    } else {
        out += "null";
    }
    out += ",\"status\":";
    if (status_name != nullptr) {
        AppendJsonString(out, status_name);
    } else {
        out += "null";
    }
    out += ",\"sample_seq\":";
    out += std::to_string(sample_seq);
    out += '}';
}

void AppendModuleJson(std::string &out, const ingest::NodeState &n)
{
    out += '{';
    out += "\"uuid\":";
    AppendJsonString(out, FormatUuidHex(n.module_uuid));
    out += ",\"power_base_uuid\":";
    AppendJsonString(out, FormatUuidHex(n.power_base_uuid));
    out += ",\"parent_uuid\":";
    AppendJsonString(out, FormatUuidHex(n.parent_uuid));
    out += ",\"online\":";
    out += n.online ? "true" : "false";
    out += ",\"depth\":";
    out += std::to_string(static_cast<unsigned>(n.depth));
    out += ",\"role\":";
    const char *role_name = TopologyRoleName(n.role);
    if (role_name != nullptr) {
        AppendJsonString(out, role_name);
    } else {
        out += "null";
    }
    out += ",\"sensor_bitmap\":";
    out += std::to_string(static_cast<unsigned>(n.sensor_bitmap));
    out += ",\"generation\":";
    out += std::to_string(static_cast<unsigned long long>(n.boot_session_generation));
    out += ",\"last_seq\":";
    out += std::to_string(static_cast<unsigned long long>(n.last_seq));
    out += ",\"readings\":[";
    if (n.has_sample) {
        bool first = true;
        for (const auto &ch : n.last_channels) {
            if (KindInfoForChannelType(ch.type) == nullptr) {
                continue; // see AppendReadingJson's own skip note
            }
            if (!first) {
                out += ',';
            }
            first = false;
            AppendReadingJson(out, ch, n.last_sample_seq);
        }
    }
    out += "]}";
}

} // namespace

std::string BuildHealthJson(uint64_t registry_generation, size_t module_count)
{
    std::string out;
    out += "{\"schema_version\":";
    out += std::to_string(kSchemaVersion);
    out += ",\"service\":\"pogohome\",\"registry_generation\":";
    out += std::to_string(registry_generation);
    out += ",\"module_count\":";
    out += std::to_string(module_count);
    out += '}';
    return out;
}

std::string BuildHealthJson(uint64_t registry_generation, size_t module_count, size_t power_base_count)
{
    std::string out = BuildHealthJson(registry_generation, module_count);
    out.pop_back(); // the closing '}'
    out += ",\"power_base_count\":";
    out += std::to_string(power_base_count);
    out += '}';
    return out;
}

std::string BuildHealthJson(uint64_t registry_generation, size_t module_count, size_t power_base_count,
                            const std::string &hub_id)
{
    std::string out = BuildHealthJson(registry_generation, module_count, power_base_count);
    if (hub_id.empty()) {
        return out;
    }
    out.pop_back(); // the closing '}'
    out += ",\"hub_id\":";
    AppendJsonString(out, hub_id);
    out += '}';
    return out;
}

std::vector<ingest::NodeState> ResolveModuleOwners(const std::vector<ingest::NodeState> &nodes)
{
    std::vector<ingest::NodeState> out;
    out.reserve(nodes.size());
    for (const auto &n : nodes) {
        if (!n.has_topology) {
            continue; // see this file's own header comment
        }
        auto existing = std::find_if(out.begin(), out.end(), [&n](const ingest::NodeState &o) {
            return o.module_uuid == n.module_uuid;
        });
        if (existing == out.end()) {
            out.push_back(n);
            continue;
        }
        bool better = (n.online != existing->online) ? n.online : n.attach_order > existing->attach_order;
        if (better) {
            *existing = n;
        }
    }
    return out;
}

std::string BuildSnapshotJson(const std::vector<ingest::NodeState> &nodes, uint64_t registry_generation,
                               int64_t generated_at_unix_ms)
{
    ingest::RegistryView view;
    view.nodes = nodes;
    return BuildSnapshotJson(view, registry_generation, generated_at_unix_ms);
}

std::string BuildSnapshotJson(const ingest::RegistryView &view, uint64_t registry_generation,
                               int64_t generated_at_unix_ms)
{
    return BuildSnapshotJson(view, registry_generation, generated_at_unix_ms, nullptr);
}

std::string BuildSnapshotJson(const ingest::RegistryView &view, uint64_t registry_generation,
                               int64_t generated_at_unix_ms, const SnapshotManagement *management)
{
    const std::vector<ingest::NodeState> modules = ResolveModuleOwners(view.nodes);

    struct BaseEntry {
        ingest::Uuid uuid;
        bool online;
        bool tracked;
        unsigned module_count = 0;
        unsigned online_module_count = 0;
    };
    std::vector<BaseEntry> bases;
    for (const auto &b : view.power_bases) {
        bases.push_back(BaseEntry{b.power_base_uuid, b.online, true});
    }
    for (const auto &m : modules) {
        auto it = std::find_if(bases.begin(), bases.end(),
                               [&m](const BaseEntry &b) { return b.uuid == m.power_base_uuid; });
        if (it == bases.end()) {
            bases.push_back(BaseEntry{m.power_base_uuid, false, false});
            it = bases.end() - 1;
        }
        it->module_count++;
        if (m.online) {
            it->online_module_count++;
            if (!it->tracked) {
                it->online = true;
            }
        }
    }
    // A managed Base that has not been heard from since the bridge started
    // is still this hub's to unenroll: listed offline, without modules.
    std::vector<std::string> base_hex;
    for (const auto &b : bases) {
        base_hex.push_back(FormatUuidHex(b.uuid));
    }
    std::vector<std::string> silent_managed;
    if (management != nullptr) {
        for (const std::string &managed : management->managed_power_bases) {
            if (std::find(base_hex.begin(), base_hex.end(), managed) == base_hex.end() &&
                std::find(silent_managed.begin(), silent_managed.end(), managed) == silent_managed.end()) {
                silent_managed.push_back(managed);
            }
        }
    }
    auto is_managed = [&](const std::string &hex) {
        return management != nullptr && std::find(management->managed_power_bases.begin(),
                                                  management->managed_power_bases.end(),
                                                  hex) != management->managed_power_bases.end();
    };
    // What a managed Base's entry says of management.
    auto managed_fields = [&](const std::string &hex) {
        const bool self_confirming = std::find(management->self_confirming_power_bases.begin(),
                                               management->self_confirming_power_bases.end(),
                                               hex) != management->self_confirming_power_bases.end();
        return std::string(",\"management\":\"supported\",\"erase_confirmation\":") +
               (self_confirming ? "\"automatic\"" : "\"manual\"");
    };

    // The registration a request to take this Base out of the list names.
    auto record_field = [&](const std::string &hex) {
        if (management == nullptr || !management->forget_supported) {
            return std::string();
        }
        for (const auto &entry : management->records) {
            if (entry.first == hex) {
                std::string field = ",\"record\":";
                AppendJsonString(field, entry.second);
                return field;
            }
        }
        return std::string();
    };
    // What a request for this Base's removal names besides the registration.
    auto removal_serial_field = [&](const std::string &hex) {
        if (management == nullptr) {
            return std::string();
        }
        for (const auto &entry : management->removal_serials) {
            if (entry.first == hex) {
                return std::string(",\"removal_serial\":") + std::to_string(entry.second);
            }
        }
        return std::string();
    };

    std::string out;
    out += "{\"schema_version\":";
    out += std::to_string(kSchemaVersion);
    out += ",\"generated_at_unix_ms\":";
    out += std::to_string(generated_at_unix_ms);
    out += ",\"registry_generation\":";
    out += std::to_string(registry_generation);
    out += ",\"power_bases\":[";
    for (size_t i = 0; i < bases.size(); i++) {
        if (i != 0) {
            out += ',';
        }
        out += "{\"uuid\":";
        AppendJsonString(out, base_hex[i]);
        out += ",\"online\":";
        out += bases[i].online ? "true" : "false";
        out += ",\"module_count\":";
        out += std::to_string(bases[i].module_count);
        out += ",\"online_module_count\":";
        out += std::to_string(bases[i].online_module_count);
        if (management != nullptr) {
            out += is_managed(base_hex[i]) ? managed_fields(base_hex[i]) : std::string(",\"management\":\"unsupported\"");
            out += record_field(base_hex[i]);
            if (is_managed(base_hex[i])) {
                out += removal_serial_field(base_hex[i]);
            }
        }
        out += '}';
    }
    for (size_t i = 0; i < silent_managed.size(); i++) {
        if (i != 0 || !bases.empty()) {
            out += ',';
        }
        out += "{\"uuid\":";
        AppendJsonString(out, silent_managed[i]);
        out += ",\"online\":false,\"module_count\":0,\"online_module_count\":0";
        out += managed_fields(silent_managed[i]);
        out += record_field(silent_managed[i]);
        out += removal_serial_field(silent_managed[i]);
        out += '}';
    }
    out += "],\"modules\":[";
    bool first = true;
    for (const auto &n : modules) {
        if (!first) {
            out += ',';
        }
        first = false;
        AppendModuleJson(out, n);
    }
    out += ']';
    if (management != nullptr) {
        out += ",\"power_base_removals\":";
        out += management->removals_json.empty() ? std::string("[]") : management->removals_json;
        if (management->forget_supported) {
            out += ",\"power_base_forgets\":";
            out += management->forgets_json.empty() ? std::string("[]") : management->forgets_json;
            out += ",\"power_base_departures\":";
            out += management->departures_json.empty() ? std::string("[]") : management->departures_json;
        }
    }
    out += '}';
    return out;
}

} // namespace pogohome_api
