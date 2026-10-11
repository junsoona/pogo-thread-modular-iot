/*
 * Host test for the REAL local re-enrollment code (docs/power-base.md "BOOT
 * 버튼 로컬 재등록"):
 *   components/power_base_reenroll/pb_reenroll_button.c  (long press, GPIO check)
 *   components/power_base_reenroll/pb_reenroll_store.c   (window request, reset journal)
 *   components/power_base_reenroll/pb_reenroll.c         (gate, boot order, the reset)
 *   components/power_base_ble_enroll/pb_ble_admission.c  (the two BLE messages,
 *       the window, the worker pass)
 * together with the code they have to live next to:
 *   components/power_base_enrollment/power_base_enrollment.c (record, boot rule, join)
 *   components/power_base_mgmt/pb_mgmt_client.c + pb_mgmt_proto.c (remote release, receipt)
 *   components/module_config/gpio_profile.c
 * linked against the functional NVS simulator.
 *
 * What is modelled, not real:
 *   - OpenThread's store: two blobs in the simulated flash (namespace
 *     "openthread"), so that erasing them, lying about it and rebooting
 *     behave like the real thing;
 *   - a boot: sim_boot() calls the real decisions in the order
 *     power_base/main/app_main.c calls them (pb_reenroll_boot, then the
 *     enrollment controller's boot rule). The order is copied from app_main.c;
 *     every decision in it is production code;
 *   - power loss: a boot runs in a forked child over a flash file and ends
 *     right before a chosen commit (nvs_sim_power_cut_at_commit);
 *   - a Security 2 session: what the protocomm binding reports to the
 *     admission code (pb_ble_adm_session_changed / sec_request_begin/end).
 *     The handshake itself runs for real in tools/host_ble_sec2.
 * NOT here: a button, a GPIO, a BLE link, OpenThread, a power supply.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I tools/host_test_stubs/nvs_sim -I tools/host_test_stubs \
 *      -I components/power_base_enrollment/include -I components/power_base_mgmt/include \
 *      -I components/power_base_ble_enroll/include -I components/power_base_reenroll/include \
 *      -I components/module_config/include \
 *      -o /tmp/test_power_base_reenroll tools/test_power_base_reenroll.c \
 *      components/power_base_reenroll/pb_reenroll_button.c components/power_base_reenroll/pb_reenroll_store.c \
 *      components/power_base_reenroll/pb_reenroll.c components/power_base_ble_enroll/pb_ble_admission.c \
 *      components/power_base_enrollment/power_base_enrollment.c components/power_base_mgmt/pb_mgmt_client.c \
 *      components/power_base_mgmt/pb_mgmt_proto.c components/module_config/gpio_profile.c \
 *      tools/host_test_stubs/nvs_sim/nvs_sim.c \
 *   && /tmp/test_power_base_reenroll
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "host_sha256.h"
#include "nvs.h"
#include "nvs_sim.h"
#include "power_base_ble_enroll/pb_ble_admission.h"
#include "power_base_enrollment/power_base_enrollment.h"
#include "power_base_mgmt/pb_mgmt_client.h"
#include "power_base_mgmt/pb_mgmt_proto.h"
#include "power_base_reenroll/pb_reenroll.h"
#include "power_base_reenroll/pb_reenroll_button.h"
#include "power_base_reenroll/pb_reenroll_store.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        g_checks++;                                                                                                  \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                                   \
            g_failures++;                                                                                            \
        } else {                                                                                                     \
            printf("ok:   %s\n", msg);                                                                               \
        }                                                                                                            \
    } while (0)

/* Made-up identifiers; nothing here is a real board, hub, network or key. */
static const uint8_t EUI_A[8] = {0x60, 0x55, 0xf9, 0xff, 0xfe, 0x00, 0x12, 0x34};
static const uint8_t EUI_B[8] = {0x60, 0x55, 0xf9, 0xff, 0xfe, 0x00, 0x56, 0x78};
static const pb_enroll_network_id_t NET_OLD = {
    .ext_pan_id = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88},
    .mesh_local_prefix = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00},
};
static const pb_enroll_bridge_endpoint_t EP_OLD = {
    .addr = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00, 0x8c, 0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79},
    .port = 5684,
};
static const pb_enroll_network_id_t NET_NEW = {
    .ext_pan_id = {0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8},
    .mesh_local_prefix = {0xfd, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x00, 0x00},
};
static const pb_enroll_bridge_endpoint_t EP_NEW = {
    .addr = {0xfd, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x00, 0x00, 0x02, 0x04, 0x06, 0x08, 0x0a, 0x0c, 0x0e, 0x10},
    .port = 5690,
};
static const char *PSKD = "J01NME7K";

static void seq(uint8_t *out, size_t len, uint8_t first)
{
    for (size_t i = 0; i < len; i++) {
        out[i] = (uint8_t)(first + i);
    }
}

static pb_enroll_mgmt_credential_t credential(uint8_t seed)
{
    pb_enroll_mgmt_credential_t c;
    seq(c.hub_id, sizeof(c.hub_id), seed);
    seq(c.generation, sizeof(c.generation), (uint8_t)(seed + 0x10));
    seq(c.key, sizeof(c.key), (uint8_t)(seed + 0x20));
    return c;
}

/* ======================= the simulated flash ======================= */

#define OT_NS "openthread"
#define OT_KEY_DATASET "ds"
#define OT_KEY_NETINFO "ni"

