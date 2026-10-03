/*
 * BLE transport for pb_thread:mode=2 enrollment
 * (CONFIG_POWER_BASE_BLE_ENROLLMENT, set only by
 * power_base/sdkconfig.ble_enrollment). See docs/power-base.md "BLE 등록 전달
 * 경로" for the wire format and the app-side order of operations.
 *
 * This file only connects three existing things:
 *   - ESP-IDF's protocomm NimBLE transport (protocomm_ble_start/stop),
 *   - components/power_base_ble_enroll (Security 2 session + admission), and
 *   - the enrollment controller in enrollment_runtime.c, reached through
 *     pb_enrollment_request_join()/pb_enrollment_cancel() exactly like the
 *     bench console reaches it. There is no second Joiner, marker or service
 *     start path.
 *
 * Authentication is protocomm Security 2 with this board's own salt and
 * verifier (components/power_base_ble_auth). A BLE connection, the
 * advertised name and the EUI-64 inside it prove nothing; the endpoints are
 * only reachable through an established Security 2 session. No BLE pairing
 * or bonding is used and no Security 0/1 is built in.
 *
 * Both functions run in the enrollment task. The BLE stack is never started
 * or stopped from one of its own callbacks.
 */
#include "enrollment_runtime.h"

#if CONFIG_POWER_BASE_BLE_ENROLLMENT

#if CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_0 || CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1
#error "BLE enrollment must be built with protocomm Security 2 only (no Security 0/1)"
#endif
#if CONFIG_POWER_BASE_BENCH_JOINER_CONSOLE || CONFIG_OPENTHREAD_CLI
#error "the BLE enrollment build must not contain the pb_join console or the OpenThread CLI"
#endif
/* At DEBUG level ESP-IDF's security2.c/esp_srp.c hex-dump the SRP session
 * key and proofs. */
#if CONFIG_LOG_MAXIMUM_LEVEL > 3
#error "the BLE enrollment build must keep CONFIG_LOG_MAXIMUM_LEVEL at INFO or lower"
#endif

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "protocomm.h"
#include "protocomm_ble.h"
#include "power_base_ble_auth/power_base_ble_auth.h"
#include "power_base_ble_enroll/pb_ble_service.h"

static const char *TAG = "pb_enroll_ble";

/*
 * GATT service 2e8a0000-9ffb-4868-bc63-064a14069755, as NimBLE stores it
 * (least significant byte first). protocomm derives each characteristic UUID
 * by writing the endpoint's 16-bit id into bytes [12] (low) and [13] (high),
 * i.e. 2e8aXXXX-9ffb-4868-bc63-064a14069755.
 */
static const uint8_t k_service_uuid[BLE_UUID128_VAL_LENGTH] = {
    0x55, 0x97, 0x06, 0x14, 0x4a, 0x06, 0x63, 0xbc, 0x68, 0x48, 0xfb, 0x9f, 0x00, 0x00, 0x8a, 0x2e,
};

static protocomm_ble_name_uuid_t s_endpoints[] = {
    {PB_BLE_EP_SESSION, 0xFF51},
    {PB_BLE_EP_VERSION, 0xFF53},
    {PB_BLE_EP_STATUS, 0xFF60},
    {PB_BLE_EP_START, 0xFF61},
    {PB_BLE_EP_CANCEL, 0xFF62},
};

/* "POGO-PB-" + the EUI-64 as 16 upper-case hex digits. A hint for picking
 * the board in a scan list, nothing more. */
#define DEVICE_NAME_PREFIX "POGO-PB-"

typedef enum {
    BLE_OFF = 0,
    BLE_RUNNING,
    BLE_STOPPED,
    BLE_STOP_FAILED,
} ble_state_t;

static ble_state_t s_ble;
static protocomm_t *s_pc;
/* protocomm keeps pointers into this; wiped once the transport is down. */
static pb_ble_auth_cred_t s_cred;

static esp_err_t ctl_request_join(char pskd[PB_ENROLL_PSKD_BUF_LEN], void *ctx)
{
    (void)ctx;
    return pb_enrollment_request_join(pskd);
}

static esp_err_t ctl_cancel(void *ctx)
{
    (void)ctx;
    return pb_enrollment_cancel();
}

static void ctl_get_status(pb_ble_ctl_status_t *out, void *ctx)
{
    (void)ctx;
    pb_enrollment_status_t st;
    pb_enrollment_get_status(&st);
    out->state = st.state;
    out->last = st.last;
    out->attempt = st.attempt;
}

