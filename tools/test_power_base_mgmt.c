/*
 * Host test for the REAL board-side management code (docs/power-base-management.md):
 *   components/power_base_mgmt/pb_mgmt_proto.c    (wire format, MAC)
 *   components/power_base_mgmt/pb_mgmt_client.c   (poll, command checks, release order)
 *   components/power_base_enrollment/power_base_enrollment.c (record version 3, boot rule)
 * linked against the functional NVS simulator and a scripted hub that lives
 * in this file. The hub here speaks the same protocol through the same
 * pb_mgmt_proto.c; the real Linux hub code is exercised against this same
 * client in linux/matter_bridge/app/api/tests/test_power_base_management.cpp.
 *
 * What is modelled:
 *   - the hub: one credential, an optional pending command, the nonce rule
 *     (a command only to a poll that echoes the nonce of the previous
 *     answer), and every way an answer can be lost, refused, replayed or
 *     forged;
 *   - the network: a "stored Dataset" kept in the simulated flash, so that
 *     erasing it and rebooting behave like the real thing;
 *   - power loss: each boot runs in a forked child over a flash file
 *     (nvs_sim_set_backing_file) and may exit at a chosen point. Whatever was
 *     not committed is gone;
 *   - the receipt's delivery session (k_ops_session): the Dataset the
 *     platform keeps in RAM across the erase, a Thread session that writes
 *     nothing to the flash -- or, for the cases that must be caught, one that
 *     does, at the attach or later -- and a hub that takes the receipt on the
 *     management resource and completes the removal on the first copy.
 *     The scenarios before those use k_ops, a platform without such a
 *     session: there the receipt must never appear on the network;
 *   - the entry under "the Network Key's id": a network's key on an enrolled
 *     board, and the key OpenThread makes for itself when it starts and finds
 *     none there (KeyManager::Init()) -- which it does NOT do when one is
 *     there, so a network's key that an erase left behind stays where it is.
 *     Every boot (every in_child) goes the way app_main.c does: first the
 *     real pb_mgmt_released_prestart() with this file's store operations,
 *     then "OpenThread starts". "The store is empty" is therefore true only
 *     right after an erase, and what is asked later is whether a NETWORK is
 *     stored, with the origin of that entry taken from the boot's own
 *     history -- the two questions have two fakes here, as they have two
 *     functions in the firmware (components/power_base_ot_store, whose own
 *     rule tools/test_power_base_ot_store.c tests; this file's fake follows
 *     it).
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I tools/host_test_stubs/nvs_sim -I tools/host_test_stubs \
 *      -I components/power_base_enrollment/include -I components/power_base_mgmt/include \
 *      -o /tmp/test_power_base_mgmt tools/test_power_base_mgmt.c \
 *      components/power_base_mgmt/pb_mgmt_proto.c components/power_base_mgmt/pb_mgmt_client.c \
 *      components/power_base_enrollment/power_base_enrollment.c tools/host_test_stubs/nvs_sim/nvs_sim.c \
 *   && /tmp/test_power_base_mgmt
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
#include "power_base_enrollment/power_base_enrollment.h"
#include "power_base_mgmt/pb_mgmt_client.h"
#include "power_base_mgmt/pb_mgmt_proto.h"

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

/* Made-up identifiers; nothing here is a real board, hub, network or key. */
static const uint8_t EUI_A[8] = {0x60, 0x55, 0xf9, 0xff, 0xfe, 0x00, 0x12, 0x34};
static const uint8_t EUI_B[8] = {0x60, 0x55, 0xf9, 0xff, 0xfe, 0x00, 0x56, 0x78};
static const pb_enroll_network_id_t NET_OTBR = {
    .ext_pan_id = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88},
    .mesh_local_prefix = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00},
};
static const pb_enroll_bridge_endpoint_t EP_OTBR = {
    .addr = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00, 0x8c, 0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79},
    .port = 5684,
};

static void seq(uint8_t *out, size_t len, uint8_t first)
{
    for (size_t i = 0; i < len; i++) {
        out[i] = (uint8_t)(first + i);
    }
}

/* The golden credential of the documentation: hub id 10..1f, generation
 * 20..2f, key 30..4f. */
static pb_enroll_mgmt_credential_t golden_credential(void)
{
    pb_enroll_mgmt_credential_t c;
    seq(c.hub_id, sizeof(c.hub_id), 0x10);
    seq(c.generation, sizeof(c.generation), 0x20);
    seq(c.key, sizeof(c.key), 0x30);
    return c;
}

static bool test_hmac(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[32], void *ctx)
{
    (void)ctx;
    host_hmac_sha256(key, key_len, data, len, out);
    return true;
}

static void hex(const uint8_t *p, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        sprintf(&out[2 * i], "%02x", p[i]);
    }
}

/* ======================= the scripted hub ======================= */

typedef enum {
    MUT_NONE = 0,
    MUT_OTHER_BOARD,      /* a correctly signed answer, but for another EUI-64 */
    MUT_OTHER_HUB,        /* correctly signed, another hub id */
    MUT_OTHER_GENERATION, /* correctly signed with the current key, another generation */
    MUT_OLD_ENROLLMENT,   /* another generation, signed with that generation's own (other) key */
    MUT_WRONG_KEY,        /* everything right, signed with another key */
    MUT_FLIP_BODY_BYTE,
    MUT_FLIP_MAC_BYTE,
    MUT_STALE_NONCE,      /* the answer to an EARLIER poll, replayed */
    MUT_WRONG_TYPE,       /* a signed RESULT_ACK where IDLE/UNENROLL belongs */
    MUT_TRUNCATED,
    MUT_NO_ANSWER,
    MUT_REJECTED,
} mutation_t;

typedef struct {
    uint8_t eui64[8];
    pb_enroll_mgmt_credential_t cred;
    bool command_pending;
    uint8_t command_id[16];
    bool have_nonce;
    uint8_t nonce[16];
    uint8_t nonce_counter;
    mutation_t mutate_poll; /* applied to the answer of every POLL while set */
    bool skip_nonce_round;  /* a (forged) hub that puts the command into the first answer */
    int done_drop;          /* this many UNENROLL_DONE get no answer */
    bool done_reject;
    bool done_bad_ack;
    bool done_ack_other_result; /* a genuine acknowledgement, but of the OTHER result */
    /* what arrived */
    int polls, accepted, done;
    /* UNENROLL_DONE by what it says: "released" (a notice) or "erased" (the
     * receipt, which the board must never put on the network). */
    int done_released, done_erased;
    uint8_t saved[PB_MGMT_MAX_MESSAGE_LEN]; /* the last genuine UNENROLL it sent */
    size_t saved_len;
    /* HELLO. A hub that predates it refuses the type like any unknown one. */
    bool knows_hello;
    int hello_drop; /* this many HELLOs get no answer */
    bool hello_bad_ack;
    int hellos;
    uint32_t board_caps; /* from the last HELLO it took */
    /* The receipt (UNENROLL_DONE/ERASED) on the management resource. The
     * done_* switches above are for the "released" notice only. */
    int receipt_drop; /* this many receipts are taken but their answer is lost */
    bool receipt_reject; /* refused without being taken (a cancelled or superseded removal) */
    bool receipt_bad_ack;
    bool receipt_ack_other_result;
    /* A hub that predates re-acknowledging: it drops the key when the receipt
     * completed the removal, and refuses everything of that enrollment after. */
    bool forgets_key_when_complete;
    bool completed;                             /* a genuine receipt arrived */
    uint8_t receipt[PB_MGMT_UNENROLL_DONE_LEN]; /* the first one */
    bool receipt_differed;                      /* a later one was not the same bytes */
} hub_t;

static hub_t g_hub;
/* Called for every request the board sends, with its type, before the hub
 * answers. Tests use it to look at the stored record at that moment and to
 * "lose power". */
static void (*g_on_request)(uint8_t type);

static void hub_init(const uint8_t eui64[8], const pb_enroll_mgmt_credential_t *cred)
{
    memset(&g_hub, 0, sizeof(g_hub));
    memcpy(g_hub.eui64, eui64, 8);
    g_hub.cred = *cred;
    seq(g_hub.command_id, sizeof(g_hub.command_id), 0xe0);
    g_hub.nonce_counter = 0xd0;
}

static void hub_identity(pb_mgmt_identity_t *id)
{
    memcpy(id->eui64, g_hub.eui64, 8);
    memcpy(id->hub_id, g_hub.cred.hub_id, 16);
    memcpy(id->generation, g_hub.cred.generation, 16);
}

static pb_mgmt_xfer_t hub_answer_poll(const pb_mgmt_msg_t *poll, uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
    g_hub.polls++;
    if (g_hub.mutate_poll == MUT_NO_ANSWER) {
        return PB_MGMT_XFER_NO_ANSWER;
    }
    if (g_hub.mutate_poll == MUT_REJECTED) {
        return PB_MGMT_XFER_REJECTED;
    }
    if (g_hub.mutate_poll == MUT_STALE_NONCE && g_hub.saved_len != 0) {
        memcpy(resp, g_hub.saved, g_hub.saved_len);
        *resp_len = g_hub.saved_len;
        return PB_MGMT_XFER_ANSWERED;
    }

    pb_mgmt_msg_t a;
    memset(&a, 0, sizeof(a));
    hub_identity(&a.id);
    memcpy(a.board_nonce, poll->board_nonce, 16);
    bool echo_ok = g_hub.have_nonce && pb_mgmt_ct_equal(poll->hub_nonce, g_hub.nonce, 16);
    g_hub.have_nonce = false; /* one use */
    if (g_hub.command_pending && (echo_ok || g_hub.skip_nonce_round)) {
        a.type = PB_MGMT_TYPE_UNENROLL;
        memcpy(a.command_id, g_hub.command_id, 16);
    } else {
        a.type = PB_MGMT_TYPE_IDLE;
        a.flags = g_hub.command_pending ? PB_MGMT_IDLE_F_COMMAND_PENDING : 0;
        seq(g_hub.nonce, sizeof(g_hub.nonce), g_hub.nonce_counter++);
        memcpy(a.hub_nonce, g_hub.nonce, 16);
        g_hub.have_nonce = true;
    }

    uint8_t key[32];
    memcpy(key, g_hub.cred.key, 32);
    switch (g_hub.mutate_poll) {
    case MUT_OTHER_BOARD: memcpy(a.id.eui64, EUI_B, 8); break;
    case MUT_OTHER_HUB: a.id.hub_id[0] ^= 0x80; break;
    case MUT_OTHER_GENERATION: a.id.generation[15] ^= 0x01; break;
    case MUT_OLD_ENROLLMENT:
        a.id.generation[15] ^= 0x01;
        key[0] ^= 0x55;
        break;
    case MUT_WRONG_KEY: key[31] ^= 0x01; break;
    case MUT_WRONG_TYPE:
        a.type = PB_MGMT_TYPE_RESULT_ACK;
        a.flags = 0;
        memcpy(a.command_id, g_hub.command_id, 16);
        a.acked_type = PB_MGMT_TYPE_UNENROLL_DONE;
        a.acked_result = PB_MGMT_RESULT_RELEASED;
        break;
    default: break;
    }
    size_t n = pb_mgmt_pack(&a, key, test_hmac, NULL, resp, resp_cap);
    if (n == 0) {
        return PB_MGMT_XFER_NO_ANSWER;
    }
    if (a.type == PB_MGMT_TYPE_UNENROLL && g_hub.mutate_poll == MUT_NONE) {
        memcpy(g_hub.saved, resp, n);
        g_hub.saved_len = n;
    }
    if (g_hub.mutate_poll == MUT_FLIP_BODY_BYTE) {
        resp[PB_MGMT_HEADER_LEN + 20] ^= 0x01;
    } else if (g_hub.mutate_poll == MUT_FLIP_MAC_BYTE) {
        resp[n - 1] ^= 0x01;
    } else if (g_hub.mutate_poll == MUT_TRUNCATED) {
        n -= 1;
    }
    *resp_len = n;
    return PB_MGMT_XFER_ANSWERED;
}

static pb_mgmt_xfer_t hub_exchange(const pb_enroll_bridge_endpoint_t *endpoint, const uint8_t *req, size_t req_len,
                                   uint8_t *resp, size_t resp_cap, size_t *resp_len, void *ctx);

/* ======================= the board's fakes ======================= */

#define EVENTS_MAX 64
static char g_events[EVENTS_MAX];
static size_t g_event_count;
static uint32_t g_delays[16];
static size_t g_delay_count;
static bool g_erase_fails;
/* The erase returns no error but leaves the store as it was (what an
 * unchecked nvs_erase_all() failure looks like). */
static bool g_erase_lies;
static bool g_network_up_ok = true;
static pb_enroll_bridge_endpoint_t g_last_endpoint;
/* The receipt's delivery session (k_ops_session). RAM only: what the platform
 * read from the store just before the erase. A new process (a boot) starts
 * without it. */
static bool g_ram_dataset;
static bool g_session_ran;
static bool g_session_attach_ok = true;
/* A session whose attach DOES write the Dataset to the flash: what
 * OpenThread does without the switch in front of its stores. */
static bool g_session_stores_again;
/* A session that writes it later: while this (1-based) transmission of the
 * receipt is on its way, i.e. after the read-back that preceded it. */
static int g_session_stores_at_receipt;
/* Between receipt_network_up and receipt_network_down. */
static bool g_in_session;
static int g_session_ups, g_session_downs;
/* The next read-back inside a session cannot read the store (it holds
 * nothing all the same). One shot. */
static bool g_session_readback_fails_once;
/* The later look at the store (pb_mgmt_store_check_fn): how often it was
 * asked, and a store it cannot read. */
static int g_store_checks;
static bool g_store_unreadable;
/* The read-back after an erase cannot read the store either. */
static bool g_readback_fails;
/* One boot's RAM, as components/power_base_ot_store keeps it: both stores
 * were read back empty in this boot, and what was seen of the entry under the
 * Network Key's id since. A new boot starts without it. */
static bool g_read_back_empty;
static int g_key_writes_seen, g_key_removes_seen;
/* ... and which entry that one write stored (every stored entry has a serial
 * number here, standing for its bytes). */
static uint8_t g_key_serial_seen;
static uint8_t g_key_serial_next = 1;
/* Erases -- OpenThread's and the store's own -- that remove the Dataset,
 * leave the entry under the Network Key's id, and report no error. */
static bool g_erases_leave_network_key;
/* The store's own erase (used before OpenThread starts) returns an error and
 * erases nothing. */
static bool g_direct_erase_fails;
/* What pb_mgmt_released_prestart() said in the last platform_start(). */
static pb_mgmt_prestart_t g_prestart_result;

static void event(char e)
{
    if (g_event_count < EVENTS_MAX - 1) {
        g_events[g_event_count++] = e;
        g_events[g_event_count] = 0;
    }
}

static void events_reset(void)
{
    g_event_count = 0;
    g_events[0] = 0;
    g_delay_count = 0;
}

/* ---- the two stores, in the simulated flash: "ds" stands for OpenThread's
 * settings (the Dataset), "nk" for the entry under the Network Key's id in
 * the key store. That entry's byte says whose key it is -- for this file's
 * assertions only; nothing that decides anything reads it. "ns" is that
 * entry's serial number: what its bytes are, as far as "is it the same entry"
 * goes. ---- */

static void store_put(const char *key, uint8_t value)
{
    nvs_handle_t h;
    if (nvs_open("ot_sim", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, key, &value, 1);
        nvs_commit(h);
        nvs_close(h);
    }
}

