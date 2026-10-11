/*
 * Host harness: ESP-IDF's real protocomm + Security 2 server and this repo's
 * real BLE enrollment service (pb_ble_service.c, pb_ble_admission.c), the
 * real credential parser and the real enrollment controller, with only two
 * things replaced:
 *   - the BLE transport: tools/test_power_base_ble_sec2_interop.py calls
 *     protocomm_open_session()/protocomm_req_handle()/protocomm_close_session()
 *     through the pipe protocol below, i.e. exactly what
 *     protocomm_nimble.c does on connect / characteristic write / disconnect;
 *   - OpenThread: the same fake operations the other enrollment host tests
 *     use (is a key stored, is the Dataset complete, is the Joiner running).
 * So this proves what the server does with real Security 2 messages from
 * ESP-IDF's official client. It proves nothing about a BLE link.
 *
 * Pipe protocol (file descriptors from PB_HARNESS_IN_FD / PB_HARNESS_OUT_FD;
 * stdout/stderr stay free for the firmware's own log output, which the test
 * scans for secrets):
 *   request   cmd(1) len(2, big-endian) payload
 *   response  status(1) len(2, big-endian) payload
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_log.h"
#include "nvs_sim.h"
#include "protocomm.h"
#include "power_base_ble_auth/power_base_ble_auth.h"
#include "power_base_ble_enroll/pb_ble_service.h"
#include "power_base_enrollment/power_base_enrollment.h"
#include "power_base_mgmt/pb_mgmt_client.h"
#include "power_base_reenroll/pb_reenroll.h"
#include "store_glue.h"

static const char *TAG = "harness";

#define MAX_FRAME 2048

static int g_in = -1;
static int g_out = -1;

/* ---- fake OpenThread under the real controller ---- */

typedef struct {
    bool has_key;
    bool complete;
    pb_enroll_network_id_t id;
    bool thread_enabled;
    uint32_t joiner_start_calls;
    uint32_t joiner_stop_calls;
    uint32_t last_attempt;
    char last_pskd[PB_ENROLL_PSKD_BUF_LEN];
} fake_ot_t;

static fake_ot_t g_ot;
static pb_enroll_ctl_t g_ctl;
static uint8_t g_eui64[8];
static uint32_t g_now = 1000;
static pb_ble_auth_cred_t g_cred;
static protocomm_t *g_pc;
/* The prepared image's enrollment endpoint policy for the next 'I' (boot):
 * OPTIONAL unless a 'Y' command set it, so drivers that do not know the
 * command get the board they always got. */
static pb_enroll_endpoint_policy_t g_endpoint_policy = PB_ENROLL_ENDPOINT_POLICY_OPTIONAL;
/* Local re-enrollment for the next 'I' (boot), set by a 'W' command; all
 * zero unless one was sent, so drivers that do not know the command get the
 * board they always got (no capability, no window, nothing stored). */
#define LR_F_SUPPORTED 0x01u /* the board has the re-enrollment button: capability listed */
#define LR_F_WINDOW 0x02u    /* this boot is a re-enrollment window boot */
#define LR_F_ENROLLED 0x04u  /* the board boots with a stored version 3 enrollment and its Dataset */
static uint8_t g_lr_flags;
static uint32_t g_lr_reset_runs;
static uint32_t g_lr_quiesce_calls;

static const pb_enroll_network_id_t k_net = {
    .ext_pan_id = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88},
    .mesh_local_prefix = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00},
};

static esp_err_t f_joiner_start(const char *pskd, uint32_t attempt, void *ctx)
{
    (void)ctx;
    g_ot.joiner_start_calls++;
    g_ot.last_attempt = attempt;
    memset(g_ot.last_pskd, 0, sizeof(g_ot.last_pskd));
    strncpy(g_ot.last_pskd, pskd, sizeof(g_ot.last_pskd) - 1);
    return ESP_OK;
}

