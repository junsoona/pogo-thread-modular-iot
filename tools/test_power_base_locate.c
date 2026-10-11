/*
 * Host test for "the same hub at another Thread address or port"
 * (docs/power-base-management.md, section 12), on the REAL board-side code:
 *   components/power_base_mgmt/pb_mgmt_proto.c    (LOCATE / LOCATE_ACK, MAC)
 *   components/power_base_mgmt/pb_mgmt_client.c   (when a round runs, hint ->
 *                                                  claim -> confirmed -> stored)
 *   components/power_base_enrollment/power_base_enrollment.c (record version 3)
 * linked against the functional NVS simulator. Nothing of the decision is
 * stubbed: the test plays the Thread network -- which address and port
 * answers what -- and the flash, and looks at what the client sent, stored
 * and handed to the platform.
 *
 * The network model. A "listener" is one (address, port) in the board's
 * Thread network and what sits behind it:
 *   - the hub's ingest listener: answers POLL (IDLE) and LOCATE with the
 *     hub's real key; LOCATE_ACK names the endpoint the hub says it has now
 *     (its Mesh-Local EID and ingest port). The hub answers on every address
 *     its host has: its EID and its routing locator;
 *   - the hub's locate-only socket (ingest moved off the locate port):
 *     LOCATE only, everything else unanswered;
 *   - other nodes: silent, refusing, another hub with another key, the same
 *     hub id with a wrong key, a replayer, a node that changes a byte, a hub
 *     that names an endpoint nobody answers at.
 * The scripted hub packs its answers with the same pb_mgmt_proto.c; the real
 * Linux hub code answers this same client in
 * linux/matter_bridge/app/api/tests/test_power_base_management.cpp.
 *
 * Power loss: a boot runs in a forked child over a flash file
 * (nvs_sim_set_backing_file) and is cut at a chosen commit.
 *
 * Progress from round to round (scenario_progress_from_round_to_round, and
 * the end of scenario_many_candidates): more candidates than one round may
 * ask, the hub at the far end of them, lists that change between rounds, the
 * hub's answer arriving with a round's last request, and what must not get
 * weaker for it (answers that are not the hub's, a store that fails, a
 * reboot, an enrollment given up meanwhile).
 *
 * What this does NOT show: OpenThread (the platform's hint collection in
 * power_base/main/enrollment_locate_hints.c is not run here -- hints are
 * given by the test; tools/test_power_base_locate_openthread.sh runs it on
 * the real OpenThread core), CoAP, the radio, real NVS, the exporter (tools/
 * test_linux_bridge_exporter_destination.c) or the Linux hub's own code.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I tools/host_test_stubs/nvs_sim -I tools/host_test_stubs \
 *      -I components/power_base_enrollment/include -I components/power_base_mgmt/include \
 *      -o /tmp/test_power_base_locate tools/test_power_base_locate.c \
 *      components/power_base_mgmt/pb_mgmt_proto.c components/power_base_mgmt/pb_mgmt_client.c \
 *      components/power_base_enrollment/power_base_enrollment.c tools/host_test_stubs/nvs_sim/nvs_sim.c \
 *   && /tmp/test_power_base_locate
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
#define ML_PREFIX 0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00
static const pb_enroll_network_id_t NET = {
    .ext_pan_id = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88},
    .mesh_local_prefix = {ML_PREFIX},
};
#define EID_OLD {ML_PREFIX, 0x8c, 0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79}
#define EID_NEW {ML_PREFIX, 0x12, 0x9a, 0x77, 0x01, 0xbe, 0xef, 0x40, 0x02}
#define EID_NEW2 {ML_PREFIX, 0x5a, 0x00, 0x13, 0x37, 0x00, 0xaa, 0x00, 0x03}
static const pb_enroll_bridge_endpoint_t EP_OLD = {.addr = EID_OLD, .port = 5684};
static const pb_enroll_bridge_endpoint_t EP_NEW_ADDR = {.addr = EID_NEW, .port = 5684};
static const pb_enroll_bridge_endpoint_t EP_NEW_PORT = {.addr = EID_OLD, .port = 5700};
static const pb_enroll_bridge_endpoint_t EP_NEW_BOTH = {.addr = EID_NEW, .port = 5700};
static const pb_enroll_bridge_endpoint_t EP_NEW2 = {.addr = EID_NEW2, .port = 5684};
#define HUB_RLOC16 0x3000
#define HUB_RLOC16_AFTER 0x5c00

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

static bool same_ep(const pb_enroll_bridge_endpoint_t *a, const pb_enroll_bridge_endpoint_t *b)
{
    return a->port == b->port && memcmp(a->addr, b->addr, sizeof(a->addr)) == 0;
}

static pb_enroll_bridge_endpoint_t locator(uint16_t rloc16, uint16_t port)
{
    pb_enroll_bridge_endpoint_t ep;
    (void)pb_mgmt_locator_address(NET.mesh_local_prefix, rloc16, ep.addr);
    ep.port = port;
    return ep;
}

/* ======================= the network ======================= */

typedef enum {
    L_SILENT = 0,       /* nothing answers */
    L_REFUSING,         /* a CoAP server that refuses everything (4.xx) */
    L_HUB,              /* the hub's ingest listener */
    L_HUB_LOCATE_ONLY,  /* the hub's second socket on the locate port */
    L_OTHER_HUB,        /* a hub with its own id and key */
    L_WRONG_KEY,        /* says it is our hub, signs with another key */
    L_OTHER_GENERATION, /* our hub's key, another generation in the header */
    L_OTHER_BOARD,      /* our hub's key, another board's EUI-64 */
    L_REPLAYER,         /* answers with a LOCATE_ACK it recorded earlier */
    L_TAMPERER,         /* our hub's answer with the address changed */
    L_GARBAGE,          /* a 2.xx answer with bytes that are no message */
    L_HUB_NAMES,        /* our hub, but names `names` as its endpoint */
    L_WRONG_TYPE,       /* our hub's key on an IDLE where a LOCATE_ACK belongs */
} listener_kind_t;

#define LISTENERS_MAX 16
typedef struct {
    pb_enroll_bridge_endpoint_t at;
    listener_kind_t kind;
    pb_enroll_bridge_endpoint_t names; /* L_HUB_NAMES */
} listener_t;

static listener_t g_listeners[LISTENERS_MAX];
static size_t g_listener_count;

/* The hub. */
static pb_enroll_mgmt_credential_t g_hub_cred;
static pb_enroll_bridge_endpoint_t g_hub_endpoint; /* what it says its endpoint is */
static bool g_hub_command_pending;
static uint8_t g_hub_nonce_counter = 0x70;
static uint8_t g_hub_last_nonce[PB_MGMT_NONCE_LEN];
static bool g_hub_has_nonce;
static uint8_t g_hub_command_id[PB_MGMT_COMMAND_ID_LEN];
static int g_hub_done_released, g_hub_done_erased;

/* What the client sent. */
#define SENT_MAX 4096
typedef struct {
    pb_enroll_bridge_endpoint_t to;
    uint8_t type;
    uint8_t nonce[PB_MGMT_NONCE_LEN];
} sent_t;
static sent_t g_sent[SENT_MAX];
static size_t g_sent_count;

/* The replayer's recording, and "an answer that arrives one request late". */
static uint8_t g_recorded_ack[PB_MGMT_MAX_MESSAGE_LEN];
static size_t g_recorded_ack_len;
static bool g_answers_one_late;
static uint8_t g_late_answer[PB_MGMT_MAX_MESSAGE_LEN];
static size_t g_late_answer_len;

static void net_reset(void)
{
    g_listener_count = 0;
    g_sent_count = 0;
    g_hub_cred = golden_credential();
    g_hub_endpoint = EP_OLD;
    g_hub_command_pending = false;
    g_hub_has_nonce = false;
    g_hub_done_released = g_hub_done_erased = 0;
    g_recorded_ack_len = 0;
    g_answers_one_late = false;
    g_late_answer_len = 0;
    seq(g_hub_command_id, sizeof(g_hub_command_id), 0xe0);
}

static void listen_at(pb_enroll_bridge_endpoint_t at, listener_kind_t kind)
{
    if (g_listener_count < LISTENERS_MAX) {
        g_listeners[g_listener_count].at = at;
        g_listeners[g_listener_count].kind = kind;
        g_listener_count++;
    }
}

static void listen_naming(pb_enroll_bridge_endpoint_t at, pb_enroll_bridge_endpoint_t names)
{
    listen_at(at, L_HUB_NAMES);
    g_listeners[g_listener_count - 1].names = names;
}

/* The hub's host: its ingest listener on its EID and on its routing locator,
 * and -- when the ingest port is not the locate port -- the locate-only
 * socket on both. Its LOCATE_ACK names `eid`. */
static void hub_at(pb_enroll_bridge_endpoint_t eid, uint16_t rloc16)
{
    g_hub_endpoint = eid;
    listen_at(eid, L_HUB);
    listen_at(locator(rloc16, eid.port), L_HUB);
    if (eid.port != PB_MGMT_LOCATE_PORT) {
        pb_enroll_bridge_endpoint_t loc = eid;
        loc.port = PB_MGMT_LOCATE_PORT;
        listen_at(loc, L_HUB_LOCATE_ONLY);
        listen_at(locator(rloc16, PB_MGMT_LOCATE_PORT), L_HUB_LOCATE_ONLY);
    }
}

static size_t pack_as(pb_mgmt_msg_t *m, const uint8_t eui[8], const uint8_t hub_id[16], const uint8_t generation[16],
                      const uint8_t key[32], uint8_t *out)
{
    memcpy(m->id.eui64, eui, 8);
    memcpy(m->id.hub_id, hub_id, 16);
    memcpy(m->id.generation, generation, 16);
    return pb_mgmt_pack(m, key, test_hmac, NULL, out, PB_MGMT_MAX_MESSAGE_LEN);
}

static size_t locate_ack(const pb_mgmt_msg_t *req, const pb_enroll_bridge_endpoint_t *names, const uint8_t eui[8],
                         const uint8_t hub_id[16], const uint8_t generation[16], const uint8_t key[32], uint8_t *out)
{
    pb_mgmt_msg_t a;
    memset(&a, 0, sizeof(a));
    a.type = PB_MGMT_TYPE_LOCATE_ACK;
    memcpy(a.board_nonce, req->board_nonce, PB_MGMT_NONCE_LEN);
    memcpy(a.endpoint_addr, names->addr, PB_MGMT_ENDPOINT_ADDR_LEN);
    a.endpoint_port = names->port;
    return pack_as(&a, eui, hub_id, generation, key, out);
}

/* The real hub's side, as far as this test needs it: the checks
 * PowerBaseManagement makes (key, hub, generation) and the answers. */
