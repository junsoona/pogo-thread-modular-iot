/*
 * BOOT-button input for the local re-enrollment of a Power Base
 * (pb_thread:mode=2 + BLE build only) -- see docs/power-base.md "BOOT 버튼
 * 로컬 재등록".
 *
 * This file decides only; it touches no GPIO. power_base/main samples the
 * pin from a task (never from an ISR) and feeds each sample to
 * pb_reenroll_button_step(), so the same code runs unmodified in
 * tools/test_power_base_reenroll.c.
 *
 * What counts as the request ("one long press"):
 *   1. after boot, a stable release must be seen first. A button that is
 *      down from boot on -- held, stuck, shorted -- never counts, however
 *      long it stays down;
 *   2. then one continuous press of at least PB_REENROLL_BTN_HOLD_MS;
 *   3. then a stable release. Only this release reports the request, once.
 *      Nothing happens while the button is still down, so the chip is never
 *      restarted with BOOT held (which would select the ROM download mode).
 * A press shorter than the hold time, a press longer than
 * PB_REENROLL_BTN_MAX_HOLD_MS (taken for a stuck button) and a press that
 * was not sampled continuously are dropped, and a stable release is required
 * again before anything else can count.
 *
 * Contact bounce: a level counts once it has been the same for
 * PB_REENROLL_BTN_DEBOUNCE_MS, so a bounce shorter than that neither ends a
 * press nor starts one.
 *
 * Time is a free-running uint32_t millisecond counter; only differences are
 * used, so a wrap of the counter changes nothing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How often power_base/main samples the pin. */
#define PB_REENROLL_BTN_POLL_MS 20u
#define PB_REENROLL_BTN_DEBOUNCE_MS 40u
/* A release counts as stable after this long without a press. */
#define PB_REENROLL_BTN_RELEASE_STABLE_MS 300u
#define PB_REENROLL_BTN_HOLD_MS 5000u
/* Held longer than this: not a request (a stuck or shorted button). */
#define PB_REENROLL_BTN_MAX_HOLD_MS 15000u
/* Two samples further apart than this say nothing about the time between
 * them: whatever was in progress is dropped. */
#define PB_REENROLL_BTN_MAX_SAMPLE_GAP_MS 250u

typedef enum {
    /* A stable release has not been seen yet (boot, or after anything that
     * did not count). */
    PB_REENROLL_BTN_WAIT_RELEASE = 0,
    PB_REENROLL_BTN_ARMED,
    PB_REENROLL_BTN_PRESSING,  /* down, for less than the hold time */
    PB_REENROLL_BTN_HELD,      /* down for the hold time: waits for the release */
    PB_REENROLL_BTN_RELEASING, /* released after HELD, not yet stable */
} pb_reenroll_button_state_t;

typedef struct {
    pb_reenroll_button_state_t state;
    bool started;
    uint32_t last_ms;       /* time of the previous sample */
    bool raw;               /* the pin as last sampled */
    uint32_t raw_since_ms;  /* when `raw` last changed */
    bool level;             /* debounced */
    uint32_t press_start_ms;
    uint32_t requests;      /* how many requests were reported (tests, logs) */
} pb_reenroll_button_t;

void pb_reenroll_button_init(pb_reenroll_button_t *b);

/* One sample. `pressed` is the logical level (the caller applies the active
 * level). Returns true exactly once per completed request: on the sample at
 * which the release after a qualifying press became stable. */
bool pb_reenroll_button_step(pb_reenroll_button_t *b, bool pressed, uint32_t now_ms);

/* ---- which GPIO may be the button ---- */

/* The pins this board already uses (power_base_config); -1 = not used. */
typedef struct {
    int32_t uart_tx_gpio;
    int32_t uart_rx_gpio;
    int32_t detect_in_gpio;
    int32_t detect_out_gpio;
    int32_t power_enable_gpio;
} pb_reenroll_board_pins_t;

typedef enum {
    PB_REENROLL_GPIO_OK = 0,
    PB_REENROLL_GPIO_OUT_OF_RANGE, /* not 0..27 (-1 is "no button": not usable either) */
    /* Flash, USB-Serial/JTAG, not bonded out, or a strapping pin other than
     * GPIO9 (components/module_config/gpio_profile.h). */
    PB_REENROLL_GPIO_RESERVED,
    PB_REENROLL_GPIO_CONFLICT_UART_TX,
    PB_REENROLL_GPIO_CONFLICT_UART_RX,
    PB_REENROLL_GPIO_CONFLICT_DETECT_IN,
    PB_REENROLL_GPIO_CONFLICT_DETECT_OUT,
    PB_REENROLL_GPIO_CONFLICT_POWER_ENABLE,
} pb_reenroll_gpio_check_t;

/* The ESP32-H2 BOOT strapping pin. It is sampled by the ROM at reset only;
 * afterwards it is an ordinary input, which is the only way this feature
 * uses it. */
#define PB_REENROLL_BTN_BOOT_STRAPPING_GPIO 9

/* Anything but OK: the feature must stay off for this boot. */
pb_reenroll_gpio_check_t pb_reenroll_button_gpio_check(int32_t gpio, const pb_reenroll_board_pins_t *pins);
const char *pb_reenroll_gpio_check_name(pb_reenroll_gpio_check_t check);

#ifdef __cplusplus
}
#endif
