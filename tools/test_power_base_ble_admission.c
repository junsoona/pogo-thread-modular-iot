/*
 * Host test for the REAL BLE enrollment admission code:
 *   components/power_base_ble_enroll/pb_ble_admission.c   (parser, challenge,
 *       attempt record, bounded worker queue, close rules, worker pass)
 *   components/power_base_ble_auth/power_base_ble_auth.c  (credential record)
 *   components/power_base_enrollment/power_base_enrollment.c (the controller
 *       the worker drives -- the same one the bench console drives)
 * linked against the functional NVS simulator and the same kind of fake
 * OpenThread operations tools/test_power_base_enrollment.c uses.
 *
 * Also here: the second start message, PB_BLE_MSG_START_EP_REQ, which carries
 * the Linux bridge endpoint -- its golden vector (shared with the app's Swift
 * codec), every length, the address and port rules, the request digest, and
 * what becomes of the endpoint on cancel, failure and enrollment.
 *
 * What is NOT here: the Security 2 handshake and protocomm (run for real, on
 * the host, by tools/host_ble_sec2 + tools/test_power_base_ble_sec2_interop.py)
 * and anything over a real BLE link. In this file a "session" is what the
 * protocomm binding reports to the admission code: pb_ble_adm_session_changed()
 * and pb_ble_adm_sec_request_begin()/end().
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I tools/host_test_stubs/nvs_sim -I tools/host_test_stubs \
 *      -I components/power_base_enrollment/include -I components/power_base_ble_enroll/include \
 *      -I components/power_base_ble_auth/include \
 *      -o /tmp/test_power_base_ble_admission tools/test_power_base_ble_admission.c \
 *      components/power_base_ble_enroll/pb_ble_admission.c \
 *      components/power_base_ble_auth/power_base_ble_auth.c \
 *      components/power_base_enrollment/power_base_enrollment.c tools/host_test_stubs/nvs_sim/nvs_sim.c \
 *   && /tmp/test_power_base_ble_admission
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nvs.h"
#include "nvs_sim.h"
#include "power_base_ble_auth/power_base_ble_auth.h"
#include "power_base_ble_enroll/pb_ble_admission.h"
#include "power_base_enrollment/power_base_enrollment.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                                   \
            g_failures++;                                                                                            \
        } else {                                                                                                     \
            printf("ok:   %s\n", msg);                                                                               \
        }                                                                                                            \
    } while (0)

/* Made-up identifiers; nothing here is a real board, network or code. */
static const uint8_t EUI_A[8] = {0x60, 0x55, 0xf9, 0xff, 0xfe, 0x00, 0x12, 0x34};
static const uint8_t EUI_B[8] = {0x60, 0x55, 0xf9, 0xff, 0xfe, 0x00, 0x56, 0x78};
static const pb_enroll_network_id_t NET_OTBR = {
    .ext_pan_id = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88},
    .mesh_local_prefix = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00},
};
static const char *PSKD = "J01NME7K";
static const char *PSKD2 = "ANY0THER1";

/* ---- SHA-256 (FIPS 180-4), only for the test's digest op ---- */

static uint32_t rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static bool test_sha256(const uint8_t *data, size_t len, uint8_t out[32], void *ctx)
{
    (void)ctx;
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t msg[256] = {0};
    if (len > 180) {
        return false;
    }
    memcpy(msg, data, len);
    msg[len] = 0x80;
    size_t total = ((len + 9 + 63) / 64) * 64;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) {
        msg[total - 1 - i] = (uint8_t)(bits >> (8 * i));
    }
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)msg[off + 4 * i] << 24) | ((uint32_t)msg[off + 4 * i + 1] << 16) |
                   ((uint32_t)msg[off + 4 * i + 2] << 8) | msg[off + 4 * i + 3];
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t v[8];
        memcpy(v, h, sizeof(v));
        for (int i = 0; i < 64; i++) {
            uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
            uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
            uint32_t t1 = v[7] + s1 + ch + k[i] + w[i];
            uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
            uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            uint32_t t2 = s0 + maj;
            v[7] = v[6];
            v[6] = v[5];
            v[5] = v[4];
            v[4] = v[3] + t1;
            v[3] = v[2];
            v[2] = v[1];
            v[1] = v[0];
            v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; i++) {
            h[i] += v[i];
        }
    }
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
    return true;
}

/* ---- test random: different every call, and able to fail ---- */

static uint32_t g_rand_state = 0x1234abcdu;
static bool g_rand_fails;
/* When set: the next random bytes are exactly these (a golden vector's
 * challenge), once. */
static const uint8_t *g_rand_fixed;

static bool test_random(uint8_t *buf, size_t len, void *ctx)
{
    (void)ctx;
    if (g_rand_fails) {
        return false;
    }
    if (g_rand_fixed != NULL) {
        memcpy(buf, g_rand_fixed, len);
        g_rand_fixed = NULL;
        return true;
    }
    for (size_t i = 0; i < len; i++) {
        g_rand_state ^= g_rand_state << 13;
        g_rand_state ^= g_rand_state >> 17;
        g_rand_state ^= g_rand_state << 5;
        buf[i] = (uint8_t)g_rand_state;
    }
    return true;
}

static const pb_ble_adm_ops_t k_adm_ops = {.random = test_random, .sha256 = test_sha256, .ctx = NULL};

/* ---- fake OpenThread under the real controller ---- */

typedef struct {
    bool has_key;
    bool complete;
    pb_enroll_network_id_t id;
    bool thread_enabled;
    bool joiner_running;
    esp_err_t joiner_start_ret;
    int joiner_start_calls;
    int joiner_stop_calls;
    int thread_stop_calls;
    uint32_t last_attempt;
    char last_pskd[PB_ENROLL_PSKD_BUF_LEN];
} fake_ot_t;

static fake_ot_t g_ot;

static esp_err_t f_joiner_start(const char *pskd, uint32_t attempt, void *ctx)
{
    (void)ctx;
    g_ot.joiner_start_calls++;
    g_ot.last_attempt = attempt;
    memset(g_ot.last_pskd, 0, sizeof(g_ot.last_pskd));
    strncpy(g_ot.last_pskd, pskd, sizeof(g_ot.last_pskd) - 1);
    if (g_ot.joiner_start_ret == ESP_OK) {
        g_ot.joiner_running = true;
    }
    return g_ot.joiner_start_ret;
}

static void f_joiner_stop(void *ctx)
{
    (void)ctx;
    g_ot.joiner_stop_calls++;
    g_ot.joiner_running = false;
}

static esp_err_t f_thread_start_stored(bool require_complete, void *ctx)
{
    (void)ctx;
    if (!g_ot.has_key || (require_complete && !g_ot.complete)) {
        return ESP_ERR_INVALID_STATE;
    }
    g_ot.thread_enabled = true;
    return ESP_OK;
}

static void f_thread_stop(void *ctx)
{
    (void)ctx;
    g_ot.thread_stop_calls++;
    g_ot.thread_enabled = false;
}

static bool f_read_dataset(pb_enroll_dataset_info_t *out, void *ctx)
{
    (void)ctx;
    out->has_network_key = g_ot.has_key;
    out->complete = g_ot.complete;
    if (g_ot.complete) {
        out->id = g_ot.id;
    }
    return true;
}

static const pb_enroll_ops_t k_ot_ops = {
    .joiner_start = f_joiner_start,
    .joiner_stop = f_joiner_stop,
    .thread_start_stored = f_thread_start_stored,
    .thread_stop = f_thread_stop,
    .read_dataset = f_read_dataset,
    .ctx = NULL,
};

/* ---- the worker's view of the real controller ---- */

static pb_enroll_ctl_t g_ctl;
static uint32_t g_now;
static int g_lock_depth;
static int g_ctl_calls_under_lock;

/* What the worker handed to the controller with the last start. */
static int g_join_calls;
static bool g_join_had_endpoint;
static pb_enroll_bridge_endpoint_t g_join_endpoint;
static bool g_join_had_mgmt;
static pb_enroll_mgmt_credential_t g_join_mgmt;

static esp_err_t w_request_join(char pskd[PB_ENROLL_PSKD_BUF_LEN], const pb_enroll_bridge_endpoint_t *endpoint,
                                const pb_enroll_mgmt_credential_t *mgmt,
                                void *ctx)
{
    (void)ctx;
    if (g_lock_depth != 0) {
        g_ctl_calls_under_lock++;
    }
    g_join_calls++;
    g_join_had_endpoint = endpoint != NULL;
    g_join_had_mgmt = mgmt != NULL;
    memset(&g_join_endpoint, 0, sizeof(g_join_endpoint));
    memset(&g_join_mgmt, 0, sizeof(g_join_mgmt));
    if (mgmt != NULL) {
        g_join_mgmt = *mgmt;
        if (endpoint != NULL) {
            g_join_endpoint = *endpoint;
        }
        return pb_enroll_ctl_request_join_managed(&g_ctl, pskd, endpoint, mgmt, g_now);
    }
    if (endpoint != NULL) {
        g_join_endpoint = *endpoint;
        return pb_enroll_ctl_request_join_with_endpoint(&g_ctl, pskd, endpoint, g_now);
    }
    return pb_enroll_ctl_request_join(&g_ctl, pskd, g_now);
}

static esp_err_t w_cancel(void *ctx)
{
    (void)ctx;
    if (g_lock_depth != 0) {
        g_ctl_calls_under_lock++;
    }
    return pb_enroll_ctl_cancel(&g_ctl);
}

static void w_get_status(pb_ble_ctl_status_t *out, void *ctx)
{
    (void)ctx;
    if (g_lock_depth != 0) {
        g_ctl_calls_under_lock++;
    }
    out->state = g_ctl.state;
    out->last = g_ctl.last;
    out->attempt = g_ctl.attempt;
}

static void w_lock(void *ctx)
{
    (void)ctx;
    g_lock_depth++;
}

static void w_unlock(void *ctx)
{
    (void)ctx;
    g_lock_depth--;
}

static const pb_ble_worker_ops_t k_worker = {
    .request_join = w_request_join,
    .cancel = w_cancel,
    .get_status = w_get_status,
    .lock = w_lock,
    .unlock = w_unlock,
    .ctx = NULL,
};

static pb_ble_adm_t g_adm;

/* An unregistered board in the safe wait with BLE enrollment open. */
static void fresh(void)
{
    nvs_sim_reset();
    memset(&g_ot, 0, sizeof(g_ot));
    g_now = 1000;
    g_rand_fails = false;
    g_rand_fixed = NULL;
    g_lock_depth = 0;
    g_ctl_calls_under_lock = 0;
    g_join_calls = 0;
    g_join_had_endpoint = false;
    memset(&g_join_endpoint, 0, sizeof(g_join_endpoint));
    pb_enroll_ctl_init(&g_ctl, &k_ot_ops, EUI_A);
    pb_enroll_ctl_boot(&g_ctl);
    pb_ble_adm_init(&g_adm, &k_adm_ops, EUI_A, true, g_now);
    pb_ble_adm_session_changed(&g_adm); /* a client connected */
}

static pb_ble_close_reason_t poll(void)
{
    return pb_ble_adm_worker_poll(&g_adm, &k_worker, g_now);
}

/* ---- message builders (the client side of pb_ble_proto.h) ---- */

static void make_id(uint8_t id[16], uint8_t seed)
{
    for (int i = 0; i < 16; i++) {
        id[i] = (uint8_t)(seed + 7 * i + 1);
    }
}

static size_t build_status(uint8_t out[PB_BLE_STATUS_REQ_LEN], bool issue, const uint8_t *request_id)
{
    memset(out, 0, PB_BLE_STATUS_REQ_LEN);
    out[0] = PB_BLE_PROTO_VERSION;
    out[1] = PB_BLE_MSG_STATUS_REQ;
    out[2] = issue ? PB_BLE_STATUS_REQ_F_ISSUE_CHALLENGE : 0;
    if (request_id != NULL) {
        memcpy(&out[4], request_id, 16);
    }
    return PB_BLE_STATUS_REQ_LEN;
}

static size_t build_start(uint8_t out[PB_BLE_START_REQ_MAX_LEN + 8], const uint8_t target[8],
                          const uint8_t request_id[16], const uint8_t challenge[16], const char *pskd)
{
    size_t n = strlen(pskd);
    memset(out, 0, PB_BLE_START_REQ_MAX_LEN + 8);
    out[0] = PB_BLE_PROTO_VERSION;
    out[1] = PB_BLE_MSG_START_REQ;
    out[2] = (uint8_t)n;
    memcpy(&out[4], target, 8);
    memcpy(&out[12], request_id, 16);
    memcpy(&out[28], challenge, 16);
    memcpy(&out[44], pskd, n);
    return 44 + n;
}

static size_t build_cancel(uint8_t out[PB_BLE_CANCEL_REQ_LEN], const uint8_t target[8], const uint8_t request_id[16])
{
    memset(out, 0, PB_BLE_CANCEL_REQ_LEN);
    out[0] = PB_BLE_PROTO_VERSION;
    out[1] = PB_BLE_MSG_CANCEL_REQ;
    memcpy(&out[4], target, 8);
    memcpy(&out[12], request_id, 16);
    return PB_BLE_CANCEL_REQ_LEN;
}

/* pb-status with "issue a challenge"; returns the response code. */
static uint8_t get_challenge(uint8_t challenge[16])
{
    uint8_t req[PB_BLE_STATUS_REQ_LEN];
    uint8_t resp[PB_BLE_STATUS_RESP_LEN];
    build_status(req, true, NULL);
    pb_ble_adm_handle_status(&g_adm, req, sizeof(req), resp, g_now);
    memcpy(challenge, &resp[24], 16);
    return resp[2];
}

static uint8_t send_start(const uint8_t target[8], const uint8_t request_id[16], const uint8_t challenge[16],
                          const char *pskd)
{
    uint8_t req[PB_BLE_START_REQ_MAX_LEN + 8];
    uint8_t resp[PB_BLE_SHORT_RESP_LEN];
    size_t n = build_start(req, target, request_id, challenge, pskd);
    pb_ble_adm_handle_start(&g_adm, req, n, resp, g_now);
    return resp[2];
}

/* Fresh challenge + start. */
static uint8_t start_new(const uint8_t request_id[16], const char *pskd)
{
    uint8_t ch[16];
    get_challenge(ch);
    return send_start(EUI_A, request_id, ch, pskd);
}

static uint8_t send_cancel(const uint8_t target[8], const uint8_t request_id[16])
{
    uint8_t req[PB_BLE_CANCEL_REQ_LEN];
    uint8_t resp[PB_BLE_SHORT_RESP_LEN];
    build_cancel(req, target, request_id);
    pb_ble_adm_handle_cancel(&g_adm, req, sizeof(req), resp);
    return resp[2];
}

/* pb-status asking about `request_id`, no challenge. */
static void query(const uint8_t *request_id, uint8_t resp[PB_BLE_STATUS_RESP_LEN])
{
    uint8_t req[PB_BLE_STATUS_REQ_LEN];
    build_status(req, false, request_id);
    pb_ble_adm_handle_status(&g_adm, req, sizeof(req), resp, g_now);
}

