/*
 * Host-only stand-in for ESP-IDF's driver/gpio.h, backing REAL, unmodified
 * components/pogo_port/pogo_port.c (DETECT_IN/DETECT_OUT/BASE_ID) and
 * power_base/main/app_main.c-style power_enable_gpio users with a small
 * in-memory fake GPIO bank -- see tools/host_test_stubs/host_gpio_sim.c for
 * the implementation and tools/test_pogo_port_hotswap_recovery.c for the
 * test-only control functions declared at the bottom of this file (their own
 * comments explain the "raw level change" vs. "edge with ISR fired" distinction
 * that lets a test simulate a missed interrupt).
 *
 * Declarations only (extern, not static/inline): unlike the other stubs in
 * this directory, the fake GPIO bank's state must be shared across every
 * translation unit in a test binary (pogo_port.c calls gpio_get_level(),
 * the test file calls the host_gpio_test_* controls below, and both must
 * observe the SAME pins) -- so this is a real, single, linked definition in
 * host_gpio_sim.c, not a per-TU `static inline`.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_attr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int gpio_num_t;

typedef enum {
    GPIO_MODE_DISABLE = 0,
    GPIO_MODE_INPUT = 1,
    GPIO_MODE_OUTPUT = 2,
    GPIO_MODE_OUTPUT_OD = 6,
} gpio_mode_t;

typedef enum {
    GPIO_PULLUP_DISABLE = 0,
    GPIO_PULLUP_ENABLE = 1,
} gpio_pullup_t;

typedef enum {
    GPIO_PULLDOWN_DISABLE = 0,
    GPIO_PULLDOWN_ENABLE = 1,
} gpio_pulldown_t;

typedef enum {
    GPIO_INTR_DISABLE = 0,
    GPIO_INTR_ANYEDGE = 3,
} gpio_int_type_t;

typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

typedef void (*gpio_isr_t)(void *arg);

esp_err_t gpio_config(const gpio_config_t *cfg);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
int gpio_get_level(gpio_num_t pin);
esp_err_t gpio_install_isr_service(int flags);
esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t handler, void *arg);
esp_err_t gpio_isr_handler_remove(gpio_num_t pin);

/* --- Test-only controls (not part of the real ESP-IDF API), implemented in
 * host_gpio_sim.c --- */

/* Sets a pin's raw level directly, WITHOUT invoking any registered ISR
 * handler -- models the physical wire changing while an interrupt is
 * missed/never fires (used by tools/test_pogo_port_hotswap_recovery.c's
 * "missed ISR / queue overflow" regression scenario to verify
 * pogo_port.c's own periodic level-reconciliation backstop). */
void host_gpio_test_set_level(gpio_num_t pin, int level);

/* Sets a pin's raw level AND, if an ISR handler is currently registered for
 * it, calls that handler synchronously -- models a normal, working hardware
 * interrupt firing on a real edge. This is what every scenario other than
 * the "missed ISR" one should use to simulate DETECT_IN changing. */
void host_gpio_test_edge(gpio_num_t pin, int level);

/* Resets every pin to level 1 (idle-high, matching a DETECT_IN/BASE_ID
 * pull-up with nothing driving it Low) and clears every registered ISR
 * handler -- call between test scenarios that reuse the same pin numbers,
 * so one scenario's state can never leak into the next. */
void host_gpio_test_reset(void);

#ifdef __cplusplus
}
#endif
