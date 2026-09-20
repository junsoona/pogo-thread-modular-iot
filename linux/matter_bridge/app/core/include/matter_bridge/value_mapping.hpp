// Pure, CHIP-SDK-independent unit conversion from ingest::ChannelReading (the
// exact, unmodified sensor_channel_unpack() output the ingest Registry
// stores -- see linux/matter_bridge/ingest/include/ingest/registry.hpp) to
// the Matter attribute encodings this bridge publishes. Deliberately has
// zero dependency on connectedhomeip so it builds and is host-testable
// without CHIP_ROOT (see linux/matter_bridge/app/core/CMakeLists.txt) --
// linux/matter_bridge/app/chip/ is the only place that ever touches a real
// Matter cluster type, and it only ever calls into this file for "what
// number goes in the attribute", never re-deriving units itself (see
// docs/matter-bridge.md for the channel -> cluster mapping this implements).
//
// Every function here treats a channel whose `status` is not
// SENSOR_CHANNEL_STATUS_OK, or whose `scale` is not the one this firmware
// has always used for that channel type, as producing NO valid measurement
// -- never the last-known numeric `value` re-published as if it were
// current (docs/matter-bridge.md section 3.2). The caller (chip/) maps an
// Invalid result onto that Matter attribute's own nullable convention
// (DataModel::Nullable<...>::SetNull()), never a sentinel "0" or similar.
#pragma once

#include <cstdint>

#include "ingest/registry.hpp"

namespace matter_bridge {

enum class Measurement {
    Invalid, // status != OK, wrong wire scale, or the converted value overflows this attribute's own range
    Valid,
};

struct TemperatureCentiC {
    Measurement state = Measurement::Invalid;
    // Matter TemperatureMeasurement::MeasuredValue units: 0.01 degC,
    // int16, valid range [-27315, 32767] per the cluster spec. Meaningful
    // only when state == Valid.
    int16_t value = 0;

    bool operator==(const TemperatureCentiC &o) const { return state == o.state && (state != Measurement::Valid || value == o.value); }
    bool operator!=(const TemperatureCentiC &o) const { return !(*this == o); }
};

struct HumidityCentiPercent {
    Measurement state = Measurement::Invalid;
    // Matter RelativeHumidityMeasurement::MeasuredValue units: 0.01 %RH,
    // uint16, valid range [0, 10000]. Meaningful only when state == Valid.
    uint16_t value = 0;

    bool operator==(const HumidityCentiPercent &o) const { return state == o.state && (state != Measurement::Valid || value == o.value); }
    bool operator!=(const HumidityCentiPercent &o) const { return !(*this == o); }
};

struct PmConcentrationUgm3 {
    Measurement state = Measurement::Invalid;
    // Matter {Pm1,Pm25,Pm10}ConcentrationMeasurement::MeasuredValue units:
    // micrograms per cubic meter (MeasurementUnitEnum::kUgm3), IEEE754
    // single, non-negative. Meaningful only when state == Valid.
    float value = 0.0f;

