/*
 * Test-only fake PMS5003 sensor: a background thread that watches (PASSIVE
 * mode) or periodically drives (ACTIVE mode) one fake UART port exactly the
 * way a real Plantower PMS5003 would, so a host test can link and run the
 * REAL, unmodified components/pms5003/pms5003.c (and, through it,
 * pms5003_parser.c) against something that actually answers on the wire --
 * see tools/host_test_stubs/driver/uart.h. This is a stand-in for the
 * PHYSICAL SENSOR hardware itself (same category as the GPIO/UART bank
 * stubs), never a reimplementation of pms5003.c's own driver logic, which
 * runs completely unmodified against it.
 *
 * Used by tools/test_pms5003_driver.c and tools/test_conditioning_provisioning.c.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the fake sensor on `uart_num` (must already be "installed" via the
 * real pms5003_init()'s own uart_driver_install() by the time frames should
 * start flowing -- safe to start before that too, PASSIVE mode simply has
 * nothing to answer yet). `active_mode`: true pushes one fresh frame every
 * ~100ms unconditionally (mirrors PMS5003 ACTIVE mode); false answers each
 * READ command (0x42 0x4D 0xE2 ...) seen on the port's TX with exactly one
 * frame (mirrors PASSIVE mode). Only one fake sensor instance exists per
 * process -- calling this again first stops any previous one. */
void host_pms5003_fake_sensor_start(int uart_num, bool active_mode, uint16_t pm1_0, uint16_t pm2_5, uint16_t pm10);

/* Changes the PM values the next frame(s) will report -- takes effect
 * immediately for ACTIVE mode's next periodic push, or the next READ
 * response in PASSIVE mode. */
void host_pms5003_fake_sensor_set_values(uint16_t pm1_0, uint16_t pm2_5, uint16_t pm10);

/* Stops sending well-formed frames: instead pushes `len` raw bytes verbatim
 * (garbage/noise, a truncated frame, a bad-checksum frame -- whatever the
 * caller constructs) once, then reverts to normal behavior. Used to test
 * pms5003_parser.c's real resync-after-garbage path end-to-end through the
 * real driver. */
void host_pms5003_fake_sensor_inject_raw(const uint8_t *data, size_t len);

/* Stops the background thread. Safe to call even if never started. */
void host_pms5003_fake_sensor_stop(void);

#ifdef __cplusplus
}
#endif