static uint8_t store_get(const char *key)
{
    nvs_handle_t h;
    uint8_t value = 0;
    size_t len = 1;
    if (nvs_open("ot_sim", NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    if (nvs_get_blob(h, key, &value, &len) != ESP_OK) {
        value = 0;
    }
    nvs_close(h);
    return value;
}

static void store_remove(const char *key)
{
    nvs_handle_t h;
    if (nvs_open("ot_sim", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, key);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool dataset_present(void)
{
    return store_get("ds") != 0;
}

/* 'N' = a network's key, 'b' = the key OpenThread made for itself at a
 * start, 0 = nothing there. */
static char network_key_whose(void)
{
    return (char)store_get("nk");
}

static bool network_key_present(void)
{
    return store_get("nk") != 0;
}

/* A network is stored the way OpenThread stores one: the Dataset, and the
 * network's key in place of whatever was under the Network Key's id (a
 * remove and a write, both through the key store). */
/* A new entry under the Network Key's id. Its serial number is returned. */
static uint8_t network_key_write(char whose)
{
    uint8_t serial = g_key_serial_next++;
    if (g_key_serial_next == 0) {
        g_key_serial_next = 1;
    }
    store_put("nk", (uint8_t)whose);
    store_put("ns", serial);
    return serial;
}

/* The same through the key store's storage interface: seen. */
static void network_key_write_seen(char whose)
{
    g_key_serial_seen = network_key_write(whose);
    g_key_writes_seen++;
}

static void dataset_store(void)
{
    store_put("ds", 1);
    if (network_key_present()) {
        g_key_removes_seen++;
    }
    network_key_write_seen('N');
}

/* The entry under the Network Key's id, put there with nothing looking: what
 * an erase that did not remove it leaves behind. */
static void network_key_left_behind(void)
{
    (void)network_key_write('N');
}

/* OpenThread's erase: the Dataset and every key, its own included. */
static void dataset_erase(void)
{
    store_remove("ds");
    if (!g_erases_leave_network_key && network_key_present()) {
        store_remove("nk");
        store_remove("ns");
        g_key_removes_seen++;
    }
}

/* "Nothing is left": the proof of an erase. When it says yes, that is
 * remembered for this boot and the Network Key's id is watched from here. */
static bool store_empty(void)
{
    bool empty = !g_readback_fails && !dataset_present() && !network_key_present();
    if (empty) {
        g_read_back_empty = true;
        g_key_writes_seen = g_key_removes_seen = 0;
        g_key_serial_seen = 0;
    }
    return empty;
}

/* pb_mgmt_store_check_fn: "no network is stored" -- and what lies under the
 * Network Key's id is nothing, or the one entry this boot saw being stored
 * after it had read the stores back empty: one write, no remove, and the
 * entry there now is that write. Never: whatever lies there. */
static bool no_network_stored(void *ctx)
{
    (void)ctx;
    g_store_checks++;
    if (g_store_unreadable || dataset_present()) {
        return false;
    }
    return !network_key_present() || (g_read_back_empty && g_key_writes_seen == 1 && g_key_removes_seen == 0 &&
                                      store_get("ns") == g_key_serial_seen);
}

/* ---- a boot, in app_main.c's order ---- */

/* The store's own erase: no OpenThread is needed, none exists yet. */
static esp_err_t op_prestart_erase(void *ctx)
{
    (void)ctx;
    event('x');
    if (g_direct_erase_fails) {
        return ESP_FAIL;
    }
    store_remove("ds");
    if (!g_erases_leave_network_key) {
        store_remove("nk");
        store_remove("ns");
    }
    return ESP_OK;
}

static bool op_prestart_erased(void *ctx)
{
    (void)ctx;
    event('y');
    return store_empty();
}

static const pb_mgmt_prestart_ops_t k_prestart_ops = {
    .erase = op_prestart_erase,
    .erased = op_prestart_erased,
    .ctx = NULL,
};

/* What OpenThread's start does (KeyManager::Init()): a key under the Network
 * Key's id is left alone, whoever's it is; when there is none, one is made. */
static void openthread_starts(void)
{
    if (!network_key_present()) {
        network_key_write_seen('b');
    }
}

/* Power lost between the two steps of platform_start() (children only). */
static bool g_power_cut_after_prestart;

/* One boot up to the point where the firmware looks at its record: RAM is
 * new; a board whose record is a final release erases its stores and reads
 * them back; then OpenThread starts. (in_child() = one boot.) */
static void platform_start(void)
{
    g_read_back_empty = false;
    g_key_writes_seen = g_key_removes_seen = 0;
    g_key_serial_seen = 0;
    g_prestart_result = pb_mgmt_released_prestart(EUI_A, &k_prestart_ops);
    if (g_power_cut_after_prestart) {
        _exit(77);
    }
    openthread_starts();
}

/* As the firmware's BLE service asks for the receipt. */
static bool receipt_load(const uint8_t eui64[8], uint8_t out[PB_MGMT_RECEIPT_LEN])
{
    return pb_mgmt_receipt_load(eui64, no_network_stored, NULL, out);
}

static uint8_t g_random_counter = 0xb0;

static bool op_random(uint8_t *buf, size_t len, void *ctx)
{
    (void)ctx;
    seq(buf, len, g_random_counter);
    g_random_counter = (uint8_t)(g_random_counter + 0x11);
    return true;
}

static void op_quiesce(void *ctx)
{
    (void)ctx;
    event('Q');
    if (g_on_request != NULL) {
        g_on_request('Q');
    }
}

static void op_delay(uint32_t ms, void *ctx)
{
    (void)ctx;
    if (g_delay_count < 16) {
        g_delays[g_delay_count++] = ms;
    }
}

static bool op_network_up(void *ctx)
{
    (void)ctx;
    event('U');
    return g_network_up_ok && dataset_present();
}

static esp_err_t op_network_erase(void *ctx)
{
    (void)ctx;
    event('E');
    if (g_on_request != NULL) {
        g_on_request('E');
    }
    if (!g_session_ran) {
        g_ram_dataset = dataset_present(); /* kept for the receipt's session, in RAM */
    }
    if (g_erase_fails) {
        return ESP_FAIL;
    }
    if (!g_erase_lies) {
        dataset_erase();
    }
    return ESP_OK;
}

/* Reads the store back, like the firmware reads OpenThread's NVS namespace
 * and its keys. 'V' after an erase, 'v' inside the delivery session (before a
 * transmission of the receipt). */
static bool op_network_erased(void *ctx)
{
    (void)ctx;
    if (g_in_session) {
        event('v');
        if (g_session_readback_fails_once) {
            g_session_readback_fails_once = false;
            return false;
        }
        return store_empty();
    }
    event('V');
    bool empty = store_empty();
    if (empty && g_on_request != NULL) {
        g_on_request('e'); /* erased and verified, final record not yet written */
    }
    return empty;
}

static const pb_mgmt_ops_t k_ops = {
    .hmac = test_hmac,
    .random = op_random,
    .exchange = hub_exchange,
    .quiesce = op_quiesce,
    .delay_ms = op_delay,
    .network_up_for_proof = op_network_up,
    .network_erase = op_network_erase,
    .network_erased = op_network_erased,
    .ctx = NULL,
};

/* The session: Thread from the Dataset kept in RAM. Like the firmware's, it
 * does not start when nothing was kept or the flash is not empty. */
static bool op_receipt_up(void *ctx)
{
    (void)ctx;
    event('S');
    g_session_ups++;
    g_session_ran = true;
    g_in_session = true;
    if (g_on_request != NULL) {
        g_on_request('S');
    }
    if (!g_ram_dataset || !store_empty()) {
        return false;
    }
    if (g_session_stores_again) {
        dataset_store();
    }
    if (g_on_request != NULL) {
        g_on_request('a'); /* attached (or not): whatever the attach stored is stored */
    }
    return g_session_attach_ok;
}

static bool op_receipt_down(void *ctx)
{
    (void)ctx;
    event('s');
    g_session_downs++;
    g_ram_dataset = false;
    g_in_session = false;
    if (g_on_request != NULL) {
        g_on_request('s');
    }
    return store_empty();
}

/* A platform with the delivery session (the firmware's). */
static const pb_mgmt_ops_t k_ops_session = {
    .hmac = test_hmac,
    .random = op_random,
    .exchange = hub_exchange,
    .quiesce = op_quiesce,
    .delay_ms = op_delay,
    .network_up_for_proof = op_network_up,
    .network_erase = op_network_erase,
    .network_erased = op_network_erased,
    .receipt_network_up = op_receipt_up,
    .receipt_network_down = op_receipt_down,
    .ctx = NULL,
};

static void session_reset(void)
{
    g_ram_dataset = false;
    g_session_ran = false;
    g_session_attach_ok = true;
    g_session_stores_again = false;
    g_session_stores_at_receipt = 0;
    g_in_session = false;
    g_session_readback_fails_once = false;
    g_session_ups = g_session_downs = 0;
}

static pb_mgmt_xfer_t hub_exchange(const pb_enroll_bridge_endpoint_t *endpoint, const uint8_t *req, size_t req_len,
                                   uint8_t *resp, size_t resp_cap, size_t *resp_len, void *ctx)
{
    (void)ctx;
    *resp_len = 0;
    g_last_endpoint = *endpoint;
    pb_mgmt_msg_t m;
    /* The hub's own checks: structure, then the MAC with the credential it
     * holds for this board. */
    if (!pb_mgmt_parse(req, req_len, &m)) {
        event('?');
        return PB_MGMT_XFER_REJECTED;
    }
    if (!pb_mgmt_verify(req, req_len, g_hub.cred.key, test_hmac, NULL) ||
        !pb_mgmt_ct_equal(m.id.eui64, g_hub.eui64, 8) || !pb_mgmt_ct_equal(m.id.hub_id, g_hub.cred.hub_id, 16) ||
        !pb_mgmt_ct_equal(m.id.generation, g_hub.cred.generation, 16)) {
        event('!');
        return PB_MGMT_XFER_REJECTED;
    }
    if (g_hub.completed && g_hub.forgets_key_when_complete) {
        event('!'); /* no credential any more: one refusal for everything */
        return PB_MGMT_XFER_REJECTED;
    }
    switch (m.type) {
    case PB_MGMT_TYPE_HELLO: {
        if (!g_hub.knows_hello) {
            event('?');
            return PB_MGMT_XFER_REJECTED;
        }
        event('H');
        g_hub.hellos++;
        if (g_hub.hello_drop > 0) {
            g_hub.hello_drop--;
            return PB_MGMT_XFER_NO_ANSWER;
        }
        g_hub.board_caps = m.capabilities;
        pb_mgmt_msg_t a;
        memset(&a, 0, sizeof(a));
        a.type = PB_MGMT_TYPE_HELLO_ACK;
        hub_identity(&a.id);
        a.capabilities = PB_MGMT_HUB_CAP_THREAD_RECEIPT;
        size_t n = pb_mgmt_pack(&a, g_hub.cred.key, test_hmac, NULL, resp, resp_cap);
        if (g_hub.hello_bad_ack) {
            resp[n - 5] ^= 0x10;
        }
        *resp_len = n;
        return PB_MGMT_XFER_ANSWERED;
    }
    case PB_MGMT_TYPE_POLL:
        event('P');
        if (g_on_request != NULL) {
            g_on_request(m.type);
        }
        return hub_answer_poll(&m, resp, resp_cap, resp_len);
    case PB_MGMT_TYPE_UNENROLL_ACCEPTED:
        event('A');
        g_hub.accepted++;
        if (g_on_request != NULL) {
            g_on_request(m.type);
        }
        return PB_MGMT_XFER_NO_ANSWER; /* its answer is never needed */
    case PB_MGMT_TYPE_UNENROLL_DONE: {
        /* 'D' = the "released" notice. 'Z' = the receipt on the network:
         * never from a platform without the delivery session, and from one
         * with it only after the final record is stored. */
        const bool is_receipt = m.result == PB_MGMT_RESULT_ERASED;
        event(is_receipt ? 'Z' : 'D');
        g_hub.done++;
        if (is_receipt) {
            g_hub.done_erased++;
        } else {
            g_hub.done_released++;
        }
        if (g_on_request != NULL) {
            g_on_request(is_receipt ? 'Z' : m.type);
        }
        if (is_receipt && g_session_stores_at_receipt == g_hub.done_erased) {
            dataset_store(); /* after the read-back that let this one out */
        }
        if (is_receipt ? g_hub.receipt_reject : g_hub.done_reject) {
            return PB_MGMT_XFER_REJECTED;
        }
        if (is_receipt) {
            /* Taken: it parsed and its MAC verified above. */
            if (!g_hub.completed) {
                memcpy(g_hub.receipt, req, PB_MGMT_UNENROLL_DONE_LEN);
            } else if (memcmp(g_hub.receipt, req, PB_MGMT_UNENROLL_DONE_LEN) != 0) {
                g_hub.receipt_differed = true;
            }
            g_hub.completed = true;
            if (g_hub.receipt_drop > 0) {
                g_hub.receipt_drop--;
                return PB_MGMT_XFER_NO_ANSWER;
            }
        } else if (g_hub.done_drop > 0) {
            g_hub.done_drop--;
            return PB_MGMT_XFER_NO_ANSWER;
        }
        pb_mgmt_msg_t a;
        memset(&a, 0, sizeof(a));
        a.type = PB_MGMT_TYPE_RESULT_ACK;
        hub_identity(&a.id);
        memcpy(a.command_id, m.command_id, 16);
        a.acked_type = PB_MGMT_TYPE_UNENROLL_DONE;
        a.acked_result = m.result;
        if (is_receipt ? g_hub.receipt_ack_other_result : g_hub.done_ack_other_result) {
            a.acked_result = m.result == PB_MGMT_RESULT_RELEASED ? PB_MGMT_RESULT_ERASED : PB_MGMT_RESULT_RELEASED;
        }
        size_t n = pb_mgmt_pack(&a, g_hub.cred.key, test_hmac, NULL, resp, resp_cap);
        if (is_receipt ? g_hub.receipt_bad_ack : g_hub.done_bad_ack) {
            resp[n - 5] ^= 0x10;
        }
        *resp_len = n;
        return PB_MGMT_XFER_ANSWERED;
    }
    default:
        event('?');
        return PB_MGMT_XFER_REJECTED;
    }
}

/* ======================= flash helpers ======================= */

static pb_enroll_record_t enrolled_record(const pb_enroll_mgmt_credential_t *cred)
{
    pb_enroll_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.id = NET_OTBR;
    rec.has_bridge_endpoint = true;
    rec.bridge_endpoint = EP_OTBR;
    rec.has_mgmt = true;
    rec.mgmt_state = PB_ENROLL_MGMT_ENROLLED;
    rec.mgmt = *cred;
    return rec;
}

static size_t raw_record(uint8_t out[PB_ENROLL_RECORD_MAX_LEN])
{
    nvs_handle_t h;
    size_t len = PB_ENROLL_RECORD_MAX_LEN;
    memset(out, 0, PB_ENROLL_RECORD_MAX_LEN);
    if (nvs_open(PB_ENROLL_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    if (nvs_get_blob(h, PB_ENROLL_NVS_KEY, out, &len) != ESP_OK) {
        len = 0;
    }
    nvs_close(h);
    return len;
}

/* A board that was enrolled with the golden credential: record version 3
 * and a stored Dataset. */
static void flash_enrolled(const uint8_t eui64[8], const pb_enroll_mgmt_credential_t *cred)
{
    pb_enroll_record_t rec = enrolled_record(cred);
    nvs_sim_reset();
    /* A new board in a new boot: nothing was read back, no fault is set. */
    g_read_back_empty = false;
    g_key_writes_seen = g_key_removes_seen = 0;
    g_store_unreadable = g_readback_fails = g_erases_leave_network_key = g_direct_erase_fails = false;
    (void)pb_enroll_record_store(eui64, &rec);
    dataset_store();
}

/* ======================= scenarios: the wire format ======================= */

static bool mac_is(const uint8_t *msg, size_t len, const char *expected_hex)
{
    char got[65] = {0};
    hex(&msg[len - 32], 32, got);
    return strcmp(got, expected_hex) == 0;
}

static void scenario_test_hmac_is_hmac(void)
{
    printf("\n-- the test's own HMAC-SHA-256 is HMAC-SHA-256 (RFC 4231) --\n");
    uint8_t out[32];
    char got[65] = {0};
    host_hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, out);
    hex(out, 32, got);
    CHECK(strcmp(got, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") == 0, "RFC 4231 test case 2");
    uint8_t key[20];
    memset(key, 0x0b, sizeof(key));
    host_hmac_sha256(key, sizeof(key), (const uint8_t *)"Hi There", 8, out);
    hex(out, 32, got);
    CHECK(strcmp(got, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7") == 0, "RFC 4231 test case 1");
}

static void scenario_golden_vectors(void)
{
    printf("\n-- golden vectors (docs/power-base-management.md; the same bytes are checked on the hub with OpenSSL) --\n");
    pb_enroll_mgmt_credential_t cred = golden_credential();
    pb_mgmt_msg_t m;
    uint8_t wire[PB_MGMT_MAX_MESSAGE_LEN];
    uint8_t bn[16], hn[16], cmd[16];
    seq(bn, 16, 0xb0);
    seq(hn, 16, 0xd0);
    seq(cmd, 16, 0xe0);

#define BASE(t)                                                                                                      \
    do {                                                                                                             \
        memset(&m, 0, sizeof(m));                                                                                    \
        m.type = (t);                                                                                                \
        memcpy(m.id.eui64, EUI_A, 8);                                                                                \
        memcpy(m.id.hub_id, cred.hub_id, 16);                                                                        \
        memcpy(m.id.generation, cred.generation, 16);                                                                \
    } while (0)

    BASE(PB_MGMT_TYPE_POLL);
    memcpy(m.board_nonce, bn, 16);
    memcpy(m.hub_nonce, hn, 16);
    m.state = PB_MGMT_STATE_ENROLLED;
    size_t n = pb_mgmt_pack(&m, cred.key, test_hmac, NULL, wire, sizeof(wire));
    CHECK(n == PB_MGMT_POLL_LEN && wire[0] == 1 && wire[1] == 0x01 && wire[2] == 0 && wire[3] == 0 &&
              memcmp(&wire[4], EUI_A, 8) == 0 && wire[12] == 0x10 && wire[27] == 0x1f && wire[28] == 0x20 &&
              wire[43] == 0x2f && wire[44] == 0xb0 && wire[60] == 0xd0 && wire[76] == 1 && wire[77] == 0 &&
              wire[78] == 0 && wire[79] == 0,
          "POLL: 112 bytes, every field at its documented offset");
    CHECK(mac_is(wire, n, "e688613943593c0dcca578bede6c46ea660c135bfe5dd7b613f9a9a04cc77c5f"), "POLL: golden MAC");
    pb_mgmt_msg_t back;
    CHECK(pb_mgmt_parse(wire, n, &back) && pb_mgmt_verify(wire, n, cred.key, test_hmac, NULL) &&
              memcmp(&back, &m, sizeof(m)) == 0,
          "POLL: parses back to the same message and verifies");

    BASE(PB_MGMT_TYPE_IDLE);
    m.flags = PB_MGMT_IDLE_F_COMMAND_PENDING;
    memcpy(m.board_nonce, bn, 16);
    memcpy(m.hub_nonce, hn, 16);
    n = pb_mgmt_pack(&m, cred.key, test_hmac, NULL, wire, sizeof(wire));
    CHECK(n == PB_MGMT_IDLE_LEN && wire[1] == 0x81 && wire[2] == 0x01 &&
              mac_is(wire, n, "7a1c2f5a3a0a982d10d1fe6d8bfc81209403bc879521526069d478ba32a2b6f2"),
          "IDLE (command pending): 108 bytes, golden MAC");

    BASE(PB_MGMT_TYPE_UNENROLL);
    memcpy(m.board_nonce, bn, 16);
    memcpy(m.command_id, cmd, 16);
    n = pb_mgmt_pack(&m, cred.key, test_hmac, NULL, wire, sizeof(wire));
    CHECK(n == PB_MGMT_UNENROLL_LEN && wire[1] == 0x82 && wire[60] == 0xe0 &&
              mac_is(wire, n, "2125a1901a7889b262b44d33ff43fc6fb3b22d0217227f91a551ee6bd2bb1a8c"),
          "UNENROLL: 108 bytes, golden MAC");

    BASE(PB_MGMT_TYPE_UNENROLL_ACCEPTED);
    memcpy(m.command_id, cmd, 16);
    n = pb_mgmt_pack(&m, cred.key, test_hmac, NULL, wire, sizeof(wire));
    CHECK(n == PB_MGMT_UNENROLL_ACCEPTED_LEN && wire[1] == 0x02 &&
              mac_is(wire, n, "cca2b43ad652e17a0a6b9b37119c65ee8362074788c3f27ae9eb8013b3f78dc7"),
          "UNENROLL_ACCEPTED: 92 bytes, golden MAC");

    BASE(PB_MGMT_TYPE_UNENROLL_DONE);
    memcpy(m.command_id, cmd, 16);
    m.result = PB_MGMT_RESULT_RELEASED;
    n = pb_mgmt_pack(&m, cred.key, test_hmac, NULL, wire, sizeof(wire));
    CHECK(n == PB_MGMT_UNENROLL_DONE_LEN && wire[1] == 0x03 && wire[60] == 1 &&
              mac_is(wire, n, "02f00a8d63fba56ff747911889aae31a29b60d053178db827a8e8730a05b7eba"),
          "UNENROLL_DONE / RELEASED (the notice): 96 bytes, golden MAC -- the same bytes as before the result "
          "ERASED existed; what changed is that no hub calls them a completion");
    uint8_t released_wire[PB_MGMT_UNENROLL_DONE_LEN];
    memcpy(released_wire, wire, n);

    /* The golden MACs below were computed with Python's hmac/hashlib from the
     * documented layout, not with this code. */
    m.result = PB_MGMT_RESULT_ERASED;
    n = pb_mgmt_pack(&m, cred.key, test_hmac, NULL, wire, sizeof(wire));
    CHECK(n == PB_MGMT_UNENROLL_DONE_LEN && wire[1] == 0x03 && wire[60] == 2 && wire[61] == 0 && wire[62] == 0 &&
              wire[63] == 0 && mac_is(wire, n, "efd7833baf44c006d3f980b77630e2250e2d7f61328cceee83ac30a4a41fe01b"),
          "UNENROLL_DONE / ERASED (the receipt): 96 bytes, golden MAC");
    CHECK(memcmp(released_wire, wire, 60) == 0 && released_wire[60] != wire[60] &&
              memcmp(&released_wire[64], &wire[64], 32) != 0,
          "the two differ in the result byte and therefore in the MAC: one can never be turned into the other "
          "without the key");
    uint8_t with_mac[PB_MGMT_MAX_MESSAGE_LEN];
    size_t n2 = pb_mgmt_pack_with_mac(&m, pb_mgmt_mac_of(wire, n), with_mac, sizeof(with_mac));
    CHECK(n2 == n && memcmp(wire, with_mac, n) == 0, "the receipt rebuilt from its stored MAC: the same bytes");
    m.result = 3;
    CHECK(pb_mgmt_pack(&m, cred.key, test_hmac, NULL, wire, sizeof(wire)) == 0 &&
              (m.result = 0, pb_mgmt_pack(&m, cred.key, test_hmac, NULL, wire, sizeof(wire)) == 0),
          "no other result can be encoded");

    BASE(PB_MGMT_TYPE_RESULT_ACK);
    memcpy(m.command_id, cmd, 16);
    m.acked_type = PB_MGMT_TYPE_UNENROLL_DONE;
    m.acked_result = PB_MGMT_RESULT_RELEASED;
    n = pb_mgmt_pack(&m, cred.key, test_hmac, NULL, wire, sizeof(wire));
    CHECK(n == PB_MGMT_RESULT_ACK_LEN && wire[1] == 0x83 && wire[60] == 0x03 && wire[61] == 1 && wire[62] == 0 &&
              wire[63] == 0 && mac_is(wire, n, "b49c98d6300315bc13e54178bd1e8600401dd5c0304858516fdf469a33a84112"),
          "RESULT_ACK of the released notice: names the result it acknowledges, golden MAC");
    CHECK(pb_mgmt_parse(wire, n, &back) && back.acked_type == PB_MGMT_TYPE_UNENROLL_DONE &&
              back.acked_result == PB_MGMT_RESULT_RELEASED,
          "and parses back with it");
    m.acked_result = PB_MGMT_RESULT_ERASED;
    n = pb_mgmt_pack(&m, cred.key, test_hmac, NULL, wire, sizeof(wire));
    CHECK(n == PB_MGMT_RESULT_ACK_LEN && wire[61] == 2 &&
              mac_is(wire, n, "174d78c5ec891b13dad88c9eaf93c5c4d9b21602eafeab955569673dbf8085e4"),
          "RESULT_ACK of the receipt: golden MAC, different from the notice's");
    m.acked_result = 0;
    CHECK(pb_mgmt_pack(&m, cred.key, test_hmac, NULL, wire, sizeof(wire)) == 0,
          "an acknowledgement of UNENROLL_DONE that names no result cannot be encoded (the form an earlier hub sent)");
    m.acked_type = PB_MGMT_TYPE_UNENROLL_ACCEPTED;
    n = pb_mgmt_pack(&m, cred.key, test_hmac, NULL, wire, sizeof(wire));
    CHECK(n == PB_MGMT_RESULT_ACK_LEN && wire[60] == 0x02 && wire[61] == 0 &&
              mac_is(wire, n, "33e83b068d5d9ff3975fae6cd65eccb1c5458c957816eb2af45475d826bac23d"),
          "RESULT_ACK of UNENROLL_ACCEPTED: no result, golden MAC");
    m.acked_result = PB_MGMT_RESULT_RELEASED;
    CHECK(pb_mgmt_pack(&m, cred.key, test_hmac, NULL, wire, sizeof(wire)) == 0,
          "and an acknowledgement of UNENROLL_ACCEPTED cannot name one");

    /* HELLO / HELLO_ACK (added 2026-10-08; golden MACs from Python's hmac). */
    BASE(PB_MGMT_TYPE_HELLO);
    m.capabilities = PB_MGMT_BOARD_CAP_THREAD_RECEIPT;
    n = pb_mgmt_pack(&m, cred.key, test_hmac, NULL, wire, sizeof(wire));
    CHECK(n == PB_MGMT_HELLO_LEN && wire[0] == 1 && wire[1] == 0x05 && wire[2] == 0 && wire[3] == 0 && wire[44] == 0 &&
              wire[45] == 0 && wire[46] == 0 && wire[47] == 1 && pb_mgmt_is_zero(&wire[48], 4) &&
              mac_is(wire, n, "5ce6b6e36eb4f9ce77dade4eb4b03fded2243fe4af50ef92169d2d2142decb21"),
          "HELLO: 84 bytes, capabilities big-endian at offset 44, golden MAC");
    CHECK(pb_mgmt_parse(wire, n, &back) && back.type == PB_MGMT_TYPE_HELLO &&
              back.capabilities == PB_MGMT_BOARD_CAP_THREAD_RECEIPT && pb_mgmt_verify(wire, n, cred.key, test_hmac, NULL),
          "HELLO: parses back and verifies");
    m.capabilities = 0x80000001u;
    n = pb_mgmt_pack(&m, cred.key, test_hmac, NULL, wire, sizeof(wire));
    CHECK(n == PB_MGMT_HELLO_LEN && wire[44] == 0x80 &&
              mac_is(wire, n, "61cc48d6b8129bdcad718af4e2c2fe1cc8da82c60dc05f26fcf4cebf315a196f") &&
              pb_mgmt_parse(wire, n, &back) && back.capabilities == 0x80000001u,
          "HELLO: a capability bit this code does not know is carried, not refused");
    wire[48] = 1;
    CHECK(!pb_mgmt_parse(wire, n, &back), "HELLO: the four reserved bytes after the capabilities must be zero");
    m.capabilities = PB_MGMT_BOARD_CAP_THREAD_RECEIPT;
    m.flags = 1;
    CHECK(pb_mgmt_pack(&m, cred.key, test_hmac, NULL, wire, sizeof(wire)) == 0, "HELLO defines no flag");
    BASE(PB_MGMT_TYPE_HELLO_ACK);
    m.capabilities = PB_MGMT_HUB_CAP_THREAD_RECEIPT;
    n = pb_mgmt_pack(&m, cred.key, test_hmac, NULL, wire, sizeof(wire));
    CHECK(n == PB_MGMT_HELLO_ACK_LEN && wire[1] == 0x85 && wire[47] == 1 &&
              mac_is(wire, n, "9fb3c6c60ba32ecda5ca9efafdadbb32e0c2ac6771eb9cb51e5f35e820d9c314"),
          "HELLO_ACK: 84 bytes, golden MAC");
#undef BASE
}

static void scenario_malformed_messages(void)
{
    printf("\n-- malformed, unknown and unauthentic messages --\n");
    pb_enroll_mgmt_credential_t cred = golden_credential();
    pb_mgmt_msg_t m, out;
    uint8_t wire[PB_MGMT_MAX_MESSAGE_LEN + 4];
    memset(&m, 0, sizeof(m));
    m.type = PB_MGMT_TYPE_UNENROLL;
    memcpy(m.id.eui64, EUI_A, 8);
    memcpy(m.id.hub_id, cred.hub_id, 16);
    memcpy(m.id.generation, cred.generation, 16);
    seq(m.board_nonce, 16, 0xb0);
    seq(m.command_id, 16, 0xe0);
    size_t n = pb_mgmt_pack(&m, cred.key, test_hmac, NULL, wire, sizeof(wire));
    CHECK(n == PB_MGMT_UNENROLL_LEN && pb_mgmt_parse(wire, n, &out), "baseline UNENROLL parses");

    uint8_t t[PB_MGMT_MAX_MESSAGE_LEN + 4];
#define MUTATED(stmt, why)                                                                                           \
    do {                                                                                                             \
        memcpy(t, wire, n);                                                                                          \
        size_t len = n;                                                                                              \
        stmt;                                                                                                        \
        CHECK(!pb_mgmt_parse(t, len, &out), why);                                                                    \
    } while (0)
    MUTATED(t[0] = 2, "version 2 is refused");
    MUTATED(t[0] = 0, "version 0 is refused");
    MUTATED(t[1] = 0x7f, "an unknown type is refused");
    MUTATED(t[1] = 0x04, "a bridge_export_proto message type is not a management type");
    MUTATED(t[2] = 0x01, "a flag bit UNENROLL does not define is refused");
    MUTATED(t[3] = 1, "a non-zero reserved byte is refused");
    MUTATED(len = n - 1, "one byte short is refused");
    MUTATED(len = n + 1, "one byte long is refused");
    MUTATED(len = PB_MGMT_HEADER_LEN, "a bare header is refused");
    MUTATED(memset(&t[4], 0, 8), "an all-zero EUI-64 is refused");
    MUTATED(memset(&t[12], 0, 16), "an all-zero hub id is refused");
    MUTATED(memset(&t[28], 0, 16), "an all-zero generation is refused");
    MUTATED(memset(&t[44], 0, 16), "an all-zero board nonce is refused");
    MUTATED(memset(&t[60], 0, 16), "an all-zero command id is refused");
    MUTATED(t[1] = PB_MGMT_TYPE_UNENROLL_DONE, "another type's layout with this length is refused");
#undef MUTATED
    CHECK(!pb_mgmt_parse(NULL, n, &out) && !pb_mgmt_parse(wire, 0, &out), "NULL and empty are refused");

    /* values the protocol does not have */
    pb_mgmt_msg_t bad = m;
    bad.type = PB_MGMT_TYPE_UNENROLL_DONE;
    bad.result = 3; /* 1 = released, 2 = erased; there is no third */
    CHECK(pb_mgmt_pack(&bad, cred.key, test_hmac, NULL, t, sizeof(t)) == 0, "UNENROLL_DONE with a result the protocol does not have cannot be built");
    bad.type = PB_MGMT_TYPE_POLL;
    bad.state = 0;
    CHECK(pb_mgmt_pack(&bad, cred.key, test_hmac, NULL, t, sizeof(t)) == 0, "POLL with another state cannot be built");
    bad.type = PB_MGMT_TYPE_RESULT_ACK;
    bad.acked_type = PB_MGMT_TYPE_POLL;
    CHECK(pb_mgmt_pack(&bad, cred.key, test_hmac, NULL, t, sizeof(t)) == 0, "RESULT_ACK of a POLL cannot be built");
    CHECK(pb_mgmt_pack(&m, cred.key, test_hmac, NULL, t, n - 1) == 0, "a buffer one byte too small: nothing is written");

    /* authentication */
    CHECK(pb_mgmt_verify(wire, n, cred.key, test_hmac, NULL), "the right key verifies");
    uint8_t other[32];
    memcpy(other, cred.key, 32);
    other[0] ^= 1;
    CHECK(!pb_mgmt_verify(wire, n, other, test_hmac, NULL), "a key differing in one bit does not");
    int rejected = 0;
    for (size_t i = 0; i < n; i++) {
        memcpy(t, wire, n);
        t[i] ^= 0x01;
        if (!pb_mgmt_verify(t, n, cred.key, test_hmac, NULL)) {
            rejected++;
        }
    }
    CHECK(rejected == (int)n, "flipping any single byte of the message breaks the MAC");
    CHECK(!pb_mgmt_verify(wire, n - 1, cred.key, test_hmac, NULL), "a truncated message does not verify");
}

/* ======================= scenarios: the client ======================= */

static void scenario_poll_idle(void)
{
    printf("\n-- an enrolled board polls; nothing is pending --\n");
    pb_enroll_mgmt_credential_t cred = golden_credential();
    flash_enrolled(EUI_A, &cred);
    hub_init(EUI_A, &cred);
    events_reset();
    uint8_t before[PB_ENROLL_RECORD_MAX_LEN], after[PB_ENROLL_RECORD_MAX_LEN];
    size_t before_len = raw_record(before);

    pb_mgmt_client_t c;
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    CHECK(pb_mgmt_client_load(&c) == PB_MGMT_BOOT_ARMED && c.armed, "record version 3, ENROLLED: the client is armed");
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_IDLE && g_hub.polls == 1, "poll: the hub answers IDLE");
    CHECK(memcmp(&g_last_endpoint, &EP_OTBR, sizeof(EP_OTBR)) == 0, "the poll goes to the record's own bridge endpoint");
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_IDLE && pb_mgmt_client_poll(&c) == PB_MGMT_POLL_IDLE,
          "and again, and again");
    size_t after_len = raw_record(after);
    CHECK(before_len == PB_ENROLL_RECORD_V3_LEN && after_len == before_len && memcmp(before, after, before_len) == 0,
          "polling writes nothing: the stored record is byte for byte the same");
    CHECK(strchr(g_events, 'Q') == NULL && strchr(g_events, 'E') == NULL && dataset_present(),
          "no service was stopped and the network was not touched");
    pb_mgmt_client_wipe(&c);
    CHECK(pb_mgmt_is_zero(c.rec.mgmt.key, 32) && !c.armed, "wipe removes the key from RAM");
}

/* What the stored record says, in one number: mgmt_state * 10 + release flags
 * (0 for no version 3 record). */
static int stored_state(const uint8_t eui64[8])
{
    pb_enroll_record_t rec;
    if (pb_enroll_record_load(eui64, &rec) != PB_ENROLL_MARKER_VALID || !rec.has_mgmt) {
        return 0;
    }
    return (int)rec.mgmt_state * 10 + rec.release_flags;
}

static int g_state_at_accepted, g_state_at_done, g_state_at_erase, g_state_at_verified;
static bool g_key_present_at_done, g_dataset_at_done;
/* Whether a receipt could be read from the board at each point. */
static bool g_receipt_at_done, g_receipt_at_erase, g_receipt_at_verified;

static bool receipt_available(void)
{
    uint8_t receipt[PB_MGMT_RECEIPT_LEN];
    return receipt_load(EUI_A, receipt);
}

static void observe_release(uint8_t type)
{
    if (type == PB_MGMT_TYPE_UNENROLL_ACCEPTED) {
        g_state_at_accepted = stored_state(EUI_A);
    } else if (type == PB_MGMT_TYPE_UNENROLL_DONE && g_state_at_done == 0) {
        pb_enroll_record_t rec;
        g_state_at_done = stored_state(EUI_A);
        if (pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_VALID) {
            g_key_present_at_done = !pb_mgmt_is_zero(rec.mgmt.key, 32);
        }
        g_dataset_at_done = dataset_present();
        g_receipt_at_done = receipt_available();
    } else if (type == 'E') {
        g_state_at_erase = stored_state(EUI_A);
        g_receipt_at_erase = receipt_available();
    } else if (type == 'e') {
        g_state_at_verified = stored_state(EUI_A);
        g_receipt_at_verified = receipt_available();
    }
}

static void scenario_unenroll_online(void)
{
    printf("\n-- an online board is unenrolled: accepted, released, erased and the receipt are four things --\n");
    pb_enroll_mgmt_credential_t cred = golden_credential();
    flash_enrolled(EUI_A, &cred);
    /* What must survive: the prepared image's namespaces in 'cfg'. */
    static const uint8_t cfg_blob[] = {'P', 'B', 'A', '1', 1, 2, 3, 4, 5, 6, 7, 8};
    static const uint8_t wiring_blob[] = {9, 8, 7, 6, 5, 4, 3};
    nvs_sim_put_blob("cfg", "pb_ble_auth", "cred", cfg_blob, sizeof(cfg_blob));
    nvs_sim_put_blob("cfg", "power_base_cfg", "blob", wiring_blob, sizeof(wiring_blob));

    hub_init(EUI_A, &cred);
    g_hub.command_pending = true;
    events_reset();
    g_state_at_accepted = g_state_at_done = g_state_at_erase = g_state_at_verified = 0;
    g_key_present_at_done = false;
    g_dataset_at_done = false;
    g_receipt_at_done = g_receipt_at_erase = g_receipt_at_verified = true;
    g_on_request = observe_release;

    pb_mgmt_client_t c;
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    pb_mgmt_client_load(&c);
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_COMMAND_PENDING && stored_state(EUI_A) == 10 &&
              strchr(g_events, 'Q') == NULL,
          "first poll: the hub only says a command is waiting; nothing changes on the board");
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_UNENROLLED, "second poll (echoing the hub's nonce): the command is carried out");
    g_on_request = NULL;

    CHECK(strcmp(g_events, "PPQADEV") == 0,
          "order: poll, poll, stop services, accepted, released notice, network erase, store read back");
    CHECK(g_state_at_accepted == 20, "when UNENROLL_ACCEPTED is sent the record is UNENROLLING (journaled), not released");
    CHECK(g_state_at_done == 31 && g_key_present_at_done && g_dataset_at_done,
          "when the released notice is sent the record is RELEASED with the erase PENDING: the Dataset is still "
          "stored (the notice can still be answered) and nothing has been erased");
    CHECK(g_hub.done_released == 1 && g_hub.done_erased == 0 && strchr(g_events, 'Z') == NULL,
          "what goes to the hub over Thread says 'released' -- the message that says 'erased' is never put on the "
          "network");
    CHECK(!g_receipt_at_done && !g_receipt_at_erase && g_state_at_erase == 31,
          "no receipt exists before the erase: not when the notice is sent, not when the erase starts");
    CHECK(!g_receipt_at_verified && g_state_at_verified == 31,
          "nor right after the store was read back empty: the receipt appears with the final record, not before");
    CHECK(g_hub.accepted == 1 && g_hub.done == 1 && c.notice_attempts == 1 && c.notice_acked,
          "one notice was enough: the hub's acknowledgement of exactly that notice verified");
    CHECK(stored_state(EUI_A) == 36, "final record: RELEASED, erased, notice acknowledged");
    CHECK(!dataset_present(), "the stored network (Dataset) is gone");

    pb_enroll_record_t rec;
    CHECK(pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_VALID && pb_enroll_record_is_final_release(&rec) &&
              !rec.has_bridge_endpoint && pb_mgmt_is_zero((const uint8_t *)&rec.id, sizeof(rec.id)) &&
              pb_mgmt_is_zero(rec.bridge_endpoint.addr, 16) && rec.bridge_endpoint.port == 0 &&
              pb_mgmt_is_zero(rec.mgmt.key, 32) && pb_mgmt_is_zero(rec.ack_mac, 32),
          "the final record names no network and no endpoint and holds no key");
    CHECK(memcmp(rec.mgmt.hub_id, cred.hub_id, 16) == 0 && memcmp(rec.mgmt.generation, cred.generation, 16) == 0 &&
              memcmp(rec.command_id, g_hub.command_id, 16) == 0 && !pb_mgmt_is_zero(rec.done_mac, 32),
          "what it keeps is the receipt: which hub, which enrollment, which command, and the MAC");

    /* The receipt: the one message that says "erased", made after the erase
     * was verified, checkable only by the hub that issued the key. */
    uint8_t receipt[PB_MGMT_RECEIPT_LEN];
    pb_mgmt_msg_t rm;
    CHECK(receipt_load(EUI_A, receipt), "after the final record there is a receipt");
    CHECK(pb_mgmt_parse(receipt, sizeof(receipt), &rm) && rm.type == PB_MGMT_TYPE_UNENROLL_DONE &&
              rm.result == PB_MGMT_RESULT_ERASED && memcmp(rm.id.eui64, EUI_A, 8) == 0 &&
              memcmp(rm.id.hub_id, cred.hub_id, 16) == 0 && memcmp(rm.id.generation, cred.generation, 16) == 0 &&
              memcmp(rm.command_id, g_hub.command_id, 16) == 0,
          "it is UNENROLL_DONE with result ERASED for this board, hub, generation and command");
    CHECK(pb_mgmt_verify(receipt, sizeof(receipt), cred.key, test_hmac, NULL),
          "and its MAC verifies with the enrollment's management key (which the board no longer has)");
    uint8_t other_key[32];
    memcpy(other_key, cred.key, 32);
    other_key[0] ^= 1;
    CHECK(!pb_mgmt_verify(receipt, sizeof(receipt), other_key, test_hmac, NULL), "and with no other key");
    uint8_t again[PB_MGMT_RECEIPT_LEN];
    CHECK(receipt_load(EUI_A, again) && memcmp(again, receipt, sizeof(receipt)) == 0,
          "reading it again gives the same bytes and changes nothing");
    CHECK(!receipt_load(EUI_B, again) && pb_mgmt_is_zero(again, sizeof(again)), "another board's identity gets none");
    uint8_t raw[PB_ENROLL_RECORD_MAX_LEN];
    size_t raw_len = raw_record(raw);
    bool key_bytes_in_flash = false;
    for (size_t i = 0; i + 32 <= raw_len; i++) {
        if (memcmp(&raw[i], cred.key, 32) == 0) {
            key_bytes_in_flash = true;
        }
    }
    CHECK(raw_len == PB_ENROLL_RECORD_V3_LEN && !key_bytes_in_flash, "the key's bytes are not in the stored record any more");
    CHECK(pb_mgmt_is_zero(c.rec.mgmt.key, 32) && !c.armed, "nor in the client's RAM");

    /* cfg is untouched. */
    nvs_handle_t h;
    uint8_t got[32];
    size_t len = sizeof(got);
    bool cred_ok = nvs_open_from_partition("cfg", "pb_ble_auth", NVS_READONLY, &h) == ESP_OK &&
                   nvs_get_blob(h, "cred", got, &len) == ESP_OK && len == sizeof(cfg_blob) &&
                   memcmp(got, cfg_blob, len) == 0;
    nvs_close(h);
    len = sizeof(got);
    bool wiring_ok = nvs_open_from_partition("cfg", "power_base_cfg", NVS_READONLY, &h) == ESP_OK &&
                     nvs_get_blob(h, "blob", got, &len) == ESP_OK && len == sizeof(wiring_blob) &&
                     memcmp(got, wiring_blob, len) == 0;
    nvs_close(h);
    CHECK(cred_ok && wiring_ok, "cfg is untouched: the setup credential and the wiring configuration are intact");

    /* The board afterwards: not enrolled, nothing to poll, nothing pending. */
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    CHECK(pb_mgmt_client_load(&c) == PB_MGMT_BOOT_NOTHING && pb_mgmt_client_poll(&c) == PB_MGMT_POLL_NOT_ARMED,
          "afterwards: nothing to do for management, and no poll is sent");
    pb_enroll_network_id_t marker;
    CHECK(pb_enroll_marker_load(EUI_A, &marker) == PB_ENROLL_MARKER_ABSENT, "and there is no enrollment marker");
}

static void scenario_rejected_commands(void)
{
    printf("\n-- commands the board must not obey --\n");
    static const struct {
        mutation_t mutation;
        const char *what;
    } cases[] = {
        {MUT_OTHER_BOARD, "a correctly signed command for another board's EUI-64"},
        {MUT_OTHER_HUB, "a correctly signed command naming another hub"},
        {MUT_OTHER_GENERATION, "a command for another enrollment generation, signed with the current key"},
        {MUT_OLD_ENROLLMENT, "a command of an earlier enrollment (its own generation and key)"},
        {MUT_WRONG_KEY, "a command signed with another key"},
        {MUT_FLIP_BODY_BYTE, "a command with one changed byte (the command id)"},
        {MUT_FLIP_MAC_BYTE, "a command with one changed MAC byte"},
        {MUT_WRONG_TYPE, "a signed RESULT_ACK in place of the answer"},
        {MUT_TRUNCATED, "a command one byte short"},
        {MUT_NO_ANSWER, "no answer at all"},
        {MUT_REJECTED, "a 4.xx answer (a hub that does not know the resource or the credential)"},
    };
    pb_enroll_mgmt_credential_t cred = golden_credential();
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        flash_enrolled(EUI_A, &cred);
        hub_init(EUI_A, &cred);
        g_hub.command_pending = true;
        g_hub.skip_nonce_round = true; /* the forged answer carries the command at once */
        g_hub.mutate_poll = cases[i].mutation;
        events_reset();
        uint8_t before[PB_ENROLL_RECORD_MAX_LEN], after[PB_ENROLL_RECORD_MAX_LEN];
        size_t before_len = raw_record(before);
        pb_mgmt_client_t c;
        pb_mgmt_client_init(&c, &k_ops, EUI_A);
        pb_mgmt_client_load(&c);
        pb_mgmt_poll_result_t r1 = pb_mgmt_client_poll(&c);
        pb_mgmt_poll_result_t r2 = pb_mgmt_client_poll(&c);
        size_t after_len = raw_record(after);
        bool untouched = before_len == after_len && memcmp(before, after, before_len) == 0 && dataset_present() &&
                         strchr(g_events, 'Q') == NULL && strchr(g_events, 'E') == NULL &&
                         strchr(g_events, 'D') == NULL && c.armed;
        CHECK(r1 == PB_MGMT_POLL_NO_ANSWER && r2 == PB_MGMT_POLL_NO_ANSWER && untouched, cases[i].what);
    }

    /* The same board still obeys the genuine command afterwards. */
    hub_init(EUI_A, &cred);
    g_hub.command_pending = true;
    pb_mgmt_client_t c;
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    pb_mgmt_client_load(&c);
    pb_mgmt_client_poll(&c);
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_UNENROLLED, "(the genuine command is still obeyed afterwards)");
}

static void scenario_replay(void)
{
    printf("\n-- replayed commands --\n");
    pb_enroll_mgmt_credential_t cred = golden_credential();

    /* A genuine UNENROLL is captured in one run ... */
    flash_enrolled(EUI_A, &cred);
    hub_init(EUI_A, &cred);
    g_hub.command_pending = true;
    pb_mgmt_client_t c;
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    pb_mgmt_client_load(&c);
    pb_mgmt_client_poll(&c);
    pb_mgmt_client_poll(&c);
    uint8_t captured[PB_MGMT_MAX_MESSAGE_LEN];
    size_t captured_len = g_hub.saved_len;
    memcpy(captured, g_hub.saved, captured_len);
    CHECK(captured_len == PB_MGMT_UNENROLL_LEN && pb_mgmt_verify(captured, captured_len, cred.key, test_hmac, NULL),
          "a genuine, correctly signed UNENROLL was captured");

    /* ... and played to the same board after it was enrolled again with the
     * SAME credential (which a real hub never reissues -- the worst case
     * for the board) in a new boot: the nonce is another one. */
    flash_enrolled(EUI_A, &cred);
    hub_init(EUI_A, &cred);
    memcpy(g_hub.saved, captured, captured_len);
    g_hub.saved_len = captured_len;
    g_hub.mutate_poll = MUT_STALE_NONCE;
    events_reset();
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    pb_mgmt_client_load(&c);
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_NO_ANSWER && pb_mgmt_client_poll(&c) == PB_MGMT_POLL_NO_ANSWER &&
              stored_state(EUI_A) == 10 && strchr(g_events, 'Q') == NULL && c.unauthentic_answers == 2,
          "replayed after a reboot: refused (it answers a nonce of an earlier poll), the board stays enrolled");

    /* Within one boot: the answer to poll N played as the answer to poll N+1. */
    hub_init(EUI_A, &cred);
    g_hub.command_pending = false;
    g_hub.mutate_poll = MUT_STALE_NONCE;
    memcpy(g_hub.saved, captured, captured_len);
    g_hub.saved_len = captured_len;
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_NO_ANSWER && stored_state(EUI_A) == 10,
          "replayed within the same boot: refused as well");

    /* A new enrollment: new generation and key. The old command means nothing. */
    pb_enroll_mgmt_credential_t next = cred;
    seq(next.generation, 16, 0x70);
    seq(next.key, 32, 0x90);
    flash_enrolled(EUI_A, &next);
    hub_init(EUI_A, &next);
    memcpy(g_hub.saved, captured, captured_len);
    g_hub.saved_len = captured_len;
    g_hub.mutate_poll = MUT_STALE_NONCE;
    events_reset();
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    pb_mgmt_client_load(&c);
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_NO_ANSWER && stored_state(EUI_A) == 10 && dataset_present(),
          "the earlier enrollment's command does not apply to the new enrollment");
    /* And a poll made with the OLD credential is not the new enrollment's. */
    hub_init(EUI_A, &cred); /* a hub that still only knows the old generation */
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_NO_ANSWER && strchr(g_events, '!') != NULL,
          "and the hub refuses a poll made with a credential it does not hold for this board");
}

