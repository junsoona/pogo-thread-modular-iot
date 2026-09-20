#include "thread_manager/thread_manager.h"

#include <stdio.h>
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
#include "freertos/task.h"
#include "openthread/ip6.h"
#include "openthread/thread_ftd.h" /* otThreadSetRouterEligible() -- module always builds FTD (see sdkconfig.defaults) */

static const char *TAG = "thread_manager";

#define OT_LOCK_WAIT pdMS_TO_TICKS(1000)

static QueueHandle_t s_role_change_queue;

/* "xx:xx:xx:xx:xx:xx:xx:xx\0" */
static void format_ext_addr(const otExtAddress *addr, char out[24])
{
    snprintf(out, 24, "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x", addr->m8[0], addr->m8[1], addr->m8[2], addr->m8[3],
             addr->m8[4], addr->m8[5], addr->m8[6], addr->m8[7]);
}

/*
 * Thread radio multi-hop diagnostic (see docs/validation-status.md's
 * "Thread radio multi-hop routing" section for the full judgement
 * criteria this feeds): logs this device's own Extended Address (so a
 * LEAF's reported parent can be matched against an INTERMEDIATE's own
 * log from a separate real-hardware capture) and, only while actually
 * attached as a Thread CHILD (a LEAF's normal state; an INTERMEDIATE/
 * Power Base run as ROUTER/LEADER and have no parent to report), the
 * radio parent's Extended Address via the real otThreadGetParentInfo() --
 * never POGO/application-layer topology, which `power_base_transport.c`
 * already logs separately and does not by itself prove which physical
 * link a CoAP packet actually radio-relayed over.
 *
 * Called ONLY from inside ot_state_changed_cb() below, itself only
 * invoked on an actual OT_CHANGED_THREAD_ROLE event -- never a polling
 * loop, so this never repeats while nothing has changed.
 */
static void log_role_and_parent(otInstance *instance, otDeviceRole role)
{
    const otExtAddress *own_ext_addr = otLinkGetExtendedAddress(instance);
    char own_str[24] = "unknown";
    if (own_ext_addr != NULL) {
        format_ext_addr(own_ext_addr, own_str);
    }

    if (role == OT_DEVICE_ROLE_CHILD) {
        otRouterInfo parent;
        if (otThreadGetParentInfo(instance, &parent) == OT_ERROR_NONE) {
            char parent_str[24];
            format_ext_addr(&parent.mExtAddress, parent_str);
            ESP_LOGI(TAG, "thread radio: role=%d(%s) own_ext_addr=%s parent_ext_addr=%s parent_rloc16=0x%04x",
                     role, thread_manager_role_to_str(role), own_str, parent_str, parent.mRloc16);
        } else {
            ESP_LOGW(TAG, "thread radio: role=%d(%s) own_ext_addr=%s -- otThreadGetParentInfo failed while CHILD",
                     role, thread_manager_role_to_str(role), own_str);
        }
    } else {
        /* ROUTER/LEADER (INTERMEDIATE, Power Base) have no radio parent;
         * DISABLED/DETACHED have none yet -- own address is still useful
         * so a later real-hardware comparison has it on record. */
        ESP_LOGI(TAG, "thread radio: role=%d(%s) own_ext_addr=%s", role, thread_manager_role_to_str(role), own_str);
    }
}

static void ot_state_changed_cb(otChangedFlags flags, void *context)
{
    if (flags & OT_CHANGED_THREAD_ROLE) {
        /* Called back from inside the OpenThread task itself: per
         * esp_openthread_lock.h, the API lock must NOT be taken again here --
         * every OT API call in this function and log_role_and_parent() above
         * relies on that same already-established rule. */
        otInstance *instance = (otInstance *)context;
        otDeviceRole role = otThreadGetDeviceRole(instance);
        log_role_and_parent(instance, role);
        xQueueSend(s_role_change_queue, &role, 0);
    }
}

/* Guards s_parent_known/s_last_logged_parent below -- read-modify-write from
 * whatever task calls thread_manager_check_parent_for_sample() (currently
 * always sensor_transport's own task, but this stays correct even if a
 * second caller is ever added). Same short-critical-section idiom
 * sensor_transport.c's own s_send_lock already uses for a comparable
 * "shared between callers, compare-then-write" case. */
