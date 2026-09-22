#include <cstdint>
#include <cstring>

#include "pogohome_api/sensor_kind.hpp"
#include "test_check.hpp"

extern "C" {
#include "sensor_channel/sensor_channel.h"
#include "topology_proto/topology_proto.h"
}

using pogohome_api::ChannelStatusName;
using pogohome_api::FormatScaledValue;
using pogohome_api::KindInfoForChannelType;
using pogohome_api::TopologyRoleName;

void RunSensorKindTests(int *failures)
{
    printf("\n-- scenario: all 10 sensor_channel_type_t values map to their documented API kind/unit --\n");
    {
        struct Expect {
            uint8_t type;
            const char *kind;
            const char *unit; // nullptr means no unit
        };
        const Expect table[] = {
            {SENSOR_CHANNEL_ILLUMINANCE, "illuminance", "lux"},
            {SENSOR_CHANNEL_TEMPERATURE, "temperature", "celsius"},
            {SENSOR_CHANNEL_HUMIDITY, "humidity", "percent"},
            {SENSOR_CHANNEL_VOC_RAW, "voc_raw", nullptr},
            {SENSOR_CHANNEL_VOC_INDEX, "voc_index", nullptr},
            {SENSOR_CHANNEL_NOX_RAW, "nox_raw", nullptr},
            {SENSOR_CHANNEL_NOX_INDEX, "nox_index", nullptr},
            {SENSOR_CHANNEL_PM1_0, "pm1", "ug/m3"},
            {SENSOR_CHANNEL_PM2_5, "pm2_5", "ug/m3"},
            {SENSOR_CHANNEL_PM10, "pm10", "ug/m3"},
        };
        for (const auto &e : table) {
            const auto *info = KindInfoForChannelType(e.type);
            char msg[128];
            std::snprintf(msg, sizeof(msg), "channel type %u maps to a kind entry", (unsigned)e.type);
            API_CHECK(info != nullptr, msg);
            if (info == nullptr) {
                continue;
            }
            std::snprintf(msg, sizeof(msg), "channel type %u kind == \"%s\"", (unsigned)e.type, e.kind);
            API_CHECK(std::strcmp(info->kind, e.kind) == 0, msg);
            if (e.unit == nullptr) {
                std::snprintf(msg, sizeof(msg), "channel type %u (%s) has no invented unit", (unsigned)e.type,
                              e.kind);
                API_CHECK(info->unit == nullptr, msg);
            } else {
                std::snprintf(msg, sizeof(msg), "channel type %u (%s) unit == \"%s\"", (unsigned)e.type, e.kind,
                              e.unit);
                API_CHECK(info->unit != nullptr && std::strcmp(info->unit, e.unit) == 0, msg);
            }
        }
        API_CHECK(KindInfoForChannelType(SENSOR_CHANNEL_TYPE_COUNT) == nullptr,
                  "an out-of-range channel type (== SENSOR_CHANNEL_TYPE_COUNT) maps to nullptr, not a fabricated kind");
        API_CHECK(KindInfoForChannelType(255) == nullptr, "channel type 255 maps to nullptr");
    }

    printf("\n-- scenario: every real sensor_channel_status_t value (0..7) maps to a stable status name --\n");
    {
        struct Expect {
            uint8_t status;
            const char *name;
        };
        const Expect table[] = {
            {SENSOR_CHANNEL_STATUS_OK, "ok"},
            {SENSOR_CHANNEL_STATUS_NOT_INITIALIZED, "not_initialized"},
            {SENSOR_CHANNEL_STATUS_TIMEOUT, "timeout"},
            {SENSOR_CHANNEL_STATUS_BUS_ERROR, "bus_error"},
            {SENSOR_CHANNEL_STATUS_DRIVER_ERROR, "driver_error"},
            {SENSOR_CHANNEL_STATUS_CRC_ERROR, "crc_error"},
            {SENSOR_CHANNEL_STATUS_RANGE_ERROR, "range_error"},
            {SENSOR_CHANNEL_STATUS_CONDITIONING, "conditioning"},
        };
        for (const auto &e : table) {
            const char *name = ChannelStatusName(e.status);
            char msg[96];
            std::snprintf(msg, sizeof(msg), "status %u == \"%s\"", (unsigned)e.status, e.name);
            API_CHECK(name != nullptr && std::strcmp(name, e.name) == 0, msg);
        }
        API_CHECK(ChannelStatusName(200) == nullptr, "an unrecognized status byte maps to nullptr");
    }

    printf("\n-- scenario: every real topology_role_t value (0..4) maps to a stable role name --\n");
    {
        struct Expect {
            uint8_t role;
            const char *name;
        };
        const Expect table[] = {
            {TOPOLOGY_ROLE_UNASSIGNED, "unassigned"}, {TOPOLOGY_ROLE_ROOT, "root"},
            {TOPOLOGY_ROLE_INTERMEDIATE, "intermediate"}, {TOPOLOGY_ROLE_LEAF, "leaf"},
            {TOPOLOGY_ROLE_ERROR, "error"},
        };
        for (const auto &e : table) {
            const char *name = TopologyRoleName(e.role);
            char msg[96];
            std::snprintf(msg, sizeof(msg), "role %u == \"%s\"", (unsigned)e.role, e.name);
            API_CHECK(name != nullptr && std::strcmp(name, e.name) == 0, msg);
        }
        API_CHECK(TopologyRoleName(200) == nullptr, "an unrecognized role byte maps to nullptr");
    }

    printf("\n-- scenario: FormatScaledValue() boundaries per sensor_scale_t --\n");
    {
        API_CHECK(FormatScaledValue(0, SENSOR_SCALE_NONE) == "0", "NONE scale: 0 -> \"0\" (whole integer, no decimal)");
        API_CHECK(FormatScaledValue(-42, SENSOR_SCALE_NONE) == "-42", "NONE scale: negative raw ticks stay a plain integer");
        API_CHECK(FormatScaledValue(350, SENSOR_SCALE_UNIT) == "350", "UNIT scale: already-whole value printed as-is");

        API_CHECK(FormatScaledValue(23400, SENSOR_SCALE_MILLI) == "23.400",
                  "MILLI scale: 23400 milli-units -> \"23.400\"");
        API_CHECK(FormatScaledValue(-21456, SENSOR_SCALE_MILLI) == "-21.456",
                  "MILLI scale: a negative raw value formats exactly (fixed-point integer division, no rounding)");
        API_CHECK(FormatScaledValue(0, SENSOR_SCALE_MILLI) == "0.000", "MILLI scale: exactly 0 -> \"0.000\"");
        API_CHECK(FormatScaledValue(999, SENSOR_SCALE_MILLI) == "0.999", "MILLI scale: sub-1-unit value keeps its own decimal digits");
        API_CHECK(FormatScaledValue(1000, SENSOR_SCALE_MILLI) == "1.000", "MILLI scale: exactly 1000 -> \"1.000\"");

        API_CHECK(FormatScaledValue(1500000, SENSOR_SCALE_MICRO) == "1.500000",
                  "MICRO scale: 1500000 micro-units -> \"1.500000\"");
        API_CHECK(FormatScaledValue(-1, SENSOR_SCALE_MICRO) == "-0.000001", "MICRO scale: smallest negative magnitude");

        // INT32_MIN specifically, to prove the abs()-via-int64_t path never
        // hits the INT32_MIN negation UB a naive int32_t abs() would.
        API_CHECK(FormatScaledValue(INT32_MIN, SENSOR_SCALE_MILLI) == "-2147483.648",
                  "MILLI scale: INT32_MIN is formatted exactly, no overflow/UB");
        API_CHECK(FormatScaledValue(INT32_MIN, SENSOR_SCALE_NONE) == "-2147483648",
                  "NONE scale: INT32_MIN printed as a plain integer");
    }
}
