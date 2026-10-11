/*
 * Power Base management wire protocol, version 1 -- the authenticated
 * messages a registered Power Base and its hub exchange on the CoAP resource
 * "pogo/v1/mgmt" (docs/power-base-management.md, section 6; keep both in
 * step). Pure, dependency-free C: the same file is compiled into the board
 * firmware (components/power_base_mgmt), the Linux hub
 * (linux/matter_bridge/app/api) and the host tests.
 *
 * This is a separate format. It does not extend components/bridge_export_proto
 * (the unauthenticated data path) and nothing here is accepted there.
 *
 * Every message has a fixed size, every integer is big-endian:
 *
 *   [0]      version = PB_MGMT_PROTO_VERSION
 *   [1]      type (pb_mgmt_type_t)
 *   [2]      flags (per type; undefined bits must be zero)
 *   [3]      0
 *   [4..11]  the board's EUI-64
 *   [12..27] hub id
 *   [28..43] enrollment generation
 *   [44..]   body, see below
 *   last 32  HMAC-SHA-256(management key, PB_MGMT_MAC_DOMAIN | everything before the MAC)
 *
 *   POLL              board -> hub   board_nonce(16) | hub_nonce_echo(16) | state(1) | 0(3)   112 bytes
 *   IDLE              hub -> board   board_nonce(16) | hub_nonce(16)                           108 bytes
 *                                    flags PB_MGMT_IDLE_F_COMMAND_PENDING
 *   UNENROLL          hub -> board   board_nonce(16) | command_id(16)                          108 bytes
 *   UNENROLL_ACCEPTED board -> hub   command_id(16)                                             92 bytes
 *   UNENROLL_DONE     board -> hub   command_id(16) | result(1) | 0(3)                          96 bytes
 *   RESULT_ACK        hub -> board   command_id(16) | acked_type(1) | acked_result(1) | 0(2)    96 bytes
 *   HELLO             board -> hub   capabilities(4) | 0(4)                                     84 bytes
 *   HELLO_ACK         hub -> board   capabilities(4) | 0(4)                                     84 bytes
 *   LOCATE            board -> hub   board_nonce(16) | 0(4)                                     96 bytes
 *   LOCATE_ACK        hub -> board   board_nonce(16) | address(16) | port(2) | 0(2)            112 bytes
 *
 * LOCATE and LOCATE_ACK were added on 2026-10-09, again without changing the
 * version byte and for the same reason as HELLO: a hub that does not know
 * them answers 4.00, which a board takes as "this is not where my hub
 * answers". LOCATE asks "where does my hub take my messages now?" of
 * whatever answers at the address it is sent to; LOCATE_ACK is the hub's
 * answer: its ingest listener's Thread Mesh-Local EID and port, under the
 * key of this board's enrollment and with this request's nonce. The address
 * a LOCATE was sent to is only a hint from the Thread network (anyone on it
 * can be there); the endpoint in an authenticated LOCATE_ACK is the hub's
 * word. Neither message changes anything on the hub. What the board does
 * with the answer -- it uses an endpoint only after the hub confirmed it
 * from that very endpoint -- is in pb_mgmt_client.h.
 *
 * HELLO and HELLO_ACK were added later (2026-10-08) without changing the
 * version byte: nothing about the six messages above changed, and a peer that
 * does not know the two types answers them like any other unknown type (the
 * hub with 4.00), which the sender takes as "no capabilities". They say what
 * the sender can do beyond the six messages; a receiver ignores the bits it
 * does not know.
 *
 * UNENROLL_DONE says one of two different things, by its result byte, and
 * they must never be read as one:
 *   PB_MGMT_RESULT_RELEASED  the board stored that this enrollment is out of
 *                            use for good (it will never resume it). Its
 *                            stored join information (the Thread Dataset) is
 *                            NOT erased yet. Sent over Thread, which still
 *                            works then. NOT a completion.
 *   PB_MGMT_RESULT_ERASED    the stored join information is erased, that was
 *                            checked, and the final record is stored. The
 *                            board creates this message only after all of
 *                            that: it is its "release receipt". Nothing of
 *                            the network is left in its flash by then. A
 *                            board with PB_MGMT_BOARD_CAP_THREAD_RECEIPT
 *                            sends it to the hub itself, over one last Thread
 *                            session that exists in RAM only
 *                            (pb_mgmt_client.h); every board also hands it
 *                            out over BLE (pb_ble_proto.h) for an app to
 *                            relay. The same bytes either way. Only this one
 *                            completes a removal.
 * RESULT_ACK repeats the result it acknowledges, so an acknowledgement of
 * the first can never be taken for one of the second.
 *
 * The key is the per-enrollment management key the hub issued; it is never a
 * Thread PSKd, a Network Key or a setup code. Nothing here logs a key, a
 * nonce or a MAC.
 *
 * Freshness is the caller's business (pb_mgmt_client.h on the board, the hub's
 * PowerBaseManagement): POLL/IDLE/UNENROLL and LOCATE/LOCATE_ACK carry
 * nonces; the three result messages deliberately carry none, so they are the same bytes every time for
 * one (EUI-64, hub, generation, command id) and applying one twice changes
 * nothing.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PB_MGMT_PROTO_VERSION 1u
/* The CoAP Uri-Path of the resource, on the hub's ingest port. */
#define PB_MGMT_URI_PATH "pogo/v1/mgmt"

