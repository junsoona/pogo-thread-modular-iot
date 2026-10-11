#include "enrollment_mgmt.h"

#include <string.h>
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "openthread/coap.h"
#include "openthread/dataset.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/message.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

#include "coap_response_timing/coap_response_timing.h"
#include "enrollment_locate_hints.h"
#include "power_base_mgmt/pb_mgmt_client.h"
#include "power_base_ot_store/pb_stored_network.h"
#include "power_base_ot_store/pb_volatile_store.h"
#include "thread_manager/thread_manager.h"

static const char *TAG = "pb_mgmt_rt";

#define OT_LOCK_WAIT pdMS_TO_TICKS(1000)
#define MGMT_ACK_TIMEOUT_MS CONFIG_SENSOR_COAP_ACK_TIMEOUT_MS
/* 5120 until the task also looked for the hub: a locate round adds, when it
 * moves, three copies of the record on top of a poll's own buffers (its hint
 * list is not on the stack). Not measured on a board. */
#define MGMT_TASK_STACK 6144
/* Boot path: how long Thread is given to attach just to deliver the proof. */
#define PROOF_ATTACH_WAIT_MS 60000u
/* The receipt's delivery session: how long Thread is given to attach as a
 * child of whatever router answers. */
#define RECEIPT_ATTACH_WAIT_MS 45000u
#define ATTACH_POLL_MS 200u
/* Lets the last log lines out before esp_restart(). */
#define RESTART_DELAY_MS 300u

static pb_mgmt_client_t s_client;
static pb_mgmt_quiesce_cb_t s_quiesce;
static pb_mgmt_endpoint_moved_cb_t s_endpoint_moved;
static pb_mgmt_locate_allowed_cb_t s_locate_allowed;

/* One request at a time (this file's only caller is one task); the same
 * token/staleness guard as linux_bridge_exporter.c, plus the answer's
 * payload. */
static SemaphoreHandle_t s_done_sem;
static portMUX_TYPE s_xfer_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_token;
static otError s_result;
static bool s_got_message;
static otCoapCode s_code;
static uint8_t s_payload[PB_MGMT_MAX_MESSAGE_LEN];
static size_t s_payload_len;

/* The Active Dataset of the network this board is leaving, read just before
 * the erase and kept in RAM for the receipt's delivery session only
 * (op_receipt_network_up). It holds the Network Key: never logged, wiped when
 * the session ends, gone with the restart that follows every release. */
static otOperationalDatasetTlvs s_session_tlvs;
static bool s_session_tlvs_valid;

static bool role_is_attached(otDeviceRole role)
{
    return role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER;
}

static bool wait_attached(uint32_t limit_ms)
{
    for (uint32_t waited = 0; waited < limit_ms; waited += ATTACH_POLL_MS) {
        if (role_is_attached(thread_manager_get_role())) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(ATTACH_POLL_MS));
    }
    return false;
}

/* Requests go out on OpenThread's application CoAP socket. It is bound by
 * power_base_transport_init() once the services run -- which a boot that only
 * finishes a release never reaches -- and OpenThread refuses to send on an
 * unbound one. Binding it again when it is bound is not an error. */
static bool coap_socket_ready(void)
{
    if (!esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        return false;
    }
    otError err = otCoapStart(esp_openthread_get_instance(), OT_DEFAULT_COAP_PORT);
    esp_openthread_lock_release();
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "the CoAP socket could not be bound (otError=%d)", (int)err);
    }
    return err == OT_ERROR_NONE;
}

/* ---- crypto: PSA only, nothing of our own ---- */

static bool op_hmac(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                    uint8_t out[PB_MGMT_MAC_LEN], void *ctx)
{
    (void)ctx;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, (size_t)key_len * 8u);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_key_id_t id = 0;
    if (psa_import_key(&attr, key, key_len, &id) != PSA_SUCCESS) {
        return false;
    }
    size_t mac_len = 0;
    psa_status_t st = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), data, data_len, out, PB_MGMT_MAC_LEN, &mac_len);
    (void)psa_destroy_key(id);
    return st == PSA_SUCCESS && mac_len == PB_MGMT_MAC_LEN;
}

