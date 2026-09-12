#include "pogo_proto/pogo_frame.h"
#include "pogo_proto/pogo_crc16.h"
#include <string.h>

size_t pogo_frame_encode(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len,
                          uint8_t *out_buf, size_t out_buf_size)
{
    if (len > POGO_FRAME_MAX_PAYLOAD) {
        return 0;
    }
    size_t total = 1 + 1 + 1 + 1 + 2 + (size_t)len + 2;
    if (out_buf == NULL || out_buf_size < total) {
        return 0;
    }

    size_t i = 0;
    out_buf[i++] = POGO_FRAME_SOF;
    out_buf[i++] = POGO_FRAME_VERSION;
    out_buf[i++] = type;
    out_buf[i++] = seq;
    out_buf[i++] = (uint8_t)(len >> 8);
    out_buf[i++] = (uint8_t)(len & 0xFF);
    if (len > 0) {
        memcpy(&out_buf[i], payload, len);
    }
    i += len;

    /* CRC covers VERSION..PAYLOAD, i.e. out_buf[1 .. i-1]. */
    uint16_t crc = pogo_crc16_ccitt(&out_buf[1], i - 1);
    out_buf[i++] = (uint8_t)(crc >> 8);
    out_buf[i++] = (uint8_t)(crc & 0xFF);

    return i;
}

static void parser_reset(pogo_frame_parser_t *p, bool as_drop)
{
    if (as_drop) {
        p->frames_dropped++;
    }
    p->state = POGO_PARSE_STATE_SOF;
    p->payload_idx = 0;
    p->crc_buf_len = 0;
}

void pogo_frame_parser_init(pogo_frame_parser_t *parser)
{
    memset(parser, 0, sizeof(*parser));
    parser->state = POGO_PARSE_STATE_SOF;
}

bool pogo_frame_parser_feed(pogo_frame_parser_t *p, uint8_t byte, pogo_frame_t *out_frame)
{
    switch (p->state) {
    case POGO_PARSE_STATE_SOF:
        if (byte == POGO_FRAME_SOF) {
            p->crc_buf_len = 0;
            p->state = POGO_PARSE_STATE_VERSION;
        }
        /* Otherwise: still hunting for sync; not a "dropped frame". */
        return false;

    case POGO_PARSE_STATE_VERSION:
        if (byte != POGO_FRAME_VERSION) {
            parser_reset(p, true);
            return false;
        }
        p->frame.version = byte;
        p->crc_buf[p->crc_buf_len++] = byte;
        p->state = POGO_PARSE_STATE_TYPE;
        return false;

    case POGO_PARSE_STATE_TYPE:
        p->frame.type = byte;
        p->crc_buf[p->crc_buf_len++] = byte;
        p->state = POGO_PARSE_STATE_SEQ;
        return false;

    case POGO_PARSE_STATE_SEQ:
        p->frame.seq = byte;
        p->crc_buf[p->crc_buf_len++] = byte;
        p->state = POGO_PARSE_STATE_LEN_HI;
        return false;

    case POGO_PARSE_STATE_LEN_HI:
        p->frame.len = (uint16_t)byte << 8;
        p->crc_buf[p->crc_buf_len++] = byte;
        p->state = POGO_PARSE_STATE_LEN_LO;
        return false;

    case POGO_PARSE_STATE_LEN_LO:
        p->frame.len = (uint16_t)(p->frame.len | byte);
        p->crc_buf[p->crc_buf_len++] = byte;
        if (p->frame.len > POGO_FRAME_MAX_PAYLOAD) {
            parser_reset(p, true);
            return false;
        }
        p->payload_idx = 0;
        p->state = (p->frame.len == 0) ? POGO_PARSE_STATE_CRC_HI : POGO_PARSE_STATE_PAYLOAD;
        return false;

    case POGO_PARSE_STATE_PAYLOAD:
        p->frame.payload[p->payload_idx++] = byte;
        p->crc_buf[p->crc_buf_len++] = byte;
        if (p->payload_idx >= p->frame.len) {
            p->state = POGO_PARSE_STATE_CRC_HI;
        }
        return false;

    case POGO_PARSE_STATE_CRC_HI:
        p->crc_hi = byte;
        p->state = POGO_PARSE_STATE_CRC_LO;
        return false;

    case POGO_PARSE_STATE_CRC_LO: {
        uint16_t received_crc = (uint16_t)(((uint16_t)p->crc_hi << 8) | byte);
        uint16_t computed_crc = pogo_crc16_ccitt(p->crc_buf, p->crc_buf_len);
        if (received_crc != computed_crc) {
            parser_reset(p, true);
            return false;
        }
        *out_frame = p->frame;
        p->frames_ok++;
        parser_reset(p, false);
        return true;
    }

    default:
        parser_reset(p, true);
        return false;
    }
}
