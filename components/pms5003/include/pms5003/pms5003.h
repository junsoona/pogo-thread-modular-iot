/*
 * PMS5003 sensor_provider interface (section 11 of the follow-up spec).
 *
 * PMS5003 UART sensor_provider. It is intentionally terminal-only: UART1
 * uses GPIO0/1 (GPIO0=ESP TX->PMS RX, GPIO1=ESP RX<-PMS TX) for the sensor
 * signal, while UART0 remains the live UPSTREAM link and the downstream
 * POGO face (UART1/GPIO10-11) stays disabled. module_config validates this
 * exclusivity before the driver starts, so an INTERMEDIATE topology can
 * never silently lose its downstream POGO connection to the sensor.
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
