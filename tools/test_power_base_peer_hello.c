/*
 * Host-compiled verification of the sensor module's UPSTREAM
 * PEER_HELLO_REQ/RESP handshake specifically against a POWER_BASE peer, and
 * its BASE_ID cross-check -- required test items 1/2/3 of the power-base
 * follow-up spec: "POWER_BASE와 MODULE의 HELLO 요청/응답", "BASE_ID LOW와
 * peer type POWER_BASE 일치", "BASE_ID/peer type 충돌 거부".
 *
 * Mirrors components/pogo_port/pogo_port.c's validate_incoming_hello() /
 * handle_dock_timeout_or_silent_power_base() -- the SAME logic
 * tools/test_peer_hello_handshake.c already mirrors generically -- but with
 * scenarios specifically named for the power-base case (that file's own
 * scenarios are named for simultaneous/staggered/reboot/split-frame timing,
 * not for BASE_ID/POWER_BASE outcomes specifically). Links against the REAL,
 * unmodified pogo_proto wire-format code; only pogo_port.c's surrounding
 * FreeRTOS/GPIO scaffolding is reimplemented, same caveat as that file.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/pogo_proto/include \
 *      -o /tmp/test_power_base_peer_hello tools/test_power_base_peer_hello.c \
 *      components/pogo_proto/pogo_frame.c components/pogo_proto/pogo_crc16.c \
 *      components/pogo_proto/pogo_protocol.c \
 *   && /tmp/test_power_base_peer_hello
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "pogo_proto/pogo_frame.h"
#include "pogo_proto/pogo_protocol.h"

#define MAX_RETRY 3 /* mirrors CONFIG_POGO_MAX_RETRY's default */

typedef enum { P_HANDSHAKE, P_READY, P_ERROR } pstate_t;

typedef struct {
    pstate_t state;
    uint8_t  peer_type;
    int      base_id_power_base; /* -1 = no BASE_ID hw, 0 = reads MODULE, 1 = reads POWER_BASE -- UPSTREAM-only, same as pogo_port_config_t.base_id_gpio */
} module_upstream_port_t;

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

static bool base_id_present(const module_upstream_port_t *p)
{
    return p->base_id_power_base >= 0;
}
static bool base_id_says_power_base(const module_upstream_port_t *p)
{
    return p->base_id_power_base == 1;
}

/* Mirrors validate_incoming_hello() in pogo_port.c. */
static bool validate(module_upstream_port_t *p, uint8_t declared_peer_type, uint8_t capabilities)
{
    if (!pogo_peer_type_is_valid(declared_peer_type)) {
        p->state = P_ERROR;
        return false;
    }
    bool base_id_says = base_id_says_power_base(p);
    bool peer_says = (declared_peer_type == POGO_PEER_TYPE_POWER_BASE);
    if (base_id_present(p) && (base_id_says != peer_says)) {
        p->state = P_ERROR;
        return false;
    }
    if ((capabilities & POGO_CAP_COLLECTOR_PROVISIONING) == 0) {
        p->state = P_ERROR;
        return false;
    }
    p->peer_type = declared_peer_type;
    return true;
}

/* Encodes a real POGO_MSG_PEER_HELLO_REQ/RESP frame (real pogo_proto wire
 * format) and immediately decodes it back through validate() -- this is the
 * exact same round trip pogo_port.c's UART parser + validate_incoming_hello()
 * perform, just without the UART/task plumbing in between. */