#define PB_MGMT_EUI64_LEN 8
#define PB_MGMT_HUB_ID_LEN 16
#define PB_MGMT_GENERATION_LEN 16
#define PB_MGMT_KEY_LEN 32
#define PB_MGMT_NONCE_LEN 16
#define PB_MGMT_COMMAND_ID_LEN 16
#define PB_MGMT_MAC_LEN 32

#define PB_MGMT_HEADER_LEN 44

typedef enum {
    PB_MGMT_TYPE_POLL = 0x01,
    PB_MGMT_TYPE_UNENROLL_ACCEPTED = 0x02,
    PB_MGMT_TYPE_UNENROLL_DONE = 0x03,
    PB_MGMT_TYPE_HELLO = 0x05,
    PB_MGMT_TYPE_LOCATE = 0x06,
    PB_MGMT_TYPE_IDLE = 0x81,
    PB_MGMT_TYPE_UNENROLL = 0x82,
    PB_MGMT_TYPE_RESULT_ACK = 0x83,
    PB_MGMT_TYPE_HELLO_ACK = 0x85,
    PB_MGMT_TYPE_LOCATE_ACK = 0x86,
} pb_mgmt_type_t;

#define PB_MGMT_POLL_LEN 112
#define PB_MGMT_IDLE_LEN 108
#define PB_MGMT_UNENROLL_LEN 108
#define PB_MGMT_UNENROLL_ACCEPTED_LEN 92
#define PB_MGMT_UNENROLL_DONE_LEN 96
#define PB_MGMT_RESULT_ACK_LEN 96
#define PB_MGMT_HELLO_LEN 84
#define PB_MGMT_HELLO_ACK_LEN 84
#define PB_MGMT_LOCATE_LEN 96
#define PB_MGMT_LOCATE_ACK_LEN 112
#define PB_MGMT_MAX_MESSAGE_LEN 112

#define PB_MGMT_ENDPOINT_ADDR_LEN 16
/* Where every hub answers LOCATE, whatever port its ingest listener was
 * given: the ingest listener's default port. A hub started on another port
 * keeps a second socket here that answers LOCATE and nothing else, so a
 * board that only knows the port it was enrolled with can still ask. */
#define PB_MGMT_LOCATE_PORT 5684u

/* IDLE: a command is waiting for this board; poll again, echoing this
 * answer's hub nonce. */
#define PB_MGMT_IDLE_F_COMMAND_PENDING 0x01u

/* POLL state byte. */
#define PB_MGMT_STATE_ENROLLED 1u

/* UNENROLL_DONE result byte (see the top comment); anything else is
 * malformed. RELEASED keeps the value and the literal meaning it always had
 * ("the persistent release record is stored"); what changed is that a hub no
 * longer calls that complete. */
#define PB_MGMT_RESULT_RELEASED 1u
#define PB_MGMT_RESULT_ERASED 2u

/* HELLO capabilities (a board's). */
/* After its erase was verified and the final record stored, this board sends
 * its release receipt to the hub itself, over Thread. A board without it
 * hands the receipt out over BLE only. */
#define PB_MGMT_BOARD_CAP_THREAD_RECEIPT 0x00000001u
/* This board looks for its hub when the stored endpoint stops answering, and
 * moves to the endpoint the hub then confirms (LOCATE). A note for the hub's
 * log; nothing depends on it. */
#define PB_MGMT_BOARD_CAP_ENDPOINT_LOCATE 0x00000002u