static void scenario_response_loss(void)
{
    printf("\n-- lost answers and bounded retries --\n");
    pb_enroll_mgmt_credential_t cred = golden_credential();
    pb_mgmt_client_t c;
    static const uint32_t expected_backoff[] = PB_MGMT_PROOF_BACKOFF_MS;

    /* The first three notices get no answer, the fourth is acknowledged. */
    flash_enrolled(EUI_A, &cred);
    hub_init(EUI_A, &cred);
    g_hub.command_pending = true;
    g_hub.done_drop = 3;
    events_reset();
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    pb_mgmt_client_load(&c);
    pb_mgmt_client_poll(&c);
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_UNENROLLED && g_hub.done == 4 && c.notice_attempts == 4,
          "three lost answers: the same released notice is sent four times");
    CHECK(g_delay_count == 3 && g_delays[0] == expected_backoff[0] && g_delays[1] == expected_backoff[1] &&
              g_delays[2] == expected_backoff[2],
          "with the documented waits before the second, third and fourth attempt");
    CHECK(stored_state(EUI_A) == 36 && !dataset_present() && g_hub.done_erased == 0,
          "then: network erased and verified, final record (notice acknowledged); nothing but notices went out");

    /* No answer ever: bounded, and the board is released all the same. */
    flash_enrolled(EUI_A, &cred);
    hub_init(EUI_A, &cred);
    g_hub.command_pending = true;
    g_hub.done_drop = 1000;
    events_reset();
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    pb_mgmt_client_load(&c);
    pb_mgmt_client_poll(&c);
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_UNENROLLED && g_hub.done == PB_MGMT_PROOF_ATTEMPTS &&
              g_delay_count == PB_MGMT_PROOF_ATTEMPTS - 1,
          "no answer at all: exactly the bounded number of attempts, then it stops");
    bool backoff_ok = true;
    for (size_t i = 0; i < g_delay_count; i++) {
        backoff_ok = backoff_ok && g_delays[i] == expected_backoff[i];
    }
    CHECK(backoff_ok, "the waits are the documented backoff, in order");
    CHECK(stored_state(EUI_A) == 34 && !c.notice_acked && !dataset_present(),
          "the board still erases its network and writes the final record; the 'notice acknowledged' flag is clear");
    CHECK(receipt_available() && g_hub.done_erased == 0,
          "and it holds its receipt for a client to collect -- the hub has heard nothing, and nothing claimed it did");

    /* The hub refuses the notice (4.xx): no point repeating it. */
    flash_enrolled(EUI_A, &cred);
    hub_init(EUI_A, &cred);
    g_hub.command_pending = true;
    g_hub.done_reject = true;
    events_reset();
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    pb_mgmt_client_load(&c);
    pb_mgmt_client_poll(&c);
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_UNENROLLED && g_hub.done == 1 && stored_state(EUI_A) == 34,
          "a refused notice is not sent again; the release goes on to the erase without an acknowledgement");

    /* An answer that is not the expected acknowledgement is no acknowledgement. */
    flash_enrolled(EUI_A, &cred);
    hub_init(EUI_A, &cred);
    g_hub.command_pending = true;
    g_hub.done_bad_ack = true;
    events_reset();
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    pb_mgmt_client_load(&c);
    pb_mgmt_client_poll(&c);
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_UNENROLLED && g_hub.done == PB_MGMT_PROOF_ATTEMPTS &&
              stored_state(EUI_A) == 34 && !c.notice_acked,
          "a forged acknowledgement (one changed byte) is never taken for the hub's");

    /* A genuine acknowledgement -- right key, right command -- of the OTHER
     * result ("erased") does not acknowledge the "released" notice. */
    flash_enrolled(EUI_A, &cred);
    hub_init(EUI_A, &cred);
    g_hub.command_pending = true;
    g_hub.done_ack_other_result = true;
    events_reset();
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    pb_mgmt_client_load(&c);
    pb_mgmt_client_poll(&c);
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_UNENROLLED && g_hub.done == PB_MGMT_PROOF_ATTEMPTS &&
              stored_state(EUI_A) == 34 && !c.notice_acked,
          "an acknowledgement that names the other result is not this notice's acknowledgement");

    /* The command's own answer is lost: nothing happened, the next poll round delivers it. */
    flash_enrolled(EUI_A, &cred);
    hub_init(EUI_A, &cred);
    g_hub.command_pending = true;
    events_reset();
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    pb_mgmt_client_load(&c);
    pb_mgmt_client_poll(&c);
    g_hub.mutate_poll = MUT_NO_ANSWER;
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_NO_ANSWER && stored_state(EUI_A) == 10,
          "the answer carrying the command is lost: the board is unchanged");
    g_hub.mutate_poll = MUT_NONE;
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_COMMAND_PENDING && pb_mgmt_client_poll(&c) == PB_MGMT_POLL_UNENROLLED,
          "the next two polls deliver the same command");
}

