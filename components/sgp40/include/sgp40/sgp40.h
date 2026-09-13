/*
 * Sensirion SGP40 VOC sensor_provider driver (section 10 of the follow-up
 * spec). Registers itself under SENSOR_DRIVER_ID_SGP40 (see
 * sensor_provider.h) via g_sgp40_provider_ops.
 *
 * I2C commands/timings below are taken from Sensirion's own public
 * reference driver (https://github.com/Sensirion/embedded-i2c-sgp40,
 * BSD-3-Clause) -- fixed 7-bit address 0x59, measure-raw-signal command
 * 0x260F, self-test 0x280E, no separate "conditioning" command (unlike
 * SGP41 -- see components/sgp41): the VOC Index Algorithm's own initial
 * blackout period is what absorbs the hotplate's warm-up, so this driver
 * only marks the first CONDITIONING_WINDOW_MS as SENSOR_CHANNEL_STATUS_CONDITIONING
 * for reporting purposes, without a distinct I2C command.
 */
#pragma once

#include "sensor_provider/sensor_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const sensor_provider_ops_t g_sgp40_provider_ops;

#ifdef __cplusplus
}
#endif
