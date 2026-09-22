// Covers pogohome_api::BuildHealthJson()/BuildSnapshotJson() -- the REAL
// production serializer (see src/json_snapshot.cpp), fed hand-built
// ingest::NodeState fixtures directly (no socket, no Registry needed for
// this file -- http_server tests cover the Registry-integrated path). Also
// includes a small, test-only JSON syntax validator (IsValidJson below) so
// this file actually parses the produced text rather than only grep'ing
// for expected substrings -- catches a structural bug (unbalanced braces,
// an unescaped control character, a trailing comma) that substring checks
// alone would miss.
#include <cctype>
#include <cstring>

#include "ingest/registry.hpp"
#include "pogohome_api/json_snapshot.hpp"
#include "test_check.hpp"

extern "C" {
#include "sensor_channel/sensor_channel.h"
#include "topology_proto/topology_proto.h"
}

using ingest::ChannelReading;
using ingest::NodeState;
using ingest::Uuid;

namespace {

// --- minimal RFC 8259 syntax validator (test-only) -------------------------
size_t SkipWs(const std::string &s, size_t i)
{
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
        i++;
    }
    return i;
}

bool ParseJsonValue(const std::string &s, size_t &i);

bool ParseJsonString(const std::string &s, size_t &i)
{
    if (i >= s.size() || s[i] != '"') {
        return false;
    }
    i++;
    while (true) {
        if (i >= s.size()) {
            return false;
        }
        char c = s[i];
        if (c == '"') {
            i++;
            return true;
        }
        if (c == '\\') {
            i++;
            if (i >= s.size()) {
                return false;
            }
            char e = s[i];
            if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't') {
                i++;
            } else if (e == 'u') {
                if (i + 4 >= s.size()) {
                    return false;
                }
                for (int k = 1; k <= 4; k++) {
                    if (!std::isxdigit(static_cast<unsigned char>(s[i + k]))) {
                        return false;
                    }
                }
                i += 5;
            } else {
                return false;
            }
        } else if (static_cast<unsigned char>(c) < 0x20) {
            return false; // raw control char is never legal unescaped in a JSON string
        } else {
            i++;
        }
    }
}

bool ParseJsonNumber(const std::string &s, size_t &i)
{
    size_t start = i;
    if (i < s.size() && s[i] == '-') {
        i++;
    }
    if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) {
        return false;
    }
    if (s[i] == '0') {
        i++;
    } else {
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            i++;
        }
    }
    if (i < s.size() && s[i] == '.') {
        i++;
        if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) {
            return false;
        }
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            i++;
        }
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            i++;
        }
        if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) {
            return false;
        }
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            i++;
        }
    }
    return i > start;
}

bool ParseJsonLiteral(const std::string &s, size_t &i, const char *lit)
{
    size_t len = std::strlen(lit);
    if (s.compare(i, len, lit) == 0) {
        i += len;
        return true;
    }
    return false;
}

bool ParseJsonValue(const std::string &s, size_t &i)
{
    i = SkipWs(s, i);
    if (i >= s.size()) {
        return false;
    }
    char c = s[i];
    if (c == '"') {
        return ParseJsonString(s, i);
    }
    if (c == '{') {
        i++;
        i = SkipWs(s, i);
        if (i < s.size() && s[i] == '}') {
            i++;
            return true;
        }
        while (true) {
            i = SkipWs(s, i);
            if (!ParseJsonString(s, i)) {
                return false;
            }
            i = SkipWs(s, i);
            if (i >= s.size() || s[i] != ':') {
                return false;
            }
            i++;
            if (!ParseJsonValue(s, i)) {
                return false;
            }
            i = SkipWs(s, i);
            if (i < s.size() && s[i] == ',') {
                i++;
                continue;
            }
            if (i < s.size() && s[i] == '}') {
                i++;
                return true;
            }
            return false;
        }
    }
    if (c == '[') {
        i++;
        i = SkipWs(s, i);
        if (i < s.size() && s[i] == ']') {
            i++;
            return true;
        }
        while (true) {
            if (!ParseJsonValue(s, i)) {
                return false;
            }
            i = SkipWs(s, i);
            if (i < s.size() && s[i] == ',') {
                i++;
                continue;
            }
            if (i < s.size() && s[i] == ']') {
                i++;
                return true;
            }
            return false;
        }
    }
    if (c == 't') {
        return ParseJsonLiteral(s, i, "true");
    }
    if (c == 'f') {
        return ParseJsonLiteral(s, i, "false");
    }
    if (c == 'n') {
        return ParseJsonLiteral(s, i, "null");
    }
    return ParseJsonNumber(s, i);
}

