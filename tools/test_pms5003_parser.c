/*
 * Host-compiled test for components/pms5003/pms5003_parser.c -- section
 * 11/13 of the follow-up spec: "분할 수신, garbage prefix, checksum 오류,
 * back-to-back frame host test".
 *
 * Links against the REAL, unmodified pms5003_parser.c (no ESP-IDF/FreeRTOS
 * dependency, no hardware transport dependency either -- see that
 * component's pms5003.c for why the actual UART transport is a separate,
 * currently-blocked concern).
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/pms5003/include \
 *      -o /tmp/test_pms5003_parser tools/test_pms5003_parser.c \
 *      components/pms5003/pms5003_parser.c \
 *   && /tmp/test_pms5003_parser
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "pms5003/pms5003_parser.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s\n", msg);                                                                               \
            g_failures++;                                                                                            \
        } else {                                                                                                     \
            printf("ok:   %s\n", msg);                                                                               \
        }                                                                                                            \
    } while (0)

/* Builds one valid 32-byte frame with the given PM values (particle counts
 * and CF=1 fields left at 0 for simplicity -- irrelevant to framing/checksum
 * correctness) and a correct checksum. */
static void build_frame(uint8_t out[PMS5003_FRAME_LEN], uint16_t pm1_0, uint16_t pm2_5, uint16_t pm10)
{
    memset(out, 0, PMS5003_FRAME_LEN);
    out[0] = 0x42;
    out[1] = 0x4D;
    out[2] = 0x00;
    out[3] = 28;
    /* CF=1 fields (bytes 4..9) left 0 */
    out[10] = (uint8_t)(pm1_0 >> 8);
    out[11] = (uint8_t)(pm1_0 & 0xFF);
    out[12] = (uint8_t)(pm2_5 >> 8);
    out[13] = (uint8_t)(pm2_5 & 0xFF);
    out[14] = (uint8_t)(pm10 >> 8);
    out[15] = (uint8_t)(pm10 & 0xFF);
    /* bytes 16..29 (particle counts + reserved) left 0 */
    uint32_t sum = 0;
    for (int i = 0; i < 30; i++) {
        sum += out[i];
    }
    out[30] = (uint8_t)((sum >> 8) & 0xFF);
    out[31] = (uint8_t)(sum & 0xFF);
}

static bool feed_all(pms5003_parser_t *p, const uint8_t *bytes, size_t len, pms5003_frame_t *out)
{
    bool got = false;
    for (size_t i = 0; i < len; i++) {
        if (pms5003_parser_feed(p, bytes[i], out)) {
            got = true;
        }
    }
    return got;
}

static void test_well_formed_frame(void)
{
    pms5003_parser_t p;
    pms5003_parser_init(&p);
    uint8_t frame[PMS5003_FRAME_LEN];
    build_frame(frame, 12, 34, 56);

    pms5003_frame_t out;
    bool ok = feed_all(&p, frame, sizeof(frame), &out);
    CHECK(ok, "a well-formed frame is parsed successfully");
    CHECK(out.pm1_0_atm == 12 && out.pm2_5_atm == 34 && out.pm10_atm == 56, "PM1.0/2.5/10 (atmospheric) decode correctly, big-endian");
    CHECK(p.frames_ok == 1 && p.frames_dropped == 0, "frames_ok/frames_dropped counters are accurate");
}

static void test_split_reception(void)
{
    /* Same frame, fed one byte at a time with the state persisting between
     * calls (already true of feed_all's loop) -- but explicitly split into
     * multiple "chunks" to model a real UART driver handing over partial
     * reads. */
    pms5003_parser_t p;
    pms5003_parser_init(&p);
    uint8_t frame[PMS5003_FRAME_LEN];
    build_frame(frame, 1, 2, 3);

    pms5003_frame_t out;
    bool got = false;
    /* Chunk sizes: 1, 5, 10, 16 (sums to 32). */
    size_t chunks[] = {1, 5, 10, 16};
    size_t offset = 0;
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        for (size_t i = 0; i < chunks[c]; i++) {
            if (pms5003_parser_feed(&p, frame[offset + i], &out)) {
                got = true;
            }
        }
        offset += chunks[c];
    }
    CHECK(got, "a frame split across multiple receive chunks is still parsed correctly");
    CHECK(out.pm1_0_atm == 1 && out.pm2_5_atm == 2 && out.pm10_atm == 3, "values decode correctly after split reception");
}

static void test_garbage_prefix(void)
{
    pms5003_parser_t p;
    pms5003_parser_init(&p);
    uint8_t garbage[] = {0x00, 0xFF, 0x11, 0x4D, 0x42, 0x99}; /* includes a stray 0x4D and 0x42 out of position */
    uint8_t frame[PMS5003_FRAME_LEN];
    build_frame(frame, 7, 8, 9);

    pms5003_frame_t out;
    bool got = false;
    for (size_t i = 0; i < sizeof(garbage); i++) {
        if (pms5003_parser_feed(&p, garbage[i], &out)) got = true;
    }
    for (size_t i = 0; i < sizeof(frame); i++) {
        if (pms5003_parser_feed(&p, frame[i], &out)) got = true;
    }
    CHECK(got, "a valid frame following an arbitrary garbage prefix is still found and parsed");
    CHECK(out.pm1_0_atm == 7, "decoded values are correct despite the preceding garbage");
}