/* What "OpenThread stored": has_key(1) complete(1) ext_pan_id(8) mesh_local_prefix(8). */
static void ot_store(bool complete, const pb_enroll_network_id_t *id)
{
    uint8_t ds[18] = {1, complete ? 1 : 0};
    memcpy(&ds[2], id->ext_pan_id, 8);
    memcpy(&ds[10], id->mesh_local_prefix, 8);
    uint8_t ni[4] = {0xde, 0xad, 0xbe, 0xef};
    nvs_handle_t h;
    if (nvs_open(OT_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, OT_KEY_DATASET, ds, sizeof(ds));
        nvs_set_blob(h, OT_KEY_NETINFO, ni, sizeof(ni));
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool ot_key_present(const char *key)
{
    return nvs_sim_peek_blob("nvs", OT_NS, key, NULL, 0) >= 0;
}

static bool ot_empty(void)
{
    return !ot_key_present(OT_KEY_DATASET) && !ot_key_present(OT_KEY_NETINFO);
}

/* What a pb_thread:mode=2 board's fixed "cfg" partition holds (made up), and
 * something of somebody else's in the dynamic partition. None of it is this
 * feature's to touch. */
static void cfg_store(void)
{
    uint8_t board[40], cred[64], bridge[24], other[12];
    seq(board, sizeof(board), 0x01);
    seq(cred, sizeof(cred), 0x41);
    seq(bridge, sizeof(bridge), 0x81);
    seq(other, sizeof(other), 0xc1);
    nvs_sim_put_blob("cfg", "power_base_cfg", "cfg", board, sizeof(board));
    nvs_sim_put_blob("cfg", "pb_ble_auth", "cred", cred, sizeof(cred));
    nvs_sim_put_u8("cfg", "pb_thread", "mode", 2);
    nvs_sim_put_blob("cfg", "pb_bridge", "cfg", bridge, sizeof(bridge));
    nvs_sim_put_blob("nvs", "phy", "cal", other, sizeof(other));
}

typedef struct {
    int board_len, cred_len, bridge_len, other_len;
    uint8_t board[40], cred[64], bridge[24], other[12];
} cfg_image_t;

static void cfg_read(cfg_image_t *img)
{
    memset(img, 0, sizeof(*img));
    img->board_len = nvs_sim_peek_blob("cfg", "power_base_cfg", "cfg", img->board, sizeof(img->board));
    img->cred_len = nvs_sim_peek_blob("cfg", "pb_ble_auth", "cred", img->cred, sizeof(img->cred));
    img->bridge_len = nvs_sim_peek_blob("cfg", "pb_bridge", "cfg", img->bridge, sizeof(img->bridge));
    img->other_len = nvs_sim_peek_blob("nvs", "phy", "cal", img->other, sizeof(img->other));
}

/* The enrollment record and OpenThread's store, byte for byte. */
typedef struct {
    int rec_len, ds_len, ni_len;
    uint8_t rec[PB_ENROLL_RECORD_MAX_LEN], ds[18], ni[4];
} net_image_t;

static void net_read(net_image_t *img)
{
    memset(img, 0, sizeof(*img));
    img->rec_len = nvs_sim_peek_blob("nvs", PB_ENROLL_NVS_NAMESPACE, PB_ENROLL_NVS_KEY, img->rec, sizeof(img->rec));
    img->ds_len = nvs_sim_peek_blob("nvs", OT_NS, OT_KEY_DATASET, img->ds, sizeof(img->ds));
    img->ni_len = nvs_sim_peek_blob("nvs", OT_NS, OT_KEY_NETINFO, img->ni, sizeof(img->ni));
}

static bool net_same(const net_image_t *x, const net_image_t *y)
{
    return memcmp(x, y, sizeof(*x)) == 0;
}

/* A board enrolled with hub "old": record version 3, complete Dataset. */
static void world_enrolled_v3(const uint8_t eui[8])
{
    nvs_sim_reset();
    cfg_store();
    pb_enroll_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.id = NET_OLD;
    rec.has_bridge_endpoint = true;
    rec.bridge_endpoint = EP_OLD;
    rec.has_mgmt = true;
    rec.mgmt_state = PB_ENROLL_MGMT_ENROLLED;
    rec.mgmt = credential(0x10);
    if (pb_enroll_record_store(eui, &rec) != ESP_OK) {
        printf("FAIL: world_enrolled_v3 could not store its record\n");
        g_failures++;
    }
    ot_store(true, &NET_OLD);
}

static void set_record_state(const uint8_t eui[8], pb_enroll_mgmt_state_t state, uint8_t release_flags)
{
    pb_enroll_record_t rec;
    if (pb_enroll_record_load(eui, &rec) != PB_ENROLL_MARKER_VALID) {
        printf("FAIL: set_record_state found no record\n");
        g_failures++;
        return;
    }
    rec.mgmt_state = state;
    rec.release_flags = release_flags;
    seq(rec.command_id, sizeof(rec.command_id), 0xe0);
    if (pb_enroll_record_store(eui, &rec) != ESP_OK) {
        printf("FAIL: set_record_state could not store\n");
        g_failures++;
    }
}

/* ======================= fakes under the real code ======================= */

static int g_quiesce_calls, g_erase_calls;
static bool g_erase_fails;
/* The erase returns no error and leaves the store as it was. */
static bool g_erase_lies;
/* The erase removes the Dataset and leaves another entry behind. */
static bool g_erase_partial;

static void r_quiesce(void *ctx)
{
    (void)ctx;
    g_quiesce_calls++;
}

static esp_err_t r_network_erase(void *ctx)
{
    (void)ctx;
    g_erase_calls++;
    if (g_erase_fails) {
        return ESP_FAIL;
    }
    if (g_erase_lies) {
        return ESP_OK;
    }
    nvs_handle_t h;
    if (nvs_open(OT_NS, NVS_READWRITE, &h) != ESP_OK) {
        return ESP_FAIL;
    }
    (void)nvs_erase_key(h, OT_KEY_DATASET);
    if (!g_erase_partial) {
        (void)nvs_erase_key(h, OT_KEY_NETINFO);
    }
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* Reads the store back, like the firmware reads OpenThread's NVS namespace. */
static bool r_network_erased(void *ctx)
{
    (void)ctx;
    return ot_empty();
}

/* What the firmware's BLE service asks before it hands out a release
 * receipt: is a network stored? */
static bool r_no_network_stored(void *ctx)
{
    (void)ctx;
    return ot_empty();
}

static bool receipt_load(const uint8_t eui64[8], uint8_t out[PB_MGMT_RECEIPT_LEN])
{
    return pb_mgmt_receipt_load(eui64, r_no_network_stored, NULL, out);
}

/* The receipt the stored record holds, whatever the stores say: for
 * comparing bytes only -- no board hands one out like this. */
static bool r_not_asked(void *ctx)
{
    (void)ctx;
    return true;
}

static bool receipt_in_record(const uint8_t eui64[8], uint8_t out[PB_MGMT_RECEIPT_LEN])
{
    return pb_mgmt_receipt_load(eui64, r_not_asked, NULL, out);
}

static const pb_reenroll_ops_t k_reset_ops = {
    .quiesce = r_quiesce,
    .network_erase = r_network_erase,
    .network_erased = r_network_erased,
    .ctx = NULL,
};

static void fakes_reset(void)
{
    g_quiesce_calls = 0;
    g_erase_calls = 0;
    g_erase_fails = false;
    g_erase_lies = false;
    g_erase_partial = false;
}

/* The enrollment controller's OpenThread operations. */
static int g_thread_starts, g_joiner_starts;
static uint32_t g_last_attempt;

static esp_err_t e_joiner_start(const char *pskd, uint32_t attempt, void *ctx)
{
    (void)pskd;
    (void)ctx;
    g_joiner_starts++;
    g_last_attempt = attempt;
    return ESP_OK;
}

static void e_joiner_stop(void *ctx)
{
    (void)ctx;
}

static bool e_read_dataset(pb_enroll_dataset_info_t *out, void *ctx)
{
    (void)ctx;
    uint8_t ds[18];
    memset(out, 0, sizeof(*out));
    if (nvs_sim_peek_blob("nvs", OT_NS, OT_KEY_DATASET, ds, sizeof(ds)) != (int)sizeof(ds)) {
        return true; /* nothing stored */
    }
    out->has_network_key = ds[0] != 0;
    out->complete = ds[1] != 0;
    memcpy(out->id.ext_pan_id, &ds[2], 8);
    memcpy(out->id.mesh_local_prefix, &ds[10], 8);
    return true;
}

static esp_err_t e_thread_start_stored(bool require_complete, void *ctx)
{
    (void)ctx;
    pb_enroll_dataset_info_t ds;
    e_read_dataset(&ds, NULL);
    if (!ds.has_network_key || (require_complete && !ds.complete)) {
        return ESP_ERR_INVALID_STATE;
    }
    g_thread_starts++;
    return ESP_OK;
}

static void e_thread_stop(void *ctx)
{
    (void)ctx;
}

static const pb_enroll_ops_t k_enroll_ops = {
    .joiner_start = e_joiner_start,
    .joiner_stop = e_joiner_stop,
    .thread_start_stored = e_thread_start_stored,
    .thread_stop = e_thread_stop,
    .read_dataset = e_read_dataset,
    .ctx = NULL,
};

/* ======================= a boot, in app_main.c's order ======================= */

typedef enum {
    SIM_RESUMED = 0, /* the stored enrollment was resumed: Thread on, services may start */
    SIM_WAITING,     /* the ordinary safe wait: BLE enrollment would open */
    SIM_WINDOW,      /* a re-enrollment window boot */
    SIM_RESTART,     /* a journaled reset was finished: the chip restarts */
    SIM_STOPPED,     /* safe stop */
    SIM_RELEASE_PENDING, /* a remote release is finished first (pb_mgmt_runtime_finish_pending_release) */
} sim_boot_t;

static pb_reenroll_boot_t g_boot;
static pb_enroll_ctl_t g_ctl;

static bool mgmt_hmac(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[32], void *ctx)
{
    (void)ctx;
    host_hmac_sha256(key, key_len, data, len, out);
    return true;
}

static sim_boot_t sim_boot(const uint8_t eui[8], bool feature_enabled)
{
    g_thread_starts = 0;
    g_joiner_starts = 0;
    /* pb_reenroll_runtime_boot() */
    pb_reenroll_boot(eui, feature_enabled, &k_reset_ops, &g_boot);
    if (g_boot.action == PB_REENROLL_BOOT_RESTART) {
        return SIM_RESTART;
    }
    if (g_boot.action == PB_REENROLL_BOOT_SAFE_STOP) {
        return SIM_STOPPED;
    }
    /* pb_mgmt_runtime_finish_pending_release(): only whether it has work. */
    pb_mgmt_ops_t mops;
    memset(&mops, 0, sizeof(mops));
    mops.hmac = mgmt_hmac;
    pb_mgmt_client_t mgmt;
    pb_mgmt_client_init(&mgmt, &mops, eui);
    pb_mgmt_boot_t mboot = pb_mgmt_client_load(&mgmt);
    pb_mgmt_client_wipe(&mgmt);
    if (mboot == PB_MGMT_BOOT_RELEASE_PENDING) {
        return SIM_RELEASE_PENDING;
    }
    if (g_boot.action == PB_REENROLL_BOOT_WINDOW) {
        /* pb_enrollment_runtime_start_reenroll_window(): the boot rule is
         * NOT applied. */
        pb_enroll_ctl_init(&g_ctl, &k_enroll_ops, eui);
        return SIM_WINDOW;
    }
    /* pb_enrollment_runtime_start() */
    pb_enroll_ctl_init(&g_ctl, &k_enroll_ops, eui);
    return pb_enroll_ctl_boot(&g_ctl) == PB_ENROLL_STATE_ENROLLED ? SIM_RESUMED : SIM_WAITING;
}

/* ======================= the BLE side ======================= */

static uint32_t g_rand_state = 0x1234abcdu;

static bool adm_random(uint8_t *buf, size_t len, void *ctx)
{
    (void)ctx;
    for (size_t i = 0; i < len; i++) {
        g_rand_state = g_rand_state * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(g_rand_state >> 24);
    }
    return true;
}

static bool adm_sha256(const uint8_t *data, size_t len, uint8_t out[32], void *ctx)
{
    (void)ctx;
    host_sha256(data, len, out);
    return true;
}

static const pb_ble_adm_ops_t k_adm_ops = {.random = adm_random, .sha256 = adm_sha256, .ctx = NULL};

static pb_ble_adm_t g_adm;
static uint32_t g_now;
static const uint8_t *g_board_eui;
static int g_join_requests, g_reset_runs;
/* In a window boot the worker reports "enrolled" while the record says so
 * (power_base/main/enrollment_ble.c, ctl_get_status). */
static bool g_window_boot;

static esp_err_t w_request_join(char pskd[PB_ENROLL_PSKD_BUF_LEN], const pb_enroll_bridge_endpoint_t *endpoint,
                                const pb_enroll_mgmt_credential_t *mgmt, void *ctx)
{
    (void)ctx;
    g_join_requests++;
    if (mgmt != NULL) {
        return pb_enroll_ctl_request_join_managed(&g_ctl, pskd, endpoint, mgmt, g_now);
    }
    if (endpoint != NULL) {
        return pb_enroll_ctl_request_join_with_endpoint(&g_ctl, pskd, endpoint, g_now);
    }
    return pb_enroll_ctl_request_join(&g_ctl, pskd, g_now);
}

static esp_err_t w_cancel(void *ctx)
{
    (void)ctx;
    return pb_enroll_ctl_cancel(&g_ctl);
}

static void w_get_status(pb_ble_ctl_status_t *out, void *ctx)
{
    (void)ctx;
    out->state = g_ctl.state;
    out->last = g_ctl.last;
    out->attempt = g_ctl.attempt;
    if (g_window_boot) {
        pb_reenroll_snapshot_t snap;
        pb_reenroll_snapshot(g_board_eui, &snap);
        if (snap.condition == PB_REENROLL_COND_ENROLLED) {
            out->state = PB_ENROLL_STATE_ENROLLED;
        }
    }
}

/* power_base/main/enrollment_reenroll.c: pb_reenroll_runtime_local_reset(). */
static uint8_t w_local_reset(const uint8_t op_id[PB_BLE_OP_ID_LEN], uint8_t *reason, void *ctx)
{
    (void)ctx;
    g_reset_runs++;
    pb_reenroll_reason_t why = PB_REENROLL_REASON_NONE;
    pb_reenroll_reset_result_t result = pb_reenroll_reset_begin(g_board_eui, op_id, &k_reset_ops, &why);
    *reason = (uint8_t)why;
    return (uint8_t)result;
}

/* power_base/main/enrollment_reenroll.c: pb_reenroll_runtime_snapshot(). */
static void w_get_local_reset(pb_ble_lr_snapshot_t *out, void *ctx)
{
    (void)ctx;
    pb_reenroll_snapshot_t snap;
    pb_reenroll_snapshot(g_board_eui, &snap);
    memset(out, 0, sizeof(*out));
    out->condition = (uint8_t)snap.condition;
    if (snap.journal_status == PB_REENROLL_REC_VALID) {
        out->journal_valid = true;
        out->journal_verified = snap.journal.stage == PB_REENROLL_STAGE_VERIFIED;
        memcpy(out->journal_op, snap.journal.op_id, sizeof(out->journal_op));
    }
}

static const pb_ble_worker_ops_t k_worker = {
    .request_join = w_request_join,
    .cancel = w_cancel,
    .get_status = w_get_status,
    .lock = NULL,
    .unlock = NULL,
    .ctx = NULL,
    .local_reset = w_local_reset,
    .get_local_reset = w_get_local_reset,
};

/* What pb_ble_service_start() does, for a board with (or without) the
 * button, in a window boot or an ordinary one. */
static void ble_start(const uint8_t eui[8], bool local_reset, bool window)
{
    g_board_eui = eui;
    g_window_boot = window;
    g_now = 1000;
    g_join_requests = 0;
    g_reset_runs = 0;
    pb_ble_ctl_status_t st;
    memset(&st, 0, sizeof(st));
    w_get_status(&st, NULL);
    pb_ble_adm_init(&g_adm, &k_adm_ops, eui, true, g_now);
    g_adm.ctl = st;
    if (local_reset) {
        pb_ble_lr_snapshot_t snap;
        w_get_local_reset(&snap, NULL);
        pb_ble_adm_set_local_reset(&g_adm, true, window, g_now);
        g_adm.lr_snap = snap;
        g_adm.lr_refresh = false;
    }
}

static pb_ble_close_reason_t ble_poll(void)
{
    return pb_ble_adm_worker_poll(&g_adm, &k_worker, g_now);
}

/* A Security 2 session was established (connect + two good handshake messages). */
static void ble_session(void)
{
    pb_ble_adm_session_changed(&g_adm);
    for (int i = 0; i < 2; i++) {
        if (pb_ble_adm_sec_request_begin(&g_adm)) {
            pb_ble_adm_sec_request_end(&g_adm, true);
        }
    }
}

static uint8_t ble_challenge(uint8_t challenge[16])
{
    uint8_t req[PB_BLE_STATUS_REQ_LEN] = {PB_BLE_PROTO_VERSION, PB_BLE_MSG_STATUS_REQ, PB_BLE_STATUS_REQ_F_ISSUE_CHALLENGE, 0};
    uint8_t resp[PB_BLE_STATUS_RESP_LEN];
    pb_ble_adm_handle_status(&g_adm, req, sizeof(req), resp, g_now);
    memcpy(challenge, &resp[24], 16);
    return resp[2];
}

static size_t build_reset(uint8_t out[PB_BLE_LOCAL_RESET_REQ_LEN], const uint8_t target[8], const uint8_t op[16],
                          const uint8_t challenge[16])
{
    memset(out, 0, PB_BLE_LOCAL_RESET_REQ_LEN);
    out[0] = PB_BLE_PROTO_VERSION;
    out[1] = PB_BLE_MSG_LOCAL_RESET_REQ;
    out[2] = PB_BLE_LOCAL_RESET_VERSION;
    memcpy(&out[4], target, 8);
    memcpy(&out[12], op, 16);
    memcpy(&out[28], challenge, 16);
    out[44] = PB_BLE_LOCAL_RESET_CONFIRM_ERASE;
    return PB_BLE_LOCAL_RESET_REQ_LEN;
}

/* Sends raw bytes to pb-start; returns the code, and the response type. */
static uint8_t send_start_raw(const uint8_t *bytes, size_t len, uint8_t *resp_type)
{
    uint8_t buf[PB_BLE_START_MGMT_REQ_MAX_LEN + 16];
    uint8_t resp[PB_BLE_SHORT_RESP_LEN];
    memcpy(buf, bytes, len);
    size_t n = pb_ble_adm_handle_start(&g_adm, buf, len, resp, g_now);
    if (n != PB_BLE_SHORT_RESP_LEN || resp[0] != PB_BLE_PROTO_VERSION || resp[3] != 0) {
        return 0xff;
    }
    if (resp_type != NULL) {
        *resp_type = resp[1];
    }
    return resp[2];
}

static uint8_t send_reset(const uint8_t target[8], const uint8_t op[16], const uint8_t challenge[16])
{
    uint8_t req[PB_BLE_LOCAL_RESET_REQ_LEN];
    uint8_t type = 0;
    uint8_t code = send_start_raw(req, build_reset(req, target, op, challenge), &type);
    return type == PB_BLE_MSG_LOCAL_RESET_RESP ? code : 0xfe;
}

/* A new operation with a fresh challenge of the current session. */
static uint8_t reset_new(const uint8_t op[16])
{
    uint8_t challenge[16];
    if (ble_challenge(challenge) != PB_BLE_CODE_OK) {
        return 0xfd;
    }
    return send_reset(g_board_eui, op, challenge);
}

typedef struct {
    uint8_t code, condition, flags, phase, reason;
    uint16_t seconds_left;
    uint8_t raw[PB_BLE_LOCAL_RESET_STATUS_RESP_LEN];
} lr_status_t;

static lr_status_t reset_status(const uint8_t target[8], const uint8_t *op)
{
    uint8_t req[PB_BLE_LOCAL_RESET_STATUS_REQ_LEN] = {PB_BLE_PROTO_VERSION, PB_BLE_MSG_LOCAL_RESET_STATUS_REQ, 0, 0};
    memcpy(&req[4], target, 8);
    if (op != NULL) {
        memcpy(&req[12], op, 16);
    }
    lr_status_t st;
    memset(&st, 0, sizeof(st));
    pb_ble_adm_handle_local_reset_status(&g_adm, req, sizeof(req), st.raw, g_now);
    st.code = st.raw[2];
    st.condition = st.raw[13];
    st.flags = st.raw[14];
    st.phase = st.raw[15];
    st.reason = st.raw[16];
    st.seconds_left = (uint16_t)(((uint16_t)st.raw[18] << 8) | st.raw[19]);
    return st;
}

static void make_op(uint8_t op[16], uint8_t seed)
{
    seq(op, 16, seed);
}

/* ======================= 1. the button ======================= */

static pb_reenroll_button_t g_btn;
static uint32_t g_btn_now;

/* Samples every `step` ms for `ms` ms; returns how many requests it reported. */
static int btn_run(bool pressed, uint32_t ms, uint32_t step)
{
    int requests = 0;
    for (uint32_t t = 0; t < ms; t += step) {
        g_btn_now += step;
        if (pb_reenroll_button_step(&g_btn, pressed, g_btn_now)) {
            requests++;
        }
    }
    return requests;
}

static void btn_boot(uint32_t start_ms, bool pressed_at_boot)
{
    pb_reenroll_button_init(&g_btn);
    g_btn_now = start_ms;
    (void)pb_reenroll_button_step(&g_btn, pressed_at_boot, g_btn_now);
}

static void scenario_button(void)
{
    printf("\n== the button: only one long press and its release ==\n");
    const uint32_t P = PB_REENROLL_BTN_POLL_MS;

    btn_boot(1000, false);
    CHECK(btn_run(false, 1000, P) == 0, "idle: nothing");
    CHECK(g_btn.state == PB_REENROLL_BTN_ARMED, "a stable release after boot arms the button");
    CHECK(btn_run(true, 5200, P) == 0, "held 5.2 s: nothing happens while it is still down");
    CHECK(g_btn.state == PB_REENROLL_BTN_HELD, "the press qualified and waits for the release");
    CHECK(btn_run(false, 200, P) == 0, "released for 200 ms: not stable yet");
    CHECK(btn_run(false, 400, P) == 1, "the stable release reports the request, once");
    CHECK(btn_run(false, 5000, P) == 0, "and nothing more afterwards");
    CHECK(g_btn.requests == 1, "exactly one request");

    btn_boot(1000, false);
    btn_run(false, 1000, P);
    CHECK(btn_run(true, 300, P) == 0 && btn_run(false, 2000, P) == 0, "a short press (300 ms): nothing");
    CHECK(btn_run(true, 4000, P) == 0 && btn_run(false, 2000, P) == 0, "a 4 s press: nothing");
    CHECK(g_btn.state == PB_REENROLL_BTN_ARMED && g_btn.requests == 0, "the button is armed again, no request");

    /* The boundary, sampled every millisecond: the press is counted from its
     * first sample to the last sample that still saw the button down. */
    btn_boot(50000, false);
    btn_run(false, 1000, 1);
    CHECK(btn_run(true, PB_REENROLL_BTN_HOLD_MS, 1) == 0 && btn_run(false, 2000, 1) == 0,
          "a press seen down for 4999 ms: nothing");
    CHECK(g_btn.requests == 0, "no request below the hold time");
    btn_boot(50000, false);
    btn_run(false, 1000, 1);
    CHECK(btn_run(true, PB_REENROLL_BTN_HOLD_MS + 1, 1) == 0, "a press seen down for 5000 ms: still nothing while down");
    CHECK(btn_run(false, 2000, 1) == 1, "and one request after its stable release");

    /* Bounce. */
    btn_boot(1000, false);
    btn_run(false, 1000, P);
    int requests = 0;
    for (int i = 0; i < 200; i++) { /* 8 s of 20 ms down / 20 ms up: never stable */
        requests += btn_run(true, 20, 20);
        requests += btn_run(false, 20, 20);
    }
    requests += btn_run(false, 2000, P);
    CHECK(requests == 0 && g_btn.requests == 0, "contact chatter for 8 s never becomes a press");
    btn_boot(1000, false);
    btn_run(false, 1000, P);
    requests = btn_run(true, 2000, P);
    requests += btn_run(false, 20, 20); /* one 20 ms bounce in the middle of the press */
    requests += btn_run(true, 3400, P);
    CHECK(requests == 0 && g_btn.state == PB_REENROLL_BTN_HELD, "a 20 ms bounce inside a long press does not end it");
    requests = btn_run(false, 60, 20); /* released ... */
    requests += btn_run(true, 20, 20); /* ... a bounce on the way up ... */
    requests += btn_run(false, 1000, P);
    CHECK(requests == 1, "a bounce during the release does not lose or double the request");
    btn_boot(1000, false);
    btn_run(false, 1000, P);
    btn_run(true, 5200, P);
    btn_run(false, 100, P);
    requests = btn_run(true, 400, P); /* pressed again before the release settled */
    requests += btn_run(false, 2000, P);
    CHECK(requests == 0, "pressed again before the release was stable: no request");

    /* Down from boot on. */
    btn_boot(1000, true);
    CHECK(btn_run(true, 20000, P) == 0, "held from boot for 20 s: nothing");
    CHECK(btn_run(false, 2000, P) == 0, "and its release reports nothing either");
    CHECK(g_btn.state == PB_REENROLL_BTN_ARMED, "only now is the button armed");
    CHECK(btn_run(true, 5200, P) == 0 && btn_run(false, 1000, P) == 1, "a real long press afterwards counts");

    /* Down from boot for a time that would qualify as a long press. */
    btn_boot(1000, true);
    CHECK(btn_run(true, 7000, P) == 0 && btn_run(false, 2000, P) == 0,
          "held from boot for 7 s and released: nothing (it never counted as a press)");
    CHECK(g_btn.requests == 0 && g_btn.state == PB_REENROLL_BTN_ARMED, "that release only arms the button");
    /* Released at boot, but pressed before the release was stable. */
    btn_boot(1000, false);
    btn_run(false, 100, P);
    CHECK(btn_run(true, 7000, P) == 0 && btn_run(false, 2000, P) == 0,
          "pressed 100 ms after boot, before a stable release was seen: nothing");
    CHECK(g_btn.requests == 0, "no request without the stable release first");

    /* Held and never released; held too long. */
    btn_boot(1000, false);
    btn_run(false, 1000, P);
    CHECK(btn_run(true, PB_REENROLL_BTN_MAX_HOLD_MS + 60000, P) == 0, "held for over a minute: nothing while down");
    CHECK(btn_run(false, 3000, P) == 0, "a press longer than the maximum is not a request when it ends");
    CHECK(g_btn.requests == 0, "no request from a stuck button");

    /* Repeated events. */
    btn_boot(1000, false);
    btn_run(false, 1000, P);
    requests = 0;
    for (int i = 0; i < 3; i++) {
        requests += btn_run(true, 5200, P);
        requests += btn_run(false, 1000, P);
    }
    CHECK(requests == 3 && g_btn.requests == 3, "three separate long presses are three separate requests");
    btn_boot(1000, false);
    btn_run(false, 1000, P);
    btn_run(true, 5200, P);
    /* The first released sample is 20 ms in; the release is stable 300 ms
     * after that sample. */
    requests = btn_run(false, 300, P);
    CHECK(requests == 0, "one sample before the release is stable: nothing yet");
    g_btn_now += P;
    for (int i = 0; i < 10; i++) { /* the same instant sampled again and again */
        requests += pb_reenroll_button_step(&g_btn, false, g_btn_now) ? 1 : 0;
    }
    CHECK(requests == 1, "the sample at which the release became stable reports once, however often it is repeated");

    /* The millisecond counter wraps in the middle of the press. */
    btn_boot(0xFFFFFFFFu - 3000u, false);
    btn_run(false, 1000, P);
    CHECK(g_btn.state == PB_REENROLL_BTN_ARMED, "armed shortly before the counter wraps");
    CHECK(btn_run(true, 5200, P) == 0 && g_btn_now < 10000u, "the counter wrapped during the press");
    CHECK(btn_run(false, 1000, P) == 1, "the press across the wrap is one request");
    btn_boot(0xFFFFFFFFu - 100u, false);
    btn_run(false, 1000, P);
    CHECK(btn_run(true, 4000, P) == 0 && btn_run(false, 1000, P) == 0, "a short press across the wrap is still short");

    /* Sampling stopped for a while (the task was starved). */
    btn_boot(1000, false);
    btn_run(false, 1000, P);
    btn_run(true, 2000, P);
    g_btn_now += 4000; /* nobody looked for 4 s */
    requests = pb_reenroll_button_step(&g_btn, true, g_btn_now) ? 1 : 0;
    requests += btn_run(true, 2000, P);
    requests += btn_run(false, 1000, P);
    CHECK(requests == 0, "a press that was not sampled continuously does not count");
}

static void scenario_button_gpio(void)
{
    printf("\n== the button GPIO: never a pin the board already uses ==\n");
    /* The Power Base wiring of docs/power-base.md "POGO 연결": TX 10, RX 11,
     * DETECT_OUT 3, DETECT_IN 23, no power-switching pin. */
    const pb_reenroll_board_pins_t pins = {
        .uart_tx_gpio = 10, .uart_rx_gpio = 11, .detect_in_gpio = 23, .detect_out_gpio = 3, .power_enable_gpio = -1};
    CHECK(pb_reenroll_button_gpio_check(9, &pins) == PB_REENROLL_GPIO_OK, "GPIO9 (BOOT) is free on that wiring");
    CHECK(pb_reenroll_button_gpio_check(10, &pins) == PB_REENROLL_GPIO_CONFLICT_UART_TX, "the POGO UART TX pin is refused");
    CHECK(pb_reenroll_button_gpio_check(11, &pins) == PB_REENROLL_GPIO_CONFLICT_UART_RX, "the POGO UART RX pin is refused");
    CHECK(pb_reenroll_button_gpio_check(23, &pins) == PB_REENROLL_GPIO_CONFLICT_DETECT_IN, "DETECT_IN is refused");
    CHECK(pb_reenroll_button_gpio_check(3, &pins) == PB_REENROLL_GPIO_CONFLICT_DETECT_OUT, "DETECT_OUT is refused");
    pb_reenroll_board_pins_t with_power = pins;
    with_power.power_enable_gpio = 12;
    CHECK(pb_reenroll_button_gpio_check(12, &with_power) == PB_REENROLL_GPIO_CONFLICT_POWER_ENABLE,
          "the power enable pin is refused");
    CHECK(pb_reenroll_button_gpio_check(12, &pins) == PB_REENROLL_GPIO_OK, "the same pin is fine when no power pin is configured");
    pb_reenroll_board_pins_t on_boot_pin = pins;
    on_boot_pin.detect_in_gpio = 9;
    CHECK(pb_reenroll_button_gpio_check(9, &on_boot_pin) == PB_REENROLL_GPIO_CONFLICT_DETECT_IN,
          "GPIO9 itself is refused when the board's cfg uses it");
    CHECK(pb_reenroll_button_gpio_check(-1, &pins) == PB_REENROLL_GPIO_OUT_OF_RANGE, "no pin is not a button");
    CHECK(pb_reenroll_button_gpio_check(28, &pins) == PB_REENROLL_GPIO_OUT_OF_RANGE, "GPIO28 does not exist");
    static const int reserved[] = {6, 7, 8, 15, 16, 17, 18, 19, 20, 21, 25, 26, 27};
    bool all_reserved = true;
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        all_reserved = all_reserved && pb_reenroll_button_gpio_check(reserved[i], &pins) == PB_REENROLL_GPIO_RESERVED;
    }
    CHECK(all_reserved, "flash, USB, not-bonded pins and the other strapping pins (8, 25) are refused");
}

static void scenario_gate(void)
{
    printf("\n== the gate: a long press starts nothing beside something else ==\n");
    pb_reenroll_gate_input_t in = {.feature_enabled = true};
    CHECK(pb_reenroll_gate(&in) == PB_REENROLL_GATE_OK, "an idle board with the feature: allowed");
    in = (pb_reenroll_gate_input_t){.feature_enabled = false};
    CHECK(pb_reenroll_gate(&in) == PB_REENROLL_GATE_DISABLED, "a board without the feature: never");
    in = (pb_reenroll_gate_input_t){.feature_enabled = true, .enrollment_busy = true};
    CHECK(pb_reenroll_gate(&in) == PB_REENROLL_GATE_ENROLLMENT_BUSY, "a Joiner run or attach under way: refused");
    in = (pb_reenroll_gate_input_t){.feature_enabled = true, .remote_unenroll_active = true};
    CHECK(pb_reenroll_gate(&in) == PB_REENROLL_GATE_REMOTE_UNENROLL, "a remote unenroll being carried out: refused");
    in = (pb_reenroll_gate_input_t){.feature_enabled = true, .local_reset_active = true};
    CHECK(pb_reenroll_gate(&in) == PB_REENROLL_GATE_LOCAL_RESET, "a local reset under way: refused");
    in = (pb_reenroll_gate_input_t){.feature_enabled = true, .already_requested = true};
    CHECK(pb_reenroll_gate(&in) == PB_REENROLL_GATE_ALREADY_REQUESTED, "a second request before the restart: refused");
    in = (pb_reenroll_gate_input_t){.feature_enabled = true, .enrollment_busy = true, .remote_unenroll_active = true};
    CHECK(pb_reenroll_gate(&in) == PB_REENROLL_GATE_REMOTE_UNENROLL, "the remote unenroll is named first");
}

/* ======================= 2. the two records ======================= */

static void scenario_store(void)
{
    printf("\n== the records: stored means read back ==\n");
    nvs_sim_reset();
    pb_reenroll_journal_t j;
    CHECK(pb_reenroll_window_load(EUI_A) == PB_REENROLL_REC_ABSENT, "no window request on a fresh board");
    CHECK(pb_reenroll_journal_load(EUI_A, &j) == PB_REENROLL_REC_ABSENT, "no journal on a fresh board");
    CHECK(pb_reenroll_window_consume() == ESP_OK && nvs_sim_commit_count() == 0,
          "consuming nothing is fine and writes nothing");
    CHECK(nvs_sim_peek_blob("nvs", PB_REENROLL_NVS_NAMESPACE, PB_REENROLL_NVS_KEY_WINDOW, NULL, 0) < 0,
          "reading never creates anything");

    CHECK(pb_reenroll_window_store(EUI_A) == ESP_OK, "a window request is stored");
    uint8_t raw[64];
    CHECK(nvs_sim_peek_blob("nvs", PB_REENROLL_NVS_NAMESPACE, PB_REENROLL_NVS_KEY_WINDOW, raw, sizeof(raw)) ==
              PB_REENROLL_WINDOW_LEN,
          "it is 24 bytes");
    CHECK(memcmp(raw, "PBW1", 4) == 0 && raw[4] == 1 && memcmp(&raw[8], EUI_A, 8) == 0, "magic, version 1, this EUI-64");
    CHECK(pb_reenroll_window_load(EUI_A) == PB_REENROLL_REC_VALID, "and reads back valid");
    CHECK(pb_reenroll_window_load(EUI_B) == PB_REENROLL_REC_FOREIGN, "for another board it is foreign");
    bool every_flip_caught = true;
    for (int i = 0; i < PB_REENROLL_WINDOW_LEN; i++) {
        uint8_t bad[PB_REENROLL_WINDOW_LEN];
        memcpy(bad, raw, sizeof(bad));
        bad[i] ^= 0x04;
        nvs_sim_put_blob("nvs", PB_REENROLL_NVS_NAMESPACE, PB_REENROLL_NVS_KEY_WINDOW, bad, sizeof(bad));
        every_flip_caught = every_flip_caught && pb_reenroll_window_load(EUI_A) != PB_REENROLL_REC_VALID;
    }
    CHECK(every_flip_caught, "a changed bit anywhere in it is caught");
    nvs_sim_put_blob("nvs", PB_REENROLL_NVS_NAMESPACE, PB_REENROLL_NVS_KEY_WINDOW, raw, PB_REENROLL_WINDOW_LEN - 1);
    CHECK(pb_reenroll_window_load(EUI_A) == PB_REENROLL_REC_CORRUPT, "another size is corrupt");
    CHECK(pb_reenroll_window_consume() == ESP_OK && pb_reenroll_window_load(EUI_A) == PB_REENROLL_REC_ABSENT,
          "consume removes whatever is there");

    make_op(j.op_id, 0x51);
    j.stage = PB_REENROLL_STAGE_APPROVED;
    CHECK(pb_reenroll_journal_store(EUI_A, &j) == ESP_OK, "a journal is stored");
    CHECK(nvs_sim_peek_blob("nvs", PB_REENROLL_NVS_NAMESPACE, PB_REENROLL_NVS_KEY_JOURNAL, raw, sizeof(raw)) ==
              PB_REENROLL_JOURNAL_LEN,
          "it is 40 bytes");
    CHECK(memcmp(raw, "PBR1", 4) == 0 && raw[4] == PB_REENROLL_FORMAT_VERSION && raw[5] == 1 &&
              memcmp(&raw[8], EUI_A, 8) == 0 && memcmp(&raw[16], j.op_id, 16) == 0,
          "magic, format version, stage, target EUI-64 and operation id are in it");
    pb_reenroll_journal_t back;
    CHECK(pb_reenroll_journal_load(EUI_A, &back) == PB_REENROLL_REC_VALID && back.stage == PB_REENROLL_STAGE_APPROVED &&
              memcmp(back.op_id, j.op_id, 16) == 0,
          "and reads back as written");
    CHECK(pb_reenroll_journal_load(EUI_B, &back) == PB_REENROLL_REC_FOREIGN, "another board's journal is foreign");
    every_flip_caught = true;
    for (int i = 0; i < PB_REENROLL_JOURNAL_LEN; i++) {
        uint8_t bad[PB_REENROLL_JOURNAL_LEN];
        memcpy(bad, raw, sizeof(bad));
        bad[i] ^= 0x10;
        nvs_sim_put_blob("nvs", PB_REENROLL_NVS_NAMESPACE, PB_REENROLL_NVS_KEY_JOURNAL, bad, sizeof(bad));
        every_flip_caught = every_flip_caught && pb_reenroll_journal_load(EUI_A, &back) != PB_REENROLL_REC_VALID;
    }
    CHECK(every_flip_caught, "a changed bit anywhere in the journal is caught (integrity check)");
    nvs_sim_put_blob("nvs", PB_REENROLL_NVS_NAMESPACE, PB_REENROLL_NVS_KEY_JOURNAL, raw, PB_REENROLL_JOURNAL_LEN);
    nvs_sim_inject(NVS_SIM_OP_GET_BLOB, PB_REENROLL_NVS_KEY_JOURNAL, 1, ESP_FAIL);
    CHECK(pb_reenroll_journal_load(EUI_A, &back) == PB_REENROLL_REC_READ_ERROR, "a failing read is a read error, not 'absent'");

    pb_reenroll_journal_t bad_j = j;
    bad_j.stage = (pb_reenroll_stage_t)7;
    CHECK(pb_reenroll_journal_store(EUI_A, &bad_j) == ESP_ERR_INVALID_ARG, "an unknown stage is never written");
    memset(bad_j.op_id, 0, sizeof(bad_j.op_id));
    bad_j.stage = PB_REENROLL_STAGE_APPROVED;
    CHECK(pb_reenroll_journal_store(EUI_A, &bad_j) == ESP_ERR_INVALID_ARG, "an all-zero operation id is never written");

    /* A store that fails in each way is reported as a failure. */
    nvs_sim_reset();
    nvs_sim_inject(NVS_SIM_OP_SET_BLOB, PB_REENROLL_NVS_KEY_JOURNAL, 1, ESP_FAIL);
    CHECK(pb_reenroll_journal_store(EUI_A, &j) != ESP_OK, "journal: a failing write is a failure");
    nvs_sim_reset();
    nvs_sim_inject(NVS_SIM_OP_COMMIT, NULL, 1, ESP_FAIL);
    CHECK(pb_reenroll_journal_store(EUI_A, &j) != ESP_OK, "journal: a failing commit is a failure");
    nvs_sim_reset();
    nvs_sim_inject(NVS_SIM_OP_SET_BLOB, PB_REENROLL_NVS_KEY_JOURNAL, 1, ESP_OK);
    CHECK(pb_reenroll_journal_store(EUI_A, &j) != ESP_OK, "journal: a write that claims success and wrote nothing is caught by the read-back");
    nvs_sim_reset();
    nvs_sim_inject(NVS_SIM_OP_GET_BLOB, PB_REENROLL_NVS_KEY_JOURNAL, 1, ESP_FAIL);
    CHECK(pb_reenroll_journal_store(EUI_A, &j) != ESP_OK, "journal: a read-back that fails is a failure");
    nvs_sim_reset();
    nvs_sim_inject(NVS_SIM_OP_SET_BLOB, PB_REENROLL_NVS_KEY_WINDOW, 1, ESP_OK);
    CHECK(pb_reenroll_window_store(EUI_A) != ESP_OK, "window request: a write that did not happen is caught");
    nvs_sim_reset();
    pb_reenroll_window_store(EUI_A);
    nvs_sim_inject(NVS_SIM_OP_ERASE_KEY, PB_REENROLL_NVS_KEY_WINDOW, 1, ESP_OK);
    CHECK(pb_reenroll_window_consume() != ESP_OK, "window request: an erase that did not happen is caught");
    CHECK(pb_reenroll_window_consume() == ESP_OK, "and a later consume that works is fine");
}

/* ======================= 3. the window changes nothing ======================= */

static void scenario_window_changes_nothing(void)
{
    printf("\n== opening, expiring, cancelling: the stored enrollment is untouched ==\n");
    net_image_t before, after;
    cfg_image_t cfg_before, cfg_after;

    world_enrolled_v3(EUI_A);
    fakes_reset();
    net_read(&before);
    cfg_read(&cfg_before);
    CHECK(sim_boot(EUI_A, true) == SIM_RESUMED && g_thread_starts == 1, "an ordinary boot resumes the enrollment");
    CHECK(g_ctl.has_bridge_endpoint && memcmp(&g_ctl.bridge_endpoint, &EP_OLD, sizeof(EP_OLD)) == 0 && g_ctl.has_mgmt,
          "with its endpoint and management credential");

    /* The button: only the request record is written. */
    CHECK(pb_reenroll_window_store(EUI_A) == ESP_OK, "the long press stores the window request");
    net_read(&after);
    CHECK(net_same(&before, &after), "the request changed neither the record nor the Dataset");

    CHECK(sim_boot(EUI_A, true) == SIM_WINDOW, "the next boot is a window boot");
    CHECK(g_boot.window_consumed && pb_reenroll_window_load(EUI_A) == PB_REENROLL_REC_ABSENT,
          "the request was used up before the window opened");
    CHECK(g_thread_starts == 0, "the enrollment is NOT resumed in the window boot (Thread is never enabled)");
    CHECK(!pb_enroll_ctl_services_allowed(&g_ctl), "so no service may start in it");
    CHECK(g_quiesce_calls == 0 && g_erase_calls == 0, "and nothing was stopped or erased by booting");
    net_read(&after);
    CHECK(net_same(&before, &after), "opening the window changed nothing");

    /* The window runs out with nobody there. */
    ble_start(EUI_A, true, true);
    CHECK(ble_poll() == PB_BLE_OPEN, "the window is open");
    g_now += PB_BLE_REENROLL_WINDOW_MS - 1;
    CHECK(ble_poll() == PB_BLE_OPEN, "open until the last millisecond of 120 s");
    g_now += 1;
    CHECK(ble_poll() == PB_BLE_CLOSE_WINDOW_EXPIRED, "closed at 120 s");
    net_read(&after);
    CHECK(net_same(&before, &after), "an expired window changed nothing");
    CHECK(g_reset_runs == 0 && g_join_requests == 0, "the worker ran nothing");

    /* The restore: an ordinary boot. */
    CHECK(sim_boot(EUI_A, true) == SIM_RESUMED && g_thread_starts == 1,
          "the restart after the window resumes the enrollment by the usual boot rule");
    CHECK(!g_boot.window_consumed, "no request was left to open a second window");
    CHECK(memcmp(&g_ctl.bridge_endpoint, &EP_OLD, sizeof(EP_OLD)) == 0, "the same hub address as before");
    CHECK(sim_boot(EUI_A, true) == SIM_RESUMED, "and so does every boot after it");

    /* Sessions and authentication do not extend the window. */
    pb_reenroll_window_store(EUI_A);
    sim_boot(EUI_A, true);
    ble_start(EUI_A, true, true);
    uint8_t challenge[16];
    for (int i = 0; i < 11; i++) {
        g_now += 10000;
        ble_session();
        (void)ble_challenge(challenge);
        (void)ble_poll();
    }
    lr_status_t st = reset_status(EUI_A, NULL);
    CHECK(st.code == PB_BLE_CODE_OK && (st.flags & PB_BLE_LR_F_WINDOW_OPEN) != 0 && st.seconds_left == 10,
          "after 110 s of connecting and authenticating: 10 s left, not more");
    g_now += 10000;
    CHECK(ble_poll() == PB_BLE_CLOSE_WINDOW_EXPIRED, "the window closes at 120 s whatever connected meanwhile");
    net_read(&after);
    CHECK(net_same(&before, &after), "authenticating alone (no confirmed reset) changed nothing");

    /* Wrong setup code, again and again. */
    pb_reenroll_window_store(EUI_A);
    sim_boot(EUI_A, true);
    ble_start(EUI_A, true, true);
    for (unsigned i = 0; i < PB_BLE_MAX_AUTH_FAILURES; i++) {
        pb_ble_adm_session_changed(&g_adm);
        if (pb_ble_adm_sec_request_begin(&g_adm)) {
            pb_ble_adm_sec_request_end(&g_adm, false);
        }
    }
    CHECK(ble_poll() == PB_BLE_CLOSE_AUTH_LOCKED, "five failed authentications close the window");
    net_read(&after);
    CHECK(net_same(&before, &after), "wrong setup codes changed nothing");
    CHECK(sim_boot(EUI_A, true) == SIM_RESUMED, "and the board resumes its enrollment");

    /* Disconnect in the middle, before any command. */
    pb_reenroll_window_store(EUI_A);
    sim_boot(EUI_A, true);
    ble_start(EUI_A, true, true);
    ble_session();
    (void)ble_challenge(challenge);
    pb_ble_adm_session_changed(&g_adm); /* the link dropped */
    CHECK(ble_poll() == PB_BLE_OPEN, "a disconnect neither closes the window nor starts anything");
    net_read(&after);
    CHECK(net_same(&before, &after), "a disconnect changed nothing");

    cfg_read(&cfg_after);
    CHECK(memcmp(&cfg_before, &cfg_after, sizeof(cfg_before)) == 0, "cfg and the other namespaces are byte for byte what they were");

    /* A build (or board) without the button drops the request. */
    world_enrolled_v3(EUI_A);
    pb_reenroll_window_store(EUI_A);
    CHECK(sim_boot(EUI_A, false) == SIM_RESUMED, "without the feature a request opens no window: ordinary boot");
    CHECK(pb_reenroll_window_load(EUI_A) == PB_REENROLL_REC_ABSENT, "and the request is gone");

    /* A request for another board, or a damaged one. */
    world_enrolled_v3(EUI_A);
    pb_reenroll_window_store(EUI_B);
    CHECK(sim_boot(EUI_A, true) == SIM_RESUMED && pb_reenroll_window_load(EUI_A) == PB_REENROLL_REC_ABSENT,
          "another board's request opens nothing and is erased");
    world_enrolled_v3(EUI_A);
    uint8_t junk[PB_REENROLL_WINDOW_LEN];
    memset(junk, 0x5a, sizeof(junk));
    nvs_sim_put_blob("nvs", PB_REENROLL_NVS_NAMESPACE, PB_REENROLL_NVS_KEY_WINDOW, junk, sizeof(junk));
    CHECK(sim_boot(EUI_A, true) == SIM_RESUMED, "a damaged request opens nothing");

    /* The request cannot be erased: no window, and no loop. */
    world_enrolled_v3(EUI_A);
    pb_reenroll_window_store(EUI_A);
    net_read(&before);
    nvs_sim_inject(NVS_SIM_OP_ERASE_KEY, PB_REENROLL_NVS_KEY_WINDOW, 1, ESP_FAIL);
    CHECK(sim_boot(EUI_A, true) == SIM_RESUMED && !g_boot.window_consumed,
          "a request that cannot be used up opens no window (ordinary boot)");
    net_read(&after);
    CHECK(net_same(&before, &after), "and nothing was changed");

    /* A board without the feature that never used it: nothing is read into
     * existence. */
    world_enrolled_v3(EUI_A);
    int commits = nvs_sim_commit_count();
    CHECK(sim_boot(EUI_A, false) == SIM_RESUMED && nvs_sim_commit_count() == commits,
          "a board that never used the feature boots without a single write");
    CHECK(nvs_sim_peek_blob("nvs", PB_REENROLL_NVS_NAMESPACE, PB_REENROLL_NVS_KEY_JOURNAL, NULL, 0) < 0,
          "and has no re-enrollment record at all");
}

/* A window boot of the enrolled board, with a session and the worker ready. */
static void window_boot_ready(void)
{
    world_enrolled_v3(EUI_A);
    fakes_reset();
    pb_reenroll_window_store(EUI_A);
    if (sim_boot(EUI_A, true) != SIM_WINDOW) {
        printf("FAIL: window_boot_ready did not get a window boot\n");
        g_failures++;
    }
    ble_start(EUI_A, true, true);
    ble_session();
}

/* ======================= 4. what a reset command must be ======================= */

static void scenario_command_checks(void)
{
    printf("\n== the reset command: every binding is checked, and a refusal changes nothing ==\n");
    net_image_t before, after;
    uint8_t op[16], op2[16], challenge[16], req[PB_BLE_LOCAL_RESET_REQ_LEN + 8];
    uint8_t type = 0;
    make_op(op, 0x61);
    make_op(op2, 0x71);

    window_boot_ready();
    net_read(&before);
    lr_status_t st = reset_status(EUI_A, NULL);
    CHECK(st.code == PB_BLE_CODE_OK && st.condition == PB_BLE_LR_COND_ENROLLED && st.flags == PB_BLE_LR_F_WINDOW_OPEN &&
              st.seconds_left == 120 && st.phase == 0,
          "status: enrolled, window open, 120 s left, no operation");
    CHECK(memcmp(&st.raw[4], EUI_A, 8) == 0 && st.raw[12] == PB_BLE_LOCAL_RESET_VERSION, "status names this board");
    bool reserved_zero = st.raw[3] == 0 && st.raw[17] == 0;
    for (int i = 20; i < PB_BLE_LOCAL_RESET_STATUS_RESP_LEN; i++) {
        reserved_zero = reserved_zero && st.raw[i] == 0;
    }
    CHECK(reserved_zero, "status: reserved bytes are zero");

    /* Shape. */
    ble_challenge(challenge);
    size_t n = build_reset(req, EUI_A, op, challenge);
    CHECK(send_start_raw(req, n - 1, &type) == PB_BLE_CODE_MALFORMED && type == PB_BLE_MSG_LOCAL_RESET_RESP, "one byte short: MALFORMED");
    req[n] = 0;
    CHECK(send_start_raw(req, n + 1, &type) == PB_BLE_CODE_MALFORMED, "one byte long: MALFORMED");
    build_reset(req, EUI_A, op, challenge);
    req[2] = 2;
    CHECK(send_start_raw(req, n, &type) == PB_BLE_CODE_MALFORMED, "another local reset format: MALFORMED");
    build_reset(req, EUI_A, op, challenge);
    req[3] = 1;
    CHECK(send_start_raw(req, n, &type) == PB_BLE_CODE_MALFORMED, "reserved byte [3] set: MALFORMED");
    for (int off = 45; off < 48; off++) {
        build_reset(req, EUI_A, op, challenge);
        req[off] = 1;
        CHECK(send_start_raw(req, n, &type) == PB_BLE_CODE_MALFORMED, "a reserved byte after the confirmation set: MALFORMED");
    }
    build_reset(req, EUI_A, op, challenge);
    req[44] = 0;
    CHECK(send_start_raw(req, n, &type) == PB_BLE_CODE_MALFORMED, "no confirmation byte: not a reset request");
    build_reset(req, EUI_A, op, challenge);
    req[44] = 0x03;
    CHECK(send_start_raw(req, n, &type) == PB_BLE_CODE_MALFORMED, "another confirmation value: MALFORMED");
    uint8_t zero_op[16] = {0};
    build_reset(req, EUI_A, zero_op, challenge);
    CHECK(send_start_raw(req, n, &type) == PB_BLE_CODE_MALFORMED, "an all-zero operation id: MALFORMED");
    build_reset(req, EUI_A, op, challenge);
    req[0] = 2;
    CHECK(send_start_raw(req, n, &type) == PB_BLE_CODE_UNSUPPORTED_VERSION && type == PB_BLE_MSG_START_RESP,
          "another protocol version: UNSUPPORTED_VERSION, answered as pb-start");
    CHECK(g_adm.ch_valid, "none of those used up the challenge");
    CHECK(send_reset(EUI_B, op, challenge) == PB_BLE_CODE_WRONG_TARGET, "another board's EUI-64: WRONG_TARGET");
    CHECK(g_adm.q_count == 0 && g_adm.lr_phase == PB_BLE_LR_REC_NONE, "nothing was queued by any of them");

    /* Challenge. */
    uint8_t wrong[16];
    memcpy(wrong, challenge, 16);
    wrong[5] ^= 1;
    CHECK(send_reset(EUI_A, op, wrong) == PB_BLE_CODE_CHALLENGE_INVALID, "a wrong challenge: CHALLENGE_INVALID");
    CHECK(send_reset(EUI_A, op, challenge) == PB_BLE_CODE_CHALLENGE_INVALID, "and the real one is spent by that attempt");
    ble_challenge(challenge);
    g_now += PB_BLE_CHALLENGE_TTL_MS;
    CHECK(send_reset(EUI_A, op, challenge) == PB_BLE_CODE_CHALLENGE_EXPIRED, "a challenge older than its lifetime: CHALLENGE_EXPIRED");
    ble_challenge(challenge);
    ble_session(); /* a new session: the old one's challenge is not its */
    CHECK(send_reset(EUI_A, op, challenge) == PB_BLE_CODE_CHALLENGE_INVALID, "a challenge of an earlier session: CHALLENGE_INVALID");
    uint8_t none[16] = {0};
    CHECK(send_reset(EUI_A, op, none) == PB_BLE_CODE_CHALLENGE_INVALID, "no challenge issued in this session: CHALLENGE_INVALID");
    ble_poll();
    net_read(&after);
    CHECK(net_same(&before, &after) && g_reset_runs == 0, "no refused command changed anything or reached the worker");
    pb_reenroll_journal_t j;
    CHECK(pb_reenroll_journal_load(EUI_A, &j) == PB_REENROLL_REC_ABSENT, "and no journal exists");

    /* A start is never taken in a window boot. */
    ble_challenge(challenge);
    uint8_t start[PB_BLE_START_REQ_MAX_LEN];
    memset(start, 0, sizeof(start));
    start[0] = PB_BLE_PROTO_VERSION;
    start[1] = PB_BLE_MSG_START_REQ;
    start[2] = (uint8_t)strlen(PSKD);
    memcpy(&start[4], EUI_A, 8);
    make_op(&start[12], 0x33);
    memcpy(&start[28], challenge, 16);
    memcpy(&start[44], PSKD, strlen(PSKD));
    CHECK(send_start_raw(start, PB_BLE_START_REQ_FIXED_LEN + strlen(PSKD), &type) == PB_BLE_CODE_NOT_WAITING &&
              type == PB_BLE_MSG_START_RESP,
          "pb-start in a window boot: NOT_WAITING (no join before the reset is verified and the board restarted)");
    CHECK(g_adm.ch_valid && g_adm.q_count == 0, "it used up no challenge and queued nothing");
    ble_poll();
    CHECK(g_join_requests == 0, "no Joiner request reached the controller");

    /* The window ran out. */
    window_boot_ready();
    ble_challenge(challenge);
    g_now += PB_BLE_REENROLL_WINDOW_MS; /* not polled: the worker has not closed BLE yet */
    ble_challenge(challenge);
    CHECK(send_reset(EUI_A, op, challenge) == PB_BLE_CODE_WINDOW_CLOSED, "a command after 120 s: WINDOW_CLOSED");
    CHECK(g_adm.ch_valid, "which used up no challenge");
    st = reset_status(EUI_A, NULL);
    CHECK((st.flags & PB_BLE_LR_F_WINDOW_OPEN) == 0 && st.seconds_left == 0, "status then says the window is not open");
    CHECK(ble_poll() == PB_BLE_CLOSE_WINDOW_EXPIRED, "and the worker closes BLE");
    ble_challenge(challenge);
    CHECK(send_reset(EUI_A, op, challenge) == PB_BLE_CODE_CLOSED, "a command after BLE closed: CLOSED");

    /* An ordinary boot of a board with the button: no window was opened. */
    world_enrolled_v3(EUI_A);
    sim_boot(EUI_A, true);
    net_read(&before);
    ble_start(EUI_A, true, false);
    ble_session();
    st = reset_status(EUI_A, NULL);
    CHECK(st.code == PB_BLE_CODE_OK && (st.flags & PB_BLE_LR_F_WINDOW_OPEN) == 0, "no button, no window: status says so");
    CHECK(reset_new(op) == PB_BLE_CODE_WINDOW_CLOSED, "an authenticated, confirmed command without the button: WINDOW_CLOSED");
    ble_poll();
    net_read(&after);
    CHECK(net_same(&before, &after) && g_reset_runs == 0, "authentication without the physical window erased nothing");

    /* A board without the capability: both messages are unknown types. */
    world_enrolled_v3(EUI_A);
    pb_reenroll_window_store(EUI_A);
    sim_boot(EUI_A, true);
    net_read(&before);
    ble_start(EUI_A, false, false);
    ble_session();
    ble_challenge(challenge);
    n = build_reset(req, EUI_A, op, challenge);
    CHECK(send_start_raw(req, n, &type) == PB_BLE_CODE_MALFORMED && type == PB_BLE_MSG_START_RESP,
          "firmware without local_reset_v1 answers the command like any unknown pb-start type");
    uint8_t sreq[PB_BLE_LOCAL_RESET_STATUS_REQ_LEN] = {PB_BLE_PROTO_VERSION, PB_BLE_MSG_LOCAL_RESET_STATUS_REQ, 0, 0};
    memcpy(&sreq[4], EUI_A, 8);
    CHECK(!pb_ble_adm_is_local_reset_status_request(&g_adm, sreq, sizeof(sreq)),
          "and does not take the status message as its own");
    uint8_t sresp[PB_BLE_STATUS_RESP_LEN];
    CHECK(pb_ble_adm_handle_status(&g_adm, sreq, sizeof(sreq), sresp, g_now) == PB_BLE_STATUS_RESP_LEN &&
              sresp[1] == PB_BLE_MSG_STATUS_RESP && sresp[2] == PB_BLE_CODE_MALFORMED,
          "pb-status answers it with its 40-byte MALFORMED, as for any unknown type");
    ble_poll();
    net_read(&after);
    CHECK(net_same(&before, &after) && g_reset_runs == 0, "nothing reached the worker and nothing changed");

    /* The status message's own checks. */
    window_boot_ready();
    uint8_t out[PB_BLE_LOCAL_RESET_STATUS_RESP_LEN];
    CHECK(pb_ble_adm_is_local_reset_status_request(&g_adm, sreq, sizeof(sreq)), "a board with the capability takes the status message");
    pb_ble_adm_handle_local_reset_status(&g_adm, sreq, sizeof(sreq) - 1, out, g_now);
    CHECK(out[1] == PB_BLE_MSG_LOCAL_RESET_STATUS_RESP && out[2] == PB_BLE_CODE_MALFORMED, "status one byte short: MALFORMED");
    uint8_t bad_status[PB_BLE_LOCAL_RESET_STATUS_REQ_LEN];
    memcpy(bad_status, sreq, sizeof(sreq));
    bad_status[2] = 1;
    pb_ble_adm_handle_local_reset_status(&g_adm, bad_status, sizeof(bad_status), out, g_now);
    CHECK(out[2] == PB_BLE_CODE_MALFORMED, "status with a reserved byte set: MALFORMED");
    st = reset_status(EUI_B, NULL);
    CHECK(st.code == PB_BLE_CODE_WRONG_TARGET && st.condition == 0 && st.flags == 0 && memcmp(&st.raw[4], EUI_A, 8) == 0,
          "status for another board: WRONG_TARGET, and nothing about this one but its EUI-64");
}

/* ======================= 5. the reset, start to end ======================= */

static void scenario_reset_happy_path(void)
{
    printf("\n== a confirmed reset: accepted, in progress, verified -- three different things ==\n");
    uint8_t op[16], other[16], challenge[16];
    make_op(op, 0x81);
    make_op(other, 0x91);
    cfg_image_t cfg_before, cfg_after;

    window_boot_ready();
    cfg_read(&cfg_before);
    ble_challenge(challenge);
    CHECK(send_reset(EUI_A, op, challenge) == PB_BLE_CODE_ACCEPTED, "the command is ACCEPTED");
    pb_reenroll_journal_t j;
    CHECK(pb_reenroll_journal_load(EUI_A, &j) == PB_REENROLL_REC_ABSENT && !ot_empty() && g_reset_runs == 0,
          "accepted is not erased: the BLE callback wrote nothing and erased nothing");
    lr_status_t st = reset_status(EUI_A, op);
    CHECK((st.flags & PB_BLE_LR_F_OP_MATCH) != 0 && st.phase == PB_BLE_LR_PHASE_QUEUED &&
              st.condition == PB_BLE_LR_COND_RESET_PENDING,
          "status: queued");
    CHECK(send_reset(EUI_A, op, challenge) == PB_BLE_CODE_DUPLICATE, "the same request again (a retransmission): DUPLICATE");
    CHECK(reset_new(other) == PB_BLE_CODE_BUSY, "another operation beside it: BUSY");
    CHECK(g_adm.q_count == 1, "one command is queued, not two or three");
    st = reset_status(EUI_A, other);
    CHECK((st.flags & PB_BLE_LR_F_OP_MATCH) == 0 && st.phase == 0, "status knows nothing of the other operation id");

    /* The window runs out while the command waits for the worker: it is
     * carried out all the same. */
    g_now += PB_BLE_REENROLL_WINDOW_MS + 5000;
    CHECK(ble_poll() == PB_BLE_OPEN, "the worker runs the accepted reset although the window's time is over");
    CHECK(g_reset_runs == 1 && g_quiesce_calls == 1 && g_erase_calls == 1, "exactly one reset ran: quiesce, then the erase");
    CHECK(pb_reenroll_journal_load(EUI_A, &j) == PB_REENROLL_REC_VALID && j.stage == PB_REENROLL_STAGE_VERIFIED &&
              memcmp(j.op_id, op, 16) == 0,
          "the journal says VERIFIED for this operation");
    pb_enroll_record_t rec;
    CHECK(pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_ABSENT, "the enrollment record is gone (marker, hub address, management key)");
    CHECK(ot_empty(), "OpenThread's store is empty");
    st = reset_status(EUI_A, op);
    CHECK((st.flags & PB_BLE_LR_F_OP_MATCH) != 0 && st.phase == PB_BLE_LR_PHASE_VERIFIED &&
              st.condition == PB_BLE_LR_COND_NOT_ENROLLED && st.reason == 0,
          "status: verified, and the board is not enrolled");
    CHECK(reset_new(op) == PB_BLE_CODE_DUPLICATE, "the same operation asked again: DUPLICATE, nothing new");
    CHECK(reset_new(other) == PB_BLE_CODE_BUSY, "another operation before the restart: BUSY");
    ble_poll();
    CHECK(g_reset_runs == 1, "no second reset was run");

    uint8_t receipt[PB_MGMT_RECEIPT_LEN];
    CHECK(!receipt_load(EUI_A, receipt), "a local reset makes no release receipt (it is not a remote unenroll)");
    cfg_read(&cfg_after);
    CHECK(memcmp(&cfg_before, &cfg_after, sizeof(cfg_before)) == 0,
          "cfg (wiring, identity, the setup credential) and the other namespaces are untouched");

    /* The grace period, then the restart. */
    g_now += PB_BLE_LR_RESULT_GRACE_MS - 1;
    CHECK(ble_poll() == PB_BLE_OPEN, "BLE stays up for the grace period so the result can be read");
    g_now += 1;
    CHECK(ble_poll() == PB_BLE_CLOSE_LOCAL_RESET_DONE, "then it closes: the board restarts");

    /* After the restart: an unenrolled board, and the result is still there. */
    CHECK(sim_boot(EUI_A, true) == SIM_WAITING && g_thread_starts == 0,
          "the next boot is the ordinary safe wait: Thread is never enabled on the old Dataset");
    CHECK(!pb_enroll_ctl_services_allowed(&g_ctl), "no service (exporter, downstream provisioning) may start before a new enrollment");
    CHECK(!g_ctl.has_bridge_endpoint && !g_ctl.has_mgmt, "no cached endpoint and no management credential is restored");
    pb_mgmt_ops_t mops;
    memset(&mops, 0, sizeof(mops));
    pb_mgmt_client_t mgmt;
    pb_mgmt_client_init(&mgmt, &mops, EUI_A);
    CHECK(pb_mgmt_client_load(&mgmt) == PB_MGMT_BOOT_NOTHING, "the management poll has nothing to start from");
    ble_start(EUI_A, true, false);
    ble_session();
    st = reset_status(EUI_A, op);
    CHECK(st.code == PB_BLE_CODE_OK && (st.flags & PB_BLE_LR_F_OP_MATCH) != 0 && st.phase == PB_BLE_LR_PHASE_VERIFIED &&
              st.condition == PB_BLE_LR_COND_NOT_ENROLLED && (st.flags & PB_BLE_LR_F_WINDOW_OPEN) == 0,
          "after the restart a client that lost the answer still reads: this operation, verified, not enrolled");
    st = reset_status(EUI_A, other);
    CHECK((st.flags & PB_BLE_LR_F_OP_MATCH) == 0, "and an operation that never ran is not known");
    CHECK(reset_new(op) == PB_BLE_CODE_DUPLICATE, "the same operation sent again after the restart: DUPLICATE (nothing to do)");
    CHECK(reset_new(other) == PB_BLE_CODE_WINDOW_CLOSED, "a new operation needs the button again");
    ble_poll();
    CHECK(g_reset_runs == 0, "nothing was run in this boot");

    /* A new enrollment, with another hub. */
    printf("\n== after the reset: a new enrollment replaces destination and credential ==\n");
    char pskd[PB_ENROLL_PSKD_BUF_LEN] = {0};
    strcpy(pskd, PSKD);
    pb_enroll_mgmt_credential_t new_cred = credential(0x90);
    CHECK(pb_enroll_ctl_request_join_managed(&g_ctl, pskd, &EP_NEW, &new_cred, g_now) == ESP_OK, "a Joiner request is accepted now");
    ot_store(false, &NET_NEW); /* what a successful Joiner stores */
    pb_enroll_ctl_on_joiner_done(&g_ctl, g_last_attempt, PB_ENROLL_JOINER_OK, g_now);
    CHECK(!pb_enroll_ctl_services_allowed(&g_ctl), "Joiner success alone is not enrollment: services stay off");
    ot_store(true, &NET_NEW); /* attached: the complete Dataset arrived */
    pb_enroll_ctl_tick(&g_ctl, true, g_now + 1000);
    CHECK(pb_enroll_ctl_services_allowed(&g_ctl), "attached with the complete Dataset and the record stored: enrolled");
    CHECK(pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_VALID && rec.has_mgmt &&
              memcmp(&rec.bridge_endpoint, &EP_NEW, sizeof(EP_NEW)) == 0 &&
              memcmp(&rec.mgmt, &new_cred, sizeof(new_cred)) == 0 && memcmp(&rec.id, &NET_NEW, sizeof(NET_NEW)) == 0,
          "the record names the NEW network, hub address and management credential");
    pb_enroll_mgmt_credential_t old_cred = credential(0x10);
    uint8_t raw[PB_ENROLL_RECORD_MAX_LEN];
    int raw_len = nvs_sim_peek_blob("nvs", PB_ENROLL_NVS_NAMESPACE, PB_ENROLL_NVS_KEY, raw, sizeof(raw));
    bool old_key_present = false;
    for (int i = 0; i + (int)sizeof(old_cred.key) <= raw_len; i++) {
        old_key_present = old_key_present || memcmp(&raw[i], old_cred.key, sizeof(old_cred.key)) == 0;
    }
    CHECK(!old_key_present, "nothing of the old management key is in it");
    CHECK(sim_boot(EUI_A, true) == SIM_RESUMED && memcmp(&g_ctl.bridge_endpoint, &EP_NEW, sizeof(EP_NEW)) == 0,
          "the next boot resumes the NEW enrollment, towards the new hub");
    /* (That the new hub then reports heartbeat_received is the hub's own
     * judgement and is tested with the hub and the app; nothing here claims it.) */

    /* The old operation id is spent once the board is enrolled again. */
    pb_reenroll_window_store(EUI_A);
    CHECK(sim_boot(EUI_A, true) == SIM_WINDOW, "the button opens a window for the new enrollment too");
    ble_start(EUI_A, true, true);
    ble_session();
    st = reset_status(EUI_A, op);
    CHECK(st.phase == PB_BLE_LR_PHASE_VERIFIED && st.condition == PB_BLE_LR_COND_ENROLLED,
          "status still says that old operation was verified -- and that the board is enrolled now");
    CHECK(reset_new(op) == PB_BLE_CODE_REQUEST_CONFLICT, "the old operation id cannot stand for a new reset: REQUEST_CONFLICT");
    ble_poll();
    CHECK(g_reset_runs == 0 && pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_VALID, "and nothing was erased");
    CHECK(reset_new(other) == PB_BLE_CODE_ACCEPTED, "a new operation id, confirmed, is accepted");
    ble_poll();
    CHECK(pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_ABSENT && ot_empty(), "and resets the new enrollment");
}

/* ======================= 6. the approval journal ======================= */

static void scenario_journal_failures(void)
{
    printf("\n== the approval cannot be journaled: the reset is not started ==\n");
    static const struct {
        nvs_sim_op_t op;
        esp_err_t err;
        const char *what;
    } faults[] = {
        {NVS_SIM_OP_SET_BLOB, ESP_FAIL, "the write fails"},
        {NVS_SIM_OP_COMMIT, ESP_FAIL, "the commit fails"},
        {NVS_SIM_OP_SET_BLOB, ESP_OK, "the write claims success and stores nothing (read-back)"},
        {NVS_SIM_OP_GET_BLOB, ESP_FAIL, "the read-back fails"},
    };
    uint8_t op[16];
    make_op(op, 0xa1);
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        printf("-- %s\n", faults[i].what);
        net_image_t before, after;
        window_boot_ready();
        net_read(&before);
        CHECK(reset_new(op) == PB_BLE_CODE_ACCEPTED, "the command is accepted (queued)");
        /* The snapshot read at service start already happened; the next
         * journal access is the worker's store. */
        if (faults[i].op == NVS_SIM_OP_GET_BLOB) {
            /* 1st and 2nd: pb_reenroll_snapshot()'s journal load (size, none);
             * the read-back is the first read after the write. Absent journal:
             * the snapshot's size query is the 1st get, the read-back's size
             * query the 2nd. */
            nvs_sim_inject(faults[i].op, PB_REENROLL_NVS_KEY_JOURNAL, 2, faults[i].err);
        } else if (faults[i].op == NVS_SIM_OP_COMMIT) {
            nvs_sim_inject(faults[i].op, NULL, 1, faults[i].err);
        } else {
            nvs_sim_inject(faults[i].op, PB_REENROLL_NVS_KEY_JOURNAL, 1, faults[i].err);
        }
        ble_poll();
        CHECK(g_reset_runs == 1 && g_quiesce_calls == 0 && g_erase_calls == 0, "the worker ran it: no step after the journal was taken");
        net_read(&after);
        CHECK(net_same(&before, &after), "the record and the Dataset are byte for byte what they were");
        lr_status_t st = reset_status(EUI_A, op);
        CHECK((st.flags & PB_BLE_LR_F_OP_MATCH) != 0 && st.phase == PB_BLE_LR_PHASE_NOT_STARTED &&
                  st.reason == PB_BLE_LR_REASON_JOURNAL_STORE_FAILED && st.condition == PB_BLE_LR_COND_ENROLLED,
              "status: not started (journal store failed), still enrolled -- not 'verified', not 'in progress'");
        pb_reenroll_journal_t j;
        CHECK(pb_reenroll_journal_load(EUI_A, &j) == PB_REENROLL_REC_ABSENT, "no approved journal was left behind");
        CHECK(reset_new(op) == PB_BLE_CODE_REQUEST_CONFLICT, "that operation id is over: REQUEST_CONFLICT, never silently restarted");
        g_now += PB_BLE_REENROLL_WINDOW_MS;
        CHECK(ble_poll() == PB_BLE_CLOSE_WINDOW_EXPIRED, "the window then simply runs out");
        CHECK(sim_boot(EUI_A, true) == SIM_RESUMED, "and the next boot resumes the enrollment: nothing was reset");
    }
}

/* ======================= 7. a step fails ======================= */

static void scenario_step_failures(void)
{
    printf("\n== a step of the reset fails: never resumed, carried on at the next boot ==\n");
    uint8_t op[16];
    make_op(op, 0xb1);
    pb_reenroll_journal_t j;
    pb_reenroll_reason_t why;

    typedef enum { F_RECORD_ERASE_ERROR, F_RECORD_ERASE_DROPPED, F_NET_ERASE_ERROR, F_NET_ERASE_LIES, F_NET_ERASE_PARTIAL, F_JOURNAL_FINAL } fault_t;
    static const struct {
        fault_t fault;
        pb_reenroll_reason_t reason;
        const char *what;
    } cases[] = {
        {F_RECORD_ERASE_ERROR, PB_REENROLL_REASON_RECORD_CLEAR_FAILED, "the enrollment record cannot be erased"},
        {F_RECORD_ERASE_DROPPED, PB_REENROLL_REASON_RECORD_STILL_PRESENT, "the record's erase claims success and did nothing"},
        {F_NET_ERASE_ERROR, PB_REENROLL_REASON_NETWORK_ERASE_FAILED, "erasing OpenThread's store fails"},
        {F_NET_ERASE_LIES, PB_REENROLL_REASON_NETWORK_NOT_EMPTY, "the store erase returns no error and erased nothing"},
        {F_NET_ERASE_PARTIAL, PB_REENROLL_REASON_NETWORK_NOT_EMPTY, "the store erase leaves an entry behind"},
        {F_JOURNAL_FINAL, PB_REENROLL_REASON_JOURNAL_FINAL_FAILED, "the journal cannot be closed"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        printf("-- %s\n", cases[i].what);
        window_boot_ready();
        CHECK(reset_new(op) == PB_BLE_CODE_ACCEPTED, "the command is accepted");
        switch (cases[i].fault) {
        case F_RECORD_ERASE_ERROR: nvs_sim_inject(NVS_SIM_OP_ERASE_KEY, PB_ENROLL_NVS_KEY, 1, ESP_FAIL); break;
        case F_RECORD_ERASE_DROPPED: nvs_sim_inject(NVS_SIM_OP_ERASE_KEY, PB_ENROLL_NVS_KEY, 1, ESP_OK); break;
        case F_NET_ERASE_ERROR: g_erase_fails = true; break;
        case F_NET_ERASE_LIES: g_erase_lies = true; break;
        case F_NET_ERASE_PARTIAL: g_erase_partial = true; break;
        case F_JOURNAL_FINAL: nvs_sim_inject(NVS_SIM_OP_SET_BLOB, PB_REENROLL_NVS_KEY_JOURNAL, 2, ESP_FAIL); break;
        }
        ble_poll();
        lr_status_t st = reset_status(EUI_A, op);
        CHECK((st.flags & PB_BLE_LR_F_OP_MATCH) != 0 && st.phase == PB_BLE_LR_PHASE_IN_PROGRESS &&
                  st.reason == (uint8_t)cases[i].reason && st.condition == PB_BLE_LR_COND_RESET_PENDING,
              "status: in progress with the reason -- never 'verified'");
        CHECK(pb_reenroll_journal_load(EUI_A, &j) == PB_REENROLL_REC_VALID && j.stage == PB_REENROLL_STAGE_APPROVED,
              "the journal still says APPROVED");
        g_now += PB_BLE_LR_RESULT_GRACE_MS;
        CHECK(ble_poll() == PB_BLE_CLOSE_LOCAL_RESET_DONE, "the board restarts");

        /* The fault is still there at the next boot. */
        switch (cases[i].fault) {
        case F_RECORD_ERASE_ERROR: nvs_sim_inject(NVS_SIM_OP_ERASE_KEY, PB_ENROLL_NVS_KEY, 1, ESP_FAIL); break;
        case F_RECORD_ERASE_DROPPED: nvs_sim_inject(NVS_SIM_OP_ERASE_KEY, PB_ENROLL_NVS_KEY, 1, ESP_OK); break;
        case F_JOURNAL_FINAL: nvs_sim_inject(NVS_SIM_OP_SET_BLOB, PB_REENROLL_NVS_KEY_JOURNAL, 1, ESP_FAIL); break;
        default: break;
        }
        CHECK(sim_boot(EUI_A, true) == SIM_STOPPED && g_thread_starts == 0,
              "next boot, fault still there: stopped -- the old network is NOT resumed");
        CHECK(g_boot.reset_result == PB_REENROLL_RESET_INCOMPLETE && g_boot.reason == cases[i].reason &&
                  g_boot.condition == PB_REENROLL_COND_RESET_PENDING,
              "the boot says why, and that the reset is still pending");
        ble_start(EUI_A, true, false);
        ble_session();
        CHECK(reset_status(EUI_A, op).phase == PB_BLE_LR_PHASE_IN_PROGRESS, "the operation still reads 'in progress'");

        /* A build without the button finishes it too. */
        fakes_reset();
        CHECK(sim_boot(EUI_A, false) == SIM_RESTART, "a later boot, fault gone: the reset is finished (also by a build without the button)");
        CHECK(pb_reenroll_journal_load(EUI_A, &j) == PB_REENROLL_REC_VALID && j.stage == PB_REENROLL_STAGE_VERIFIED &&
                  memcmp(j.op_id, op, 16) == 0,
              "the same operation is now VERIFIED");
        pb_enroll_record_t rec;
        CHECK(pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_ABSENT && ot_empty(), "record and store are empty");
        CHECK(sim_boot(EUI_A, true) == SIM_WAITING && g_thread_starts == 0, "and the boot after it is the safe wait of an unenrolled board");
    }

    /* The same operation sent again while its journal is pending carries on;
     * another one does not replace it. */
    printf("-- the pending operation and another one\n");
    window_boot_ready();
    g_erase_fails = true;
    CHECK(pb_reenroll_reset_begin(EUI_A, op, &k_reset_ops, &why) == PB_REENROLL_RESET_INCOMPLETE, "a reset whose erase fails is incomplete");
    uint8_t other[16];
    make_op(other, 0xc1);
    CHECK(pb_reenroll_reset_begin(EUI_A, other, &k_reset_ops, &why) == PB_REENROLL_RESET_NOT_STARTED &&
              why == PB_REENROLL_REASON_OTHER_RESET_PENDING,
          "another operation is refused while one is journaled");
    CHECK(pb_reenroll_journal_load(EUI_A, &j) == PB_REENROLL_REC_VALID && memcmp(j.op_id, op, 16) == 0,
          "the journal still names the first operation");
    g_erase_fails = false;
    CHECK(pb_reenroll_reset_begin(EUI_A, op, &k_reset_ops, &why) == PB_REENROLL_RESET_VERIFIED,
          "the same operation again carries on and finishes -- one journal, one operation");
}

/* ======================= 8. power loss at every write ======================= */

static const char *g_flash_path = "/tmp/pb_reenroll_flash.bin";

/* Runs `body` as one boot in a child over the flash file; `cut_at_commit` > 0
 * ends it right before that commit. Returns the child's exit code. */
static int in_child(int cut_at_commit, int (*body)(void))
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, 1);
        }
        if (!nvs_sim_set_backing_file(g_flash_path)) {
            _exit(90);
        }
        nvs_sim_power_cut_at_commit(cut_at_commit);
        _exit(body());
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 99;
}