static bool op_random(uint8_t *buf, size_t len, void *ctx)
{
    (void)ctx;
    return psa_generate_random(buf, len) == PSA_SUCCESS;
}

/* ---- CoAP ---- */

/* OpenThread task, API lock held: copies the answer and wakes the caller.
 * No decision is made here and nothing is written. */
static void response_handler(void *ctx_v, otMessage *message, const otMessageInfo *message_info, otError result)
{
    (void)message_info;
    uint32_t my_token = (uint32_t)(uintptr_t)ctx_v;
    uint8_t payload[PB_MGMT_MAX_MESSAGE_LEN];
    size_t payload_len = 0;
    otCoapCode code = OT_COAP_CODE_EMPTY;
    bool got = false;
    if (result == OT_ERROR_NONE && message != NULL) {
        got = true;
        code = otCoapMessageGetCode(message);
        uint16_t offset = otMessageGetOffset(message);
        uint16_t length = otMessageGetLength(message);
        /* An answer longer than any management message is not one: it is
         * reported as empty rather than cut short. */
        if (length > offset && (size_t)(length - offset) <= sizeof(payload)) {
            payload_len = otMessageRead(message, offset, payload, (uint16_t)(length - offset));
        }
    }

    bool stale;
    taskENTER_CRITICAL(&s_xfer_lock);
    stale = (my_token != s_token);
    if (!stale) {
        s_result = result;
        s_got_message = got;
        s_code = code;
        s_payload_len = payload_len;
        memcpy(s_payload, payload, payload_len);
    }
    taskEXIT_CRITICAL(&s_xfer_lock);
    if (!stale) {
        xSemaphoreGive(s_done_sem);
    }
}

static pb_mgmt_xfer_t op_exchange(const pb_enroll_bridge_endpoint_t *endpoint, const uint8_t *req, size_t req_len,
                                  uint8_t *resp, size_t resp_cap, size_t *resp_len, void *ctx)
{
    (void)ctx;
    *resp_len = 0;
    if (!role_is_attached(thread_manager_get_role())) {
        return PB_MGMT_XFER_NO_ANSWER;
    }

    uint32_t my_token;
    taskENTER_CRITICAL(&s_xfer_lock);
    my_token = ++s_token;
    s_got_message = false;
    s_payload_len = 0;
    taskEXIT_CRITICAL(&s_xfer_lock);
    xSemaphoreTake(s_done_sem, 0);

    otInstance *instance = esp_openthread_get_instance();
    otError err = OT_ERROR_NO_BUFS;
    if (esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        otMessage *message = otCoapNewMessage(instance, NULL);
        if (message != NULL) {
            otCoapMessageInit(message, OT_COAP_TYPE_CONFIRMABLE, OT_COAP_CODE_POST);
            err = otCoapMessageAppendUriPathOptions(message, PB_MGMT_URI_PATH);
            if (err == OT_ERROR_NONE) {
                err = otCoapMessageSetPayloadMarker(message);
            }
            if (err == OT_ERROR_NONE) {
                err = otMessageAppend(message, req, (uint16_t)req_len);
            }
            if (err == OT_ERROR_NONE) {
                otMessageInfo info = {0};
                memcpy(info.mPeerAddr.mFields.m8, endpoint->addr, sizeof(info.mPeerAddr.mFields.m8));
                info.mPeerPort = endpoint->port;
                otCoapTxParameters tx = {
                    .mAckTimeout = MGMT_ACK_TIMEOUT_MS,
                    .mAckRandomFactorNumerator = COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_NUM,
                    .mAckRandomFactorDenominator = COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_DEN,
                    .mMaxRetransmit = 0, /* the client decides about repeating */
                };
                err = otCoapSendRequestWithParameters(instance, message, &info, response_handler,
                                                      (void *)(uintptr_t)my_token, &tx);
            }
            if (err != OT_ERROR_NONE) {
                otMessageFree(message);
            }
        }
        esp_openthread_lock_release();
    } else {
        err = OT_ERROR_BUSY;
    }
    if (err != OT_ERROR_NONE) {
        return PB_MGMT_XFER_NO_ANSWER;
    }

    uint32_t wait_ms = coap_response_timing_wait_ms(MGMT_ACK_TIMEOUT_MS, COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_NUM,
                                                    COAP_RESPONSE_TIMING_ACK_RANDOM_FACTOR_DEN, MGMT_ACK_TIMEOUT_MS);
    if (xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        /* Whatever arrives later belongs to no request. */
        taskENTER_CRITICAL(&s_xfer_lock);
        s_token++;
        taskEXIT_CRITICAL(&s_xfer_lock);
        return PB_MGMT_XFER_NO_ANSWER;
    }

    pb_mgmt_xfer_t out = PB_MGMT_XFER_NO_ANSWER;
    taskENTER_CRITICAL(&s_xfer_lock);
    if (s_result == OT_ERROR_NONE && s_got_message) {
        uint8_t code_class = (uint8_t)(s_code >> 5);
        if (code_class == 2) {
            out = PB_MGMT_XFER_ANSWERED;
            if (s_payload_len <= resp_cap) {
                memcpy(resp, s_payload, s_payload_len);
                *resp_len = s_payload_len;
            }
        } else if (code_class == 4) {
            out = PB_MGMT_XFER_REJECTED;
        }
    }
    memset(s_payload, 0, sizeof(s_payload));
    s_payload_len = 0;
    taskEXIT_CRITICAL(&s_xfer_lock);
    return out;
}

