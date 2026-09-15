/*
 * Host-compiled verification of the PEER_HELLO_REQ/PEER_HELLO_RESP handshake
 * sequencing in components/pogo_port/pogo_port.c, for the scenarios in the
 * follow-up spec's section 1: simultaneous boot, staggered boot (either
 * order), one side rebooting after both are PEER_READY, and a handshake
 * frame arriving split across multiple UART reads.
 *
 * This links against the REAL, unmodified wire-format code
 * (pogo_frame_encode/pogo_frame_parser_feed/pogo_pack_peer_hello/
 * pogo_unpack_peer_hello/pogo_peer_type_is_valid from components/pogo_proto,
 * which have no ESP-IDF/FreeRTOS dependency) -- only the surrounding
 * FreeRTOS-task/GPIO scaffolding of pogo_port.c is reimplemented here, as a
 * minimal model that mirrors process_handshake()/process_peer_ready()/
 * validate_incoming_hello()/handle_incoming_hello_req()/
 * handle_incoming_hello_resp() step for step. If those functions change in
 * pogo_port.c, update the model below to match -- same caveat as
 * tools/test_polling_calc.c.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/pogo_proto/include \
 *      -o /tmp/test_peer_hello_handshake tools/test_peer_hello_handshake.c \
 *      components/pogo_proto/pogo_frame.c components/pogo_proto/pogo_crc16.c \
 *      components/pogo_proto/pogo_protocol.c \
 *   && /tmp/test_peer_hello_handshake
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "pogo_proto/pogo_frame.h"
#include "pogo_proto/pogo_protocol.h"

#define MAX_RETRY 3 /* mirrors CONFIG_POGO_MAX_RETRY's default */

typedef enum { P_WAIT, P_HANDSHAKE, P_READY, P_ERROR } pstate_t;

typedef struct {
    const char *name;
    uint8_t own_type;
    pstate_t state;
    uint8_t hello_seq;
    int retries;
    uint8_t peer_type;
    pogo_frame_parser_t parser;
    int base_id_power_base; /* -1 = no BASE_ID hw, 0 = reads MODULE, 1 = reads POWER_BASE */
} port_t;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char *msg)
{
    g_checks++;
    if (!cond) {
        printf("FAIL: %s\n", msg);
        g_failures++;
    } else {
        printf("ok:   %s\n", msg);
    }
}

static void port_init(port_t *p, const char *name, uint8_t own_type, int base_id_power_base)
{
    memset(p, 0, sizeof(*p));
    p->name = name;
    p->own_type = own_type;
    p->state = P_WAIT;
    p->base_id_power_base = base_id_power_base;
}

static size_t make_frame(uint8_t type, uint8_t seq, uint8_t proto_version, uint8_t peer_type, uint8_t *buf,
                          size_t bufsize)
{
    pogo_peer_hello_t h = {.protocol_version = proto_version,
                            .peer_type = peer_type,
                            .capabilities = POGO_HELLO_SUPPORTED_CAPABILITIES};
    uint8_t payload[3];
    size_t len = pogo_pack_peer_hello(&h, payload, sizeof(payload));
    return pogo_frame_encode(type, seq, payload, len, buf, bufsize);
}

static size_t make_frame_with_capabilities(uint8_t type, uint8_t seq, uint8_t proto_version, uint8_t peer_type,
                                            uint8_t capabilities, uint8_t *buf, size_t bufsize)
{
    pogo_peer_hello_t h = {.protocol_version = proto_version, .peer_type = peer_type, .capabilities = capabilities};
    uint8_t payload[3];
    size_t len = pogo_pack_peer_hello(&h, payload, sizeof(payload));
    return pogo_frame_encode(type, seq, payload, len, buf, bufsize);
}

static bool base_id_present(const port_t *p)
{
    return p->base_id_power_base >= 0;
}
static bool base_id_says_power_base(const port_t *p)
{
    return p->base_id_power_base == 1;
}

