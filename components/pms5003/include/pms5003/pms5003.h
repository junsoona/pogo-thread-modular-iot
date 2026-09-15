/*
 * PMS5003 sensor_provider interface (section 11 of the follow-up spec).
 *
 * PMS5003 UART sensor_provider. It is intentionally terminal-only: UART1
 * and GPIO10/11 are borrowed from the otherwise-disabled DOWNSTREAM POGO
 * face, while UART0 remains the live UPSTREAM link. module_config validates
 * this exclusivity before the driver starts, so an INTERMEDIATE topology
 * can never silently lose its downstream POGO connection to the sensor.
 */
#pragma once

#include "sensor_provider/sensor_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const sensor_provider_ops_t g_pms5003_provider_ops;

#ifdef __cplusplus
}
#endif
