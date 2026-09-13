#include "thread_manager/thread_manager.h"

#include <string.h>
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if !SOC_IEEE802154_SUPPORTED
#error "thread_manager requires a target with a native IEEE 802.15.4 radio (ESP32-H2/C6/...)"
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
#include "freertos/queue.h"
#include "openthread/ip6.h"
#include "openthread/thread_ftd.h" /* otThreadSetRouterEligible() -- module always builds FTD (see sdkconfig.defaults) */

static const char *TAG = "thread_manager";

#define OT_LOCK_WAIT pdMS_TO_TICKS(1000)

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

esp_err_t thread_manager_init(void)
{
    s_role_change_queue = xQueueCreate(4, sizeof(otDeviceRole));
    if (s_role_change_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

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
        ESP_LOGE(TAG, "failed to acquire OpenThread lock during init");
        return ESP_ERR_TIMEOUT;
    }
    otSetStateChangedCallback(instance, ot_state_changed_cb, instance);
    esp_openthread_lock_release();

    ESP_LOGI(TAG, "OpenThread FTD stack started (Thread not yet enabled -- waiting for a role)");
    return ESP_OK;
}

esp_err_t thread_manager_stop(void)
{
    otInstance *instance = esp_openthread_get_instance();
    if (!esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        return ESP_ERR_TIMEOUT;
    }
    otError e1 = otThreadSetEnabled(instance, false);
    otError e2 = otIp6SetEnabled(instance, false);
    esp_openthread_lock_release();

    if (e1 != OT_ERROR_NONE || e2 != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "thread_manager_stop: otThreadSetEnabled=%d otIp6SetEnabled=%d", e1, e2);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Thread disabled/detached");
    return ESP_OK;
}

esp_err_t thread_manager_start_as_root(void)
{
    otInstance *instance = esp_openthread_get_instance();
    if (!esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_OK;
    do {
        /* Thread must be disabled before link mode / router eligibility change. */
        if (otThreadSetEnabled(instance, false) != OT_ERROR_NONE) {
            ESP_LOGE(TAG, "ROOT: otThreadSetEnabled(false) failed");
            result = ESP_FAIL;
            break;
        }
        if (otIp6SetEnabled(instance, false) != OT_ERROR_NONE) {
            ESP_LOGE(TAG, "ROOT: otIp6SetEnabled(false) failed");
            result = ESP_FAIL;
            break;
        }

        otLinkModeConfig link_mode = {.mRxOnWhenIdle = true, .mDeviceType = true, .mNetworkData = true};
        if (otThreadSetLinkMode(instance, link_mode) != OT_ERROR_NONE) {
            ESP_LOGE(TAG, "ROOT: otThreadSetLinkMode failed");
            result = ESP_FAIL;
            break;
        }
        if (otThreadSetRouterEligible(instance, true) != OT_ERROR_NONE) {
            ESP_LOGE(TAG, "ROOT: otThreadSetRouterEligible(true) failed");
            result = ESP_FAIL;
            break;
        }

        if (!otDatasetIsCommissioned(instance)) {
            ESP_LOGI(TAG, "ROOT: no dataset in NVS -- forming a new Thread network from Kconfig defaults");
            result = esp_openthread_auto_start(NULL);
        } else {
            ESP_LOGI(TAG, "ROOT: existing dataset found in NVS -- resuming as Leader/Router");
            if (otIp6SetEnabled(instance, true) != OT_ERROR_NONE) {
                ESP_LOGE(TAG, "ROOT: otIp6SetEnabled(true) failed");
                result = ESP_FAIL;
                break;
            }
            if (otThreadSetEnabled(instance, true) != OT_ERROR_NONE) {
                ESP_LOGE(TAG, "ROOT: otThreadSetEnabled(true) failed");
                result = ESP_FAIL;
                break;
            }
        }
    } while (0);

    esp_openthread_lock_release();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "ROOT: failed to start Thread network");
    }
    return result;
}