/* Mirrors validate_incoming_hello() in pogo_port.c. */
static bool validate(port_t *p, const pogo_frame_t *frame, uint8_t *out_peer_type)
{
    pogo_peer_hello_t h;
    if (!pogo_unpack_peer_hello(frame->payload, frame->len, &h)) {
        return false;
    }
    if (h.protocol_version != POGO_FRAME_VERSION) {
        p->state = P_ERROR;
        return false;
    }
    if (!pogo_peer_type_is_valid(h.peer_type)) {
        p->state = P_ERROR;
        return false;
    }
    if (base_id_present(p) && (base_id_says_power_base(p) != (h.peer_type == POGO_PEER_TYPE_POWER_BASE))) {
        p->state = P_ERROR;
        return false;
    }
    if ((h.capabilities & POGO_CAP_COLLECTOR_PROVISIONING) == 0) {
        p->state = P_ERROR;
        return false;
    }
    *out_peer_type = h.peer_type;
    return true;
}

/* Mirrors begin_handshake() in pogo_port.c. */
static size_t begin_handshake(port_t *p, uint8_t *buf, size_t bufsize)
{
    p->state = P_HANDSHAKE;
    pogo_frame_parser_init(&p->parser);
    p->hello_seq = 1;
    p->retries = 0;
    return make_frame(POGO_MSG_PEER_HELLO_REQ, p->hello_seq, POGO_FRAME_VERSION, p->own_type, buf, bufsize);
}

/* Mirrors retry-on-timeout in process_handshake() +
 * handle_dock_timeout_or_silent_power_base(). Returns new REQ bytes, or 0
 * length if the cycle ended (READY via silent POWER_BASE, or ERROR). */
static size_t retry_handshake(port_t *p, uint8_t *buf, size_t bufsize)
{
    p->retries++;
    if (p->retries > MAX_RETRY) {
        if (base_id_present(p) && base_id_says_power_base(p)) {
            p->peer_type = POGO_PEER_TYPE_POWER_BASE;
            p->state = P_READY;
        } else {
            p->state = P_ERROR;
        }
        return 0;
    }
    p->hello_seq++;
    return make_frame(POGO_MSG_PEER_HELLO_REQ, p->hello_seq, POGO_FRAME_VERSION, p->own_type, buf, bufsize);
}

/*
 * Feeds raw wire bytes (may be a whole frame or an arbitrary fragment of
 * one) into p's persistent parser, dispatching exactly like
 * process_handshake()/process_peer_ready() do in pogo_port.c: a HELLO_REQ
 * is always answered with HELLO_RESP (same seq) regardless of p's own
 * state; a HELLO_RESP only completes p's own initiator role if it matches
 * the outstanding hello_seq while still in HANDSHAKE. Returns the number of
 * reply bytes written to reply_buf (0 if nothing to send).
 */
static size_t feed_bytes(port_t *p, const uint8_t *bytes, size_t n, uint8_t *reply_buf, size_t reply_bufsize)
{
    size_t reply_len = 0;
    for (size_t i = 0; i < n; i++) {
        pogo_frame_t frame;
        if (!pogo_frame_parser_feed(&p->parser, bytes[i], &frame)) {
            continue;
        }
        if (frame.type == POGO_MSG_PEER_HELLO_REQ) {
            uint8_t peer_type;
            if (validate(p, &frame, &peer_type)) {
                p->peer_type = peer_type;
                reply_len = make_frame(POGO_MSG_PEER_HELLO_RESP, frame.seq, POGO_FRAME_VERSION, p->own_type,
                                        reply_buf, reply_bufsize);
            }
        } else if (frame.type == POGO_MSG_PEER_HELLO_RESP) {
            if (p->state == P_HANDSHAKE && frame.seq == p->hello_seq) {
                uint8_t peer_type;
                if (validate(p, &frame, &peer_type)) {
                    p->peer_type = peer_type;
                    p->state = P_READY;
                }
            }
            /* else: late/stray RESP, ignored -- matches process_peer_ready() */
        }
    }
    return reply_len;
}