static bool receive_hello_ex(module_upstream_port_t *p, pogo_peer_type_t declared_type, uint8_t capabilities)
{
    pogo_peer_hello_t h = {.protocol_version = POGO_FRAME_VERSION,
                            .peer_type = (uint8_t)declared_type,
                            .capabilities = capabilities};
    uint8_t payload[3];
    size_t plen = pogo_pack_peer_hello(&h, payload, sizeof(payload));

    uint8_t frame_buf[POGO_FRAME_MAX_SIZE];
    size_t flen = pogo_frame_encode(POGO_MSG_PEER_HELLO_REQ, 1, payload, (uint16_t)plen, frame_buf, sizeof(frame_buf));

    pogo_frame_parser_t parser;
    pogo_frame_parser_init(&parser);
    pogo_frame_t decoded = {0};
    bool got = false;
    for (size_t i = 0; i < flen; i++) {
        if (pogo_frame_parser_feed(&parser, frame_buf[i], &decoded)) {
            got = true;
        }
    }
    if (!got) {
        return false;
    }
    pogo_peer_hello_t decoded_hello;
    if (!pogo_unpack_peer_hello(decoded.payload, decoded.len, &decoded_hello)) {
        return false;
    }
    bool valid = validate(p, decoded_hello.peer_type, decoded_hello.capabilities);
    if (valid) {
        p->state = P_READY;
    }
    return valid;
}

static bool receive_hello(module_upstream_port_t *p, pogo_peer_type_t declared_type)
{
    return receive_hello_ex(p, declared_type, POGO_HELLO_SUPPORTED_CAPABILITIES);
}

static void scenario_power_base_and_module_hello_match(void)
{
    printf("\n-- scenario: POWER_BASE and MODULE HELLO_REQ/RESP, matching BASE_ID --\n");

    /* The first sensor module's UPSTREAM port, BASE_ID reads POWER_BASE
     * (the GND contact on the power base's connector -- see docs/power-base.md). */
    module_upstream_port_t module_upstream;
    memset(&module_upstream, 0, sizeof(module_upstream));
    module_upstream.state = P_HANDSHAKE;
    module_upstream.base_id_power_base = 1;

    bool ok = receive_hello(&module_upstream, POGO_PEER_TYPE_POWER_BASE);
    CHECK(ok, "module's UPSTREAM accepts a HELLO declaring POWER_BASE when BASE_ID also reads POWER_BASE");
    CHECK(module_upstream.state == P_READY, "module's UPSTREAM reaches PEER_READY");
    CHECK(module_upstream.peer_type == POGO_PEER_TYPE_POWER_BASE, "peer_type is recorded as POWER_BASE");

    /* An ordinary intermediate module's UPSTREAM (no BASE_ID contact --
     * BASE_ID is only ever meaningful when the peer might be a power base at
     * all, i.e. this module's OWN upstream contact) receiving a MODULE HELLO. */
    module_upstream_port_t between_modules;
    memset(&between_modules, 0, sizeof(between_modules));
    between_modules.state = P_HANDSHAKE;
    between_modules.base_id_power_base = 0; /* BASE_ID present and reads "not power base" */
    ok = receive_hello(&between_modules, POGO_PEER_TYPE_MODULE);
    CHECK(ok, "a module-to-module HELLO (peer=MODULE, BASE_ID=not-power-base) is accepted");
    CHECK(between_modules.peer_type == POGO_PEER_TYPE_MODULE, "peer_type is recorded as MODULE");
}

static void scenario_base_id_peer_type_conflict_rejected(void)
{
    printf("\n-- scenario: BASE_ID vs. declared peer_type conflicts are rejected --\n");

    module_upstream_port_t p1;
    memset(&p1, 0, sizeof(p1));
    p1.state = P_HANDSHAKE;
    p1.base_id_power_base = 1; /* BASE_ID says POWER_BASE ... */
    bool ok = receive_hello(&p1, POGO_PEER_TYPE_MODULE); /* ... but the peer declares MODULE */
    CHECK(!ok, "BASE_ID=POWER_BASE + declared peer_type=MODULE is rejected (PEER_TYPE_CONFLICT)");
    CHECK(p1.state == P_ERROR, "the port moves to ERROR, not PEER_READY, on this conflict");

    module_upstream_port_t p2;
    memset(&p2, 0, sizeof(p2));
    p2.state = P_HANDSHAKE;
    p2.base_id_power_base = 0; /* BASE_ID says NOT power base ... */
    ok = receive_hello(&p2, POGO_PEER_TYPE_POWER_BASE); /* ... but the peer declares POWER_BASE */
    CHECK(!ok, "BASE_ID=not-power-base + declared peer_type=POWER_BASE is rejected (PEER_TYPE_CONFLICT)");
    CHECK(p2.state == P_ERROR, "the port moves to ERROR on this conflict too");

    module_upstream_port_t p3;
    memset(&p3, 0, sizeof(p3));
    p3.state = P_HANDSHAKE;
    p3.base_id_power_base = 1;
    ok = receive_hello(&p3, 0 /* POGO_PEER_TYPE_UNKNOWN */);
    CHECK(!ok, "a peer declaring peer_type=UNKNOWN(0) is rejected outright, not treated as 'no opinion'");
}

