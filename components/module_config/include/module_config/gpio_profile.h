/*
 * ESP32-H2 GPIO usability profile -- pure classification logic, split out
 * from module_config.c so it has zero ESP-IDF dependency and can be
 * host-compiled directly for tools/test_gpio_profile.c (section 2 of the
 * GPIO-correction follow-up: "C와 Python의 금지 GPIO 목록이 달라지지
 * 않도록 host test를 추가한다" -- tools/pack_module_config.py's
 * GPIO_MAX/STRAPPING_GPIOS/NOT_BONDED_OUT_GPIOS/FLASH_GPIOS/USB_GPIOS
 * constants MUST match this file's classification exactly, and
 * tools/test_gpio_profile_parity.py checks that mechanically rather than by
 * hand-eyeballing two hardcoded lists).
 *
 * `module_config` is only ever used by the `module` project, which only
 * ever targets ESP32-H2 (see module/sdkconfig.defaults), and the ESP32-H2
 * SuperMini dev boards ARE the final prototype hardware (no separate PCB is
 * planned) -- so this profile is permanent, not a placeholder.
 *
 * Policy (corrected after the initial GPIO-profile pass wrongly classified
 * GPIO2/3 as strapping and missed that GPIO6/7 are not bonded out at all on
 * the SuperMini/SiP package):
 *   - valid range: -1 ("not present on this board") or 0..27.
 *   - boot strapping (reject): GPIO8, GPIO9, GPIO25.
 *   - not bonded out on the ESP32-H2 SuperMini/SiP package at all (reject):
 *     GPIO6, GPIO7 -- the SiP flash model does not expose these, and no
 *     SuperMini header pin carries them either, so no config may ever
 *     assign them regardless of electrical intent.
 *   - internal SPI flash pins (reject): GPIO15..GPIO21.
 *   - native USB D-/D+, i.e. USB-Serial/JTAG (reject): GPIO26, GPIO27.
 *   - GPIO2..GPIO5: ALLOWED as ordinary POGO/sensor GPIOs in this firmware's
 *     current configuration (USB-Serial/JTAG console, no external pad JTAG
 *     enabled) -- NOT strapping pins, not rejected. The one caveat (external
 *     pad JTOG cannot be used at the same time these are repurposed) is
 *     documented, not enforced here -- this firmware never enables pad JTAG
 *     and never touches any JTAG-related eFuse.
 *   - GPIO13: not rejected by this chip-level profile at all -- it is
 *     avoided by the confirmed board profile (configs/prototype_*.json) only
 *     because it is documented as the ESP32-H2 SuperMini's onboard LED pin
 *     (components/pogo_proto/Kconfig), a board-level fact, not a chip-level
 *     restriction.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPIO_PROFILE_MAX 27 /* ESP32-H2 exposes GPIO0..GPIO27 */

/* -1 ("not present") is always allowed; any other value must be in
 * [0, GPIO_PROFILE_MAX]. */
bool gpio_profile_in_range(int32_t gpio);

/*
 * Returns a short, stable reason string if `gpio` is reserved/unusable on
 * this profile, or NULL if it is a plain, assignable GPIO (or -1). The
 * returned strings are used verbatim in CONFIG_ERROR log lines, so they must
 * stay human-readable and MUST NOT change casually (Python mirrors these
 * same four category names for the parity check above).
 */
const char *gpio_profile_reserved_reason(int32_t gpio);

#ifdef __cplusplus
}
#endif
