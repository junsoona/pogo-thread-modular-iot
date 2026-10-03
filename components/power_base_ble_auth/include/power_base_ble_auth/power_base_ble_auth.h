/*
 * Per-board BLE enrollment credential (pb_thread:mode=2, BLE transport) --
 * see docs/power-base.md "BLE 등록 전달 경로".
 *
 * The board never stores its setup code. The prepare tool
 * (tools/gen_power_base_ble_config.py) draws a random setup code per board,
 * derives the protocomm Security 2 salt and verifier from it (SRP-6a, RFC
 * 5054 3072-bit group, g = 5, SHA-512 -- the only group ESP-IDF's
 * components/protocomm implements) with
 *     username = this board's EUI-64 as 16 lower-case hex digits
 *     password = the setup code
 * and writes ONE record into the read-only "cfg" partition:
 *
 *   namespace "pb_ble_auth", key "cred", blob of PB_BLE_AUTH_RECORD_LEN bytes
 *     [0..3]     magic "PBA1"
 *     [4]        format version (PB_BLE_AUTH_FORMAT_VERSION)
 *     [5]        SRP group id, 1 = protocomm Security 2 (3072-bit, SHA-512)
 *     [6]        salt length, always PB_BLE_AUTH_SALT_LEN
 *     [7]        zero
 *     [8..15]    EUI-64 the record was made for
 *     [16..31]   salt
 *     [32..415]  verifier, big-endian, left-padded with zeros to 384 bytes
 *     [416..419] CRC-32 (IEEE, little-endian) of bytes [0..415]
 *
 * The verifier is not the setup code, but it must still be kept out of logs:
 * it lets someone test guesses of the code offline.
 *
 * A record is usable only when it is intact AND names the EUI-64 the caller
 * passes (the board's own eFuse EUI-64). Anything else means "no BLE
 * enrollment on this board"; nothing here falls back to another credential.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PB_BLE_AUTH_NVS_PARTITION "cfg"
#define PB_BLE_AUTH_NVS_NAMESPACE "pb_ble_auth"
#define PB_BLE_AUTH_NVS_KEY "cred"

#define PB_BLE_AUTH_FORMAT_VERSION 1u
#define PB_BLE_AUTH_SRP_GROUP_SEC2_3072_SHA512 1u
#define PB_BLE_AUTH_EUI64_LEN 8
#define PB_BLE_AUTH_SALT_LEN 16
#define PB_BLE_AUTH_VERIFIER_LEN 384
#define PB_BLE_AUTH_RECORD_LEN 420
/* 16 hex digits + NUL. */
#define PB_BLE_AUTH_USERNAME_BUF_LEN 17

typedef enum {
    PB_BLE_AUTH_OK = 0,
    PB_BLE_AUTH_ABSENT,              /* no namespace or no key: BLE enrollment not prepared */
    PB_BLE_AUTH_CORRUPT,             /* wrong size, magic, reserved byte, CRC, or a degenerate salt/verifier */
    PB_BLE_AUTH_UNSUPPORTED_VERSION, /* intact, but a format version or SRP group this firmware does not know */
    PB_BLE_AUTH_FOREIGN,             /* intact, but made for another EUI-64 */
    PB_BLE_AUTH_READ_ERROR,          /* NVS itself failed */
} pb_ble_auth_status_t;

typedef struct {
    uint8_t salt[PB_BLE_AUTH_SALT_LEN];
    uint8_t verifier[PB_BLE_AUTH_VERIFIER_LEN];
} pb_ble_auth_cred_t;

/* Stable log strings. */
const char *pb_ble_auth_status_name(pb_ble_auth_status_t status);

/* Checks one raw record. *out is filled only for PB_BLE_AUTH_OK and zeroed
 * otherwise. Never logs record bytes. */
pb_ble_auth_status_t pb_ble_auth_parse(const uint8_t *raw, size_t len, const uint8_t eui64[PB_BLE_AUTH_EUI64_LEN],
                                       pb_ble_auth_cred_t *out);

/* Reads "cfg":pb_ble_auth/cred and applies pb_ble_auth_parse(). */
pb_ble_auth_status_t pb_ble_auth_load(const uint8_t eui64[PB_BLE_AUTH_EUI64_LEN], pb_ble_auth_cred_t *out);

/* The fixed SRP username for this board: 16 lower-case hex digits. */
void pb_ble_auth_username(const uint8_t eui64[PB_BLE_AUTH_EUI64_LEN], char out[PB_BLE_AUTH_USERNAME_BUF_LEN]);

#ifdef __cplusplus
}
#endif
