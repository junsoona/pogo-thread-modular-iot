#include "pms5003/pms5003_parser.h"

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

void pms5003_parser_init(pms5003_parser_t *parser)
{
    parser->state = PMS5003_PARSE_WAIT_START1;
    parser->idx = 0;
    parser->frames_ok = 0;
    parser->frames_dropped = 0;
}

static void reset_to_start1(pms5003_parser_t *parser)
{
    parser->state = PMS5003_PARSE_WAIT_START1;
    parser->idx = 0;
}

bool pms5003_parser_feed(pms5003_parser_t *parser, uint8_t byte, pms5003_frame_t *out_frame)
{
    switch (parser->state) {
    case PMS5003_PARSE_WAIT_START1:
        if (byte == 0x42) {
            parser->buf[0] = byte;
            parser->idx = 1;
            parser->state = PMS5003_PARSE_WAIT_START2;
        }
        /* else: garbage prefix byte -- ignored, still waiting for start1. */
        return false;

    case PMS5003_PARSE_WAIT_START2:
        if (byte == 0x4D) {
            parser->buf[1] = byte;
            parser->idx = 2;
            parser->state = PMS5003_PARSE_LEN_HI;
        } else if (byte != 0x42) {
            /* Not a valid start1/start2 pair -- resync, but a fresh 0x42
             * here is itself a new candidate start1 (handled by staying in
             * WAIT_START2 without re-consuming a byte-count, since this
             * branch already excludes byte==0x42). */
            reset_to_start1(parser);
        }
        return false;

    case PMS5003_PARSE_LEN_HI:
        parser->buf[2] = byte;
        parser->idx = 3;
        parser->state = PMS5003_PARSE_LEN_LO;
        return false;

    case PMS5003_PARSE_LEN_LO: {
        parser->buf[3] = byte;
        parser->idx = 4;
        uint16_t len_field = rd_u16(&parser->buf[2]);
        if (len_field != PMS5003_EXPECTED_LEN_FIELD) {
            /* Range/structural check (section 11: "범위 검증"): the
             * datasheet fixes this field at 28 for the standard output
             * frame -- anything else means we locked onto a false start
             * (e.g. 0x42 0x4D occurring inside noise/garbage), not a real
             * frame. Drop and resync rather than trying to parse further. */
            parser->frames_dropped++;
            reset_to_start1(parser);
            return false;
        }
        parser->state = PMS5003_PARSE_DATA;
        return false;
    }

    case PMS5003_PARSE_DATA:
        parser->buf[parser->idx++] = byte;
        if (parser->idx < PMS5003_FRAME_LEN) {
            return false;
        }
        /* Full 32 bytes assembled -- verify checksum (sum of bytes 0..29)
         * before trusting any of it. */
        {
            uint32_t sum = 0;
            for (int i = 0; i < 30; i++) {
                sum += parser->buf[i];
            }
            uint16_t expected = rd_u16(&parser->buf[30]);
            /* Ready for the next frame regardless of outcome -- back-to-back
             * frames need no gap (section 11). */
            reset_to_start1(parser);
            if ((uint16_t)sum != expected) {
                parser->frames_dropped++;
                return false;
            }
        }
        out_frame->pm1_0_cf1 = rd_u16(&parser->buf[4]);
        out_frame->pm2_5_cf1 = rd_u16(&parser->buf[6]);
        out_frame->pm10_cf1 = rd_u16(&parser->buf[8]);
        out_frame->pm1_0_atm = rd_u16(&parser->buf[10]);
        out_frame->pm2_5_atm = rd_u16(&parser->buf[12]);
        out_frame->pm10_atm = rd_u16(&parser->buf[14]);
        out_frame->particles_0_3um = rd_u16(&parser->buf[16]);
        out_frame->particles_0_5um = rd_u16(&parser->buf[18]);
        out_frame->particles_1_0um = rd_u16(&parser->buf[20]);
        out_frame->particles_2_5um = rd_u16(&parser->buf[22]);
        out_frame->particles_5_0um = rd_u16(&parser->buf[24]);
        out_frame->particles_10um = rd_u16(&parser->buf[26]);
        parser->frames_ok++;
        return true;

    default:
        reset_to_start1(parser);
        return false;
    }
}
