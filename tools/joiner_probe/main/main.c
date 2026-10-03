/* Bench-only ESP32-H2 Thread Joiner probe. Never flash onto the operating
 * Power Base: this firmware has none of the POGO/bridge application logic. */
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_console.h"
#include "esp_openthread.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_vfs_eventfd.h"
#include "nvs_flash.h"

void app_main(void)
{
    esp_vfs_eventfd_config_t eventfd = {.max_fds = 3};
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd));

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "joiner>";
    esp_console_dev_usb_serial_jtag_config_t usb_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_console_repl_t *repl = NULL;
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usb_config, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    static esp_openthread_config_t config = {
        .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
        .platform_config = {
            .radio_config = {.radio_mode = RADIO_MODE_NATIVE},
            .host_config = {.host_connection_mode = HOST_CONNECTION_MODE_NONE},
            .port_config = {.storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10},
        },
    };
    ESP_ERROR_CHECK(esp_openthread_start(&config));
    /* No auto-start: the operator explicitly invokes Joiner in the CLI. */
}