static portMUX_TYPE s_parent_check_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_parent_known;
static otExtAddress s_last_logged_parent;

void thread_manager_check_parent_for_sample(void)
{
    otInstance *instance = esp_openthread_get_instance();
    if (!esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        return; /* diagnostic only -- never block or fail the caller's real send */
    }
    otDeviceRole role = otThreadGetDeviceRole(instance);
    if (role != OT_DEVICE_ROLE_CHILD) {
        /* Not a LEAF's normal attached state right now -- nothing to
         * re-check (own address + role on ROUTER/LEADER transitions are
         * already covered by ot_state_changed_cb() above). */
        esp_openthread_lock_release();
        return;
    }
    otRouterInfo parent;
    otError err = otThreadGetParentInfo(instance, &parent);
    esp_openthread_lock_release();
    if (err != OT_ERROR_NONE) {
        return;
    }

    bool changed;
    taskENTER_CRITICAL(&s_parent_check_lock);
    changed = !s_parent_known || memcmp(&parent.mExtAddress, &s_last_logged_parent, sizeof(otExtAddress)) != 0;
    if (changed) {
        s_last_logged_parent = parent.mExtAddress;
        s_parent_known = true;
    }
    taskEXIT_CRITICAL(&s_parent_check_lock);

    if (changed) {
        char parent_str[24];
        format_ext_addr(&parent.mExtAddress, parent_str);
        ESP_LOGI(TAG, "thread radio: parent re-check at sample send -- parent_ext_addr=%s parent_rloc16=0x%04x",
                 parent_str, parent.mRloc16);
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

/*
 * Shared by thread_manager_start_as_always_on_ftd(),
 * thread_manager_start_as_root(), and (this follow-up)
 * thread_manager_start_as_always_on_ftd_with_dataset(): disables Thread/IP6
 * (a precondition for changing link mode / router eligibility) and then
 * applies the ONE fixed always-on-FTD link mode every caller in this file
 * uses (rx-on-when-idle=true, full FTD, router-eligible=true). Must be
 * called while already holding the OpenThread lock. `log_tag` is used only
 * for these two steps' own error logs -- each caller still logs its own
 * higher-level outcome afterward.
 */
static esp_err_t disable_thread_and_apply_always_on_ftd_link_mode(otInstance *instance, const char *log_tag)
{
    if (otThreadSetEnabled(instance, false) != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "%s: otThreadSetEnabled(false) failed", log_tag);
        return ESP_FAIL;
    }
    if (otIp6SetEnabled(instance, false) != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "%s: otIp6SetEnabled(false) failed", log_tag);
        return ESP_FAIL;
    }

    otLinkModeConfig link_mode = {.mRxOnWhenIdle = true, .mDeviceType = true, .mNetworkData = true};
    if (otThreadSetLinkMode(instance, link_mode) != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "%s: otThreadSetLinkMode failed", log_tag);
        return ESP_FAIL;
    }
    if (otThreadSetRouterEligible(instance, true) != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "%s: otThreadSetRouterEligible(true) failed", log_tag);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/*
 * Shared implementation behind both thread_manager_start_as_always_on_ftd()
 * and thread_manager_start_as_root() -- the two public names apply the
 * IDENTICAL link-mode/resume-or-form behavior and differ only in the log
 * prefix (`log_tag`), so a real-hardware log can immediately tell which
 * caller's policy is in play ("ROOT:" for the DEV_ROOT regression fixture,
 * "always-on FTD:" for anything that does not itself claim to be a fixed
 * Thread ROOT/Leader -- see thread_manager.h's comments on each).
 */