static void f_joiner_stop(void *ctx)
{
    (void)ctx;
    g_ot.joiner_stop_calls++;
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

/* ---- what enrollment_ble.c supplies on the board ---- */

static esp_err_t ctl_request_join(char pskd[PB_ENROLL_PSKD_BUF_LEN], const pb_enroll_bridge_endpoint_t *endpoint,
                                  const pb_enroll_mgmt_credential_t *mgmt, void *ctx)
{
    (void)ctx;
    if (mgmt != NULL) {
        return pb_enroll_ctl_request_join_managed(&g_ctl, pskd, endpoint, mgmt, g_now);
    }
    if (endpoint != NULL) {
        return pb_enroll_ctl_request_join_with_endpoint(&g_ctl, pskd, endpoint, g_now);
    }
    return pb_enroll_ctl_request_join(&g_ctl, pskd, g_now);
}

static esp_err_t ctl_cancel(void *ctx)
{
    (void)ctx;
    return pb_enroll_ctl_cancel(&g_ctl);
}

static void ctl_get_status(pb_ble_ctl_status_t *out, void *ctx)
{
    (void)ctx;
    out->state = g_ctl.state;
    out->last = g_ctl.last;
    out->attempt = g_ctl.attempt;
}

static uint32_t harness_now_ms(void)
{
    return g_now;
}

/* ---- local reset: what enrollment_reenroll.c supplies on the board, over
 *      the fake OpenThread store above ---- */

static void lr_quiesce(void *ctx)
{
    (void)ctx;
    g_lr_quiesce_calls++;
    g_ot.thread_enabled = false;
}

static esp_err_t lr_network_erase(void *ctx)
{
    (void)ctx;
    g_ot.has_key = false;
    g_ot.complete = false;
    memset(&g_ot.id, 0, sizeof(g_ot.id));
    return ESP_OK;
}

static bool lr_network_erased(void *ctx)
{
    (void)ctx;
    return !g_ot.has_key && !g_ot.complete;
}

static const pb_reenroll_ops_t k_lr_ops = {
    .quiesce = lr_quiesce,
    .network_erase = lr_network_erase,
    .network_erased = lr_network_erased,
    .ctx = NULL,
};

static uint8_t ctl_local_reset(const uint8_t op_id[PB_BLE_OP_ID_LEN], uint8_t *reason, void *ctx)
{
    (void)ctx;
    g_lr_reset_runs++;
    pb_reenroll_reason_t why = PB_REENROLL_REASON_NONE;
    pb_reenroll_reset_result_t result = pb_reenroll_reset_begin(g_eui64, op_id, &k_lr_ops, &why);
    *reason = (uint8_t)why;
    return (uint8_t)result;
}

static void ctl_get_local_reset(pb_ble_lr_snapshot_t *out, void *ctx)
{
    (void)ctx;
    pb_reenroll_snapshot_t snap;
    pb_reenroll_snapshot(g_eui64, &snap);
    memset(out, 0, sizeof(*out));
    out->condition = (uint8_t)snap.condition;
    if (snap.journal_status == PB_REENROLL_REC_VALID) {
        out->journal_valid = true;
        out->journal_verified = snap.journal.stage == PB_REENROLL_STAGE_VERIFIED;
        memcpy(out->journal_op, snap.journal.op_id, sizeof(out->journal_op));
    }
}

/* enrollment_ble.c's ctl_get_status(): in a window boot the controller was
 * not booted; pb-status says "enrolled" while the record says so. */
static void ctl_get_status_window(pb_ble_ctl_status_t *out, void *ctx)
{
    ctl_get_status(out, ctx);
    pb_reenroll_snapshot_t snap;
    pb_reenroll_snapshot(g_eui64, &snap);
    if (snap.condition == PB_REENROLL_COND_ENROLLED) {
        out->state = PB_ENROLL_STATE_ENROLLED;
    }
}

/* ---- pipe I/O ---- */

static bool read_full(void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = read(g_in, p, n);
        if (r < 0 && errno == EINTR) {
            continue; /* the FreeRTOS POSIX port schedules with signals */
        }
        if (r <= 0) {
            return false;
        }
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static void write_full(const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n > 0) {
        ssize_t w = write(g_out, p, n);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            exit(3);
        }
        p += w;
        n -= (size_t)w;
    }
}

