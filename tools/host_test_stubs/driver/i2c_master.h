/*
 * Host-only stand-in for ESP-IDF's driver/i2c_master.h -- only the subset
 * components/sgp40/sgp40.c, components/sgp41/sgp41.c (and, by the same
 * shape, components/sht45/sht45.c, components/bh1750/bh1750.c) actually
 * call: bus/device creation and teardown, and a blocking transmit/receive
 * pair. Field names and enum values match the real ESP-IDF v6.1 header
 * exactly (not a simplified reinterpretation) so a REAL, unmodified driver
 * .c file host-compiles unchanged.
 *
 * This header only declares the API; tools/host_test_stubs/host_i2c_fake_device.c
 * is the actual implementation (a single in-memory fake I2C device a test
 * controls via host_i2c_fake_device.h) -- same split as driver/gpio.h's
 * declarations vs. host_gpio_sim.c's implementation.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int i2c_port_num_t;

typedef struct i2c_master_bus_t *i2c_master_bus_handle_t;
typedef struct i2c_master_dev_t *i2c_master_dev_handle_t;

typedef enum {
    I2C_CLK_SRC_DEFAULT = 0,
} soc_periph_i2c_clk_src_t;

typedef enum {
    I2C_ADDR_BIT_LEN_7 = 0,
    I2C_ADDR_BIT_LEN_10 = 1,
} i2c_addr_bit_len_t;

typedef struct {
    i2c_port_num_t i2c_port;
    gpio_num_t sda_io_num;
    gpio_num_t scl_io_num;
    soc_periph_i2c_clk_src_t clk_source;
    uint8_t glitch_ignore_cnt;
    struct {
        uint32_t enable_internal_pullup : 1;
    } flags;
} i2c_master_bus_config_t;

typedef struct {
    i2c_addr_bit_len_t dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
} i2c_device_config_t;

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *bus_config, i2c_master_bus_handle_t *ret_bus_handle);
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus_handle);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus_handle, const i2c_device_config_t *dev_config,
                                     i2c_master_dev_handle_t *ret_handle);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t handle);
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus_handle, uint16_t address, int xfer_timeout_ms);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t i2c_dev, const uint8_t *write_buffer, size_t write_size,
                               int xfer_timeout_ms);
esp_err_t i2c_master_receive(i2c_master_dev_handle_t i2c_dev, uint8_t *read_buffer, size_t read_size,
                              int xfer_timeout_ms);

#ifdef __cplusplus
}
#endif
