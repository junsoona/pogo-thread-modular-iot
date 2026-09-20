/*
 * Test-only fake I2C device: a single in-memory stand-in for the physical
 * sensor on the other end of driver/i2c_master.h's blocking transmit/
 * receive calls, so a host test can link and run a REAL, unmodified I2C
 * sensor_provider driver (components/sgp40/sgp40.c,
 * components/sgp41/sgp41.c) against something that actually answers on the
 * bus -- same category as tools/host_test_stubs/host_pms5003_fake_sensor.c,
 * but far simpler: ESP-IDF's i2c_master API is itself synchronous/blocking,
 * so no background thread or real wire timing is needed -- a test just
 * queues the exact bytes the next i2c_master_receive() call should hand
 * back before calling into the driver.
 *
 * One fake device per process (mirrors every sensor_provider driver's own
 * "one sensor per module, ever" singleton, see e.g. sgp40.c's s_ctx).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Clears all queued responses and transmit history. Call at the start of
 * each test scenario. */
void host_i2c_fake_device_reset(void);

/* Queues exactly one response, `len` bytes (len <= 8), for the NEXT
 * i2c_master_receive() call -- consumed (and removed from the queue) by
 * that call. Up to 4 responses may be queued at once (this driver family
 * never needs more than one or two in flight). */
void host_i2c_fake_device_queue_response(const uint8_t *data, size_t len);

/* i2c_master_receive() with no response queued returns ESP_FAIL (simulates
 * an unresponsive/disconnected device) unless this is called first with
 * `fail_writes=true`, in which case i2c_master_transmit() ALSO fails --
 * used to test a driver's own bus-error handling, not needed for a plain
 * "the caller's own tick budget ran out" scenario. */
void host_i2c_fake_device_set_fail_writes(bool fail_writes);

/* Test inspection: the bytes written by the MOST RECENT i2c_master_transmit()
 * call, copied into `out` (capacity `out_cap`). Returns how many bytes were
 * actually written (0 if no transmit has happened yet since the last reset). */
size_t host_i2c_fake_device_last_transmit(uint8_t *out, size_t out_cap);

/* Total i2c_master_receive() calls since the last reset -- lets a test
 * assert a receive was (or, for the tick-timeout regression, was NOT)
 * ever reached. */
int host_i2c_fake_device_receive_call_count(void);

#ifdef __cplusplus
}
#endif