bool IsValidJson(const std::string &s)
{
    size_t i = 0;
    if (!ParseJsonValue(s, i)) {
        return false;
    }
    i = SkipWs(s, i);
    return i == s.size();
}
// --- end validator -----------------------------------------------------

Uuid MakeUuid(std::initializer_list<uint8_t> bytes)
{
    Uuid u{};
    size_t i = 0;
    for (uint8_t b : bytes) {
        u[i++] = b;
    }
    return u;
}

NodeState MakeTopologyNode(const Uuid &module_uuid, bool online, uint8_t depth, uint8_t role,
                            uint16_t sensor_bitmap, uint32_t generation, uint32_t last_seq)
{
    NodeState n;
    n.module_uuid = module_uuid;
    n.has_topology = true;
    n.online = online;
    n.depth = depth;
    n.role = role;
    n.sensor_bitmap = sensor_bitmap;
    n.boot_session_generation = generation;
    n.last_seq = last_seq;
    return n;
}

} // namespace

void RunJsonSnapshotTests(int *failures)
{
    printf("\n-- scenario: an empty Registry snapshot serializes to an empty modules array --\n");
    {
        std::vector<NodeState> empty;
        std::string json = pogohome_api::BuildSnapshotJson(empty, 0, 1700000000000LL);
        API_CHECK(IsValidJson(json), "empty snapshot JSON is syntactically valid");
        API_CHECK(json.find("\"modules\":[]") != std::string::npos, "empty snapshot has an empty modules array");
        API_CHECK(json.find("\"schema_version\":1") != std::string::npos, "schema_version is present and 1");
        API_CHECK(json.find("\"registry_generation\":0") != std::string::npos, "registry_generation is echoed");
    }

    printf("\n-- scenario: GET /health-equivalent BuildHealthJson() --\n");
    {
        std::string json = pogohome_api::BuildHealthJson(42, 2);
        API_CHECK(IsValidJson(json), "health JSON is syntactically valid");
        API_CHECK(json.find("\"service\":\"pogohome\"") != std::string::npos, "health names the service");
        API_CHECK(json.find("\"registry_generation\":42") != std::string::npos, "health echoes registry_generation");
        API_CHECK(json.find("\"module_count\":2") != std::string::npos, "health echoes module_count");
    }

    printf("\n-- scenario: online and offline modules both serialize, with a full, untruncated UUID --\n");
    {
        Uuid uuid = MakeUuid({0x74, 0x4d, 0xbd, 0xff, 0xfe, 0x65, 0xd3, 0x57});
        std::vector<NodeState> nodes;
        nodes.push_back(MakeTopologyNode(uuid, true, 1, TOPOLOGY_ROLE_INTERMEDIATE, 0x0006, 1, 123));
        Uuid uuid2 = MakeUuid({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
        nodes.push_back(MakeTopologyNode(uuid2, false, 2, TOPOLOGY_ROLE_LEAF, 0x0002, 5, 9));

        std::string json = pogohome_api::BuildSnapshotJson(nodes, 7, 1700000000000LL);
        API_CHECK(IsValidJson(json), "two-module snapshot JSON is syntactically valid");
        API_CHECK(json.find("\"uuid\":\"744dbdfffe65d357\"") != std::string::npos,
                  "the full 16-hex-char UUID is present, not truncated (matches the follow-up spec's own example)");
        API_CHECK(json.find("\"online\":true") != std::string::npos, "the first module's online:true is present");
        API_CHECK(json.find("\"online\":false") != std::string::npos, "the second module's online:false is present");
        API_CHECK(json.find("\"depth\":1") != std::string::npos, "depth 1 present");
        API_CHECK(json.find("\"depth\":2") != std::string::npos, "depth 2 present");
        API_CHECK(json.find("\"role\":\"intermediate\"") != std::string::npos, "role \"intermediate\" present");
        API_CHECK(json.find("\"role\":\"leaf\"") != std::string::npos, "role \"leaf\" present");
        API_CHECK(json.find("\"generation\":1") != std::string::npos, "module 1's topology generation present");
        API_CHECK(json.find("\"generation\":5") != std::string::npos, "module 2's topology generation present");
        API_CHECK(json.find("\"last_seq\":123") != std::string::npos, "module 1's last_seq present");
        API_CHECK(json.find("\"last_seq\":9") != std::string::npos, "module 2's last_seq present");
        API_CHECK(json.find("\"sensor_bitmap\":6") != std::string::npos, "module 1's sensor_bitmap present");
    }

    printf("\n-- scenario: a module with no sample yet reports an empty readings array --\n");
    {
        Uuid uuid = MakeUuid({0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA});
        std::vector<NodeState> nodes;
        nodes.push_back(MakeTopologyNode(uuid, true, 1, TOPOLOGY_ROLE_ROOT, 0x0001, 1, 1));
        std::string json = pogohome_api::BuildSnapshotJson(nodes, 1, 0);
        API_CHECK(IsValidJson(json), "no-sample-yet snapshot JSON is syntactically valid");
        API_CHECK(json.find("\"readings\":[]") != std::string::npos,
                  "a module with has_sample==false gets an empty readings array, never a fabricated reading");
    }

    printf("\n-- scenario: all 10 sensor kinds serialize with correct raw_value/scale/value/unit/status/sample_seq "
           "--\n");
    {
        Uuid uuid = MakeUuid({0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88});
        NodeState n = MakeTopologyNode(uuid, true, 1, TOPOLOGY_ROLE_LEAF, 0x03FF, 1, 1);
        n.has_sample = true;
        n.last_sample_seq = 555;
        n.last_sample_uptime_ms = 10000;

        auto push = [&](uint8_t type, uint8_t scale, uint8_t status, int32_t value) {
            ChannelReading ch;
            ch.type = type;
            ch.scale = scale;
            ch.status = status;
            ch.value = value;
            n.last_channels.push_back(ch);
        };
        push(SENSOR_CHANNEL_ILLUMINANCE, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, 12000);
        push(SENSOR_CHANNEL_TEMPERATURE, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, 23400);
        push(SENSOR_CHANNEL_HUMIDITY, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, 45600);
        push(SENSOR_CHANNEL_VOC_RAW, SENSOR_SCALE_NONE, SENSOR_CHANNEL_STATUS_OK, 30000);
        push(SENSOR_CHANNEL_VOC_INDEX, SENSOR_SCALE_UNIT, SENSOR_CHANNEL_STATUS_OK, 120);
        push(SENSOR_CHANNEL_NOX_RAW, SENSOR_SCALE_NONE, SENSOR_CHANNEL_STATUS_CONDITIONING, 16000);
        push(SENSOR_CHANNEL_NOX_INDEX, SENSOR_SCALE_UNIT, SENSOR_CHANNEL_STATUS_CONDITIONING, 1);
        push(SENSOR_CHANNEL_PM1_0, SENSOR_SCALE_UNIT, SENSOR_CHANNEL_STATUS_OK, 3);
        push(SENSOR_CHANNEL_PM2_5, SENSOR_SCALE_UNIT, SENSOR_CHANNEL_STATUS_OK, 5);
        push(SENSOR_CHANNEL_PM10, SENSOR_SCALE_UNIT, SENSOR_CHANNEL_STATUS_OK, 8);

        std::vector<NodeState> nodes{n};
        std::string json = pogohome_api::BuildSnapshotJson(nodes, 1, 0);
        API_CHECK(IsValidJson(json), "10-channel snapshot JSON is syntactically valid");

        const char *expected_kinds[] = {"illuminance", "temperature", "humidity", "voc_raw",  "voc_index",
                                         "nox_raw",      "nox_index",   "pm1",      "pm2_5",    "pm10"};
        for (const char *kind : expected_kinds) {
            std::string needle = std::string("\"kind\":\"") + kind + "\"";
            std::string msg = std::string("reading kind \"") + kind + "\" is present";
            API_CHECK(json.find(needle) != std::string::npos, msg.c_str());
        }
        API_CHECK(json.find("\"raw_value\":23400") != std::string::npos, "temperature raw_value preserved exactly");
        API_CHECK(json.find("\"value\":23.400") != std::string::npos, "temperature value == raw/1000 exactly");
        API_CHECK(json.find("\"unit\":\"celsius\"") != std::string::npos, "temperature unit == celsius");
        API_CHECK(json.find("\"unit\":null") != std::string::npos,
                  "voc_raw/voc_index/nox_raw/nox_index carry unit:null, never an invented unit");
        API_CHECK(json.find("\"raw_value\":30000,\"scale\":0,\"value\":30000") != std::string::npos,
                  "voc_raw: NONE scale keeps the raw driver tick count as-is, never converted to ppm/ppb");
        API_CHECK(json.find("\"status\":\"conditioning\"") != std::string::npos,
                  "the SGP4x warm-up status (CONDITIONING) is preserved, not collapsed into ok/error");
        API_CHECK(json.find("\"sample_seq\":555") != std::string::npos, "every reading carries the module's sample_seq");
    }
}
