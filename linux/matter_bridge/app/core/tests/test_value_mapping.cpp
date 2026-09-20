// Covers this repo's test list items 1-3 (SHT45 temperature/humidity
// conversion, PMS5003 PM1/PM2.5/PM10 conversion, non-OK status -> invalid)
// directly against the production matter_bridge::Convert*() functions --
// see include/matter_bridge/value_mapping.hpp.
#include "matter_bridge/value_mapping.hpp"

extern "C" {
#include "sensor_channel/sensor_channel.h"
}

#include "test_check.hpp"

using matter_bridge::ConvertHumidity;
using matter_bridge::ConvertIlluminance;
using matter_bridge::ConvertPm;
using matter_bridge::ConvertTemperature;
using matter_bridge::Measurement;

static ingest::ChannelReading make_channel(uint8_t type, uint8_t scale, uint8_t status, int32_t value)
{
    ingest::ChannelReading ch;
    ch.type = type;
    ch.scale = scale;
    ch.status = status;
    ch.value = value;
    return ch;
}

void RunValueMappingTests(int *failures)
{
    printf("\n-- ConvertTemperature: SHT45 milli-degC -> Matter centi-degC --\n");
    {
        auto ok = make_channel(SENSOR_CHANNEL_TEMPERATURE, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, 21456);
        auto t = ConvertTemperature(ok);
        MB_CHECK(t.state == Measurement::Valid, "a normal OK temperature sample converts to Valid");
        MB_CHECK(t.value == 2146, "21456 millidegC rounds to 2146 centidegC (nearest, not truncated)");

        auto neg = make_channel(SENSOR_CHANNEL_TEMPERATURE, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, -5678);
        auto tn = ConvertTemperature(neg);
        MB_CHECK(tn.state == Measurement::Valid, "a negative temperature is still a valid measurement");
        MB_CHECK(tn.value == -568, "-5678 millidegC rounds to -568 centidegC (rounding is symmetric, not biased "
                                    "toward zero for negatives)");

        auto not_ok = make_channel(SENSOR_CHANNEL_TEMPERATURE, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_CRC_ERROR, 21456);
        MB_CHECK(ConvertTemperature(not_ok).state == Measurement::Invalid,
                 "status != OK never publishes the last numeric value -- Invalid regardless of the value field");

        auto not_initialized =
            make_channel(SENSOR_CHANNEL_TEMPERATURE, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_NOT_INITIALIZED, 0);
        MB_CHECK(ConvertTemperature(not_initialized).state == Measurement::Invalid, "NOT_INITIALIZED is Invalid too");

        auto wrong_scale = make_channel(SENSOR_CHANNEL_TEMPERATURE, SENSOR_SCALE_UNIT, SENSOR_CHANNEL_STATUS_OK, 25);
        MB_CHECK(ConvertTemperature(wrong_scale).state == Measurement::Invalid,
                 "an unexpected wire scale is rejected rather than misinterpreted as millidegC");

        auto wrong_type = make_channel(SENSOR_CHANNEL_HUMIDITY, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, 21456);
        MB_CHECK(ConvertTemperature(wrong_type).state == Measurement::Invalid,
                 "a channel of the wrong type is rejected, not silently reinterpreted");

        auto at_min = make_channel(SENSOR_CHANNEL_TEMPERATURE, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, -273150);
        MB_CHECK(ConvertTemperature(at_min).state == Measurement::Valid && ConvertTemperature(at_min).value == -27315,
                 "exactly the attribute's own minimum (-27315 centidegC) is accepted");

        auto below_min = make_channel(SENSOR_CHANNEL_TEMPERATURE, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, -273160);
        MB_CHECK(ConvertTemperature(below_min).state == Measurement::Invalid,
                 "one below the attribute's own minimum is rejected as an overflow, never clamped");
    }

    printf("\n-- ConvertHumidity: SHT45 milli-%%RH -> Matter centi-%%RH --\n");
    {
        auto ok = make_channel(SENSOR_CHANNEL_HUMIDITY, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, 55123);
        auto h = ConvertHumidity(ok);
        MB_CHECK(h.state == Measurement::Valid, "a normal OK humidity sample converts to Valid");
        MB_CHECK(h.value == 5512, "55123 milli-%RH rounds to 5512 centi-%RH");

        auto negative = make_channel(SENSOR_CHANNEL_HUMIDITY, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, -100);
        MB_CHECK(ConvertHumidity(negative).state == Measurement::Invalid,
                 "a physically impossible negative %RH is rejected, never clamped to 0");

        auto over_100 = make_channel(SENSOR_CHANNEL_HUMIDITY, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, 150000);
        MB_CHECK(ConvertHumidity(over_100).state == Measurement::Invalid,
                 "a %RH above the attribute's own 100.00% ceiling is rejected as an overflow");

        auto not_ok = make_channel(SENSOR_CHANNEL_HUMIDITY, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_TIMEOUT, 55123);
        MB_CHECK(ConvertHumidity(not_ok).state == Measurement::Invalid, "status != OK -> Invalid");
    }

    printf("\n-- ConvertPm: PMS5003 whole-unit ug/m3 -> Matter concentration (uniform for PM1/PM2.5/PM10) --\n");
    {
        for (uint8_t type : {(uint8_t)SENSOR_CHANNEL_PM1_0, (uint8_t)SENSOR_CHANNEL_PM2_5, (uint8_t)SENSOR_CHANNEL_PM10}) {
            auto ok = make_channel(type, SENSOR_SCALE_UNIT, SENSOR_CHANNEL_STATUS_OK, 42);
            auto pm = ConvertPm(ok);
            MB_CHECK(pm.state == Measurement::Valid && pm.value == 42.0f,
                     "a normal OK PM sample passes its whole-unit ug/m3 value through unchanged");

            auto not_ok = make_channel(type, SENSOR_SCALE_UNIT, SENSOR_CHANNEL_STATUS_DRIVER_ERROR, 42);
            MB_CHECK(ConvertPm(not_ok).state == Measurement::Invalid, "status != OK -> Invalid for a PM channel too");

            auto negative = make_channel(type, SENSOR_SCALE_UNIT, SENSOR_CHANNEL_STATUS_OK, -1);
            MB_CHECK(ConvertPm(negative).state == Measurement::Invalid,
                     "a physically impossible negative concentration is rejected");

            auto wrong_scale = make_channel(type, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, 42000);
            MB_CHECK(ConvertPm(wrong_scale).state == Measurement::Invalid,
                     "an unexpected wire scale is rejected rather than misinterpreted");
        }
    }

    printf("\n-- ConvertIlluminance: BH1750 milli-lux -> Matter's CSA log-encoded MeasuredValue (cluster 0x0400) "
           "--\n");
    {
        // Real field-test range observed via the Linux ingest server on a
        // real BH1750 (see docs/matter-bridge.md): 18 lux (heavily shaded)
        // to 15,273 lux (bright light) on module ..d357. Expected encoded
        // values below are round(10000*log10(lux)+1), computed independently
        // (not copied from the production formula).
        auto low = make_channel(SENSOR_CHANNEL_ILLUMINANCE, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, 18000);
        auto low_v = ConvertIlluminance(low);
        MB_CHECK(low_v.state == Measurement::Valid, "an 18 lux OK sample converts to Valid");
        MB_CHECK(low_v.value == 12554, "18000 millilux (18 lux) encodes to MeasuredValue 12554");

        auto high = make_channel(SENSOR_CHANNEL_ILLUMINANCE, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, 15273000);
        auto high_v = ConvertIlluminance(high);
        MB_CHECK(high_v.state == Measurement::Valid, "a 15,273 lux OK sample converts to Valid");
        MB_CHECK(high_v.value == 41840, "15,273,000 millilux (15,273 lux) encodes to MeasuredValue 41840");

        auto exactly_1_lux = make_channel(SENSOR_CHANNEL_ILLUMINANCE, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, 1000);
        MB_CHECK(ConvertIlluminance(exactly_1_lux).state == Measurement::Valid && ConvertIlluminance(exactly_1_lux).value == 1,
                 "exactly 1.000 lux (the formula's own log10(1)=0 case) encodes to MeasuredValue 1, the attribute's "
                 "own spec-defined minimum");

        auto just_under_1_lux =
            make_channel(SENSOR_CHANNEL_ILLUMINANCE, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, 999);
        auto under_v = ConvertIlluminance(just_under_1_lux);
        MB_CHECK(under_v.state == Measurement::Valid && under_v.value == 0,
                 "999 millilux (0.999 lux, just under the spec's 1 lux floor) is still a Valid reading, encoded as "
                 "the spec's own MeasuredValue=0 'too low to be measured' sentinel -- never computed via the log "
                 "formula, which is undefined/nonsensical below 1 lux");

        auto exactly_zero = make_channel(SENSOR_CHANNEL_ILLUMINANCE, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, 0);
        auto zero_v = ConvertIlluminance(exactly_zero);
        MB_CHECK(zero_v.state == Measurement::Valid && zero_v.value == 0,
                 "exactly 0 millilux (full darkness) is Valid with MeasuredValue=0 -- 0 is a legitimate reading, "
                 "never confused with an Invalid/null measurement");

        // Regression test for a real bug this pass fixes: chip_endpoint.cpp
        // used to configure this cluster's own maxMeasuredValue as a
        // narrower, device-tuned 50000 (~99,995 lux) rather than this
        // shared matter_bridge::kIlluminanceMeasuredValueMax constant --
        // real direct-sunlight illuminance (commonly cited around 100,000
        // lux) encodes to 50001, ABOVE that old bound, so this function
        // would report it Valid while SetMeasuredValue() rejected it
        // (ConstraintError) every single time, forever. Asserting it here
        // against the actual production constant (not a hardcoded literal)
        // makes the two layers' agreement itself the thing under test.
        auto bright_sunlight =
            make_channel(SENSOR_CHANNEL_ILLUMINANCE, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, 100000000);
        auto sunlight_v = ConvertIlluminance(bright_sunlight);
        MB_CHECK(sunlight_v.state == Measurement::Valid, "100,000,000 millilux (100,000 lux, direct sunlight) is a "
                                                            "normal, real reading and must convert to Valid");
        MB_CHECK(sunlight_v.value == 50001, "100,000 lux encodes to MeasuredValue 50001");
        MB_CHECK(sunlight_v.value <= matter_bridge::kIlluminanceMeasuredValueMax,
                 "this Valid reading falls within the cluster's own configured maxMeasuredValue (the SAME shared "
                 "constant chip_endpoint.cpp uses) -- it must never be silently and permanently rejected by "
                 "SetMeasuredValue() the way a narrower, independently-tuned SDK-side bound once allowed");

        MB_CHECK(matter_bridge::kIlluminanceMeasuredValueMin == 1 && matter_bridge::kIlluminanceMeasuredValueMax == 65534,
                 "sanity check: the shared min/max constants themselves are exactly the attribute's own spec-defined "
                 "absolute range, matching every boundary case tested above and below");

        auto near_int32_max =
            make_channel(SENSOR_CHANNEL_ILLUMINANCE, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, 2147483647);
        auto near_max_v = ConvertIlluminance(near_int32_max);
        MB_CHECK(near_max_v.state == Measurement::Valid && near_max_v.value == 63320,
                 "even this channel's full 32-bit non-negative range (2,147,483,647 millilux, an unrealistic BH1750 "
                 "reading but a legal wire value) encodes to 63320 -- comfortably inside the attribute's own "
                 "representable ceiling, never spuriously rejected as an overflow");

        auto negative = make_channel(SENSOR_CHANNEL_ILLUMINANCE, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, -1);
        MB_CHECK(ConvertIlluminance(negative).state == Measurement::Invalid,
                 "a physically impossible negative lux is rejected, never clamped to 0");

        auto not_ok = make_channel(SENSOR_CHANNEL_ILLUMINANCE, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_TIMEOUT, 18000);
        MB_CHECK(ConvertIlluminance(not_ok).state == Measurement::Invalid, "status != OK -> Invalid");

        auto wrong_scale = make_channel(SENSOR_CHANNEL_ILLUMINANCE, SENSOR_SCALE_UNIT, SENSOR_CHANNEL_STATUS_OK, 18);
        MB_CHECK(ConvertIlluminance(wrong_scale).state == Measurement::Invalid,
                 "an unexpected wire scale is rejected rather than misinterpreted as millilux");

        auto wrong_type = make_channel(SENSOR_CHANNEL_TEMPERATURE, SENSOR_SCALE_MILLI, SENSOR_CHANNEL_STATUS_OK, 18000);
        MB_CHECK(ConvertIlluminance(wrong_type).state == Measurement::Invalid,
                 "a channel of the wrong type is rejected, not silently reinterpreted");
    }
}
