#include "upper_thread.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if !SOC_IEEE802154_SUPPORTED
#error "upper_thread requires a target with a native IEEE 802.15.4 radio (ESP32-H2/C6/...)"
#endif

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "openthread/ip6.h"

static const char *TAG = "upper_thread";

#define OT_LOCK_WAIT pdMS_TO_TICKS(1000)

esp_err_t upper_thread_init(void)
{
    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = 3, /* netif, ot task queue, radio driver -- see esp-idf openthread examples */
    };
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

    static esp_openthread_config_t config = {
        .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
        .platform_config =
            {
                .radio_config = {.radio_mode = RADIO_MODE_NATIVE},
                .host_config = {.host_connection_mode = HOST_CONNECTION_MODE_NONE},
                .port_config =
                    {
                        .storage_partition_name = "nvs",
                        .netif_queue_size = 10,
                        .task_queue_size = 10,
                    },
            },
    };

    esp_err_t err = esp_openthread_start(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_openthread_start failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_netif_set_default_netif(esp_openthread_get_netif());

    otInstance *instance = esp_openthread_get_instance();
    if (!esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        ESP_LOGE(TAG, "failed to acquire OpenThread lock during network setup");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_OK;
    if (!otDatasetIsCommissioned(instance)) {
        ESP_LOGI(TAG, "no dataset in NVS -- forming a new Thread network from Kconfig defaults");
        result = esp_openthread_auto_start(NULL);
    } else {
        ESP_LOGI(TAG, "existing dataset found in NVS -- resuming as Leader/Router");
        if (otIp6SetEnabled(instance, true) != OT_ERROR_NONE || otThreadSetEnabled(instance, true) != OT_ERROR_NONE) {
            result = ESP_FAIL;
        }
    }

    esp_openthread_lock_release();

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "failed to start Thread network");
    }
    return result;
}

esp_err_t upper_thread_get_active_dataset_tlvs(otOperationalDatasetTlvs *out_tlvs)
{
    otInstance *instance = esp_openthread_get_instance();
    if (!esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        return ESP_ERR_TIMEOUT;
    }
    otError err = otDatasetGetActiveTlvs(instance, out_tlvs);
    esp_openthread_lock_release();
    return (err == OT_ERROR_NONE) ? ESP_OK : ESP_FAIL;
}

otDeviceRole upper_thread_get_role(void)
{
    otInstance *instance = esp_openthread_get_instance();
    otDeviceRole role = OT_DEVICE_ROLE_DISABLED;
    if (esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        role = otThreadGetDeviceRole(instance);
        esp_openthread_lock_release();
    }
    return role;
}