static pb_mgmt_xfer_t hub_answers(const pb_mgmt_msg_t *req, const uint8_t *wire, size_t wire_len, bool locate_only,
                                  uint8_t *resp, size_t *resp_len)
{
    if (!pb_mgmt_verify(wire, wire_len, g_hub_cred.key, test_hmac, NULL) ||
        memcmp(req->id.hub_id, g_hub_cred.hub_id, 16) != 0 ||
        memcmp(req->id.generation, g_hub_cred.generation, 16) != 0) {
        return PB_MGMT_XFER_REJECTED; /* 4.01 */
    }
    pb_mgmt_msg_t a;
    memset(&a, 0, sizeof(a));
    switch (req->type) {
    case PB_MGMT_TYPE_LOCATE:
        *resp_len = locate_ack(req, &g_hub_endpoint, req->id.eui64, g_hub_cred.hub_id, g_hub_cred.generation,
                               g_hub_cred.key, resp);
        return PB_MGMT_XFER_ANSWERED;
    case PB_MGMT_TYPE_POLL:
        if (locate_only) {
            return PB_MGMT_XFER_NO_ANSWER;
        }
        memcpy(a.board_nonce, req->board_nonce, PB_MGMT_NONCE_LEN);
        if (g_hub_command_pending && g_hub_has_nonce &&
            memcmp(req->hub_nonce, g_hub_last_nonce, PB_MGMT_NONCE_LEN) == 0) {
            a.type = PB_MGMT_TYPE_UNENROLL;
            memcpy(a.command_id, g_hub_command_id, PB_MGMT_COMMAND_ID_LEN);
        } else {
            a.type = PB_MGMT_TYPE_IDLE;
            seq(g_hub_last_nonce, PB_MGMT_NONCE_LEN, g_hub_nonce_counter);
            g_hub_nonce_counter = (uint8_t)(g_hub_nonce_counter + 0x11);
            g_hub_has_nonce = true;
            memcpy(a.hub_nonce, g_hub_last_nonce, PB_MGMT_NONCE_LEN);
            a.flags = g_hub_command_pending ? PB_MGMT_IDLE_F_COMMAND_PENDING : 0;
        }
        *resp_len = pack_as(&a, req->id.eui64, g_hub_cred.hub_id, g_hub_cred.generation, g_hub_cred.key, resp);
        return PB_MGMT_XFER_ANSWERED;
    case PB_MGMT_TYPE_HELLO:
        if (locate_only) {
            return PB_MGMT_XFER_NO_ANSWER;
        }
        a.type = PB_MGMT_TYPE_HELLO_ACK;
        a.capabilities = PB_MGMT_HUB_CAP_THREAD_RECEIPT | PB_MGMT_HUB_CAP_ENDPOINT_LOCATE;
        *resp_len = pack_as(&a, req->id.eui64, g_hub_cred.hub_id, g_hub_cred.generation, g_hub_cred.key, resp);
        return PB_MGMT_XFER_ANSWERED;
    case PB_MGMT_TYPE_UNENROLL_ACCEPTED:
    case PB_MGMT_TYPE_UNENROLL_DONE:
        if (locate_only) {
            return PB_MGMT_XFER_NO_ANSWER;
        }
        if (req->type == PB_MGMT_TYPE_UNENROLL_DONE) {
            if (req->result == PB_MGMT_RESULT_ERASED) {
                g_hub_done_erased++;
            } else {
                g_hub_done_released++;
            }
            g_hub_command_pending = false;
        }
        a.type = PB_MGMT_TYPE_RESULT_ACK;
        memcpy(a.command_id, req->command_id, PB_MGMT_COMMAND_ID_LEN);
        a.acked_type = req->type;
        a.acked_result = req->type == PB_MGMT_TYPE_UNENROLL_DONE ? req->result : 0;
        *resp_len = pack_as(&a, req->id.eui64, g_hub_cred.hub_id, g_hub_cred.generation, g_hub_cred.key, resp);
        return PB_MGMT_XFER_ANSWERED;
    default:
        return PB_MGMT_XFER_REJECTED;
    }
}

static pb_mgmt_xfer_t listener_answers(const listener_t *l, const pb_mgmt_msg_t *req, const uint8_t *wire,
                                       size_t wire_len, uint8_t *resp, size_t *resp_len)
{
    uint8_t other[32];
    pb_enroll_mgmt_credential_t alien;
    seq(alien.hub_id, 16, 0x90);
    seq(alien.generation, 16, 0xa0);
    seq(alien.key, 32, 0xc0);
    switch (l->kind) {
    case L_SILENT:
        return PB_MGMT_XFER_NO_ANSWER;
    case L_REFUSING:
        return PB_MGMT_XFER_REJECTED;
    case L_HUB:
        return hub_answers(req, wire, wire_len, false, resp, resp_len);
    case L_HUB_LOCATE_ONLY:
        return hub_answers(req, wire, wire_len, true, resp, resp_len);
    /* The four below name EP_NEW2: an endpoint this board COULD store (an
     * EID of its own network), and the test puts the same kind of node
     * there too, so the confirmation would succeed. The only thing between
     * these answers and a move is the check each one fails. */
    case L_OTHER_HUB:
        /* A real hub of another home: it does not know this board. For the
         * worst case it answers anyway, with its own identity and key. */
        *resp_len = locate_ack(req, &EP_NEW2, req->id.eui64, alien.hub_id, alien.generation, alien.key, resp);
        return PB_MGMT_XFER_ANSWERED;
    case L_WRONG_KEY:
        *resp_len = locate_ack(req, &EP_NEW2, req->id.eui64, g_hub_cred.hub_id, g_hub_cred.generation, alien.key, resp);
        return PB_MGMT_XFER_ANSWERED;
    case L_OTHER_GENERATION:
        seq(other, 16, 0x55);
        *resp_len = locate_ack(req, &EP_NEW2, req->id.eui64, g_hub_cred.hub_id, other, g_hub_cred.key, resp);
        return PB_MGMT_XFER_ANSWERED;
    case L_OTHER_BOARD:
        seq(other, 8, 0x61);
        *resp_len = locate_ack(req, &EP_NEW2, other, g_hub_cred.hub_id, g_hub_cred.generation, g_hub_cred.key, resp);
        return PB_MGMT_XFER_ANSWERED;
    case L_REPLAYER:
        if (g_recorded_ack_len == 0) {
            return PB_MGMT_XFER_NO_ANSWER;
        }
        memcpy(resp, g_recorded_ack, g_recorded_ack_len);
        *resp_len = g_recorded_ack_len;
        return PB_MGMT_XFER_ANSWERED;
    case L_TAMPERER:
        if (hub_answers(req, wire, wire_len, false, resp, resp_len) == PB_MGMT_XFER_ANSWERED &&
            req->type == PB_MGMT_TYPE_LOCATE) {
            /* The address in the answer becomes one of this node's choosing
             * (storable, and the same node answers there). */
            memcpy(&resp[PB_MGMT_HEADER_LEN + 16], EP_NEW2.addr, 16);
        }
        return PB_MGMT_XFER_ANSWERED;
    case L_GARBAGE:
        memset(resp, 0x5a, PB_MGMT_LOCATE_ACK_LEN);
        *resp_len = PB_MGMT_LOCATE_ACK_LEN;
        return PB_MGMT_XFER_ANSWERED;
    case L_HUB_NAMES:
        if (req->type != PB_MGMT_TYPE_LOCATE || !pb_mgmt_verify(wire, wire_len, g_hub_cred.key, test_hmac, NULL)) {
            return PB_MGMT_XFER_NO_ANSWER;
        }
        *resp_len = locate_ack(req, &l->names, req->id.eui64, g_hub_cred.hub_id, g_hub_cred.generation,
                               g_hub_cred.key, resp);
        return PB_MGMT_XFER_ANSWERED;
    case L_WRONG_TYPE: {
        pb_mgmt_msg_t a;
        memset(&a, 0, sizeof(a));
        a.type = PB_MGMT_TYPE_IDLE;
        memcpy(a.board_nonce, req->board_nonce, PB_MGMT_NONCE_LEN);
        /* Where a LOCATE_ACK carries its address, this IDLE carries EP_NEW2's. */
        memcpy(a.hub_nonce, EP_NEW2.addr, PB_MGMT_NONCE_LEN);
        *resp_len = pack_as(&a, req->id.eui64, g_hub_cred.hub_id, g_hub_cred.generation, g_hub_cred.key, resp);
        return PB_MGMT_XFER_ANSWERED;
    }
    }
    return PB_MGMT_XFER_NO_ANSWER;
}

static pb_mgmt_xfer_t net_exchange(const pb_enroll_bridge_endpoint_t *endpoint, const uint8_t *req, size_t req_len,
                                   uint8_t *resp, size_t resp_cap, size_t *resp_len, void *ctx)
{
    (void)ctx;
    (void)resp_cap;
    *resp_len = 0;
    pb_mgmt_msg_t m;
    if (!pb_mgmt_parse(req, req_len, &m)) {
        return PB_MGMT_XFER_REJECTED;
    }
    if (g_sent_count < SENT_MAX) {
        g_sent[g_sent_count].to = *endpoint;
        g_sent[g_sent_count].type = m.type;
        memcpy(g_sent[g_sent_count].nonce, m.board_nonce, PB_MGMT_NONCE_LEN);
        g_sent_count++;
    }
    pb_mgmt_xfer_t x = PB_MGMT_XFER_NO_ANSWER;
    uint8_t now[PB_MGMT_MAX_MESSAGE_LEN];
    size_t now_len = 0;
    for (size_t i = 0; i < g_listener_count; i++) {
        if (same_ep(&g_listeners[i].at, endpoint)) {
            x = listener_answers(&g_listeners[i], &m, req, req_len, now, &now_len);
            break;
        }
    }
    if (x == PB_MGMT_XFER_ANSWERED && m.type == PB_MGMT_TYPE_LOCATE && now_len == PB_MGMT_LOCATE_ACK_LEN) {
        memcpy(g_recorded_ack, now, now_len); /* what an eavesdropper keeps */
        g_recorded_ack_len = now_len;
    }
    if (g_answers_one_late && m.type == PB_MGMT_TYPE_LOCATE) {
        /* This request gets the answer that was meant for the one before. */
        uint8_t held[PB_MGMT_MAX_MESSAGE_LEN];
        size_t held_len = g_late_answer_len;
        memcpy(held, g_late_answer, held_len);
        g_late_answer_len = (x == PB_MGMT_XFER_ANSWERED) ? now_len : 0;
        memcpy(g_late_answer, now, g_late_answer_len);
        if (held_len == 0) {
            return PB_MGMT_XFER_NO_ANSWER;
        }
        memcpy(resp, held, held_len);
        *resp_len = held_len;
        return PB_MGMT_XFER_ANSWERED;
    }
    if (x == PB_MGMT_XFER_ANSWERED) {
        memcpy(resp, now, now_len);
        *resp_len = now_len;
    }
    return x;
}

static size_t sent_of_type(uint8_t type)
{
    size_t n = 0;
    for (size_t i = 0; i < g_sent_count; i++) {
        n += g_sent[i].type == type ? 1 : 0;
    }
    return n;
}

static size_t sent_to(const pb_enroll_bridge_endpoint_t *ep, uint8_t type)
{
    size_t n = 0;
    for (size_t i = 0; i < g_sent_count; i++) {
        n += (g_sent[i].type == type && same_ep(&g_sent[i].to, ep)) ? 1 : 0;
    }
    return n;
}

/* ======================= the platform ======================= */

static uint8_t g_hints[PB_MGMT_LOCATE_MAX_HINTS + 32][PB_ENROLL_BRIDGE_ADDR_LEN];
static size_t g_hint_count;
static int g_hint_calls;
static size_t g_hint_cap_seen;
static bool g_locate_allowed = true;
static int g_moved_calls;
static pb_enroll_bridge_endpoint_t g_moved_to;
static bool g_record_had_new_endpoint_when_told;
static int g_quiesce_calls;
static uint8_t g_random_counter = 0xb0;
static bool g_network_erased = true;
static int g_exit_in_endpoint_changed; /* power loss right after the platform was told */

static void hints_reset(void)
{
    g_hint_count = 0;
    g_hint_calls = 0;
    g_hint_cap_seen = 0;
}

static void hint_rloc(uint16_t rloc16)
{
    (void)pb_mgmt_locator_address(NET.mesh_local_prefix, rloc16, g_hints[g_hint_count++]);
}

static bool op_random(uint8_t *buf, size_t len, void *ctx)
{
    (void)ctx;
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(g_random_counter + i);
    }
    g_random_counter = (uint8_t)(g_random_counter + 0x07);
    if (g_random_counter == 0) {
        g_random_counter = 1;
    }
    return true;
}

static size_t op_hints(uint8_t (*out)[PB_ENROLL_BRIDGE_ADDR_LEN], size_t cap, void *ctx)
{
    (void)ctx;
    g_hint_calls++;
    g_hint_cap_seen = cap;
    size_t n = g_hint_count < cap ? g_hint_count : cap;
    memcpy(out, g_hints, n * PB_ENROLL_BRIDGE_ADDR_LEN);
    /* A platform that says it wrote more than there was room for. */
    return g_hint_count;
}

static void op_endpoint_changed(const pb_enroll_bridge_endpoint_t *endpoint, void *ctx)
{
    (void)ctx;
    g_moved_calls++;
    g_moved_to = *endpoint;
    pb_enroll_record_t rec;
    g_record_had_new_endpoint_when_told = pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_VALID &&
                                          rec.has_bridge_endpoint && same_ep(&rec.bridge_endpoint, endpoint);
    if (g_exit_in_endpoint_changed != 0) {
        _exit(g_exit_in_endpoint_changed);
    }
}

static bool op_locate_allowed(void *ctx)
{
    (void)ctx;
    return g_locate_allowed;
}

static void op_quiesce(void *ctx)
{
    (void)ctx;
    g_quiesce_calls++;
}

static void op_delay(uint32_t ms, void *ctx)
{
    (void)ms;
    (void)ctx;
}

static bool op_network_up(void *ctx)
{
    (void)ctx;
    return true;
}

static esp_err_t op_network_erase(void *ctx)
{
    (void)ctx;
    g_network_erased = true;
    return ESP_OK;
}