/* HELLO_ACK capabilities (a hub's). */
/* This hub completes a removal on a receipt that arrives on the management
 * resource, and answers the same receipt again with the same
 * acknowledgement. (A hub that predates HELLO completes on such a receipt as
 * well; it refuses a repeated one.) */
#define PB_MGMT_HUB_CAP_THREAD_RECEIPT 0x00000001u
/* This hub answers LOCATE. A board never waits for this bit before it asks:
 * it has to ask exactly when it cannot reach its hub. */
#define PB_MGMT_HUB_CAP_ENDPOINT_LOCATE 0x00000002u

/* 16 bytes, hashed before the message. */
#define PB_MGMT_MAC_DOMAIN "pogo-pb-mgmt-v1"
#define PB_MGMT_MAC_DOMAIN_LEN 16

typedef struct {
    uint8_t eui64[PB_MGMT_EUI64_LEN];
    uint8_t hub_id[PB_MGMT_HUB_ID_LEN];
    uint8_t generation[PB_MGMT_GENERATION_LEN];
} pb_mgmt_identity_t;

/* One decoded message. Fields a type does not carry are zero. */
typedef struct {
    uint8_t type;
    uint8_t flags;
    pb_mgmt_identity_t id;
    uint8_t board_nonce[PB_MGMT_NONCE_LEN];   /* POLL, IDLE, UNENROLL, LOCATE, LOCATE_ACK */
    uint8_t hub_nonce[PB_MGMT_NONCE_LEN];     /* POLL (echo), IDLE */
    uint8_t state;                            /* POLL */
    uint8_t command_id[PB_MGMT_COMMAND_ID_LEN]; /* UNENROLL, UNENROLL_ACCEPTED, UNENROLL_DONE, RESULT_ACK */
    uint8_t result;                           /* UNENROLL_DONE */
    uint8_t acked_type;                       /* RESULT_ACK */
    uint8_t acked_result;                     /* RESULT_ACK: the result of the UNENROLL_DONE it answers, else 0 */
    uint32_t capabilities;                    /* HELLO, HELLO_ACK */
    /* LOCATE_ACK: the hub's ingest endpoint -- IPv6 address in network byte
     * order and a port that is not 0. */
    uint8_t endpoint_addr[PB_MGMT_ENDPOINT_ADDR_LEN];
    uint16_t endpoint_port;
} pb_mgmt_msg_t;

/* HMAC-SHA-256 from the platform's own library (PSA on the board, OpenSSL on
 * the hub). False on failure. */
typedef bool (*pb_mgmt_hmac_fn)(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len,
                                uint8_t out[PB_MGMT_MAC_LEN], void *ctx);

/* The fixed length of a message of `type`, 0 for an unknown type. */
size_t pb_mgmt_message_len(uint8_t type);

/* Comparison whose duration does not depend on where the bytes differ. */
bool pb_mgmt_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);
bool pb_mgmt_is_zero(const uint8_t *p, size_t len);
/* memset that the compiler may not remove. */
void pb_mgmt_wipe(void *buf, size_t len);

/* Writes the complete message with its MAC. Returns its length, or 0 when
 * the message is not encodable (unknown type, undefined flag bits, a result
 * or state outside the protocol), `cap` is too small or the HMAC failed.
 * Nothing is written on failure. */
size_t pb_mgmt_pack(const pb_mgmt_msg_t *msg, const uint8_t key[PB_MGMT_KEY_LEN], pb_mgmt_hmac_fn hmac, void *hmac_ctx,
                    uint8_t *out, size_t cap);

/* The same message with a MAC computed earlier (the board keeps the MAC of
 * its completion proof after it has destroyed the key). */
size_t pb_mgmt_pack_with_mac(const pb_mgmt_msg_t *msg, const uint8_t mac[PB_MGMT_MAC_LEN], uint8_t *out, size_t cap);

/* Structure only: version, type, exact length, reserved bytes, flags, value
 * ranges. Does NOT authenticate; call pb_mgmt_verify() before acting on
 * anything in `out`. */
bool pb_mgmt_parse(const uint8_t *data, size_t len, pb_mgmt_msg_t *out);

/* True when the trailing MAC of data[0..len) is the one `key` gives. */
bool pb_mgmt_verify(const uint8_t *data, size_t len, const uint8_t key[PB_MGMT_KEY_LEN], pb_mgmt_hmac_fn hmac,
                    void *hmac_ctx);

/* The trailing MAC of an encoded message (len must be its full length). */
const uint8_t *pb_mgmt_mac_of(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