static void scenario_write_failures(void)
{
    printf("\n-- flash write failures --\n");
    pb_enroll_mgmt_credential_t cred = golden_credential();
    pb_mgmt_client_t c;

    /* The journal cannot be written. */
    flash_enrolled(EUI_A, &cred);
    hub_init(EUI_A, &cred);
    g_hub.command_pending = true;
    events_reset();
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    pb_mgmt_client_load(&c);
    pb_mgmt_client_poll(&c);
    nvs_sim_force_next_commit_error(ESP_FAIL);
    /* What a failed commit leaves behind is unspecified on real NVS (the old
     * record or the journaled one); both are states the next boot handles.
     * What must hold: the release did not go on. */
    int left = 0;
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_UNENROLL_INTERRUPTED && ((left = stored_state(EUI_A)) == 10 || left == 20) &&
              g_hub.accepted == 0 && g_hub.done == 0 && dataset_present() && strchr(g_events, 'E') == NULL,
          "journal write fails: the release stops there -- no acceptance, no proof, network kept, never RELEASED");

    /* The network cannot be erased: NOT an unenroll that finished. The
     * record stays "erase pending", there is no receipt, and the caller is
     * told so. The next boot does the step again. Never resumed in between. */
    flash_enrolled(EUI_A, &cred);
    hub_init(EUI_A, &cred);
    g_hub.command_pending = true;
    events_reset();
    g_erase_fails = true;
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    pb_mgmt_client_load(&c);
    pb_mgmt_client_poll(&c);
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_RELEASED_NOT_ERASED,
          "network erase fails: the poll does NOT report an unenroll -- the caller is told the erase is not done");
    CHECK(stored_state(EUI_A) == 31 && dataset_present() && !receipt_available() && g_hub.done_erased == 0 &&
              strchr(g_events, 'V') == NULL,
          "the record stays RELEASED with the erase pending, the Dataset is still stored, and no receipt exists");
    CHECK(g_hub.done_released == 1, "(the hub was told 'released' -- which is true -- and nothing more)");
    g_erase_fails = false;
    hub_init(EUI_A, &cred);
    events_reset();
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    CHECK(pb_mgmt_client_load(&c) == PB_MGMT_BOOT_RELEASE_PENDING, "next boot: a release is pending");
    CHECK(pb_mgmt_client_resume_release(&c) == PB_MGMT_RELEASE_FINAL && strcmp(g_events, "UDEV") == 0 &&
              stored_state(EUI_A) == 36 && !dataset_present() && receipt_available(),
          "it brings the network up for the notice only, sends it, erases, reads the store back, and is final");
    CHECK(strchr(g_events, 'Q') == NULL, "(no service existed at boot, so none was stopped)");

    /* The erase returns no error but the store is not empty afterwards
     * (ESP-IDF's wipe ignores the result of its nvs_erase_all()). The
     * read-back catches it. */
    flash_enrolled(EUI_A, &cred);
    hub_init(EUI_A, &cred);
    g_hub.command_pending = true;
    events_reset();
    g_erase_lies = true;
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    pb_mgmt_client_load(&c);
    pb_mgmt_client_poll(&c);
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_RELEASED_NOT_ERASED && strcmp(g_events, "PPQADEV") == 0,
          "erase 'succeeds' without erasing: the store is read back, found not empty, and the erase does not count");
    CHECK(stored_state(EUI_A) == 31 && dataset_present() && !receipt_available(),
          "erase pending, Dataset stored, no receipt -- exactly as after a failed erase");
    events_reset();
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    pb_mgmt_client_load(&c);
    CHECK(pb_mgmt_client_resume_release(&c) == PB_MGMT_RELEASE_INCOMPLETE && stored_state(EUI_A) == 31 &&
              !receipt_available(),
          "and a next boot on which it is still not empty is still not final, however often it is tried");
    g_erase_lies = false;
    hub_init(EUI_A, &cred);
    events_reset();
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    pb_mgmt_client_load(&c);
    CHECK(pb_mgmt_client_resume_release(&c) == PB_MGMT_RELEASE_FINAL && stored_state(EUI_A) == 36 && receipt_available(),
          "once the store really is empty the release becomes final");
}

/* ======================= scenarios: power loss ======================= */

static char g_flash[64];

enum {
    CRASH_NONE = 0,
    CRASH_IN_QUIESCE,      /* services stopped, nothing written */
    CRASH_AT_ACCEPTED,     /* journal written */
    CRASH_AT_FIRST_PROOF,  /* release written, released notice not delivered */
    CRASH_BEFORE_ERASE,    /* notice delivered (or given up), network not erased */
    CRASH_AFTER_ERASE,     /* network erased and read back empty, final record not written */
    /* The receipt's delivery session (k_ops_session only). */
    CRASH_AT_SESSION,      /* final record stored, the session about to start */
    CRASH_AFTER_ATTACH,    /* in the session, attached: nothing read back or sent yet */
    CRASH_AT_RECEIPT,      /* in the session, the receipt on its way to the hub */
    CRASH_AT_SESSION_END,  /* the receipt was sent, the session about to be taken down */
};
/* How a child that lost power exits: the second when the hub had by then
 * completed the removal (taken a receipt). */
#define POWER_LOST 77
#define POWER_LOST_HUB_COMPLETED 78
static int g_crash_at;
/* A flash commit that fails (instead of a power cut) at one of the same
 * points: 0 = none. */
static int g_fail_commit_at;

static void crash_hook(uint8_t type)
{
    if ((g_crash_at == CRASH_IN_QUIESCE && type == 'Q') ||
        (g_crash_at == CRASH_AT_ACCEPTED && type == PB_MGMT_TYPE_UNENROLL_ACCEPTED) ||
        (g_crash_at == CRASH_AT_FIRST_PROOF && type == PB_MGMT_TYPE_UNENROLL_DONE) ||
        (g_crash_at == CRASH_BEFORE_ERASE && type == 'E') || (g_crash_at == CRASH_AFTER_ERASE && type == 'e') ||
        (g_crash_at == CRASH_AT_SESSION && type == 'S') || (g_crash_at == CRASH_AFTER_ATTACH && type == 'a') ||
        (g_crash_at == CRASH_AT_RECEIPT && type == 'Z') || (g_crash_at == CRASH_AT_SESSION_END && type == 's')) {
        _exit(g_hub.completed ? POWER_LOST_HUB_COMPLETED : POWER_LOST);
    }
    /* The NEXT commit after this point fails: after ACCEPTED that is the
     * release record, after the verified erase it is the final record. */
    if ((g_fail_commit_at == CRASH_AT_ACCEPTED && type == PB_MGMT_TYPE_UNENROLL_ACCEPTED) ||
        (g_fail_commit_at == CRASH_AFTER_ERASE && type == 'e')) {
        nvs_sim_force_next_commit_error(ESP_FAIL);
    }
}

/* ---- the real enrollment controller's boot, to see whether it resumes ---- */

static bool g_thread_started;

static esp_err_t f_joiner_start(const char *pskd, uint32_t attempt, void *ctx)
{
    (void)pskd;
    (void)attempt;
    (void)ctx;
    return ESP_OK;
}
static void f_joiner_stop(void *ctx)
{
    (void)ctx;
}
static esp_err_t f_thread_start_stored(bool require_complete, void *ctx)
{
    (void)require_complete;
    (void)ctx;
    if (!dataset_present()) {
        return ESP_FAIL;
    }
    g_thread_started = true;
    return ESP_OK;
}
static void f_thread_stop(void *ctx)
{
    (void)ctx;
}
static bool f_read_dataset(pb_enroll_dataset_info_t *out, void *ctx)
{
    (void)ctx;
    if (!dataset_present()) {
        return false;
    }
    out->has_network_key = true;
    out->complete = true;
    out->id = NET_OTBR;
    return true;
}
static const pb_enroll_ops_t k_enroll_ops = {
    .joiner_start = f_joiner_start,
    .joiner_stop = f_joiner_stop,
    .thread_start_stored = f_thread_start_stored,
    .thread_stop = f_thread_stop,
    .read_dataset = f_read_dataset,
    .ctx = NULL,
};

/* What goes wrong in the next in_child() boot, from its very start (the
 * parent sets it; the child inherits it). */
enum {
    BOOT_ERASES_LEAVE_NETWORK_KEY = 1, /* every erase removes the Dataset and leaves the Network Key */
    BOOT_STORE_UNREADABLE = 2,         /* no read of the store succeeds */
    BOOT_DIRECT_ERASE_FAILS = 4,       /* the erase before OpenThread starts returns an error */
    BOOT_POWER_CUT_AFTER_PRESTART = 8, /* power lost after that erase, before OpenThread starts */
};
static int g_boot_faults;

static int in_child(int (*fn)(int), int arg)
{
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        if (!nvs_sim_set_backing_file(g_flash)) {
            _exit(91);
        }
        g_erases_leave_network_key = (g_boot_faults & BOOT_ERASES_LEAVE_NETWORK_KEY) != 0;
        g_store_unreadable = g_readback_fails = (g_boot_faults & BOOT_STORE_UNREADABLE) != 0;
        g_direct_erase_fails = (g_boot_faults & BOOT_DIRECT_ERASE_FAILS) != 0;
        g_power_cut_after_prestart = (g_boot_faults & BOOT_POWER_CUT_AFTER_PRESTART) != 0;
        platform_start(); /* a boot */
        _exit(fn(arg));
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 99;
}

/* Child: a board that is enrolled and polled twice with a command pending,
 * losing power at `crash_at`. Exit 50 + poll result when it did not. */
static int boot_poll_twice(int crash_at)
{
    pb_enroll_mgmt_credential_t cred = golden_credential();
    hub_init(EUI_A, &cred);
    g_hub.command_pending = true;
    g_crash_at = crash_at;
    g_on_request = crash_hook;
    pb_mgmt_client_t c;
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    if (pb_mgmt_client_load(&c) != PB_MGMT_BOOT_ARMED) {
        return 92;
    }
    pb_mgmt_client_poll(&c);
    return 50 + (int)pb_mgmt_client_poll(&c);
}

/* Child: the same, but instead of losing power the flash commit after
 * `fail_at` fails. Exit 50 + poll result. */
static int boot_poll_twice_failing_commit(int fail_at)
{
    g_fail_commit_at = fail_at;
    return boot_poll_twice(CRASH_NONE);
}

/* Child: 1 when the board can hand out a receipt, 0 when not; +10 when the
 * Dataset is still stored. */
static int boot_receipt(int unused)
{
    (void)unused;
    return (receipt_available() ? 1 : 0) + (dataset_present() ? 10 : 0);
}

/* Child: 1 when this boot's store is "empty" in the sense of the erase's own
 * read-back. */
static int boot_store_empty(int unused)
{
    (void)unused;
    return store_empty() ? 1 : 0;
}

/* Child: what the next boot finds and whether the enrollment controller
 * would resume it. Exit: resumed*100 + mgmt_state*10 + release flags. */
static int boot_report(int unused)
{
    (void)unused;
    g_thread_started = false;
    pb_enroll_ctl_t ctl;
    pb_enroll_ctl_init(&ctl, &k_enroll_ops, EUI_A);
    pb_enroll_state_t state = pb_enroll_ctl_boot(&ctl);
    bool resumed = state == PB_ENROLL_STATE_ENROLLED && g_thread_started;
    if (!resumed && (ctl.last != PB_ENROLL_RESULT_NONE || g_thread_started)) {
        return 93; /* a released board reports "not enrolled, nothing wrong", Thread untouched */
    }
    return (resumed ? 100 : 0) + stored_state(EUI_A);
}

/* Child: the boot path of the firmware -- finish a pending release. Exit
 * 60 + release result, +20 when the network is still stored afterwards. */