static bool op_network_erased(void *ctx)
{
    (void)ctx;
    return g_network_erased;
}

/* A platform that can look for its hub (no receipt session: not what this
 * file is about). */
static const pb_mgmt_ops_t k_ops = {
    .hmac = test_hmac,
    .random = op_random,
    .exchange = net_exchange,
    .quiesce = op_quiesce,
    .delay_ms = op_delay,
    .network_up_for_proof = op_network_up,
    .network_erase = op_network_erase,
    .network_erased = op_network_erased,
    .locate_hints = op_hints,
    .endpoint_changed = op_endpoint_changed,
    .locate_allowed = op_locate_allowed,
};

/* The same platform without the two locate operations. */
static const pb_mgmt_ops_t k_ops_no_locate = {
    .hmac = test_hmac,
    .random = op_random,
    .exchange = net_exchange,
    .quiesce = op_quiesce,
    .delay_ms = op_delay,
    .network_up_for_proof = op_network_up,
    .network_erase = op_network_erase,
    .network_erased = op_network_erased,
};

/* ======================= the flash ======================= */

static pb_enroll_record_t enrolled_record(const pb_enroll_bridge_endpoint_t *ep)
{
    pb_enroll_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.id = NET;
    rec.has_bridge_endpoint = true;
    rec.bridge_endpoint = *ep;
    rec.has_mgmt = true;
    rec.mgmt_state = PB_ENROLL_MGMT_ENROLLED;
    rec.mgmt = golden_credential();
    return rec;
}

static size_t raw_record(uint8_t out[PB_ENROLL_RECORD_MAX_LEN])
{
    memset(out, 0, PB_ENROLL_RECORD_MAX_LEN);
    int n = nvs_sim_peek_blob("nvs", PB_ENROLL_NVS_NAMESPACE, PB_ENROLL_NVS_KEY, out, PB_ENROLL_RECORD_MAX_LEN);
    return n < 0 ? 0 : (size_t)n;
}

/* The stored record is exactly the enrollment `before`, with `ep` as its
 * endpoint and nothing else different: compared as the bytes a fresh store
 * of that record gives. */
static bool stored_is(const pb_enroll_bridge_endpoint_t *ep)
{
    uint8_t now[PB_ENROLL_RECORD_MAX_LEN];
    size_t now_len = raw_record(now);
    pb_enroll_record_t loaded;
    if (now_len != PB_ENROLL_RECORD_V3_LEN || pb_enroll_record_load(EUI_A, &loaded) != PB_ENROLL_MARKER_VALID) {
        return false;
    }
    pb_enroll_mgmt_credential_t cred = golden_credential();
    return loaded.has_mgmt && loaded.mgmt_state == PB_ENROLL_MGMT_ENROLLED && loaded.release_flags == 0 &&
           loaded.has_bridge_endpoint && same_ep(&loaded.bridge_endpoint, ep) &&
           memcmp(&loaded.id, &NET, sizeof(NET)) == 0 && memcmp(loaded.mgmt.hub_id, cred.hub_id, 16) == 0 &&
           memcmp(loaded.mgmt.generation, cred.generation, 16) == 0 && memcmp(loaded.mgmt.key, cred.key, 32) == 0 &&
           pb_mgmt_is_zero(loaded.command_id, sizeof(loaded.command_id)) &&
           pb_mgmt_is_zero(loaded.done_mac, sizeof(loaded.done_mac));
}

static pb_mgmt_client_t g_client;

/* A board enrolled with the golden credential at `ep`, its services
 * running: the record is in flash and the management client is armed. */
static void board_enrolled_at(const pb_enroll_bridge_endpoint_t *ep, const pb_mgmt_ops_t *ops)
{
    pb_enroll_record_t rec = enrolled_record(ep);
    nvs_sim_reset();
    (void)pb_enroll_record_store(EUI_A, &rec);
    hints_reset();
    g_locate_allowed = true;
    g_moved_calls = 0;
    g_record_had_new_endpoint_when_told = false;
    g_quiesce_calls = 0;
    g_exit_in_endpoint_changed = 0;
    pb_mgmt_client_init(&g_client, ops, EUI_A);
    (void)pb_mgmt_client_load(&g_client);
}

static pb_mgmt_poll_result_t poll_times(int n)
{
    pb_mgmt_poll_result_t last = PB_MGMT_POLL_NOT_ARMED;
    for (int i = 0; i < n; i++) {
        last = pb_mgmt_client_poll(&g_client);
    }
    return last;
}

/* Polls until one says the endpoint moved, at most `limit` times. Returns
 * how many polls that took, 0 if none did. */
static int polls_until_moved(int limit)
{
    for (int i = 1; i <= limit; i++) {
        if (pb_mgmt_client_poll(&g_client) == PB_MGMT_POLL_ENDPOINT_MOVED) {
            return i;
        }
    }
    return 0;
}

/* ======================= scenarios ======================= */

static void scenario_wire_format(void)
{
    printf("\n== wire: LOCATE and LOCATE_ACK ==\n");
    pb_enroll_mgmt_credential_t cred = golden_credential();
    uint8_t wire[PB_MGMT_MAX_MESSAGE_LEN];
    char mac_hex[65];
    pb_mgmt_msg_t m, back;

    memset(&m, 0, sizeof(m));
    m.type = PB_MGMT_TYPE_LOCATE;
    seq(m.board_nonce, PB_MGMT_NONCE_LEN, 0xb0);
    size_t n = pack_as(&m, EUI_A, cred.hub_id, cred.generation, cred.key, wire);
    CHECK(n == PB_MGMT_LOCATE_LEN && n == 96, "LOCATE is 96 bytes");
    hex(pb_mgmt_mac_of(wire, n), 32, mac_hex);
    /* Computed separately with Python's hmac over the documented layout. */
    CHECK(strcmp(mac_hex, "5ceec372200fa66bf2904adb8d47502e1b314bbee8ef110d19789272f4989231") == 0,
          "LOCATE golden MAC (EUI 6055f9fffe001234, hub 10..1f, generation 20..2f, key 30..4f, nonce b0..bf)");
    CHECK(wire[0] == 1 && wire[1] == 0x06 && wire[2] == 0 && wire[3] == 0 && wire[44] == 0xb0 && wire[59] == 0xbf &&
              pb_mgmt_is_zero(&wire[60], 4),
          "LOCATE layout: version 1, type 0x06, nonce at 44, four zero bytes");
    CHECK(pb_mgmt_parse(wire, n, &back) && back.type == PB_MGMT_TYPE_LOCATE &&
              memcmp(back.board_nonce, m.board_nonce, 16) == 0 && pb_mgmt_verify(wire, n, cred.key, test_hmac, NULL),
          "LOCATE parses back and verifies");
    wire[61] = 1;
    CHECK(!pb_mgmt_parse(wire, n, &back), "LOCATE with a reserved byte set is malformed");
    wire[61] = 0;
    CHECK(!pb_mgmt_parse(wire, n - 1, &back) && !pb_mgmt_parse(wire, n + 1, &back), "LOCATE of another length is malformed");
    memset(&m.board_nonce, 0, sizeof(m.board_nonce));
    n = pack_as(&m, EUI_A, cred.hub_id, cred.generation, cred.key, wire);
    CHECK(n == PB_MGMT_LOCATE_LEN && !pb_mgmt_parse(wire, n, &back), "LOCATE with a nonce of zeros is malformed");

    memset(&m, 0, sizeof(m));
    m.type = PB_MGMT_TYPE_LOCATE_ACK;
    seq(m.board_nonce, PB_MGMT_NONCE_LEN, 0xb0);
    memcpy(m.endpoint_addr, EP_OLD.addr, 16);
    m.endpoint_port = 5684;
    n = pack_as(&m, EUI_A, cred.hub_id, cred.generation, cred.key, wire);
    CHECK(n == PB_MGMT_LOCATE_ACK_LEN && n == 112, "LOCATE_ACK is 112 bytes");
    hex(pb_mgmt_mac_of(wire, n), 32, mac_hex);
    CHECK(strcmp(mac_hex, "6fa4e266b7ef48249a301bb5639a81953cb0cd7494dcd29b69d3eaa271faf831") == 0,
          "LOCATE_ACK golden MAC (the same, address fd11:2233:4455:0:8c1f:2e3d:4c5b:6a79, port 5684)");
    CHECK(wire[1] == 0x86 && memcmp(&wire[60], EP_OLD.addr, 16) == 0 && wire[76] == 0x16 && wire[77] == 0x34 &&
              wire[78] == 0 && wire[79] == 0,
          "LOCATE_ACK layout: nonce at 44, address at 60, port big-endian at 76, two zero bytes");
    CHECK(pb_mgmt_parse(wire, n, &back) && back.endpoint_port == 5684 &&
              memcmp(back.endpoint_addr, EP_OLD.addr, 16) == 0,
          "LOCATE_ACK parses back");
    uint8_t changed[PB_MGMT_MAX_MESSAGE_LEN];
    memcpy(changed, wire, n);
    changed[75] ^= 0x01;
    CHECK(pb_mgmt_parse(changed, n, &back) && !pb_mgmt_verify(changed, n, cred.key, test_hmac, NULL),
          "a changed address byte still parses and no longer verifies");
    memcpy(changed, wire, n);
    changed[77] ^= 0x01;
    CHECK(!pb_mgmt_verify(changed, n, cred.key, test_hmac, NULL), "a changed port no longer verifies");
    memcpy(changed, wire, n);
    changed[78] = 1;
    CHECK(!pb_mgmt_parse(changed, n, &back), "LOCATE_ACK with a reserved byte set is malformed");
    m.endpoint_port = 0;
    CHECK(pack_as(&m, EUI_A, cred.hub_id, cred.generation, cred.key, changed) == 0, "port 0 cannot be encoded");
    memcpy(changed, wire, n);
    changed[76] = changed[77] = 0;
    CHECK(!pb_mgmt_parse(changed, n, &back), "LOCATE_ACK with port 0 is malformed");
    m.endpoint_port = 5684;
    memset(m.endpoint_addr, 0, 16);
    CHECK(pack_as(&m, EUI_A, cred.hub_id, cred.generation, cred.key, changed) == 0,
          "the unspecified address cannot be encoded");
    CHECK(pb_mgmt_message_len(PB_MGMT_TYPE_LOCATE) == 96 && pb_mgmt_message_len(PB_MGMT_TYPE_LOCATE_ACK) == 112 &&
              PB_MGMT_MAX_MESSAGE_LEN == 112,
          "no message is longer than before (112 bytes)");

    uint8_t addr[16];
    static const uint8_t want[16] = {ML_PREFIX, 0x00, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x30, 0x00};
    CHECK(pb_mgmt_locator_address(NET.mesh_local_prefix, 0x3000, addr) && memcmp(addr, want, 16) == 0,
          "locator address: prefix | 0000:00ff:fe00:3000");
    CHECK(!pb_mgmt_locator_address(NET.mesh_local_prefix, 0xfffe, addr) &&
              !pb_mgmt_locator_address(NET.mesh_local_prefix, 0xffff, addr),
          "0xfffe and 0xffff name no node");
    pb_enroll_bridge_endpoint_t loc = locator(0x3000, 5684);
    CHECK(pb_enroll_bridge_endpoint_check(&loc) == PB_ENROLL_ENDPOINT_LOCATOR,
          "a locator is refused as a stored endpoint by the enrollment record's own rule");
}

static void scenario_healthy_hub_is_left_alone(void)
{
    printf("\n== a hub that answers: nothing is looked for, nothing is written ==\n");
    net_reset();
    hub_at(EP_OLD, HUB_RLOC16);
    board_enrolled_at(&EP_OLD, &k_ops);
    hint_rloc(HUB_RLOC16);
    int commits = nvs_sim_commit_count();
    CHECK(poll_times(200) == PB_MGMT_POLL_IDLE, "200 polls, each answered");
    CHECK(sent_of_type(PB_MGMT_TYPE_LOCATE) == 0, "no LOCATE was ever sent");
    CHECK(g_hint_calls == 0, "the platform was never asked for hints");
    CHECK(nvs_sim_commit_count() == commits && g_moved_calls == 0 && stored_is(&EP_OLD),
          "no flash write, no endpoint change, the record is as it was");
    CHECK(sent_of_type(PB_MGMT_TYPE_HELLO) == 1, "HELLO was sent once");
}

