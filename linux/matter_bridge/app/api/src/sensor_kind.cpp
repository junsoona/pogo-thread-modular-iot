#include "pogohome_api/sensor_kind.hpp"

#include <cstdio>

extern "C" {
#include "sensor_channel/sensor_channel.h"
#include "topology_proto/topology_proto.h"
}

namespace pogohome_api {

namespace {

// Indexed by sensor_channel_type_t -- see sensor_channel.h's own enum
// (ILLUMINANCE=0 .. PM10=9, SENSOR_CHANNEL_TYPE_COUNT=10).
constexpr SensorKindInfo kKindTable[SENSOR_CHANNEL_TYPE_COUNT] = {
    {"illuminance", "lux"},
    {"temperature", "celsius"},
    {"humidity", "percent"},
    {"voc_raw", nullptr},
    {"voc_index", nullptr},
    {"nox_raw", nullptr},
    {"nox_index", nullptr},
    {"pm1", "ug/m3"},
    {"pm2_5", "ug/m3"},
    {"pm10", "ug/m3"},
};

} // namespace

const SensorKindInfo *KindInfoForChannelType(uint8_t type)
{
    if (type >= SENSOR_CHANNEL_TYPE_COUNT) {
        return nullptr;
    }
    return &kKindTable[type];
}

const char *ChannelStatusName(uint8_t status)
{
    switch (status) {
    case SENSOR_CHANNEL_STATUS_OK:
        return "ok";
    case SENSOR_CHANNEL_STATUS_NOT_INITIALIZED:
        return "not_initialized";
    case SENSOR_CHANNEL_STATUS_TIMEOUT:
        return "timeout";
    case SENSOR_CHANNEL_STATUS_BUS_ERROR:
        return "bus_error";
    case SENSOR_CHANNEL_STATUS_DRIVER_ERROR:
        return "driver_error";
    case SENSOR_CHANNEL_STATUS_CRC_ERROR:
        return "crc_error";
    case SENSOR_CHANNEL_STATUS_RANGE_ERROR:
        return "range_error";
    case SENSOR_CHANNEL_STATUS_CONDITIONING:
        return "conditioning";
    default:
        return nullptr;
    }
}

const char *TopologyRoleName(uint8_t role)
{
    switch (role) {
    case TOPOLOGY_ROLE_UNASSIGNED:
        return "unassigned";
    case TOPOLOGY_ROLE_ROOT:
        return "root";
    case TOPOLOGY_ROLE_INTERMEDIATE:
        return "intermediate";
    case TOPOLOGY_ROLE_LEAF:
        return "leaf";
    case TOPOLOGY_ROLE_ERROR:
        return "error";
    default:
        return nullptr;
    }
}

std::string FormatScaledValue(int32_t raw, uint8_t scale)
{
    int64_t divisor = 1;
    int digits = 0;
    switch (scale) {
    case SENSOR_SCALE_MILLI:
        divisor = 1000;
        digits = 3;
        break;
    case SENSOR_SCALE_MICRO:
        divisor = 1000000;
        digits = 6;
        break;
    case SENSOR_SCALE_NONE:
    case SENSOR_SCALE_UNIT:
    default:
        divisor = 1;
        digits = 0;
        break;
    }

    if (digits == 0) {
        return std::to_string(static_cast<int64_t>(raw));
    }

    // int32_t's full range (INT32_MIN included) always fits in int64_t, so
    // this negation never overflows -- unlike negating an int32_t directly,
    // which is undefined behavior for INT32_MIN.
    bool negative = raw < 0;
    int64_t abs_raw = negative ? -static_cast<int64_t>(raw) : static_cast<int64_t>(raw);
    int64_t int_part = abs_raw / divisor;
    int64_t frac_part = abs_raw % divisor;

    char frac_buf[16];
    std::snprintf(frac_buf, sizeof(frac_buf), "%0*lld", digits, static_cast<long long>(frac_part));

    std::string out;
    if (negative) {
        out += '-';
    }
    out += std::to_string(int_part);
    out += '.';
    out += frac_buf;
    return out;
}

} // namespace pogohome_api
