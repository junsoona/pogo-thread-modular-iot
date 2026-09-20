#include "matter_bridge/value_mapping.hpp"

extern "C" {
#include "sensor_channel/sensor_channel.h"
}

#include <cmath>
#include <limits>

namespace matter_bridge {

namespace {

// Rounds `value` (a count of milli-units) to the nearest centi-unit
// (divide by 10, ties rounded away from zero) -- e.g. 21456 millidegC ->
// 2146 centidegC, -21456 -> -2146. Plain truncating division would bias
// every negative reading toward zero (less accurate, and inconsistent with
// how positive readings round), so this is done explicitly rather than
// relying on C++'s truncating integer division semantics.
int32_t RoundMilliToCenti(int32_t milli)
{
    if (milli >= 0) {
        return (milli + 5) / 10;
    }
    return -((-milli + 5) / 10);
}

} // namespace

TemperatureCentiC ConvertTemperature(const ingest::ChannelReading &ch)
{
    TemperatureCentiC out;
    if (ch.type != SENSOR_CHANNEL_TEMPERATURE) {
        return out; // Invalid: not a temperature channel at all
    }
    if (ch.status != SENSOR_CHANNEL_STATUS_OK) {
        return out; // Invalid: docs/matter-bridge.md section 3.2 -- never publish a stale/unconfirmed value
    }
    if (ch.scale != SENSOR_SCALE_MILLI) {
        return out; // Invalid: unexpected wire scale -- never misinterpret units
    }

    int64_t centi = RoundMilliToCenti(ch.value);
    // TemperatureMeasurement::MeasuredValue's own spec-defined valid range.
    constexpr int64_t kMin = -27315;
    constexpr int64_t kMax = 32767;
    if (centi < kMin || centi > kMax) {
        return out; // Invalid: out of the attribute's own representable range
    }

    out.state = Measurement::Valid;
    out.value = static_cast<int16_t>(centi);
    return out;
}

HumidityCentiPercent ConvertHumidity(const ingest::ChannelReading &ch)
{
    HumidityCentiPercent out;
    if (ch.type != SENSOR_CHANNEL_HUMIDITY) {
        return out;
    }
    if (ch.status != SENSOR_CHANNEL_STATUS_OK) {
        return out;
    }
    if (ch.scale != SENSOR_SCALE_MILLI) {
        return out;
    }

    int64_t centi = RoundMilliToCenti(ch.value);
    // RelativeHumidityMeasurement::MeasuredValue's own spec-defined valid
    // range -- also rejects a physically impossible negative %RH.
    constexpr int64_t kMin = 0;
    constexpr int64_t kMax = 10000;
    if (centi < kMin || centi > kMax) {
        return out;
    }

    out.state = Measurement::Valid;
    out.value = static_cast<uint16_t>(centi);
    return out;
}

PmConcentrationUgm3 ConvertPm(const ingest::ChannelReading &ch)
{
    PmConcentrationUgm3 out;
    if (ch.type != SENSOR_CHANNEL_PM1_0 && ch.type != SENSOR_CHANNEL_PM2_5 && ch.type != SENSOR_CHANNEL_PM10) {
        return out;
    }
    if (ch.status != SENSOR_CHANNEL_STATUS_OK) {
        return out;
    }
    if (ch.scale != SENSOR_SCALE_UNIT) {
        return out;
    }
    if (ch.value < 0) {
        return out; // a physically impossible negative concentration
    }

    out.state = Measurement::Valid;
    out.value = static_cast<float>(ch.value);
    return out;
}

IlluminanceMeasuredValue ConvertIlluminance(const ingest::ChannelReading &ch)
{
    IlluminanceMeasuredValue out;
    if (ch.type != SENSOR_CHANNEL_ILLUMINANCE) {
        return out;
    }
    if (ch.status != SENSOR_CHANNEL_STATUS_OK) {
        return out;
    }
    if (ch.scale != SENSOR_SCALE_MILLI) {
        return out;
    }
    if (ch.value < 0) {
        return out; // a physically impossible negative lux
    }

    // Below 1.0 lux (1000 milli-lux), including exactly 0, is the spec's
    // own MeasuredValue=0 "too low to be measured" sentinel -- the log
    // formula below is only defined (and only ever computed) for lux >= 1.0.
    if (ch.value < 1000) {
        out.state = Measurement::Valid;
        out.value = 0;
        return out;
    }

    double lux = static_cast<double>(ch.value) / 1000.0;
    double encoded = std::round(10000.0 * std::log10(lux) + 1.0);

    // The attribute's own absolute representable ceiling (kIlluminanceMeasuredValueMax,
    // one below the reserved 0xFFFF) -- see this file's own top-of-header
    // doc comment on why this is a SHARED constant with chip_endpoint.cpp's
    // own configured minMeasuredValue/maxMeasuredValue, not a private local
    // one: this guarantees the SDK layer's own configured range is never
    // stricter than what this function itself ever returns as Valid.
    // Mathematically unreachable for any real BH1750 reading: even this
    // channel's full 32-bit non-negative milli-lux range (int32_t max,
    // ~2.147e6 lux) encodes to ~63321, comfortably under this ceiling.
    // Guarded anyway rather than relying on that property implicitly, same
    // as ConvertTemperature()/ConvertHumidity()'s own range checks.
    if (encoded < static_cast<double>(kIlluminanceMeasuredValueMin) ||
        encoded > static_cast<double>(kIlluminanceMeasuredValueMax)) {
        return out; // Invalid: out of the attribute's own representable range
    }

    out.state = Measurement::Valid;
    out.value = static_cast<uint16_t>(encoded);
    return out;
}

} // namespace matter_bridge