static void scenario_short_outage(void)
{
    printf("\n== a hub that is silent for under a minute, and one that is paused for long ==\n");
    net_reset();
    board_enrolled_at(&EP_OLD, &k_ops); /* nothing listens: the hub is down */
    hint_rloc(HUB_RLOC16);
    (void)poll_times(PB_MGMT_LOCATE_AFTER_SILENT_POLLS - 1);
    CHECK(sent_of_type(PB_MGMT_TYPE_LOCATE) == 0 && g_hint_calls == 0,
          "5 silent polls: nothing is looked for yet");
    hub_at(EP_OLD, HUB_RLOC16);
    CHECK(poll_times(1) == PB_MGMT_POLL_IDLE && g_client.silent_polls == 0, "the hub answers again: counted from zero");
    g_listener_count = 0; /* down again */
    (void)poll_times(PB_MGMT_LOCATE_AFTER_SILENT_POLLS - 1);
    CHECK(sent_of_type(PB_MGMT_TYPE_LOCATE) == 0, "another 5 silent polls after an answer: still nothing");

    /* A long pause: rounds at silent poll 6, 12, 24, 48, 96, 186, 276, ... */
    net_reset();
    board_enrolled_at(&EP_OLD, &k_ops);
    hint_rloc(HUB_RLOC16);
    hint_rloc(0x4400);
    int commits = nvs_sim_commit_count();
    static const int round_at[] = {6, 12, 24, 48, 96, 186, 276};
    bool schedule_ok = true;
    bool bounded = true;
    int rounds = 0;
    for (int poll = 1; poll <= 300; poll++) {
        size_t before = sent_of_type(PB_MGMT_TYPE_LOCATE);
        int hint_calls_before = g_hint_calls;
        (void)pb_mgmt_client_poll(&g_client);
        size_t probes = sent_of_type(PB_MGMT_TYPE_LOCATE) - before;
        bool expected = rounds < 7 && poll == round_at[rounds];
        if ((probes != 0) != expected || (g_hint_calls != hint_calls_before) != expected) {
            schedule_ok = false;
        }
        if (expected) {
            rounds++;
        }
        if (probes > PB_MGMT_LOCATE_MAX_PROBES) {
            bounded = false;
        }
        if (g_sent_count > SENT_MAX - 16) {
            g_sent_count = 0;
        }
    }
    CHECK(schedule_ok && rounds == 7, "rounds run at silent poll 6, 12, 24, 48, 96, 186, 276 and at no other poll");
    CHECK(bounded, "no round sends more than PB_MGMT_LOCATE_MAX_PROBES requests");
    CHECK(g_client.locate_probes == 7u * 3u, "each round: the stored address and each of two hints, once (the stored "
                                             "port is the locate port here) -- 3 requests");
    CHECK(nvs_sim_commit_count() == commits && g_moved_calls == 0 && stored_is(&EP_OLD),
          "five hours of silence: no flash write, no endpoint change, the record is as it was");
    CHECK(g_client.last_locate == PB_MGMT_LOCATE_NOT_FOUND, "every round ended as 'not found'");

    hub_at(EP_OLD, HUB_RLOC16);
    CHECK(poll_times(1) == PB_MGMT_POLL_IDLE && g_client.locate_rounds == 0 &&
              g_client.next_locate_at == PB_MGMT_LOCATE_AFTER_SILENT_POLLS,
          "the hub is back at the same endpoint: the next poll is answered and the schedule starts over");
    CHECK(stored_is(&EP_OLD) && g_moved_calls == 0, "nothing was changed for a hub that only paused");
}

static void expect_move(const char *label, const pb_enroll_bridge_endpoint_t *to, int commits_before)
{
    char msg[200];
    snprintf(msg, sizeof(msg), "%s: the record holds the new endpoint, and nothing else in it changed", label);
    CHECK(stored_is(to), msg);
    snprintf(msg, sizeof(msg), "%s: exactly one flash write", label);
    CHECK(nvs_sim_commit_count() == commits_before + 1, msg);
    snprintf(msg, sizeof(msg), "%s: the platform was told once, the new endpoint, after it was in flash", label);
    CHECK(g_moved_calls == 1 && same_ep(&g_moved_to, to) && g_record_had_new_endpoint_when_told, msg);
    size_t polls_to_new = sent_to(to, PB_MGMT_TYPE_POLL);
    pb_mgmt_poll_result_t next = pb_mgmt_client_poll(&g_client);
    snprintf(msg, sizeof(msg), "%s: the next poll goes to the new endpoint and is answered", label);
    CHECK(next == PB_MGMT_POLL_IDLE && sent_to(to, PB_MGMT_TYPE_POLL) == polls_to_new + 1 &&
              same_ep(&g_client.rec.bridge_endpoint, to),
          msg);
}

static void scenario_address_changed(void)
{
    printf("\n== the hub's Thread address changed (same hub, same network, same credential) ==\n");
    net_reset();
    hub_at(EP_NEW_ADDR, HUB_RLOC16_AFTER); /* nothing at the old address any more */
    board_enrolled_at(&EP_OLD, &k_ops);
    hint_rloc(0x4400);              /* some other router: silent */
    hint_rloc(HUB_RLOC16_AFTER);    /* the hub's host, as the Network Data names it */
    int commits = nvs_sim_commit_count();
    int took = polls_until_moved(20);
    CHECK(took == (int)PB_MGMT_LOCATE_AFTER_SILENT_POLLS, "found in the first round, at the 6th silent poll");
    pb_enroll_bridge_endpoint_t hub_locator = locator(HUB_RLOC16_AFTER, 5684);
    CHECK(sent_to(&hub_locator, PB_MGMT_TYPE_LOCATE) == 1 && sent_to(&EP_NEW_ADDR, PB_MGMT_TYPE_LOCATE) == 1,
          "asked at the hint (a locator), then once more at the endpoint the hub named");
    CHECK(sent_of_type(PB_MGMT_TYPE_LOCATE) == 4, "four requests in all: stored address, the silent router, the hub's "
                                                  "locator, the confirmation");
    expect_move("address change", &EP_NEW_ADDR, commits);
    CHECK(g_client.locate_moves == 1 && g_client.silent_polls == 0, "counted as one move; the silence count starts over");
}

static void scenario_port_changed(void)
{
    printf("\n== the hub's ingest port changed ==\n");
    /* 5684 -> 5700: the hub keeps a locate-only socket on 5684. */
    net_reset();
    hub_at(EP_NEW_PORT, HUB_RLOC16);
    board_enrolled_at(&EP_OLD, &k_ops); /* no hints at all */
    int commits = nvs_sim_commit_count();
    CHECK(polls_until_moved(20) == (int)PB_MGMT_LOCATE_AFTER_SILENT_POLLS, "found in the first round, without any hint");
    CHECK(sent_to(&EP_OLD, PB_MGMT_TYPE_LOCATE) == 1 && sent_to(&EP_NEW_PORT, PB_MGMT_TYPE_LOCATE) == 1 &&
              sent_of_type(PB_MGMT_TYPE_LOCATE) == 2,
          "asked on the locate port of the stored address, confirmed on the new port: two requests");
    expect_move("default port -> another port", &EP_NEW_PORT, commits);

    /* 5700 -> 5684. */
    net_reset();
    hub_at(EP_OLD, HUB_RLOC16);
    board_enrolled_at(&EP_NEW_PORT, &k_ops);
    commits = nvs_sim_commit_count();
    CHECK(polls_until_moved(20) == (int)PB_MGMT_LOCATE_AFTER_SILENT_POLLS, "back to the default port: found");
    CHECK(sent_of_type(PB_MGMT_TYPE_LOCATE) == 1, "one request: the answer came from the endpoint it names");
    expect_move("another port -> default port", &EP_OLD, commits);

    /* 5700 -> 5701. */
    net_reset();
    pb_enroll_bridge_endpoint_t ep_5701 = EP_OLD;
    ep_5701.port = 5701;
    hub_at(ep_5701, HUB_RLOC16);
    board_enrolled_at(&EP_NEW_PORT, &k_ops);
    commits = nvs_sim_commit_count();
    CHECK(polls_until_moved(20) == (int)PB_MGMT_LOCATE_AFTER_SILENT_POLLS, "one non-default port to another: found");
    expect_move("another port -> a third port", &ep_5701, commits);

    /* Address and port at once. */
    net_reset();
    hub_at(EP_NEW_BOTH, HUB_RLOC16_AFTER);
    board_enrolled_at(&EP_OLD, &k_ops);
    hint_rloc(HUB_RLOC16_AFTER);
    commits = nvs_sim_commit_count();
    CHECK(polls_until_moved(20) == (int)PB_MGMT_LOCATE_AFTER_SILENT_POLLS, "address and port both changed: found");
    expect_move("address and port", &EP_NEW_BOTH, commits);
}

static void scenario_hub_at_stored_endpoint_only_locate(void)
{
    printf("\n== LOCATE is answered at the stored endpoint, POLL is not ==\n");
    net_reset();
    /* The hub is where the record says, and says so; its polls go
     * unanswered for another reason (here: only its locate socket is up). */
    g_hub_endpoint = EP_OLD;
    listen_at(EP_OLD, L_HUB_LOCATE_ONLY);
    board_enrolled_at(&EP_OLD, &k_ops);
    int commits = nvs_sim_commit_count();
    CHECK(poll_times(30) == PB_MGMT_POLL_NO_ANSWER && g_client.last_locate == PB_MGMT_LOCATE_SAME_ENDPOINT,
          "the hub confirms the stored endpoint: 'same endpoint'");
    CHECK(nvs_sim_commit_count() == commits && g_moved_calls == 0 && stored_is(&EP_OLD),
          "nothing is written and nothing is moved for the endpoint that is already stored");
}