static uint8_t g_cut_op[16];

/* The window boot's worker carrying out an accepted reset. Exit code: the
 * number of commits it made (when it was not cut). */
static int body_reset(void)
{
    fakes_reset();
    pb_reenroll_reason_t why;
    (void)pb_reenroll_reset_begin(EUI_A, g_cut_op, &k_reset_ops, &why);
    return nvs_sim_commit_count();
}

/* An ordinary boot. Exit code: 10 + sim_boot_t, +40 when Thread was enabled
 * on a stored Dataset, +80 when the stored record is still an enrollment. */
static int body_boot(void)
{
    fakes_reset();
    sim_boot_t result = sim_boot(EUI_A, true);
    return 10 + (int)result + (g_thread_starts > 0 ? 40 : 0);
}

static int body_state(void)
{
    pb_reenroll_journal_t j;
    pb_enroll_record_t rec;
    pb_reenroll_rec_status_t js = pb_reenroll_journal_load(EUI_A, &j);
    int code = 0;
    if (js == PB_REENROLL_REC_VALID) {
        code |= j.stage == PB_REENROLL_STAGE_APPROVED ? 1 : 2;
    }
    if (pb_enroll_record_load(EUI_A, &rec) != PB_ENROLL_MARKER_ABSENT) {
        code |= 4;
    }
    if (!ot_empty()) {
        code |= 8;
    }
    return code;
}