static void scenario_simultaneous(void)
{
    printf("\n-- scenario: simultaneous boot --\n");
    port_t a, b;
    port_init(&a, "A", POGO_PEER_TYPE_MODULE, -1);
    port_init(&b, "B", POGO_PEER_TYPE_MODULE, -1);

    uint8_t req_a[64], req_b[64], reply_a[64], reply_b[64];
    size_t len_a = begin_handshake(&a, req_a, sizeof(req_a));
    size_t len_b = begin_handshake(&b, req_b, sizeof(req_b));

    size_t rlen_a = feed_bytes(&a, req_b, len_b, reply_a, sizeof(reply_a)); /* A gets B's REQ -> A replies RESP */
    size_t rlen_b = feed_bytes(&b, req_a, len_a, reply_b, sizeof(reply_b)); /* B gets A's REQ -> B replies RESP */

    feed_bytes(&a, reply_b, rlen_b, reply_a, sizeof(reply_a)); /* A gets B's RESP -> A completes */
    feed_bytes(&b, reply_a, rlen_a, reply_b, sizeof(reply_b)); /* B gets A's RESP -> B completes */

    check(a.state == P_READY, "A reaches PEER_READY");
    check(b.state == P_READY, "B reaches PEER_READY");
}

static void scenario_staggered(const char *first_name, const char *second_name)
{
    printf("\n-- scenario: staggered boot (%s first, %s ~5s later) --\n", first_name, second_name);
    port_t early, late;
    port_init(&early, first_name, POGO_PEER_TYPE_MODULE, -1);
    port_init(&late, second_name, POGO_PEER_TYPE_MODULE, -1);

    uint8_t req_early[64], req_late[64], reply_early[64], reply_late[64];
    size_t len_early = begin_handshake(&early, req_early, sizeof(req_early));

    /* `early` retries a few times with nobody to answer -- models several
     * seconds passing before `late` boots. */
    for (int i = 0; i < 3; i++) {
        len_early = retry_handshake(&early, req_early, sizeof(req_early));
        check(early.state == P_HANDSHAKE, "early side stays in HANDSHAKE while retrying alone");
    }

    /* `late` finally boots and sends its own first REQ. */
    size_t len_late = begin_handshake(&late, req_late, sizeof(req_late));

    /* `late`'s REQ reaches `early` -> `early` answers regardless of its own
     * retry count. */
    size_t rlen_early = feed_bytes(&early, req_late, len_late, reply_early, sizeof(reply_early));
    check(rlen_early > 0, "early side answers late side's HELLO_REQ with a HELLO_RESP");
    feed_bytes(&late, reply_early, rlen_early, reply_late, sizeof(reply_late));
    check(late.state == P_READY, "late side reaches PEER_READY from early side's reply");

    /* `early`'s current outstanding REQ reaches `late` (now PEER_READY) --
     * still answered per section 1 item 4/5. */
    size_t rlen_late = feed_bytes(&late, req_early, len_early, reply_late, sizeof(reply_late));
    check(rlen_late > 0, "already-PEER_READY late side still answers early side's HELLO_REQ");
    feed_bytes(&early, reply_late, rlen_late, reply_early, sizeof(reply_early));
    check(early.state == P_READY, "early side reaches PEER_READY from late side's reply");
}

