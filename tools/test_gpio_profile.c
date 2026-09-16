/*
 * Host-compiled test for components/module_config/gpio_profile.c -- the
 * GPIO-correction follow-up's section 2 minimum test list: "GPIO2 허용,
 * GPIO3 허용, GPIO6/7 거부, GPIO8/9/25 거부, GPIO15/21 거부, GPIO26/27 거부".
 *
 * Links against the REAL, unmodified gpio_profile.c (no ESP-IDF/FreeRTOS
 * dependency).
 *
 * Also supports a `--dump` mode: prints one `<gpio>,<allowed 0|1>,<reason or
 * empty>` line per GPIO in [-1, 27], used by
 * tools/test_gpio_profile_parity.py to mechanically check that
 * tools/pack_module_config.py's Python classification never drifts from
 * this C one, instead of two hand-maintained lists silently diverging.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/module_config/include \
 *      -o /tmp/test_gpio_profile tools/test_gpio_profile.c \
 *      components/module_config/gpio_profile.c \
 *   && /tmp/test_gpio_profile
 *   /tmp/test_gpio_profile --dump   # for the Python parity check
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "module_config/gpio_profile.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s\n", msg);                                                                               \
            g_failures++;                                                                                            \
        } else {                                                                                                     \
            printf("ok:   %s\n", msg);                                                                               \
        }                                                                                                            \
    } while (0)

static bool allowed(int32_t gpio)
{
    return gpio_profile_in_range(gpio) && gpio_profile_reserved_reason(gpio) == NULL;
}

static void dump(void)
{
    for (int32_t gpio = -1; gpio <= GPIO_PROFILE_MAX; gpio++) {
        bool in_range = gpio_profile_in_range(gpio);
        const char *reason = in_range ? gpio_profile_reserved_reason(gpio) : "out of range";
        printf("%d,%d,%s\n", gpio, (in_range && reason == NULL) ? 1 : 0, reason ? reason : "");
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
        dump();
        return 0;
    }

    /* -1 (not present) is always allowed. */
    CHECK(gpio_profile_in_range(-1) && gpio_profile_reserved_reason(-1) == NULL, "-1 (not present) is always allowed");

    /* GPIO2/GPIO3 allowed -- NOT strapping pins in this profile (corrected). */
    CHECK(allowed(2), "GPIO2 is allowed (usable as a POGO GPIO with USB-Serial/JTAG console)");
    CHECK(allowed(3), "GPIO3 is allowed");
    CHECK(allowed(4), "GPIO4 is allowed (UPSTREAM UART0 TX)");
    CHECK(allowed(5), "GPIO5 is allowed (UPSTREAM UART0 RX)");

    /* GPIO6/GPIO7: not bonded out on the SuperMini/SiP package -- rejected. */
    CHECK(!allowed(6), "GPIO6 is rejected (not bonded out on ESP32-H2 SuperMini/SiP package)");
    CHECK(!allowed(7), "GPIO7 is rejected (not bonded out on ESP32-H2 SuperMini/SiP package)");
    CHECK(strcmp(gpio_profile_reserved_reason(6), "not bonded out on ESP32-H2 SuperMini/SiP package") == 0,
          "GPIO6's rejection reason names the SuperMini/SiP package specifically");

    /* GPIO8/9/25: boot strapping -- rejected. */
    CHECK(!allowed(8), "GPIO8 is rejected (boot strapping)");
    CHECK(!allowed(9), "GPIO9 is rejected (boot strapping)");
    CHECK(!allowed(25), "GPIO25 is rejected (boot strapping)");
    CHECK(strcmp(gpio_profile_reserved_reason(8), "boot strapping pin") == 0,
          "GPIO8's rejection reason is specifically \"boot strapping pin\"");

    /* GPIO15..21: internal SPI flash -- rejected, every value in the range. */
    for (int32_t gpio = 15; gpio <= 21; gpio++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "GPIO%d is rejected (internal SPI flash)", (int)gpio);
        CHECK(!allowed(gpio), msg);
    }
    CHECK(strcmp(gpio_profile_reserved_reason(15), "internal SPI flash pin") == 0,
          "GPIO15's rejection reason is specifically \"internal SPI flash pin\"");

    /* GPIO26/27: USB Serial/JTAG -- rejected. */
    CHECK(!allowed(26), "GPIO26 is rejected (USB Serial/JTAG)");
    CHECK(!allowed(27), "GPIO27 is rejected (USB Serial/JTAG)");

    /* GPIO13: NOT rejected by the chip-level profile (board-level LED avoidance
     * is a separate, documented decision in the configs/ JSON files, not enforced here). */
    CHECK(allowed(13), "GPIO13 is NOT rejected by the chip-level profile (board-level LED avoidance is separate)");

    /* GPIO0/1 (I2C) and GPIO3/10/11/12/14/23 (the rest of the confirmed
     * profile) must all be plain, allowed GPIOs. DOWNSTREAM DETECT_OUT moved
     * from GPIO22 to GPIO3 in the header-accessibility follow-up (GPIO22 is
     * not a convenient header pin on the real ESP32-H2 SuperMini board and
     * required direct soldering with board-damage risk) -- GPIO22 itself
     * remains ALLOWED by this chip-level policy (never reserved), it is
     * simply no longer part of the confirmed profile's active pin set; see
     * docs/pogo-interface.md's GPIO history section. */
    int32_t profile_gpios[] = {0, 1, 3, 10, 11, 12, 14, 23};
    bool all_profile_allowed = true;
    for (size_t i = 0; i < sizeof(profile_gpios) / sizeof(profile_gpios[0]); i++) {
        if (!allowed(profile_gpios[i])) {
            all_profile_allowed = false;
        }
    }
    CHECK(all_profile_allowed, "every GPIO the confirmed profile actually uses (0,1,3,10,11,12,14,23) is allowed");

    /* Out-of-range values. */
    CHECK(!gpio_profile_in_range(28), "GPIO28 is out of range");
    CHECK(!gpio_profile_in_range(-2), "-2 is out of range (only -1 means \"not present\")");

    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
