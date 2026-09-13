/*
 * ROHM BH1750FVI ambient light (illuminance) sensor_provider driver
 * (section 4 of the follow-up spec). Registers itself under
 * SENSOR_DRIVER_ID_BH1750 (see sensor_provider.h) via g_bh1750_provider_ops.
 *
 * Uses the fixed I2C address from module_config (i2c_address) ONLY -- never
 * probes 0x23/0x5C or any other address (section 4: "지정된 주소만
 * 사용하며 다른 주소를 자동 탐색하지 않음").
 */
#pragma once

#include "sensor_provider/sensor_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const sensor_provider_ops_t g_bh1750_provider_ops;

#ifdef __cplusplus
}
#endif