static int boot_finish_release(int crash_at)
{
    pb_enroll_mgmt_credential_t cred = golden_credential();
    hub_init(EUI_A, &cred);
    g_crash_at = crash_at;
    g_on_request = crash_hook;
    pb_mgmt_client_t c;
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    if (pb_mgmt_client_load(&c) != PB_MGMT_BOOT_RELEASE_PENDING) {
        return 94;
    }
    int result = (int)pb_mgmt_client_resume_release(&c);
    return 60 + result + (dataset_present() ? 20 : 0);
}

static int seed_enrolled(int unused)
{
    (void)unused;
    pb_enroll_mgmt_credential_t cred = golden_credential();
    pb_enroll_record_t rec = enrolled_record(&cred);
    if (pb_enroll_record_store(EUI_A, &rec) != ESP_OK) {
        return 95;
    }
    dataset_store();
    return 0;
}

static void flash_file_reset(void)
{
    g_boot_faults = 0;
    unlink(g_flash);
    CHECK(in_child(seed_enrolled, 0) == 0, "(a board enrolled with a management credential, with its Dataset stored)");
}

static void scenario_power_loss(void)
{
    printf("\n-- power loss between every stored step, and what each next boot does --\n");
    snprintf(g_flash, sizeof(g_flash), "/tmp/pb_mgmt_flash_%d.bin", (int)getpid());

    flash_file_reset();
    CHECK(in_child(boot_report, 0) == 110, "baseline: the next boot resumes the enrollment (record ENROLLED)");

    /* 1. Services stopped, nothing written yet. */
    flash_file_reset();
    CHECK(in_child(boot_poll_twice, CRASH_IN_QUIESCE) == 77, "power lost after the services were stopped, before any write");
    CHECK(in_child(boot_report, 0) == 110, "next boot: still enrolled and resumed -- the hub will give the command again");
    CHECK(in_child(boot_poll_twice, CRASH_NONE) == 50 + PB_MGMT_POLL_UNENROLLED, "and then it is carried out");
    CHECK(in_child(boot_report, 0) == 36, "after which the board is released for good (erase verified, final record)");

    /* 2. Journal written. */
    flash_file_reset();
    CHECK(in_child(boot_poll_twice, CRASH_AT_ACCEPTED) == 77, "power lost right after the command was journaled");
    CHECK(in_child(boot_report, 0) == 20, "next boot: UNENROLLING -- NOT resumed, Thread untouched, although the Dataset is stored");
    CHECK(in_child(boot_receipt, 0) == 10, "no receipt (and the Dataset is still stored)");
    CHECK(in_child(boot_finish_release, CRASH_NONE) == 60 + PB_MGMT_RELEASE_FINAL,
          "the boot path stores the release, sends the notice, erases the network and checks it");
    CHECK(in_child(boot_report, 0) == 36, "final: RELEASED, erased, notice acknowledged; not resumed");
    CHECK(in_child(boot_receipt, 0) == 1, "and now there is a receipt, and no Dataset");

    /* 3. Release written, notice not delivered. */
    flash_file_reset();
    CHECK(in_child(boot_poll_twice, CRASH_AT_FIRST_PROOF) == 77, "power lost after the release was stored, before the notice went out");
    CHECK(in_child(boot_report, 0) == 31, "next boot: RELEASED with the erase pending -- not resumed");
    CHECK(in_child(boot_receipt, 0) == 10, "no receipt");
    CHECK(in_child(boot_finish_release, CRASH_NONE) == 60 + PB_MGMT_RELEASE_FINAL, "the boot path sends the notice and finishes");
    CHECK(in_child(boot_report, 0) == 36, "final: RELEASED, erased, notice acknowledged");

    /* 4. Notice delivered, network not yet erased. */
    flash_file_reset();
    CHECK(in_child(boot_poll_twice, CRASH_BEFORE_ERASE) == 77, "power lost after the notice, before the network erase");
    CHECK(in_child(boot_report, 0) == 31, "next boot: RELEASED, erase pending (the acknowledgement was only in RAM) -- not resumed");
    CHECK(in_child(boot_receipt, 0) == 10, "no receipt: the hub was told 'released', and that is all it can have been told");
    CHECK(in_child(boot_finish_release, CRASH_NONE) == 60 + PB_MGMT_RELEASE_FINAL, "the notice is sent again (the hub takes it again) and the release finishes");
    CHECK(in_child(boot_report, 0) == 36, "final: RELEASED, erased, notice acknowledged");

    /* 5. Network erased and read back empty, final record not written. */
    flash_file_reset();
    CHECK(in_child(boot_poll_twice, CRASH_AFTER_ERASE) == 77, "power lost after the verified erase, before the final record");
    CHECK(in_child(boot_report, 0) == 31, "next boot: RELEASED, erase pending, no Dataset -- not resumed");
    CHECK(in_child(boot_receipt, 0) == 0, "no receipt yet although the Dataset is gone: a receipt is only ever in a final record");
    CHECK(in_child(boot_finish_release, CRASH_NONE) == 60 + PB_MGMT_RELEASE_FINAL, "no network to send on: the erase is done and checked again and the release finishes");
    CHECK(in_child(boot_report, 0) == 34, "final: RELEASED, erased (the 'notice acknowledged' flag is clear -- it is not claimed twice)");
    CHECK(in_child(boot_receipt, 0) == 1, "and the receipt exists");

    /* 6. Power lost again and again inside the boot path itself. */
    flash_file_reset();
    CHECK(in_child(boot_poll_twice, CRASH_AT_ACCEPTED) == 77, "journaled, power lost");
    CHECK(in_child(boot_finish_release, CRASH_AT_FIRST_PROOF) == 77, "boot path: power lost after it stored the release");
    CHECK(in_child(boot_report, 0) == 31, "next boot: RELEASED, erase pending");
    CHECK(in_child(boot_finish_release, CRASH_BEFORE_ERASE) == 77, "boot path: power lost before the network erase");
    CHECK(in_child(boot_report, 0) == 31, "next boot: still RELEASED, erase pending -- never ENROLLED again");
    CHECK(in_child(boot_finish_release, CRASH_AFTER_ERASE) == 77, "boot path: power lost after the verified erase");
    CHECK(in_child(boot_report, 0) == 31 && in_child(boot_receipt, 0) == 0, "next boot: erase pending, no receipt");
    CHECK(in_child(boot_finish_release, CRASH_NONE) == 60 + PB_MGMT_RELEASE_FINAL, "and the next boot finishes it");
    CHECK(in_child(boot_report, 0) == 34, "final");
    CHECK(in_child(boot_report, 0) == 34 && in_child(boot_report, 0) == 34 && in_child(boot_receipt, 0) == 1,
          "and every boot after that: released, not resumed, nothing rewritten, the same receipt");

    /* 7. Not a power cut but a failed write, at the two release writes. What
     * is in flash afterwards is what a reboot finds. */
    flash_file_reset();
    CHECK(in_child(boot_poll_twice_failing_commit, CRASH_AT_ACCEPTED) == 50 + PB_MGMT_POLL_UNENROLL_INTERRUPTED,
          "the release record cannot be written: the poll reports an interrupted unenroll, not a release");
    CHECK(in_child(boot_report, 0) == 20 && in_child(boot_receipt, 0) == 10,
          "flash still holds the journal (UNENROLLING), the Dataset, and no receipt");
    CHECK(in_child(boot_finish_release, CRASH_NONE) == 60 + PB_MGMT_RELEASE_FINAL && in_child(boot_report, 0) == 36,
          "the next boot carries the release through");

    flash_file_reset();
    CHECK(in_child(boot_poll_twice_failing_commit, CRASH_AFTER_ERASE) == 50 + PB_MGMT_POLL_RELEASED_NOT_ERASED,
          "the FINAL record cannot be written after a verified erase: the poll does not report an unenroll");
    CHECK(in_child(boot_report, 0) == 31 && in_child(boot_receipt, 0) == 0,
          "flash says 'erase pending' (the Dataset is in fact gone) and holds no receipt: nothing claims the erase");
    CHECK(in_child(boot_finish_release, CRASH_NONE) == 60 + PB_MGMT_RELEASE_FINAL && in_child(boot_report, 0) == 34 &&
              in_child(boot_receipt, 0) == 1,
          "the next boot erases and checks again, writes the final record, and only then has a receipt");
    unlink(g_flash);
}

/* ======================= scenarios: the receipt's delivery session ======================= */

static bool key_in_flash(const uint8_t key[32])
{
    uint8_t raw[PB_ENROLL_RECORD_MAX_LEN];
    size_t raw_len = raw_record(raw);
    for (size_t i = 0; i + 32 <= raw_len; i++) {
        if (memcmp(&raw[i], key, 32) == 0) {
            return true;
        }
    }
    return false;
}

static int g_state_at_session, g_state_at_receipt;
static bool g_receipt_at_session, g_dataset_at_session, g_dataset_at_receipt, g_key_in_flash_at_receipt;
static uint8_t g_board_receipt[PB_MGMT_RECEIPT_LEN];

static void observe_session(uint8_t type)
{
    if (type == 'S') {
        g_state_at_session = stored_state(EUI_A);
        g_receipt_at_session = receipt_available();
        g_dataset_at_session = dataset_present();
    } else if (type == 'Z' && g_state_at_receipt == 0) {
        pb_enroll_mgmt_credential_t cred = golden_credential();
        g_state_at_receipt = stored_state(EUI_A);
        g_dataset_at_receipt = dataset_present();
        g_key_in_flash_at_receipt = key_in_flash(cred.key);
        (void)receipt_load(EUI_A, g_board_receipt);
    }
}

/* An enrolled board on a platform with the delivery session, and a hub with
 * a removal waiting for it. */
static void session_start(const pb_enroll_mgmt_credential_t *cred, bool hub_knows_hello)
{
    flash_enrolled(EUI_A, cred);
    hub_init(EUI_A, cred);
    g_hub.knows_hello = hub_knows_hello;
    g_hub.command_pending = true;
    events_reset();
    session_reset();
    g_erase_fails = g_erase_lies = false;
    g_on_request = NULL;
    g_state_at_session = g_state_at_receipt = 0;
}

/* Both polls of an unenroll, on that platform. */
static pb_mgmt_poll_result_t session_unenroll(pb_mgmt_client_t *c)
{
    pb_mgmt_client_init(c, &k_ops_session, EUI_A);
    pb_mgmt_client_load(c);
    pb_mgmt_client_poll(c);
    return pb_mgmt_client_poll(c);
}

static void scenario_receipt_delivered(void)
{
    printf("\n-- with the delivery session: the board hands its receipt to the hub itself, after the erase --\n");
    pb_enroll_mgmt_credential_t cred = golden_credential();
    session_start(&cred, true);
    g_on_request = observe_session;

    pb_mgmt_client_t c;
    pb_mgmt_client_init(&c, &k_ops_session, EUI_A);
    pb_mgmt_client_load(&c);
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_COMMAND_PENDING, "first poll: a command is waiting");
    CHECK(g_hub.hellos == 1 && g_hub.board_caps == PB_MGMT_BOARD_CAP_THREAD_RECEIPT && c.hello_done &&
              c.hub_capabilities == PB_MGMT_HUB_CAP_THREAD_RECEIPT && stored_state(EUI_A) == 10,
          "after the hub's first answer the board announced that it sends its receipt itself; nothing was stored");
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_UNENROLLED, "second poll: the command is carried out to its end");
    g_on_request = NULL;

    CHECK(strcmp(g_events, "PHPQADEVSvZs") == 0,
          "order: poll, hello, poll, stop services, accepted, released notice, erase, read back, session up, "
          "read back again, receipt, session down");
    CHECK(g_state_at_session == 36 && g_receipt_at_session && !g_dataset_at_session,
          "the session starts only after the final record is stored: the erase is verified, the receipt exists, "
          "no Dataset is in flash");
    CHECK(g_state_at_receipt == 36 && !g_dataset_at_receipt && !g_key_in_flash_at_receipt,
          "when the receipt goes out, neither the network nor the management key is in flash");
    pb_mgmt_msg_t rm;
    CHECK(g_hub.completed && g_hub.done_erased == 1 && pb_mgmt_parse(g_hub.receipt, sizeof(g_hub.receipt), &rm) &&
              rm.type == PB_MGMT_TYPE_UNENROLL_DONE && rm.result == PB_MGMT_RESULT_ERASED &&
              memcmp(rm.id.eui64, EUI_A, 8) == 0 && memcmp(rm.id.hub_id, cred.hub_id, 16) == 0 &&
              memcmp(rm.id.generation, cred.generation, 16) == 0 && memcmp(rm.command_id, g_hub.command_id, 16) == 0 &&
              pb_mgmt_verify(g_hub.receipt, sizeof(g_hub.receipt), cred.key, test_hmac, NULL),
          "what the hub got is UNENROLL_DONE/ERASED for this board, hub, generation and command, and it verifies");
    CHECK(memcmp(g_hub.receipt, g_board_receipt, PB_MGMT_RECEIPT_LEN) == 0,
          "byte for byte the receipt the board hands out over BLE");
    CHECK(c.receipt_acked && c.receipt_attempts == 1 && c.notice_acked,
          "one receipt was enough: the hub's acknowledgement of exactly the receipt verified");
    CHECK(g_session_ups == 1 && g_session_downs == 1, "the session was taken down again");
    uint8_t after[PB_MGMT_RECEIPT_LEN];
    CHECK(stored_state(EUI_A) == 36 && !dataset_present() && receipt_load(EUI_A, after) &&
              memcmp(after, g_board_receipt, sizeof(after)) == 0,
          "the delivery changed nothing that is stored: the final record and its receipt are as before");
    CHECK(pb_mgmt_is_zero(c.rec.mgmt.key, 32) && !c.armed, "and the key is gone from RAM as well");

    pb_mgmt_client_init(&c, &k_ops_session, EUI_A);
    CHECK(pb_mgmt_client_load(&c) == PB_MGMT_BOOT_NOTHING && pb_mgmt_client_poll(&c) == PB_MGMT_POLL_NOT_ARMED &&
              pb_mgmt_client_resume_release(&c) == PB_MGMT_RELEASE_NOTHING,
          "afterwards: nothing to do for management, nothing is sent again");

    printf("\n-- a platform without the session announces nothing --\n");
    session_start(&cred, true);
    g_hub.command_pending = false;
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    pb_mgmt_client_load(&c);
    pb_mgmt_client_poll(&c);
    pb_mgmt_client_poll(&c);
    CHECK(g_hub.hellos == 0 && strcmp(g_events, "PP") == 0 && !c.hello_done,
          "no HELLO: a board that cannot send its receipt has nothing to announce");
}

static void scenario_hello(void)
{
    printf("\n-- HELLO: once per boot, never in the way of a poll --\n");
    pb_enroll_mgmt_credential_t cred = golden_credential();
    pb_mgmt_client_t c;

    session_start(&cred, true);
    g_hub.command_pending = false;
    pb_mgmt_client_init(&c, &k_ops_session, EUI_A);
    pb_mgmt_client_load(&c);
    uint8_t before[PB_ENROLL_RECORD_MAX_LEN], after[PB_ENROLL_RECORD_MAX_LEN];
    size_t before_len = raw_record(before);
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_IDLE && pb_mgmt_client_poll(&c) == PB_MGMT_POLL_IDLE &&
              pb_mgmt_client_poll(&c) == PB_MGMT_POLL_IDLE && strcmp(g_events, "PHPP") == 0,
          "answered once: it is not sent again in this boot");
    CHECK(raw_record(after) == before_len && memcmp(before, after, before_len) == 0, "and it stores nothing");

    session_start(&cred, false);
    g_hub.command_pending = false;
    pb_mgmt_client_init(&c, &k_ops_session, EUI_A);
    pb_mgmt_client_load(&c);
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_IDLE && pb_mgmt_client_poll(&c) == PB_MGMT_POLL_IDLE &&
              strcmp(g_events, "P?P") == 0 && c.hello_done && c.hub_capabilities == 0,
          "a hub that predates HELLO refuses it like any unknown type: taken as 'no capabilities', not sent again, "
          "and the polls go on");

    session_start(&cred, true);
    g_hub.command_pending = false;
    g_hub.hello_drop = 99;
    pb_mgmt_client_init(&c, &k_ops_session, EUI_A);
    pb_mgmt_client_load(&c);
    for (int i = 0; i < 6; i++) {
        pb_mgmt_client_poll(&c);
    }
    CHECK(g_hub.hellos == PB_MGMT_HELLO_ATTEMPTS && g_hub.polls == 6 && !c.hello_done,
          "no answer: tried again with later polls, a bounded number of times; every poll still went out");

    session_start(&cred, true);
    g_hub.command_pending = false;
    g_hub.hello_drop = 1;
    pb_mgmt_client_init(&c, &k_ops_session, EUI_A);
    pb_mgmt_client_load(&c);
    pb_mgmt_client_poll(&c);
    pb_mgmt_client_poll(&c);
    pb_mgmt_client_poll(&c);
    CHECK(g_hub.hellos == 2 && c.hello_done && strcmp(g_events, "PHPHP") == 0, "lost once: the second one is answered");

    session_start(&cred, true);
    g_hub.command_pending = false;
    g_hub.hello_bad_ack = true;
    pb_mgmt_client_init(&c, &k_ops_session, EUI_A);
    pb_mgmt_client_load(&c);
    pb_mgmt_client_poll(&c);
    CHECK(!c.hello_done && c.hub_capabilities == 0, "an answer whose MAC does not verify is no answer");

    /* The poll right after an unanswered HELLO still fetches the command:
     * the hub's nonce is not used up by it. */
    session_start(&cred, true);
    g_hub.hello_drop = 1;
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_UNENROLLED && c.receipt_acked,
          "a lost HELLO between the two polls of a command does not cost the command");
}

static void scenario_receipt_retries(void)
{
    printf("\n-- the receipt's answer is lost, forged or refused --\n");
    pb_enroll_mgmt_credential_t cred = golden_credential();
    pb_mgmt_client_t c;

    session_start(&cred, true);
    g_hub.receipt_drop = 2;
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_UNENROLLED && c.receipt_acked && c.receipt_attempts == 3 &&
              g_hub.done_erased == 3 && !g_hub.receipt_differed,
          "two answers lost: the same bytes are sent again and the third is acknowledged");
    CHECK(g_delay_count == 2 && g_delays[0] == 2000 && g_delays[1] == 4000, "after 2 s and 4 s");
    CHECK(strcmp(g_events, "PHPQADEVSvZvZvZs") == 0,
          "all of it inside the one session, the store read back before each of the three");

    session_start(&cred, true);
    g_hub.receipt_drop = 99;
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_UNENROLLED && !c.receipt_acked &&
              c.receipt_attempts == PB_MGMT_RECEIPT_ATTEMPTS && g_hub.done_erased == PB_MGMT_RECEIPT_ATTEMPTS,
          "never answered: a bounded number of attempts, and the unenroll is still what it was -- complete on the board");
    CHECK(g_delay_count == 4 && g_delays[0] == 2000 && g_delays[1] == 4000 && g_delays[2] == 8000 && g_delays[3] == 16000,
          "with the backoff 2, 4, 8, 16 s");
    CHECK(stored_state(EUI_A) == 36 && receipt_available() && g_session_downs == 1,
          "the session ended and the receipt waits in the final record for a client");

    session_start(&cred, true);
    g_hub.receipt_bad_ack = true;
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_UNENROLLED && !c.receipt_acked &&
              c.receipt_attempts == PB_MGMT_RECEIPT_ATTEMPTS,
          "an acknowledgement whose MAC does not verify is not one");
    session_start(&cred, true);
    g_hub.receipt_ack_other_result = true;
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_UNENROLLED && !c.receipt_acked &&
              c.receipt_attempts == PB_MGMT_RECEIPT_ATTEMPTS,
          "a genuine acknowledgement of the released NOTICE is not an acknowledgement of the receipt");

    session_start(&cred, true);
    g_hub.receipt_reject = true;
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_UNENROLLED && !c.receipt_acked && c.receipt_attempts == 1 &&
              !g_hub.completed && g_delay_count == 0,
          "refused by the hub: not sent again");
    CHECK(stored_state(EUI_A) == 36 && receipt_available(), "the board is released and erased all the same, receipt kept");

    printf("\n-- a hub that predates HELLO and re-acknowledging --\n");
    session_start(&cred, false);
    g_hub.forgets_key_when_complete = true;
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_UNENROLLED && g_hub.completed && c.receipt_acked &&
              c.receipt_attempts == 1 && strcmp(g_events, "P?PQADEVSvZs") == 0,
          "it takes the receipt on the management resource and acknowledges it: completed without HELLO");
    session_start(&cred, false);
    g_hub.forgets_key_when_complete = true;
    g_hub.receipt_drop = 1;
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_UNENROLLED && g_hub.completed && !c.receipt_acked &&
              c.receipt_attempts == 2,
          "its answer lost: it completed, refuses the repeated receipt, and the board stops -- the hub has what "
          "it needs although the board cannot know");

    printf("\n-- no session --\n");
    session_start(&cred, true);
    g_session_attach_ok = false;
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_UNENROLLED && strcmp(g_events, "PHPQADEVSs") == 0 &&
              g_hub.done_erased == 0 && !c.receipt_acked && c.receipt_attempts == 0,
          "Thread does not attach: nothing is sent, the session is taken down, the unenroll is complete");
    CHECK(stored_state(EUI_A) == 36 && receipt_available() && !dataset_present(),
          "the receipt waits in the final record, as on a board without the session");

    /* The released notice is never the receipt, session or not. */
    session_start(&cred, true);
    g_hub.done_drop = 99;
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_UNENROLLED && !c.notice_acked && c.receipt_acked &&
              g_hub.done_released == PB_MGMT_PROOF_ATTEMPTS && g_hub.done_erased == 1,
          "the notice was never acknowledged, the receipt was: two different messages, two different answers");
    CHECK(stored_state(EUI_A) == 34, "the final record does not claim the notice was acknowledged");
}

