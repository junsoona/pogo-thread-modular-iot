#include "module_config/gpio_profile.h"
#include "module_config/deep_sleep_gpio.h"

#include <stddef.h>

/*
 * Compile-time cross-check (ESP-IDF/esp32h2 builds only -- host test builds
 * of this file, e.g. tools/test_gpio_profile.c, do not have ESP-IDF's soc/
 * headers on their include path, so __has_include() below simply evaluates
 * false there and this block is skipped entirely): confirms
 * deep_sleep_gpio.h's host-testable RTC-GPIO-range restatement agrees with
 * the REAL RTCIO_GPIO12_CHANNEL macro ESP-IDF v6.1's own
 * soc/esp32h2/include/soc/rtc_io_channel.h defines for GPIO12 (the new
 * UPSTREAM DETECT_IN pin -- see docs/power-base.md's "Deep-sleep 범위"
 * section). A future ESP-IDF revision that removed or renumbered this
 * channel would fail this build loudly, not silently invalidate an
 * unverified assumption baked into deep_sleep_gpio.h. */
#if defined(__has_include)
#if __has_include("soc/rtc_io_channel.h")
#include "soc/rtc_io_channel.h"
#ifndef RTCIO_GPIO12_CHANNEL
#error "GPIO12 is no longer an ESP32-H2 RTC GPIO channel in this ESP-IDF version -- re-review the Deep-sleep wake-GPIO choice in docs/power-base.md before proceeding (see components/module_config/deep_sleep_gpio.h)"
#endif
_Static_assert(DEEP_SLEEP_WAKE_GPIO_RTC_MIN <= 12 && 12 <= DEEP_SLEEP_WAKE_GPIO_RTC_MAX,
               "deep_sleep_gpio.h's host-testable RTC GPIO range must include GPIO12, matching "
               "soc/esp32h2/include/soc/rtc_io_channel.h's RTCIO_GPIO12_CHANNEL");
#endif
#endif

bool gpio_profile_in_range(int32_t gpio)
{
    return gpio == -1 || (gpio >= 0 && gpio <= GPIO_PROFILE_MAX);
}

const char *gpio_profile_reserved_reason(int32_t gpio)
{
    static const int32_t strapping[] = {8, 9, 25};
    static const int32_t not_bonded_out[] = {6, 7};
    static const int32_t flash[] = {15, 16, 17, 18, 19, 20, 21};
    static const int32_t usb[] = {26, 27};

    if (gpio < 0) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(strapping) / sizeof(strapping[0]); i++) {
        if (gpio == strapping[i]) {
            return "boot strapping pin";
        }
    }
    for (size_t i = 0; i < sizeof(not_bonded_out) / sizeof(not_bonded_out[0]); i++) {
        if (gpio == not_bonded_out[i]) {
            return "not bonded out on ESP32-H2 SuperMini/SiP package";
        }
    }
    for (size_t i = 0; i < sizeof(flash) / sizeof(flash[0]); i++) {
        if (gpio == flash[i]) {
            return "internal SPI flash pin";
        }
    }
    for (size_t i = 0; i < sizeof(usb) / sizeof(usb[0]); i++) {
        if (gpio == usb[i]) {
            return "USB Serial/JTAG pin";
        }
    }
    return NULL;
}
