#include "module_config/gpio_profile.h"

#include <stddef.h>

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