static void scenario_wrong_answers(void)
{
    printf("\n== answers that are not the hub's ==\n");
    static const struct {
        listener_kind_t kind;
        const char *what;
    } cases[] = {
        {L_OTHER_HUB, "another hub (its own id and key)"},
        {L_WRONG_KEY, "this hub's id, another key"},
        {L_OTHER_GENERATION, "this hub's key, another enrollment generation"},
        {L_OTHER_BOARD, "this hub's key, another board's EUI-64"},
        {L_TAMPERER, "this hub's answer with the address changed on the way"},
        {L_GARBAGE, "a 2.xx answer that is no message"},
        {L_REFUSING, "a server that refuses (4.xx; also what a hub without LOCATE does)"},
        {L_WRONG_TYPE, "this hub's key on another message type"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        net_reset();
        board_enrolled_at(&EP_OLD, &k_ops);
        hint_rloc(0x7000);
        pb_enroll_bridge_endpoint_t there = locator(0x7000, 5684);
        if (cases[i].kind == L_TAMPERER) {
            /* It can reach the real hub, which is at the new address. */
            g_hub_endpoint = EP_NEW_ADDR;
        }
        /* The same node sits at the endpoint these answers name. */
        listen_at(there, cases[i].kind);
        listen_at(EP_NEW2, cases[i].kind); /* and whatever it might name answers the same way */
        int commits = nvs_sim_commit_count();
        (void)poll_times(100);
        char msg[200];
        snprintf(msg, sizeof(msg), "%s: never taken over (record, flash and platform untouched)", cases[i].what);
        CHECK(sent_to(&there, PB_MGMT_TYPE_LOCATE) >= 3 && nvs_sim_commit_count() == commits && g_moved_calls == 0 &&
                  stored_is(&EP_OLD) && same_ep(&g_client.rec.bridge_endpoint, &EP_OLD),
              msg);
    }

    printf("-- a recorded LOCATE_ACK played again --\n");
    net_reset();
    hub_at(EP_NEW_ADDR, HUB_RLOC16_AFTER);
    board_enrolled_at(&EP_OLD, &k_ops);
    hint_rloc(HUB_RLOC16_AFTER);
    CHECK(polls_until_moved(20) != 0 && g_recorded_ack_len == PB_MGMT_LOCATE_ACK_LEN,
          "a real move happens (and its LOCATE_ACK, naming the endpoint of that time, is recorded)");
    /* Later the hub is somewhere else again and unreachable; the recording
     * is played from the old place. */
    g_listener_count = 0;
    listen_at(locator(0x7000, 5684), L_REPLAYER);
    listen_at(EP_NEW_ADDR, L_REPLAYER);
    hints_reset();
    hint_rloc(0x7000);
    int commits = nvs_sim_commit_count();
    g_moved_calls = 0;
    (void)poll_times(100);
    CHECK(nvs_sim_commit_count() == commits && g_moved_calls == 0 && stored_is(&EP_NEW_ADDR) &&
              g_client.last_locate == PB_MGMT_LOCATE_NOT_FOUND,
          "the recording answers a nonce that is not this request's: ignored");

    /* The same with a recording that names somewhere the board is NOT yet:
     * the hub was at NEW2 when it was recorded, the board has since been
     * moved back to OLD by the real hub, and the recording is played. */
    net_reset();
    hub_at(EP_NEW2, 0x7000);
    board_enrolled_at(&EP_OLD, &k_ops);
    hint_rloc(0x7000);
    CHECK(polls_until_moved(20) != 0 && stored_is(&EP_NEW2) && g_recorded_ack_len == PB_MGMT_LOCATE_ACK_LEN,
          "(a move to NEW2 is recorded)");
    g_listener_count = 0;
    hub_at(EP_OLD, HUB_RLOC16);
    hints_reset();
    hint_rloc(HUB_RLOC16);
    uint8_t keep[PB_MGMT_MAX_MESSAGE_LEN];
    memcpy(keep, g_recorded_ack, sizeof(keep));
    CHECK(polls_until_moved(20) != 0 && stored_is(&EP_OLD), "(the real hub moves the board back to OLD)");
    memcpy(g_recorded_ack, keep, sizeof(keep));
    g_listener_count = 0;
    listen_at(locator(0x7400, 5684), L_REPLAYER);
    listen_at(EP_NEW2, L_REPLAYER);
    hints_reset();
    hint_rloc(0x7400);
    commits = nvs_sim_commit_count();
    g_moved_calls = 0;
    (void)poll_times(100);
    CHECK(nvs_sim_commit_count() == commits && g_moved_calls == 0 && stored_is(&EP_OLD),
          "an old, authentic LOCATE_ACK naming an endpoint the board has left, played at the hint AND at that "
          "endpoint: not taken over");

    printf("-- an answer that arrives one request late --\n");
    net_reset();
    board_enrolled_at(&EP_OLD, &k_ops);
    /* Two nodes the hub answers through: the first names an endpoint nobody
     * is at, the second names the real one. Every answer reaches the board
     * one request late, i.e. as the answer to the NEXT request. */
    hint_rloc(0x7000);
    hint_rloc(0x7400);
    listen_naming(locator(0x7000, 5684), EP_NEW2);
    listen_naming(locator(0x7400, 5684), EP_NEW_ADDR);
    g_answers_one_late = true;
    commits = nvs_sim_commit_count();
    (void)poll_times(100);
    CHECK(nvs_sim_commit_count() == commits && g_moved_calls == 0 && stored_is(&EP_OLD),
          "an authentic answer to an EARLIER request is not an answer to this one: nothing is taken over");
}

static void scenario_claims_that_are_not_endpoints(void)
{
    printf("\n== the hub names something this board cannot use, or is not at ==\n");
    static const struct {
        pb_enroll_bridge_endpoint_t names;
        const char *what;
    } cases[] = {
        {{.addr = {0xfd, 0x99, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00, 0x12, 0x9a, 0x77, 0x01, 0xbe, 0xef, 0x40, 0x02},
          .port = 5684},
         "an address in another network's Mesh-Local Prefix"},
        {{.addr = {ML_PREFIX, 0x00, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x5c, 0x00}, .port = 5684},
         "a routing locator of this network"},
        {{.addr = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x12, 0x9a, 0x77, 0x01, 0xbe, 0xef, 0x40, 0x02}, .port = 5684},
         "a link-local address"},
        {{.addr = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0x12, 0x9a, 0x77, 0x01, 0xbe, 0xef, 0x40, 0x02}, .port = 5684},
         "a global address"},
        {{.addr = {0xff, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01}, .port = 5684}, "a multicast address"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        net_reset();
        board_enrolled_at(&EP_OLD, &k_ops);
        hint_rloc(0x7000);
        listen_naming(locator(0x7000, 5684), cases[i].names);
        listen_naming(cases[i].names, cases[i].names); /* and it really answers there */
        int commits = nvs_sim_commit_count();
        (void)poll_times(100);
        char msg[200];
        snprintf(msg, sizeof(msg), "%s, named with the hub's own key: not an endpoint -- nothing is asked there and "
                                   "nothing is stored",
                 cases[i].what);
        CHECK(sent_to(&cases[i].names, PB_MGMT_TYPE_LOCATE) == 0 && nvs_sim_commit_count() == commits &&
                  g_moved_calls == 0 && stored_is(&EP_OLD),
              msg);
    }

    printf("-- named, authentic, and nobody answers there --\n");
    net_reset();
    board_enrolled_at(&EP_OLD, &k_ops);
    hint_rloc(0x7000);
    listen_naming(locator(0x7000, 5684), EP_NEW_ADDR);
    int commits = nvs_sim_commit_count();
    (void)poll_times(100);
    CHECK(sent_to(&EP_NEW_ADDR, PB_MGMT_TYPE_LOCATE) >= 3 && nvs_sim_commit_count() == commits && g_moved_calls == 0 &&
              stored_is(&EP_OLD),
          "an endpoint the board could not reach itself is never stored");

    printf("-- the hub moves again while it is being asked --\n");
    net_reset();
    board_enrolled_at(&EP_OLD, &k_ops);
    hint_rloc(0x7000);
    listen_naming(locator(0x7000, 5684), EP_NEW_ADDR); /* "I am at NEW" */
    listen_naming(EP_NEW_ADDR, EP_NEW2);               /* at NEW: "I am at NEW2" */
    commits = nvs_sim_commit_count();
    (void)poll_times(PB_MGMT_LOCATE_AFTER_SILENT_POLLS);
    CHECK(nvs_sim_commit_count() == commits && g_moved_calls == 0 && stored_is(&EP_OLD) &&
              sent_to(&EP_NEW2, PB_MGMT_TYPE_LOCATE) == 0,
          "the two answers disagree: nothing is taken over in this round, and the third place is not chased");
    /* It has settled at NEW2 by the next round. */
    g_listener_count = 0;
    hub_at(EP_NEW2, 0x7000);
    CHECK(polls_until_moved(20) != 0 && stored_is(&EP_NEW2), "the next round finds it where it now confirms itself");
}

static void scenario_many_candidates(void)
{
    printf("\n== many hints, most of them not the hub ==\n");
    net_reset();
    hub_at(EP_NEW_ADDR, 0x9c00);
    board_enrolled_at(&EP_OLD, &k_ops);
    /* More hints than a round takes, duplicates, and addresses outside the
     * network. The hub's locator is the 7th distinct usable one. */
    hint_rloc(0x1000);
    hint_rloc(0x1000);
    memset(g_hints[g_hint_count], 0, 16);
    g_hints[g_hint_count][0] = 0xfd;
    g_hints[g_hint_count++][1] = 0x99; /* another prefix */
    hint_rloc(0x1400);
    hint_rloc(0x1800);
    hint_rloc(0x1c00);
    hint_rloc(0x9c00); /* 7th entry of 8 the platform is asked for */
    hint_rloc(0x2400);
    hint_rloc(0x2800);
    hint_rloc(0x2c00);
    hint_rloc(0x3400);
    listen_at(locator(0x1000, 5684), L_REFUSING);
    listen_at(locator(0x1400, 5684), L_OTHER_HUB);
    listen_at(locator(0x1800, 5684), L_GARBAGE);
    int commits = nvs_sim_commit_count();
    int took = polls_until_moved(20);
    CHECK(took == (int)PB_MGMT_LOCATE_AFTER_SILENT_POLLS, "the hub is found among the others, in the first round");
    CHECK(g_hint_cap_seen == PB_MGMT_LOCATE_MAX_HINTS, "the platform is asked for at most PB_MGMT_LOCATE_MAX_HINTS hints");
    CHECK(sent_of_type(PB_MGMT_TYPE_LOCATE) <= PB_MGMT_LOCATE_MAX_PROBES, "within one round's request limit");
    pb_enroll_bridge_endpoint_t dup = locator(0x1000, 5684);
    pb_enroll_bridge_endpoint_t alien = {.addr = {0xfd, 0x99}, .port = 5684};
    CHECK(sent_to(&dup, PB_MGMT_TYPE_LOCATE) == 1 && sent_to(&alien, PB_MGMT_TYPE_LOCATE) == 0,
          "a hint given twice is asked once; a hint outside the network is not asked");
    expect_move("many candidates", &EP_NEW_ADDR, commits);

    printf("-- more candidates than one round may ask --\n");
    net_reset();
    hub_at(EP_NEW_ADDR, 0x9c00);
    board_enrolled_at(&EP_OLD, &k_ops);
    pb_enroll_record_t rec = enrolled_record(&EP_NEW_PORT); /* stored port 5700: two ports per hint */
    (void)pb_enroll_record_store(EUI_A, &rec);
    (void)pb_mgmt_client_load(&g_client);
    for (uint16_t r = 0x1000; r < 0x1000 + 7 * 0x400; r = (uint16_t)(r + 0x400)) {
        hint_rloc(r);
    }
    hint_rloc(0x9c00); /* 8th: its two requests would be the 16th and 17th */
    (void)poll_times(PB_MGMT_LOCATE_AFTER_SILENT_POLLS);
    CHECK(sent_of_type(PB_MGMT_TYPE_LOCATE) == PB_MGMT_LOCATE_MAX_PROBES && g_moved_calls == 0,
          "the round stops at its request limit, the hub not yet reached");
    /* Found in the 2026-10-09 review: every round started over at the first
     * candidate, and these last two were never asked. */
    commits = nvs_sim_commit_count();
    int eventually = polls_until_moved(200);
    CHECK(eventually > 0, "later bounded rounds eventually reach the 8th valid hint on its second port");
    CHECK(eventually == 6 && g_hint_calls == 2 && g_client.locate_probes == PB_MGMT_LOCATE_MAX_PROBES + 7u,
          "in the very next round: the stored address, the five candidates the first round did not get to, and "
          "the hub's own endpoint -- 7 requests");
    expect_move("more candidates than one round", &EP_NEW_ADDR, commits);
}

/* ---- the search goes on where the last round stopped ---- */

typedef struct {
    int rounds;
    int polls;
    size_t most_in_a_round;
    bool moved;
} search_t;

/* Polls until the endpoint moved or `max_rounds` rounds have run. */
static search_t run_rounds(int max_rounds)
{
    search_t r = {0, 0, 0, false};
    while (r.rounds < max_rounds && r.polls < 20000) {
        size_t before = sent_of_type(PB_MGMT_TYPE_LOCATE);
        int hint_calls = g_hint_calls;
        pb_mgmt_poll_result_t x = pb_mgmt_client_poll(&g_client);
        r.polls++;
        size_t asked = sent_of_type(PB_MGMT_TYPE_LOCATE) - before;
        if (g_hint_calls != hint_calls) {
            r.rounds++;
        }
        if (asked > r.most_in_a_round) {
            r.most_in_a_round = asked;
        }
        if (x == PB_MGMT_POLL_ENDPOINT_MOVED) {
            r.moved = true;
            break;
        }
    }
    return r;
}

/* The n-th LOCATE (1 = first) sent at or after g_sent[from]; NULL if there
 * is none. */
static const sent_t *locate_after(size_t from, int n)
{
    for (size_t i = from; i < g_sent_count; i++) {
        if (g_sent[i].type == PB_MGMT_TYPE_LOCATE && --n == 0) {
            return &g_sent[i];
        }
    }
    return NULL;
}

static size_t locates_after(size_t from)
{
    size_t n = 0;
    for (size_t i = from; i < g_sent_count; i++) {
        n += g_sent[i].type == PB_MGMT_TYPE_LOCATE ? 1 : 0;
    }
    return n;
}

static bool sent_went_to(const sent_t *sent, pb_enroll_bridge_endpoint_t to)
{
    return sent != NULL && same_ep(&sent->to, &to);
}

/* h(i): the i-th of a row of made-up routers. */
static uint16_t h(int i)
{
    return (uint16_t)(0x1000 + i * 0x400);
}

#define FAR_HUB_RLOC16 0x9c00
/* The hub at a new address AND on a port that is not the locate port. */
static const pb_enroll_bridge_endpoint_t EP_MOVED = {.addr = EID_NEW, .port = 5700};

/* A board whose record holds port 5700 (two candidates per hint), five
 * routers that do not answer and then the hub: the hub's locator on the
 * stored port is the 12th request of the first round, and its answer names
 * EP_MOVED. */