static void scenario_one_side_reboots(void)
{
    printf("\n-- scenario: PEER_READY established, then one side reboots --\n");
    port_t a, b;
    port_init(&a, "A", POGO_PEER_TYPE_MODULE, -1);
    port_init(&b, "B", POGO_PEER_TYPE_MODULE, -1);

    uint8_t buf1[64], buf2[64], buf3[64];
    size_t len_a = begin_handshake(&a, buf1, sizeof(buf1));
    size_t len_b = begin_handshake(&b, buf2, sizeof(buf2));
    size_t r1 = feed_bytes(&a, buf2, len_b, buf3, sizeof(buf3));
    uint8_t r1_copy[64];
    memcpy(r1_copy, buf3, r1);
    size_t r2 = feed_bytes(&b, buf1, len_a, buf3, sizeof(buf3));
    feed_bytes(&a, buf3, r2, buf1, sizeof(buf1));
    feed_bytes(&b, r1_copy, r1, buf2, sizeof(buf2));
    check(a.state == P_READY && b.state == P_READY, "both reach PEER_READY before the reboot");

    /* A "reboots": fresh handshake state, sends REQ again (its own seq
     * restarts at 1). B never detected any detach (no DETECT hardware in
     * dev compat mode, or A's reboot alone doesn't toggle DETECT_IN even in
     * AUTO mode if the physical dock never actually separated) and is still
     * sitting in PEER_READY throughout. */
    uint8_t req_a2[64], reply_b2[64], reply_a2[64];
    size_t len_a2 = begin_handshake(&a, req_a2, sizeof(req_a2));
    check(a.state == P_HANDSHAKE, "rebooted side re-enters HANDSHAKE");

    size_t rlen_b2 = feed_bytes(&b, req_a2, len_a2, reply_b2, sizeof(reply_b2));
    check(b.state == P_READY, "B answers without ever leaving PEER_READY");
    check(rlen_b2 > 0, "B sends a fresh HELLO_RESP to the rebooted side");

    feed_bytes(&a, reply_b2, rlen_b2, reply_a2, sizeof(reply_a2));
    check(a.state == P_READY, "rebooted side reaches PEER_READY again");
}

static void scenario_split_frame(void)
{
    printf("\n-- scenario: HELLO_REQ delivered as fragmented UART reads --\n");
    port_t a, b;
    port_init(&a, "A", POGO_PEER_TYPE_MODULE, -1);
    port_init(&b, "B", POGO_PEER_TYPE_MODULE, -1);
    pogo_frame_parser_init(&b.parser);
    b.state = P_HANDSHAKE; /* B is mid-handshake, listening */

    uint8_t req[64];
    size_t len = begin_handshake(&a, req, sizeof(req));

    /* Deliver the same frame's bytes to B split into three uneven chunks,
     * exactly as multiple short uart_read_bytes() calls would. */
    size_t c1 = len / 3, c2 = len / 3;
    uint8_t reply[64];
    size_t total_reply = 0;
    total_reply += feed_bytes(&b, req, c1, reply, sizeof(reply));
    check(total_reply == 0, "no reply yet from a partially-received frame");
    total_reply += feed_bytes(&b, req + c1, c2, reply, sizeof(reply));
    check(total_reply == 0, "still no reply after a second partial chunk");
    total_reply += feed_bytes(&b, req + c1 + c2, len - c1 - c2, reply, sizeof(reply));
    check(total_reply > 0, "reply produced once the final fragment completes the frame");

    /* And with a corrupted frame immediately preceding a valid one, the
     * persistent parser must resync on its own (this exercises the
     * unmodified, already-proven pogo_frame_parser_t -- included here for
     * end-to-end confidence in the same integration path the handshake
     * actually uses). */
    port_t c;
    port_init(&c, "C", POGO_PEER_TYPE_MODULE, -1);
    pogo_frame_parser_init(&c.parser);
    c.state = P_HANDSHAKE;

    uint8_t garbage[8] = {0xA5, 0x01, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00}; /* bad CRC */
    uint8_t reply2[64];
    feed_bytes(&c, garbage, sizeof(garbage), reply2, sizeof(reply2));
    size_t reply3_len = feed_bytes(&c, req, len, reply2, sizeof(reply2));
    check(reply3_len > 0, "parser resyncs on the next valid frame after a corrupted one");
}