static int body_make_world(void)
{
    world_enrolled_v3(EUI_A);
    /* world_enrolled_v3() reset the simulator: point it at the file again
     * and write the image out. */
    net_image_t img;
    net_read(&img);
    nvs_sim_reset();
    if (!nvs_sim_set_backing_file(g_flash_path)) {
        return 1;
    }
    cfg_store();
    nvs_sim_put_blob("nvs", PB_ENROLL_NVS_NAMESPACE, PB_ENROLL_NVS_KEY, img.rec, (size_t)img.rec_len);
    ot_store(true, &NET_OLD); /* commits: the file now holds everything */
    return 0;
}

static void scenario_power_loss(void)
{
    printf("\n== power loss at every commit of the reset ==\n");
    make_op(g_cut_op, 0xd1);
    unlink(g_flash_path);
    CHECK(in_child(0, body_make_world) == 0, "an enrolled board's flash image is made");
    CHECK(in_child(0, body_state) == (4 | 8), "it holds the record and the Dataset, no journal");
    int commits = in_child(0, body_reset);
    CHECK(commits >= 4, "a complete reset makes several separate commits");
    CHECK(in_child(0, body_state) == 2, "and ends with: journal VERIFIED, no record, empty store");

    bool never_resumed_after_approval = true;
    bool resumed_when_not_approved = true;
    bool always_finishes = true;
    bool never_new_join_before_verified = true;
    for (int cut = 1; cut <= commits; cut++) {
        unlink(g_flash_path);
        in_child(0, body_make_world);
        int rc = in_child(cut, body_reset);
        if (rc != NVS_SIM_POWER_CUT_EXIT) {
            printf("FAIL: commit %d was not reached (rc=%d)\n", cut, rc);
            g_failures++;
            continue;
        }
        int state = in_child(0, body_state);
        bool approved = (state & 1) != 0;
        bool verified = (state & 2) != 0;
        int boot = in_child(0, body_boot);
        bool thread_enabled = boot >= 50;
        sim_boot_t result = (sim_boot_t)((boot >= 50 ? boot - 40 : boot) - 10);
        if (!approved && !verified) {
            /* The approval never reached the flash: nothing was reset. */
            resumed_when_not_approved = resumed_when_not_approved && result == SIM_RESUMED && state == (4 | 8);
        } else {
            /* From the approval on, the old network is never enabled again. */
            never_resumed_after_approval = never_resumed_after_approval && !thread_enabled && result != SIM_RESUMED;
            if (approved) {
                /* The boot carried on. */
                always_finishes = always_finishes && result == SIM_RESTART && in_child(0, body_state) == 2;
                never_new_join_before_verified = never_new_join_before_verified && result != SIM_WAITING;
            }
        }
        /* Whatever happened, two more boots end in a stable, consistent state. */
        (void)in_child(0, body_boot);
        int last = in_child(0, body_boot);
        int final_state = in_child(0, body_state);
        bool consistent = (final_state == (4 | 8) && last == 10 + SIM_RESUMED + 40) ||
                          (final_state == 2 && last == 10 + SIM_WAITING);
        if (!consistent) {
            printf("FAIL: cut before commit %d ends in state %d / boot %d\n", cut, final_state, last);
            g_failures++;
        }
    }
    CHECK(resumed_when_not_approved, "cut before the approval was durable: the enrollment is intact and resumed");
    CHECK(never_resumed_after_approval, "cut anywhere after it: the old network is never enabled again");
    CHECK(always_finishes, "the next boot carries on with the reset and verifies it");
    CHECK(never_new_join_before_verified, "and no boot offers a new join while the reset is unfinished");
    unlink(g_flash_path);

    /* A restart in the middle of the window, before any command. */
    world_enrolled_v3(EUI_A);
    net_image_t before, after;
    net_read(&before);
    pb_reenroll_window_store(EUI_A);
    CHECK(sim_boot(EUI_A, true) == SIM_WINDOW, "window boot");
    /* ... power cycle, with nothing written since ... */
    CHECK(sim_boot(EUI_A, true) == SIM_RESUMED, "a restart during the window is an ordinary boot: the enrollment is resumed");
    CHECK(sim_boot(EUI_A, true) == SIM_RESUMED, "the request does not come back at a later boot (no reboot loop)");
    net_read(&after);
    CHECK(net_same(&before, &after), "and nothing was changed");
}

