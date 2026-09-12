#include "lower_thread.h"

#include <string.h>
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if !SOC_IEEE802154_SUPPORTED
#error "lower_thread requires a target with a native IEEE 802.15.4 radio (ESP32-H2/C6/...)"
#endif

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_vfs_eventfd.h"
#include "openthread/ip6.h"

static const char *TAG = "lower_thread";

#define OT_LOCK_WAIT pdMS_TO_TICKS(1000)

/* Sleepy End Device data-poll interval. Not exposed via Kconfig (unlike the
 * POGO UART parameters) because it is a Thread radio tuning knob, not part
 * of the wired provisioning link this MVP focuses on. */
#define LOWER_SED_POLL_PERIOD_MS 3000

static QueueHandle_t s_role_change_queue;

static void ot_state_changed_cb(otChangedFlags flags, void *context)
{
    if (flags & OT_CHANGED_THREAD_ROLE) {
        /* Called back from inside the OpenThread task itself: per
         * esp_openthread_lock.h, the API lock must NOT be taken again here. */
        otDeviceRole role = otThreadGetDeviceRole((otInstance *)context);
        xQueueSend(s_role_change_queue, &role, 0);
    }
}

esp_err_t lower_thread_init(void)
{
    s_role_change_queue = xQueueCreate(4, sizeof(otDeviceRole));
    if (s_role_change_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = 3,
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
        ESP_LOGE(TAG, "failed to acquire OpenThread lock during SED setup");
        return ESP_ERR_TIMEOUT;
    }

    otLinkModeConfig link_mode = {0};
    link_mode.mRxOnWhenIdle = false; /* SED: radio off between polls */
    link_mode.mDeviceType = false;   /* MTD, never promoted to Router */
    link_mode.mNetworkData = false;  /* stable network data only */

    esp_err_t result = ESP_OK;
    if (otLinkSetPollPeriod(instance, LOWER_SED_POLL_PERIOD_MS) != OT_ERROR_NONE ||
        otThreadSetLinkMode(instance, link_mode) != OT_ERROR_NONE) {
        result = ESP_FAIL;
    }
    if (result == ESP_OK) {
        otSetStateChangedCallback(instance, ot_state_changed_cb, instance);
    }

    esp_openthread_lock_release();

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "failed to configure SED link mode");
    } else {
        ESP_LOGI(TAG, "OpenThread MTD/SED ready (poll period %dms)", LOWER_SED_POLL_PERIOD_MS);
    }
    return result;
}

static bool bytes_to_tlvs(const uint8_t *dataset_tlvs, uint16_t dataset_len, otOperationalDatasetTlvs *out)
{
    if (dataset_len > OT_OPERATIONAL_DATASET_MAX_LENGTH) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->mTlvs, dataset_tlvs, dataset_len);
    out->mLength = (uint8_t)dataset_len;
    return true;
}

bool lower_thread_validate_dataset_tlvs(const uint8_t *dataset_tlvs, uint16_t dataset_len)
{
    otOperationalDatasetTlvs tlvs;
    if (!bytes_to_tlvs(dataset_tlvs, dataset_len, &tlvs)) {
        return false;
    }
    /* otDatasetIsValid() only parses the TLV structure -- it does not touch
     * otInstance state, but esp_openthread_lock.h documents the lock
     * requirement for "every OpenThread API that takes an otInstance
     * argument", so we take it here too for any two calls that race. */
    if (!esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        return false;
    }
    bool valid = otDatasetIsValid(&tlvs, /*aActive=*/true);
    esp_openthread_lock_release();
    return valid;
}

esp_err_t lower_thread_apply_dataset_and_join(const uint8_t *dataset_tlvs, uint16_t dataset_len)
{
    otOperationalDatasetTlvs tlvs;
    if (!bytes_to_tlvs(dataset_tlvs, dataset_len, &tlvs)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t result = esp_openthread_auto_start(&tlvs);
    esp_openthread_lock_release();
    return result;
}

otDeviceRole lower_thread_get_role(void)
{
    otInstance *instance = esp_openthread_get_instance();
    otDeviceRole role = OT_DEVICE_ROLE_DISABLED;
    if (esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        role = otThreadGetDeviceRole(instance);
        esp_openthread_lock_release();
    }
    return role;
}

bool lower_thread_poll_role_change(otDeviceRole *out_role)
{
    return xQueueReceive(s_role_change_queue, out_role, 0) == pdTRUE;
}
