/*
 * protocomm binding of the BLE enrollment rules: one protocomm instance with
 * ESP-IDF's Security 2 (SRP-6a + AES-256-GCM, components/protocomm) and the
 * pb-status / pb-start / pb-cancel endpoints of pb_ble_admission.h.
 *
 * Nothing here is BLE specific. power_base/main/enrollment_ble.c hands the
 * returned protocomm instance to protocomm_ble_start(); the host harness in
 * tools/host_ble_sec2 drives the same instance through
 * protocomm_req_handle() with ESP-IDF's own Python client.
 *
 * Security: ESP-IDF's protocomm_security2 does all of the cryptography. The
 * security object registered here is that same object with three entries
 * wrapped -- new/close transport session and the handshake handler -- so
 * that the admission rules learn, in the BLE callback itself, that the
 * session changed (to drop the challenge) and whether a handshake message
 * failed (to count it). The wrappers add no cryptography and change no
 * message. Security 0 and Security 1 are never registered.
 *
 * Where code runs
 *   NimBLE host task: ESP-IDF's handshake (the SRP big-number work is done
 *     there by the SDK) and the three endpoint handlers below, which only
 *     take the leaf mutex and call pb_ble_adm_handle_*().
 *   Enrollment worker: pb_ble_service_poll().
 *
 * Lock order (see also power_base/main/enrollment_runtime.c):
 *   enrollment_runtime's controller mutex  ->  OpenThread API lock
 *   leaf mutex of this file: taken alone. It is never held while calling the
 *   controller, OpenThread, NVS or the BLE stack, and the BLE side never
 *   takes the controller mutex.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "protocomm.h"
#include "power_base_ble_auth/power_base_ble_auth.h"
#include "power_base_ble_enroll/pb_ble_admission.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What "proto-ver" answers. ESP-IDF's provisioning clients read
 * prov.sec_ver (2 = Security 2) and prov.sec_patch_ver (1 = the AES-GCM IV
 * is an 8-byte session id followed by a big-endian counter that both sides
 * advance after every encrypt and decrypt). pb_enroll.proto stays 1;
 * pb_enroll.cap lists what this board takes beyond the first version 1
 * messages (PB_BLE_MSG_START_EP_REQ). A client that does not know "cap"
 * ignores it. Readable before authentication, so it is a hint for choosing
 * the message, never a reason to trust anything. */
#define PB_BLE_VERSION_JSON                                                                                          \
    "{\"prov\":{\"ver\":\"v1.1\",\"sec_ver\":2,\"sec_patch_ver\":1,\"cap\":[]},\"pb_enroll\":{\"proto\":1,"       \
    "\"cap\":[\"" PB_BLE_CAP_BRIDGE_ENDPOINT_V1 "\"]}}"

typedef struct {
    uint8_t eui64[PB_BLE_EUI64_LEN];
    bool joiner_eui64_ok;
    /* The prepared image's enrollment endpoint policy (zero = OPTIONAL). */
    pb_enroll_endpoint_policy_t endpoint_policy;
    /* Must stay valid until pb_ble_service_stop(): protocomm keeps the
     * pointers, not a copy. */
    const pb_ble_auth_cred_t *cred;
    /* request_join / cancel / get_status / ctx; lock and unlock are ignored
     * (this file supplies its own). */
    pb_ble_worker_ops_t ctl;
    /* Monotonic milliseconds; NULL = FreeRTOS tick count. */
    uint32_t (*now_ms)(void);
} pb_ble_service_config_t;

/* Creates the protocomm instance (version, Security 2, endpoints). The
 * caller starts a transport on *out_pc. ESP_ERR_NOT_SUPPORTED when the build
 * has no Security 2, or has Security 0/1 as well. */
esp_err_t pb_ble_service_start(const pb_ble_service_config_t *cfg, protocomm_t **out_pc);

/* Worker context only. See pb_ble_adm_worker_poll(). */
pb_ble_close_reason_t pb_ble_service_poll(void);

/* Frees the protocomm instance and wipes the secrets held by the admission
 * state. Only after the transport on it has been stopped, and never from a
 * transport callback. */
void pb_ble_service_stop(void);

#ifdef __cplusplus
}
#endif