/* Faults that begin when the session is taken down: what the erase that
 * follows a session meets. */
static void fail_erase_from_session_end(uint8_t type)
{
    if (type == 's') {
        g_erase_fails = true; /* the erase returns an error */
    }
}

static void erase_misses_flash_from_session_end(uint8_t type)
{
    if (type == 's') {
        g_erase_lies = true; /* the erase returns ESP_OK and erased nothing */
    }
}

static void nothing_can_be_written_from_session_end(uint8_t type)
{
    if (type == 's') {
        g_erase_fails = true;
        nvs_sim_inject(NVS_SIM_OP_SET_BLOB, NULL, 1, ESP_FAIL); /* nor can the record be put back */
    }
}

static void scenario_receipt_session_must_not_store(void)
{
    printf("\n-- a session that stored the network again: nothing is sent on it, and nothing claims the erase --\n");
    pb_enroll_mgmt_credential_t cred = golden_credential();
    pb_mgmt_client_t c;

    /* What attaching does without the switch in front of OpenThread's
     * stores: the Dataset is in flash again. */
    session_start(&cred, true);
    g_session_stores_again = true;
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_UNENROLLED && strcmp(g_events, "PHPQADEVSvsEV") == 0,
          "the store is read back once the session is up and before anything is sent: found not empty, the session "
          "is taken down, the store erased again and read back again");
    CHECK(!g_hub.completed && g_hub.done_erased == 0 && c.receipt_attempts == 0 && !c.receipt_acked,
          "no receipt left the board: the hub has completed nothing");
    CHECK(stored_state(EUI_A) == 36 && !dataset_present() && receipt_available(),
          "the release is final only with nothing of the network in flash; the receipt waits there for a client");

    /* The first of the two faults found in review (2026-10-08): this session,
     * and an erase that then fails. Before the fix the receipt had gone out
     * during the session and the hub had completed. */
    session_start(&cred, true);
    g_session_stores_again = true;
    g_on_request = fail_erase_from_session_end;
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_RELEASED_NOT_ERASED,
          "and when it cannot be erased again the poll does not report an unenroll");
    g_on_request = NULL;
    CHECK(!g_hub.completed && g_hub.done_erased == 0,
          "[fault review 1] erase failure must not already have completed at the hub");
    CHECK(stored_state(EUI_A) == 31 && dataset_present() && !receipt_available(),
          "the record is back to 'erase pending': the board hands out no receipt while its store holds a network");
    CHECK(key_in_flash(cred.key), "(with the key, which the next attempt needs to make the receipt)");
    CHECK(pb_mgmt_is_zero(c.rec.mgmt.key, 32), "and the client's RAM is wiped");

    /* The next boot: an ordinary pending release, with a session of its
     * own. */
    g_erase_fails = false;
    session_reset();
    events_reset();
    pb_mgmt_client_init(&c, &k_ops_session, EUI_A);
    CHECK(pb_mgmt_client_load(&c) == PB_MGMT_BOOT_RELEASE_PENDING &&
              pb_mgmt_client_resume_release(&c) == PB_MGMT_RELEASE_FINAL && !dataset_present() &&
              strcmp(g_events, "UDEVSvZs") == 0,
          "the next boot sends the notice, erases, checks, finishes -- and delivers in its own session");
    uint8_t second[PB_MGMT_RECEIPT_LEN];
    CHECK(g_hub.completed && g_hub.done_erased == 1 && c.receipt_acked && receipt_load(EUI_A, second) &&
              memcmp(g_hub.receipt, second, sizeof(second)) == 0,
          "only now does the hub complete, on the receipt this board also hands out: after an erase that checked out");

    printf("\n-- written later in the session --\n");
    session_start(&cred, true);
    g_hub.receipt_drop = 1;
    g_session_stores_at_receipt = 1;
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_UNENROLLED && strcmp(g_events, "PHPQADEVSvZvsEV") == 0,
          "written while the first copy was on its way, its answer lost: the read-back before the second finds it, "
          "the second is not sent, the store is erased again");
    CHECK(c.receipt_attempts == 1 && g_hub.done_erased == 1 && !c.receipt_acked && g_delay_count == 1,
          "one copy left the board, on a store that had just been read back empty");
    CHECK(stored_state(EUI_A) == 36 && !dataset_present() && receipt_available(), "final, nothing of the network in flash");

    session_start(&cred, true);
    g_session_stores_at_receipt = 1;
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_UNENROLLED && strcmp(g_events, "PHPQADEVSvZsEV") == 0 && c.receipt_acked,
          "written after a receipt that was acknowledged: found by the read-back when the session ends, erased again");
    CHECK(stored_state(EUI_A) == 36 && !dataset_present() && receipt_available(), "final, nothing of the network in flash");

    session_start(&cred, true);
    g_session_stores_at_receipt = 1;
    g_on_request = fail_erase_from_session_end;
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_RELEASED_NOT_ERASED && g_hub.completed && g_hub.done_erased == 1,
          "what no order can rule out: written AFTER the read-back that let the receipt out, and then not erasable. "
          "The hub completed on a receipt that was true when it left; the board does not report an unenroll");
    g_on_request = NULL;
    g_erase_fails = false;
    CHECK(stored_state(EUI_A) == 31 && dataset_present() && !receipt_available(),
          "and takes its receipt back: 'erase pending', nothing handed out, the next boot erases");

    printf("\n-- a read-back that proves nothing, and an erase that did not reach the flash --\n");
    session_start(&cred, true);
    g_session_readback_fails_once = true;
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_UNENROLLED && strcmp(g_events, "PHPQADEVSvsEV") == 0 &&
              !g_hub.completed && c.receipt_attempts == 0,
          "a store that cannot be read back in the session is not an empty store: nothing is sent, it is erased "
          "and read back again");
    CHECK(stored_state(EUI_A) == 36 && receipt_available(), "after which the release is final and the receipt waits");

    session_start(&cred, true);
    g_session_stores_again = true;
    g_on_request = erase_misses_flash_from_session_end;
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_RELEASED_NOT_ERASED && !g_hub.completed,
          "an erase after the session that reports success and did not reach the flash (OpenThread's own erase, "
          "once its stores are switched to RAM) is no erase: not an unenroll, nothing sent");
    g_on_request = NULL;
    g_erase_lies = false;
    CHECK(stored_state(EUI_A) == 31 && dataset_present() && !receipt_available(), "'erase pending', no receipt");

    printf("\n-- the receipt cannot even be taken back --\n");
    session_start(&cred, true);
    g_session_stores_again = true;
    g_on_request = nothing_can_be_written_from_session_end;
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_RELEASED_NOT_ERASED && !g_hub.completed,
          "the store cannot be erased and the record cannot be written: not an unenroll, nothing sent");
    g_on_request = NULL;
    g_erase_fails = false;
    CHECK(stored_state(EUI_A) == 36 && dataset_present(), "(what is stored is still the final record -- on a stored network)");
    CHECK(!receipt_available(), "even so no receipt is handed out: a final record alone does not make one");
    session_reset();
    events_reset();
    pb_mgmt_client_init(&c, &k_ops_session, EUI_A);
    CHECK(pb_mgmt_client_load(&c) == PB_MGMT_BOOT_NOTHING &&
              pb_mgmt_client_check_final_release(&c, no_network_stored, NULL) == PB_MGMT_FINAL_ERASED_AGAIN &&
              strcmp(g_events, "EV") == 0,
          "the next boot finds the final record on a stored network, erases, and reads the store back");
    CHECK(stored_state(EUI_A) == 36 && !dataset_present() && receipt_available(),
          "and now the receipt says what is true, and can be collected");
}

static void scenario_receipt_needs_a_clear_store(void)
{
    printf("\n-- a final record alone hands out no receipt: the stores are read for every request --\n");
    pb_enroll_mgmt_credential_t cred = golden_credential();
    pb_mgmt_client_t c;
    uint8_t good[PB_MGMT_RECEIPT_LEN], out[PB_MGMT_RECEIPT_LEN];

    session_start(&cred, true);
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_UNENROLLED && receipt_load(EUI_A, good),
          "(a released board: erase verified, final record, receipt)");
    g_store_checks = 0;
    CHECK(receipt_load(EUI_A, out) && receipt_load(EUI_A, out) && g_store_checks == 2,
          "the stores are asked every time, not once");
    memset(out, 0xee, sizeof(out));
    CHECK(!pb_mgmt_receipt_load(EUI_A, NULL, NULL, out) && pb_mgmt_is_zero(out, sizeof(out)),
          "no way to ask the stores: no receipt, and nothing in the caller's buffer");
    dataset_store();
    memset(out, 0xee, sizeof(out));
    CHECK(stored_state(EUI_A) == 36 && !receipt_load(EUI_A, out) && pb_mgmt_is_zero(out, sizeof(out)),
          "a network in the store under the final record: no receipt, nothing in the buffer");
    dataset_erase();
    g_store_unreadable = true;
    CHECK(!receipt_load(EUI_A, out), "a store that cannot be read: no receipt");
    g_store_unreadable = false;
    CHECK(receipt_load(EUI_A, out) && memcmp(out, good, sizeof(out)) == 0,
          "readable and clear again: the same receipt as before (the record was never touched)");

    /* A restart later: the stores are erased and read back before OpenThread
     * starts, and OpenThread makes a key of its own. */
    platform_start();
    CHECK(g_prestart_result == PB_MGMT_PRESTART_CLEARED && network_key_whose() == 'b' && !store_empty(),
          "(after a restart OpenThread's own key is in the store: 'empty' is not true there any more)");
    CHECK(receipt_load(EUI_A, out) && memcmp(out, good, sizeof(out)) == 0,
          "that key was seen being made on an empty store, so it is no network: the receipt is handed out as before");

    printf("\n-- every boot checks a final record against the stores --\n");
    events_reset();
    g_store_checks = 0;
    pb_mgmt_client_init(&c, &k_ops_session, EUI_A);
    CHECK(pb_mgmt_client_load(&c) == PB_MGMT_BOOT_NOTHING &&
              pb_mgmt_client_check_final_release(&c, no_network_stored, NULL) == PB_MGMT_FINAL_VERIFIED &&
              g_events[0] == 0 && g_store_checks == 1 && network_key_whose() == 'b',
          "final and no network stored: verified -- nothing more is erased, not OpenThread's own key either");
    dataset_store();
    CHECK(pb_mgmt_client_check_final_release(&c, no_network_stored, NULL) == PB_MGMT_FINAL_ERASED_AGAIN &&
              strcmp(g_events, "EV") == 0 && store_empty() && stored_state(EUI_A) == 36,
          "a network under the final record: erased and read back in this boot; the record is not touched");
    dataset_store();
    g_erase_fails = true;
    CHECK(pb_mgmt_client_check_final_release(&c, no_network_stored, NULL) == PB_MGMT_FINAL_NOT_ERASED &&
              dataset_present() && !receipt_available() && stored_state(EUI_A) == 36,
          "the erase fails: not erased, no receipt, the record as it was");
    g_erase_fails = false;
    g_erase_lies = true;
    CHECK(pb_mgmt_client_check_final_release(&c, no_network_stored, NULL) == PB_MGMT_FINAL_NOT_ERASED &&
              !receipt_available(),
          "the erase reports success and erased nothing: the read-back says so");
    g_erase_lies = false;
    CHECK(pb_mgmt_client_check_final_release(&c, no_network_stored, NULL) == PB_MGMT_FINAL_ERASED_AGAIN &&
              receipt_available(),
          "the next attempt erases it: the receipt is there again");
    g_store_unreadable = true;
    events_reset();
    CHECK(pb_mgmt_client_check_final_release(&c, no_network_stored, NULL) == PB_MGMT_FINAL_ERASED_AGAIN &&
              strcmp(g_events, "EV") == 0,
          "a store that cannot be read at boot is not taken as clear: erased and read back");
    g_store_unreadable = false;
    events_reset();
    CHECK(pb_mgmt_client_check_final_release(&c, NULL, NULL) == PB_MGMT_FINAL_ERASED_AGAIN && strcmp(g_events, "EV") == 0,
          "nor is a platform that gives no way to ask");

    /* Not this function's business: every other record. */
    flash_enrolled(EUI_A, &cred);
    dataset_store();
    events_reset();
    g_store_checks = 0;
    pb_mgmt_client_init(&c, &k_ops_session, EUI_A);
    CHECK(pb_mgmt_client_check_final_release(&c, no_network_stored, NULL) == PB_MGMT_FINAL_NONE && g_events[0] == 0 &&
              g_store_checks == 0 && dataset_present() && stored_state(EUI_A) == 10,
          "an enrolled board: not a final record -- nothing is read, nothing is erased");
    CHECK(!receipt_load(EUI_A, out) && g_store_checks == 0,
          "and without a final record there is no receipt; the stores are not even asked");
    session_start(&cred, true);
    g_erase_fails = true;
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_RELEASED_NOT_ERASED && stored_state(EUI_A) == 31,
          "(a board left 'erase pending')");
    g_erase_fails = false;
    events_reset();
    pb_mgmt_client_init(&c, &k_ops_session, EUI_A);
    CHECK(pb_mgmt_client_check_final_release(&c, no_network_stored, NULL) == PB_MGMT_FINAL_NONE && g_events[0] == 0 &&
              dataset_present(),
          "'erase pending' is not final either: that is pb_mgmt_client_resume_release()'s to finish");
}

/* Child: an enrolled board on a platform with the session, polled twice with
 * a command pending, losing power at `crash_at`. Exit 50 + poll result, plus
 * what became of the receipt at the hub: +100 when the board verified the
 * hub's acknowledgement, +30 when the hub completed (took a receipt) and the
 * board did not get to know. */
#define CHILD_RECEIPT_ACKED 100
#define CHILD_HUB_COMPLETED_UNACKED 30
static bool g_child_session_stores_again;
static int g_child_session_stores_at_receipt;

static int boot_poll_twice_session(int crash_at)
{
    pb_enroll_mgmt_credential_t cred = golden_credential();
    hub_init(EUI_A, &cred);
    g_hub.knows_hello = true;
    g_hub.command_pending = true;
    /* A boot starts with nothing in RAM (the child inherited the test's). */
    session_reset();
    g_erase_fails = g_erase_lies = false;
    g_session_stores_again = g_child_session_stores_again;
    g_session_stores_at_receipt = g_child_session_stores_at_receipt;
    g_crash_at = crash_at;
    g_on_request = crash_hook;
    pb_mgmt_client_t c;
    pb_mgmt_client_init(&c, &k_ops_session, EUI_A);
    if (pb_mgmt_client_load(&c) != PB_MGMT_BOOT_ARMED) {
        return 92;
    }
    pb_mgmt_client_poll(&c);
    int result = (int)pb_mgmt_client_poll(&c);
    return 50 + result +
           (c.receipt_acked ? CHILD_RECEIPT_ACKED : (g_hub.completed ? CHILD_HUB_COMPLETED_UNACKED : 0));
}

/* The same with a session whose attach stores the network again. */
static int boot_poll_twice_session_storing(int crash_at)
{
    g_child_session_stores_again = true;
    return boot_poll_twice_session(crash_at);
}

/* The same with a session that stores it while the first receipt is on its
 * way. */
static int boot_poll_twice_session_storing_late(int crash_at)
{
    g_child_session_stores_at_receipt = 1;
    return boot_poll_twice_session(crash_at);
}

/* Child: the firmware's boot path when no release is pending -- the check of
 * a final record. `erase` 0: the erase works, 1: it fails, 2: it reports
 * success and erases nothing. Exit 40 + check result, +20 when the network is
 * still stored afterwards. */
static int boot_check_final(int erase)
{
    session_reset();
    g_erase_fails = erase == 1;
    g_erase_lies = erase == 2;
    g_on_request = NULL;
    pb_mgmt_client_t c;
    pb_mgmt_client_init(&c, &k_ops_session, EUI_A);
    if (pb_mgmt_client_load(&c) == PB_MGMT_BOOT_RELEASE_PENDING) {
        return 96;
    }
    int result = (int)pb_mgmt_client_check_final_release(&c, no_network_stored, NULL);
    return 40 + result + (dataset_present() ? 20 : 0);
}

/* Child: one whole boot on a record that is a final release -- the check of
 * the record, and what that boot would then hand out. Exit 100 + 10 * check
 * result, +1 when a receipt is handed out, +2 when the Dataset is stored, +4
 * when the entry under the Network Key's id is a network's key. */
#define FINAL_BOOT(check, flags) (100 + 10 * (int)(check) + (flags))
#define FB_RECEIPT 1
#define FB_DATASET 2
#define FB_NETWORK_KEY 4

static int boot_final_report(int unused)
{
    (void)unused;
    session_reset();
    g_erase_fails = g_erase_lies = false;
    g_on_request = NULL;
    pb_mgmt_client_t c;
    pb_mgmt_client_init(&c, &k_ops_session, EUI_A);
    if (pb_mgmt_client_load(&c) == PB_MGMT_BOOT_RELEASE_PENDING) {
        return 96;
    }
    int check = (int)pb_mgmt_client_check_final_release(&c, no_network_stored, NULL);
    return FINAL_BOOT(check, (receipt_available() ? FB_RECEIPT : 0) | (dataset_present() ? FB_DATASET : 0) |
                                 (network_key_whose() == 'N' ? FB_NETWORK_KEY : 0));
}

/* Child: the firmware's boot path on that platform. Exit 60 + release result,
 * +20 when the network is still stored afterwards, +100 when the hub
 * acknowledged the receipt; 94 when no release is pending. */
static int boot_finish_release_session(int crash_at)
{
    pb_enroll_mgmt_credential_t cred = golden_credential();
    hub_init(EUI_A, &cred);
    g_hub.knows_hello = true;
    session_reset();
    g_erase_fails = g_erase_lies = false;
    g_crash_at = crash_at;
    g_on_request = crash_hook;
    pb_mgmt_client_t c;
    pb_mgmt_client_init(&c, &k_ops_session, EUI_A);
    if (pb_mgmt_client_load(&c) != PB_MGMT_BOOT_RELEASE_PENDING) {
        return 94;
    }
    int result = (int)pb_mgmt_client_resume_release(&c);
    return 60 + result + (dataset_present() ? 20 : 0) + (c.receipt_acked ? 100 : 0);
}