static void test_checksum_error_dropped(void)
{
    pms5003_parser_t p;
    pms5003_parser_init(&p);
    uint8_t frame[PMS5003_FRAME_LEN];
    build_frame(frame, 10, 20, 30);
    frame[31] ^= 0xFF; /* corrupt the checksum's low byte */

    pms5003_frame_t out;
    bool got = feed_all(&p, frame, sizeof(frame), &out);
    CHECK(!got, "a frame with a bad checksum is NOT delivered");
    CHECK(p.frames_dropped == 1 && p.frames_ok == 0, "checksum failure is counted as a dropped frame, not a success");
}

static void test_bad_length_field_dropped(void)
{
    pms5003_parser_t p;
    pms5003_parser_init(&p);
    uint8_t frame[PMS5003_FRAME_LEN];
    build_frame(frame, 1, 1, 1);
    frame[3] = 27; /* frame_length field says 27, not the documented fixed 28 */

    pms5003_frame_t out;
    /* Only the first 4 bytes are consumed before the parser notices the bad
     * length field and resyncs -- feed the rest too; none of it should ever
     * complete a frame since resync discards the stale partial state. */
    bool got = feed_all(&p, frame, sizeof(frame), &out);
    CHECK(!got, "a frame_length field other than 28 is rejected (range/structural check)");
    CHECK(p.frames_dropped >= 1, "the bad-length-field case is counted as a dropped frame");
}

static void test_back_to_back_frames(void)
{
    pms5003_parser_t p;
    pms5003_parser_init(&p);
    uint8_t frame_a[PMS5003_FRAME_LEN], frame_b[PMS5003_FRAME_LEN];
    build_frame(frame_a, 100, 200, 300);
    build_frame(frame_b, 111, 222, 333);

    pms5003_frame_t out;
    int completed = 0;
    uint16_t last_pm1_0 = 0;
    for (size_t i = 0; i < sizeof(frame_a); i++) {
        if (pms5003_parser_feed(&p, frame_a[i], &out)) {
            completed++;
            last_pm1_0 = out.pm1_0_atm;
        }
    }
    CHECK(completed == 1 && last_pm1_0 == 100, "first of two back-to-back frames parses with no gap needed");
    for (size_t i = 0; i < sizeof(frame_b); i++) {
        if (pms5003_parser_feed(&p, frame_b[i], &out)) {
            completed++;
            last_pm1_0 = out.pm1_0_atm;
        }
    }
    CHECK(completed == 2 && last_pm1_0 == 111, "second back-to-back frame (fed immediately after the first, no gap) also parses");
    CHECK(p.frames_ok == 2, "both back-to-back frames are counted");
}

static void check_command(pms5003_command_t command, uint8_t data_low,
                          const uint8_t expected[PMS5003_COMMAND_LEN], const char *name)
{
    uint8_t actual[PMS5003_COMMAND_LEN];
    pms5003_build_command(command, data_low, actual);
    CHECK(memcmp(actual, expected, PMS5003_COMMAND_LEN) == 0, name);
}

static void test_host_commands(void)
{
    static const uint8_t passive[] = {0x42, 0x4D, 0xE1, 0x00, 0x00, 0x01, 0x70};
    static const uint8_t active[]  = {0x42, 0x4D, 0xE1, 0x00, 0x01, 0x01, 0x71};
    static const uint8_t read[]    = {0x42, 0x4D, 0xE2, 0x00, 0x00, 0x01, 0x71};
    static const uint8_t sleep[]   = {0x42, 0x4D, 0xE4, 0x00, 0x00, 0x01, 0x73};
    static const uint8_t wake[]    = {0x42, 0x4D, 0xE4, 0x00, 0x01, 0x01, 0x74};
    check_command(PMS5003_COMMAND_SET_MODE, 0, passive, "passive-mode command/checksum");
    check_command(PMS5003_COMMAND_SET_MODE, 1, active, "active-mode command/checksum");
    check_command(PMS5003_COMMAND_READ, 0, read, "passive-read command/checksum");
    check_command(PMS5003_COMMAND_SLEEP, 0, sleep, "sleep command/checksum");
    check_command(PMS5003_COMMAND_SLEEP, 1, wake, "wake command/checksum");
}

int main(void)
{
    test_host_commands();
    test_well_formed_frame();
    test_split_reception();
    test_garbage_prefix();
    test_checksum_error_dropped();
    test_bad_length_field_dropped();
    test_back_to_back_frames();

    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