static esp_err_t start_always_on_ftd_impl(const char *log_tag, const char *resume_suffix)
{
    otInstance *instance = esp_openthread_get_instance();
    if (!esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_OK;
    do {
        result = disable_thread_and_apply_always_on_ftd_link_mode(instance, log_tag);
        if (result != ESP_OK) {
            break;
        }

        if (!otDatasetIsCommissioned(instance)) {
            ESP_LOGI(TAG, "%s: no dataset in NVS -- forming a new Thread network from Kconfig defaults", log_tag);
            result = esp_openthread_auto_start(NULL);
        } else {
            ESP_LOGI(TAG, "%s: existing dataset found in NVS -- resuming%s", log_tag, resume_suffix);
            if (otIp6SetEnabled(instance, true) != OT_ERROR_NONE) {
                ESP_LOGE(TAG, "%s: otIp6SetEnabled(true) failed", log_tag);
                result = ESP_FAIL;
                break;
            }
            if (otThreadSetEnabled(instance, true) != OT_ERROR_NONE) {
                ESP_LOGE(TAG, "%s: otThreadSetEnabled(true) failed", log_tag);
                result = ESP_FAIL;
                break;
            }
        }
    } while (0);

    esp_openthread_lock_release();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "%s: failed to start Thread network", log_tag);
    }
    return result;
}

esp_err_t thread_manager_start_as_always_on_ftd(void)
{
    return start_always_on_ftd_impl("always-on FTD", "");
}

esp_err_t thread_manager_start_as_root(void)
{
    /* Log text unchanged from before this follow-up ("resuming as
     * Leader/Router") -- see this function's own header comment: kept
     * verbatim for the DEV_ROOT regression fixture, never unnecessarily
     * changed by this session. */
    return start_always_on_ftd_impl("ROOT", " as Leader/Router");
}

/*
 * EXTERNAL_OTBR: see thread_manager.h's own comment for the full contract.
 * Never falls back to forming/resuming a standalone network on any failure
 * path -- every `break` below simply returns the error to the caller.
 */
esp_err_t thread_manager_start_as_always_on_ftd_with_dataset(const uint8_t *dataset_tlvs, uint16_t dataset_len)
{
    if (dataset_tlvs == NULL || dataset_len == 0 || dataset_len > OT_OPERATIONAL_DATASET_MAX_LENGTH) {
        ESP_LOGE(TAG, "external OTBR: rejecting dataset (tlvs=%s len=%u) -- refusing to start",
                 (dataset_tlvs == NULL) ? "NULL" : "non-NULL", (unsigned)dataset_len);
        return ESP_ERR_INVALID_ARG;
    }

    otOperationalDatasetTlvs tlvs;
    memset(&tlvs, 0, sizeof(tlvs));
    memcpy(tlvs.mTlvs, dataset_tlvs, dataset_len);
    tlvs.mLength = (uint8_t)dataset_len;

    otInstance *instance = esp_openthread_get_instance();
    if (!esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_OK;
    do {
        /* Real OpenThread semantic validation -- malformed TLVs or missing
         * mandatory fields are caught here, not just re-checked for length
         * (the loader that produced dataset_tlvs, components/
         * power_base_thread_config, only ever checks storage-level size). */
        if (!otDatasetIsValid(&tlvs, /*aActive=*/true)) {
            ESP_LOGE(TAG, "external OTBR: dataset failed otDatasetIsValid() -- refusing to join (no standalone "
                          "fallback)");
            result = ESP_ERR_INVALID_ARG;
            break;
        }

        result = disable_thread_and_apply_always_on_ftd_link_mode(instance, "external OTBR");
        if (result != ESP_OK) {
            break;
        }

        result = esp_openthread_auto_start(&tlvs);
    } while (0);

    esp_openthread_lock_release();

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "external OTBR: failed to join existing Thread network -- halting is the caller's "
                      "responsibility (no standalone fallback here)");
    } else {
        /* Dataset TLVs are never logged: they contain the Network Key. */
        ESP_LOGI(TAG, "external OTBR: Active Dataset accepted (%u bytes) -- joining existing Thread network",
                 (unsigned)dataset_len);
    }
    return result;
}