static void scenario_network_key_origin(void)
{
    printf("\n-- before the network stack starts: only a final record has its stores erased --\n");
    pb_enroll_mgmt_credential_t cred = golden_credential();
    pb_mgmt_client_t c;

    flash_enrolled(EUI_A, &cred);
    events_reset();
    CHECK(pb_mgmt_released_prestart(EUI_A, &k_prestart_ops) == PB_MGMT_PRESTART_NONE && g_events[0] == 0 &&
              dataset_present() && network_key_whose() == 'N' && stored_state(EUI_A) == 10,
          "an enrolled board: nothing is read, nothing is erased -- its network is what it runs on");
    session_start(&cred, true);
    g_erase_fails = true;
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_RELEASED_NOT_ERASED && stored_state(EUI_A) == 31, "(a board left 'erase pending')");
    g_erase_fails = false;
    events_reset();
    CHECK(pb_mgmt_released_prestart(EUI_A, &k_prestart_ops) == PB_MGMT_PRESTART_NONE && g_events[0] == 0 &&
              dataset_present(),
          "a pending release: nothing either -- that boot still needs the network for the notice and the session");
    nvs_sim_reset();
    dataset_store();
    events_reset();
    CHECK(pb_mgmt_released_prestart(EUI_A, &k_prestart_ops) == PB_MGMT_PRESTART_NONE && g_events[0] == 0 &&
              dataset_present() && pb_mgmt_released_prestart(NULL, &k_prestart_ops) == PB_MGMT_PRESTART_NONE,
          "no record: nothing");

    session_start(&cred, true);
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_UNENROLLED && stored_state(EUI_A) == 36, "(a released board: final record)");
    uint8_t before[PB_ENROLL_RECORD_MAX_LEN], after[PB_ENROLL_RECORD_MAX_LEN];
    size_t before_len = raw_record(before);
    dataset_store();
    events_reset();
    CHECK(pb_mgmt_released_prestart(EUI_A, &k_prestart_ops) == PB_MGMT_PRESTART_CLEARED && strcmp(g_events, "xy") == 0 &&
              !dataset_present() && !network_key_present(),
          "a final record: the stores are erased and read back, whatever was in them");
    CHECK(raw_record(after) == before_len && memcmp(before, after, before_len) == 0, "the record itself is not touched");
    events_reset();
    CHECK(pb_mgmt_released_prestart(EUI_A, &k_prestart_ops) == PB_MGMT_PRESTART_CLEARED && strcmp(g_events, "xy") == 0,
          "nothing stored: the same two steps, nothing to erase");
    g_direct_erase_fails = true;
    CHECK(pb_mgmt_released_prestart(EUI_A, &k_prestart_ops) == PB_MGMT_PRESTART_CLEARED,
          "an erase that reports an error on stores that read back empty: the read-back is the proof");
    dataset_store();
    events_reset();
    CHECK(pb_mgmt_released_prestart(EUI_A, &k_prestart_ops) == PB_MGMT_PRESTART_NOT_CLEARED &&
              strcmp(g_events, "xy") == 0 && dataset_present(),
          "the erase fails and the network is still there: not cleared (and it was read back all the same)");
    g_direct_erase_fails = false;
    g_erases_leave_network_key = true;
    CHECK(pb_mgmt_released_prestart(EUI_A, &k_prestart_ops) == PB_MGMT_PRESTART_NOT_CLEARED && !dataset_present() &&
              network_key_whose() == 'N',
          "an erase that reports no error and leaves the Network Key: not cleared");
    g_erases_leave_network_key = false;
    g_readback_fails = true;
    CHECK(pb_mgmt_released_prestart(EUI_A, &k_prestart_ops) == PB_MGMT_PRESTART_NOT_CLEARED,
          "the stores cannot be read back: not cleared, although the erase went through");
    g_readback_fails = false;
    const pb_mgmt_prestart_ops_t no_erase = {.erase = NULL, .erased = op_prestart_erased, .ctx = NULL};
    CHECK(pb_mgmt_released_prestart(EUI_A, NULL) == PB_MGMT_PRESTART_NOT_CLEARED &&
              pb_mgmt_released_prestart(EUI_A, &no_erase) == PB_MGMT_PRESTART_NOT_CLEARED,
          "a platform that gives no way to erase or read back: not cleared");

    printf("\n-- a final record and nothing but the old Network Key --\n");
    session_start(&cred, true);
    CHECK(session_unenroll(&c) == PB_MGMT_POLL_UNENROLLED && receipt_available(), "(a released board, erase verified, receipt)");
    /* What an erase that removed everything but that one key leaves (how a
     * board gets there under a final record: the power-loss scenarios). */
    network_key_left_behind();
    CHECK(stored_state(EUI_A) == 36 && !dataset_present() && network_key_whose() == 'N' && !receipt_available(),
          "[key origin review] no Dataset, no other key -- and no receipt: a key under the Network Key's id is not "
          "OpenThread's own for lying there");
    /* A boot that only looked, and started OpenThread on it. */
    g_read_back_empty = false;
    g_key_writes_seen = g_key_removes_seen = 0;
    openthread_starts();
    CHECK(network_key_whose() == 'N' && !receipt_available(),
          "OpenThread's start finds a key and leaves it: the network's key is still what is stored, and still no receipt");
    events_reset();
    pb_mgmt_client_init(&c, &k_ops_session, EUI_A);
    g_erases_leave_network_key = true;
    CHECK(pb_mgmt_client_load(&c) == PB_MGMT_BOOT_NOTHING &&
              pb_mgmt_client_check_final_release(&c, no_network_stored, NULL) == PB_MGMT_FINAL_NOT_ERASED &&
              strcmp(g_events, "EV") == 0 && !receipt_available(),
          "that boot's check of the final record: not verified -- OpenThread's erase is tried, leaves the key again, "
          "and the read-back says so");
    g_erases_leave_network_key = false;
    /* The firmware's boot. */
    platform_start();
    CHECK(g_prestart_result == PB_MGMT_PRESTART_CLEARED && network_key_whose() == 'b',
          "the firmware's boot: the old key is erased before OpenThread starts, that is read back, and what "
          "OpenThread then stores is its own");
    events_reset();
    CHECK(pb_mgmt_client_check_final_release(&c, no_network_stored, NULL) == PB_MGMT_FINAL_VERIFIED && g_events[0] == 0 &&
              receipt_available(),
          "now the final record checks out, and the receipt can be collected -- the manual confirmation works again");

    /* The other way to the same answer: what is seen of that entry. */
    dataset_store();
    store_remove("ds");
    CHECK(network_key_whose() == 'N' && !receipt_available(),
          "the entry replaced by a network's key later in the same boot (a remove and a write, both seen): no receipt");
    platform_start();
    CHECK(receipt_available(), "(a restart: erased, read back, OpenThread's own)");
    /* An entry changed in the same boot by something that does not go
     * through the key store's storage interface: one write was seen, no
     * remove -- and what is there is not that write. */
    network_key_left_behind();
    CHECK(network_key_whose() == 'N' && g_key_writes_seen == 1 && g_key_removes_seen == 0 && !receipt_available(),
          "the entry changed with nothing looking: no receipt -- it is not the entry that was seen being stored");
    platform_start();
    CHECK(network_key_whose() == 'b' && receipt_available(), "the next boot erases it: OpenThread's own key again");
}

static void scenario_receipt_power_loss(void)
{
    printf("\n-- power loss around the delivery session, and what each next boot does --\n");
    snprintf(g_flash, sizeof(g_flash), "/tmp/pb_mgmt_flash_%d.bin", (int)getpid());

    flash_file_reset();
    CHECK(in_child(boot_poll_twice_session, CRASH_NONE) == 50 + PB_MGMT_POLL_UNENROLLED + CHILD_RECEIPT_ACKED,
          "no power loss: unenrolled, and the hub acknowledged the receipt");
    CHECK(in_child(boot_report, 0) == 36 && in_child(boot_receipt, 0) == 1,
          "next boot: final record, not resumed, the receipt is there, no Dataset");
    CHECK(in_child(boot_store_empty, 0) == 0,
          "(that boot's store is not 'empty' -- OpenThread put its own key there when it started, on stores that "
          "boot had just erased and read back -- and the receipt is handed out all the same)");
    CHECK(in_child(boot_check_final, 0) == 40 + PB_MGMT_FINAL_VERIFIED && in_child(boot_store_empty, 0) == 0,
          "its check of the final record: verified, nothing erased");

    static const struct {
        int at;
        int exit;
        const char *what;
    } cuts[] = {
        {CRASH_AT_SESSION, POWER_LOST, "power lost after the final record, before the session started"},
        {CRASH_AFTER_ATTACH, POWER_LOST, "power lost in the session: attached, nothing sent yet"},
        {CRASH_AT_RECEIPT, POWER_LOST, "power lost in the session, the receipt on its way"},
        {CRASH_AT_SESSION_END, POWER_LOST_HUB_COMPLETED,
         "power lost after the hub took the receipt, before the session was taken down"},
    };
    for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
        flash_file_reset();
        CHECK(in_child(boot_poll_twice_session, cuts[i].at) == cuts[i].exit, cuts[i].what);
        CHECK(in_child(boot_report, 0) == 36 && in_child(boot_receipt, 0) == 1,
              "next boot: the record is final, the enrollment is not resumed, no Dataset is stored, and the "
              "receipt can be collected");
        CHECK(in_child(boot_finish_release_session, CRASH_NONE) == 94 &&
                  in_child(boot_check_final, 0) == 40 + PB_MGMT_FINAL_VERIFIED,
              "no release is pending and no network is stored: that boot sends nothing and erases nothing (the "
              "receipt is collected over BLE, or the hub already has it)");
    }

    /* The boot path delivers too, when the network is still stored at that
     * boot. */
    flash_file_reset();
    CHECK(in_child(boot_poll_twice_session, CRASH_BEFORE_ERASE) == POWER_LOST, "power lost before the network erase");
    CHECK(in_child(boot_finish_release_session, CRASH_NONE) == 160 + PB_MGMT_RELEASE_FINAL,
          "the next boot sends the notice, erases, checks, stores the final record -- and delivers the receipt in "
          "a session of its own");
    CHECK(in_child(boot_report, 0) == 36 && in_child(boot_receipt, 0) == 1, "final; not resumed");

    flash_file_reset();
    CHECK(in_child(boot_poll_twice_session, CRASH_AFTER_ERASE) == POWER_LOST,
          "power lost after the verified erase, before the final record");
    CHECK(in_child(boot_finish_release_session, CRASH_NONE) == 60 + PB_MGMT_RELEASE_FINAL,
          "the next boot has no network and nothing in RAM to make a session from: it erases what OpenThread's "
          "start put there, checks, and finishes without sending (not acknowledged)");
    CHECK(in_child(boot_report, 0) == 34 && in_child(boot_receipt, 0) == 1, "final, with a receipt to collect");

    flash_file_reset();
    CHECK(in_child(boot_poll_twice_session, CRASH_BEFORE_ERASE) == POWER_LOST, "power lost before the network erase");
    CHECK(in_child(boot_finish_release_session, CRASH_AT_RECEIPT) == POWER_LOST,
          "and again in the boot path's own session");
    CHECK(in_child(boot_report, 0) == 36 && in_child(boot_receipt, 0) == 1 &&
              in_child(boot_finish_release_session, CRASH_NONE) == 94,
          "next boot: final, not resumed, nothing pending");

    /* The second of the two faults found in review (2026-10-08): a session
     * that stored the network again AND a power cut before that was dealt
     * with. The final record is already stored (it is what lets the session
     * start), so the next boot finds a final record on a stored network.
     * That boot erases its stores before OpenThread starts; the cases below
     * begin with the boots in which that erase does not work. */
    printf("\n-- power lost in a session that stored the network again --\n");
    flash_file_reset();
    CHECK(in_child(boot_poll_twice_session_storing, CRASH_AFTER_ATTACH) == POWER_LOST,
          "(the attach stored the network again; power lost before anything was read back or sent -- the hub was "
          "told nothing)");
    g_boot_faults = BOOT_DIRECT_ERASE_FAILS;
    CHECK(in_child(boot_receipt, 0) == 10,
          "[fault review 2] power cut with a stored Dataset must not expose an erase receipt");
    CHECK(in_child(boot_report, 0) == 36 && in_child(boot_report, 0) == 36,
          "no boot resumes the enrollment or touches Thread: the record says released");
    CHECK(in_child(boot_check_final, 1) == 40 + PB_MGMT_FINAL_NOT_ERASED + 20 && in_child(boot_receipt, 0) == 10 &&
              in_child(boot_report, 0) == 36,
          "a boot in which neither erase works: not erased, still no receipt, still not resumed");
    CHECK(in_child(boot_check_final, 2) == 40 + PB_MGMT_FINAL_NOT_ERASED + 20 && in_child(boot_receipt, 0) == 10,
          "a boot whose second erase reports success and erased nothing: the same");
    CHECK(in_child(boot_final_report, 0) == FINAL_BOOT(PB_MGMT_FINAL_ERASED_AGAIN, FB_RECEIPT),
          "a boot whose first erase fails and whose second works: the network under the final record is erased, "
          "the store read back, and from then on that boot hands out the receipt");
    g_boot_faults = 0;
    CHECK(in_child(boot_final_report, 0) == FINAL_BOOT(PB_MGMT_FINAL_VERIFIED, FB_RECEIPT) && in_child(boot_report, 0) == 36,
          "later boots: verified, the receipt, no Dataset, not resumed");

    flash_file_reset();
    CHECK(in_child(boot_poll_twice_session_storing, CRASH_AFTER_ATTACH) == POWER_LOST, "(the same power cut)");
    CHECK(in_child(boot_final_report, 0) == FINAL_BOOT(PB_MGMT_FINAL_VERIFIED, FB_RECEIPT),
          "an ordinary next boot: the stored network is erased and read back before OpenThread starts -- verified, "
          "no Dataset, no network's key, and only then a receipt");

    /* The sequence of the review itself: the same session with the power cut
     * set for "the receipt on its way". There is no such moment any more. */
    flash_file_reset();
    CHECK(in_child(boot_poll_twice_session_storing, CRASH_AT_RECEIPT) == 50 + PB_MGMT_POLL_UNENROLLED,
          "(that session with the cut set for 'the receipt on its way': no receipt ever is -- nothing was sent, the "
          "hub completed nothing, the store was erased again and read back)");
    CHECK(in_child(boot_receipt, 0) == 1 && in_child(boot_report, 0) == 36,
          "next boot: final, no Dataset, the receipt waits for a client");

    /* Written later, and the power lost before the session's own read-back. */
    flash_file_reset();
    CHECK(in_child(boot_poll_twice_session_storing_late, CRASH_AT_SESSION_END) == POWER_LOST_HUB_COMPLETED,
          "(written after the receipt went out, power lost before the session was taken down: the hub completed "
          "on a receipt that was true when it left)");
    g_boot_faults = BOOT_DIRECT_ERASE_FAILS;
    CHECK(in_child(boot_receipt, 0) == 10 && in_child(boot_report, 0) == 36,
          "a next boot that cannot erase: the board hands out no receipt on a stored network, and resumes nothing");
    g_boot_faults = 0;
    CHECK(in_child(boot_final_report, 0) == FINAL_BOOT(PB_MGMT_FINAL_VERIFIED, FB_RECEIPT),
          "a next boot that can: erased before OpenThread starts -- what the hub was told is true again");

    /* The fault found in the second review (2026-10-08): an erase that removes
     * everything of the network but its key. What is left is a final record
     * and one entry under the Network Key's id -- where OpenThread's own key
     * lies on every other released board, and where OpenThread, starting,
     * leaves whatever it finds. */
    printf("\n-- a final record and nothing but the old Network Key, across boots --\n");
    flash_file_reset();
    CHECK(in_child(boot_poll_twice_session_storing, CRASH_AFTER_ATTACH) == POWER_LOST,
          "(a final record on a network stored again, as above)");
    g_boot_faults = BOOT_ERASES_LEAVE_NETWORK_KEY;
    CHECK(in_child(boot_final_report, 0) == FINAL_BOOT(PB_MGMT_FINAL_NOT_ERASED, FB_NETWORK_KEY),
          "a boot whose erases remove the Dataset and leave the Network Key (reporting no error): the read-backs "
          "say 'not empty' -- not erased, no receipt");
    CHECK(in_child(boot_final_report, 0) == FINAL_BOOT(PB_MGMT_FINAL_NOT_ERASED, FB_NETWORK_KEY),
          "[key origin review] the next boot finds a final record and nothing but the old Network Key: still no "
          "receipt -- the key is not taken for OpenThread's own");
    CHECK(in_child(boot_report, 0) == 36 && in_child(boot_receipt, 0) == 0,
          "and the boots after it, as long as that key cannot be removed: released, not resumed, no receipt");
    g_boot_faults = BOOT_STORE_UNREADABLE;
    CHECK(in_child(boot_final_report, 0) == FINAL_BOOT(PB_MGMT_FINAL_NOT_ERASED, 0) ||
              in_child(boot_final_report, 0) == FINAL_BOOT(PB_MGMT_FINAL_NOT_ERASED, FB_NETWORK_KEY),
          "a boot in which the store cannot be read: nothing is proven -- no receipt");
    flash_file_reset();
    CHECK(in_child(boot_poll_twice_session_storing, CRASH_AFTER_ATTACH) == POWER_LOST, "(again)");
    g_boot_faults = BOOT_ERASES_LEAVE_NETWORK_KEY;
    CHECK(in_child(boot_final_report, 0) == FINAL_BOOT(PB_MGMT_FINAL_NOT_ERASED, FB_NETWORK_KEY),
          "(a final record and nothing but the old Network Key)");
    g_boot_faults = BOOT_POWER_CUT_AFTER_PRESTART;
    CHECK(in_child(boot_final_report, 0) == POWER_LOST,
          "a boot in which the erase works -- and the power goes right after it, before OpenThread starts");
    g_boot_faults = 0;
    CHECK(in_child(boot_final_report, 0) == FINAL_BOOT(PB_MGMT_FINAL_VERIFIED, FB_RECEIPT),
          "the next boot: the old key is gone, OpenThread makes its own on stores read back empty -- verified, and "
          "the receipt can be collected (the manual confirmation works again)");
    CHECK(in_child(boot_final_report, 0) == FINAL_BOOT(PB_MGMT_FINAL_VERIFIED, FB_RECEIPT) &&
              in_child(boot_store_empty, 0) == 0 && in_child(boot_report, 0) == 36,
          "and every boot after it: OpenThread's key of the boot before is erased, a new one made, the receipt "
          "handed out");

    flash_file_reset();
    CHECK(in_child(boot_poll_twice_session_storing, CRASH_AFTER_ATTACH) == POWER_LOST, "(again)");
    g_boot_faults = BOOT_ERASES_LEAVE_NETWORK_KEY;
    CHECK(in_child(boot_final_report, 0) == FINAL_BOOT(PB_MGMT_FINAL_NOT_ERASED, FB_NETWORK_KEY),
          "(a final record and nothing but the old Network Key)");
    g_boot_faults = BOOT_DIRECT_ERASE_FAILS;
    CHECK(in_child(boot_final_report, 0) == FINAL_BOOT(PB_MGMT_FINAL_ERASED_AGAIN, FB_RECEIPT),
          "a boot whose erase before OpenThread starts fails: OpenThread starts on the old key and leaves it, the "
          "check of the record does not accept it, OpenThread's erase removes it and the read-back confirms -- "
          "only then a receipt");
    g_boot_faults = 0;
    unlink(g_flash);
}

/* ======================= scenarios: record version 3 and the old ones ======================= */

