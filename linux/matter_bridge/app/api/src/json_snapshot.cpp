#include "pogohome_api/json_snapshot.hpp"

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

std::string BuildSnapshotJson(const std::vector<ingest::NodeState> &nodes, uint64_t registry_generation,
                               int64_t generated_at_unix_ms)
{
    std::string out;
    out += "{\"schema_version\":";
    out += std::to_string(kSchemaVersion);
    out += ",\"generated_at_unix_ms\":";
    out += std::to_string(generated_at_unix_ms);
    out += ",\"registry_generation\":";
    out += std::to_string(registry_generation);
    out += ",\"modules\":[";
    bool first = true;
    for (const auto &n : nodes) {
        if (!n.has_topology) {
            continue; // see this file's own header comment
        }
        if (!first) {
            out += ',';
        }
        first = false;
        AppendModuleJson(out, n);
    }
    out += "]}";
    return out;
}

} // namespace pogohome_api