esp_err_t thread_manager_join_with_dataset(thread_manager_role_t role, const uint8_t *dataset_tlvs,
                                            uint16_t dataset_len, uint32_t poll_period_ms)
{
    otOperationalDatasetTlvs tlvs;
    if (dataset_len > OT_OPERATIONAL_DATASET_MAX_LENGTH) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&tlvs, 0, sizeof(tlvs));
    memcpy(tlvs.mTlvs, dataset_tlvs, dataset_len);
    tlvs.mLength = (uint8_t)dataset_len;

    otInstance *instance = esp_openthread_get_instance();
    if (!esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_OK;
    do {
        if (otThreadSetEnabled(instance, false) != OT_ERROR_NONE) {
            ESP_LOGE(TAG, "otThreadSetEnabled(false) failed");
            result = ESP_FAIL;
            break;
        }
        if (otIp6SetEnabled(instance, false) != OT_ERROR_NONE) {
            ESP_LOGE(TAG, "otIp6SetEnabled(false) failed");
            result = ESP_FAIL;
            break;
        }

        otLinkModeConfig link_mode;
        bool router_eligible;
        if (role == THREAD_MANAGER_ROLE_LEAF) {
            link_mode.mRxOnWhenIdle = false; /* SED: radio off between polls */
            link_mode.mDeviceType = false;   /* MTD-like behavior, never promoted to Router */
            link_mode.mNetworkData = false;  /* stable network data only */
            router_eligible = false;
        } else {
            /* INTERMEDIATE: full FTD behavior, router-eligible, joins the
             * same network handed down from upstream -- never forms its own. */
            link_mode.mRxOnWhenIdle = true;
            link_mode.mDeviceType = true;
            link_mode.mNetworkData = true;
            router_eligible = true;
        }
        if (otThreadSetLinkMode(instance, link_mode) != OT_ERROR_NONE) {
            ESP_LOGE(TAG, "otThreadSetLinkMode failed");
            result = ESP_FAIL;
            break;
        }
        if (otThreadSetRouterEligible(instance, router_eligible) != OT_ERROR_NONE) {
            ESP_LOGE(TAG, "otThreadSetRouterEligible(%d) failed", router_eligible);
            result = ESP_FAIL;
            break;
        }
        if (role == THREAD_MANAGER_ROLE_LEAF) {
            uint32_t period = (poll_period_ms > 0) ? poll_period_ms : CONFIG_THREAD_MANAGER_DEFAULT_LEAF_POLL_PERIOD_MS;
            if (otLinkSetPollPeriod(instance, period) != OT_ERROR_NONE) {
                ESP_LOGE(TAG, "otLinkSetPollPeriod(%u) failed", (unsigned)period);
                result = ESP_FAIL;
                break;
            }
            ESP_LOGI(TAG, "LEAF link mode: router_eligible=false rx-on-when-idle=false poll_period=%ums", period);
        } else {
            ESP_LOGI(TAG, "INTERMEDIATE link mode: router_eligible=true full FTD");
        }

        result = esp_openthread_auto_start(&tlvs);
    } while (0);

    esp_openthread_lock_release();

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "failed to apply dataset / start Thread join");
    } else {
        /* Dataset TLVs are never logged: they contain the Network Key. */
        ESP_LOGI(TAG, "dataset applied (%u bytes) -- Thread join in progress", (unsigned)dataset_len);
    }
    return result;
}

esp_err_t thread_manager_get_active_dataset_tlvs(otOperationalDatasetTlvs *out_tlvs)
{
    otInstance *instance = esp_openthread_get_instance();
    if (!esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        return ESP_ERR_TIMEOUT;
    }
    otError err = otDatasetGetActiveTlvs(instance, out_tlvs);
    esp_openthread_lock_release();
    return (err == OT_ERROR_NONE) ? ESP_OK : ESP_FAIL;
}

bool thread_manager_validate_dataset_tlvs(const uint8_t *dataset_tlvs, uint16_t dataset_len)
{
    if (dataset_len > OT_OPERATIONAL_DATASET_MAX_LENGTH) {
        return false;
    }
    otOperationalDatasetTlvs tlvs;
    memset(&tlvs, 0, sizeof(tlvs));
    memcpy(tlvs.mTlvs, dataset_tlvs, dataset_len);
    tlvs.mLength = (uint8_t)dataset_len;

    if (!esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        return false;
    }
    bool valid = otDatasetIsValid(&tlvs, /*aActive=*/true);
    esp_openthread_lock_release();
    return valid;
}

esp_err_t thread_manager_get_leader_address(otIp6Address *out_addr)
{
    otInstance *instance = esp_openthread_get_instance();
    if (!esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        return ESP_ERR_TIMEOUT;
    }
    otError err = otThreadGetLeaderRloc(instance, out_addr);
    esp_openthread_lock_release();
    if (err != OT_ERROR_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

otDeviceRole thread_manager_get_role(void)
{
    otInstance *instance = esp_openthread_get_instance();
    otDeviceRole role = OT_DEVICE_ROLE_DISABLED;
    if (esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        role = otThreadGetDeviceRole(instance);
        esp_openthread_lock_release();
    }
    return role;
}

bool thread_manager_poll_role_change(otDeviceRole *out_role)
{
    return xQueueReceive(s_role_change_queue, out_role, 0) == pdTRUE;
}