static void scenario_record_v3(void)
{
    printf("\n-- record version 3: what may be stored in each state --\n");
    pb_enroll_mgmt_credential_t cred = golden_credential();
    pb_enroll_record_t rec = enrolled_record(&cred), back;
    nvs_sim_reset();
    CHECK(pb_enroll_record_store(EUI_A, &rec) == ESP_OK && pb_enroll_record_load(EUI_A, &back) == PB_ENROLL_MARKER_VALID &&
              memcmp(&rec, &back, sizeof(rec)) == 0,
          "ENROLLED: stored and read back identically (200 bytes)");
    uint8_t raw[PB_ENROLL_RECORD_MAX_LEN];
    CHECK(raw_record(raw) == PB_ENROLL_RECORD_V3_LEN && raw[4] == 3 && raw[5] == 0x01 && raw[6] == 1 && raw[7] == 0 &&
              raw[50] == 1 && raw[51] == 0 && raw[52] == 0x10 && raw[68] == 0x20 && raw[84] == 0x30 && raw[115] == 0x4f,
          "the documented offsets: version, flags, state, format, hub id, generation, key");
    CHECK(pb_enroll_record_load(EUI_B, &back) == PB_ENROLL_MARKER_FOREIGN, "another board's EUI-64: foreign");

#define REFUSED(stmt, why)                                                                                           \
    do {                                                                                                             \
        pb_enroll_record_t r = enrolled_record(&cred);                                                               \
        stmt;                                                                                                        \
        nvs_sim_reset();                                                                                             \
        CHECK(pb_enroll_record_store(EUI_A, &r) == ESP_ERR_INVALID_ARG && raw_record(raw) == 0, why);                \
    } while (0)
    REFUSED(memset(r.mgmt.key, 0, 32), "ENROLLED without a key is refused, nothing written");
    REFUSED(memset(r.mgmt.hub_id, 0, 16), "ENROLLED without a hub id is refused");
    REFUSED(memset(r.mgmt.generation, 0, 16), "ENROLLED without a generation is refused");
    REFUSED(r.has_bridge_endpoint = false, "ENROLLED without a bridge endpoint is refused");
    REFUSED(r.bridge_endpoint.addr[0] = 0xfe, "ENROLLED with an endpoint outside the network is refused");
    REFUSED(r.command_id[0] = 1, "ENROLLED with a command id is refused");
    REFUSED(r.release_flags = 1, "ENROLLED with release flags is refused");
    REFUSED(r.mgmt_state = PB_ENROLL_MGMT_UNENROLLING, "UNENROLLING without a command id is refused");
    REFUSED(r.mgmt_state = PB_ENROLL_MGMT_RELEASED, "RELEASED without a release flag is refused");
    REFUSED(r.mgmt_state = (pb_enroll_mgmt_state_t)7, "an unknown state is refused");
    /* RELEASED, erase pending: everything it takes to tell the hub and to
     * make the receipt later -- and no receipt. */
#define PENDING(r) ((r).mgmt_state = PB_ENROLL_MGMT_RELEASED, (r).release_flags = PB_ENROLL_RELEASE_F_ERASE_PENDING, (r).command_id[0] = 0xe0)
    REFUSED((PENDING(r), memset(r.mgmt.key, 0, 32)), "RELEASED/erase pending without the key is refused");
    REFUSED((PENDING(r), r.command_id[0] = 0), "RELEASED/erase pending without a command id is refused");
    REFUSED((PENDING(r), r.done_mac[0] = 1), "RELEASED/erase pending that already holds a receipt is refused");
    REFUSED((PENDING(r), r.release_flags = 3), "'erase pending' together with 'notice acknowledged' is refused");
    REFUSED((PENDING(r), r.release_flags = 5), "'erase pending' together with 'erased' is refused");
    REFUSED((PENDING(r), r.has_bridge_endpoint = false), "RELEASED/erase pending without the endpoint is refused");
    /* RELEASED, final: the receipt, no key, no network. */
#define FINAL(r)                                                                                                     \
    (memset(&(r).id, 0, sizeof((r).id)), (r).has_bridge_endpoint = false, memset(&(r).bridge_endpoint, 0, sizeof((r).bridge_endpoint)), \
     memset((r).mgmt.key, 0, 32), (r).mgmt_state = PB_ENROLL_MGMT_RELEASED, (r).release_flags = PB_ENROLL_RELEASE_F_ERASED, \
     (r).command_id[0] = 0xe0, (r).done_mac[0] = 0x11)
    REFUSED((FINAL(r), r.release_flags = PB_ENROLL_RELEASE_F_NOTICE_ACKED), "a final record without 'erased' is refused");
    REFUSED((FINAL(r), r.release_flags = 0x0c), "a final record with an unknown flag is refused");
    REFUSED((FINAL(r), r.mgmt.key[5] = 1), "a final record that still holds key bytes is refused");
    REFUSED((FINAL(r), memset(r.done_mac, 0, 32)), "a final record without the receipt MAC is refused");
    REFUSED((FINAL(r), r.command_id[0] = 0), "a final record without the command id is refused");
    REFUSED((FINAL(r), memset(r.mgmt.hub_id, 0, 16)), "a final record without the hub id is refused");
    REFUSED((FINAL(r), r.id.ext_pan_id[0] = 1), "a final record that still names a network is refused");
    REFUSED((FINAL(r), r.ack_mac[0] = 1), "a final record with anything in the unused MAC field is refused");
#undef REFUSED
    {
        pb_enroll_record_t r = enrolled_record(&cred);
        PENDING(r);
        nvs_sim_reset();
        CHECK(pb_enroll_record_store(EUI_A, &r) == ESP_OK && pb_enroll_record_load(EUI_A, &back) == PB_ENROLL_MARKER_VALID &&
                  memcmp(&r, &back, sizeof(r)) == 0 && !pb_enroll_record_is_final_release(&back) &&
                  !pb_enroll_record_is_enrolled(&back),
              "RELEASED/erase pending: stored and read back; neither enrolled nor final");
        r = enrolled_record(&cred);
        FINAL(r);
        r.release_flags |= PB_ENROLL_RELEASE_F_NOTICE_ACKED;
        nvs_sim_reset();
        CHECK(pb_enroll_record_store(EUI_A, &r) == ESP_OK && pb_enroll_record_load(EUI_A, &back) == PB_ENROLL_MARKER_VALID &&
                  memcmp(&r, &back, sizeof(r)) == 0 && pb_enroll_record_is_final_release(&back),
              "RELEASED/final: stored and read back; final");
        CHECK(raw_record(raw) == PB_ENROLL_RECORD_V3_LEN && raw[5] == 0 && raw[6] == 3 && raw[7] == 0x06 &&
                  pb_mgmt_is_zero(&raw[16], 34) && pb_mgmt_is_zero(&raw[84], 32) && raw[52] == 0x10 && raw[68] == 0x20 &&
                  raw[116] == 0xe0 && raw[132] == 0x11 && pb_mgmt_is_zero(&raw[164], 32),
              "the documented final layout: no network, no endpoint, no key; hub id, generation, command id, receipt MAC");
    }
#undef PENDING
#undef FINAL

    /* A record with a valid CRC whose fields do not fit its state is not
     * half-trusted when read. */
    nvs_sim_reset();
    pb_enroll_record_store(EUI_A, &rec);
    size_t len = raw_record(raw);
    raw[6] = 3; /* RELEASED, with no release flag */
    {
        /* CRC-32 (IEEE) of [0..195], little-endian, as the firmware writes it. */
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < 196; i++) {
            crc ^= raw[i];
            for (int b = 0; b < 8; b++) {
                crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
            }
        }
        crc = ~crc;
        raw[196] = (uint8_t)crc;
        raw[197] = (uint8_t)(crc >> 8);
        raw[198] = (uint8_t)(crc >> 16);
        raw[199] = (uint8_t)(crc >> 24);
    }
    nvs_sim_put_blob("nvs", PB_ENROLL_NVS_NAMESPACE, PB_ENROLL_NVS_KEY, raw, len);
    CHECK(pb_enroll_record_load(EUI_A, &back) == PB_ENROLL_MARKER_CORRUPT && pb_mgmt_is_zero((const uint8_t *)&back, sizeof(back)),
          "valid CRC, impossible contents (RELEASED without a release flag): corrupt, nothing of it is returned");
    raw[6] = 1;
    raw[100] ^= 0x01; /* one key bit, CRC now wrong */
    nvs_sim_put_blob("nvs", PB_ENROLL_NVS_NAMESPACE, PB_ENROLL_NVS_KEY, raw, len);
    CHECK(pb_enroll_record_load(EUI_A, &back) == PB_ENROLL_MARKER_CORRUPT, "a flipped bit: corrupt");
    pb_mgmt_client_t c;
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    CHECK(pb_mgmt_client_load(&c) == PB_MGMT_BOOT_NOTHING && !c.armed, "a corrupt record arms nothing");
}

static void scenario_old_records(void)
{
    printf("\n-- boards enrolled before management existed (record versions 1 and 2) --\n");
    uint8_t before[PB_ENROLL_RECORD_MAX_LEN], after[PB_ENROLL_RECORD_MAX_LEN];
    pb_enroll_mgmt_credential_t cred = golden_credential();
    pb_mgmt_client_t c;

    for (int version = 1; version <= 2; version++) {
        pb_enroll_record_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.id = NET_OTBR;
        if (version == 2) {
            rec.has_bridge_endpoint = true;
            rec.bridge_endpoint = EP_OTBR;
        }
        nvs_sim_reset();
        pb_enroll_record_store(EUI_A, &rec);
        dataset_store();
        size_t before_len = raw_record(before);
        hub_init(EUI_A, &cred);
        g_hub.command_pending = true;
        g_hub.skip_nonce_round = true;
        events_reset();

        pb_mgmt_client_init(&c, &k_ops, EUI_A);
        bool nothing = pb_mgmt_client_load(&c) == PB_MGMT_BOOT_NOTHING && !c.armed &&
                       pb_mgmt_client_poll(&c) == PB_MGMT_POLL_NOT_ARMED && g_hub.polls == 0 && g_event_count == 0 &&
                       pb_mgmt_client_resume_release(&c) == PB_MGMT_RELEASE_NOTHING;
        size_t after_len = raw_record(after);
        bool same = before_len == (version == 1 ? PB_ENROLL_MARKER_LEN : PB_ENROLL_MARKER_V2_LEN) &&
                    after_len == before_len && memcmp(before, after, before_len) == 0;
        g_thread_started = false;
        pb_enroll_ctl_t ctl;
        pb_enroll_ctl_init(&ctl, &k_enroll_ops, EUI_A);
        bool resumed = pb_enroll_ctl_boot(&ctl) == PB_ENROLL_STATE_ENROLLED && g_thread_started && !ctl.has_mgmt;
        size_t final_len = raw_record(after);
        same = same && final_len == before_len && memcmp(before, after, before_len) == 0;
        if (version == 1) {
            CHECK(nothing && same && resumed,
                  "version 1: no poll is ever sent, the board resumes as before, the record is byte for byte unchanged");
        } else {
            CHECK(nothing && same && resumed,
                  "version 2: no poll is ever sent, the board resumes as before, the record is byte for byte unchanged");
        }
    }

    /* No record at all. */
    nvs_sim_reset();
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    CHECK(pb_mgmt_client_load(&c) == PB_MGMT_BOOT_NOTHING && raw_record(before) == 0, "no record: nothing, and nothing is created");
}

static void scenario_reenroll_after_release(void)
{
    printf("\n-- a released board is enrolled again (the real controller) --\n");
    pb_enroll_mgmt_credential_t cred = golden_credential();
    flash_enrolled(EUI_A, &cred);
    hub_init(EUI_A, &cred);
    g_hub.command_pending = true;
    events_reset();
    pb_mgmt_client_t c;
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    pb_mgmt_client_load(&c);
    pb_mgmt_client_poll(&c);
    pb_mgmt_client_poll(&c);
    uint8_t old_command[PB_MGMT_MAX_MESSAGE_LEN];
    size_t old_len = g_hub.saved_len;
    memcpy(old_command, g_hub.saved, old_len);
    CHECK(stored_state(EUI_A) == 36 && receipt_available(), "(released, erase verified; the receipt is there to collect)");

    /* The controller at the next boot: waiting, nothing wrong. */
    g_thread_started = false;
    pb_enroll_ctl_t ctl;
    pb_enroll_ctl_init(&ctl, &k_enroll_ops, EUI_A);
    CHECK(pb_enroll_ctl_boot(&ctl) == PB_ENROLL_STATE_WAITING && ctl.last == PB_ENROLL_RESULT_NONE && !g_thread_started,
          "boot: waiting for enrollment, last result 'none', Thread untouched");
    /* The same holds on a board whose image requires a delivered endpoint:
     * the released record is not mistaken for an old version 1 enrollment. */
    pb_enroll_ctl_init(&ctl, &k_enroll_ops, EUI_A);
    pb_enroll_ctl_set_endpoint_policy(&ctl, PB_ENROLL_ENDPOINT_POLICY_REQUIRED);
    CHECK(pb_enroll_ctl_boot(&ctl) == PB_ENROLL_STATE_WAITING && ctl.last == PB_ENROLL_RESULT_NONE,
          "also with the 'endpoint required' image policy");

    /* A new enrollment with a new generation and key. */
    pb_enroll_mgmt_credential_t next;
    memcpy(next.hub_id, cred.hub_id, 16);
    seq(next.generation, 16, 0x70);
    seq(next.key, 32, 0x90);
    char pskd[PB_ENROLL_PSKD_BUF_LEN] = "J01NME7K";
    CHECK(pb_enroll_ctl_request_join_managed(&ctl, pskd, &EP_OTBR, &next, 0) == ESP_OK && raw_record(old_command + 0) == 0,
          "a new managed join is accepted, and the released record is erased before the Joiner starts");
    CHECK(!receipt_available(),
          "with it the receipt is gone: a client that wants it for the hub collects it before it starts a new enrollment");
    memcpy(old_command, g_hub.saved, old_len);
    dataset_store(); /* what the Joiner and the attach leave in OpenThread's storage */
    pb_enroll_ctl_on_joiner_done(&ctl, ctl.attempt, PB_ENROLL_JOINER_OK, 100);
    pb_enroll_ctl_tick(&ctl, true, 200);
    pb_enroll_record_t rec;
    CHECK(ctl.state == PB_ENROLL_STATE_ENROLLED && ctl.has_mgmt && !ctl.pending_mgmt &&
              pb_mgmt_is_zero((const uint8_t *)&ctl.pending_mgmt_cred, sizeof(ctl.pending_mgmt_cred)),
          "enrolled; the controller keeps no copy of the key");
    CHECK(pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_VALID && rec.has_mgmt &&
              rec.mgmt_state == PB_ENROLL_MGMT_ENROLLED && memcmp(&rec.mgmt, &next, sizeof(next)) == 0,
          "record version 3 with the NEW generation and key");

    /* The old enrollment's command, replayed at the new enrollment. */
    hub_init(EUI_A, &next);
    memcpy(g_hub.saved, old_command, old_len);
    g_hub.saved_len = old_len;
    g_hub.mutate_poll = MUT_STALE_NONCE;
    events_reset();
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    CHECK(pb_mgmt_client_load(&c) == PB_MGMT_BOOT_ARMED && pb_mgmt_client_poll(&c) == PB_MGMT_POLL_NO_ANSWER &&
              stored_state(EUI_A) == 10,
          "the previous enrollment's unenroll command has no effect on the new enrollment");
    g_hub.mutate_poll = MUT_NONE;
    CHECK(pb_mgmt_client_poll(&c) == PB_MGMT_POLL_IDLE, "and the new enrollment polls normally");

    /* A managed join that fails drops the key. */
    nvs_sim_reset();
    pb_enroll_ctl_init(&ctl, &k_enroll_ops, EUI_A);
    pb_enroll_ctl_boot(&ctl);
    strcpy(pskd, "J01NME7K");
    pb_enroll_ctl_request_join_managed(&ctl, pskd, &EP_OTBR, &next, 0);
    CHECK(ctl.pending_mgmt && memcmp(&ctl.pending_mgmt_cred, &next, sizeof(next)) == 0, "(the key is held in RAM while joining)");
    pb_enroll_ctl_on_joiner_done(&ctl, ctl.attempt, PB_ENROLL_JOINER_ERR_SECURITY, 100);
    CHECK(ctl.state == PB_ENROLL_STATE_WAITING && !ctl.pending_mgmt &&
              pb_mgmt_is_zero((const uint8_t *)&ctl.pending_mgmt_cred, sizeof(ctl.pending_mgmt_cred)) && raw_record(old_command) == 0,
          "a failed join wipes the key from RAM and stores nothing");
    strcpy(pskd, "J01NME7K");
    pb_enroll_mgmt_credential_t zero_key = next;
    memset(zero_key.key, 0, 32);
    CHECK(pb_enroll_ctl_request_join_managed(&ctl, pskd, &EP_OTBR, &zero_key, 0) == ESP_ERR_INVALID_ARG &&
              ctl.state == PB_ENROLL_STATE_WAITING,
          "a credential with an all-zero key is refused before anything starts");
    strcpy(pskd, "J01NME7K");
    CHECK(pb_enroll_ctl_request_join_managed(&ctl, pskd, NULL, &next, 0) == ESP_ERR_INVALID_ARG,
          "a management credential without a bridge endpoint is refused");
}

/* ======================= scenarios: nothing secret in the log ======================= */

static void scenario_secrets_never_logged(void)
{
    printf("\n-- no key, nonce, MAC or command id in the log output --\n");
    char path[] = "/tmp/pb_mgmt_logXXXXXX";
    int fd = mkstemp(path);
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    dup2(fd, STDERR_FILENO);

    pb_enroll_mgmt_credential_t cred = golden_credential();
    flash_enrolled(EUI_A, &cred);
    hub_init(EUI_A, &cred);
    g_hub.command_pending = true;
    g_hub.done_drop = 1;
    pb_mgmt_client_t c;
    pb_mgmt_client_init(&c, &k_ops, EUI_A);
    pb_mgmt_client_load(&c);
    g_hub.mutate_poll = MUT_WRONG_KEY;
    pb_mgmt_client_poll(&c);
    g_hub.mutate_poll = MUT_NONE;
    pb_mgmt_client_poll(&c);
    pb_mgmt_client_poll(&c);
    /* the delivery session, with an answer lost, refused, and a store that
     * was written again */
    session_start(&cred, true);
    g_hub.receipt_drop = 1;
    session_unenroll(&c);
    session_start(&cred, false);
    g_hub.receipt_reject = true;
    session_unenroll(&c);
    session_start(&cred, true);
    g_session_stores_again = true;
    g_on_request = fail_erase_from_session_end;
    session_unenroll(&c);
    g_on_request = NULL;
    g_erase_fails = false;
    /* a store written late, a final record left on a stored network, its
     * receipt refused, and the boot that finds it -- erase failing, then not */
    session_start(&cred, true);
    g_session_stores_at_receipt = 1;
    g_on_request = nothing_can_be_written_from_session_end;
    session_unenroll(&c);
    g_on_request = NULL;
    (void)receipt_available();
    pb_mgmt_client_init(&c, &k_ops_session, EUI_A);
    pb_mgmt_client_check_final_release(&c, no_network_stored, NULL);
    g_erase_fails = false;
    pb_mgmt_client_check_final_release(&c, no_network_stored, NULL);
    session_reset();
    /* the boot path and the controller as well */
    flash_enrolled(EUI_A, &cred);
    pb_enroll_ctl_t ctl;
    pb_enroll_ctl_init(&ctl, &k_enroll_ops, EUI_A);
    pb_enroll_ctl_boot(&ctl);
    char pskd[PB_ENROLL_PSKD_BUF_LEN] = "J01NME7K";
    nvs_sim_reset();
    pb_enroll_ctl_init(&ctl, &k_enroll_ops, EUI_A);
    pb_enroll_ctl_boot(&ctl);
    pb_enroll_ctl_request_join_managed(&ctl, pskd, &EP_OTBR, &cred, 0);
    dataset_store();
    pb_enroll_ctl_on_joiner_done(&ctl, ctl.attempt, PB_ENROLL_JOINER_OK, 100);
    pb_enroll_ctl_tick(&ctl, true, 200);

    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    static char log[65536];
    memset(log, 0, sizeof(log));
    lseek(fd, 0, SEEK_SET);
    ssize_t n = read(fd, log, sizeof(log) - 1);
    close(fd);
    unlink(path);
    CHECK(n > 0, "the scenario produced log output");

    /* Any run of 4 bytes of the key, generation, hub id or command id as hex
     * (either case), and the raw bytes themselves. */
    bool leaked = false;
    const struct {
        const uint8_t *p;
        size_t len;
    } secrets[] = {{cred.key, 32}, {cred.generation, 16}, {cred.hub_id, 16}, {g_hub.command_id, 16}};
    for (size_t s = 0; s < sizeof(secrets) / sizeof(secrets[0]); s++) {
        for (size_t i = 0; i + 4 <= secrets[s].len; i++) {
            char lower[9] = {0}, upper[9] = {0};
            for (int b = 0; b < 4; b++) {
                sprintf(&lower[2 * b], "%02x", secrets[s].p[i + b]);
                sprintf(&upper[2 * b], "%02X", secrets[s].p[i + b]);
            }
            if (strstr(log, lower) != NULL || strstr(log, upper) != NULL) {
                leaked = true;
            }
        }
        for (ssize_t i = 0; i + (ssize_t)secrets[s].len <= n; i++) {
            if (memcmp(&log[i], secrets[s].p, secrets[s].len) == 0) {
                leaked = true;
            }
        }
    }
    CHECK(!leaked, "no key, generation, hub id or command id appears in the log, as hex or as bytes");
    CHECK(strstr(log, "fd11") == NULL && strstr(log, "FD11") == NULL, "nor the bridge address");
}

int main(void)
{
    scenario_test_hmac_is_hmac();
    scenario_golden_vectors();
    scenario_malformed_messages();
    scenario_record_v3();
    scenario_poll_idle();
    scenario_unenroll_online();
    scenario_rejected_commands();
    scenario_replay();
    scenario_response_loss();
    scenario_write_failures();
    scenario_power_loss();
    scenario_receipt_delivered();
    scenario_hello();
    scenario_receipt_retries();
    scenario_receipt_session_must_not_store();
    scenario_receipt_needs_a_clear_store();
    scenario_network_key_origin();
    scenario_receipt_power_loss();
    scenario_old_records();
    scenario_reenroll_after_release();
    scenario_secrets_never_logged();

    printf("\n%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
