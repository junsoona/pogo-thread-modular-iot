/*
 * POGO UART frame codec / streaming parser.
 *
 * Wire format (all multi-byte integers big-endian / network order):
 *
 *   SOF(1) | VERSION(1) | TYPE(1) | SEQ(1) | LEN(2) | PAYLOAD(LEN) | CRC16(2)
 *
 * CRC16 (see pogo_crc16.h) is computed over VERSION..PAYLOAD, i.e. everything
 * except the SOF byte and the CRC field itself.
 *
 * This layer only knows about frame *structure* -- it has no idea what TYPE
 * values mean. Structural problems (bad SOF sync, wrong VERSION, an
 * oversized LEN, or a CRC mismatch) make the parser drop the in-progress
 * frame and resynchronize on the next SOF byte. An unrecognized-but
 * well-formed TYPE is NOT a framing error: that is a protocol-layer concern
 * (see pogo_protocol.h) so a single unknown message never has to blow away
 * byte sync.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POGO_FRAME_SOF          0xA5
#define POGO_FRAME_VERSION      0x01

/* Large enough to carry a full OpenThread Active Operational Dataset TLV
 * blob (OT_OPERATIONAL_DATASET_MAX_LENGTH == 254) as a single frame
 * payload. This component does not depend on OpenThread headers, so the
 * bound is restated here as a plain constant. */
#define POGO_FRAME_MAX_PAYLOAD  254

/* SOF + VERSION + TYPE + SEQ + LEN(2) + PAYLOAD + CRC16(2) */
#define POGO_FRAME_MAX_SIZE     (1 + 1 + 1 + 1 + 2 + POGO_FRAME_MAX_PAYLOAD + 2)

typedef struct {
    uint8_t  version;
    uint8_t  type;
    uint8_t  seq;
    uint16_t len;
    uint8_t  payload[POGO_FRAME_MAX_PAYLOAD];
} pogo_frame_t;

/*
 * Encodes one frame into out_buf.
 * Returns the number of bytes written, or 0 if len > POGO_FRAME_MAX_PAYLOAD
 * or out_buf_size is too small.
 */
size_t pogo_frame_encode(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len,
                          uint8_t *out_buf, size_t out_buf_size);

typedef enum {
    POGO_PARSE_STATE_SOF = 0,
    POGO_PARSE_STATE_VERSION,
    POGO_PARSE_STATE_TYPE,
    POGO_PARSE_STATE_SEQ,
    POGO_PARSE_STATE_LEN_HI,
    POGO_PARSE_STATE_LEN_LO,
    POGO_PARSE_STATE_PAYLOAD,
    POGO_PARSE_STATE_CRC_HI,
    POGO_PARSE_STATE_CRC_LO,
} pogo_parse_state_t;

typedef struct {
    pogo_parse_state_t state;
    pogo_frame_t        frame;
    uint16_t             payload_idx;
    uint8_t               crc_hi;
    /* Running copy of VERSION..PAYLOAD used to recompute the CRC once the
     * trailing CRC16 field has been received. */
    uint8_t  crc_buf[5 + POGO_FRAME_MAX_PAYLOAD];
    uint16_t crc_buf_len;
    /* Diagnostics -- not required for correctness. */
    uint32_t frames_ok;
    uint32_t frames_dropped;
} pogo_frame_parser_t;

void pogo_frame_parser_init(pogo_frame_parser_t *parser);

/*
 * Feed one received byte into the parser.
 * Returns true and fills *out_frame when a complete, CRC-valid frame has
 * been assembled (the parser is reset and ready for the next frame).
 * Returns false otherwise, including when a malformed frame was just
 * dropped internally (frames_dropped is incremented; check it if you want
 * to log/count resync events).
 */
bool pogo_frame_parser_feed(pogo_frame_parser_t *parser, uint8_t byte, pogo_frame_t *out_frame);

#ifdef __cplusplus
}
#endif