/* ======================= 9. a journal that cannot be trusted ======================= */

static void scenario_unreadable_journal(void)
{
    printf("\n== the journal is damaged, foreign or unreadable: stop, never resume ==\n");
    uint8_t op[16];
    make_op(op, 0xe1);
    pb_reenroll_journal_t j;
    net_image_t before, after;

    /* Damaged. */
    world_enrolled_v3(EUI_A);
    fakes_reset();
    uint8_t junk[PB_REENROLL_JOURNAL_LEN];
    memset(junk, 0xa5, sizeof(junk));
    nvs_sim_put_blob("nvs", PB_REENROLL_NVS_NAMESPACE, PB_REENROLL_NVS_KEY_JOURNAL, junk, sizeof(junk));
    net_read(&before);
    CHECK(sim_boot(EUI_A, true) == SIM_STOPPED && g_thread_starts == 0, "a damaged journal: stopped, the enrollment is not resumed");
    CHECK(g_boot.journal_status == PB_REENROLL_REC_CORRUPT && g_boot.condition == PB_REENROLL_COND_UNREADABLE, "the boot says the journal is corrupt");
    CHECK(g_erase_calls == 0, "and erases nothing by itself");
    net_read(&after);
    CHECK(net_same(&before, &after), "nothing was changed");
    CHECK(sim_boot(EUI_A, false) == SIM_STOPPED, "a build without the button stops the same way");

    /* For another board. */
    world_enrolled_v3(EUI_A);
    j.stage = PB_REENROLL_STAGE_APPROVED;
    memcpy(j.op_id, op, 16);
    pb_reenroll_journal_store(EUI_B, &j);
    CHECK(sim_boot(EUI_A, true) == SIM_STOPPED && g_boot.journal_status == PB_REENROLL_REC_FOREIGN && g_erase_calls == 0,
          "another board's APPROVED journal: stopped, and NOT carried out on this board");
    pb_enroll_record_t rec;
    CHECK(pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_VALID && !ot_empty(), "this board's enrollment is untouched");

    /* Cannot be read. */
    world_enrolled_v3(EUI_A);
    j.stage = PB_REENROLL_STAGE_VERIFIED;
    pb_reenroll_journal_store(EUI_A, &j);
    nvs_sim_inject(NVS_SIM_OP_GET_BLOB, PB_REENROLL_NVS_KEY_JOURNAL, 1, ESP_FAIL);
    CHECK(sim_boot(EUI_A, true) == SIM_STOPPED && g_boot.journal_status == PB_REENROLL_REC_READ_ERROR,
          "a journal that cannot be read: stopped (a read error is not 'no journal')");
    CHECK(sim_boot(EUI_A, true) == SIM_RESUMED, "when it reads again as a finished one, the boot is ordinary");

    /* The way out: the button, authentication and a confirmed reset. */
    world_enrolled_v3(EUI_A);
    fakes_reset();
    nvs_sim_put_blob("nvs", PB_REENROLL_NVS_NAMESPACE, PB_REENROLL_NVS_KEY_JOURNAL, junk, sizeof(junk));
    pb_reenroll_window_store(EUI_A);
    CHECK(sim_boot(EUI_A, true) == SIM_WINDOW && g_boot.recovery, "with the button: a window opens to replace the damaged journal");
    CHECK(g_thread_starts == 0, "still nothing is resumed");
    ble_start(EUI_A, true, true);
    ble_session();
    lr_status_t st = reset_status(EUI_A, NULL);
    CHECK(st.condition == PB_BLE_LR_COND_UNREADABLE && (st.flags & PB_BLE_LR_F_WINDOW_OPEN) != 0, "status: unreadable, window open");
    g_now += PB_BLE_REENROLL_WINDOW_MS;
    CHECK(ble_poll() == PB_BLE_CLOSE_WINDOW_EXPIRED, "the window alone repairs nothing");
    CHECK(sim_boot(EUI_A, true) == SIM_STOPPED, "and the board is stopped again afterwards");
    pb_reenroll_window_store(EUI_A);
    sim_boot(EUI_A, true);
    ble_start(EUI_A, true, true);
    ble_session();
    CHECK(reset_new(op) == PB_BLE_CODE_ACCEPTED, "a confirmed reset is accepted in that window");
    ble_poll();
    CHECK(pb_reenroll_journal_load(EUI_A, &j) == PB_REENROLL_REC_VALID && j.stage == PB_REENROLL_STAGE_VERIFIED &&
              memcmp(j.op_id, op, 16) == 0,
          "it replaced the damaged journal and was verified");
    CHECK(pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_ABSENT && ot_empty(), "record and store are empty");
    CHECK(sim_boot(EUI_A, true) == SIM_WAITING, "the board is an ordinary unenrolled board again");

    /* The same repair on a board that holds a remote unenroll's receipt: the
     * reset clears the store and closes the journal, and keeps the receipt. */
    world_enrolled_v3(EUI_A);
    fakes_reset();
    {
        pb_enroll_record_t final_rec;
        memset(&final_rec, 0, sizeof(final_rec));
        final_rec.has_mgmt = true;
        final_rec.mgmt_state = PB_ENROLL_MGMT_RELEASED;
        final_rec.release_flags = PB_ENROLL_RELEASE_F_ERASED;
        pb_enroll_mgmt_credential_t c = credential(0x10);
        memcpy(final_rec.mgmt.hub_id, c.hub_id, sizeof(c.hub_id));
        memcpy(final_rec.mgmt.generation, c.generation, sizeof(c.generation));
        seq(final_rec.command_id, sizeof(final_rec.command_id), 0xe0);
        seq(final_rec.done_mac, sizeof(final_rec.done_mac), 0x70);
        pb_enroll_record_store(EUI_A, &final_rec);
    }
    nvs_sim_put_blob("nvs", PB_REENROLL_NVS_NAMESPACE, PB_REENROLL_NVS_KEY_JOURNAL, junk, sizeof(junk));
    uint8_t receipt_before[PB_MGMT_RECEIPT_LEN], receipt_after[PB_MGMT_RECEIPT_LEN];
    CHECK(receipt_in_record(EUI_A, receipt_before), "a board with a release receipt in its record and a damaged journal");
    CHECK(!ot_empty() && !receipt_load(EUI_A, receipt_after),
          "(its store holds a network: until that is erased the receipt is not handed out)");
    pb_reenroll_reason_t repair_why;
    CHECK(pb_reenroll_reset_begin(EUI_A, op, &k_reset_ops, &repair_why) == PB_REENROLL_RESET_VERIFIED && ot_empty(),
          "the confirmed reset repairs it: store empty, journal VERIFIED");
    CHECK(receipt_load(EUI_A, receipt_after) && memcmp(receipt_before, receipt_after, sizeof(receipt_before)) == 0,
          "and the release receipt is still there, byte for byte");

    /* The replacement cannot be stored: the damaged journal stays, the board stays stopped. */
    world_enrolled_v3(EUI_A);
    fakes_reset();
    nvs_sim_put_blob("nvs", PB_REENROLL_NVS_NAMESPACE, PB_REENROLL_NVS_KEY_JOURNAL, junk, sizeof(junk));
    net_read(&before);
    pb_reenroll_reason_t why;
    nvs_sim_inject(NVS_SIM_OP_SET_BLOB, PB_REENROLL_NVS_KEY_JOURNAL, 1, ESP_FAIL);
    CHECK(pb_reenroll_reset_begin(EUI_A, op, &k_reset_ops, &why) == PB_REENROLL_RESET_NOT_STARTED &&
              why == PB_REENROLL_REASON_JOURNAL_STORE_FAILED && g_erase_calls == 0,
          "a replacement that cannot be stored starts nothing");
    net_read(&after);
    CHECK(net_same(&before, &after), "nothing was erased");
    CHECK(sim_boot(EUI_A, true) == SIM_STOPPED, "and the board is still stopped, not quietly resumed");
}