    bool operator==(const PmConcentrationUgm3 &o) const { return state == o.state && (state != Measurement::Valid || value == o.value); }
    bool operator!=(const PmConcentrationUgm3 &o) const { return !(*this == o); }
};

// Illuminance Measurement's own absolute representable ceiling for
// MeasuredValue (and the one below the reserved 0xFFFF), per the CSA spec
// (data_model/1.6/clusters/IlluminanceMeasurement.xml -- MinMeasuredValue
// itself is constrained to [1, 65533], and MaxMeasuredValue may legally be
// one above that, 65534, with no real reading ever valid above it either).
// Shared here (rather than re-typed as a separate literal in
// linux/matter_bridge/app/chip/chip_endpoint.cpp, this codebase's usual
// pattern for Temperature/Humidity's own spec-range constants) specifically
// so this bridge's configured Illuminance Measurement cluster
// minMeasuredValue/maxMeasuredValue can never be STRICTER than what
// ConvertIlluminance() itself ever returns as Valid -- a real bug this pass
// fixes: chip_endpoint.cpp used to configure a narrower, device-tuned
// maxMeasuredValue of 50000, which real direct-sunlight illuminance
// (~100,000 lux encodes to ~50001) could exceed, silently and permanently
// rejected by SetMeasuredValue()'s own ConstraintError every single time
// (BridgeManager has no way to distinguish "will succeed on retry" from
// "structurally can never succeed", so it would have retried forever).
constexpr uint16_t kIlluminanceMeasuredValueMin = 1;
constexpr uint16_t kIlluminanceMeasuredValueMax = 65534;

struct IlluminanceMeasuredValue {
    Measurement state = Measurement::Invalid;
    // Matter IlluminanceMeasurement::MeasuredValue (cluster 0x0400) units:
    // NOT lux, NOT milli-lux -- this is the CSA-defined log-encoded uint16
    // (src/app/clusters/illuminance-measurement-server, connectedhomeip
    // v1.6.0.0, confirmed against that exact tag's real source and
    // data_model/1.6/clusters/IlluminanceMeasurement.xml):
    //   lux >= 1:  value = round(10000 * log10(lux)) + 1, valid range
    //              [kIlluminanceMeasuredValueMin, kIlluminanceMeasuredValueMax]
    //              (this file's own constants, above).
    //   lux <  1 (including exactly 0): value = 0, the spec's own "too low
    //              to be measured" sentinel -- NEVER computed via the log
    //              formula (log10 of a value < 1 lux would produce a
    //              nonsensical non-positive encoded value).
    // Meaningful only when state == Valid; value == 0 is itself a valid
    // reading (dark/near-zero light), never confused with Invalid/null.
    uint16_t value = 0;

    bool operator==(const IlluminanceMeasuredValue &o) const { return state == o.state && (state != Measurement::Valid || value == o.value); }
    bool operator!=(const IlluminanceMeasuredValue &o) const { return !(*this == o); }
};

// `ch` must be a channel of type SENSOR_CHANNEL_TEMPERATURE (the caller
// selects the right ChannelReading out of NodeState::last_channels --
// this function does not search for it). Returns Invalid if ch.type is
// something else, ch.status != OK, ch.scale != SENSOR_SCALE_MILLI (this
// firmware's own SHT45 driver's only scale for this channel -- see
// components/sht45/sht45.c), or the converted value falls outside
// TemperatureMeasurement's own valid range.
TemperatureCentiC ConvertTemperature(const ingest::ChannelReading &ch);

// `ch` must be SENSOR_CHANNEL_HUMIDITY. Same rejection rules as
// ConvertTemperature, plus: a converted value outside [0, 10000] (a
// physically impossible negative %RH, or above 100.00%) is Invalid.
HumidityCentiPercent ConvertHumidity(const ingest::ChannelReading &ch);

// `ch` must be one of SENSOR_CHANNEL_PM1_0/PM2_5/PM10 (this function is the
// same for all three -- components/pms5003/pms5003.c packs all of them as
// SENSOR_SCALE_UNIT, already whole ug/m3, see that file). Invalid if
// ch.status != OK, ch.scale != SENSOR_SCALE_UNIT, or the value is negative
// (a physically impossible concentration). This function does NOT apply
// this bridge's configured Matter min/maxMeasured bound -- that is the
// cluster's own SetMeasuredValue()'s job (see linux/matter_bridge/app/chip/);
// this function only rejects values that could never be a real
// concentration reading in the first place.
PmConcentrationUgm3 ConvertPm(const ingest::ChannelReading &ch);

// `ch` must be SENSOR_CHANNEL_ILLUMINANCE (BH1750). Invalid if ch.status !=
// OK, ch.scale != SENSOR_SCALE_MILLI (this firmware's own BH1750 driver's
// only scale for this channel -- see components/bh1750/bh1750.c), or
// ch.value < 0 (a physically impossible negative lux). Otherwise applies the
// Illuminance Measurement cluster's own CSA log encoding (see
// IlluminanceMeasuredValue's own doc comment): raw lux under 1.0 (including
// exactly 0) becomes the spec's MeasuredValue=0 "too low to measure"
// sentinel; raw lux >= 1.0 is log-encoded and rejected (never clamped) if
// the encoded result falls outside the attribute's own representable
// ceiling -- unreachable for any real BH1750 reading (this driver's own
// 32-bit milli-lux ceiling encodes to well under that limit) but never
// silently truncated if it ever were.
IlluminanceMeasuredValue ConvertIlluminance(const ingest::ChannelReading &ch);

} // namespace matter_bridge