static void hub_answers_at_the_last_request(void)
{
    net_reset();
    hub_at(EP_MOVED, FAR_HUB_RLOC16);
    board_enrolled_at(&EP_NEW_PORT, &k_ops);
    for (int i = 0; i < 5; i++) {
        hint_rloc(h(i));
    }
    hint_rloc(FAR_HUB_RLOC16);
}

static void scenario_progress_from_round_to_round(void)
{
    printf("\n== more candidates than a round may ask: the search goes on where it stopped ==\n");
    const pb_enroll_bridge_endpoint_t head = {.addr = EID_OLD, .port = PB_MGMT_LOCATE_PORT};
    char msg[240];

    printf("-- the hub is the last of 20 hints and the stored port is not the locate port: 40 candidates --\n");
    net_reset();
    hub_at(EP_NEW_ADDR, FAR_HUB_RLOC16);
    board_enrolled_at(&EP_NEW_PORT, &k_ops);
    for (int i = 0; i < 19; i++) {
        hint_rloc(h(i));
    }
    hint_rloc(FAR_HUB_RLOC16);
    int commits = nvs_sim_commit_count();
    search_t found = run_rounds(10);
    CHECK(found.moved && found.rounds == 4, "found in the 4th round (40 candidates, 11 new ones a round)");
    CHECK(found.rounds <= (40 + (int)PB_MGMT_LOCATE_MAX_PROBES - 3) / ((int)PB_MGMT_LOCATE_MAX_PROBES - 2),
          "inside the documented bound of ceil(candidates / 10) rounds");
    CHECK(found.polls == 48, "at silent poll 48: the rounds ran at 6, 12, 24 and 48 -- the pauses were kept");
    CHECK(found.most_in_a_round <= PB_MGMT_LOCATE_MAX_PROBES, "no round sent more than its 12 requests");
    bool each_once = true;
    for (int i = 0; i < 19; i++) {
        pb_enroll_bridge_endpoint_t a = locator(h(i), 5700);
        pb_enroll_bridge_endpoint_t b = locator(h(i), PB_MGMT_LOCATE_PORT);
        each_once = each_once && sent_to(&a, PB_MGMT_TYPE_LOCATE) == 1 && sent_to(&b, PB_MGMT_TYPE_LOCATE) == 1;
    }
    CHECK(each_once, "on the way every candidate in front of the hub was asked exactly once: none again, none left out");
    CHECK(sent_to(&head, PB_MGMT_TYPE_LOCATE) == 4, "and the stored address on the locate port once in every round");
    CHECK(g_hint_cap_seen == PB_MGMT_LOCATE_MAX_HINTS, "the platform is asked for all its hints every round");
    expect_move("the last of 20 hints", &EP_NEW_ADDR, commits);

    printf("-- more hints than the client takes: the hub is the last one it does take --\n");
    net_reset();
    hub_at(EP_NEW_ADDR, FAR_HUB_RLOC16);
    board_enrolled_at(&EP_OLD, &k_ops); /* stored port = locate port: one candidate per hint */
    for (int i = 0; i < (int)PB_MGMT_LOCATE_MAX_HINTS - 1; i++) {
        hint_rloc((uint16_t)(0x0401 + i));
    }
    hint_rloc(FAR_HUB_RLOC16);
    for (int i = 0; i < 10; i++) {
        hint_rloc((uint16_t)(0xa001 + i)); /* past PB_MGMT_LOCATE_MAX_HINTS */
    }
    commits = nvs_sim_commit_count();
    found = run_rounds(30);
    printf("info: %d hints: found in round %d, at silent poll %d\n", (int)PB_MGMT_LOCATE_MAX_HINTS, found.rounds,
           found.polls);
    CHECK(found.moved && found.rounds == 12, "128 candidates, 11 a round: found in round 12");
    CHECK(found.rounds <= ((int)PB_MGMT_LOCATE_MAX_HINTS + 9) / 10 && found.most_in_a_round <= PB_MGMT_LOCATE_MAX_PROBES,
          "inside the bound, and no round over its 12 requests");
    pb_enroll_bridge_endpoint_t past = locator(0xa001, PB_MGMT_LOCATE_PORT);
    CHECK(sent_to(&past, PB_MGMT_TYPE_LOCATE) == 0 && stored_is(&EP_NEW_ADDR) &&
              nvs_sim_commit_count() == commits + 1,
          "a hint past the 128th is not asked; one flash write, the new endpoint");

    printf("-- the hub's answer comes with the round's last request --\n");
    hub_answers_at_the_last_request();
    commits = nvs_sim_commit_count();
    (void)poll_times(PB_MGMT_LOCATE_AFTER_SILENT_POLLS);
    pb_enroll_bridge_endpoint_t hub_locator = locator(FAR_HUB_RLOC16, 5700);
    CHECK(sent_of_type(PB_MGMT_TYPE_LOCATE) == PB_MGMT_LOCATE_MAX_PROBES &&
              sent_went_to(locate_after(0, (int)PB_MGMT_LOCATE_MAX_PROBES), hub_locator),
          "the 12th request of the round is the one the hub answers, from its routing locator");
    CHECK(g_client.last_locate == PB_MGMT_LOCATE_UNCONFIRMED && g_moved_calls == 0 &&
              nvs_sim_commit_count() == commits && stored_is(&EP_NEW_PORT) &&
              sent_to(&EP_MOVED, PB_MGMT_TYPE_LOCATE) == 0,
          "that answer alone moves nothing: the endpoint it names was not asked, nothing is stored");
    size_t mark = g_sent_count;
    int took = polls_until_moved(40);
    const sent_t *first = locate_after(mark, 1);
    CHECK(took == 6 && sent_went_to(first, EP_MOVED),
          "the next round asks the endpoint the hub named first of all");
    bool fresh = first != NULL && !pb_mgmt_is_zero(first->nonce, PB_MGMT_NONCE_LEN);
    for (size_t i = 0; first != NULL && i < mark; i++) {
        fresh = fresh && !(g_sent[i].type == PB_MGMT_TYPE_LOCATE &&
                           memcmp(g_sent[i].nonce, first->nonce, PB_MGMT_NONCE_LEN) == 0);
    }
    CHECK(fresh, "with a nonce no request before it carried");
    CHECK(locates_after(mark) == 1, "its answer -- from that endpoint, naming that endpoint -- is the confirmation: "
                                    "one request");
    expect_move("the endpoint carried over", &EP_MOVED, commits);

    printf("-- ... and the endpoint carried over is only a place to ask --\n");
    static const struct {
        listener_kind_t kind;
        const char *what;
    } there[] = {
        {L_SILENT, "nobody answers there"},
        {L_REPLAYER, "the hub's answer of the round before is sent again from there"},
        {L_WRONG_KEY, "a node with another key answers there"},
        {L_OTHER_HUB, "another hub answers there"},
        {L_OTHER_GENERATION, "an answer for another enrollment generation comes from there"},
        {L_TAMPERER, "the hub's answer comes from there with its address changed"},
    };
    for (size_t i = 0; i < sizeof(there) / sizeof(there[0]); i++) {
        hub_answers_at_the_last_request();
        (void)poll_times(PB_MGMT_LOCATE_AFTER_SILENT_POLLS);
        bool carried = g_client.last_locate == PB_MGMT_LOCATE_UNCONFIRMED && g_client.locate_pending_valid;
        /* The hub itself is gone again; at the endpoint it named sits ... */
        g_listener_count = 0;
        if (there[i].kind != L_SILENT) {
            listen_at(EP_MOVED, there[i].kind);
            listen_at(EP_NEW2, there[i].kind);
        }
        commits = nvs_sim_commit_count();
        mark = g_sent_count;
        (void)poll_times(6);
        snprintf(msg, sizeof(msg), "%s: asked once, first; nothing is stored, the platform is not told", there[i].what);
        CHECK(carried && sent_went_to(locate_after(mark, 1), EP_MOVED) && g_moved_calls == 0 &&
                  nvs_sim_commit_count() == commits && stored_is(&EP_NEW_PORT) && !g_client.locate_pending_valid,
              msg);
        mark = g_sent_count;
        size_t asked_there = sent_to(&EP_MOVED, PB_MGMT_TYPE_LOCATE);
        (void)poll_times(12);
        snprintf(msg, sizeof(msg), "%s: the round after that does not ask there again", there[i].what);
        CHECK(locates_after(mark) != 0 && sent_to(&EP_MOVED, PB_MGMT_TYPE_LOCATE) == asked_there &&
                  sent_went_to(locate_after(mark, 1), head) && g_moved_calls == 0,
              msg);
    }

    printf("-- the list changes between rounds --\n");
    net_reset();
    hub_at(EP_NEW_ADDR, FAR_HUB_RLOC16);
    board_enrolled_at(&EP_NEW_PORT, &k_ops);
    for (int i = 0; i < 19; i++) {
        hint_rloc(h(i));
    }
    hint_rloc(FAR_HUB_RLOC16);
    commits = nvs_sim_commit_count();
    (void)poll_times(PB_MGMT_LOCATE_AFTER_SILENT_POLLS); /* round 1: h(0)..h(4) on both ports, h(5) on 5700 */
    /* Three new routers in front, one of them twice, and an address of
     * another network. */
    hints_reset();
    hint_rloc(0x6000);
    hint_rloc(0x6400);
    hint_rloc(0x6000);
    memset(g_hints[g_hint_count], 0, 16);
    g_hints[g_hint_count][0] = 0xfd;
    g_hints[g_hint_count++][1] = 0x99;
    hint_rloc(0x6800);
    for (int i = 0; i < 19; i++) {
        hint_rloc(h(i));
    }
    hint_rloc(FAR_HUB_RLOC16);
    mark = g_sent_count;
    (void)poll_times(6); /* round 2 */
    CHECK(sent_went_to(locate_after(mark, 1), head) &&
              sent_went_to(locate_after(mark, 2), locator(h(5), PB_MGMT_LOCATE_PORT)),
          "the list grew in front of where the walk stood: it goes on behind the candidate it asked last, not from "
          "the top");
    /* Round 2 ended on h(10). That router leaves the network. */
    hints_reset();
    hint_rloc(0x6000);
    hint_rloc(0x6400);
    hint_rloc(0x6800);
    for (int i = 0; i < 19; i++) {
        if (i != 10) {
            hint_rloc(h(i));
        }
    }
    hint_rloc(FAR_HUB_RLOC16);
    mark = g_sent_count;
    (void)poll_times(12); /* round 3 */
    CHECK(sent_went_to(locate_after(mark, 2), locator(h(11), 5700)),
          "the candidate the walk stood on is gone: it goes on with what stood behind it, leaving nothing out");
    /* The order changes: the hub's router is now the first hint. */
    hints_reset();
    hint_rloc(FAR_HUB_RLOC16);
    hint_rloc(0x6000);
    hint_rloc(0x6400);
    hint_rloc(0x6800);
    for (int i = 0; i < 19; i++) {
        if (i != 10) {
            hint_rloc(h(i));
        }
    }
    pb_enroll_bridge_endpoint_t new_router = locator(0x6000, 5700);
    size_t asked_new = sent_to(&new_router, PB_MGMT_TYPE_LOCATE);
    found = run_rounds(4);
    CHECK(found.moved && found.rounds == 1 && found.most_in_a_round <= PB_MGMT_LOCATE_MAX_PROBES,
          "a hint that moved to the front is reached when the walk comes around the end -- here in the next round");
    CHECK(sent_to(&new_router, PB_MGMT_TYPE_LOCATE) == asked_new,
          "(the walk did not start over: the routers that joined in front were not asked on the way)");
    CHECK(stored_is(&EP_NEW_ADDR) && nvs_sim_commit_count() == commits + 1 && g_moved_calls == 1,
          "one flash write, the new endpoint");

    printf("-- answers that are not the hub's, one of them at a round's last request --\n");
    net_reset();
    hub_at(EP_NEW_ADDR, FAR_HUB_RLOC16);
    board_enrolled_at(&EP_NEW_PORT, &k_ops);
    for (int i = 0; i < 11; i++) {
        hint_rloc(h(i));
    }
    hint_rloc(FAR_HUB_RLOC16);
    listen_at(locator(h(0), 5700), L_OTHER_HUB);
    listen_at(locator(h(1), PB_MGMT_LOCATE_PORT), L_WRONG_KEY);
    listen_at(locator(h(2), 5700), L_OTHER_GENERATION);
    listen_at(locator(h(3), PB_MGMT_LOCATE_PORT), L_OTHER_BOARD);
    listen_at(locator(h(4), 5700), L_TAMPERER);
    listen_at(locator(h(5), 5700), L_WRONG_KEY); /* the 12th request of round 1 */
    listen_at(locator(h(7), 5700), L_GARBAGE);
    listen_at(locator(h(8), PB_MGMT_LOCATE_PORT), L_REFUSING);
    listen_at(EP_NEW2, L_OTHER_HUB); /* where those answers point */
    commits = nvs_sim_commit_count();
    (void)poll_times(PB_MGMT_LOCATE_AFTER_SILENT_POLLS);
    CHECK(sent_of_type(PB_MGMT_TYPE_LOCATE) == PB_MGMT_LOCATE_MAX_PROBES &&
              sent_went_to(locate_after(0, (int)PB_MGMT_LOCATE_MAX_PROBES), locator(h(5), 5700)),
          "round 1 ends on an answer made with another key");
    CHECK(g_client.last_locate == PB_MGMT_LOCATE_NOT_FOUND && !g_client.locate_pending_valid,
          "an answer that is not the hub's leaves nothing to ask next, at the last request as anywhere else");
    found = run_rounds(4);
    CHECK(found.moved && found.rounds == 2 && sent_to(&EP_NEW2, PB_MGMT_TYPE_LOCATE) == 0,
          "the hub is found two rounds later; the endpoint the other answers named was never asked");
    CHECK(stored_is(&EP_NEW_ADDR) && nvs_sim_commit_count() == commits + 1 && g_moved_calls == 1,
          "one flash write in all, the hub's endpoint");

    printf("-- the store fails in a later round --\n");
    net_reset();
    hub_at(EP_NEW_ADDR, FAR_HUB_RLOC16);
    board_enrolled_at(&EP_NEW_PORT, &k_ops);
    for (int i = 0; i < 6; i++) {
        hint_rloc(h(i));
    }
    hint_rloc(FAR_HUB_RLOC16); /* candidates 13 and 14: round 2 */
    (void)poll_times(PB_MGMT_LOCATE_AFTER_SILENT_POLLS);
    nvs_sim_inject(NVS_SIM_OP_COMMIT, NULL, 1, ESP_FAIL);
    (void)poll_times(6);
    CHECK(g_client.last_locate == PB_MGMT_LOCATE_STORE_FAILED && stored_is(&EP_NEW_PORT) && g_moved_calls == 0,
          "round 2 confirms the hub and cannot store: the record is the old one, nothing uses the new endpoint");
    mark = g_sent_count;
    took = polls_until_moved(40);
    CHECK(took == 12 && sent_went_to(locate_after(mark, 1), EP_NEW_ADDR) && locates_after(mark) == 1,
          "round 3 asks that endpoint first and stores it -- it does not wait until the walk comes around again");
    CHECK(stored_is(&EP_NEW_ADDR) && g_moved_calls == 1, "the record holds it");

    printf("-- what is forgotten, and when --\n");
    hub_answers_at_the_last_request();
    (void)poll_times(PB_MGMT_LOCATE_AFTER_SILENT_POLLS);
    bool carried = g_client.locate_pending_valid && g_client.locate_cursor_valid;
    /* A reboot: what the search kept was in RAM. */
    pb_mgmt_client_init(&g_client, &k_ops, EUI_A);
    (void)pb_mgmt_client_load(&g_client);
    g_listener_count = 0;
    mark = g_sent_count;
    (void)poll_times(PB_MGMT_LOCATE_AFTER_SILENT_POLLS);
    CHECK(carried && sent_went_to(locate_after(mark, 1), head) &&
              sent_went_to(locate_after(mark, 2), locator(h(0), 5700)) &&
              sent_to(&EP_MOVED, PB_MGMT_TYPE_LOCATE) == 0,
          "after a reboot nothing is carried over: the search starts at the top, and the endpoint named before the "
          "reboot is not asked");

    /* The record is read again by the same client (as after an enrollment
     * that ended and one that began): what the search kept belonged to the
     * record that was dropped. */
    hub_answers_at_the_last_request();
    (void)poll_times(PB_MGMT_LOCATE_AFTER_SILENT_POLLS);
    carried = g_client.locate_pending_valid && g_client.locate_cursor_valid;
    (void)pb_mgmt_client_load(&g_client);
    CHECK(carried && !g_client.locate_pending_valid && !g_client.locate_cursor_valid,
          "reading the record again forgets where the search stood and what it carried over");

    /* The guard behind that: a carried endpoint is asked only while it is an
     * endpoint of THIS record's network. (Nothing in the client can leave
     * one from another network behind; the state is set by hand here.) */
    hub_answers_at_the_last_request();
    (void)poll_times(PB_MGMT_LOCATE_AFTER_SILENT_POLLS);
    pb_enroll_bridge_endpoint_t elsewhere = {.addr = {0xfd, 0x99, 0, 0, 0, 0, 0, 0, 0x02, 0, 0, 0, 0, 0, 0, 0x01},
                                             .port = 5700};
    g_client.locate_pending = elsewhere;
    g_client.locate_pending_valid = true;
    g_listener_count = 0;                /* the hub itself is silent now, so that only this is looked at */
    listen_naming(elsewhere, elsewhere); /* the hub's key answers there: it would be confirmed if it were asked */
    commits = nvs_sim_commit_count();
    mark = g_sent_count;
    (void)poll_times(6);
    CHECK(sent_to(&elsewhere, PB_MGMT_TYPE_LOCATE) == 0 && sent_went_to(locate_after(mark, 1), head) &&
              nvs_sim_commit_count() == commits && g_moved_calls == 0,
          "an endpoint outside this enrollment's network is not asked, carried over or not");

    net_reset();
    board_enrolled_at(&EP_NEW_PORT, &k_ops);
    for (int i = 0; i < 20; i++) {
        hint_rloc(h(i));
    }
    (void)poll_times(PB_MGMT_LOCATE_AFTER_SILENT_POLLS); /* the walk stands at h(5) */
    hub_at(EP_NEW_PORT, FAR_HUB_RLOC16);               /* the hub is back where it was ... */
    bool answered = poll_times(1) == PB_MGMT_POLL_IDLE;
    g_listener_count = 0; /* ... and gone again */
    mark = g_sent_count;
    (void)poll_times(PB_MGMT_LOCATE_AFTER_SILENT_POLLS);
    CHECK(answered && sent_went_to(locate_after(mark, 2), locator(h(0), 5700)),
          "an answered poll ends the search: the next one starts at the top of the list");

    for (int variant = 0; variant < 2; variant++) {
        hub_answers_at_the_last_request();
        (void)poll_times(PB_MGMT_LOCATE_AFTER_SILENT_POLLS);
        carried = g_client.locate_pending_valid;
        const char *what;
        if (variant == 0) {
            what = "an unenroll command was journaled meanwhile";
            pb_enroll_record_t other = enrolled_record(&EP_NEW_PORT);
            other.mgmt_state = PB_ENROLL_MGMT_UNENROLLING;
            seq(other.command_id, 16, 0xe0);
            (void)pb_enroll_record_store(EUI_A, &other);
        } else {
            what = "the platform says a reset or unenroll is under way";
            g_locate_allowed = false;
        }
        uint8_t before[PB_ENROLL_RECORD_MAX_LEN], after[PB_ENROLL_RECORD_MAX_LEN];
        size_t before_len = raw_record(before);
        commits = nvs_sim_commit_count();
        mark = g_sent_count;
        (void)poll_times(6);
        size_t after_len = raw_record(after);
        snprintf(msg, sizeof(msg), "%s: the endpoint carried over is asked and confirmed, and still nothing is "
                                   "written and nobody is told",
                 what);
        CHECK(carried && sent_went_to(locate_after(mark, 1), EP_MOVED) &&
                  g_client.last_locate == PB_MGMT_LOCATE_RECORD_CHANGED && g_moved_calls == 0 &&
                  nvs_sim_commit_count() == commits && before_len == after_len &&
                  memcmp(before, after, before_len) == 0,
              msg);
    }
}