static bool all_zero(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        if (b[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool nothing_started(void)
{
    return g_ot.joiner_start_calls == 0 && g_ctl.state == PB_ENROLL_STATE_WAITING && !g_adm.pskd_pending &&
           g_adm.q_count == 0 && all_zero(g_adm.pskd, sizeof(g_adm.pskd));
}

/* ---- credential record ---- */

static uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static void seal(uint8_t rec[PB_BLE_AUTH_RECORD_LEN])
{
    uint32_t crc = crc32_ieee(rec, 416);
    rec[416] = (uint8_t)crc;
    rec[417] = (uint8_t)(crc >> 8);
    rec[418] = (uint8_t)(crc >> 16);
    rec[419] = (uint8_t)(crc >> 24);
}

/* A well-formed record with made-up salt/verifier bytes (not derived from
 * any code -- this file never runs SRP). */
static void make_record(uint8_t rec[PB_BLE_AUTH_RECORD_LEN], const uint8_t eui[8])
{
    memset(rec, 0, PB_BLE_AUTH_RECORD_LEN);
    memcpy(rec, "PBA1", 4);
    rec[4] = PB_BLE_AUTH_FORMAT_VERSION;
    rec[5] = PB_BLE_AUTH_SRP_GROUP_SEC2_3072_SHA512;
    rec[6] = PB_BLE_AUTH_SALT_LEN;
    memcpy(&rec[8], eui, 8);
    for (int i = 0; i < 16; i++) {
        rec[16 + i] = (uint8_t)(0xa0 + i);
    }
    for (int i = 0; i < 384; i++) {
        rec[32 + i] = (uint8_t)(3 * i + 5);
    }
    seal(rec);
}

static void scenario_credential_record(void)
{
    printf("\n-- setup credential record: usable only when intact and made for this EUI-64 --\n");
    uint8_t rec[PB_BLE_AUTH_RECORD_LEN];
    pb_ble_auth_cred_t cred;

    make_record(rec, EUI_A);
    CHECK(pb_ble_auth_parse(rec, sizeof(rec), EUI_A, &cred) == PB_BLE_AUTH_OK, "intact record for this board: ok");
    CHECK(memcmp(cred.salt, &rec[16], 16) == 0 && memcmp(cred.verifier, &rec[32], 384) == 0,
          "salt and verifier are the record's bytes");

    CHECK(pb_ble_auth_parse(rec, sizeof(rec), EUI_B, &cred) == PB_BLE_AUTH_FOREIGN && all_zero(&cred, sizeof(cred)),
          "record made for another EUI-64: refused, nothing returned");
    CHECK(pb_ble_auth_parse(rec, sizeof(rec) - 1, EUI_A, &cred) == PB_BLE_AUTH_CORRUPT, "truncated record: corrupt");
    CHECK(pb_ble_auth_parse(NULL, 0, EUI_A, &cred) == PB_BLE_AUTH_CORRUPT, "no bytes: corrupt");

    make_record(rec, EUI_A);
    rec[40] ^= 0x01;
    CHECK(pb_ble_auth_parse(rec, sizeof(rec), EUI_A, &cred) == PB_BLE_AUTH_CORRUPT && all_zero(&cred, sizeof(cred)),
          "one flipped verifier bit (CRC mismatch): corrupt");
    make_record(rec, EUI_A);
    rec[20] ^= 0x80;
    CHECK(pb_ble_auth_parse(rec, sizeof(rec), EUI_A, &cred) == PB_BLE_AUTH_CORRUPT,
          "one flipped salt bit (CRC mismatch): corrupt");
    make_record(rec, EUI_A);
    rec[0] = 'X';
    seal(rec);
    CHECK(pb_ble_auth_parse(rec, sizeof(rec), EUI_A, &cred) == PB_BLE_AUTH_CORRUPT, "wrong magic: corrupt");
    make_record(rec, EUI_A);
    rec[4] = 2;
    seal(rec);
    CHECK(pb_ble_auth_parse(rec, sizeof(rec), EUI_A, &cred) == PB_BLE_AUTH_UNSUPPORTED_VERSION,
          "format version 2: unsupported, not guessed");
    make_record(rec, EUI_A);
    rec[5] = 9;
    seal(rec);
    CHECK(pb_ble_auth_parse(rec, sizeof(rec), EUI_A, &cred) == PB_BLE_AUTH_UNSUPPORTED_VERSION,
          "unknown SRP group: unsupported");
    make_record(rec, EUI_A);
    memset(&rec[16], 0, 16);
    seal(rec);
    CHECK(pb_ble_auth_parse(rec, sizeof(rec), EUI_A, &cred) == PB_BLE_AUTH_CORRUPT, "all-zero salt: corrupt");
    make_record(rec, EUI_A);
    memset(&rec[32], 0, 384);
    seal(rec);
    CHECK(pb_ble_auth_parse(rec, sizeof(rec), EUI_A, &cred) == PB_BLE_AUTH_CORRUPT,
          "verifier 0 (would accept any code): corrupt");
    rec[32 + 383] = 1;
    seal(rec);
    CHECK(pb_ble_auth_parse(rec, sizeof(rec), EUI_A, &cred) == PB_BLE_AUTH_CORRUPT,
          "verifier 1 (would accept any code): corrupt");
    make_record(rec, EUI_A);
    rec[7] = 1;
    seal(rec);
    CHECK(pb_ble_auth_parse(rec, sizeof(rec), EUI_A, &cred) == PB_BLE_AUTH_CORRUPT, "non-zero reserved byte: corrupt");

    printf("\n-- the same rules through the cfg partition --\n");
    nvs_sim_reset();
    CHECK(pb_ble_auth_load(EUI_A, &cred) == PB_BLE_AUTH_ABSENT, "no pb_ble_auth namespace: absent");
    nvs_sim_create_namespace("cfg", PB_BLE_AUTH_NVS_NAMESPACE);
    CHECK(pb_ble_auth_load(EUI_A, &cred) == PB_BLE_AUTH_ABSENT, "namespace without the key: absent");
    make_record(rec, EUI_A);
    nvs_sim_put_blob("cfg", PB_BLE_AUTH_NVS_NAMESPACE, PB_BLE_AUTH_NVS_KEY, rec, sizeof(rec));
    CHECK(pb_ble_auth_load(EUI_A, &cred) == PB_BLE_AUTH_OK && memcmp(cred.verifier, &rec[32], 384) == 0,
          "stored record loads");
    CHECK(pb_ble_auth_load(EUI_B, &cred) == PB_BLE_AUTH_FOREIGN, "a cfg image copied from another board: refused");
    nvs_sim_put_blob("cfg", PB_BLE_AUTH_NVS_NAMESPACE, PB_BLE_AUTH_NVS_KEY, rec, 100);
    CHECK(pb_ble_auth_load(EUI_A, &cred) == PB_BLE_AUTH_CORRUPT, "blob of the wrong size: corrupt");
    nvs_sim_force_next_init_error(ESP_FAIL);
    CHECK(pb_ble_auth_load(EUI_A, &cred) == PB_BLE_AUTH_READ_ERROR && all_zero(&cred, sizeof(cred)),
          "cfg partition unreadable: read error, nothing returned");

    char user[PB_BLE_AUTH_USERNAME_BUF_LEN];
    pb_ble_auth_username(EUI_A, user);
    CHECK(strcmp(user, "6055f9fffe001234") == 0, "SRP username is the EUI-64 as 16 lower-case hex digits");
}

/* ---- scenarios ---- */

static void scenario_status(void)
{
    printf("\n-- pb-status: identity, state, challenge --\n");
    fresh();
    uint8_t req[PB_BLE_STATUS_REQ_LEN + 4];
    uint8_t resp[PB_BLE_STATUS_RESP_LEN];

    build_status(req, false, NULL);
    size_t n = pb_ble_adm_handle_status(&g_adm, req, PB_BLE_STATUS_REQ_LEN, resp, g_now);
    CHECK(n == PB_BLE_STATUS_RESP_LEN && resp[0] == 1 && resp[1] == PB_BLE_MSG_STATUS_RESP && resp[2] == PB_BLE_CODE_OK,
          "well-formed status answered");
    CHECK(memcmp(&resp[4], EUI_A, 8) == 0, "the board's real EUI-64 is reported");
    CHECK(resp[12] == PB_ENROLL_STATE_WAITING && resp[13] == PB_ENROLL_RESULT_NONE && resp[14] == 0,
          "waiting, no result, no attempt");
    CHECK(all_zero(&resp[24], 16) && resp[18] == 0 && resp[19] == 0 && !g_adm.ch_valid,
          "no challenge unless asked for");

    uint8_t ch1[16], ch2[16];
    CHECK(get_challenge(ch1) == PB_BLE_CODE_OK && !all_zero(ch1, 16) && g_adm.ch_valid, "challenge issued on request");
    build_status(req, true, NULL);
    pb_ble_adm_handle_status(&g_adm, req, PB_BLE_STATUS_REQ_LEN, resp, g_now);
    memcpy(ch2, &resp[24], 16);
    CHECK((resp[14] & PB_BLE_STATUS_F_CHALLENGE_ISSUED) != 0 && resp[18] == 0 && resp[19] == 60,
          "challenge flag set, lifetime 60 s reported");
    CHECK(memcmp(ch1, ch2, 16) != 0 && memcmp(g_adm.ch, ch2, 16) == 0, "a new challenge replaces the previous one");

    g_rand_fails = true;
    CHECK(get_challenge(ch1) == PB_BLE_CODE_INTERNAL && !g_adm.ch_valid && all_zero(ch1, 16),
          "no random bytes: no challenge, and the old one is gone");
    g_rand_fails = false;

    printf("\n-- pb-status: malformed and unknown-version requests --\n");
    build_status(req, false, NULL);
    pb_ble_adm_handle_status(&g_adm, req, PB_BLE_STATUS_REQ_LEN - 1, resp, g_now);
    CHECK(resp[2] == PB_BLE_CODE_MALFORMED && all_zero(&resp[4], 36), "truncated: malformed, no identity returned");
    pb_ble_adm_handle_status(&g_adm, req, PB_BLE_STATUS_REQ_LEN + 1, resp, g_now);
    CHECK(resp[2] == PB_BLE_CODE_MALFORMED, "one byte too long: malformed");
    pb_ble_adm_handle_status(&g_adm, req, 0, resp, g_now);
    CHECK(resp[2] == PB_BLE_CODE_MALFORMED, "empty: malformed");
    build_status(req, false, NULL);
    req[1] = PB_BLE_MSG_START_REQ;
    pb_ble_adm_handle_status(&g_adm, req, PB_BLE_STATUS_REQ_LEN, resp, g_now);
    CHECK(resp[2] == PB_BLE_CODE_MALFORMED, "another message type on this endpoint: malformed");
    build_status(req, false, NULL);
    req[2] = 0x02;
    pb_ble_adm_handle_status(&g_adm, req, PB_BLE_STATUS_REQ_LEN, resp, g_now);
    CHECK(resp[2] == PB_BLE_CODE_MALFORMED, "unknown flag bit: malformed");
    build_status(req, false, NULL);
    req[3] = 1;
    pb_ble_adm_handle_status(&g_adm, req, PB_BLE_STATUS_REQ_LEN, resp, g_now);
    CHECK(resp[2] == PB_BLE_CODE_MALFORMED, "non-zero reserved byte: malformed");
    build_status(req, true, NULL);
    req[0] = 2;
    pb_ble_adm_handle_status(&g_adm, req, PB_BLE_STATUS_REQ_LEN, resp, g_now);
    CHECK(resp[0] == 1 && resp[2] == PB_BLE_CODE_UNSUPPORTED_VERSION && all_zero(&resp[4], 36) && !g_adm.ch_valid,
          "version 2: unsupported, answered as version 1, no challenge issued");
}

static void scenario_start_accepted(void)
{
    printf("\n-- authenticated start for this EUI-64: accepted, then run by the worker --\n");
    fresh();
    uint8_t id[16], ch[16];
    make_id(id, 1);
    get_challenge(ch);

    uint8_t req[PB_BLE_START_REQ_MAX_LEN + 8];
    uint8_t resp[PB_BLE_SHORT_RESP_LEN];
    size_t n = build_start(req, EUI_A, id, ch, PSKD);
    size_t rn = pb_ble_adm_handle_start(&g_adm, req, n, resp, g_now);
    CHECK(rn == PB_BLE_SHORT_RESP_LEN && resp[0] == 1 && resp[1] == PB_BLE_MSG_START_RESP &&
              resp[2] == PB_BLE_CODE_ACCEPTED,
          "start accepted");
    CHECK(all_zero(req, n), "the decrypted request buffer (holding the PSKd) is wiped");
    CHECK(g_ot.joiner_start_calls == 0 && g_ctl.state == PB_ENROLL_STATE_WAITING,
          "accepted does not mean started: the BLE callback did not touch the Joiner");
    CHECK(g_adm.q_count == 1 && g_adm.pskd_pending && !g_adm.ch_valid, "one queued command; the challenge is spent");

    uint8_t st[PB_BLE_STATUS_RESP_LEN];
    query(id, st);
    CHECK((st[14] & PB_BLE_STATUS_F_ATTEMPT_ACTIVE) && (st[14] & PB_BLE_STATUS_F_REQUEST_MATCH) &&
              st[15] == PB_BLE_PHASE_QUEUED,
          "status: this request is queued");

    CHECK(poll() == PB_BLE_OPEN, "worker pass: BLE stays open");
    CHECK(g_ot.joiner_start_calls == 1 && strcmp(g_ot.last_pskd, PSKD) == 0, "the worker started the Joiner with that PSKd");
    CHECK(g_ctl_calls_under_lock == 0 && g_lock_depth == 0,
          "the controller was never called with the BLE-side lock held");
    CHECK(!g_adm.pskd_pending && all_zero(g_adm.pskd, sizeof(g_adm.pskd)), "the PSKd slot is wiped");
    CHECK(g_ctl.state == PB_ENROLL_STATE_JOINING && g_adm.rec_phase == PB_BLE_REC_RUNNING, "controller is joining");
    query(id, st);
    CHECK(st[12] == PB_ENROLL_STATE_JOINING && st[15] == PB_BLE_PHASE_RUNNING && st[23] == 1,
          "status: joining, this request running, attempt 1");
}

static void scenario_start_rejections(void)
{
    printf("\n-- requests that must be refused before any Joiner starts --\n");
    uint8_t id[16], ch[16];
    make_id(id, 2);

    fresh();
    get_challenge(ch);
    CHECK(send_start(EUI_B, id, ch, PSKD) == PB_BLE_CODE_WRONG_TARGET, "request for another board's EUI-64 refused");
    poll();
    CHECK(nothing_started(), "wrong target: nothing queued, nothing started");
    CHECK(send_start(EUI_A, id, ch, PSKD) == PB_BLE_CODE_CHALLENGE_INVALID,
          "the challenge that request carried cannot be used again");

    fresh();
    memset(ch, 0x5a, sizeof(ch));
    CHECK(send_start(EUI_A, id, ch, PSKD) == PB_BLE_CODE_CHALLENGE_INVALID, "start without any issued challenge refused");
    get_challenge(ch);
    ch[3] ^= 1;
    CHECK(send_start(EUI_A, id, ch, PSKD) == PB_BLE_CODE_CHALLENGE_INVALID, "wrong challenge value refused");
    ch[3] ^= 1;
    CHECK(send_start(EUI_A, id, ch, PSKD) == PB_BLE_CODE_CHALLENGE_INVALID,
          "the right value after a wrong guess: already spent");
    poll();
    CHECK(nothing_started(), "challenge failures: nothing started");

    fresh();
    get_challenge(ch);
    g_now += PB_BLE_CHALLENGE_TTL_MS - 1;
    CHECK(send_start(EUI_A, id, ch, PSKD) == PB_BLE_CODE_ACCEPTED, "challenge used 1 ms before it expires: accepted");
    fresh();
    get_challenge(ch);
    g_now += PB_BLE_CHALLENGE_TTL_MS;
    CHECK(send_start(EUI_A, id, ch, PSKD) == PB_BLE_CODE_CHALLENGE_EXPIRED, "challenge at its lifetime: expired");
    poll();
    CHECK(nothing_started(), "expired challenge: nothing started");
    fresh();
    g_now = 0xffffff00u; /* the 32-bit millisecond clock is about to wrap */
    get_challenge(ch);
    g_now += 1000;
    CHECK(send_start(EUI_A, id, ch, PSKD) == PB_BLE_CODE_ACCEPTED, "challenge age is computed across the clock wrap");

    fresh();
    get_challenge(ch);
    pb_ble_adm_session_changed(&g_adm); /* disconnect */
    pb_ble_adm_session_changed(&g_adm); /* the next client connects */
    CHECK(send_start(EUI_A, id, ch, PSKD) == PB_BLE_CODE_CHALLENGE_INVALID,
          "a challenge from the previous connection refused");
    fresh();
    get_challenge(ch);
    CHECK(pb_ble_adm_sec_request_begin(&g_adm), "(a new Security 2 handshake starts on the same connection)");
    pb_ble_adm_sec_request_end(&g_adm, true);
    CHECK(send_start(EUI_A, id, ch, PSKD) == PB_BLE_CODE_CHALLENGE_INVALID,
          "a challenge from before a re-handshake refused");
    poll();
    CHECK(nothing_started(), "stale-session challenges: nothing started");

    printf("\n-- malformed start messages --\n");
    uint8_t req[PB_BLE_START_REQ_MAX_LEN + 8];
    uint8_t resp[PB_BLE_SHORT_RESP_LEN];
    size_t n;

    fresh();
    get_challenge(ch);
    n = build_start(req, EUI_A, id, ch, PSKD);
    pb_ble_adm_handle_start(&g_adm, req, n - 1, resp, g_now);
    CHECK(resp[2] == PB_BLE_CODE_MALFORMED && all_zero(req, n - 1), "one byte short: malformed, buffer wiped");
    n = build_start(req, EUI_A, id, ch, PSKD);
    pb_ble_adm_handle_start(&g_adm, req, n + 1, resp, g_now);
    CHECK(resp[2] == PB_BLE_CODE_MALFORMED, "one byte long: malformed");
    n = build_start(req, EUI_A, id, ch, PSKD);
    pb_ble_adm_handle_start(&g_adm, req, 43, resp, g_now);
    CHECK(resp[2] == PB_BLE_CODE_MALFORMED, "cut inside the header: malformed");
    pb_ble_adm_handle_start(&g_adm, req, 0, resp, g_now);
    CHECK(resp[2] == PB_BLE_CODE_MALFORMED, "empty: malformed");
    memset(req, 'A', sizeof(req));
    req[0] = PB_BLE_PROTO_VERSION;
    req[1] = PB_BLE_MSG_START_REQ;
    req[2] = 33;
    req[3] = 0;
    pb_ble_adm_handle_start(&g_adm, req, 77, resp, g_now);
    CHECK(resp[2] == PB_BLE_CODE_MALFORMED && all_zero(req, 77), "33-character PSKd (77 bytes): oversized, wiped");
    n = build_start(req, EUI_A, id, ch, PSKD);
    req[0] = 2;
    pb_ble_adm_handle_start(&g_adm, req, n, resp, g_now);
    CHECK(resp[0] == 1 && resp[2] == PB_BLE_CODE_UNSUPPORTED_VERSION && all_zero(req, n),
          "version 2: unsupported, never interpreted, wiped");
    n = build_start(req, EUI_A, id, ch, PSKD);
    req[1] = PB_BLE_MSG_CANCEL_REQ;
    pb_ble_adm_handle_start(&g_adm, req, n, resp, g_now);
    CHECK(resp[2] == PB_BLE_CODE_MALFORMED, "wrong message type: malformed");
    n = build_start(req, EUI_A, id, ch, PSKD);
    req[3] = 7;
    pb_ble_adm_handle_start(&g_adm, req, n, resp, g_now);
    CHECK(resp[2] == PB_BLE_CODE_MALFORMED, "non-zero reserved byte: malformed");
    uint8_t zero_id[16] = {0};
    n = build_start(req, EUI_A, zero_id, ch, PSKD);
    pb_ble_adm_handle_start(&g_adm, req, n, resp, g_now);
    CHECK(resp[2] == PB_BLE_CODE_MALFORMED, "all-zero request id: malformed");
    CHECK(g_adm.ch_valid, "malformed messages do not consume the challenge");
    poll();
    CHECK(nothing_started(), "malformed messages: nothing started");

    printf("\n-- invalid PSKd --\n");
    static const char *bad[] = {"j01nme7k", "J01NM", "J01NMEI7", "J01 ME7K", "J01NME7Z"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        fresh();
        CHECK(start_new(id, bad[i]) == PB_BLE_CODE_INVALID_PSKD, "PSKd outside the Thread rule refused");
        poll();
        CHECK(nothing_started(), "invalid PSKd: nothing started");
    }

    printf("\n-- board that cannot run a Joiner --\n");
    fresh();
    pb_ble_adm_init(&g_adm, &k_adm_ops, EUI_A, false, g_now);
    uint8_t st[PB_BLE_STATUS_RESP_LEN];
    query(NULL, st);
    CHECK((st[14] & PB_BLE_STATUS_F_JOINER_UNAVAILABLE) != 0, "status says the Joiner is unavailable");
    CHECK(start_new(id, PSKD) == PB_BLE_CODE_UNAVAILABLE, "start refused");
    poll();
    CHECK(nothing_started(), "unavailable: nothing started");
}

static void scenario_duplicates_and_concurrency(void)
{
    printf("\n-- the same request again does not start a second Joiner --\n");
    fresh();
    uint8_t id[16], other[16], ch[16];
    make_id(id, 3);
    make_id(other, 90);
    get_challenge(ch);
    CHECK(send_start(EUI_A, id, ch, PSKD) == PB_BLE_CODE_ACCEPTED, "first copy accepted");
    CHECK(send_start(EUI_A, id, ch, PSKD) == PB_BLE_CODE_DUPLICATE, "identical retransmission: duplicate");
    CHECK(g_adm.q_count == 1 && g_adm.rec_gen == 1, "still one queued command, one generation");
    poll();
    CHECK(g_ot.joiner_start_calls == 1, "exactly one Joiner start");
    CHECK(send_start(EUI_A, id, ch, PSKD) == PB_BLE_CODE_DUPLICATE, "retransmission while the Joiner runs: duplicate");
    poll();
    CHECK(g_ot.joiner_start_calls == 1 && g_ctl.attempt == 1, "still exactly one Joiner start");

    printf("\n-- the same request id with other content --\n");
    CHECK(send_start(EUI_A, id, ch, PSKD2) == PB_BLE_CODE_REQUEST_CONFLICT, "same id, different PSKd: refused");
    uint8_t ch2[16];
    get_challenge(ch2);
    CHECK(send_start(EUI_A, id, ch2, PSKD) == PB_BLE_CODE_REQUEST_CONFLICT, "same id, different challenge: refused");
    CHECK(send_start(EUI_B, id, ch, PSKD) == PB_BLE_CODE_REQUEST_CONFLICT, "same id, different target: refused");
    CHECK(g_adm.ch_valid, "none of these consumed the new challenge");

    printf("\n-- another request while one is in progress --\n");
    CHECK(send_start(EUI_A, other, ch2, PSKD2) == PB_BLE_CODE_BUSY, "a different request id: busy");
    pb_ble_adm_session_changed(&g_adm);
    pb_ble_adm_session_changed(&g_adm); /* a second client, authenticated with the same setup code */
    CHECK(start_new(other, PSKD2) == PB_BLE_CODE_BUSY, "the second client's start: busy");
    poll();
    CHECK(g_ot.joiner_start_calls == 1 && strcmp(g_ot.last_pskd, PSKD) == 0 &&
              memcmp(g_adm.rec_request_id, id, 16) == 0 && g_adm.rec_phase == PB_BLE_REC_RUNNING,
          "the attempt in progress is untouched");
    uint8_t st[PB_BLE_STATUS_RESP_LEN];
    query(other, st);
    CHECK((st[14] & PB_BLE_STATUS_F_ATTEMPT_ACTIVE) && !(st[14] & PB_BLE_STATUS_F_REQUEST_MATCH) && st[15] == 0 &&
              all_zero(&st[24], 16),
          "the second client sees that an attempt is active, not whose");
    CHECK(send_cancel(EUI_A, other) == PB_BLE_CODE_NO_SUCH_ATTEMPT, "the second client cannot cancel it");
    poll();
    CHECK(g_ot.joiner_stop_calls == 0 && g_ctl.state == PB_ENROLL_STATE_JOINING, "the Joiner keeps running");
}

static void scenario_disconnect(void)
{
    printf("\n-- BLE disconnect does not cancel an accepted attempt --\n");
    fresh();
    uint8_t id[16];
    make_id(id, 4);
    CHECK(start_new(id, PSKD) == PB_BLE_CODE_ACCEPTED, "start accepted");
    pb_ble_adm_session_changed(&g_adm); /* disconnect before the worker even ran */
    CHECK(!g_adm.ch_valid && all_zero(g_adm.ch, 16), "session secrets (challenge) are dropped");
    poll();
    CHECK(g_ot.joiner_start_calls == 1 && g_ctl.state == PB_ENROLL_STATE_JOINING,
          "the accepted start still runs after the disconnect");
    pb_ble_adm_session_changed(&g_adm);
    g_now += 30000;
    poll();
    CHECK(g_ot.joiner_stop_calls == 0 && g_ctl.state == PB_ENROLL_STATE_JOINING, "the Joiner keeps running");

    pb_ble_adm_session_changed(&g_adm); /* the same app reconnects */
    uint8_t st[PB_BLE_STATUS_RESP_LEN];
    query(id, st);
    CHECK((st[14] & PB_BLE_STATUS_F_REQUEST_MATCH) && st[15] == PB_BLE_PHASE_RUNNING,
          "after reconnecting, the request id still names the attempt");

    g_now += PB_ENROLL_JOINER_GUARD_MS;
    pb_enroll_ctl_tick(&g_ctl, false, g_now);
    poll();
    query(id, st);
    CHECK(g_ctl.state == PB_ENROLL_STATE_WAITING && st[15] == PB_BLE_PHASE_FAILED &&
              st[16] == PB_ENROLL_RESULT_JOINER_GUARD_EXPIRED,
          "the attempt ends by the controller's own guard, not by the disconnect");
}

static void scenario_cancel(void)
{
    printf("\n-- explicit cancel --\n");
    uint8_t id[16], other[16];
    make_id(id, 5);
    make_id(other, 77);
    uint8_t st[PB_BLE_STATUS_RESP_LEN];

    fresh();
    CHECK(send_cancel(EUI_A, id) == PB_BLE_CODE_NO_SUCH_ATTEMPT, "cancel with no attempt: nothing to cancel");
    start_new(id, PSKD);
    CHECK(send_cancel(EUI_B, id) == PB_BLE_CODE_WRONG_TARGET, "cancel for another board refused");
    CHECK(send_cancel(EUI_A, other) == PB_BLE_CODE_NO_SUCH_ATTEMPT, "cancel with another request id refused");
    CHECK(g_adm.rec_phase == PB_BLE_REC_QUEUED && g_adm.pskd_pending, "the queued attempt is untouched");
    CHECK(send_cancel(EUI_A, id) == PB_BLE_CODE_OK, "cancel of the queued attempt by its own request id");
    CHECK(!g_adm.pskd_pending && all_zero(g_adm.pskd, sizeof(g_adm.pskd)) && all_zero(g_adm.rec_tag, 32),
          "its PSKd and digest are wiped at once");
    CHECK(send_cancel(EUI_A, id) == PB_BLE_CODE_OK, "repeated cancel is harmless");
    poll();
    CHECK(g_ot.joiner_start_calls == 0 && g_ctl.state == PB_ENROLL_STATE_WAITING && g_adm.q_count == 0,
          "the worker drops the stale start: no Joiner ever ran");
    query(id, st);
    CHECK(st[15] == PB_BLE_PHASE_CANCELLED, "status: cancelled");

    fresh();
    start_new(id, PSKD);
    poll();
    CHECK(send_cancel(EUI_A, id) == PB_BLE_CODE_OK && g_ot.joiner_stop_calls == 0,
          "cancel while joining: taken, but the BLE callback did not stop the Joiner itself");
    CHECK(send_cancel(EUI_A, id) == PB_BLE_CODE_OK && g_adm.q_count == 1, "repeated cancel queues nothing more");
    poll();
    CHECK(g_ot.joiner_stop_calls == 1 && g_ctl.state == PB_ENROLL_STATE_WAITING &&
              g_ctl.last == PB_ENROLL_RESULT_CANCELLED,
          "the worker cancelled through the existing controller");
    query(id, st);
    CHECK(st[15] == PB_BLE_PHASE_CANCELLED && st[16] == PB_ENROLL_RESULT_CANCELLED && !(st[14] & 1),
          "status: cancelled, no attempt active");

    printf("\n-- cancel after Joiner success (attaching) --\n");
    fresh();
    start_new(id, PSKD);
    poll();
    g_ot.has_key = true;
    pb_enroll_ctl_on_joiner_done(&g_ctl, g_ot.last_attempt, PB_ENROLL_JOINER_OK, g_now);
    poll();
    CHECK(g_ctl.state == PB_ENROLL_STATE_ATTACHING && g_ot.thread_enabled, "(attaching)");
    CHECK(send_cancel(EUI_A, id) == PB_BLE_CODE_OK, "cancel taken");
    poll();
    CHECK(g_ctl.state == PB_ENROLL_STATE_WAITING && !g_ot.thread_enabled, "attach abandoned, Thread stopped, waiting");

    printf("\n-- cancel never undoes a finished enrollment --\n");
    fresh();
    start_new(id, PSKD);
    poll();
    g_ot.has_key = true;
    pb_enroll_ctl_on_joiner_done(&g_ctl, g_ot.last_attempt, PB_ENROLL_JOINER_OK, g_now);
    poll();
    CHECK(send_cancel(EUI_A, id) == PB_BLE_CODE_OK, "(cancel queued while still attaching)");
    g_ot.complete = true;
    g_ot.id = NET_OTBR;
    pb_enroll_ctl_tick(&g_ctl, true, g_now); /* enrolled before the worker got to the cancel */
    CHECK(g_ctl.state == PB_ENROLL_STATE_ENROLLED, "(enrolled, marker stored)");
    poll();
    pb_enroll_network_id_t nid;
    CHECK(g_ctl.state == PB_ENROLL_STATE_ENROLLED && g_ot.thread_enabled && g_ot.thread_stop_calls == 0 &&
              pb_enroll_marker_load(EUI_A, &nid) == PB_ENROLL_MARKER_VALID,
          "the late cancel changed nothing: enrolled, Thread up, marker valid");
    query(id, st);
    CHECK(st[2] == PB_BLE_CODE_CLOSED && st[15] == PB_BLE_PHASE_ENROLLED, "status: enrolled, BLE enrollment closed");
    CHECK(send_cancel(EUI_A, id) == PB_BLE_CODE_CLOSED, "a cancel after enrollment is refused");

    printf("\n-- malformed cancel --\n");
    fresh();
    start_new(id, PSKD);
    uint8_t req[PB_BLE_CANCEL_REQ_LEN + 2];
    uint8_t resp[PB_BLE_SHORT_RESP_LEN];
    build_cancel(req, EUI_A, id);
    pb_ble_adm_handle_cancel(&g_adm, req, PB_BLE_CANCEL_REQ_LEN - 1, resp);
    CHECK(resp[1] == PB_BLE_MSG_CANCEL_RESP && resp[2] == PB_BLE_CODE_MALFORMED, "truncated: malformed");
    pb_ble_adm_handle_cancel(&g_adm, req, PB_BLE_CANCEL_REQ_LEN + 1, resp);
    CHECK(resp[2] == PB_BLE_CODE_MALFORMED, "too long: malformed");
    req[2] = 1;
    pb_ble_adm_handle_cancel(&g_adm, req, PB_BLE_CANCEL_REQ_LEN, resp);
    CHECK(resp[2] == PB_BLE_CODE_MALFORMED, "non-zero reserved byte: malformed");
    build_cancel(req, EUI_A, id);
    req[0] = 9;
    pb_ble_adm_handle_cancel(&g_adm, req, PB_BLE_CANCEL_REQ_LEN, resp);
    CHECK(resp[2] == PB_BLE_CODE_UNSUPPORTED_VERSION, "unknown version: unsupported");
    CHECK(g_adm.rec_phase == PB_BLE_REC_QUEUED && g_adm.pskd_pending, "malformed cancels left the attempt alone");
}

static void scenario_failure_and_retry(void)
{
    printf("\n-- a failed attempt, then a retry --\n");
    fresh();
    uint8_t id[16], id2[16];
    make_id(id, 6);
    make_id(id2, 60);
    uint8_t st[PB_BLE_STATUS_RESP_LEN];

    start_new(id, PSKD);
    poll();
    pb_enroll_ctl_on_joiner_done(&g_ctl, g_ot.last_attempt, PB_ENROLL_JOINER_ERR_SECURITY, g_now);
    CHECK(poll() == PB_BLE_OPEN, "after a Joiner failure BLE stays open for a retry");
    query(id, st);
    CHECK(st[12] == PB_ENROLL_STATE_WAITING && st[13] == PB_ENROLL_RESULT_JOINER_SECURITY &&
              st[15] == PB_BLE_PHASE_FAILED && st[16] == PB_ENROLL_RESULT_JOINER_SECURITY && !(st[14] & 1),
          "status: waiting, this request failed with joiner_security");
    CHECK(all_zero(g_adm.rec_tag, 32), "the request digest is wiped once the attempt ended");
    CHECK(start_new(id, PSKD) == PB_BLE_CODE_REQUEST_CONFLICT, "re-using the finished request id is refused");
    CHECK(start_new(id2, PSKD2) == PB_BLE_CODE_ACCEPTED, "a new request id with a new challenge is accepted");
    poll();
    CHECK(g_ot.joiner_start_calls == 2 && g_ctl.attempt == 2 && strcmp(g_ot.last_pskd, PSKD2) == 0,
          "second Joiner attempt started with the new PSKd");
    query(id, st);
    CHECK(!(st[14] & PB_BLE_STATUS_F_REQUEST_MATCH), "the old request id no longer names anything");

    printf("\n-- the controller refuses a queued start --\n");
    fresh();
    start_new(id, PSKD);
    g_ot.joiner_start_ret = ESP_FAIL;
    poll();
    query(id, st);
    CHECK(g_ctl.state == PB_ENROLL_STATE_WAITING && st[15] == PB_BLE_PHASE_FAILED &&
              st[16] == PB_ENROLL_RESULT_JOINER_START_FAILED && !g_adm.pskd_pending,
          "Joiner start failure is reported for that request, PSKd wiped");

    fresh();
    start_new(id, PSKD);
    nvs_sim_force_next_commit_error(ESP_FAIL); /* the old marker cannot be erased */
    poll();
    query(id, st);
    CHECK(g_ot.joiner_start_calls == 0 && st[15] == PB_BLE_PHASE_FAILED &&
              st[16] == PB_ENROLL_RESULT_MARKER_CLEAR_FAILED,
          "marker-clear failure: no Joiner, reported as that request's result");

    fresh();
    start_new(id, PSKD);
    {
        /* Something else moved the controller out of WAITING between the
         * BLE callback and the worker (not possible in the BLE build, which
         * has no other transport -- the worker must still cope). */
        char other[PB_ENROLL_PSKD_BUF_LEN] = {0};
        strcpy(other, PSKD2);
        pb_enroll_ctl_request_join(&g_ctl, other, g_now);
    }
    poll();
    query(id, st);
    CHECK(g_ot.joiner_start_calls == 1 && strcmp(g_ot.last_pskd, PSKD2) == 0 && st[15] == PB_BLE_PHASE_FAILED &&
              st[16] == PB_BLE_REQ_RESULT_START_REFUSED,
          "a start the controller refuses (not waiting) fails that request and starts no second Joiner");
    CHECK(start_new(id2, PSKD) == PB_BLE_CODE_NOT_WAITING, "while the board is not waiting, new starts are refused");
}

static void scenario_joiner_success_is_not_enrollment(void)
{
    printf("\n-- Joiner success alone is not success --\n");
    fresh();
    uint8_t id[16];
    make_id(id, 7);
    uint8_t st[PB_BLE_STATUS_RESP_LEN];
    start_new(id, PSKD);
    poll();
    g_ot.has_key = true; /* what the Joiner stores: key, channel, PAN ID */
    pb_enroll_ctl_on_joiner_done(&g_ctl, g_ot.last_attempt, PB_ENROLL_JOINER_OK, g_now);
    CHECK(poll() == PB_BLE_OPEN, "after Joiner success BLE stays open");
    query(id, st);
    CHECK(st[2] == PB_BLE_CODE_OK && st[12] == PB_ENROLL_STATE_ATTACHING && st[15] == PB_BLE_PHASE_RUNNING,
          "status: attaching, the request is still only running");
    CHECK(!pb_enroll_ctl_services_allowed(&g_ctl), "services are not allowed yet");

    pb_enroll_ctl_tick(&g_ctl, true, g_now); /* attached, Dataset still partial */
    CHECK(poll() == PB_BLE_OPEN && g_adm.rec_phase == PB_BLE_REC_RUNNING, "attached with a partial Dataset: still running");

    g_now += PB_ENROLL_ATTACH_DEADLINE_MS;
    pb_enroll_ctl_tick(&g_ctl, false, g_now);
    poll();
    query(id, st);
    CHECK(st[12] == PB_ENROLL_STATE_WAITING && st[15] == PB_BLE_PHASE_FAILED &&
              st[16] == PB_ENROLL_RESULT_ATTACH_TIMEOUT,
          "Joiner success followed by no attach ends as a failed request (attach_timeout)");

    printf("\n-- enrolled: BLE enrollment closes, once --\n");
    fresh();
    start_new(id, PSKD);
    poll();
    g_ot.has_key = true;
    pb_enroll_ctl_on_joiner_done(&g_ctl, g_ot.last_attempt, PB_ENROLL_JOINER_OK, g_now);
    poll();
    g_ot.complete = true;
    g_ot.id = NET_OTBR;
    pb_enroll_ctl_tick(&g_ctl, true, g_now);
    CHECK(poll() == PB_BLE_CLOSE_ENROLLED, "the worker pass after enrollment says: close BLE (enrolled)");
    CHECK(poll() == PB_BLE_CLOSE_ENROLLED && g_adm.closed == PB_BLE_CLOSE_ENROLLED, "and keeps saying so");
    CHECK(pb_enroll_ctl_services_allowed(&g_ctl) && g_ot.thread_enabled && g_ot.thread_stop_calls == 0,
          "Thread stays up and services are allowed: closing BLE touched neither");
    query(id, st);
    CHECK(st[2] == PB_BLE_CODE_CLOSED && st[12] == PB_ENROLL_STATE_ENROLLED && st[15] == PB_BLE_PHASE_ENROLLED,
          "status (if a callback still ran): closed, enrolled");
    uint8_t ch[16];
    CHECK(get_challenge(ch) == PB_BLE_CODE_CLOSED && all_zero(ch, 16) && !g_adm.ch_valid, "no challenge after the close");
    uint8_t id2[16];
    make_id(id2, 70);
    CHECK(send_start(EUI_A, id2, ch, PSKD2) == PB_BLE_CODE_CLOSED, "no start after the close");
    CHECK(!pb_ble_adm_sec_request_begin(&g_adm), "no new Security 2 handshake after the close");
    poll();
    CHECK(g_ot.joiner_start_calls == 1, "no second Joiner");

    printf("\n-- a board that resumed at boot --\n");
    nvs_sim_reset();
    memset(&g_ot, 0, sizeof(g_ot));
    g_ot.has_key = true;
    g_ot.complete = true;
    g_ot.id = NET_OTBR;
    pb_enroll_marker_store(EUI_A, &NET_OTBR);
    pb_enroll_ctl_init(&g_ctl, &k_ot_ops, EUI_A);
    CHECK(pb_enroll_ctl_boot(&g_ctl) == PB_ENROLL_STATE_ENROLLED && g_ctl.resumed, "(valid marker + matching Dataset: resumed)");
    pb_ble_adm_init(&g_adm, &k_adm_ops, EUI_A, true, g_now);
    CHECK(poll() == PB_BLE_CLOSE_ENROLLED, "even if BLE were opened, the first worker pass closes it");
    CHECK(start_new(id, PSKD) == PB_BLE_CODE_CLOSED && g_ot.joiner_start_calls == 0, "no start on an enrolled board");
}

static void scenario_queue_full(void)
{
    printf("\n-- the worker queue is bounded --\n");
    fresh();
    uint8_t id[16];
    /* The worker is not running: each accepted start leaves its command in
     * the queue even after the request itself is cancelled. */
    for (int i = 0; i < PB_BLE_CMD_QUEUE_LEN; i++) {
        make_id(id, (uint8_t)(100 + i));
        CHECK(start_new(id, PSKD) == PB_BLE_CODE_ACCEPTED, "start accepted while the queue has room");
        CHECK(send_cancel(EUI_A, id) == PB_BLE_CODE_OK, "(cancelled again)");
    }
    CHECK(g_adm.q_count == PB_BLE_CMD_QUEUE_LEN, "queue full");
    make_id(id, 200);
    CHECK(start_new(id, PSKD) == PB_BLE_CODE_BUSY, "queue full: start answered busy, not accepted");
    CHECK(!g_adm.pskd_pending && all_zero(g_adm.pskd, sizeof(g_adm.pskd)) && g_adm.rec_phase == PB_BLE_REC_CANCELLED &&
              g_adm.q_count == PB_BLE_CMD_QUEUE_LEN,
          "nothing was recorded for the refused start, its PSKd is nowhere");
    poll();
    CHECK(g_ot.joiner_start_calls == 0 && g_adm.q_count == 0, "the worker drained the stale commands: no Joiner");
    CHECK(start_new(id, PSKD) == PB_BLE_CODE_ACCEPTED, "with room again, a start is accepted");
    poll();
    CHECK(g_ot.joiner_start_calls == 1, "and runs");
}

static void scenario_auth_limits(void)
{
    printf("\n-- authentication attempt limit --\n");
    fresh();
    for (unsigned i = 0; i < PB_BLE_MAX_AUTH_FAILURES - 1; i++) {
        pb_ble_adm_sec_request_begin(&g_adm);
        pb_ble_adm_sec_request_end(&g_adm, false);
    }
    CHECK(poll() == PB_BLE_OPEN && pb_ble_adm_sec_request_begin(&g_adm), "4 failed handshake messages: still open");
    pb_ble_adm_sec_request_end(&g_adm, false);
    CHECK(!pb_ble_adm_sec_request_begin(&g_adm), "5th failure: further handshake messages are not even processed");
    CHECK(poll() == PB_BLE_CLOSE_AUTH_LOCKED, "the worker is told to shut BLE down (auth_locked)");

    fresh();
    for (unsigned i = 0; i < PB_BLE_MAX_SEC_REQUESTS; i++) {
        if (!pb_ble_adm_sec_request_begin(&g_adm)) {
            g_failures++;
            printf("FAIL: handshake message %u refused early\n", i);
        }
        pb_ble_adm_sec_request_end(&g_adm, true);
    }
    CHECK(poll() == PB_BLE_OPEN, "40 successful handshake messages: still open");
    CHECK(!pb_ble_adm_sec_request_begin(&g_adm) && poll() == PB_BLE_CLOSE_AUTH_LOCKED,
          "the 41st is refused and BLE closes, even though none failed");

    printf("\n-- a lock-out does not cancel an accepted attempt --\n");
    fresh();
    uint8_t id[16];
    make_id(id, 8);
    start_new(id, PSKD);
    for (unsigned i = 0; i < PB_BLE_MAX_AUTH_FAILURES; i++) {
        pb_ble_adm_sec_request_begin(&g_adm);
        pb_ble_adm_sec_request_end(&g_adm, false);
    }
    CHECK(poll() == PB_BLE_CLOSE_AUTH_LOCKED, "BLE closes");
    CHECK(g_ot.joiner_start_calls == 1 && g_ctl.state == PB_ENROLL_STATE_JOINING,
          "the start accepted before the lock-out was still run");
    poll();
    CHECK(g_ot.joiner_stop_calls == 0 && g_ctl.state == PB_ENROLL_STATE_JOINING, "and is not cancelled by the close");
}

static void scenario_window(void)
{
    printf("\n-- advertising window --\n");
    fresh();
    g_now = 1000 + PB_BLE_ADV_WINDOW_MS - 1;
    CHECK(poll() == PB_BLE_OPEN, "1 ms before the window ends: open");
    g_now += 1;
    CHECK(poll() == PB_BLE_CLOSE_WINDOW_EXPIRED, "at the window end with nothing in progress: close");
    uint8_t id[16];
    make_id(id, 9);
    CHECK(start_new(id, PSKD) == PB_BLE_CODE_CLOSED, "closed: no start");

    fresh();
    start_new(id, PSKD);
    poll();
    g_now = 1000 + PB_BLE_ADV_WINDOW_MS + 5000;
    CHECK(poll() == PB_BLE_OPEN, "window over but an attempt is running: BLE stays open for it");
    pb_enroll_ctl_on_joiner_done(&g_ctl, g_ot.last_attempt, PB_ENROLL_JOINER_ERR_NOT_FOUND, g_now);
    CHECK(poll() == PB_BLE_CLOSE_WINDOW_EXPIRED, "once that attempt ended: close");

    nvs_sim_reset();
    memset(&g_ot, 0, sizeof(g_ot));
    g_now = 0xfffff000u;
    pb_enroll_ctl_init(&g_ctl, &k_ot_ops, EUI_A);
    pb_enroll_ctl_boot(&g_ctl);
    pb_ble_adm_init(&g_adm, &k_adm_ops, EUI_A, true, g_now);
    g_now += PB_BLE_ADV_WINDOW_MS - 1;
    CHECK(poll() == PB_BLE_OPEN, "window across the 32-bit clock wrap: still open just before");
    g_now += 1;
    CHECK(poll() == PB_BLE_CLOSE_WINDOW_EXPIRED, "and closed exactly at its end");
}

static void scenario_no_secret_in_logs(void)
{
    printf("\n-- no PSKd, challenge or request id in the log output --\n");
    char path[] = "/tmp/pb_ble_adm_logXXXXXX";
    int fd = mkstemp(path);
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    dup2(fd, STDERR_FILENO);

    fresh();
    uint8_t id[16], id2[16], ch[16];
    make_id(id, 10);
    make_id(id2, 110);
    get_challenge(ch);
    send_start(EUI_A, id, ch, PSKD);
    send_start(EUI_A, id, ch, PSKD);
    send_start(EUI_A, id, ch, PSKD2);
    poll();
    start_new(id2, "j01nme7k");
    send_cancel(EUI_A, id);
    poll();
    start_new(id2, PSKD2);
    poll();
    pb_enroll_ctl_on_joiner_done(&g_ctl, g_ot.last_attempt, PB_ENROLL_JOINER_ERR_SECURITY, g_now);
    poll();
    for (unsigned i = 0; i < PB_BLE_MAX_AUTH_FAILURES; i++) {
        pb_ble_adm_sec_request_begin(&g_adm);
        pb_ble_adm_sec_request_end(&g_adm, false);
    }
    poll();

    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    char log[32768] = {0};
    lseek(fd, 0, SEEK_SET);
    ssize_t n = read(fd, log, sizeof(log) - 1);
    close(fd);
    unlink(path);
    CHECK(n > 0, "the scenario produced log output");
    char hex[40];
    snprintf(hex, sizeof(hex), "%02x%02x%02x%02x", ch[0], ch[1], ch[2], ch[3]);
    char idhex[40];
    snprintf(idhex, sizeof(idhex), "%02x%02x%02x%02x", id[0], id[1], id[2], id[3]);
    CHECK(strstr(log, PSKD) == NULL && strstr(log, PSKD2) == NULL && strstr(log, "j01nme7k") == NULL,
          "no PSKd (accepted, conflicting or malformed) in the log");
    CHECK(strstr(log, hex) == NULL && strstr(log, idhex) == NULL, "no challenge or request id bytes in the log");
}


/* ==== pb-start with the bridge endpoint (PB_BLE_MSG_START_EP_REQ) ==== */

/* A Mesh-Local EID inside NET_OTBR's prefix, and the ingest port. Made up. */
static const pb_enroll_bridge_endpoint_t EP_OTBR = {
    .addr = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00, 0x8c, 0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79},
    .port = 5684,
};

static bool ep_equal(const pb_enroll_bridge_endpoint_t *x, const pb_enroll_bridge_endpoint_t *y)
{
    return memcmp(x->addr, y->addr, sizeof(x->addr)) == 0 && x->port == y->port;
}

static size_t build_start_ep(uint8_t out[PB_BLE_START_EP_REQ_MAX_LEN + 8], const uint8_t target[8],
                             const uint8_t request_id[16], const uint8_t challenge[16],
                             const pb_enroll_bridge_endpoint_t *ep, const char *pskd)
{
    size_t n = strlen(pskd);
    memset(out, 0, PB_BLE_START_EP_REQ_MAX_LEN + 8);
    out[0] = PB_BLE_PROTO_VERSION;
    out[1] = PB_BLE_MSG_START_EP_REQ;
    out[2] = (uint8_t)n;
    memcpy(&out[4], target, 8);
    memcpy(&out[12], request_id, 16);
    memcpy(&out[28], challenge, 16);
    out[44] = PB_BLE_BRIDGE_ENDPOINT_VERSION;
    out[46] = (uint8_t)(ep->port >> 8);
    out[47] = (uint8_t)ep->port;
    memcpy(&out[48], ep->addr, 16);
    memcpy(&out[64], pskd, n);
    return 64 + n;
}

/* Sends the bytes as they are; resp gets the whole 4-byte answer. */
static uint8_t send_raw_start(const uint8_t *msg, size_t len, uint8_t resp[PB_BLE_SHORT_RESP_LEN])
{
    uint8_t copy[PB_BLE_START_EP_REQ_MAX_LEN + 16];
    memcpy(copy, msg, len);
    pb_ble_adm_handle_start(&g_adm, copy, len, resp, g_now);
    return resp[2];
}

static uint8_t send_start_ep(const uint8_t request_id[16], const uint8_t challenge[16],
                             const pb_enroll_bridge_endpoint_t *ep, const char *pskd)
{
    uint8_t req[PB_BLE_START_EP_REQ_MAX_LEN + 8];
    uint8_t resp[PB_BLE_SHORT_RESP_LEN];
    size_t n = build_start_ep(req, EUI_A, request_id, challenge, ep, pskd);
    pb_ble_adm_handle_start(&g_adm, req, n, resp, g_now);
    return resp[2];
}

/* Fresh challenge + start with an endpoint. */
static uint8_t start_ep_new(const uint8_t request_id[16], const pb_enroll_bridge_endpoint_t *ep, const char *pskd)
{
    uint8_t ch[16];
    get_challenge(ch);
    return send_start_ep(request_id, ch, ep, pskd);
}

static bool endpoint_slot_empty(void)
{
    return !g_adm.endpoint_pending && all_zero(&g_adm.endpoint, sizeof(g_adm.endpoint));
}

static bool no_record(void)
{
    pb_enroll_record_t rec;
    return pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_ABSENT;
}

static size_t hex_to_bytes(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = strlen(hex) / 2;
    if (n > cap) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        sscanf(&hex[2 * i], "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return n;
}

/*
 * The same bytes are in the app repository's
 * PowerBaseBLEKit/Tests/PowerBaseBLEKitTests/CodecTests.swift
 * (BridgeEndpointGolden). Both sides must produce and accept exactly these.
 *   target 6055f9fffe001234, request id a0..af, challenge c0..cf,
 *   endpoint v1, port 5684 (0x1634), address fd11:2233:4455:0:8c1f:2e3d:4c5b:6a79,
 *   PSKd "J01NME7K"
 */
#define GOLDEN_START_EP_HEX                                                                                          \
    "01040800" "6055f9fffe001234" "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf" "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf"               \
    "0100" "1634" "fd112233445500008c1f2e3d4c5b6a79" "4a30314e4d45374b"
#define GOLDEN_START_EP_RESP_ACCEPTED_HEX "01840100"
#define GOLDEN_START_EP_RESP_INVALID_ENDPOINT_HEX "01841d00"

static void scenario_endpoint_golden_vector(void)
{
    printf("\n-- pb-start with the bridge endpoint: golden vector shared with the Swift codec --\n");
    uint8_t golden[PB_BLE_START_EP_REQ_MAX_LEN + 8];
    size_t golden_len = hex_to_bytes(GOLDEN_START_EP_HEX, golden, sizeof(golden));
    CHECK(golden_len == 72 && golden_len == PB_BLE_START_EP_REQ_FIXED_LEN + 8, "the vector is 64 + 8 bytes");

    uint8_t id[16], ch[16];
    for (int i = 0; i < 16; i++) {
        id[i] = (uint8_t)(0xa0 + i);
        ch[i] = (uint8_t)(0xc0 + i);
    }
    uint8_t built[PB_BLE_START_EP_REQ_MAX_LEN + 8];
    size_t n = build_start_ep(built, EUI_A, id, ch, &EP_OTBR, PSKD);
    CHECK(n == golden_len && memcmp(built, golden, golden_len) == 0,
          "the layout in pb_ble_proto.h produces exactly the vector's bytes");
    CHECK(golden[46] == 0x16 && golden[47] == 0x34, "the port is big-endian at [46..47]");

    /* The real admission code takes those bytes. */
    fresh();
    g_rand_fixed = ch;
    uint8_t issued[16];
    CHECK(get_challenge(issued) == PB_BLE_CODE_OK && memcmp(issued, ch, 16) == 0, "(setup) the vector's challenge");
    uint8_t resp[PB_BLE_SHORT_RESP_LEN];
    uint8_t expect[PB_BLE_SHORT_RESP_LEN];
    hex_to_bytes(GOLDEN_START_EP_RESP_ACCEPTED_HEX, expect, sizeof(expect));
    send_raw_start(golden, golden_len, resp);
    CHECK(memcmp(resp, expect, sizeof(expect)) == 0, "answer is 01 84 01 00: its own response type, ACCEPTED");
    CHECK(g_adm.endpoint_pending && ep_equal(&g_adm.endpoint, &EP_OTBR) && g_adm.pskd_pending &&
              strcmp(g_adm.pskd, PSKD) == 0,
          "the address, the port and the PSKd were read from the vector's offsets");
    CHECK(g_ot.joiner_start_calls == 0 && no_record(), "accepted: nothing started and nothing stored in the BLE callback");

    CHECK(poll() == PB_BLE_OPEN && g_join_calls == 1 && g_join_had_endpoint && ep_equal(&g_join_endpoint, &EP_OTBR),
          "the worker hands exactly that endpoint to the controller");
    CHECK(g_ctl_calls_under_lock == 0, "with the leaf lock released");
    CHECK(g_ctl.state == PB_ENROLL_STATE_JOINING && g_ctl.pending_endpoint && ep_equal(&g_ctl.pending_ep, &EP_OTBR),
          "the controller holds it in RAM while joining");
    CHECK(endpoint_slot_empty() && all_zero(g_adm.pskd, sizeof(g_adm.pskd)), "the admission slot is wiped once taken");
    CHECK(no_record(), "still nothing in NVS");

    /* An invalid endpoint's answer, as bytes. */
    fresh();
    pb_enroll_bridge_endpoint_t bad = EP_OTBR;
    bad.port = 0;
    get_challenge(ch);
    n = build_start_ep(built, EUI_A, id, ch, &bad, PSKD);
    hex_to_bytes(GOLDEN_START_EP_RESP_INVALID_ENDPOINT_HEX, expect, sizeof(expect));
    send_raw_start(built, n, resp);
    CHECK(memcmp(resp, expect, sizeof(expect)) == 0, "port 0 is answered 01 84 1d 00 (INVALID_ENDPOINT)");
}

static void scenario_endpoint_lengths_and_format(void)
{
    printf("\n-- pb-start with the bridge endpoint: every length, reserved bytes, message types --\n");
    uint8_t id[16], ch[16];
    make_id(id, 0x40);
    uint8_t msg[PB_BLE_START_EP_REQ_MAX_LEN + 16];
    uint8_t resp[PB_BLE_SHORT_RESP_LEN];

    /* A valid 72-byte request cut or padded to every length 0..104: only
     * its own length is taken. */
    int accepted = 0, malformed = 0, other = 0;
    for (size_t len = 0; len <= PB_BLE_START_EP_REQ_MAX_LEN + 8; len++) {
        fresh();
        get_challenge(ch);
        memset(msg, 0, sizeof(msg));
        build_start_ep(msg, EUI_A, id, ch, &EP_OTBR, PSKD);
        uint8_t code = send_raw_start(msg, len, resp);
        if (code == PB_BLE_CODE_ACCEPTED && len == 72) {
            accepted++;
        } else if (code == PB_BLE_CODE_MALFORMED && len != 72 && nothing_started() && endpoint_slot_empty()) {
            malformed++;
        } else {
            other++;
        }
        /* A malformed request does not use up the challenge. */
        if (len != 72 && len >= 2) {
            build_start_ep(msg, EUI_A, id, ch, &EP_OTBR, PSKD);
            if (send_raw_start(msg, 72, resp) != PB_BLE_CODE_ACCEPTED) {
                other++;
            }
        }
    }
    CHECK(accepted == 1 && malformed == (int)(PB_BLE_START_EP_REQ_MAX_LEN + 8) && other == 0,
          "lengths 0..104 of one request: only 72 is accepted, all others MALFORMED with the challenge kept");

    /* pskd_len decides the length: 6..32 are the valid sizes. */
    static const char *PSKD6 = "J01NME";
    static const char *PSKD32 = "ABCDEFGHJKLMNPRSTUVWXY0123456789";
    fresh();
    CHECK(start_ep_new(id, &EP_OTBR, PSKD6) == PB_BLE_CODE_ACCEPTED, "70 bytes (6-character PSKd): accepted");
    fresh();
    CHECK(start_ep_new(id, &EP_OTBR, PSKD32) == PB_BLE_CODE_ACCEPTED, "96 bytes (32-character PSKd): accepted");
    fresh();
    CHECK(start_ep_new(id, &EP_OTBR, "J01NM") == PB_BLE_CODE_INVALID_PSKD && nothing_started(),
          "69 bytes with pskd_len 5: INVALID_PSKD");
    fresh();
    get_challenge(ch);
    size_t n = build_start_ep(msg, EUI_A, id, ch, &EP_OTBR, PSKD32);
    msg[2] = 33;
    msg[n] = 'A';
    CHECK(send_raw_start(msg, n + 1, resp) == PB_BLE_CODE_MALFORMED && nothing_started(),
          "97 bytes with pskd_len 33: MALFORMED");
    fresh();
    get_challenge(ch);
    n = build_start_ep(msg, EUI_A, id, ch, &EP_OTBR, PSKD);
    msg[2] = 9; /* says 9, carries 8 */
    CHECK(send_raw_start(msg, n, resp) == PB_BLE_CODE_MALFORMED && nothing_started(),
          "pskd_len that disagrees with the length: MALFORMED");

    /* Reserved bytes. */
    fresh();
    get_challenge(ch);
    n = build_start_ep(msg, EUI_A, id, ch, &EP_OTBR, PSKD);
    msg[3] = 1;
    CHECK(send_raw_start(msg, n, resp) == PB_BLE_CODE_MALFORMED && resp[1] == PB_BLE_MSG_START_EP_RESP &&
              nothing_started(),
          "reserved byte [3] set: MALFORMED");
    n = build_start_ep(msg, EUI_A, id, ch, &EP_OTBR, PSKD);
    msg[45] = 0x80;
    CHECK(send_raw_start(msg, n, resp) == PB_BLE_CODE_MALFORMED && nothing_started() && endpoint_slot_empty(),
          "reserved byte [45] set: MALFORMED");
    n = build_start_ep(msg, EUI_A, id, ch, &EP_OTBR, PSKD);
    CHECK(send_raw_start(msg, n, resp) == PB_BLE_CODE_ACCEPTED, "neither used up the challenge");

    /* All-zero request id. */
    fresh();
    get_challenge(ch);
    uint8_t zero_id[16] = {0};
    n = build_start_ep(msg, EUI_A, zero_id, ch, &EP_OTBR, PSKD);
    CHECK(send_raw_start(msg, n, resp) == PB_BLE_CODE_MALFORMED && nothing_started(), "all-zero request id: MALFORMED");

    /* The two layouts are not interchangeable. */
    fresh();
    get_challenge(ch);
    n = build_start_ep(msg, EUI_A, id, ch, &EP_OTBR, PSKD);
    msg[1] = PB_BLE_MSG_START_REQ; /* 72 bytes announced as the version 1 start: 44 + 8 != 72 */
    CHECK(send_raw_start(msg, n, resp) == PB_BLE_CODE_MALFORMED && resp[1] == PB_BLE_MSG_START_RESP &&
              nothing_started(),
          "the endpoint layout under the version 1 type: MALFORMED, answered as pb-start");
    uint8_t legacy[PB_BLE_START_REQ_MAX_LEN + 8];
    n = build_start(legacy, EUI_A, id, ch, PSKD);
    legacy[1] = PB_BLE_MSG_START_EP_REQ; /* 52 bytes announced as the endpoint start: shorter than 64 */
    CHECK(send_raw_start(legacy, n, resp) == PB_BLE_CODE_MALFORMED && resp[1] == PB_BLE_MSG_START_EP_RESP &&
              nothing_started(),
          "the version 1 layout under the endpoint type: MALFORMED");
    n = build_start_ep(msg, EUI_A, id, ch, &EP_OTBR, PSKD);
    msg[1] = 0x05;
    CHECK(send_raw_start(msg, n, resp) == PB_BLE_CODE_MALFORMED && resp[1] == PB_BLE_MSG_START_RESP &&
              nothing_started(),
          "an unknown message type: MALFORMED");
    n = build_start_ep(msg, EUI_A, id, ch, &EP_OTBR, PSKD);
    msg[0] = 2;
    CHECK(send_raw_start(msg, n, resp) == PB_BLE_CODE_UNSUPPORTED_VERSION && resp[0] == PB_BLE_PROTO_VERSION &&
              nothing_started(),
          "another protocol version: UNSUPPORTED_VERSION, nothing read");
    n = build_start_ep(msg, EUI_A, id, ch, &EP_OTBR, PSKD);
    uint8_t before[PB_BLE_START_EP_REQ_MAX_LEN + 8];
    memcpy(before, msg, n);
    pb_ble_adm_handle_start(&g_adm, msg, n, resp, g_now);
    CHECK(resp[2] == PB_BLE_CODE_ACCEPTED && all_zero(msg, n), "the decrypted request buffer (PSKd, address) is wiped");
}

static void scenario_endpoint_rejections(void)
{
    printf("\n-- pb-start with the bridge endpoint: addresses, port, challenge, target --\n");
    uint8_t id[16], ch[16];
    make_id(id, 0x50);
    static const struct {
        uint8_t addr[16];
        const char *what;
    } bad_addr[] = {
        {{0}, "unspecified (::)"},
        {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, "loopback (::1)"},
        {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 168, 1, 10}, "IPv4-mapped (::ffff:192.168.1.10)"},
        {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 192, 168, 1, 10}, "IPv4-compatible (::192.168.1.10)"},
        {{0xff, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, "multicast (ff03::1)"},
        {{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x8c, 0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79}, "link-local (fe80::/10)"},
        {{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0x8c, 0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79}, "global (2001:db8::/32)"},
        {{0xfc, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 0x8c, 0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79}, "fc00::/8, not fd00::/8"},
        {{0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 0, 0, 0, 0xff, 0xfe, 0, 0x4c, 0x00}, "an RLOC (…:0:ff:fe00:4c00)"},
        {{0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 0, 0, 0, 0xff, 0xfe, 0, 0xfc, 0x00}, "an ALOC (…:0:ff:fe00:fc00)"},
    };
    for (size_t i = 0; i < sizeof(bad_addr) / sizeof(bad_addr[0]); i++) {
        fresh();
        pb_enroll_bridge_endpoint_t ep = {.port = 5684};
        memcpy(ep.addr, bad_addr[i].addr, 16);
        uint8_t code = start_ep_new(id, &ep, PSKD);
        printf("      (%s)\n", bad_addr[i].what);
        CHECK(code == PB_BLE_CODE_INVALID_ENDPOINT && nothing_started() && endpoint_slot_empty() && no_record() &&
                  g_adm.rec_phase == PB_BLE_REC_NONE,
              "INVALID_ENDPOINT: no attempt, no Joiner, nothing kept, nothing stored");
        CHECK(poll() == PB_BLE_OPEN && g_join_calls == 0, "and the worker has nothing to run");
    }

    fresh();
    pb_enroll_bridge_endpoint_t ep = EP_OTBR;
    ep.port = 0;
    CHECK(start_ep_new(id, &ep, PSKD) == PB_BLE_CODE_INVALID_ENDPOINT && nothing_started() && endpoint_slot_empty(),
          "port 0: INVALID_ENDPOINT");
    fresh();
    ep.port = 1;
    CHECK(start_ep_new(id, &ep, PSKD) == PB_BLE_CODE_ACCEPTED && g_adm.endpoint.port == 1, "port 1: accepted");
    fresh();
    ep.port = 65535;
    CHECK(start_ep_new(id, &ep, PSKD) == PB_BLE_CODE_ACCEPTED && g_adm.endpoint.port == 65535, "port 65535: accepted");

    /* Endpoint format versions other than 1. */
    uint8_t msg[PB_BLE_START_EP_REQ_MAX_LEN + 8];
    uint8_t resp[PB_BLE_SHORT_RESP_LEN];
    for (int v = 0; v <= 2; v += 2) {
        fresh();
        get_challenge(ch);
        size_t n = build_start_ep(msg, EUI_A, id, ch, &EP_OTBR, PSKD);
        msg[44] = (uint8_t)v;
        CHECK(send_raw_start(msg, n, resp) == PB_BLE_CODE_INVALID_ENDPOINT && nothing_started() &&
                  endpoint_slot_empty(),
              "endpoint format version 0 / 2: INVALID_ENDPOINT");
    }

    /* The same checks as the version 1 start, with an endpoint on board. */
    fresh();
    uint8_t wrong[16] = {0};
    get_challenge(ch);
    memcpy(wrong, ch, 16);
    wrong[5] ^= 1;
    CHECK(send_start_ep(id, wrong, &EP_OTBR, PSKD) == PB_BLE_CODE_CHALLENGE_INVALID && nothing_started() &&
              endpoint_slot_empty() && no_record(),
          "wrong challenge: CHALLENGE_INVALID, the endpoint is not kept");
    CHECK(send_start_ep(id, ch, &EP_OTBR, PSKD) == PB_BLE_CODE_CHALLENGE_INVALID && nothing_started(),
          "and the right challenge is used up by that attempt");

    fresh();
    CHECK(send_start_ep(id, wrong, &EP_OTBR, PSKD) == PB_BLE_CODE_CHALLENGE_INVALID && nothing_started(),
          "no challenge issued in this session: CHALLENGE_INVALID");

    fresh();
    get_challenge(ch);
    g_now += PB_BLE_CHALLENGE_TTL_MS;
    CHECK(send_start_ep(id, ch, &EP_OTBR, PSKD) == PB_BLE_CODE_CHALLENGE_EXPIRED && nothing_started() &&
              endpoint_slot_empty(),
          "expired challenge: CHALLENGE_EXPIRED");

    fresh();
    get_challenge(ch);
    pb_ble_adm_session_changed(&g_adm); /* another session: a new handshake would be needed */
    CHECK(send_start_ep(id, ch, &EP_OTBR, PSKD) == PB_BLE_CODE_CHALLENGE_INVALID && nothing_started(),
          "a challenge from an earlier session (no authentication for this one): refused");

    fresh();
    get_challenge(ch);
    size_t n = build_start_ep(msg, EUI_B, id, ch, &EP_OTBR, PSKD);
    CHECK(send_raw_start(msg, n, resp) == PB_BLE_CODE_WRONG_TARGET && nothing_started() && endpoint_slot_empty(),
          "another board's EUI-64: WRONG_TARGET");

    fresh();
    pb_ble_adm_init(&g_adm, &k_adm_ops, EUI_A, false, g_now);
    pb_ble_adm_session_changed(&g_adm);
    CHECK(start_ep_new(id, &EP_OTBR, PSKD) == PB_BLE_CODE_UNAVAILABLE && nothing_started() && endpoint_slot_empty(),
          "a board that cannot run a Joiner: UNAVAILABLE");

    /* Auth lock / closed: nothing is taken any more. */
    fresh();
    for (unsigned i = 0; i < PB_BLE_MAX_AUTH_FAILURES; i++) {
        pb_ble_adm_sec_request_begin(&g_adm);
        pb_ble_adm_sec_request_end(&g_adm, false);
    }
    CHECK(poll() == PB_BLE_CLOSE_AUTH_LOCKED, "(setup) five failed handshakes close BLE enrollment");
    uint8_t any[16] = {1};
    CHECK(send_start_ep(id, any, &EP_OTBR, PSKD) == PB_BLE_CODE_CLOSED && nothing_started() && endpoint_slot_empty() &&
              no_record(),
          "after failed authentication: CLOSED, no Joiner, nothing stored");
}

static void scenario_endpoint_duplicates(void)
{
    printf("\n-- same request id: the same endpoint is a duplicate, another one is a conflict --\n");
    uint8_t id[16], ch[16];
    make_id(id, 0x60);
    uint8_t msg[PB_BLE_START_EP_REQ_MAX_LEN + 8];
    uint8_t resp[PB_BLE_SHORT_RESP_LEN];

    fresh();
    get_challenge(ch);
    size_t n = build_start_ep(msg, EUI_A, id, ch, &EP_OTBR, PSKD);
    CHECK(send_raw_start(msg, n, resp) == PB_BLE_CODE_ACCEPTED, "(setup) accepted");
    uint32_t gen = g_adm.rec_gen;

    n = build_start_ep(msg, EUI_A, id, ch, &EP_OTBR, PSKD);
    CHECK(send_raw_start(msg, n, resp) == PB_BLE_CODE_DUPLICATE && resp[1] == PB_BLE_MSG_START_EP_RESP,
          "the very same request again: DUPLICATE");

    pb_enroll_bridge_endpoint_t other = EP_OTBR;
    other.addr[15] ^= 0x01;
    n = build_start_ep(msg, EUI_A, id, ch, &other, PSKD);
    CHECK(send_raw_start(msg, n, resp) == PB_BLE_CODE_REQUEST_CONFLICT,
          "same request id, another address: REQUEST_CONFLICT, not DUPLICATE");
    other = EP_OTBR;
    other.port = 5685;
    n = build_start_ep(msg, EUI_A, id, ch, &other, PSKD);
    CHECK(send_raw_start(msg, n, resp) == PB_BLE_CODE_REQUEST_CONFLICT,
          "same request id, another port: REQUEST_CONFLICT, not DUPLICATE");
    uint8_t legacy[PB_BLE_START_REQ_MAX_LEN + 8];
    n = build_start(legacy, EUI_A, id, ch, PSKD);
    CHECK(send_raw_start(legacy, n, resp) == PB_BLE_CODE_REQUEST_CONFLICT && resp[1] == PB_BLE_MSG_START_RESP,
          "same request id, challenge and PSKd as the version 1 message: REQUEST_CONFLICT (the type is part of it)");
    n = build_start_ep(msg, EUI_A, id, ch, &EP_OTBR, PSKD2);
    CHECK(send_raw_start(msg, n, resp) == PB_BLE_CODE_REQUEST_CONFLICT, "same request id, another PSKd: REQUEST_CONFLICT");

    CHECK(g_adm.rec_gen == gen && g_adm.rec_phase == PB_BLE_REC_QUEUED && g_adm.endpoint_pending &&
              ep_equal(&g_adm.endpoint, &EP_OTBR) && strcmp(g_adm.pskd, PSKD) == 0 && g_adm.q_count == 1,
          "none of them touched the accepted attempt, its endpoint or its PSKd");
    CHECK(poll() == PB_BLE_OPEN && g_join_calls == 1 && g_ot.joiner_start_calls == 1 && g_join_had_endpoint &&
              ep_equal(&g_join_endpoint, &EP_OTBR) && strcmp(g_ot.last_pskd, PSKD) == 0,
          "one Joiner, with the first request's endpoint and PSKd");

    /* The reverse: an accepted version 1 start, then the endpoint message
     * under the same id. */
    fresh();
    get_challenge(ch);
    CHECK(send_start(EUI_A, id, ch, PSKD) == PB_BLE_CODE_ACCEPTED, "(setup) a version 1 start accepted");
    n = build_start_ep(msg, EUI_A, id, ch, &EP_OTBR, PSKD);
    CHECK(send_raw_start(msg, n, resp) == PB_BLE_CODE_REQUEST_CONFLICT && endpoint_slot_empty(),
          "the endpoint message reusing a version 1 request id: REQUEST_CONFLICT, no endpoint taken");
    CHECK(send_start(EUI_A, id, ch, PSKD) == PB_BLE_CODE_DUPLICATE, "the version 1 start again: still DUPLICATE");
    CHECK(poll() == PB_BLE_OPEN && g_join_calls == 1 && !g_join_had_endpoint && !g_ctl.pending_endpoint,
          "the version 1 attempt runs without any endpoint");

    /* Another request id while one is in progress. */
    uint8_t id2[16];
    make_id(id2, 0x61);
    CHECK(start_ep_new(id2, &EP_OTBR, PSKD2) == PB_BLE_CODE_BUSY && endpoint_slot_empty() && g_join_calls == 1,
          "a second request (other id) while one runs: BUSY, its endpoint is not kept");
}

static void scenario_endpoint_pending_is_dropped(void)
{
    printf("\n-- the delivered endpoint never outlives an attempt that did not enroll --\n");
    uint8_t id[16];
    uint8_t st[PB_BLE_STATUS_RESP_LEN];

    /* Cancelled while still queued. */
    make_id(id, 0x70);
    fresh();
    CHECK(start_ep_new(id, &EP_OTBR, PSKD) == PB_BLE_CODE_ACCEPTED && g_adm.endpoint_pending, "(setup) queued");
    CHECK(send_cancel(EUI_A, id) == PB_BLE_CODE_OK && endpoint_slot_empty() && all_zero(g_adm.pskd, sizeof(g_adm.pskd)),
          "cancel while queued: the endpoint and the PSKd are dropped at once");
    CHECK(poll() == PB_BLE_OPEN && g_join_calls == 0 && g_ot.joiner_start_calls == 0 && !g_ctl.pending_endpoint &&
              no_record(),
          "the worker never starts it; the controller never saw the endpoint");

    /* The Joiner fails. */
    make_id(id, 0x71);
    fresh();
    start_ep_new(id, &EP_OTBR, PSKD);
    poll();
    CHECK(g_ctl.state == PB_ENROLL_STATE_JOINING && g_ctl.pending_endpoint, "(setup) joining with a pending endpoint");
    pb_enroll_ctl_on_joiner_done(&g_ctl, g_ot.last_attempt, PB_ENROLL_JOINER_ERR_SECURITY, g_now);
    poll();
    query(id, st);
    CHECK(st[15] == PB_BLE_PHASE_FAILED && st[16] == PB_ENROLL_RESULT_JOINER_SECURITY, "the request is reported failed");
    CHECK(!g_ctl.pending_endpoint && all_zero(&g_ctl.pending_ep, sizeof(g_ctl.pending_ep)) && no_record() &&
              !pb_enroll_ctl_services_allowed(&g_ctl),
          "Joiner failure: pending endpoint gone, nothing stored, no services");

    /* Cancelled while attaching. */
    make_id(id, 0x72);
    fresh();
    start_ep_new(id, &EP_OTBR, PSKD);
    poll();
    g_ot.has_key = true;
    pb_enroll_ctl_on_joiner_done(&g_ctl, g_ot.last_attempt, PB_ENROLL_JOINER_OK, g_now);
    poll();
    CHECK(g_ctl.state == PB_ENROLL_STATE_ATTACHING && g_ctl.pending_endpoint && no_record(),
          "(setup) attaching: the endpoint is still RAM only");
    CHECK(send_cancel(EUI_A, id) == PB_BLE_CODE_OK, "cancel while attaching is taken");
    poll();
    query(id, st);
    CHECK(st[15] == PB_BLE_PHASE_CANCELLED && g_ctl.state == PB_ENROLL_STATE_WAITING && !g_ot.thread_enabled,
          "the attempt is cancelled and Thread is off");
    CHECK(!g_ctl.pending_endpoint && all_zero(&g_ctl.pending_ep, sizeof(g_ctl.pending_ep)) && no_record(),
          "cancel while attaching: pending endpoint gone, nothing stored");

    /* A later version 1 attempt on the same boot does not inherit it. */
    uint8_t id2[16];
    make_id(id2, 0x73);
    CHECK(start_new(id2, PSKD2) == PB_BLE_CODE_ACCEPTED, "(setup) a version 1 start afterwards");
    poll();
    g_ot.has_key = true;
    pb_enroll_ctl_on_joiner_done(&g_ctl, g_ot.last_attempt, PB_ENROLL_JOINER_OK, g_now);
    g_ot.complete = true;
    g_ot.id = NET_OTBR;
    pb_enroll_ctl_tick(&g_ctl, true, g_now);
    pb_enroll_record_t rec;
    CHECK(g_ctl.state == PB_ENROLL_STATE_ENROLLED && !g_ctl.has_bridge_endpoint &&
              pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_VALID && !rec.has_bridge_endpoint,
          "it enrolls with a version 1 record: the cancelled attempt's endpoint is nowhere");
}

static void scenario_endpoint_enrollment(void)
{
    printf("\n-- enrolled: the endpoint is stored with the marker, only for the joined network --\n");
    uint8_t id[16];
    uint8_t st[PB_BLE_STATUS_RESP_LEN];
    pb_enroll_record_t rec;

    make_id(id, 0x80);
    fresh();
    start_ep_new(id, &EP_OTBR, PSKD);
    poll();
    g_ot.has_key = true;
    pb_enroll_ctl_on_joiner_done(&g_ctl, g_ot.last_attempt, PB_ENROLL_JOINER_OK, g_now);
    pb_enroll_ctl_tick(&g_ctl, true, g_now);
    CHECK(g_ctl.state == PB_ENROLL_STATE_ATTACHING && no_record(), "attached with a partial Dataset: nothing stored yet");
    g_ot.complete = true;
    g_ot.id = NET_OTBR;
    pb_enroll_ctl_tick(&g_ctl, true, g_now);
    CHECK(g_ctl.state == PB_ENROLL_STATE_ENROLLED && g_ctl.has_bridge_endpoint &&
              ep_equal(&g_ctl.bridge_endpoint, &EP_OTBR) && !g_ctl.pending_endpoint,
          "enrolled with the delivered endpoint");
    CHECK(pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_VALID && rec.has_bridge_endpoint &&
              ep_equal(&rec.bridge_endpoint, &EP_OTBR) && memcmp(&rec.id, &NET_OTBR, sizeof(rec.id)) == 0,
          "one record holds the marker and the endpoint");
    CHECK(poll() == PB_BLE_CLOSE_ENROLLED, "BLE enrollment closes");
    query(id, st);
    CHECK(st[15] == PB_BLE_PHASE_ENROLLED && st[16] == PB_ENROLL_RESULT_ENROLLED && st[13] <= 18,
          "the request is reported enrolled with a version 1 result value");

    /* The network the board joined is not the endpoint's. */
    make_id(id, 0x81);
    fresh();
    pb_enroll_bridge_endpoint_t elsewhere = EP_OTBR;
    elsewhere.addr[1] ^= 0xff; /* another fd00::/8 prefix: passes the format check */
    CHECK(start_ep_new(id, &elsewhere, PSKD) == PB_BLE_CODE_ACCEPTED, "(setup) an endpoint in another Mesh-Local Prefix");
    poll();
    g_ot.has_key = true;
    pb_enroll_ctl_on_joiner_done(&g_ctl, g_ot.last_attempt, PB_ENROLL_JOINER_OK, g_now);
    g_ot.complete = true;
    g_ot.id = NET_OTBR;
    pb_enroll_ctl_tick(&g_ctl, true, g_now);
    CHECK(g_ctl.state == PB_ENROLL_STATE_WAITING && g_ctl.last == PB_ENROLL_RESULT_DATASET_MISMATCH &&
              !g_ot.thread_enabled && !pb_enroll_ctl_services_allowed(&g_ctl),
          "attached to a network the endpoint is not in: not enrolled, Thread stopped");
    CHECK(no_record() && !g_ctl.pending_endpoint && !g_ctl.has_bridge_endpoint, "nothing stored, nothing kept");
    poll();
    query(id, st);
    CHECK(st[15] == PB_BLE_PHASE_FAILED && st[16] == PB_ENROLL_RESULT_DATASET_MISMATCH,
          "the request is reported failed (dataset_mismatch)");

    /* The record cannot be written. */
    make_id(id, 0x82);
    fresh();
    start_ep_new(id, &EP_OTBR, PSKD);
    poll();
    g_ot.has_key = true;
    pb_enroll_ctl_on_joiner_done(&g_ctl, g_ot.last_attempt, PB_ENROLL_JOINER_OK, g_now);
    g_ot.complete = true;
    g_ot.id = NET_OTBR;
    nvs_sim_force_next_commit_error(ESP_FAIL);
    pb_enroll_ctl_tick(&g_ctl, true, g_now);
    CHECK(g_ctl.state == PB_ENROLL_STATE_WAITING && g_ctl.last == PB_ENROLL_RESULT_MARKER_WRITE_FAILED &&
              !pb_enroll_ctl_services_allowed(&g_ctl) && !g_ctl.has_bridge_endpoint && no_record(),
          "storing the record failed: not enrolled, no services, no half-written record");
    CHECK(poll() == PB_BLE_OPEN, "BLE enrollment stays open for another attempt");
}

static void scenario_endpoint_never_logged(void)
{
    printf("\n-- neither the bridge address nor the PSKd reaches the log --\n");
    char path[] = "/tmp/pb_ble_ep_logXXXXXX";
    int fd = mkstemp(path);
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    dup2(fd, STDERR_FILENO);

    uint8_t id[16];
    make_id(id, 0x90);
    fresh();
    pb_enroll_bridge_endpoint_t bad = EP_OTBR;
    bad.addr[0] = 0xfe;
    bad.addr[1] = 0x80;
    start_ep_new(id, &bad, PSKD);
    start_ep_new(id, &EP_OTBR, PSKD);
    poll();
    g_ot.has_key = true;
    pb_enroll_ctl_on_joiner_done(&g_ctl, g_ot.last_attempt, PB_ENROLL_JOINER_OK, g_now);
    g_ot.complete = true;
    g_ot.id = NET_OTBR;
    pb_enroll_ctl_tick(&g_ctl, true, g_now);
    poll();

    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    char log[16384] = {0};
    lseek(fd, 0, SEEK_SET);
    ssize_t n = read(fd, log, sizeof(log) - 1);
    close(fd);
    unlink(path);
    CHECK(n > 0 && g_ctl.state == PB_ENROLL_STATE_ENROLLED, "the scenario enrolled and produced log output");
    CHECK(strstr(log, PSKD) == NULL, "no PSKd in the log");
    CHECK(strstr(log, "8c1f") == NULL && strstr(log, "8c:1f") == NULL && strstr(log, "fd11") == NULL &&
              strstr(log, "2e3d") == NULL && strstr(log, "6a79") == NULL,
          "no part of the bridge address in the log");
}


/* ==== endpoint policy: the prepared image requires a delivered endpoint ==== */

static uint32_t test_crc32(const uint8_t *data, size_t len)
{
    return crc32_ieee(data, len);
}

/* A version 1 enrollment record for EUI_A / NET_OTBR, written by hand. */
static void make_v1_record(uint8_t raw[PB_ENROLL_MARKER_LEN])
{
    memset(raw, 0, PB_ENROLL_MARKER_LEN);
    memcpy(raw, "PBE1", 4);
    raw[4] = 1;
    memcpy(&raw[8], EUI_A, 8);
    memcpy(&raw[16], NET_OTBR.ext_pan_id, 8);
    memcpy(&raw[24], NET_OTBR.mesh_local_prefix, 8);
    uint32_t crc = test_crc32(raw, 32);
    raw[32] = (uint8_t)crc;
    raw[33] = (uint8_t)(crc >> 8);
    raw[34] = (uint8_t)(crc >> 16);
    raw[35] = (uint8_t)(crc >> 24);
}

static size_t stored_record(uint8_t *out, size_t cap)
{
    nvs_handle_t h;
    size_t len = cap;
    if (nvs_open(PB_ENROLL_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    esp_err_t err = nvs_get_blob(h, PB_ENROLL_NVS_KEY, out, &len);
    nvs_close(h);
    return err == ESP_OK ? len : 0;
}

/* Like fresh(), with the image's endpoint policy given to the admission
 * rules (`adm_policy`) and to the controller (`ctl_policy`) -- on the board
 * both get the same value; the tests also set them apart to check each layer
 * on its own. `with_old_record`: the board still holds a version 1 record
 * and that network's complete Dataset from an earlier enrollment. */
static void fresh_with_policy(pb_enroll_endpoint_policy_t adm_policy, pb_enroll_endpoint_policy_t ctl_policy,
                              bool with_old_record)
{
    nvs_sim_reset();
    memset(&g_ot, 0, sizeof(g_ot));
    g_now = 1000;
    g_rand_fails = false;
    g_rand_fixed = NULL;
    g_lock_depth = 0;
    g_ctl_calls_under_lock = 0;
    g_join_calls = 0;
    g_join_had_endpoint = false;
    memset(&g_join_endpoint, 0, sizeof(g_join_endpoint));
    if (with_old_record) {
        uint8_t v1[PB_ENROLL_MARKER_LEN];
        make_v1_record(v1);
        nvs_sim_put_blob("nvs", PB_ENROLL_NVS_NAMESPACE, PB_ENROLL_NVS_KEY, v1, sizeof(v1));
        g_ot.has_key = true;
        g_ot.complete = true;
        g_ot.id = NET_OTBR;
    }
    pb_enroll_ctl_init(&g_ctl, &k_ot_ops, EUI_A);
    pb_enroll_ctl_set_endpoint_policy(&g_ctl, ctl_policy);
    pb_enroll_ctl_boot(&g_ctl);
    pb_ble_adm_init(&g_adm, &k_adm_ops, EUI_A, true, g_now);
    pb_ble_adm_set_endpoint_policy(&g_adm, adm_policy);
    pb_ble_ctl_status_t st = {.state = g_ctl.state, .last = g_ctl.last, .attempt = g_ctl.attempt};
    g_adm.ctl = st; /* what pb_ble_service_start() does */
    pb_ble_adm_session_changed(&g_adm);
}

/* The record in flash is exactly the hand-written version 1 record and the
 * fake OpenThread still holds that Dataset. */
static bool old_record_and_dataset_untouched(void)
{
    uint8_t v1[PB_ENROLL_MARKER_LEN];
    uint8_t now[80];
    make_v1_record(v1);
    return stored_record(now, sizeof(now)) == sizeof(v1) && memcmp(now, v1, sizeof(v1)) == 0 && g_ot.has_key &&
           g_ot.complete && memcmp(&g_ot.id, &NET_OTBR, sizeof(g_ot.id)) == 0;
}

static void scenario_endpoint_policy_required(void)
{
    printf("\n-- endpoint policy REQUIRED: the start without an endpoint (0x02) is refused before anything happens --\n");
    uint8_t id[16], ch[16];
    uint8_t st[PB_BLE_STATUS_RESP_LEN];
    uint8_t resp[PB_BLE_SHORT_RESP_LEN];
    uint8_t legacy[PB_BLE_START_REQ_MAX_LEN + 8];
    make_id(id, 0xa0);

    fresh_with_policy(PB_ENROLL_ENDPOINT_POLICY_REQUIRED, PB_ENROLL_ENDPOINT_POLICY_REQUIRED, true);
    CHECK(g_ctl.state == PB_ENROLL_STATE_WAITING && g_ot.joiner_start_calls == 0 && !g_ot.thread_enabled,
          "(setup) the board waits: its old version 1 record is not resumed under this policy");
    query(NULL, st);
    CHECK(st[2] == PB_BLE_CODE_OK && st[12] == PB_ENROLL_STATE_WAITING && (st[14] & PB_BLE_STATUS_F_JOINER_UNAVAILABLE) == 0 &&
              st[13] <= 18,
          "pb-status: waiting, and NOT 'Joiner unavailable' -- the endpoint start works on this board");

    CHECK(get_challenge(ch) == PB_BLE_CODE_OK, "(setup) a challenge is issued");
    size_t n = build_start(legacy, EUI_A, id, ch, PSKD);
    uint8_t copy[PB_BLE_START_REQ_MAX_LEN + 8];
    memcpy(copy, legacy, n);
    pb_ble_adm_handle_start(&g_adm, copy, n, resp, g_now);
    static const uint8_t expect[4] = {0x01, 0x82, 0x1b, 0x00};
    CHECK(memcmp(resp, expect, 4) == 0, "0x02 start: answered 01 82 1b 00 -- UNAVAILABLE, a code every version 1 client knows");
    CHECK(all_zero(copy, n), "the decrypted request (with its PSKd) is wiped");
    CHECK(g_adm.q_count == 0 && g_adm.rec_phase == PB_BLE_REC_NONE && g_adm.rec_gen == 0 && !g_adm.pskd_pending &&
              all_zero(g_adm.pskd, sizeof(g_adm.pskd)) && all_zero(g_adm.rec_request_id, 16) && endpoint_slot_empty(),
          "nothing queued, no attempt record, no PSKd kept");
    CHECK(g_adm.ch_valid, "the session's challenge was NOT used up by the refused message");
    CHECK(poll() == PB_BLE_OPEN && g_join_calls == 0 && g_ot.joiner_start_calls == 0 && g_ot.thread_stop_calls == 0 &&
              g_ctl.state == PB_ENROLL_STATE_WAITING && g_ctl.attempt == 0,
          "the worker has nothing to run: no Joiner, the controller was never asked");
    CHECK(old_record_and_dataset_untouched(), "the stored record and Dataset are byte for byte what they were");
    query(id, st);
    CHECK((st[14] & (PB_BLE_STATUS_F_REQUEST_MATCH | PB_BLE_STATUS_F_ATTEMPT_ACTIVE)) == 0 && st[15] == PB_BLE_PHASE_NONE,
          "pb-status knows nothing under that request id, and no attempt is active");

    /* Sending it again changes nothing. */
    for (int i = 0; i < 3; i++) {
        send_raw_start(legacy, n, resp);
    }
    CHECK(resp[2] == PB_BLE_CODE_UNAVAILABLE && g_adm.q_count == 0 && g_adm.rec_phase == PB_BLE_REC_NONE && g_adm.ch_valid &&
              g_ot.joiner_start_calls == 0,
          "retransmissions get the same answer (never DUPLICATE or CONFLICT) and still leave no trace");
    CHECK(send_cancel(EUI_A, id) == PB_BLE_CODE_NO_SUCH_ATTEMPT, "there is nothing to cancel under that request id");

    /* The right message afterwards: same session, same challenge, even the same request id. */
    uint8_t ep_msg[PB_BLE_START_EP_REQ_MAX_LEN + 8];
    size_t m = build_start_ep(ep_msg, EUI_A, id, ch, &EP_OTBR, PSKD);
    CHECK(send_raw_start(ep_msg, m, resp) == PB_BLE_CODE_ACCEPTED && resp[1] == PB_BLE_MSG_START_EP_RESP,
          "the endpoint start (0x04) with that same challenge and request id: ACCEPTED, as if the 0x02 had never come");
    CHECK(send_raw_start(ep_msg, m, resp) == PB_BLE_CODE_DUPLICATE, "and its own retransmission is a DUPLICATE as usual");
    CHECK(send_raw_start(legacy, n, resp) == PB_BLE_CODE_UNAVAILABLE && g_adm.rec_phase == PB_BLE_REC_QUEUED,
          "a 0x02 under the now active request id is still UNAVAILABLE and does not disturb the attempt");
    CHECK(old_record_and_dataset_untouched(), "accepted is not started: the old record is still there");
    CHECK(poll() == PB_BLE_OPEN && g_join_calls == 1 && g_join_had_endpoint && g_ot.joiner_start_calls == 1 &&
              strcmp(g_ot.last_pskd, PSKD) == 0 && g_ctl.pending_endpoint,
          "the worker starts ONE Joiner, with the endpoint");
    CHECK(no_record(), "the old record was cleared by that accepted request, as for any new attempt");
    g_ot.has_key = true;
    g_ot.complete = false;
    pb_enroll_ctl_on_joiner_done(&g_ctl, g_ot.last_attempt, PB_ENROLL_JOINER_OK, g_now);
    CHECK(no_record(), "Joiner success: still nothing stored");
    g_ot.complete = true;
    g_ot.id = NET_OTBR;
    pb_enroll_ctl_tick(&g_ctl, true, g_now);
    pb_enroll_record_t rec;
    uint8_t raw[80];
    CHECK(g_ctl.state == PB_ENROLL_STATE_ENROLLED && pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_VALID &&
              rec.has_bridge_endpoint && ep_equal(&rec.bridge_endpoint, &EP_OTBR) &&
              stored_record(raw, sizeof(raw)) == PB_ENROLL_MARKER_V2_LEN,
          "enrolled: marker and endpoint in ONE 56-byte record");
    CHECK(poll() == PB_BLE_CLOSE_ENROLLED, "BLE enrollment closes only now");

    /* A fresh board, a new session: the refusal needs no challenge at all. */
    fresh_with_policy(PB_ENROLL_ENDPOINT_POLICY_REQUIRED, PB_ENROLL_ENDPOINT_POLICY_REQUIRED, false);
    uint8_t none[16] = {7};
    n = build_start(legacy, EUI_A, id, none, PSKD);
    CHECK(send_raw_start(legacy, n, resp) == PB_BLE_CODE_UNAVAILABLE && nothing_started() && no_record(),
          "0x02 without any challenge issued: UNAVAILABLE (the policy answers before the challenge is looked at)");
    n = build_start(legacy, EUI_B, id, none, PSKD);
    CHECK(send_raw_start(legacy, n, resp) == PB_BLE_CODE_UNAVAILABLE && nothing_started(),
          "0x02 for another board's EUI-64: UNAVAILABLE too -- its content is not examined");

    /* The checks that come before the policy keep their own answers. */
    n = build_start(legacy, EUI_A, id, none, PSKD);
    legacy[3] = 1;
    CHECK(send_raw_start(legacy, n, resp) == PB_BLE_CODE_MALFORMED, "a malformed 0x02 is still MALFORMED");
    n = build_start(legacy, EUI_A, id, none, PSKD);
    legacy[0] = 2;
    CHECK(send_raw_start(legacy, n, resp) == PB_BLE_CODE_UNSUPPORTED_VERSION, "another protocol version is still UNSUPPORTED_VERSION");
    for (unsigned i = 0; i < PB_BLE_MAX_AUTH_FAILURES; i++) {
        pb_ble_adm_sec_request_begin(&g_adm);
        pb_ble_adm_sec_request_end(&g_adm, false);
    }
    poll();
    n = build_start(legacy, EUI_A, id, none, PSKD);
    CHECK(send_raw_start(legacy, n, resp) == PB_BLE_CODE_CLOSED, "a closed board still answers CLOSED");

    /* The endpoint start is judged exactly as on any board. */
    fresh_with_policy(PB_ENROLL_ENDPOINT_POLICY_REQUIRED, PB_ENROLL_ENDPOINT_POLICY_REQUIRED, false);
    pb_enroll_bridge_endpoint_t bad = EP_OTBR;
    bad.port = 0;
    CHECK(start_ep_new(id, &bad, PSKD) == PB_BLE_CODE_INVALID_ENDPOINT && nothing_started(),
          "an endpoint start with port 0: INVALID_ENDPOINT, as without the policy");
    get_challenge(ch);
    uint8_t wrong[16];
    memcpy(wrong, ch, 16);
    wrong[0] ^= 1;
    CHECK(send_start_ep(id, wrong, &EP_OTBR, PSKD) == PB_BLE_CODE_CHALLENGE_INVALID && nothing_started(),
          "an endpoint start with a wrong challenge: CHALLENGE_INVALID");
}

static void scenario_endpoint_policy_other(void)
{
    uint8_t id[16], ch[16];
    uint8_t st[PB_BLE_STATUS_RESP_LEN];
    uint8_t resp[PB_BLE_SHORT_RESP_LEN];
    uint8_t legacy[PB_BLE_START_REQ_MAX_LEN + 8];
    uint8_t ep_msg[PB_BLE_START_EP_REQ_MAX_LEN + 8];
    make_id(id, 0xb0);

    printf("\n-- no policy in the image (OPTIONAL): the 0x02 start enrolls exactly as before --\n");
    fresh_with_policy(PB_ENROLL_ENDPOINT_POLICY_OPTIONAL, PB_ENROLL_ENDPOINT_POLICY_OPTIONAL, false);
    CHECK(start_new(id, PSKD) == PB_BLE_CODE_ACCEPTED, "0x02 start: ACCEPTED");
    CHECK(poll() == PB_BLE_OPEN && g_join_calls == 1 && !g_join_had_endpoint && g_ot.joiner_start_calls == 1,
          "the worker starts the Joiner without an endpoint");
    g_ot.has_key = true;
    pb_enroll_ctl_on_joiner_done(&g_ctl, g_ot.last_attempt, PB_ENROLL_JOINER_OK, g_now);
    g_ot.complete = true;
    g_ot.id = NET_OTBR;
    pb_enroll_ctl_tick(&g_ctl, true, g_now);
    uint8_t raw[80];
    uint8_t v1[PB_ENROLL_MARKER_LEN];
    make_v1_record(v1);
    CHECK(g_ctl.state == PB_ENROLL_STATE_ENROLLED && stored_record(raw, sizeof(raw)) == 36 && memcmp(raw, v1, 36) == 0 &&
              !g_ctl.has_bridge_endpoint,
          "and the board enrolls with the same version 1 record as before");
    CHECK(poll() == PB_BLE_CLOSE_ENROLLED, "BLE enrollment closes");

    printf("\n-- unusable policy (INVALID): no start of either kind is taken --\n");
    fresh_with_policy(PB_ENROLL_ENDPOINT_POLICY_INVALID, PB_ENROLL_ENDPOINT_POLICY_INVALID, false);
    get_challenge(ch);
    size_t n = build_start(legacy, EUI_A, id, ch, PSKD);
    CHECK(send_raw_start(legacy, n, resp) == PB_BLE_CODE_UNAVAILABLE && resp[1] == PB_BLE_MSG_START_RESP && nothing_started(),
          "0x02: UNAVAILABLE");
    size_t m = build_start_ep(ep_msg, EUI_A, id, ch, &EP_OTBR, PSKD);
    CHECK(send_raw_start(ep_msg, m, resp) == PB_BLE_CODE_UNAVAILABLE && resp[1] == PB_BLE_MSG_START_EP_RESP &&
              nothing_started() && endpoint_slot_empty(),
          "0x04: UNAVAILABLE too, the endpoint is not kept");
    CHECK(poll() == PB_BLE_OPEN && g_join_calls == 0 && no_record(), "nothing reaches the worker or the flash");

    printf("\n-- (defence in depth) the controller refuses on its own if the admission layer did not --\n");
    fresh_with_policy(PB_ENROLL_ENDPOINT_POLICY_OPTIONAL, PB_ENROLL_ENDPOINT_POLICY_REQUIRED, true);
    CHECK(start_new(id, PSKD) == PB_BLE_CODE_ACCEPTED, "(setup) admission without the policy lets a 0x02 start through");
    CHECK(poll() == PB_BLE_OPEN && g_join_calls == 1 && !g_join_had_endpoint, "the worker hands it to the controller");
    CHECK(g_ot.joiner_start_calls == 0 && g_ctl.state == PB_ENROLL_STATE_WAITING && g_ctl.attempt == 0 &&
              g_ot.thread_stop_calls == 0,
          "the controller refuses it: no Joiner, still waiting");
    CHECK(old_record_and_dataset_untouched(), "and it erased neither the record nor the Dataset");
    query(id, st);
    CHECK((st[14] & PB_BLE_STATUS_F_REQUEST_MATCH) && st[15] == PB_BLE_PHASE_FAILED && st[16] == PB_BLE_REQ_RESULT_START_REFUSED &&
              (st[14] & PB_BLE_STATUS_F_ATTEMPT_ACTIVE) == 0,
          "the request is reported failed / start refused (0x80), a value version 1 clients know");
    CHECK(all_zero(g_adm.pskd, sizeof(g_adm.pskd)) && all_zero(g_adm.rec_tag, sizeof(g_adm.rec_tag)),
          "its PSKd and digest are gone");
    uint8_t id2[16];
    make_id(id2, 0xb1);
    CHECK(start_ep_new(id2, &EP_OTBR, PSKD) == PB_BLE_CODE_ACCEPTED && poll() == PB_BLE_OPEN && g_ot.joiner_start_calls == 1 &&
              g_ctl.pending_endpoint,
          "an endpoint start afterwards is accepted and started");
}

/* ---- pb-start with the bridge endpoint AND the management credential (0x06) ---- */

/*
 * The same bytes are in the app repository's
 * PowerBaseBLEKit/Tests/PowerBaseBLEKitTests/CodecTests.swift and in
 * docs/power-base-management.md (section 5):
 *   the endpoint vector above, then management format 1, hub id 10..1f,
 *   generation 20..2f, key 30..4f, PSKd "J01NME7K"
 */
#define GOLDEN_START_MGMT_HEX                                                                                        \
    "01060800" "6055f9fffe001234" "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf" "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf"               \
    "0100" "1634" "fd112233445500008c1f2e3d4c5b6a79"                                                                 \
    "0100" "101112131415161718191a1b1c1d1e1f" "202122232425262728292a2b2c2d2e2f"                                       \
    "303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f" "4a30314e4d45374b"
#define GOLDEN_START_MGMT_RESP_ACCEPTED_HEX "01860100"
#define GOLDEN_START_MGMT_RESP_INVALID_HEX "01861e00"

static pb_enroll_mgmt_credential_t golden_mgmt(void)
{
    pb_enroll_mgmt_credential_t m;
    for (int i = 0; i < 16; i++) {
        m.hub_id[i] = (uint8_t)(0x10 + i);
        m.generation[i] = (uint8_t)(0x20 + i);
    }
    for (int i = 0; i < 32; i++) {
        m.key[i] = (uint8_t)(0x30 + i);
    }
    return m;
}

static size_t build_start_mgmt(uint8_t out[PB_BLE_START_MGMT_REQ_MAX_LEN + 8], const uint8_t target[8],
                               const uint8_t request_id[16], const uint8_t challenge[16],
                               const pb_enroll_bridge_endpoint_t *ep, const pb_enroll_mgmt_credential_t *mgmt,
                               const char *pskd)
{
    size_t n = strlen(pskd);
    memset(out, 0, PB_BLE_START_MGMT_REQ_MAX_LEN + 8);
    out[0] = PB_BLE_PROTO_VERSION;
    out[1] = PB_BLE_MSG_START_MGMT_REQ;
    out[2] = (uint8_t)n;
    memcpy(&out[4], target, 8);
    memcpy(&out[12], request_id, 16);
    memcpy(&out[28], challenge, 16);
    out[44] = PB_BLE_BRIDGE_ENDPOINT_VERSION;
    out[46] = (uint8_t)(ep->port >> 8);
    out[47] = (uint8_t)ep->port;
    memcpy(&out[48], ep->addr, 16);
    out[64] = PB_BLE_MGMT_VERSION;
    memcpy(&out[66], mgmt->hub_id, 16);
    memcpy(&out[82], mgmt->generation, 16);
    memcpy(&out[98], mgmt->key, 32);
    memcpy(&out[130], pskd, n);
    return 130 + n;
}

/* Sends a copy (the handler wipes what it is given) and reports whether the
 * handler really wiped it. */
static uint8_t send_mgmt(const uint8_t *msg, size_t len, uint8_t resp[PB_BLE_SHORT_RESP_LEN], bool *wiped)
{
    uint8_t copy[PB_BLE_START_MGMT_REQ_MAX_LEN + 16];
    memcpy(copy, msg, len);
    pb_ble_adm_handle_start(&g_adm, copy, len, resp, g_now);
    if (wiped != NULL) {
        *wiped = all_zero(copy, len);
    }
    return resp[2];
}

static bool mgmt_slot_empty(void)
{
    return !g_adm.mgmt_pending && all_zero(&g_adm.mgmt, sizeof(g_adm.mgmt));
}

static void scenario_management_start(void)
{
    printf("\n-- pb-start with the management credential (0x06): golden vector, checks, secrets --\n");
    const pb_enroll_mgmt_credential_t cred = golden_mgmt();
    uint8_t golden[PB_BLE_START_MGMT_REQ_MAX_LEN + 8];
    size_t golden_len = hex_to_bytes(GOLDEN_START_MGMT_HEX, golden, sizeof(golden));
    CHECK(golden_len == 138 && golden_len == PB_BLE_START_MGMT_REQ_FIXED_LEN + 8, "the vector is 130 + 8 bytes");

    uint8_t id[16], ch[16];
    for (int i = 0; i < 16; i++) {
        id[i] = (uint8_t)(0xa0 + i);
        ch[i] = (uint8_t)(0xc0 + i);
    }
    uint8_t built[PB_BLE_START_MGMT_REQ_MAX_LEN + 8];
    size_t n = build_start_mgmt(built, EUI_A, id, ch, &EP_OTBR, &cred, PSKD);
    CHECK(n == golden_len && memcmp(built, golden, golden_len) == 0,
          "the layout in pb_ble_proto.h produces exactly the vector's bytes");
    CHECK(PB_BLE_MAX_REQUEST_LEN == 162, "the largest request is now 130 + 32 bytes");

    /* The real admission code takes those bytes. */
    fresh();
    g_rand_fixed = ch;
    uint8_t issued[16];
    CHECK(get_challenge(issued) == PB_BLE_CODE_OK && memcmp(issued, ch, 16) == 0, "(setup) the vector's challenge");
    uint8_t resp[PB_BLE_SHORT_RESP_LEN], expect[PB_BLE_SHORT_RESP_LEN];
    bool wiped = false;
    hex_to_bytes(GOLDEN_START_MGMT_RESP_ACCEPTED_HEX, expect, sizeof(expect));
    send_mgmt(golden, golden_len, resp, &wiped);
    CHECK(memcmp(resp, expect, sizeof(expect)) == 0, "answer is 01 86 01 00: its own response type, ACCEPTED");
    CHECK(wiped, "the decrypted request buffer (it held the key and the PSKd) is wiped");
    CHECK(g_adm.mgmt_pending && memcmp(&g_adm.mgmt, &cred, sizeof(cred)) == 0 && g_adm.endpoint_pending &&
              ep_equal(&g_adm.endpoint, &EP_OTBR) && g_adm.pskd_pending && strcmp(g_adm.pskd, PSKD) == 0,
          "hub id, generation, key, endpoint and PSKd were read from the vector's offsets");
    CHECK(g_ot.joiner_start_calls == 0 && no_record(), "accepted: nothing started and nothing stored in the BLE callback");

    /* The same request again, and the same id with another key. */
    send_mgmt(golden, golden_len, resp, NULL);
    CHECK(resp[1] == PB_BLE_MSG_START_MGMT_RESP && resp[2] == PB_BLE_CODE_DUPLICATE, "the exact retransmission: DUPLICATE");
    memcpy(built, golden, golden_len);
    built[98 + 31] ^= 0x01;
    send_mgmt(built, golden_len, resp, NULL);
    CHECK(resp[2] == PB_BLE_CODE_REQUEST_CONFLICT, "the same request id with another key: REQUEST_CONFLICT, never a duplicate");
    memcpy(built, golden, golden_len);
    built[82] ^= 0x01;
    send_mgmt(built, golden_len, resp, NULL);
    CHECK(resp[2] == PB_BLE_CODE_REQUEST_CONFLICT, "the same request id with another generation: REQUEST_CONFLICT");
    CHECK(g_adm.mgmt_pending && memcmp(&g_adm.mgmt, &cred, sizeof(cred)) == 0, "the accepted request's credential is unchanged");

    CHECK(poll() == PB_BLE_OPEN && g_join_calls == 1 && g_join_had_mgmt && g_join_had_endpoint &&
              memcmp(&g_join_mgmt, &cred, sizeof(cred)) == 0 && ep_equal(&g_join_endpoint, &EP_OTBR),
          "the worker hands exactly that credential and endpoint to the controller");
    CHECK(g_ctl_calls_under_lock == 0, "with the leaf lock released");
    CHECK(mgmt_slot_empty() && endpoint_slot_empty() && all_zero(g_adm.pskd, sizeof(g_adm.pskd)),
          "the admission slot is wiped once taken: no key left there");
    CHECK(g_ctl.state == PB_ENROLL_STATE_JOINING && g_ctl.pending_mgmt &&
              memcmp(&g_ctl.pending_mgmt_cred, &cred, sizeof(cred)) == 0 && no_record(),
          "the controller holds it in RAM while joining; nothing in NVS yet");

    /* Enrolled: one version 3 record. */
    g_ot.has_key = true;
    pb_enroll_ctl_on_joiner_done(&g_ctl, g_ot.last_attempt, PB_ENROLL_JOINER_OK, g_now);
    g_ot.complete = true;
    g_ot.id = NET_OTBR;
    pb_enroll_ctl_tick(&g_ctl, true, g_now);
    pb_enroll_record_t rec;
    CHECK(g_ctl.state == PB_ENROLL_STATE_ENROLLED && g_ctl.has_mgmt && !g_ctl.pending_mgmt &&
              all_zero(&g_ctl.pending_mgmt_cred, sizeof(g_ctl.pending_mgmt_cred)),
          "enrolled; the controller's RAM copy of the key is wiped");
    CHECK(pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_VALID && rec.has_mgmt &&
              rec.mgmt_state == PB_ENROLL_MGMT_ENROLLED && memcmp(&rec.mgmt, &cred, sizeof(cred)) == 0 &&
              rec.has_bridge_endpoint && ep_equal(&rec.bridge_endpoint, &EP_OTBR) &&
              memcmp(&rec.id, &NET_OTBR, sizeof(rec.id)) == 0,
          "one record (version 3) holds the marker, the endpoint and the credential");
    CHECK(poll() == PB_BLE_CLOSE_ENROLLED, "BLE enrollment closes");

    /* What is not a usable credential. */
    static const struct {
        int offset;
        int len;
        uint8_t set;
        const char *what;
    } bad[] = {
        {64, 1, 2, "management format 2 is INVALID_MANAGEMENT"},
        {64, 1, 0, "management format 0 is INVALID_MANAGEMENT"},
        {66, 16, 0, "an all-zero hub id is INVALID_MANAGEMENT"},
        {82, 16, 0, "an all-zero generation is INVALID_MANAGEMENT"},
        {98, 32, 0, "an all-zero key is INVALID_MANAGEMENT"},
    };
    hex_to_bytes(GOLDEN_START_MGMT_RESP_INVALID_HEX, expect, sizeof(expect));
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        fresh();
        get_challenge(ch);
        n = build_start_mgmt(built, EUI_A, id, ch, &EP_OTBR, &cred, PSKD);
        memset(&built[bad[i].offset], bad[i].set, (size_t)bad[i].len);
        send_mgmt(built, n, resp, &wiped);
        CHECK(memcmp(resp, expect, sizeof(expect)) == 0 && wiped && g_adm.q_count == 0 && mgmt_slot_empty() &&
                  !g_adm.pskd_pending && g_adm.rec_phase == PB_BLE_REC_NONE,
              bad[i].what);
    }
    CHECK(expect[1] == 0x86 && expect[2] == 0x1e, "(answered 01 86 1e 00; nothing queued, no attempt record, nothing kept)");

    /* Malformed: its layout, not its values. */
    fresh();
    get_challenge(ch);
    n = build_start_mgmt(built, EUI_A, id, ch, &EP_OTBR, &cred, PSKD);
    built[65] = 1;
    send_mgmt(built, n, resp, NULL);
    CHECK(resp[1] == PB_BLE_MSG_START_MGMT_RESP && resp[2] == PB_BLE_CODE_MALFORMED, "a non-zero reserved byte [65]: MALFORMED");
    n = build_start_mgmt(built, EUI_A, id, ch, &EP_OTBR, &cred, PSKD);
    send_mgmt(built, n - 1, resp, NULL);
    CHECK(resp[2] == PB_BLE_CODE_MALFORMED, "one byte short: MALFORMED");
    send_mgmt(built, n + 1, resp, NULL);
    CHECK(resp[2] == PB_BLE_CODE_MALFORMED, "one byte long: MALFORMED");
    /* The 0x04 bytes with the type changed to 0x06 are not a 0x06 message. */
    uint8_t ep_msg[PB_BLE_START_EP_REQ_MAX_LEN + 8];
    size_t ep_len = build_start_ep(ep_msg, EUI_A, id, ch, &EP_OTBR, PSKD);
    ep_msg[1] = PB_BLE_MSG_START_MGMT_REQ;
    send_mgmt(ep_msg, ep_len, resp, NULL);
    CHECK(resp[2] == PB_BLE_CODE_MALFORMED && g_adm.q_count == 0, "a 0x04-sized message with type 0x06: MALFORMED");
    /* ... and the challenge was not used up by any of those. */
    n = build_start_mgmt(built, EUI_A, id, ch, &EP_OTBR, &cred, PSKD);
    CHECK(send_mgmt(built, n, resp, NULL) == PB_BLE_CODE_ACCEPTED, "the malformed attempts did not spend the challenge");

    /* The other rules are the ones of the start with an endpoint. */
    fresh();
    get_challenge(ch);
    pb_enroll_bridge_endpoint_t bad_ep = EP_OTBR;
    bad_ep.port = 0;
    n = build_start_mgmt(built, EUI_A, id, ch, &bad_ep, &cred, PSKD);
    send_mgmt(built, n, resp, NULL);
    CHECK(resp[1] == PB_BLE_MSG_START_MGMT_RESP && resp[2] == PB_BLE_CODE_INVALID_ENDPOINT && mgmt_slot_empty(),
          "port 0: INVALID_ENDPOINT, answered with this message's own type");
    fresh();
    get_challenge(ch);
    n = build_start_mgmt(built, EUI_B, id, ch, &EP_OTBR, &cred, PSKD);
    send_mgmt(built, n, resp, NULL);
    CHECK(resp[2] == PB_BLE_CODE_WRONG_TARGET && mgmt_slot_empty(), "another board's EUI-64: WRONG_TARGET, the credential is not kept");
    fresh();
    get_challenge(ch);
    ch[0] ^= 1;
    n = build_start_mgmt(built, EUI_A, id, ch, &EP_OTBR, &cred, PSKD);
    send_mgmt(built, n, resp, NULL);
    CHECK(resp[2] == PB_BLE_CODE_CHALLENGE_INVALID && mgmt_slot_empty(), "a wrong challenge: CHALLENGE_INVALID");

    /* An image that requires the endpoint takes it (it carries one); an
     * unusable policy takes nothing. */
    fresh_with_policy(PB_ENROLL_ENDPOINT_POLICY_REQUIRED, PB_ENROLL_ENDPOINT_POLICY_REQUIRED, false);
    get_challenge(ch);
    n = build_start_mgmt(built, EUI_A, id, ch, &EP_OTBR, &cred, PSKD);
    CHECK(send_mgmt(built, n, resp, NULL) == PB_BLE_CODE_ACCEPTED, "'endpoint required' image: the 0x06 start is accepted");
    fresh_with_policy(PB_ENROLL_ENDPOINT_POLICY_INVALID, PB_ENROLL_ENDPOINT_POLICY_INVALID, false);
    get_challenge(ch);
    n = build_start_mgmt(built, EUI_A, id, ch, &EP_OTBR, &cred, PSKD);
    CHECK(send_mgmt(built, n, resp, NULL) == PB_BLE_CODE_UNAVAILABLE && mgmt_slot_empty(),
          "unusable image policy: UNAVAILABLE, nothing kept");

    /* A queued credential leaves with the attempt. */
    fresh();
    get_challenge(ch);
    n = build_start_mgmt(built, EUI_A, id, ch, &EP_OTBR, &cred, PSKD);
    send_mgmt(built, n, resp, NULL);
    uint8_t cancel[PB_BLE_CANCEL_REQ_LEN] = {PB_BLE_PROTO_VERSION, PB_BLE_MSG_CANCEL_REQ, 0, 0};
    memcpy(&cancel[4], EUI_A, 8);
    memcpy(&cancel[12], id, 16);
    pb_ble_adm_handle_cancel(&g_adm, cancel, sizeof(cancel), resp);
    CHECK(resp[2] == PB_BLE_CODE_OK && mgmt_slot_empty() && !g_adm.pskd_pending,
          "cancelling a queued start wipes its credential with its PSKd");
    get_challenge(ch);
    n = build_start_mgmt(built, EUI_A, id, ch, &EP_OTBR, &cred, PSKD);
    send_mgmt(built, n, resp, NULL);
    pb_ble_adm_wipe(&g_adm);
    CHECK(mgmt_slot_empty(), "pb_ble_adm_wipe() leaves no credential");

    /* The older starts are untouched: no credential, older record versions. */
    fresh();
    uint8_t id2[16];
    make_id(id2, 0x90);
    CHECK(start_ep_new(id2, &EP_OTBR, PSKD) == PB_BLE_CODE_ACCEPTED && !g_adm.mgmt_pending, "(the 0x04 start still carries none)");
    poll();
    CHECK(g_join_had_endpoint && !g_join_had_mgmt && !g_ctl.pending_mgmt, "and the controller is given none");
    g_ot.has_key = true;
    pb_enroll_ctl_on_joiner_done(&g_ctl, g_ot.last_attempt, PB_ENROLL_JOINER_OK, g_now);
    g_ot.complete = true;
    g_ot.id = NET_OTBR;
    pb_enroll_ctl_tick(&g_ctl, true, g_now);
    CHECK(pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_VALID && !rec.has_mgmt && rec.has_bridge_endpoint &&
              !g_ctl.has_mgmt,
          "a 0x04 enrollment still writes record version 2, without a credential");
}

/* The key never reaches the log, as hex or as bytes. */
static void scenario_management_never_logged(void)
{
    printf("\n-- the management key never reaches the log output --\n");
    char path[] = "/tmp/pb_ble_mgmt_logXXXXXX";
    int fd = mkstemp(path);
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    dup2(fd, STDERR_FILENO);

    const pb_enroll_mgmt_credential_t cred = golden_mgmt();
    uint8_t id[16], ch[16], built[PB_BLE_START_MGMT_REQ_MAX_LEN + 8], resp[PB_BLE_SHORT_RESP_LEN];
    make_id(id, 0x91);
    fresh();
    get_challenge(ch);
    size_t n = build_start_mgmt(built, EUI_A, id, ch, &EP_OTBR, &cred, PSKD);
    built[64] = 9; /* refused */
    send_mgmt(built, n, resp, NULL);
    get_challenge(ch);
    n = build_start_mgmt(built, EUI_A, id, ch, &EP_OTBR, &cred, PSKD);
    send_mgmt(built, n, resp, NULL);
    poll();
    g_ot.has_key = true;
    pb_enroll_ctl_on_joiner_done(&g_ctl, g_ot.last_attempt, PB_ENROLL_JOINER_OK, g_now);
    g_ot.complete = true;
    g_ot.id = NET_OTBR;
    pb_enroll_ctl_tick(&g_ctl, true, g_now);
    poll();

    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    static char log[65536];
    memset(log, 0, sizeof(log));
    lseek(fd, 0, SEEK_SET);
    ssize_t len = read(fd, log, sizeof(log) - 1);
    close(fd);
    unlink(path);
    CHECK(len > 0, "the scenario produced log output");
    bool leaked = false;
    const struct {
        const uint8_t *p;
        size_t n;
    } secrets[] = {{cred.key, 32}, {cred.generation, 16}, {cred.hub_id, 16}};
    for (size_t s2 = 0; s2 < sizeof(secrets) / sizeof(secrets[0]); s2++) {
        for (size_t i = 0; i + 4 <= secrets[s2].n; i++) {
            char lower[9] = {0}, upper[9] = {0};
            for (int b = 0; b < 4; b++) {
                sprintf(&lower[2 * b], "%02x", secrets[s2].p[i + b]);
                sprintf(&upper[2 * b], "%02X", secrets[s2].p[i + b]);
            }
            leaked = leaked || strstr(log, lower) != NULL || strstr(log, upper) != NULL;
        }
        for (ssize_t i = 0; i + (ssize_t)secrets[s2].n <= len; i++) {
            leaked = leaked || memcmp(&log[i], secrets[s2].p, secrets[s2].n) == 0;
        }
    }
    CHECK(!leaked, "no key, generation or hub id in the log, as hex or as bytes");
    CHECK(strstr(log, PSKD) == NULL, "and no PSKd");
}

/* ---- the release receipt (PB_BLE_MSG_RECEIPT_REQ on pb-status) ---- */

static int g_receipt_calls;
static bool g_receipt_present;

static bool fake_receipt_source(uint8_t receipt[PB_BLE_RECEIPT_LEN], void *ctx)
{
    (void)ctx;
    g_receipt_calls++;
    /* A source that fails still scribbles: what it leaves must not leak. */
    for (size_t i = 0; i < PB_BLE_RECEIPT_LEN; i++) {
        receipt[i] = (uint8_t)(0x40 + i);
    }
    return g_receipt_present;
}

static void scenario_release_receipt(void)
{
    printf("\n-- the release receipt: read inside the session, changes nothing --\n");
    uint8_t req[PB_BLE_RECEIPT_REQ_LEN] = {PB_BLE_PROTO_VERSION, PB_BLE_MSG_RECEIPT_REQ, 0, 0};
    uint8_t out[PB_BLE_RECEIPT_RESP_LEN];
    uint8_t expected[PB_BLE_RECEIPT_LEN];
    uint8_t zeros[PB_BLE_RECEIPT_LEN] = {0};
    for (size_t i = 0; i < PB_BLE_RECEIPT_LEN; i++) {
        expected[i] = (uint8_t)(0x40 + i);
    }
    memcpy(&req[4], EUI_A, 8);
    CHECK(PB_BLE_RECEIPT_RESP_LEN == 16 + PB_BLE_RECEIPT_LEN && PB_BLE_MAX_RESPONSE_LEN >= PB_BLE_RECEIPT_RESP_LEN,
          "the response is a 16-byte header and the 96-byte management message");
    CHECK(pb_ble_adm_is_receipt_request(req, sizeof(req)) && !pb_ble_adm_is_receipt_request(NULL, 0),
          "the transport recognises the message by its type");
    uint8_t status_req[PB_BLE_STATUS_REQ_LEN] = {PB_BLE_PROTO_VERSION, PB_BLE_MSG_STATUS_REQ};
    CHECK(!pb_ble_adm_is_receipt_request(status_req, sizeof(status_req)), "a pb-status request is not one");

    g_receipt_calls = 0;
    g_receipt_present = true;
    size_t n = pb_ble_adm_handle_receipt(EUI_A, req, sizeof(req), out, fake_receipt_source, NULL);
    CHECK(n == PB_BLE_RECEIPT_RESP_LEN && out[0] == PB_BLE_PROTO_VERSION && out[1] == PB_BLE_MSG_RECEIPT_RESP &&
              out[2] == PB_BLE_CODE_OK && out[3] == 0 && memcmp(&out[4], EUI_A, 8) == 0 &&
              out[12] == PB_BLE_RECEIPT_VERSION && out[13] == 1 && out[14] == 0 && out[15] == 0 &&
              memcmp(&out[16], expected, PB_BLE_RECEIPT_LEN) == 0 && g_receipt_calls == 1,
          "a board with a receipt: OK, its own EUI-64, 'present', and the receipt byte for byte");

    g_receipt_present = false;
    n = pb_ble_adm_handle_receipt(EUI_A, req, sizeof(req), out, fake_receipt_source, NULL);
    CHECK(n == PB_BLE_RECEIPT_RESP_LEN && out[2] == PB_BLE_CODE_OK && out[13] == 0 &&
              memcmp(&out[16], zeros, PB_BLE_RECEIPT_LEN) == 0,
          "a board without one: OK, 'none', and zeros -- not whatever the source left in the buffer");
    n = pb_ble_adm_handle_receipt(EUI_A, req, sizeof(req), out, NULL, NULL);
    CHECK(n == PB_BLE_RECEIPT_RESP_LEN && out[2] == PB_BLE_CODE_OK && out[13] == 0, "a build without a source: 'none'");

    g_receipt_present = true;
    g_receipt_calls = 0;
#define REFUSED(stmt, len_expr, code, why)                                                                           \
    do {                                                                                                             \
        uint8_t r[PB_BLE_RECEIPT_REQ_LEN + 1] = {0};                                                                 \
        memcpy(r, req, sizeof(req));                                                                                 \
        stmt;                                                                                                        \
        n = pb_ble_adm_handle_receipt(EUI_A, r, (len_expr), out, fake_receipt_source, NULL);                         \
        CHECK(n == PB_BLE_RECEIPT_RESP_LEN && out[1] == PB_BLE_MSG_RECEIPT_RESP && out[2] == (code) && out[13] == 0 && \
                  memcmp(&out[16], zeros, PB_BLE_RECEIPT_LEN) == 0,                                                  \
              why);                                                                                                  \
    } while (0)
    REFUSED(memcpy(&r[4], EUI_B, 8), sizeof(req), PB_BLE_CODE_WRONG_TARGET, "another board's EUI-64: WRONG_TARGET, no receipt");
    REFUSED(r[0] = 2, sizeof(req), PB_BLE_CODE_UNSUPPORTED_VERSION, "another protocol version: refused");
    REFUSED(r[2] = 1, sizeof(req), PB_BLE_CODE_MALFORMED, "a non-zero reserved byte: MALFORMED");
    REFUSED(r[3] = 1, sizeof(req), PB_BLE_CODE_MALFORMED, "the other reserved byte: MALFORMED");
    REFUSED((void)0, sizeof(req) - 1, PB_BLE_CODE_MALFORMED, "one byte short: MALFORMED");
    REFUSED((void)0, sizeof(req) + 1, PB_BLE_CODE_MALFORMED, "one byte long: MALFORMED");
    REFUSED(r[1] = PB_BLE_MSG_STATUS_REQ, sizeof(req), PB_BLE_CODE_MALFORMED, "another type: MALFORMED");
#undef REFUSED
    CHECK(g_receipt_calls == 0, "a refused request never reads the record");
    n = pb_ble_adm_handle_receipt(EUI_A, NULL, 0, out, fake_receipt_source, NULL);
    CHECK(n == PB_BLE_RECEIPT_RESP_LEN && out[2] == PB_BLE_CODE_MALFORMED, "an empty request: MALFORMED");

    /* A board from before the capability gets this message on pb-status and
     * answers like for any unknown type there. */
    fresh();
    uint8_t st[PB_BLE_STATUS_RESP_LEN];
    n = pb_ble_adm_handle_status(&g_adm, req, sizeof(req), st, g_now);
    CHECK(n == PB_BLE_STATUS_RESP_LEN && st[1] == PB_BLE_MSG_STATUS_RESP && st[2] == PB_BLE_CODE_MALFORMED,
          "the status handler itself refuses the type (what firmware without the capability answers)");
    uint8_t status_ok[PB_BLE_STATUS_RESP_LEN];
    n = pb_ble_adm_handle_status(&g_adm, status_req, sizeof(status_req), status_ok, g_now);
    CHECK(n == PB_BLE_STATUS_RESP_LEN && status_ok[2] == PB_BLE_CODE_OK && g_join_calls == 0,
          "and pb-status works as before; reading a receipt started nothing");
}

int main(void)
{
    uint8_t d[32];
    static const uint8_t abc[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                                    0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                                    0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    test_sha256((const uint8_t *)"abc", 3, d, NULL);
    CHECK(memcmp(d, abc, 32) == 0, "(test digest is SHA-256: \"abc\" vector)");

    scenario_credential_record();
    scenario_status();
    scenario_start_accepted();
    scenario_start_rejections();
    scenario_duplicates_and_concurrency();
    scenario_disconnect();
    scenario_cancel();
    scenario_failure_and_retry();
    scenario_joiner_success_is_not_enrollment();
    scenario_queue_full();
    scenario_auth_limits();
    scenario_window();
    scenario_no_secret_in_logs();
    scenario_endpoint_golden_vector();
    scenario_endpoint_lengths_and_format();
    scenario_endpoint_rejections();
    scenario_endpoint_duplicates();
    scenario_endpoint_pending_is_dropped();
    scenario_endpoint_enrollment();
    scenario_endpoint_never_logged();
    scenario_endpoint_policy_required();
    scenario_endpoint_policy_other();
    scenario_management_start();
    scenario_management_never_logged();
    scenario_release_receipt();

    printf("\n%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
