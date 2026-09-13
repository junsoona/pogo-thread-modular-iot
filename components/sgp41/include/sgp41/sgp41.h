/*
 * Sensirion SGP41 VOC+NOx sensor_provider driver (section 10 of the
 * follow-up spec). Registers itself under SENSOR_DRIVER_ID_SGP41 (see
 * sensor_provider.h) via g_sgp41_provider_ops.
 *
 * I2C commands/timings below are taken from Sensirion's own public
 * reference driver (https://github.com/Sensirion/embedded-i2c-sgp41,
 * BSD-3-Clause) -- fixed 7-bit address 0x59 (same address family as SGP40;
 * a board never pairs both on one bus), conditioning command 0x2612 (VOC
 * raw only, run for the first CONDITIONING_WINDOW_MS -- unlike SGP40, SGP41
 * DOES have a dedicated conditioning command, needed to stabilize the NOx
 * baseline before combined measurement starts), measure-raw-signals command
 * 0x2619 (VOC+NOx raw together), self-test 0x280E.
 */
#pragma once

#include "sensor_provider/sensor_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const sensor_provider_ops_t g_sgp41_provider_ops;

#ifdef __cplusplus
}
#endif