/* Capability negotiation (this follow-up, section 4): a peer whose HELLO is
 * missing POGO_CAP_COLLECTOR_PROVISIONING -- either because it declares
 * capabilities=0 explicitly, or because it is a legacy 2-byte HELLO payload
 * that predates the capabilities field entirely -- is rejected cleanly at
 * the handshake, never reaching PEER_READY. */
static void scenario_capability_mismatch_rejected(void)
{
    printf("\n-- scenario: capability negotiation rejects an incompatible peer at the handshake --\n");
    port_t p;
    port_init(&p, "P", POGO_PEER_TYPE_MODULE, -1);
    p.state = P_HANDSHAKE;

    uint8_t frame_buf[64];
    size_t len = make_frame_with_capabilities(POGO_MSG_PEER_HELLO_REQ, 1, POGO_FRAME_VERSION, POGO_PEER_TYPE_MODULE,
                                               /*capabilities=*/0, frame_buf, sizeof(frame_buf));
    pogo_frame_parser_t parser;
    pogo_frame_parser_init(&parser);
    pogo_frame_t decoded = {0};
    bool got = false;
    for (size_t i = 0; i < len; i++) {
        if (pogo_frame_parser_feed(&parser, frame_buf[i], &decoded)) {
            got = true;
        }
    }
    check(got, "the malformed-capability HELLO frame itself still decodes structurally");
    uint8_t out_peer_type;
    bool valid = validate(&p, &decoded, &out_peer_type);
    check(!valid, "a HELLO declaring capabilities=0 (missing POGO_CAP_COLLECTOR_PROVISIONING) is rejected");
    check(p.state == P_ERROR, "the port moves to ERROR, not PEER_READY, on a capability mismatch");

    /* A legacy (pre-capability-negotiation) 2-byte HELLO payload: this
     * firmware's pogo_unpack_peer_hello() requires exactly 3 bytes now, so
     * this fails to even decode as a well-formed pogo_peer_hello_t --
     * mirrors pogo_port.c's validate_incoming_hello() treating a
     * frame->len==2 HELLO as an explicit POGO_PORT_ERR_CAPABILITY_MISMATCH
     * rather than silently ignoring it as generic malformed input. */
    uint8_t legacy_payload[2] = {POGO_FRAME_VERSION, POGO_PEER_TYPE_MODULE};
    uint8_t legacy_frame_buf[64];
    size_t legacy_len =
        pogo_frame_encode(POGO_MSG_PEER_HELLO_REQ, 1, legacy_payload, sizeof(legacy_payload), legacy_frame_buf,
                           sizeof(legacy_frame_buf));
    pogo_frame_parser_t legacy_parser;
    pogo_frame_parser_init(&legacy_parser);
    pogo_frame_t legacy_decoded = {0};
    bool legacy_got = false;
    for (size_t i = 0; i < legacy_len; i++) {
        if (pogo_frame_parser_feed(&legacy_parser, legacy_frame_buf[i], &legacy_decoded)) {
            legacy_got = true;
        }
    }
    check(legacy_got, "a legacy 2-byte HELLO frame still decodes at the FRAME level (POGO_FRAME_VERSION unchanged)");
    pogo_peer_hello_t legacy_hello;
    bool legacy_unpack_ok = pogo_unpack_peer_hello(legacy_decoded.payload, legacy_decoded.len, &legacy_hello);
    check(!legacy_unpack_ok, "pogo_unpack_peer_hello() rejects a legacy 2-byte payload -- it now requires 3 bytes");
    check(legacy_decoded.len == 2, "the legacy frame's payload length is exactly 2, the signal pogo_port.c uses to "
                                    "distinguish this case from other malformed input");
}

int main(void)
{
    scenario_simultaneous();
    scenario_staggered("ROOT", "LEAF");
    scenario_staggered("LEAF", "ROOT");
    scenario_one_side_reboots();
    scenario_split_frame();
    scenario_capability_mismatch_rejected();

    printf("\n%d/%d checks passed\n", g_checks - g_failures, g_checks);
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