/* ---- the rest of pb_mgmt_ops_t ---- */

static void op_quiesce(void *ctx)
{
    (void)ctx;
    if (s_quiesce != NULL) {
        s_quiesce();
    }
}

static void op_delay_ms(uint32_t ms, void *ctx)
{
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static bool op_network_up_for_proof(void *ctx)
{
    (void)ctx;
    /* Only ever on a complete stored Dataset; never forms a network. */
    if (thread_manager_start_as_always_on_ftd_stored(true) != ESP_OK) {
        return false;
    }
    return coap_socket_ready() && wait_attached(PROOF_ATTACH_WAIT_MS);
}

static void forget_session_dataset(void)
{
    pb_mgmt_wipe(&s_session_tlvs, sizeof(s_session_tlvs));
    s_session_tlvs_valid = false;
}

/* Before the erase: afterwards there is nothing to read. Only a complete
 * Dataset is kept (the same rule a resume has); with anything less there is
 * simply no delivery session. */
static void keep_dataset_for_receipt(void)
{
    if (pb_volatile_store_is_engaged()) {
        return; /* a second erase, after the session: nothing to keep */
    }
    forget_session_dataset();
    thread_manager_dataset_summary_t summary;
    if (thread_manager_get_stored_dataset_summary(&summary) == ESP_OK && summary.complete &&
        thread_manager_get_active_dataset_tlvs(&s_session_tlvs) == ESP_OK && s_session_tlvs.mLength != 0) {
        s_session_tlvs_valid = true;
    } else {
        forget_session_dataset();
    }
}

/* OpenThread only erases its persistent store while Thread is disabled
 * (Instance::ErasePersistentInfo() returns INVALID_STATE otherwise), so
 * Thread goes down first -- and with it the only network this board has.
 * What is reported over Thread after this goes out on a session that exists
 * in RAM only (op_receipt_network_up).
 *
 * Once that session has switched OpenThread's stores to RAM, OpenThread's
 * own erase empties those tables in RAM and nothing else: its
 * otPlatSettingsWipe() and psa_its_remove() never reach the flash again in
 * this boot. An erase asked for then is asked because the flash was found
 * not empty, so it is made on the two stores themselves
 * (pb_stored_network_erase()) -- whatever became of OpenThread's. */
esp_err_t pb_network_store_erase(void)
{
    esp_err_t err = thread_manager_stop();
    if (err == ESP_OK) {
        err = thread_manager_erase_persistent_info();
    }
    if (pb_volatile_store_is_engaged()) {
        esp_err_t flash_err = pb_stored_network_erase(THREAD_MANAGER_OT_STORAGE_PARTITION);
        if (flash_err != ESP_OK) {
            ESP_LOGE(TAG, "the network stores could not be erased in flash (%s)", esp_err_to_name(flash_err));
        }
        if (err == ESP_OK) {
            err = flash_err;
        }
    }
    return err;
}

static esp_err_t op_network_erase(void *ctx)
{
    (void)ctx;
    keep_dataset_for_receipt();
    return pb_network_store_erase();
}

/* The erase is checked against the stores themselves, not against what
 * OpenThread or the PSA key store say -- and against BOTH: OpenThread's
 * settings (the Dataset without its keys) and the persistent PSA keys (the
 * Network Key and the PSKc). See pb_stored_network.h. */
bool pb_network_store_is_empty(void)
{
    return pb_stored_network_is_erased(THREAD_MANAGER_OT_STORAGE_PARTITION);
}

static bool op_network_erased(void *ctx)
{
    (void)ctx;
    return pb_network_store_is_empty();
}

/* For the time after OpenThread has started: it puts a key of its own under
 * the Network Key's id at every start, so "empty" is not the question any
 * more. What is asked here is whether a NETWORK is stored -- and an entry
 * under the Network Key's id counts as OpenThread's own only when this boot
 * read the stores back empty and then saw that entry being stored
 * (pb_stored_network.h). */
bool pb_network_store_holds_no_network(void)
{
    return pb_stored_network_holds_no_network(THREAD_MANAGER_OT_STORAGE_PARTITION);
}

static bool store_holds_no_network(void *ctx)
{
    (void)ctx;
    return pb_network_store_holds_no_network();
}

/*
 * The receipt's delivery session (pb_mgmt_client.h, "delivery"). Called after
 * the final record was stored and read back: nothing of the network is in
 * flash. Thread comes up once more, from the Dataset kept in RAM, as a new
 * device that leaves no trace. That it left none is not taken from this
 * function having returned true: the client reads the flash back
 * (op_network_erased) once this returns and before every transmission, and
 * sends nothing on a store that is not empty.
 *
 *
 *   - OpenThread's persistent stores are switched to RAM first, for good
 *     (pb_volatile_store_engage()), and only if that switch is really in the
 *     path does anything else happen: an attach always stores -- the parent's
 *     answer makes OpenThread save the Dataset, the Network Key is imported
 *     as a persistent key -- and here all of it lands in RAM;
 *   - a new random extended address: the erase reset this board's frame
 *     counters, and a router that still knows the old address would drop
 *     everything from it as a replay;
 *   - not router-eligible: it attaches as a child of whatever router answers
 *     and takes no part in the mesh;
 *   - no service, no exporter, no poll: the one thing sent is the receipt.
 *
 * Checked against the real OpenThread core by
 * tools/test_power_base_release_session_openthread.sh.
 */
static bool op_receipt_network_up(void *ctx)
{
    (void)ctx;
    if (!s_session_tlvs_valid) {
        return false; /* an earlier boot erased the network: nothing was kept */
    }
    if (!pb_network_store_is_empty()) {
        ESP_LOGE(TAG, "receipt session: the network store is not empty -- no session");
        return false;
    }
    otInstance *instance = esp_openthread_get_instance();
    if (!pb_volatile_store_engage(instance)) {
        ESP_LOGE(TAG, "receipt session: OpenThread's stores could not be switched to RAM -- no session (the receipt "
                      "waits for a client)");
        return false;
    }
    otExtAddress fresh;
    if (psa_generate_random(fresh.m8, sizeof(fresh.m8)) != PSA_SUCCESS) {
        return false;
    }

    bool started = false;
    if (esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        const otLinkModeConfig mode = {.mRxOnWhenIdle = true, .mDeviceType = true, .mNetworkData = true};
        started = otThreadSetEnabled(instance, false) == OT_ERROR_NONE &&
                  otIp6SetEnabled(instance, false) == OT_ERROR_NONE &&
                  otLinkSetExtendedAddress(instance, &fresh) == OT_ERROR_NONE &&
                  otThreadSetLinkMode(instance, mode) == OT_ERROR_NONE &&
                  otThreadSetRouterEligible(instance, false) == OT_ERROR_NONE &&
                  otDatasetSetActiveTlvs(instance, &s_session_tlvs) == OT_ERROR_NONE &&
                  otIp6SetEnabled(instance, true) == OT_ERROR_NONE &&
                  otThreadSetEnabled(instance, true) == OT_ERROR_NONE;
        esp_openthread_lock_release();
    }
    if (!started) {
        ESP_LOGE(TAG, "receipt session: Thread could not be started from RAM");
        return false;
    }
    ESP_LOGW(TAG, "receipt session: Thread enabled from RAM only (nothing is stored; contents hidden)");
    return coap_socket_ready() && wait_attached(RECEIPT_ATTACH_WAIT_MS);
}

/* Ends the session, whatever became of it: Thread down, the Dataset in RAM
 * forgotten, and the flash read back once more. */
static bool op_receipt_network_down(void *ctx)
{
    (void)ctx;
    (void)thread_manager_stop();
    forget_session_dataset();
    pb_volatile_store_stats_t stats;
    pb_volatile_store_get_stats(&stats);
    if (pb_volatile_store_is_engaged()) {
        ESP_LOGI(TAG, "receipt session ended: %u settings and %u key-store writes were kept in RAM (%u refused)",
                 (unsigned)stats.settings_kept, (unsigned)stats.its_kept,
                 (unsigned)(stats.settings_refused + stats.its_refused));
    }
    return pb_network_store_is_empty();
}

/* ---- locate: where the hub's host may answer now ---- */

/* The hints themselves are collected by enrollment_locate_hints.c, which only
 * speaks OpenThread's public API (and runs against the real OpenThread core
 * on a host for that reason). Here: the API lock. */
static size_t op_locate_hints(uint8_t (*out)[PB_ENROLL_BRIDGE_ADDR_LEN], size_t cap, void *ctx)
{
    (void)ctx;
    if (!esp_openthread_lock_acquire(OT_LOCK_WAIT)) {
        return 0;
    }
    size_t count = pb_locate_collect_hints(esp_openthread_get_instance(), out, cap);
    esp_openthread_lock_release();
    if (count >= cap) {
        /* Not reachable with cap = PB_MGMT_LOCATE_MAX_HINTS (see
         * enrollment_locate_hints.h); said aloud if it ever is, because what
         * did not fit is never asked. */
        ESP_LOGW(TAG, "locate: the hint list is full (%u) -- further nodes of this network are not asked",
                 (unsigned)count);
    }
    return count;
}

static void op_endpoint_changed(const pb_enroll_bridge_endpoint_t *endpoint, void *ctx)
{
    (void)ctx;
    if (s_endpoint_moved != NULL) {
        s_endpoint_moved(endpoint);
    }
}

static bool op_locate_allowed(void *ctx)
{
    (void)ctx;
    return s_locate_allowed == NULL || s_locate_allowed();
}

static const pb_mgmt_ops_t k_ops = {
    .hmac = op_hmac,
    .random = op_random,
    .exchange = op_exchange,
    .quiesce = op_quiesce,
    .delay_ms = op_delay_ms,
    .network_up_for_proof = op_network_up_for_proof,
    .network_erase = op_network_erase,
    .network_erased = op_network_erased,
    .receipt_network_up = op_receipt_network_up,
    .receipt_network_down = op_receipt_network_down,
    .ctx = NULL,
};

static bool runtime_prepare(const uint8_t eui64[PB_ENROLL_EUI64_LEN])
{
    if (s_done_sem == NULL) {
        s_done_sem = xSemaphoreCreateBinary();
    }
    if (s_done_sem == NULL || psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    /* Looking for the hub is for a board whose services run and whose
     * platform gave somewhere to move them to; the boot paths that only
     * finish a release never do. */
    pb_mgmt_ops_t ops = k_ops;
    if (s_endpoint_moved != NULL) {
        ops.locate_hints = op_locate_hints;
        ops.endpoint_changed = op_endpoint_changed;
        ops.locate_allowed = op_locate_allowed;
    }
    pb_mgmt_client_init(&s_client, &ops, eui64);
    return true;
}

static void restart_now(const char *why)
{
    ESP_LOGW(TAG, "%s -- restarting", why);
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
    esp_restart();
}

/* ---- a final record, before OpenThread starts ---- */

static esp_err_t prestart_erase(void *ctx)
{
    (void)ctx;
    /* The stores themselves: there is no OpenThread yet to ask. */
    return pb_stored_network_erase(THREAD_MANAGER_OT_STORAGE_PARTITION);
}

static bool prestart_erased(void *ctx)
{
    (void)ctx;
    return pb_stored_network_is_erased(THREAD_MANAGER_OT_STORAGE_PARTITION);
}

void pb_mgmt_runtime_prepare_released_start(const uint8_t eui64[PB_ENROLL_EUI64_LEN])
{
    static const pb_mgmt_prestart_ops_t ops = {.erase = prestart_erase, .erased = prestart_erased, .ctx = NULL};
    switch (pb_mgmt_released_prestart(eui64, &ops)) {
    case PB_MGMT_PRESTART_CLEARED:
        ESP_LOGI(TAG, "this board is released: its network stores are erased and read back empty before OpenThread "
                      "starts (the key OpenThread now makes for itself is no network's)");
        break;
    case PB_MGMT_PRESTART_NOT_CLEARED:
        ESP_LOGE(TAG, "this board is released, but its network stores did NOT read back empty before OpenThread "
                      "starts: what is under the Network Key's id is of unknown origin");
        break;
    case PB_MGMT_PRESTART_NONE:
    default:
        break;
    }
}

/* A final record says "erased" about the boot that wrote it. Whether a
 * network is stored all the same is asked here, at every boot, before anything
 * can ask this board for its receipt: a delivery session that wrote to the
 * stores and lost power before that was dealt with leaves exactly such a
 * record on a stored network. pb_mgmt_runtime_prepare_released_start() has
 * already erased and read back; when that did not check out, the store check
 * says no here and the erase is tried once more, through OpenThread. */
static void check_final_release(void)
{
    /* Asked before the erase: afterwards there is nothing to tell by. */
    thread_manager_dataset_summary_t loaded = {0};
    const bool dataset_loaded =
        thread_manager_get_stored_dataset_summary(&loaded) == ESP_OK && (loaded.has_network_key || loaded.complete);
    pb_mgmt_wipe(&loaded, sizeof(loaded));

    pb_mgmt_final_check_t check = pb_mgmt_client_check_final_release(&s_client, store_holds_no_network, NULL);
    pb_mgmt_client_wipe(&s_client);
    /* The erase keeps the Dataset it is about to erase for a delivery
     * session; this path has none. */
    forget_session_dataset();
    switch (check) {
    case PB_MGMT_FINAL_VERIFIED:
        ESP_LOGI(TAG, "this board is released and no network is stored: its release receipt can be collected");
        break;
    case PB_MGMT_FINAL_ERASED_AGAIN:
        /* OpenThread still holds the Dataset it loaded at init, and a restart
         * is the one clean way out of that. Only then: a restart for a store
         * that held something else would come back here at every boot if
         * that something were written at every start. */
        if (dataset_loaded) {
            restart_now("a released board's stored network was found and erased at boot");
        }
        ESP_LOGW(TAG, "a released board's network store was not clear at boot (no Dataset in it) and is erased");
        break;
    case PB_MGMT_FINAL_NOT_ERASED:
        ESP_LOGE(TAG, "this board is released, but its stored network is NOT confirmed erased: no release receipt "
                      "is handed out -- tried again at the next boot");
        (void)thread_manager_stop();
        break;
    case PB_MGMT_FINAL_NONE:
    default:
        break;
    }
}

void pb_mgmt_runtime_finish_pending_release(const uint8_t eui64[PB_ENROLL_EUI64_LEN])
{
    if (!runtime_prepare(eui64)) {
        ESP_LOGE(TAG, "management runtime could not be prepared (no memory or no crypto)");
        return;
    }
    if (pb_mgmt_client_load(&s_client) != PB_MGMT_BOOT_RELEASE_PENDING) {
        check_final_release();
        return;
    }
    pb_mgmt_release_result_t result = pb_mgmt_client_resume_release(&s_client);
    pb_mgmt_client_wipe(&s_client);
    forget_session_dataset();
    if (result == PB_MGMT_RELEASE_FINAL) {
        /* OpenThread still holds what it loaded at init; a restart is the
         * one clean way back to "nothing stored". */
        restart_now("release finished at boot");
    }
    if (pb_volatile_store_is_engaged()) {
        /* A receipt session ran and the release still did not check out:
         * nothing of that session may live on in this boot. */
        restart_now("release not finished after a receipt session");
    }
    /* INCOMPLETE: the record still says what is left ("erase pending"). This
     * boot goes on to the safe wait (the record is never resumed) and the
     * next one tries again. Thread is left disabled. Nothing has reported
     * the erase as done, and no receipt exists. */
    if (result == PB_MGMT_RELEASE_INCOMPLETE) {
        ESP_LOGE(TAG, "release not finished in this boot: the stored network is NOT confirmed erased -- tried again "
                      "at the next boot");
    }
    (void)thread_manager_stop();
}

static void mgmt_task(void *arg)
{
    (void)arg;
    bool poll_now = true;
    while (1) {
        if (!poll_now) {
            vTaskDelay(pdMS_TO_TICKS(PB_MGMT_POLL_INTERVAL_MS));
        }
        poll_now = false;
        if (!role_is_attached(thread_manager_get_role())) {
            continue;
        }
        switch (pb_mgmt_client_poll(&s_client)) {
        case PB_MGMT_POLL_COMMAND_PENDING:
            /* The hub answers with the command only to a poll that echoes
             * the nonce it just gave: ask again without waiting. */
            poll_now = true;
            break;
        case PB_MGMT_POLL_ENDPOINT_MOVED:
            /* The hub was found at another endpoint, which is stored and in
             * use: the next poll goes there, now. */
            poll_now = true;
            break;
        case PB_MGMT_POLL_UNENROLLED:
            pb_mgmt_client_wipe(&s_client);
            restart_now("this board was unenrolled by its hub (stored network erased and verified)");
            break;
        case PB_MGMT_POLL_RELEASED_NOT_ERASED:
            /* Not an unenroll that finished: the record says "erase
             * pending" and the next boot does that step again
             * (pb_mgmt_runtime_finish_pending_release()). */
            pb_mgmt_client_wipe(&s_client);
            ESP_LOGE(TAG, "this board is released, but its stored network is NOT confirmed erased");
            restart_now("the erase is tried again at the next boot");
            break;
        case PB_MGMT_POLL_UNENROLL_INTERRUPTED:
            pb_mgmt_client_wipe(&s_client);
            restart_now("an unenroll command could not be completed in this boot");
            break;
        case PB_MGMT_POLL_NOT_ARMED:
            vTaskDelete(NULL);
            return;
        case PB_MGMT_POLL_NO_ANSWER:
        case PB_MGMT_POLL_IDLE:
        default:
            break;
        }
    }
}

esp_err_t pb_mgmt_runtime_start(const uint8_t eui64[PB_ENROLL_EUI64_LEN], pb_mgmt_quiesce_cb_t quiesce,
                                pb_mgmt_endpoint_moved_cb_t endpoint_moved, pb_mgmt_locate_allowed_cb_t locate_allowed)
{
    s_quiesce = quiesce;
    s_endpoint_moved = endpoint_moved;
    s_locate_allowed = locate_allowed;
    if (!runtime_prepare(eui64)) {
        return ESP_ERR_NO_MEM;
    }
    if (pb_mgmt_client_load(&s_client) != PB_MGMT_BOOT_ARMED) {
        pb_mgmt_client_wipe(&s_client);
        ESP_LOGI(TAG, "no management credential in this board's enrollment record -- it cannot be unenrolled "
                      "remotely (enroll it again with a client that delivers one)");
        return ESP_OK;
    }
    if (xTaskCreate(mgmt_task, "pb_mgmt", MGMT_TASK_STACK, NULL, 3, NULL) != pdPASS) {
        pb_mgmt_client_wipe(&s_client);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "management poll started (every %u ms while attached; credential never logged)%s",
             (unsigned)PB_MGMT_POLL_INTERVAL_MS,
             endpoint_moved != NULL ? "; the hub is looked for when it stops answering" : "");
    return ESP_OK;
}
