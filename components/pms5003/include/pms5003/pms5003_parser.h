/*
 * PMS5003 (Plantower) 32-byte output frame parser -- pure logic, zero
 * hardware/transport dependency, so it can be exercised and host-tested
 * completely independently of how (or whether) bytes ever actually arrive
 * from a real sensor (section 11 of the follow-up spec: the hardware
 * transport is a separate, currently-blocked concern -- see
 * components/pms5003/pms5003.c and docs/pogo-interface.md's PMS5003
 * section for why).
 *
 * Frame layout (all multi-byte fields big-endian, matching the datasheet):
 *   0   1  start1 (0x42)
 *   1   1  start2 (0x4D)
 *   2   2  frame_length (always 28 for the standard output frame: 13
 *          data fields x 2 bytes + 2-byte checksum)
 *   4   2  pm1_0_cf1   (PM1.0, "CF=1" / standard-particle calibration)
 *   6   2  pm2_5_cf1   (PM2.5, CF=1)
 *   8   2  pm10_cf1    (PM10,  CF=1)
 *   10  2  pm1_0_atm   (PM1.0, atmospheric-environment calibration -- what
 *                       this parser/sensor_channel reports, matching common
 *                       ambient air-quality usage)
 *   12  2  pm2_5_atm   (PM2.5, atmospheric)
 *   14  2  pm10_atm    (PM10,  atmospheric)
 *   16  2  particles_0_3um  (count per 0.1L air, >=0.3um)
 *   18  2  particles_0_5um
 *   20  2  particles_1_0um
 *   22  2  particles_2_5um
 *   24  2  particles_5_0um
 *   26  2  particles_10um
 *   28  2  reserved
 *   30  2  checksum (sum of bytes 0..29 inclusive)
 *   = 32 bytes total.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PMS5003_FRAME_LEN         32u
#define PMS5003_EXPECTED_LEN_FIELD 28u /* the "frame_length" field's fixed documented value */
#define PMS5003_COMMAND_LEN        7u

typedef enum {
    PMS5003_COMMAND_SET_MODE = 0xE1,
    PMS5003_COMMAND_READ = 0xE2,
    PMS5003_COMMAND_SLEEP = 0xE4,
} pms5003_command_t;

typedef struct {
    uint16_t pm1_0_cf1;
    uint16_t pm2_5_cf1;
    uint16_t pm10_cf1;
    uint16_t pm1_0_atm;
    uint16_t pm2_5_atm;
    uint16_t pm10_atm;
    uint16_t particles_0_3um;
    uint16_t particles_0_5um;
    uint16_t particles_1_0um;
    uint16_t particles_2_5um;
    uint16_t particles_5_0um;
    uint16_t particles_10um;
} pms5003_frame_t;

typedef enum {
    PMS5003_PARSE_WAIT_START1 = 0,
    PMS5003_PARSE_WAIT_START2,
    PMS5003_PARSE_LEN_HI,
    PMS5003_PARSE_LEN_LO,
    PMS5003_PARSE_DATA,
} pms5003_parse_state_t;

typedef struct {
    pms5003_parse_state_t state;
    uint8_t  buf[PMS5003_FRAME_LEN];
    uint16_t idx;
    /* Diagnostics -- not required for correctness, mirrors pogo_frame_parser_t's
     * frames_ok/frames_dropped fields. */
    uint32_t frames_ok;
    uint32_t frames_dropped;
} pms5003_parser_t;

void pms5003_parser_init(pms5003_parser_t *parser);

/*
 * Feed one received byte. Returns true and fills *out_frame once a
 * complete, checksum-valid 32-byte frame has been assembled (the parser is
 * reset and immediately ready for the next frame -- back-to-back frames
 * need no gap between them). Returns false otherwise: still mid-frame, or a
 * malformed frame (bad start bytes, a frame_length field other than 28, or
 * a checksum mismatch) was just dropped and the parser resynchronized on
 * the next 0x42 byte -- check frames_dropped if you want to log/count
 * resync events, same convention as pogo_frame_parser_t.
 */
bool pms5003_parser_feed(pms5003_parser_t *parser, uint8_t byte, pms5003_frame_t *out_frame);

/* Builds the datasheet's 7-byte host command and checksum. `data_low` is
 * 0/1 for SET_MODE (passive/active) and SLEEP (sleep/wake), and 0 for READ. */
void pms5003_build_command(pms5003_command_t command, uint8_t data_low,
                           uint8_t out[PMS5003_COMMAND_LEN]);

#ifdef __cplusplus
}
#endif
