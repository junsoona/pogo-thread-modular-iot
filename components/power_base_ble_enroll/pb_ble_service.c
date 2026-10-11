#include "power_base_ble_enroll/pb_ble_service.h"

#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"

#if CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_2 && !CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_0 &&            \
    !CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "protocomm_security.h"
#include "protocomm_security2.h"

static const char *TAG = "pb_ble_svc";

/* File-scope and never freed: a transport callback that is still running
 * while the worker shuts the transport down can only ever touch these. */
static SemaphoreHandle_t s_leaf;
static pb_ble_adm_t s_adm;
static pb_ble_service_config_t s_cfg;
static protocomm_security_t s_sec;
static protocomm_security2_params_t s_sec_params;
static protocomm_t *s_pc;

static uint32_t now_ms(void)
{
    if (s_cfg.now_ms != NULL) {
        return s_cfg.now_ms();
    }
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void leaf_lock(void *ctx)
{
    (void)ctx;
    xSemaphoreTake(s_leaf, portMAX_DELAY);
}

static void leaf_unlock(void *ctx)
{
    (void)ctx;
    xSemaphoreGive(s_leaf);
}

static bool op_random(uint8_t *buf, size_t len, void *ctx)
{
    (void)ctx;
    return psa_generate_random(buf, len) == PSA_SUCCESS;
}

static bool op_sha256(const uint8_t *data, size_t len, uint8_t out[PB_BLE_SHA256_LEN], void *ctx)
{
    (void)ctx;
    size_t out_len = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, data, len, out, PB_BLE_SHA256_LEN, &out_len) == PSA_SUCCESS &&
           out_len == PB_BLE_SHA256_LEN;
}

/* ---- Security 2 with the session observed (NimBLE host task) ---- */

static esp_err_t sec_new_session(protocomm_security_handle_t handle, uint32_t session_id)
{
    leaf_lock(NULL);
    pb_ble_adm_session_changed(&s_adm);
    leaf_unlock(NULL);
    return protocomm_security2.new_transport_session(handle, session_id);
}

static esp_err_t sec_close_session(protocomm_security_handle_t handle, uint32_t session_id)
{
    leaf_lock(NULL);
    pb_ble_adm_session_changed(&s_adm);
    leaf_unlock(NULL);
    return protocomm_security2.close_transport_session(handle, session_id);
}

static esp_err_t sec_req_handler(protocomm_security_handle_t handle, const void *sec_params, uint32_t session_id,
                                 const uint8_t *inbuf, ssize_t inlen, uint8_t **outbuf, ssize_t *outlen,
                                 void *priv_data)
{
    leaf_lock(NULL);
    bool allowed = pb_ble_adm_sec_request_begin(&s_adm);
    leaf_unlock(NULL);
    if (!allowed) {
        return ESP_FAIL;
    }
    /* ESP-IDF's SRP-6a exchange. It runs in this (NimBLE host) task. */
    esp_err_t err = protocomm_security2.security_req_handler(handle, sec_params, session_id, inbuf, inlen, outbuf,
                                                             outlen, priv_data);
    leaf_lock(NULL);
    pb_ble_adm_sec_request_end(&s_adm, err == ESP_OK);
    leaf_unlock(NULL);
    return err;
}

/* ---- endpoints (NimBLE host task; protocomm has already authenticated and
 *      decrypted the request with the Security 2 session key) ---- */