/* ======================= 10. beside a remote unenroll ======================= */

static void scenario_remote_unenroll(void)
{
    printf("\n== a remote unenroll comes first, and its receipt is not the button's to remove ==\n");
    uint8_t op[16];
    make_op(op, 0xf1);
    net_image_t before, after;
    pb_reenroll_reason_t why;

    /* Journaled (UNENROLLING). */
    world_enrolled_v3(EUI_A);
    fakes_reset();
    set_record_state(EUI_A, PB_ENROLL_MGMT_UNENROLLING, 0);
    pb_reenroll_window_store(EUI_A);
    net_read(&before);
    CHECK(sim_boot(EUI_A, true) == SIM_RELEASE_PENDING, "a journaled remote unenroll + the button: the release is finished first");
    CHECK(g_boot.action == PB_REENROLL_BOOT_NORMAL && g_boot.window_consumed && g_boot.condition == PB_REENROLL_COND_REMOTE_RELEASE_PENDING,
          "no window is opened for it, and the request is used up");
    net_read(&after);
    CHECK(net_same(&before, &after) && g_erase_calls == 0, "the local side touched nothing of it");
    CHECK(pb_reenroll_reset_begin(EUI_A, op, &k_reset_ops, &why) == PB_REENROLL_RESET_NOT_STARTED &&
              why == PB_REENROLL_REASON_REMOTE_RELEASE_PENDING,
          "a reset is refused while the release is under way");
    pb_reenroll_journal_t j;
    CHECK(pb_reenroll_journal_load(EUI_A, &j) == PB_REENROLL_REC_ABSENT, "and no journal was written");

    /* Released, erase pending. */
    world_enrolled_v3(EUI_A);
    fakes_reset();
    set_record_state(EUI_A, PB_ENROLL_MGMT_RELEASED, PB_ENROLL_RELEASE_F_ERASE_PENDING);
    pb_reenroll_window_store(EUI_A);
    net_read(&before);
    CHECK(sim_boot(EUI_A, true) == SIM_RELEASE_PENDING && g_boot.action == PB_REENROLL_BOOT_NORMAL,
          "released with the erase pending + the button: the remote release still comes first");
    net_read(&after);
    CHECK(net_same(&before, &after), "nothing of it was changed");
    /* The BLE side, had a window been open: refused. */
    ble_start(EUI_A, true, true);
    ble_session();
    CHECK(reset_status(EUI_A, NULL).condition == PB_BLE_LR_COND_REMOTE_RELEASE_PENDING, "status: remote release pending");
    CHECK(reset_new(op) == PB_BLE_CODE_BUSY, "the command is answered BUSY");
    ble_poll();
    CHECK(g_reset_runs == 0, "and never reaches the worker");

    /* A final release: the receipt. */
    world_enrolled_v3(EUI_A);
    fakes_reset();
    {
        pb_enroll_record_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.has_mgmt = true;
        rec.mgmt_state = PB_ENROLL_MGMT_RELEASED;
        rec.release_flags = PB_ENROLL_RELEASE_F_ERASED;
        pb_enroll_mgmt_credential_t c = credential(0x10);
        memcpy(rec.mgmt.hub_id, c.hub_id, sizeof(c.hub_id));
        memcpy(rec.mgmt.generation, c.generation, sizeof(c.generation));
        seq(rec.command_id, sizeof(rec.command_id), 0xe0);
        seq(rec.done_mac, sizeof(rec.done_mac), 0x70);
        CHECK(pb_enroll_record_store(EUI_A, &rec) == ESP_OK, "a final release record (with its receipt) is stored");
    }
    nvs_handle_t h;
    if (nvs_open(OT_NS, NVS_READWRITE, &h) == ESP_OK) { /* its erase was verified */
        nvs_erase_key(h, OT_KEY_DATASET);
        nvs_erase_key(h, OT_KEY_NETINFO);
        nvs_commit(h);
        nvs_close(h);
    }
    uint8_t receipt_before[PB_MGMT_RECEIPT_LEN], receipt_after[PB_MGMT_RECEIPT_LEN];
    CHECK(receipt_load(EUI_A, receipt_before), "the board hands out its release receipt");
    pb_reenroll_window_store(EUI_A);
    CHECK(sim_boot(EUI_A, true) == SIM_WAITING && g_boot.condition == PB_REENROLL_COND_NOT_ENROLLED,
          "the button on an already released board: no window, the ordinary safe wait");
    CHECK(receipt_load(EUI_A, receipt_after) && memcmp(receipt_before, receipt_after, sizeof(receipt_before)) == 0,
          "the receipt is still there, byte for byte");
    ble_start(EUI_A, true, false);
    ble_session();
    lr_status_t st = reset_status(EUI_A, NULL);
    CHECK(st.condition == PB_BLE_LR_COND_NOT_ENROLLED, "status: not enrolled -- nothing to reset, enroll the usual way");
    /* Even in an open window an already reset board is not reset again. */
    ble_start(EUI_A, true, true);
    ble_session();
    CHECK(reset_new(op) == PB_BLE_CODE_NOT_ENROLLED, "a reset command for it: NOT_ENROLLED");
    ble_poll();
    CHECK(g_reset_runs == 0 && pb_reenroll_journal_load(EUI_A, &j) == PB_REENROLL_REC_ABSENT, "nothing ran and nothing was journaled");
    CHECK(pb_reenroll_reset_begin(EUI_A, op, &k_reset_ops, &why) == PB_REENROLL_RESET_NOT_STARTED &&
              why == PB_REENROLL_REASON_NOT_ENROLLED && g_erase_calls == 0,
          "the reset itself refuses too: not started, nothing erased");
    CHECK(receipt_load(EUI_A, receipt_after) && memcmp(receipt_before, receipt_after, sizeof(receipt_before)) == 0,
          "and the receipt is untouched");

    /* A board that was never enrolled. */
    nvs_sim_reset();
    cfg_store();
    pb_reenroll_window_store(EUI_A);
    CHECK(sim_boot(EUI_A, true) == SIM_WAITING, "the button on an unenrolled board: the ordinary safe wait (BLE enrollment opens)");
    ble_start(EUI_A, true, false);
    ble_session();
    CHECK(reset_status(EUI_A, NULL).condition == PB_BLE_LR_COND_NOT_ENROLLED, "status: not enrolled");

    /* The enrollment record is damaged (never resumed anyway). */
    world_enrolled_v3(EUI_A);
    uint8_t junk[PB_ENROLL_RECORD_V3_LEN];
    memset(junk, 0x3c, sizeof(junk));
    nvs_sim_put_blob("nvs", PB_ENROLL_NVS_NAMESPACE, PB_ENROLL_NVS_KEY, junk, sizeof(junk));
    pb_reenroll_window_store(EUI_A);
    CHECK(sim_boot(EUI_A, true) == SIM_WAITING && g_thread_starts == 0,
          "a damaged enrollment record: not resumed, no window, the ordinary safe wait");

    /* A reset in progress and the button: the reset comes first. */
    world_enrolled_v3(EUI_A);
    fakes_reset();
    g_erase_fails = true;
    pb_reenroll_reset_begin(EUI_A, op, &k_reset_ops, &why);
    g_erase_fails = false;
    pb_reenroll_window_store(EUI_A);
    CHECK(sim_boot(EUI_A, true) == SIM_RESTART && g_boot.window_consumed,
          "a journaled local reset + the button: the reset is finished, the request is dropped");
    CHECK(sim_boot(EUI_A, true) == SIM_WAITING, "and the next boot opens no window");
}

int main(void)
{
    scenario_button();
    scenario_button_gpio();
    scenario_gate();
    scenario_store();
    scenario_window_changes_nothing();
    scenario_command_checks();
    scenario_reset_happy_path();
    scenario_journal_failures();
    scenario_step_failures();
    scenario_power_loss();
    scenario_unreadable_journal();
    scenario_remote_unenroll();

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (g_failures != 0) {
        printf("FAILED\n");
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