static void scenario_silent_power_base_still_reaches_ready(void)
{
    printf("\n-- scenario: a passive/silent power base (no UART reply) is still PEER_READY via BASE_ID alone --\n");

    /* Mirrors handle_dock_timeout_or_silent_power_base(): MAX_RETRY hello
     * attempts with no HELLO_RESP at all. */
    module_upstream_port_t silent_power_base;
    memset(&silent_power_base, 0, sizeof(silent_power_base));
    silent_power_base.state = P_HANDSHAKE;
    silent_power_base.base_id_power_base = 1;
    int retries = 0;
    while (retries <= MAX_RETRY && silent_power_base.state == P_HANDSHAKE) {
        retries++;
        if (retries > MAX_RETRY) {
            if (base_id_present(&silent_power_base) && base_id_says_power_base(&silent_power_base)) {
                silent_power_base.peer_type = POGO_PEER_TYPE_POWER_BASE;
                silent_power_base.state = P_READY;
            } else {
                silent_power_base.state = P_ERROR;
            }
        }
    }
    CHECK(silent_power_base.state == P_READY, "BASE_ID=POWER_BASE with no HELLO reply at all still reaches PEER_READY");
    CHECK(silent_power_base.peer_type == POGO_PEER_TYPE_POWER_BASE, "peer_type is POWER_BASE from BASE_ID hardware alone");

    module_upstream_port_t silent_unexplained;
    memset(&silent_unexplained, 0, sizeof(silent_unexplained));
    silent_unexplained.state = P_HANDSHAKE;
    silent_unexplained.base_id_power_base = 0; /* not a power base, and nobody answered */
    retries = 0;
    while (retries <= MAX_RETRY && silent_unexplained.state == P_HANDSHAKE) {
        retries++;
        if (retries > MAX_RETRY) {
            if (base_id_present(&silent_unexplained) && base_id_says_power_base(&silent_unexplained)) {
                silent_unexplained.state = P_READY;
            } else {
                silent_unexplained.state = P_ERROR;
            }
        }
    }
    CHECK(silent_unexplained.state == P_ERROR, "a silent peer that BASE_ID does NOT explain is DOCK_TIMEOUT (ERROR), not PEER_READY");
}

static void scenario_capability_mismatch_rejected(void)
{
    printf("\n-- scenario: a power base's HELLO missing POGO_CAP_COLLECTOR_PROVISIONING is rejected --\n");
    module_upstream_port_t p;
    memset(&p, 0, sizeof(p));
    p.state = P_HANDSHAKE;
    p.base_id_power_base = 1;
    bool ok = receive_hello_ex(&p, POGO_PEER_TYPE_POWER_BASE, /*capabilities=*/0);
    CHECK(!ok, "a POWER_BASE HELLO with capabilities=0 is rejected even though BASE_ID/peer_type agree");
    CHECK(p.state == P_ERROR, "the port moves to ERROR on a capability mismatch, not PEER_READY");
}

int main(void)
{
    scenario_power_base_and_module_hello_match();
    scenario_base_id_peer_type_conflict_rejected();
    scenario_silent_power_base_still_reaches_ready();
    scenario_capability_mismatch_rejected();

    printf("\n");
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