static esp_err_t ep_status(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen, uint8_t **outbuf,
                           ssize_t *outlen, void *priv_data)
{
    (void)session_id;
    (void)priv_data;
    const size_t in_n = inlen > 0 ? (size_t)inlen : 0;
    if (pb_ble_adm_is_receipt_request(inbuf, in_n)) {
        /* The second message on this endpoint. It reads the stored record
         * (flash), so it runs without the struct lock; it uses nothing of
         * s_adm but the EUI-64, which never changes after start. */
        uint8_t *receipt = malloc(PB_BLE_RECEIPT_RESP_LEN);
        if (receipt == NULL) {
            return ESP_ERR_NO_MEM;
        }
        size_t rn = pb_ble_adm_handle_receipt(s_cfg.eui64, inbuf, in_n, receipt, s_cfg.receipt, s_cfg.receipt_ctx);
        *outbuf = receipt;
        *outlen = (ssize_t)rn;
        return ESP_OK;
    }
    /* lr_supported never changes after start, so it is read without the lock. */
    if (pb_ble_adm_is_local_reset_status_request(&s_adm, inbuf, in_n)) {
        /* The third message on this endpoint. It reads only what the worker
         * published into the struct: no flash, nothing changed. */
        uint8_t *lr = malloc(PB_BLE_LOCAL_RESET_STATUS_RESP_LEN);
        if (lr == NULL) {
            return ESP_ERR_NO_MEM;
        }
        leaf_lock(NULL);
        size_t ln = pb_ble_adm_handle_local_reset_status(&s_adm, inbuf, in_n, lr, now_ms());
        leaf_unlock(NULL);
        *outbuf = lr;
        *outlen = (ssize_t)ln;
        return ESP_OK;
    }
    uint8_t *resp = malloc(PB_BLE_STATUS_RESP_LEN);
    if (resp == NULL) {
        return ESP_ERR_NO_MEM;
    }
    leaf_lock(NULL);
    size_t n = pb_ble_adm_handle_status(&s_adm, inbuf, in_n, resp, now_ms());
    leaf_unlock(NULL);
    *outbuf = resp;
    *outlen = (ssize_t)n;
    return ESP_OK;
}

static esp_err_t ep_start(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen, uint8_t **outbuf, ssize_t *outlen,
                          void *priv_data)
{
    (void)session_id;
    (void)priv_data;
    /* protocomm hands over its own heap copy of the decrypted request and
     * frees it without clearing it; it holds the PSKd, so it is wiped here
     * on every path. */
    uint8_t *req = (uint8_t *)(uintptr_t)inbuf;
    size_t req_len = inlen > 0 ? (size_t)inlen : 0;
    uint8_t *resp = malloc(PB_BLE_SHORT_RESP_LEN);
    if (resp == NULL) {
        if (req != NULL) {
            pb_enroll_wipe(req, req_len);
        }
        return ESP_ERR_NO_MEM;
    }
    leaf_lock(NULL);
    size_t n = pb_ble_adm_handle_start(&s_adm, req, req_len, resp, now_ms());
    leaf_unlock(NULL);
    *outbuf = resp;
    *outlen = (ssize_t)n;
    return ESP_OK;
}

static esp_err_t ep_cancel(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen, uint8_t **outbuf,
                           ssize_t *outlen, void *priv_data)
{
    (void)session_id;
    (void)priv_data;
    uint8_t *resp = malloc(PB_BLE_SHORT_RESP_LEN);
    if (resp == NULL) {
        return ESP_ERR_NO_MEM;
    }
    leaf_lock(NULL);
    size_t n = pb_ble_adm_handle_cancel(&s_adm, inbuf, inlen > 0 ? (size_t)inlen : 0, resp);
    leaf_unlock(NULL);
    *outbuf = resp;
    *outlen = (ssize_t)n;
    return ESP_OK;
}