static void scenario_store_failures(void)
{
    printf("\n== the new endpoint cannot be stored ==\n");
    static const struct {
        nvs_sim_op_t op;
        esp_err_t err;
        const char *what;
    } cases[] = {
        {NVS_SIM_OP_COMMIT, ESP_FAIL, "the commit fails"},
        {NVS_SIM_OP_SET_BLOB, ESP_FAIL, "the write fails"},
        {NVS_SIM_OP_SET_BLOB, ESP_OK, "the write says OK and writes nothing"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        net_reset();
        hub_at(EP_NEW_ADDR, HUB_RLOC16_AFTER);
        board_enrolled_at(&EP_OLD, &k_ops);
        hint_rloc(HUB_RLOC16_AFTER);
        nvs_sim_inject(cases[i].op, cases[i].op == NVS_SIM_OP_COMMIT ? NULL : PB_ENROLL_NVS_KEY, 1, cases[i].err);
        pb_mgmt_poll_result_t r = poll_times(PB_MGMT_LOCATE_AFTER_SILENT_POLLS);
        char msg[200];
        snprintf(msg, sizeof(msg), "%s: the round ends as 'store failed', not as a move", cases[i].what);
        CHECK(r == PB_MGMT_POLL_NO_ANSWER && g_client.last_locate == PB_MGMT_LOCATE_STORE_FAILED, msg);
        snprintf(msg, sizeof(msg), "%s: the record is the old one, complete; the platform was not told; the client "
                                   "keeps polling the old endpoint",
                 cases[i].what);
        size_t polls_old = sent_to(&EP_OLD, PB_MGMT_TYPE_POLL);
        (void)poll_times(1);
        CHECK(stored_is(&EP_OLD) && g_moved_calls == 0 && same_ep(&g_client.rec.bridge_endpoint, &EP_OLD) &&
                  sent_to(&EP_OLD, PB_MGMT_TYPE_POLL) == polls_old + 1 && sent_to(&EP_NEW_ADDR, PB_MGMT_TYPE_POLL) == 0,
              msg);
        snprintf(msg, sizeof(msg), "%s: the next round stores it", cases[i].what);
        CHECK(polls_until_moved(20) != 0 && stored_is(&EP_NEW_ADDR) && g_moved_calls == 1, msg);
    }

    printf("-- the record does not read back after the write --\n");
    net_reset();
    hub_at(EP_NEW_ADDR, HUB_RLOC16_AFTER);
    board_enrolled_at(&EP_OLD, &k_ops);
    hint_rloc(HUB_RLOC16_AFTER);
    /* A load of the record reads twice (its size, then its bytes). From now
     * on: reads 1-2 = "is flash still this enrollment?", 3 = the read-back
     * after the write. */
    nvs_sim_inject(NVS_SIM_OP_GET_BLOB, PB_ENROLL_NVS_KEY, 3, ESP_FAIL);
    (void)poll_times(PB_MGMT_LOCATE_AFTER_SILENT_POLLS);
    CHECK(g_client.last_locate == PB_MGMT_LOCATE_STORE_FAILED && g_moved_calls == 0 &&
              same_ep(&g_client.rec.bridge_endpoint, &EP_OLD),
          "a read-back that fails is not a success: nothing uses the new endpoint");
    CHECK(stored_is(&EP_OLD), "and the previous record was put back, complete");
}

/* ---- power loss, over a flash file ---- */

static char g_flash[160];
#define EXIT_AFTER_TOLD 78

static int in_child(int (*fn)(int), int arg)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        nvs_sim_reset();
        if (!nvs_sim_set_backing_file(g_flash)) {
            _exit(99);
        }
        _exit(fn(arg));
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 98;
}

static int boot_enroll(int unused)
{
    (void)unused;
    pb_enroll_record_t rec = enrolled_record(&EP_OLD);
    return pb_enroll_record_store(EUI_A, &rec) == ESP_OK ? 0 : 1;
}

/* A boot in which the hub is at the new address. cut: 1 = power is lost at
 * the commit of the new record, 2 = right after the platform was told. */
static int boot_and_move(int cut)
{
    net_reset();
    hub_at(EP_NEW_ADDR, HUB_RLOC16_AFTER);
    hints_reset();
    hint_rloc(HUB_RLOC16_AFTER);
    g_locate_allowed = true;
    g_moved_calls = 0;
    g_exit_in_endpoint_changed = cut == 2 ? EXIT_AFTER_TOLD : 0;
    pb_mgmt_client_init(&g_client, &k_ops, EUI_A);
    if (pb_mgmt_client_load(&g_client) != PB_MGMT_BOOT_ARMED) {
        return 1;
    }
    if (cut == 1) {
        nvs_sim_power_cut_at_commit(1);
    }
    return polls_until_moved(20) != 0 ? 0 : 2;
}

