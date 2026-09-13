/*
 * Sensirion SHT45 temperature + humidity sensor_provider driver (section 5
 * of the follow-up spec). Registers itself under SENSOR_DRIVER_ID_SHT45
 * (see sensor_provider.h) via g_sht45_provider_ops.
 *
 * Uses the fixed I2C address from module_config (i2c_address, normally
 * 0x44) ONLY -- never probes any other address.
 */
#pragma once

#include "sensor_provider/sensor_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const sensor_provider_ops_t g_sht45_provider_ops;

#ifdef __cplusplus
}
#endif