esp_err_t pb_ble_service_start(const pb_ble_service_config_t *cfg, protocomm_t **out_pc)
{
    if (cfg == NULL || out_pc == NULL || cfg->cred == NULL || cfg->ctl.request_join == NULL ||
        cfg->ctl.cancel == NULL || cfg->ctl.get_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->local_reset && (cfg->ctl.local_reset == NULL || cfg->ctl.get_local_reset == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_pc != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    if (s_leaf == NULL) {
        s_leaf = xSemaphoreCreateMutex();
        if (s_leaf == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_cfg = *cfg;
    const pb_ble_adm_ops_t adm_ops = {.random = op_random, .sha256 = op_sha256, .ctx = NULL};
    pb_ble_ctl_status_t st;
    memset(&st, 0, sizeof(st));
    cfg->ctl.get_status(&st, cfg->ctl.ctx);
    /* What the board holds, read from flash here (the worker's context)
     * before any client can ask. */
    pb_ble_lr_snapshot_t lr_snap;
    memset(&lr_snap, 0, sizeof(lr_snap));
    if (cfg->local_reset) {
        cfg->ctl.get_local_reset(&lr_snap, cfg->ctl.ctx);
    }
    leaf_lock(NULL);
    pb_ble_adm_init(&s_adm, &adm_ops, cfg->eui64, cfg->joiner_eui64_ok, now_ms());
    pb_ble_adm_set_endpoint_policy(&s_adm, cfg->endpoint_policy);
    s_adm.ctl = st;
    if (cfg->local_reset) {
        pb_ble_adm_set_local_reset(&s_adm, true, cfg->reenroll_window, now_ms());
        s_adm.lr_snap = lr_snap;
        s_adm.lr_refresh = false;
    }
    leaf_unlock(NULL);

    protocomm_t *pc = protocomm_new();
    if (pc == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_sec = protocomm_security2;
    s_sec.new_transport_session = sec_new_session;
    s_sec.close_transport_session = sec_close_session;
    s_sec.security_req_handler = sec_req_handler;
    s_sec_params = (protocomm_security2_params_t){
        .salt = (const char *)cfg->cred->salt,
        .salt_len = PB_BLE_AUTH_SALT_LEN,
        .verifier = (const char *)cfg->cred->verifier,
        .verifier_len = PB_BLE_AUTH_VERIFIER_LEN,
    };

    esp_err_t err = protocomm_set_version(pc, PB_BLE_EP_VERSION,
                                          cfg->local_reset ? PB_BLE_VERSION_JSON_LOCAL_RESET : PB_BLE_VERSION_JSON);
    if (err == ESP_OK) {
        err = protocomm_set_security(pc, PB_BLE_EP_SESSION, &s_sec, &s_sec_params);
    }
    if (err == ESP_OK) {
        err = protocomm_add_endpoint(pc, PB_BLE_EP_STATUS, ep_status, NULL);
    }
    if (err == ESP_OK) {
        err = protocomm_add_endpoint(pc, PB_BLE_EP_START, ep_start, NULL);
    }
    if (err == ESP_OK) {
        err = protocomm_add_endpoint(pc, PB_BLE_EP_CANCEL, ep_cancel, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "protocomm setup failed (%s)", esp_err_to_name(err));
        protocomm_delete(pc);
        return err;
    }
    s_pc = pc;
    *out_pc = pc;
    return ESP_OK;
}

pb_ble_close_reason_t pb_ble_service_poll(void)
{
    if (s_leaf == NULL) {
        return PB_BLE_OPEN;
    }
    pb_ble_worker_ops_t w = s_cfg.ctl;
    w.lock = leaf_lock;
    w.unlock = leaf_unlock;
    return pb_ble_adm_worker_poll(&s_adm, &w, now_ms());
}

bool pb_ble_service_busy(void)
{
    if (s_leaf == NULL) {
        return false;
    }
    leaf_lock(NULL);
    bool busy = pb_ble_adm_attempt_active(&s_adm) || pb_ble_adm_local_reset_active(&s_adm);
    leaf_unlock(NULL);
    return busy;
}

void pb_ble_service_stop(void)
{
    if (s_pc != NULL) {
        protocomm_delete(s_pc);
        s_pc = NULL;
    }
    if (s_leaf != NULL) {
        leaf_lock(NULL);
        /* Not the PSKd slot of a start that is still queued: the worker
         * runs it on its next pass and wipes it then. */
        pb_enroll_wipe(s_adm.ch, sizeof(s_adm.ch));
        s_adm.ch_valid = false;
        leaf_unlock(NULL);
    }
    memset(&s_sec_params, 0, sizeof(s_sec_params));
}

#else /* no Security 2, or a weaker scheme is also built in */

esp_err_t pb_ble_service_start(const pb_ble_service_config_t *cfg, protocomm_t **out_pc)
{
    (void)cfg;
    (void)out_pc;
    return ESP_ERR_NOT_SUPPORTED;
}

pb_ble_close_reason_t pb_ble_service_poll(void)
{
    return PB_BLE_OPEN;
}

bool pb_ble_service_busy(void)
{
    return false;
}

void pb_ble_service_stop(void)
{
}

#endif
