/*
 * Upper module (POGO host / Thread Leader) entry point.
 *
 * Boot sequence:
 *   1. nvs_flash_init()            -- OpenThread stores its dataset here.
 *   2. pogo_transport_init(NULL)   -- POGO UART, pins/baud from Kconfig.
 *   3. upper_thread_init()         -- start OpenThread as FTD, form/resume
 *                                      the Thread network as Leader.
 *   4. upper_pairing_start()       -- pairing state machine task (PING-based
 *                                      dock detection through to JOINED).
 */
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "pogo_proto/pogo_crc16.h"
#include "pogo_proto/pogo_transport.h"

#include "upper_pairing.h"
#include "upper_thread.h"

static const char *TAG = "upper_main";

void app_main(void)
{
    if (!pogo_crc16_selftest()) {
        ESP_LOGE(TAG, "CRC16 self-test failed -- refusing to start");
        abort();
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(pogo_transport_init(NULL));
    ESP_ERROR_CHECK(upper_thread_init());

    ESP_LOGI(TAG, "upper module ready -- waiting for POGO dock (PING/PONG)");
    upper_pairing_start();
}
