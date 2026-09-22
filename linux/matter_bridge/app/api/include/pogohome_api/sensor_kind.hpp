// PogoHome Local API v1's stable string vocabulary for the wire-format
// numeric constants in components/sensor_channel/sensor_channel.h and
// components/topology_proto/topology_proto.h's topology_role_t -- see
// docs/pogohome-api-v1.md for the full contract. This file is the ONLY
// place these mappings are defined; json_snapshot.cpp only ever calls into
// it, never re-encodes a numeric constant as a string inline.
//
// Every mapping here is a direct, lossless rename of the firmware's own
// enumerator names (snake_case, no synonyms invented) -- this API never
// derives a judgment value (no AQI, no Good/Fair/Poor) and never converts a
// raw/index channel's units (see this repo's own follow-up spec, section
// "VOC/NOx Raw와 Index를 ppm/ppb로 변환하지 않는다").
#pragma once

#include <cstdint>
#include <string>

namespace pogohome_api {

// One of the 10 sensor_channel_type_t identifiers (see sensor_channel.h),
// as this API's stable "kind" string, plus its physical unit string -- or
// nullptr for voc_raw/voc_index/nox_raw/nox_index, which have no physical
// unit (voc_raw/nox_raw are driver-defined raw ticks; voc_index/nox_index
// are the Sensirion Gas Index, a dimensionless 0..500 scale) -- inventing a
// unit for these would misrepresent what the firmware actually measured.
struct SensorKindInfo {
    const char *kind;
    const char *unit; // may be nullptr
};

// Returns nullptr if `type` is not one of the 10 known sensor_channel_type_t
// values (>= SENSOR_CHANNEL_TYPE_COUNT) -- a caller must skip/omit such a
// channel rather than invent a placeholder kind string for it (this should
// never actually happen for a channel already accepted by
// ingest::Registry::apply_sample(), which only ever stores channels whose
// type came from a real sensor_channel_unpack() call -- see that function's
// own doc comment -- but this API never trusts that invariant blindly).
const SensorKindInfo *KindInfoForChannelType(uint8_t type);

// One of the 8 sensor_channel_status_t identifiers, snake_cased exactly
// (e.g. SENSOR_CHANNEL_STATUS_NOT_INITIALIZED -> "not_initialized"). Returns
// nullptr for an unrecognized status byte.
const char *ChannelStatusName(uint8_t status);

// One of the 5 topology_role_t identifiers, snake_cased exactly (e.g.
// TOPOLOGY_ROLE_INTERMEDIATE -> "intermediate"). Returns nullptr for an
// unrecognized role byte.
const char *TopologyRoleName(uint8_t role);

// Renders `raw` (a sensor_channel_reading_t.value) as a JSON number token
// (e.g. "23.400", "-1.250000", "42") per `scale`'s sensor_scale_t meaning --
// see sensor_channel.h's own doc comment: NONE/UNIT are already whole
// integers (value == raw, printed as-is); MILLI divides by 1000 (3 decimal
// digits); MICRO divides by 1000000 (6 decimal digits). Computed entirely
// in fixed-point integer arithmetic (never through a double) specifically
// so the JSON text this API emits reconstructs the exact original integer
// -- see docs/pogohome-api-v1.md's own note on this and this repo's follow-up
// spec ("JSON 부동소수점 표현 때문에 원본을 복원할 수 없게 만들지 않는다").
// An unrecognized `scale` byte is treated the same as NONE/UNIT (raw
// printed as-is) -- defensive only; every real channel's scale byte was
// already validated by sensor_channel_unpack() before ever reaching this
// API's Registry.
std::string FormatScaledValue(int32_t raw, uint8_t scale);

} // namespace pogohome_api