esp_err_t thread_manager_join_with_dataset(thread_manager_role_t role, const uint8_t *dataset_tlvs,
                                            uint16_t dataset_len, uint32_t thread_data_poll_period_ms)
{
    if (dataset_tlvs == NULL || dataset_len == 0 || dataset_len > OT_OPERATIONAL_DATASET_MAX_LENGTH) {
        ESP_LOGE(TAG, "thread_manager_join_with_dataset: rejecting dataset (tlvs=%s len=%u)",
                 (dataset_tlvs == NULL) ? "NULL" : "non-NULL", (unsigned)dataset_len);
        return ESP_ERR_INVALID_ARG;
    }

    otOperationalDatasetTlvs tlvs;
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
            /* Thread SED MAC data poll period ONLY -- never a sensor
             * measurement/sampling period, see this function's own header
             * comment (thread_manager.h) for why the two must never be
             * coupled again. */
            uint32_t period = (thread_data_poll_period_ms > 0) ? thread_data_poll_period_ms
                                                                 : CONFIG_THREAD_MANAGER_LEAF_DATA_POLL_PERIOD_MS;
            if (otLinkSetPollPeriod(instance, period) != OT_ERROR_NONE) {
                ESP_LOGE(TAG, "otLinkSetPollPeriod(%u) failed", (unsigned)period);
                result = ESP_FAIL;
                break;
            }
            ESP_LOGI(TAG, "LEAF link mode: router_eligible=false rx-on-when-idle=false "
                          "thread_data_poll_period_ms=%u (independent of this module's own sensor measurement "
                          "period)",
                     (unsigned)period);
        } else {
            ESP_LOGI(TAG, "INTERMEDIATE link mode: router_eligible=true full FTD");
        }

        result = esp_openthread_auto_start(&tlvs);
        /*
         * Diagnostic for the still-unresolved THREAD_JOIN_TIMEOUT
         * investigation (see README.md/docs/state-machine.md): a real
         * 3-tier chain test once observed the FIRST provisioning attempt's
         * call reach here with every individual OT API above returning
         * OT_ERROR_NONE (esp_openthread_auto_start() itself wraps
         * otDatasetSetActiveTlvs()/otIp6SetEnabled(true)/otThreadSetEnabled(true),
         * each already ESP_LOGE'd above on its own failure -- see ESP-IDF's
         * esp_openthread_auto_start()), yet OpenThread's own internal
         * attach state machine produced no "Attach attempt" log for the
         * whole CONFIG_POGO_JOIN_TIMEOUT_MS window; a SECOND attempt (a
         * fresh disable/re-enable cycle through this same function)
         * attached normally ~240ms later. Logging otDeviceRole here --
         * synchronously, still under the OT lock, immediately after
         * otThreadSetEnabled(true) itself returned success -- costs nothing
         * in the steady state (this function only ever runs once per real
         * join attempt, never in a polling loop) and lets a future real
         * test directly compare this snapshot across a failing vs.
         * succeeding attempt instead of guessing from the attach outcome
         * alone.
         */
        if (result == ESP_OK) {
            otDeviceRole role_now = otThreadGetDeviceRole(instance);
            ESP_LOGI(TAG, "esp_openthread_auto_start OK -- otDeviceRole immediately after enable: %d(%s)",
                     role_now, thread_manager_role_to_str(role_now));
        }
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
    if (dataset_tlvs == NULL || dataset_len == 0 || dataset_len > OT_OPERATIONAL_DATASET_MAX_LENGTH) {
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

esp_err_t thread_manager_get_mesh_local_eid(otIp6Address *out_addr)
{
    otInstance *instance = esp_openthread_get_instance();
    if (!esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        return ESP_ERR_TIMEOUT;
    }
    const otIp6Address *eid = otThreadGetMeshLocalEid(instance);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (eid != NULL) {
        /* Copy out WHILE still holding the lock -- `eid` points into
         * OpenThread's own internal state and must not be dereferenced
         * after esp_openthread_lock_release() below. */
        *out_addr = *eid;
        result = ESP_OK;
    }
    esp_openthread_lock_release();
    return result;
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

const char *thread_manager_role_to_str(otDeviceRole role)
{
    switch (role) {
    case OT_DEVICE_ROLE_DISABLED:
        return "DISABLED";
    case OT_DEVICE_ROLE_DETACHED:
        return "DETACHED";
    case OT_DEVICE_ROLE_CHILD:
        return "CHILD";
    case OT_DEVICE_ROLE_ROUTER:
        return "ROUTER";
    case OT_DEVICE_ROLE_LEADER:
        return "LEADER";
    default:
        return "UNKNOWN";
    }
}
