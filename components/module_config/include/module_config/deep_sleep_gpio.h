/*
 * Deep-sleep wake-GPIO capability check for the new UPSTREAM DETECT_IN pin
 * (GPIO12, swapped from the earlier GPIO2 -- see docs/power-base.md's
 * "Deep-sleep 범위" section and README.md's rewiring notice). Deep-sleep
 * itself is explicitly OUT of scope for this follow-up (no sleep entry/exit
 * is implemented anywhere in this firmware) -- this header only verifies,
 * ahead of any future implementation, that GPIO12 COULD be used as an ext1
 * deep-sleep wake source on ESP32-H2, so a future terminal-leaf module can
 * wake on its upstream parent's DETECT_IN edge without redoing this GPIO
 * choice.
 *
 * Two independent, complementary verifications:
 *
 *   1. Host-testable (this header, ESP-IDF-independent): a plain restatement
 *      of the ESP32-H2 RTC GPIO channel range this chip's own
 *      soc/esp32h2/include/soc/rtc_io_channel.h defines (GPIO7..GPIO14,
 *      RTCIO_GPIO7_CHANNEL..RTCIO_GPIO14_CHANNEL, ESP-IDF v6.1) -- the same
 *      table components/esp_hw_support/sleep_modes.c's
 *      esp_sleep_is_valid_wakeup_gpio() checks at runtime (via
 *      rtc_gpio_is_valid_gpio()) to decide ext1 GPIO deep-sleep wakeup
 *      eligibility. See tools/test_deep_sleep_gpio.c.
 *   2. Compile-time, ESP-IDF-only (module_config/gpio_profile.c includes
 *      this header's DEEP_SLEEP_GPIO_ESP32H2_COMPILE_TIME_CHECK macro under
 *      an esp32h2-target guard): a _Static_assert against the REAL
 *      RTCIO_GPIO12_CHANNEL macro from ESP-IDF's own soc header, so this
 *      restatement can never silently drift from the actual SoC definition
 *      across an ESP-IDF upgrade -- a mismatch fails the build loudly,
 *      before ever "checking" GPIO12's wake-capability by unproven
 *      assumption.
 *
 * Being an RTC GPIO channel member is NECESSARY but not SUFFICIENT for
 * real-hardware usability, though: GPIO7 is ALSO in this same channel range
 * (RTCIO_GPIO7_CHANNEL) but is excluded by this project's own board profile
 * (components/module_config/gpio_profile.c's NOT_BONDED_OUT_GPIOS -- not
 * bonded out on the ESP32-H2 SuperMini/SiP package at all). GPIO12 IS
 * bonded out and already used by this board profile, so it does not have
 * that problem.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP32-H2's RTC GPIO channel range (inclusive), per
 * soc/esp32h2/include/soc/rtc_io_channel.h (ESP-IDF v6.1):
 * RTCIO_GPIO7_CHANNEL .. RTCIO_GPIO14_CHANNEL. */
#define DEEP_SLEEP_WAKE_GPIO_RTC_MIN 7
#define DEEP_SLEEP_WAKE_GPIO_RTC_MAX 14

/* True if `gpio` is within ESP32-H2's RTC GPIO channel range -- necessary
 * (not sufficient on its own; see this header's top comment) for
 * esp_sleep_is_valid_wakeup_gpio()/esp_sleep_enable_ext1_wakeup_io() to ever
 * accept it as a deep-sleep wake source. */
static inline bool deep_sleep_wake_gpio_is_capable(int32_t gpio)
{
    return gpio >= DEEP_SLEEP_WAKE_GPIO_RTC_MIN && gpio <= DEEP_SLEEP_WAKE_GPIO_RTC_MAX;
}

#ifdef __cplusplus
}
#endif
