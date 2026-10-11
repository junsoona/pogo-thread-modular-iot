/*
 * The two persistent records of the local re-enrollment (docs/power-base.md
 * "BOOT 버튼 로컬 재등록"). Both live in the dynamic "nvs" partition,
 * namespace "pb_reenroll" -- next to, and never inside, the enrollment
 * record ("pb_enroll") and OpenThread's own store ("openthread"). Neither
 * holds a secret, and nothing here reads or writes the "cfg" partition.
 *
 * Window request (key "win", 24 bytes) -- "the BOOT button was held: open
 * the re-enrollment window at the next boot". Written by the button task
 * right before it restarts the chip, consumed (erased, and the erase read
 * back) by the next boot BEFORE the window is opened, so it opens at most
 * one window and a restart during that window is an ordinary boot again. It
 * authorises nothing but the window: no boot erases anything because of it.
 *   [0..3]   magic "PBW1"
 *   [4]      version 1
 *   [5..7]   zero
 *   [8..15]  EUI-64 of the board that wrote it
 *   [16..19] zero
 *   [20..23] CRC-32 (IEEE, little-endian) of bytes [0..19]
 *
 * Reset journal (key "op", 40 bytes) -- one authenticated, user-confirmed
 * local reset. It is committed and read back BEFORE the first destructive
 * step; a reset whose journal could not be stored is never started.
 *   [0..3]   magic "PBR1"
 *   [4]      format version 1
 *   [5]      stage: 1 APPROVED (the reset was started and is not known to be
 *            finished: every boot carries on with it and never resumes the
 *            old enrollment), 2 VERIFIED (the enrollment record and
 *            OpenThread's store were read back empty)
 *   [6..7]   zero
 *   [8..15]  EUI-64 of the board the reset is for
 *   [16..31] operation id, as the client chose it (not all zero)
 *   [32..35] zero
 *   [36..39] CRC-32 (IEEE, little-endian) of bytes [0..35]
 * A VERIFIED journal stays until the next reset replaces it: it is what
 * answers "what became of operation X" after the restart.
 *
 * Every store is set_blob + commit + read back and compared; every erase is
 * erase + commit + read back. A function that returns ESP_OK has seen the
 * result in the store.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "power_base_enrollment/power_base_enrollment.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PB_REENROLL_NVS_NAMESPACE "pb_reenroll"
#define PB_REENROLL_NVS_KEY_WINDOW "win"
#define PB_REENROLL_NVS_KEY_JOURNAL "op"
#define PB_REENROLL_WINDOW_LEN 24
#define PB_REENROLL_JOURNAL_LEN 40
#define PB_REENROLL_FORMAT_VERSION 1u
#define PB_REENROLL_OP_ID_LEN 16

typedef enum {
    PB_REENROLL_REC_ABSENT = 0,
    PB_REENROLL_REC_VALID,
    PB_REENROLL_REC_CORRUPT,    /* wrong size, magic, version, stage, reserved bytes or CRC */
    PB_REENROLL_REC_FOREIGN,    /* intact, but for another EUI-64 */
    PB_REENROLL_REC_READ_ERROR, /* NVS itself failed */
} pb_reenroll_rec_status_t;

const char *pb_reenroll_rec_status_name(pb_reenroll_rec_status_t status);

typedef enum {
    PB_REENROLL_STAGE_APPROVED = 1,
    PB_REENROLL_STAGE_VERIFIED = 2,
} pb_reenroll_stage_t;

typedef struct {
    pb_reenroll_stage_t stage;
    uint8_t op_id[PB_REENROLL_OP_ID_LEN];
} pb_reenroll_journal_t;

/* ---- window request ---- */

/* Never writes (it does not even create the namespace). */
pb_reenroll_rec_status_t pb_reenroll_window_load(const uint8_t eui64[PB_ENROLL_EUI64_LEN]);
esp_err_t pb_reenroll_window_store(const uint8_t eui64[PB_ENROLL_EUI64_LEN]);
/* Erases the request whatever it holds. ESP_OK only when a read afterwards
 * found nothing; also when there was nothing to erase. */
esp_err_t pb_reenroll_window_consume(void);

/* ---- reset journal ---- */

/* *out is zeroed unless the result is VALID. Never writes. */
pb_reenroll_rec_status_t pb_reenroll_journal_load(const uint8_t eui64[PB_ENROLL_EUI64_LEN], pb_reenroll_journal_t *out);
/* ESP_ERR_INVALID_ARG for an unknown stage or an all-zero operation id
 * (nothing is written). */
esp_err_t pb_reenroll_journal_store(const uint8_t eui64[PB_ENROLL_EUI64_LEN], const pb_reenroll_journal_t *journal);
/* Erases the journal whatever it holds. ESP_OK only when a read afterwards
 * found nothing. Only for taking back an APPROVED journal whose store did
 * not succeed, before any destructive step ran. */
esp_err_t pb_reenroll_journal_discard(void);

#ifdef __cplusplus
}
#endif