static void reply(uint8_t status, const uint8_t *payload, size_t len)
{
    uint8_t hdr[3] = {status, (uint8_t)(len >> 8), (uint8_t)len};
    write_full(hdr, sizeof(hdr));
    if (len > 0) {
        write_full(payload, len);
    }
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool receipt_source(uint8_t receipt[PB_BLE_RECEIPT_LEN], void *ctx)
{
    (void)ctx;
    /* enrollment_ble.c: the record AND the stores, read for every request --
     * with the real store check (store_glue.h). The stores are empty unless
     * an 'N' command put something into them, so drivers that do not know
     * the command get the board they always got. */
    return pb_mgmt_receipt_load(g_eui64, hstore_no_network, NULL, receipt);
}

static esp_err_t prestart_erase(void *ctx)
{
    return (esp_err_t)hstore_erase(ctx);
}

/* 'N': op(1) arg(1) -- the board's network stores.
 *   op 0  wipe both stores and the store component's RAM
 *   op 1  put arg into the stores with nothing looking (HSTORE_PLANT_*)
 *   op 2  faults from now on (HSTORE_FAULT_* bits in arg)
 *   op 3  a restart up to the point where the service runs, in app_main.c's
 *         order: RAM reset; pb_mgmt_released_prestart() (the real one, with
 *         the real erase and read-back); then OpenThread starts. arg bit 0:
 *         a firmware that does not erase before OpenThread starts (what the
 *         firmware did before 2026-10-08's second review) -- the prestart is
 *         left out.
 * Reply: prestart result(1, 0xff when left out), then hstore_state()'s five
 * bytes, then 1 when the store check says "no network". */
static void cmd_network_store(const uint8_t *p, size_t len)
{
    static const pb_mgmt_prestart_ops_t ops = {.erase = prestart_erase, .erased = hstore_erased, .ctx = NULL};
    if (len != 2) {
        reply(0xff, NULL, 0);
        return;
    }
    uint8_t out[7] = {0xff, 0, 0, 0, 0, 0, 0};
    switch (p[0]) {
    case 0: hstore_wipe(); break;
    case 1: hstore_plant(p[1]); break;
    case 2: hstore_set_faults(p[1]); break;
    case 3:
        hstore_restart_ram();
        if ((p[1] & 1u) == 0) {
            out[0] = (uint8_t)pb_mgmt_released_prestart(g_eui64, &ops);
        }
        (void)hstore_openthread_starts();
        break;
    default:
        reply(0xff, NULL, 0);
        return;
    }
    hstore_state(&out[1]);
    out[6] = hstore_no_network(NULL) ? 1 : 0;
    reply(0, out, sizeof(out));
}

/* 'I': eui64(8) joiner_ok(1) record(n). Status = pb_ble_auth_status_t of the
 * record; the service starts only for PB_BLE_AUTH_OK, like on the board. */
static void cmd_init(const uint8_t *p, size_t len)
{
    if (len < 9 || g_pc != NULL) {
        reply(0xff, NULL, 0);
        return;
    }
    memcpy(g_eui64, p, 8);
    bool joiner_ok = p[8] != 0;
    nvs_sim_reset();
    hstore_wipe();
    memset(&g_ot, 0, sizeof(g_ot));
    g_lr_reset_runs = 0;
    g_lr_quiesce_calls = 0;
    if ((g_lr_flags & LR_F_ENROLLED) != 0) {
        /* A board that was enrolled by an earlier boot: a version 3 record
         * (test credential only) and the complete Dataset it names. */
        pb_enroll_record_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.id = k_net;
        rec.has_bridge_endpoint = true;
        memcpy(rec.bridge_endpoint.addr, k_net.mesh_local_prefix, 8);
        rec.bridge_endpoint.addr[15] = 0x79;
        rec.bridge_endpoint.port = 5684;
        rec.has_mgmt = true;
        rec.mgmt_state = PB_ENROLL_MGMT_ENROLLED;
        memset(rec.mgmt.hub_id, 0x10, sizeof(rec.mgmt.hub_id));
        memset(rec.mgmt.generation, 0x20, sizeof(rec.mgmt.generation));
        memset(rec.mgmt.key, 0x30, sizeof(rec.mgmt.key));
        if (pb_enroll_record_store(g_eui64, &rec) != ESP_OK) {
            reply(0xfc, NULL, 0);
            return;
        }
        g_ot.has_key = true;
        g_ot.complete = true;
        g_ot.id = k_net;
    }
    pb_enroll_ctl_init(&g_ctl, &k_ot_ops, g_eui64);
    pb_enroll_ctl_set_endpoint_policy(&g_ctl, g_endpoint_policy); /* enrollment_runtime.c */
    if ((g_lr_flags & LR_F_WINDOW) == 0) {
        pb_enroll_ctl_boot(&g_ctl);
    } /* else pb_enrollment_runtime_start_reenroll_window(): the boot rule is NOT applied */

    pb_ble_auth_status_t st = pb_ble_auth_parse(&p[9], len - 9, g_eui64, &g_cred);
    if (st != PB_BLE_AUTH_OK) {
        ESP_LOGE(TAG, "credential record: %s -- BLE enrollment not offered", pb_ble_auth_status_name(st));
        reply((uint8_t)st, NULL, 0);
        return;
    }
    pb_ble_service_config_t cfg = {
        .joiner_eui64_ok = joiner_ok,
        .endpoint_policy = g_endpoint_policy, /* enrollment_ble.c */
        .cred = &g_cred,
        .ctl = {.request_join = ctl_request_join, .cancel = ctl_cancel, .get_status = ctl_get_status, .ctx = NULL},
        .now_ms = harness_now_ms,
        .receipt = receipt_source, /* enrollment_ble.c */
        .receipt_ctx = NULL,
    };
    if ((g_lr_flags & LR_F_SUPPORTED) != 0) { /* enrollment_ble.c, on a board with the button */
        cfg.local_reset = true;
        cfg.reenroll_window = (g_lr_flags & LR_F_WINDOW) != 0;
        cfg.ctl.local_reset = ctl_local_reset;
        cfg.ctl.get_local_reset = ctl_get_local_reset;
        if (cfg.reenroll_window) {
            cfg.ctl.get_status = ctl_get_status_window;
        }
    }
    memcpy(cfg.eui64, g_eui64, 8);
    esp_err_t err = pb_ble_service_start(&cfg, &g_pc);
    reply(err == ESP_OK ? 0 : 0xfe, NULL, 0);
}

/* 'F': hub_id(16) generation(16) command_id(16) mac(32) -- stores the final
 * release record a board holds after an unenroll whose erase was verified
 * (test values only), exactly as pb_mgmt_client.c writes it. The receipt the
 * service then hands out is built from it by the real pb_mgmt_receipt_load(). */
static void cmd_final_release(const uint8_t *p, size_t len)
{
    if (len != PB_ENROLL_HUB_ID_LEN + PB_ENROLL_GENERATION_LEN + PB_ENROLL_COMMAND_ID_LEN + PB_ENROLL_MAC_LEN) {
        reply(0xff, NULL, 0);
        return;
    }
    pb_enroll_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.has_mgmt = true;
    rec.mgmt_state = PB_ENROLL_MGMT_RELEASED;
    rec.release_flags = PB_ENROLL_RELEASE_F_ERASED;
    memcpy(rec.mgmt.hub_id, &p[0], PB_ENROLL_HUB_ID_LEN);
    memcpy(rec.mgmt.generation, &p[16], PB_ENROLL_GENERATION_LEN);
    memcpy(rec.command_id, &p[32], PB_ENROLL_COMMAND_ID_LEN);
    memcpy(rec.done_mac, &p[48], PB_ENROLL_MAC_LEN);
    reply(pb_enroll_record_store(g_eui64, &rec) == ESP_OK ? 0 : 0xfe, NULL, 0);
}

/* 'R': session(4) name_len(1) name data -> esp_err(4) response */
static void cmd_request(const uint8_t *p, size_t len)
{
    if (g_pc == NULL || len < 5 || len < 5u + p[4]) {
        reply(0xff, NULL, 0);
        return;
    }
    uint32_t session = get_u32(p);
    char name[64] = {0};
    size_t name_len = p[4] < sizeof(name) - 1 ? p[4] : sizeof(name) - 1;
    memcpy(name, &p[5], name_len);
    const uint8_t *data = &p[5 + p[4]];
    size_t data_len = len - 5 - p[4];

    uint8_t *out = NULL;
    ssize_t out_len = 0;
    esp_err_t err = protocomm_req_handle(g_pc, name, session, data, (ssize_t)data_len, &out, &out_len);
    static uint8_t resp[MAX_FRAME];
    put_u32(resp, (uint32_t)err);
    size_t n = 4;
    if (err == ESP_OK && out != NULL && out_len > 0 && (size_t)out_len <= sizeof(resp) - 4) {
        memcpy(&resp[4], out, (size_t)out_len);
        n += (size_t)out_len;
    }
    if (err == ESP_OK) {
        free(out);
    }
    reply(0, resp, n);
}

/* 'S': state last attempt(4) joiner_starts(4) joiner_stops(4) thread_enabled
 *      services_allowed marker_status last_pskd(33)
 *      then, about the bridge endpoint (addresses are 16 bytes, ports
 *      big-endian):
 *        pending(1) pending_addr(16) pending_port(2)   held in RAM by the controller
 *        active(1)  active_addr(16)  active_port(2)    of an ENROLLED controller
 *        stored(1)  stored_addr(16)  stored_port(2)    in the version 2 or 3 record
 *      then, about the management credential (test credentials only; this
 *      harness never sees a real one):
 *        pending(1)             held in RAM by the controller
 *        active(1)              an ENROLLED controller whose record is version 3
 *        stored(1) state(1)     the stored record is version 3, and its state
 *        hub_id(16) generation(16) key(32)   as stored */
#define STATS_MGMT_LEN (4 + PB_ENROLL_HUB_ID_LEN + PB_ENROLL_GENERATION_LEN + PB_ENROLL_MGMT_KEY_LEN)
static void put_endpoint(uint8_t *p, bool present, const pb_enroll_bridge_endpoint_t *ep)
{
    p[0] = present;
    if (present) {
        memcpy(&p[1], ep->addr, PB_ENROLL_BRIDGE_ADDR_LEN);
        p[17] = (uint8_t)(ep->port >> 8);
        p[18] = (uint8_t)ep->port;
    }
}

static void cmd_stats(void)
{
    uint8_t s[17 + PB_ENROLL_PSKD_BUF_LEN + 3 * 19 + STATS_MGMT_LEN];
    memset(s, 0, sizeof(s));
    pb_enroll_record_t rec;
    s[0] = (uint8_t)g_ctl.state;
    s[1] = (uint8_t)g_ctl.last;
    put_u32(&s[2], g_ctl.attempt);
    put_u32(&s[6], g_ot.joiner_start_calls);
    put_u32(&s[10], g_ot.joiner_stop_calls);
    s[14] = g_ot.thread_enabled;
    s[15] = pb_enroll_ctl_services_allowed(&g_ctl);
    s[16] = (uint8_t)pb_enroll_record_load(g_eui64, &rec);
    memcpy(&s[17], g_ot.last_pskd, PB_ENROLL_PSKD_BUF_LEN);
    uint8_t *e = &s[17 + PB_ENROLL_PSKD_BUF_LEN];
    put_endpoint(&e[0], g_ctl.pending_endpoint, &g_ctl.pending_ep);
    put_endpoint(&e[19], g_ctl.state == PB_ENROLL_STATE_ENROLLED && g_ctl.has_bridge_endpoint, &g_ctl.bridge_endpoint);
    put_endpoint(&e[38], rec.has_bridge_endpoint, &rec.bridge_endpoint);
    uint8_t *m = &e[3 * 19];
    m[0] = g_ctl.pending_mgmt;
    m[1] = g_ctl.state == PB_ENROLL_STATE_ENROLLED && g_ctl.has_mgmt;
    m[2] = rec.has_mgmt;
    m[3] = (uint8_t)rec.mgmt_state;
    memcpy(&m[4], rec.mgmt.hub_id, PB_ENROLL_HUB_ID_LEN);
    memcpy(&m[4 + PB_ENROLL_HUB_ID_LEN], rec.mgmt.generation, PB_ENROLL_GENERATION_LEN);
    memcpy(&m[4 + PB_ENROLL_HUB_ID_LEN + PB_ENROLL_GENERATION_LEN], rec.mgmt.key, PB_ENROLL_MGMT_KEY_LEN);
    reply(0, s, sizeof(s));
}

/* 'L': what the local reset left behind --
 *      journal_status(1) journal_stage(1) journal_op(16) condition(1)
 *      record_status(1) dataset_stored(1) reset_runs(4) quiesce_calls(4) */
static void cmd_local_reset_stats(void)
{
    uint8_t s[29];
    memset(s, 0, sizeof(s));
    pb_reenroll_snapshot_t snap;
    pb_reenroll_snapshot(g_eui64, &snap);
    s[0] = (uint8_t)snap.journal_status;
    if (snap.journal_status == PB_REENROLL_REC_VALID) {
        s[1] = (uint8_t)snap.journal.stage;
        memcpy(&s[2], snap.journal.op_id, PB_REENROLL_OP_ID_LEN);
    }
    s[18] = (uint8_t)snap.condition;
    s[19] = (uint8_t)snap.record_status;
    s[20] = g_ot.has_key || g_ot.complete;
    put_u32(&s[21], g_lr_reset_runs);
    put_u32(&s[25], g_lr_quiesce_calls);
    reply(0, s, sizeof(s));
}

void app_main(void)
{
    const char *in_fd = getenv("PB_HARNESS_IN_FD");
    const char *out_fd = getenv("PB_HARNESS_OUT_FD");
    if (in_fd == NULL || out_fd == NULL) {
        fprintf(stderr, "run through tools/test_power_base_ble_sec2_interop.py\n");
        exit(2);
    }
    g_in = atoi(in_fd);
    g_out = atoi(out_fd);

    /* security2.c posts its session events to the default loop, which
     * thread_manager_init() creates on the board. */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    static uint8_t buf[MAX_FRAME];
    while (1) {
        uint8_t hdr[3];
        if (!read_full(hdr, sizeof(hdr))) {
            exit(0);
        }
        size_t len = ((size_t)hdr[1] << 8) | hdr[2];
        if (len > sizeof(buf) || (len > 0 && !read_full(buf, len))) {
            exit(4);
        }
        switch (hdr[0]) {
        case 'I': cmd_init(buf, len); break;
        case 'O': /* connect */
            reply(g_pc != NULL && len == 4 && protocomm_open_session(g_pc, get_u32(buf)) == ESP_OK ? 0 : 1, NULL, 0);
            break;
        case 'X': /* disconnect */
            reply(g_pc != NULL && len == 4 && protocomm_close_session(g_pc, get_u32(buf)) == ESP_OK ? 0 : 1, NULL, 0);
            break;
        case 'R': cmd_request(buf, len); break;
        case 'T': /* advance the monotonic clock */
            g_now += get_u32(buf);
            reply(0, NULL, 0);
            break;
        case 'P': { /* one enrollment worker pass */
            uint8_t reason = (uint8_t)pb_ble_service_poll();
            reply(0, &reason, 1);
            break;
        }
        case 'J': /* the Joiner callback's outcome for the current attempt */
            if (buf[0] == PB_ENROLL_JOINER_OK) {
                g_ot.has_key = true; /* what a successful Joiner stores */
            }
            pb_enroll_ctl_on_joiner_done(&g_ctl, g_ot.last_attempt, (pb_enroll_joiner_outcome_t)buf[0], g_now);
            reply(0, NULL, 0);
            break;
        case 'A': /* attached(1) dataset_complete(1): one controller tick */
            if (buf[1]) {
                g_ot.complete = true;
                g_ot.id = k_net;
            }
            pb_enroll_ctl_tick(&g_ctl, buf[0] != 0, g_now);
            reply(0, NULL, 0);
            break;
        case 'S': cmd_stats(); break;
        case 'F': cmd_final_release(buf, len); break;
        case 'Y': /* policy(1): 0 optional, 1 required, 2 invalid -- before 'I' only */
            if (len != 1 || buf[0] > PB_ENROLL_ENDPOINT_POLICY_INVALID || g_pc != NULL) {
                reply(0xff, NULL, 0);
                break;
            }
            g_endpoint_policy = (pb_enroll_endpoint_policy_t)buf[0];
            reply(0, NULL, 0);
            break;
        case 'W': /* local reset flags(1), LR_F_* -- before 'I' only */
            if (len != 1 || (buf[0] & (uint8_t)~(LR_F_SUPPORTED | LR_F_WINDOW | LR_F_ENROLLED)) != 0 || g_pc != NULL) {
                reply(0xff, NULL, 0);
                break;
            }
            g_lr_flags = buf[0];
            reply(0, NULL, 0);
            break;
        case 'L': cmd_local_reset_stats(); break;
        case 'N': cmd_network_store(buf, len); break;
        case 'D': /* what enrollment_ble.c does after the transport stopped */
            pb_ble_service_stop();
            g_pc = NULL;
            reply(0, NULL, 0);
            break;
        case 'Q':
            reply(0, NULL, 0);
            fflush(stdout);
            fflush(stderr);
            exit(0);
        default: reply(0xfd, NULL, 0); break;
        }
    }
}
