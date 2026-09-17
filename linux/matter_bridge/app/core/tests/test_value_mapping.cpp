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
}
