#include "power_base_reenroll/pb_reenroll_button.h"

#include <stddef.h>
#include <string.h>
#include "module_config/gpio_profile.h"

void pb_reenroll_button_init(pb_reenroll_button_t *b)
{
    memset(b, 0, sizeof(*b));
    b->state = PB_REENROLL_BTN_WAIT_RELEASE;
}

/* Nothing that was in progress counts any more; a stable release comes
 * first again. */
static void start_over(pb_reenroll_button_t *b, bool pressed, uint32_t now_ms)
{
    b->raw = pressed;
    b->raw_since_ms = now_ms;
    b->level = pressed;
    b->state = PB_REENROLL_BTN_WAIT_RELEASE;
}

bool pb_reenroll_button_step(pb_reenroll_button_t *b, bool pressed, uint32_t now_ms)
{
    if (!b->started) {
        b->started = true;
        b->last_ms = now_ms;
        start_over(b, pressed, now_ms);
        return false;
    }
    /* Unsigned differences: a wrap of the counter is just another step. */
    const uint32_t gap_ms = now_ms - b->last_ms;
    b->last_ms = now_ms;
    if (gap_ms > PB_REENROLL_BTN_MAX_SAMPLE_GAP_MS) {
        start_over(b, pressed, now_ms);
        return false;
    }

    if (pressed != b->raw) {
        b->raw = pressed;
        b->raw_since_ms = now_ms;
    }
    const uint32_t stable_ms = now_ms - b->raw_since_ms;
    bool level_changed = false;
    if (b->raw != b->level && stable_ms >= PB_REENROLL_BTN_DEBOUNCE_MS) {
        b->level = b->raw;
        level_changed = true;
    }
    const bool released_stable = !b->raw && stable_ms >= PB_REENROLL_BTN_RELEASE_STABLE_MS;

    switch (b->state) {
    case PB_REENROLL_BTN_WAIT_RELEASE:
        if (released_stable) {
            b->level = false;
            b->state = PB_REENROLL_BTN_ARMED;
        }
        break;
    case PB_REENROLL_BTN_ARMED:
        if (b->level && level_changed) {
            b->state = PB_REENROLL_BTN_PRESSING;
            b->press_start_ms = b->raw_since_ms; /* the edge, not the end of the debounce */
        }
        break;
    case PB_REENROLL_BTN_PRESSING:
        if (!b->level) {
            b->state = PB_REENROLL_BTN_ARMED; /* a short press */
        } else if (b->raw && (now_ms - b->press_start_ms) >= PB_REENROLL_BTN_HOLD_MS) {
            /* Only a sample that still sees the button down promotes: a
             * release whose debounce is still running does not. */
            b->state = PB_REENROLL_BTN_HELD;
        }
        break;
    case PB_REENROLL_BTN_HELD:
        if (!b->level) {
            b->state = PB_REENROLL_BTN_RELEASING;
        } else if ((now_ms - b->press_start_ms) > PB_REENROLL_BTN_MAX_HOLD_MS) {
            b->state = PB_REENROLL_BTN_WAIT_RELEASE; /* stuck: not a request */
        }
        break;
    case PB_REENROLL_BTN_RELEASING:
        if (b->level) {
            b->state = PB_REENROLL_BTN_WAIT_RELEASE; /* down again before the release settled */
        } else if (released_stable) {
            b->state = PB_REENROLL_BTN_ARMED;
            b->requests++;
            return true;
        }
        break;
    default:
        start_over(b, pressed, now_ms);
        break;
    }
    return false;
}

pb_reenroll_gpio_check_t pb_reenroll_button_gpio_check(int32_t gpio, const pb_reenroll_board_pins_t *pins)
{
    if (gpio < 0 || !gpio_profile_in_range(gpio)) {
        return PB_REENROLL_GPIO_OUT_OF_RANGE;
    }
    /* The chip profile rejects every strapping pin as an assignable GPIO.
     * The BOOT pin is the one exception here, as an input after boot only. */
    if (gpio != PB_REENROLL_BTN_BOOT_STRAPPING_GPIO && gpio_profile_reserved_reason(gpio) != NULL) {
        return PB_REENROLL_GPIO_RESERVED;
    }
    if (pins == NULL) {
        return PB_REENROLL_GPIO_OK;
    }
    if (gpio == pins->uart_tx_gpio) {
        return PB_REENROLL_GPIO_CONFLICT_UART_TX;
    }
    if (gpio == pins->uart_rx_gpio) {
        return PB_REENROLL_GPIO_CONFLICT_UART_RX;
    }
    if (gpio == pins->detect_in_gpio) {
        return PB_REENROLL_GPIO_CONFLICT_DETECT_IN;
    }
    if (gpio == pins->detect_out_gpio) {
        return PB_REENROLL_GPIO_CONFLICT_DETECT_OUT;
    }
    if (gpio == pins->power_enable_gpio) {
        return PB_REENROLL_GPIO_CONFLICT_POWER_ENABLE;
    }
    return PB_REENROLL_GPIO_OK;
}

const char *pb_reenroll_gpio_check_name(pb_reenroll_gpio_check_t check)
{
    switch (check) {
    case PB_REENROLL_GPIO_OK: return "ok";
    case PB_REENROLL_GPIO_OUT_OF_RANGE: return "out_of_range";
    case PB_REENROLL_GPIO_RESERVED: return "reserved";
    case PB_REENROLL_GPIO_CONFLICT_UART_TX: return "conflict_uart_tx";
    case PB_REENROLL_GPIO_CONFLICT_UART_RX: return "conflict_uart_rx";
    case PB_REENROLL_GPIO_CONFLICT_DETECT_IN: return "conflict_detect_in";
    case PB_REENROLL_GPIO_CONFLICT_DETECT_OUT: return "conflict_detect_out";
    case PB_REENROLL_GPIO_CONFLICT_POWER_ENABLE: return "conflict_power_enable";
    }
    return "unknown";
}
