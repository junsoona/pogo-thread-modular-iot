/*
 * Host-compiled verification of components/module_config/deep_sleep_gpio.h's
 * ESP32-H2 RTC-GPIO-channel restatement, for the new UPSTREAM DETECT_IN pin
 * (GPIO12, swapped from the earlier GPIO2 -- see docs/power-base.md's
 * "Deep-sleep 범위" section and README.md's rewiring notice). Deep-sleep
 * itself is out of scope for this follow-up; this only confirms GPIO12
 * COULD be used as a future terminal-leaf ext1 deep-sleep wake source.
 *
 * Links against the REAL, unmodified deep_sleep_gpio.h (header-only, no
 * ESP-IDF dependency) and gpio_profile.c (for gpio_profile_reserved_reason(),
 * to confirm GPIO7 -- also an RTC GPIO channel member -- is still excluded
 * by this board profile's own NOT_BONDED_OUT_GPIOS policy). A SEPARATE,
 * ESP-IDF-only compile-time check (gpio_profile.c's own _Static_assert
 * against the REAL soc/esp32h2/include/soc/rtc_io_channel.h) runs during
 * every real ESP32-H2 build of this firmware -- see that file's comment.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/module_config/include \
 *      -o /tmp/test_deep_sleep_gpio tools/test_deep_sleep_gpio.c \
 *      components/module_config/gpio_profile.c \
 *   && /tmp/test_deep_sleep_gpio
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "module_config/deep_sleep_gpio.h"
#include "module_config/gpio_profile.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                                   \
            g_failures++;                                                                                            \
        } else {                                                                                                     \
            printf("ok:   %s\n", msg);                                                                               \
        }                                                                                                            \
    } while (0)

int main(void)
{
    printf("-- ESP32-H2 deep-sleep wake-GPIO capability (GPIO12, the new UPSTREAM DETECT_IN) --\n");

    CHECK(deep_sleep_wake_gpio_is_capable(12),
          "GPIO12 (new UPSTREAM DETECT_IN) is within the ESP32-H2 RTC GPIO channel range -- a future terminal-leaf "
          "ext1 deep-sleep wake source candidate");

    /* GPIO7 is ALSO an RTC GPIO channel member (RTCIO_GPIO7_CHANNEL), but
     * this board profile still rejects it outright -- being RTC-capable is
     * necessary, not sufficient, on its own (see deep_sleep_gpio.h's top
     * comment). */
    CHECK(deep_sleep_wake_gpio_is_capable(7), "GPIO7 is also within the RTC GPIO channel range in principle");
    CHECK(gpio_profile_reserved_reason(7) != NULL,
          "...but GPIO7 is still rejected by this board profile (not bonded out on the SuperMini/SiP package)");
    CHECK(strcmp(gpio_profile_reserved_reason(7), "not bonded out on ESP32-H2 SuperMini/SiP package") == 0,
          "GPIO7's rejection reason is specifically 'not bonded out', not some other category");

    /* GPIO2 (the OLD UPSTREAM DETECT_IN, now DETECT_OUT) and GPIO14
     * (BASE_ID) are both outside the RTC GPIO range on the low end / used
     * for a different signal -- GPIO2 in particular is why this follow-up
     * moved DETECT_IN to GPIO12 instead of leaving it where it was. */
    CHECK(!deep_sleep_wake_gpio_is_capable(2),
          "GPIO2 (now DETECT_OUT) is NOT within the RTC GPIO channel range -- not a wake-capable candidate");
    CHECK(deep_sleep_wake_gpio_is_capable(14),
          "GPIO14 (BASE_ID) happens to also be within the RTC GPIO channel range (not used as a wake source here, "
          "just confirming the boundary)");
    CHECK(!deep_sleep_wake_gpio_is_capable(6), "GPIO6 (outside the 7..14 range, and separately not bonded out) is not capable");
    CHECK(!deep_sleep_wake_gpio_is_capable(15), "GPIO15 (just above the 7..14 range) is not capable");

    /* Both boundary endpoints explicitly. */
    CHECK(deep_sleep_wake_gpio_is_capable(DEEP_SLEEP_WAKE_GPIO_RTC_MIN), "the range's own MIN boundary is capable");
    CHECK(deep_sleep_wake_gpio_is_capable(DEEP_SLEEP_WAKE_GPIO_RTC_MAX), "the range's own MAX boundary is capable");
    CHECK(!deep_sleep_wake_gpio_is_capable(DEEP_SLEEP_WAKE_GPIO_RTC_MIN - 1), "one below MIN is not capable");
    CHECK(!deep_sleep_wake_gpio_is_capable(DEEP_SLEEP_WAKE_GPIO_RTC_MAX + 1), "one above MAX is not capable");

    printf("\n");
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