/* What a boot finds: 10 = an armed enrollment at the old endpoint, 11 = at
 * the new one, anything else is not an enrollment that reads back whole. */
static int boot_report(int unused)
{
    (void)unused;
    pb_mgmt_client_init(&g_client, &k_ops, EUI_A);
    if (pb_mgmt_client_load(&g_client) != PB_MGMT_BOOT_ARMED) {
        return 20;
    }
    if (stored_is(&EP_OLD) && same_ep(&g_client.rec.bridge_endpoint, &EP_OLD)) {
        return 10;
    }
    if (stored_is(&EP_NEW_ADDR) && same_ep(&g_client.rec.bridge_endpoint, &EP_NEW_ADDR)) {
        return 11;
    }
    return 21;
}

/* The boot after a move: the hub is at the new address only. */
static int boot_poll_new(int unused)
{
    (void)unused;
    net_reset();
    hub_at(EP_NEW_ADDR, HUB_RLOC16_AFTER);
    hints_reset();
    pb_mgmt_client_init(&g_client, &k_ops, EUI_A);
    if (pb_mgmt_client_load(&g_client) != PB_MGMT_BOOT_ARMED) {
        return 1;
    }
    pb_mgmt_poll_result_t r = pb_mgmt_client_poll(&g_client);
    return (r == PB_MGMT_POLL_IDLE && sent_to(&EP_NEW_ADDR, PB_MGMT_TYPE_POLL) == 1 &&
            sent_of_type(PB_MGMT_TYPE_LOCATE) == 0)
               ? 0
               : 2;
}

static void scenario_power_loss(void)
{
    printf("\n== power is lost around the write ==\n");
    const char *tmp = getenv("TMPDIR");
    snprintf(g_flash, sizeof(g_flash), "%s/pb_locate_flash_%d.bin", (tmp != NULL && strlen(tmp) < 100) ? tmp : "/tmp", (int)getpid());

    unlink(g_flash);
    CHECK(in_child(boot_enroll, 0) == 0, "a board is enrolled at the old endpoint");
    CHECK(in_child(boot_and_move, 1) == NVS_SIM_POWER_CUT_EXIT, "power is lost at the commit of the new record");
    CHECK(in_child(boot_report, 0) == 10, "the next boot finds the OLD record, complete, and is armed with it");
    CHECK(in_child(boot_and_move, 0) == 0, "that boot finds the hub again and moves");
    CHECK(in_child(boot_report, 0) == 11, "the boot after it finds the NEW record, complete");

    unlink(g_flash);
    CHECK(in_child(boot_enroll, 0) == 0, "a board is enrolled at the old endpoint");
    CHECK(in_child(boot_and_move, 2) == EXIT_AFTER_TOLD, "power is lost right after the platform was told to move");
    CHECK(in_child(boot_report, 0) == 11, "the next boot finds the NEW record, complete");
    CHECK(in_child(boot_poll_new, 0) == 0,
          "and its first poll goes to the new endpoint and is answered -- nothing is looked for again");
    unlink(g_flash);
}

static void scenario_record_changed_meanwhile(void)
{
    printf("\n== the enrollment in flash is no longer the one the answer belongs to ==\n");
    pb_enroll_mgmt_credential_t cred = golden_credential();
    for (int variant = 0; variant < 6; variant++) {
        net_reset();
        hub_at(EP_NEW_ADDR, HUB_RLOC16_AFTER);
        board_enrolled_at(&EP_OLD, &k_ops);
        hint_rloc(HUB_RLOC16_AFTER);
        pb_enroll_record_t other = enrolled_record(&EP_OLD);
        const char *what = "";
        switch (variant) {
        case 0:
            what = "an unenroll command was journaled (UNENROLLING)";
            other.mgmt_state = PB_ENROLL_MGMT_UNENROLLING;
            seq(other.command_id, 16, 0xe0);
            break;
        case 1:
            what = "the enrollment was released (erase pending)";
            other.mgmt_state = PB_ENROLL_MGMT_RELEASED;
            other.release_flags = PB_ENROLL_RELEASE_F_ERASE_PENDING;
            seq(other.command_id, 16, 0xe0);
            break;
        case 2:
            what = "the release is final (the record holds a receipt, no key)";
            memset(&other, 0, sizeof(other));
            other.has_mgmt = true;
            other.mgmt_state = PB_ENROLL_MGMT_RELEASED;
            other.release_flags = PB_ENROLL_RELEASE_F_ERASED;
            memcpy(other.mgmt.hub_id, cred.hub_id, 16);
            memcpy(other.mgmt.generation, cred.generation, 16);
            seq(other.command_id, 16, 0xe0);
            seq(other.done_mac, 32, 0x01);
            break;
        case 3:
            what = "the record was erased (a local reset)";
            break;
        case 4:
            what = "the board was enrolled again (another generation and key)";
            seq(other.mgmt.generation, 16, 0x81);
            seq(other.mgmt.key, 32, 0x91);
            break;
        case 5:
            what = "the platform says a reset or unenroll is under way";
            break;
        }
        esp_err_t prepared = ESP_OK;
        if (variant == 3) {
            prepared = pb_enroll_marker_clear();
        } else if (variant == 5) {
            g_locate_allowed = false;
        } else {
            prepared = pb_enroll_record_store(EUI_A, &other);
        }
        uint8_t before[PB_ENROLL_RECORD_MAX_LEN], after[PB_ENROLL_RECORD_MAX_LEN];
        size_t before_len = raw_record(before);
        int commits = nvs_sim_commit_count();
        pb_mgmt_poll_result_t r = poll_times(PB_MGMT_LOCATE_AFTER_SILENT_POLLS);
        size_t after_len = raw_record(after);
        char msg[240];
        snprintf(msg, sizeof(msg), "%s: the hub's confirmed answer changes nothing in flash (not a byte), and "
                                   "nothing is moved",
                 what);
        CHECK(prepared == ESP_OK && r == PB_MGMT_POLL_NO_ANSWER &&
                  g_client.last_locate == PB_MGMT_LOCATE_RECORD_CHANGED && nvs_sim_commit_count() == commits &&
                  before_len == after_len && memcmp(before, after, sizeof(before)) == 0 && g_moved_calls == 0,
              msg);
        if (variant <= 3) {
            pb_enroll_record_t now;
            pb_enroll_marker_status_t st = pb_enroll_record_load(EUI_A, &now);
            snprintf(msg, sizeof(msg), "%s: the record did not become ENROLLED again", what);
            CHECK(st == PB_ENROLL_MARKER_ABSENT ||
                      (st == PB_ENROLL_MARKER_VALID && now.mgmt_state != PB_ENROLL_MGMT_ENROLLED),
                  msg);
        }
    }
}

static void scenario_unenroll_after_move(void)
{
    printf("\n== a removal that waits at the hub's new endpoint ==\n");
    net_reset();
    hub_at(EP_NEW_ADDR, HUB_RLOC16_AFTER);
    g_hub_command_pending = true;
    board_enrolled_at(&EP_OLD, &k_ops);
    hint_rloc(HUB_RLOC16_AFTER);
    g_network_erased = false;
    CHECK(polls_until_moved(20) != 0 && stored_is(&EP_NEW_ADDR), "the hub is found and the endpoint moved");
    CHECK(pb_mgmt_client_poll(&g_client) == PB_MGMT_POLL_COMMAND_PENDING, "the first poll there: a command is waiting");
    size_t locates = sent_of_type(PB_MGMT_TYPE_LOCATE);
    CHECK(pb_mgmt_client_poll(&g_client) == PB_MGMT_POLL_UNENROLLED, "the next poll carries it and the release runs to its end");
    pb_enroll_record_t rec;
    CHECK(pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_VALID && pb_enroll_record_is_final_release(&rec) &&
              !rec.has_bridge_endpoint && pb_mgmt_is_zero(rec.mgmt.key, 32),
          "the record is a final release: no endpoint, no key");
    CHECK(g_hub_done_released == 1 && sent_to(&EP_NEW_ADDR, PB_MGMT_TYPE_UNENROLL_DONE) >= 1 &&
              sent_to(&EP_OLD, PB_MGMT_TYPE_UNENROLL_DONE) == 0,
          "the 'released' notice went to the new endpoint, none to the old");
    g_listener_count = 0; /* and now nobody answers anywhere */
    CHECK(poll_times(50) == PB_MGMT_POLL_NOT_ARMED && sent_of_type(PB_MGMT_TYPE_LOCATE) == locates &&
              pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_VALID && pb_enroll_record_is_final_release(&rec),
          "a released board does not poll, does not look for a hub, and its record stays a final release");
}

static void scenario_boards_that_never_locate(void)
{
    printf("\n== boards that never look for a hub ==\n");
    /* No credential: record versions 1 and 2. */
    for (int version = 1; version <= 2; version++) {
        net_reset();
        hub_at(EP_NEW_ADDR, HUB_RLOC16_AFTER);
        nvs_sim_reset();
        pb_enroll_record_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.id = NET;
        if (version == 2) {
            rec.has_bridge_endpoint = true;
            rec.bridge_endpoint = EP_OLD;
        }
        (void)pb_enroll_record_store(EUI_A, &rec);
        uint8_t before[PB_ENROLL_RECORD_MAX_LEN], after[PB_ENROLL_RECORD_MAX_LEN];
        size_t before_len = raw_record(before);
        hints_reset();
        hint_rloc(HUB_RLOC16_AFTER);
        pb_mgmt_client_init(&g_client, &k_ops, EUI_A);
        pb_mgmt_boot_t boot = pb_mgmt_client_load(&g_client);
        pb_mgmt_poll_result_t r = poll_times(100);
        size_t after_len = raw_record(after);
        char msg[200];
        snprintf(msg, sizeof(msg), "record version %d (no credential): not armed, nothing sent, the record's %zu "
                                   "bytes untouched",
                 version, before_len);
        CHECK(boot == PB_MGMT_BOOT_NOTHING && r == PB_MGMT_POLL_NOT_ARMED && g_sent_count == 0 && g_hint_calls == 0 &&
                  before_len == after_len && before_len == (version == 1 ? PB_ENROLL_MARKER_LEN : PB_ENROLL_MARKER_V2_LEN) &&
                  memcmp(before, after, sizeof(before)) == 0,
              msg);
    }

    /* A platform without the locate operations. */
    net_reset();
    hub_at(EP_NEW_ADDR, HUB_RLOC16_AFTER);
    board_enrolled_at(&EP_OLD, &k_ops_no_locate);
    hint_rloc(HUB_RLOC16_AFTER);
    int commits = nvs_sim_commit_count();
    (void)poll_times(300);
    CHECK(sent_of_type(PB_MGMT_TYPE_LOCATE) == 0 && g_hint_calls == 0 && nvs_sim_commit_count() == commits &&
              stored_is(&EP_OLD),
          "a platform without locate operations: 300 silent polls, no LOCATE, no write");

    /* What each platform announces. */
    net_reset();
    hub_at(EP_OLD, HUB_RLOC16);
    board_enrolled_at(&EP_OLD, &k_ops);
    (void)poll_times(1);
    CHECK(g_client.hello_done && (g_client.hub_capabilities & PB_MGMT_HUB_CAP_ENDPOINT_LOCATE) != 0,
          "a platform with them announces PB_MGMT_BOARD_CAP_ENDPOINT_LOCATE (HELLO is sent and answered)");
    net_reset();
    hub_at(EP_OLD, HUB_RLOC16);
    board_enrolled_at(&EP_OLD, &k_ops_no_locate);
    (void)poll_times(1);
    CHECK(sent_of_type(PB_MGMT_TYPE_HELLO) == 0,
          "a platform with neither a receipt session nor locate has nothing to announce: no HELLO");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    scenario_wire_format();
    scenario_healthy_hub_is_left_alone();
    scenario_short_outage();
    scenario_address_changed();
    scenario_port_changed();
    scenario_hub_at_stored_endpoint_only_locate();
    scenario_wrong_answers();
    scenario_claims_that_are_not_endpoints();
    scenario_many_candidates();
    scenario_progress_from_round_to_round();
    scenario_store_failures();
    scenario_power_loss();
    scenario_record_changed_meanwhile();
    scenario_unenroll_after_move();
    scenario_boards_that_never_locate();

    printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    printf(g_failures == 0 ? "PASS: 0 failure(s)\n" : "FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