esp_err_t pb_enrollment_ble_start(void)
{
    if (s_ble != BLE_OFF) {
        return ESP_ERR_INVALID_STATE;
    }
    pb_enrollment_status_t st;
    pb_enrollment_get_status(&st);
    if (!st.joiner_eui64_matches) {
        ESP_LOGE(TAG, "BLE enrollment not offered: the Joiner EUI-64 differs from this board's EUI-64");
        return ESP_ERR_INVALID_STATE;
    }

    pb_ble_auth_status_t load_status = pb_ble_auth_load(st.eui64, &s_cred);
    if (load_status != PB_BLE_AUTH_OK) {
        pb_enroll_wipe(&s_cred, sizeof(s_cred));
        ESP_LOGE(TAG, "BLE enrollment not offered: setup credential in 'cfg' is %s (prepare this board with "
                      "tools/gen_power_base_ble_config.py for its own EUI-64)",
                 pb_ble_auth_status_name(load_status));
        return ESP_ERR_INVALID_STATE;
    }

    pb_ble_service_config_t svc = {
        .joiner_eui64_ok = st.joiner_eui64_matches,
        .cred = &s_cred,
        .ctl =
            {
                .request_join = ctl_request_join,
                .cancel = ctl_cancel,
                .get_status = ctl_get_status,
                .ctx = NULL,
            },
        .now_ms = NULL,
    };
    memcpy(svc.eui64, st.eui64, sizeof(svc.eui64));
    esp_err_t err = pb_ble_service_start(&svc, &s_pc);
    if (err != ESP_OK) {
        pb_enroll_wipe(&s_cred, sizeof(s_cred));
        ESP_LOGE(TAG, "BLE enrollment service setup failed (%s)", esp_err_to_name(err));
        return err;
    }

    protocomm_ble_config_t ble;
    memset(&ble, 0, sizeof(ble));
    snprintf(ble.device_name, sizeof(ble.device_name), DEVICE_NAME_PREFIX "%02X%02X%02X%02X%02X%02X%02X%02X",
             st.eui64[0], st.eui64[1], st.eui64[2], st.eui64[3], st.eui64[4], st.eui64[5], st.eui64[6], st.eui64[7]);
    memcpy(ble.service_uuid, k_service_uuid, sizeof(ble.service_uuid));
    ble.nu_lookup = s_endpoints;
    ble.nu_lookup_count = sizeof(s_endpoints) / sizeof(s_endpoints[0]);
    /* Security is the Security 2 session, not the BLE link: no pairing, no
     * bonding, nothing stored by the BLE stack. */
    ble.ble_bonding = 0;
    ble.ble_sm_sc = 0;
    ble.ble_link_encryption = 0;
    ble.ble_notify = 0;
    /* protocomm_ble_stop() then also shuts the NimBLE host and the BLE
     * controller down, which ends advertising and any connection. */
    ble.keep_ble_on = 0;

    err = protocomm_ble_start(s_pc, &ble);
    if (err != ESP_OK) {
        pb_ble_service_stop();
        s_pc = NULL;
        pb_enroll_wipe(&s_cred, sizeof(s_cred));
        ESP_LOGE(TAG, "BLE stack start failed (%s)", esp_err_to_name(err));
        return err;
    }
    s_ble = BLE_RUNNING;
    ESP_LOGW(TAG, "safe wait: BLE enrollment open as '%s' for up to %u s (authenticated start only; Thread and "
                  "services stay off until enrolled)",
             ble.device_name, (unsigned)(PB_BLE_ADV_WINDOW_MS / 1000u));
    return ESP_OK;
}

void pb_enrollment_ble_poll(void)
{
    pb_ble_close_reason_t reason = pb_ble_service_poll();
    if (reason == PB_BLE_OPEN || s_ble != BLE_RUNNING) {
        return;
    }

    /* Stops advertising and the NimBLE host (which ends the connection and,
     * through protocomm, the Security 2 session), then the controller. It
     * returns only after the host task has left its event loop, so no
     * endpoint callback is running when the objects below are freed. */
    esp_err_t err = protocomm_ble_stop(s_pc);
    if (err != ESP_OK) {
        /* The stack may still call into protocomm: keep every object alive.
         * The admission state is already closed, so nothing is accepted. */
        s_ble = BLE_STOP_FAILED;
        ESP_LOGE(TAG, "BLE stop failed (%s) after close (%s) -- BLE objects kept; no request is accepted any more",
                 esp_err_to_name(err), pb_ble_close_reason_name(reason));
        return;
    }
    pb_ble_service_stop();
    s_pc = NULL;
    pb_enroll_wipe(&s_cred, sizeof(s_cred));
    s_ble = BLE_STOPPED;
    ESP_LOGI(TAG, "BLE enrollment stopped (%s): advertising, connection and session are down; power-cycle to "
                  "open it again unless enrolled",
             pb_ble_close_reason_name(reason));
}

#endif /* CONFIG_POWER_BASE_BLE_ENROLLMENT */
