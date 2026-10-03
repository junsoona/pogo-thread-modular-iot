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

static bool test_random(uint8_t *buf, size_t len, void *ctx)
{
    (void)ctx;
    if (g_rand_fails) {
        return false;
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

static esp_err_t w_request_join(char pskd[PB_ENROLL_PSKD_BUF_LEN], void *ctx)
{
    (void)ctx;
    if (g_lock_depth != 0) {
        g_ctl_calls_under_lock++;
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
    g_lock_depth = 0;
    g_ctl_calls_under_lock = 0;
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

    printf("\n%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
