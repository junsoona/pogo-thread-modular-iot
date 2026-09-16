/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * OpenThread Radio Co-Processor (RCP) Example
 *
 * This example code is in the Public Domain (or CC0-1.0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 *
 * Adapted from ESP-IDF v6.1's official example
 * ($IDF_PATH/examples/openthread/ot_rcp/main/esp_ot_rcp.c) for this
 * repository's Linux OTBR RCP firmware over USB Serial/JTAG -- see
 * docs/otbr-rcp.md. app_main() itself is UNCHANGED from upstream. The
 * external-coexistence (`ot_examples_common`/`esp-qa/coexist-cmd`) and
 * IEEE 802.15.4 debug-console (`cmd_ieee802154_debug`) integrations have
 * been removed here since this project enables neither
 * CONFIG_ESP_COEX_EXTERNAL_COEXIST_ENABLE nor CONFIG_IEEE802154_DEBUG (no
 * external coexistence GPIO wiring, no RCP debug console in this initial
 * version -- see docs/otbr-rcp.md's scope section) -- those three managed
 * components would otherwise need to be pulled in via idf_component.yml for
 * no purpose here (this repository's `module`/`power_base`/`upper`/`lower`
 * projects have no `idf_component.yml` either). The RCP Spinel console
 * feature itself (CONFIG_OPENTHREAD_RCP_SPINEL_CONSOLE, off by default --
 * see rcp/sdkconfig.defaults) is kept intact, since `esp_console` is a
 * core ESP-IDF component with no managed dependency of its own.
 */

#include <stdio.h>
#include <unistd.h>

#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_openthread.h"
#include "esp_ot_config.h"
#include "esp_vfs_eventfd.h"

#if !SOC_IEEE802154_SUPPORTED
#error "RCP is only supported for the SoCs which have IEEE 802.15.4 module"
#endif

#if CONFIG_OPENTHREAD_RCP_SPINEL_CONSOLE
#include "esp_console.h"
#endif

#define TAG "ot_esp_rcp"

extern void otAppNcpInit(otInstance *instance);

void app_main(void)
{
    // Used eventfds:
    // * ot task queue
    // * radio driver
    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = 2,
    };

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

    static esp_openthread_config_t config = {
        .netif_config = {0},
        .platform_config = {
            .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
            .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
            .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
        },
    };

#if CONFIG_OPENTHREAD_RCP_SPINEL_CONSOLE
    esp_console_config_t console_config = ESP_CONSOLE_CONFIG_DEFAULT();
    esp_console_init(&console_config);
    esp_console_register_help_command();
#endif

    ESP_ERROR_CHECK(esp_openthread_start(&config));
}
