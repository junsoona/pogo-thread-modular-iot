/*
 * PMS5003 sensor_provider interface (section 11 of the follow-up spec).
 *
 * IMPORTANT: this is a REGISTRABLE INTERFACE, not a working driver. Its
 * init() always fails with a clear, honest log line
 * ("PMS5003 hardware transport blocked by two-UART allocation") and never
 * pretends to read real hardware -- ESP32-H2 has exactly two UART
 * controllers, and both are permanently committed to the two POGO ports
 * (see module_config's confirmed GPIO profile, docs/pogo-interface.md).
 * There is no third hardware UART left for PMS5003's continuous 9600 8N1
 * stream, and this firmware does not use a busy-wait software UART or
 * borrow a POGO UART that would stop working the moment this module became
 * INTERMEDIATE (both explicitly rejected -- see docs/pogo-interface.md's
 * PMS5003 section for the transport options actually considered and why
 * each was rejected or deferred).
 *
 * What IS complete: the frame parser (pms5003_parser.h/.c, fully
 * host-tested) and this registrable sensor_provider_ops_t shape, so that
 * the day a transport exists (most likely an external I2C-to-UART bridge --
 * see docs/pogo-interface.md), only start_measurement()/read_measurement()
 * need real I/O added here; the parser, channel mapping, and the rest of
 * this firmware need no change at all.
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
