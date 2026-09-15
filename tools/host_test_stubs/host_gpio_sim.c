/*
 * Implementation of the fake GPIO bank declared in driver/gpio.h -- see that
 * file's own header comment. One process-wide bank (thread-safe: pogo_port.c's
 * own port task and the test's main thread both touch it concurrently),
 * indexed directly by gpio_num_t (no sparse map needed -- a handful of pin
 * numbers in the 0..47 range, same as the real ESP32-H2).
 */
#include "driver/gpio.h"

#include <pthread.h>

#define HOST_GPIO_PIN_COUNT 48

typedef struct {
    int level;
    gpio_isr_t isr;
    void *isr_arg;
    bool isr_enabled; /* ANYEDGE configured via gpio_config() */
} host_gpio_pin_t;

static pthread_mutex_t s_gpio_mutex = PTHREAD_MUTEX_INITIALIZER;
static host_gpio_pin_t s_pins[HOST_GPIO_PIN_COUNT];
static bool s_pins_inited = false;

static void ensure_inited_locked(void)
{
    if (s_pins_inited) {
        return;
    }
    for (int i = 0; i < HOST_GPIO_PIN_COUNT; i++) {
        s_pins[i].level = 1; /* idle-high, matching an external pull-up with nothing driving it Low */
        s_pins[i].isr = NULL;
        s_pins[i].isr_arg = NULL;
        s_pins[i].isr_enabled = false;
    }
    s_pins_inited = true;
}

static bool pin_valid(gpio_num_t pin)
{
    return pin >= 0 && pin < HOST_GPIO_PIN_COUNT;
}

esp_err_t gpio_config(const gpio_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_gpio_mutex);
    ensure_inited_locked();
    for (int pin = 0; pin < HOST_GPIO_PIN_COUNT; pin++) {
        if (!((cfg->pin_bit_mask >> pin) & 1ULL)) {
            continue;
        }
        s_pins[pin].isr_enabled = (cfg->intr_type == GPIO_INTR_ANYEDGE);
        /* mode/pull_up_en/pull_down_en are accepted but not otherwise
         * modeled -- this fake bank has no concept of "undriven float",
         * only an explicit level, matching how pogo_port.c only ever reads
         * gpio_get_level() (never distinguishes float from a driven level). */
    }
    pthread_mutex_unlock(&s_gpio_mutex);
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    if (!pin_valid(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_gpio_mutex);
    ensure_inited_locked();
    s_pins[pin].level = (level != 0) ? 1 : 0;
    pthread_mutex_unlock(&s_gpio_mutex);
    return ESP_OK;
}

int gpio_get_level(gpio_num_t pin)
{
    if (!pin_valid(pin)) {
        return 0;
    }
    pthread_mutex_lock(&s_gpio_mutex);
    ensure_inited_locked();
    int level = s_pins[pin].level;
    pthread_mutex_unlock(&s_gpio_mutex);
    return level;
}

esp_err_t gpio_install_isr_service(int flags)
{
    (void)flags;
    return ESP_OK;
}

esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t handler, void *arg)
{
    if (!pin_valid(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_gpio_mutex);
    ensure_inited_locked();
    s_pins[pin].isr = handler;
    s_pins[pin].isr_arg = arg;
    pthread_mutex_unlock(&s_gpio_mutex);
    return ESP_OK;
}

esp_err_t gpio_isr_handler_remove(gpio_num_t pin)
{
    if (!pin_valid(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_gpio_mutex);
    ensure_inited_locked();
    s_pins[pin].isr = NULL;
    s_pins[pin].isr_arg = NULL;
    pthread_mutex_unlock(&s_gpio_mutex);
    return ESP_OK;
}

void host_gpio_test_set_level(gpio_num_t pin, int level)
{
    if (!pin_valid(pin)) {
        return;
    }
    pthread_mutex_lock(&s_gpio_mutex);
    ensure_inited_locked();
    s_pins[pin].level = level ? 1 : 0;
    pthread_mutex_unlock(&s_gpio_mutex);
}

void host_gpio_test_edge(gpio_num_t pin, int level)
{
    if (!pin_valid(pin)) {
        return;
    }
    pthread_mutex_lock(&s_gpio_mutex);
    ensure_inited_locked();
    s_pins[pin].level = level ? 1 : 0;
    gpio_isr_t isr = s_pins[pin].isr_enabled ? s_pins[pin].isr : NULL;
    void *isr_arg = s_pins[pin].isr_arg;
    pthread_mutex_unlock(&s_gpio_mutex);
    /* Called with the lock released: a real ISR handler
     * (pogo_port.c's detect_isr_handler) itself calls back into
     * gpio_get_level() -- holding s_gpio_mutex across that call would
     * self-deadlock (this mutex is not recursive, matching a real FreeRTOS
     * critical section's own non-reentrant contract). */
    if (isr != NULL) {
        isr(isr_arg);
    }
}

void host_gpio_test_reset(void)
{
    pthread_mutex_lock(&s_gpio_mutex);
    s_pins_inited = false;
    ensure_inited_locked();
    pthread_mutex_unlock(&s_gpio_mutex);
}
