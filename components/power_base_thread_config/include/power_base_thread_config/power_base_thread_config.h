/*
 * Power-base Thread Dataset provenance config -- a SMALL, separate
 * component from components/power_base_config (which owns the power base's
 * fixed board/electrical config: POGO GPIOs, timeouts, ...). This one owns
 * exactly one question: does this power base join an EXISTING Thread
 * network formed by a Linux OTBR (+ a separate ESP32-H2 RCP), or does it
 * form/resume its own STANDALONE network as before -- see docs/power-base.md
 * and docs/otbr-rcp.md.
 *
 * Loaded from the SAME "cfg" NVS partition as power_base_config (see
 * power_base/partitions.csv), but its OWN namespace ("pb_thread") -- this
 * deliberately does NOT change power_base_config's own 44-byte blob format
 * or schema_version at all (see tools/pack_power_base_config.py).
 *
 *   namespace: pb_thread
 *     key "mode"          u8:  0 = STANDALONE, 1 = EXTERNAL_OTBR,
 *                             2 = AWAIT_COMMISSIONING
 *     key "active_tlvs"   blob: OTBR Active Dataset TLVs, ONLY present (and
 *                                only ever read) when mode=EXTERNAL_OTBR
 *
 * Compatibility: an older/regenerated image with no "pb_thread" namespace
 * at all, or a namespace present but no "mode" key, is treated as
 * STANDALONE -- NOT an error (see power_base_thread_config_load()'s own
 * comment). This is what lets every pre-existing power_base_config.bin
 * keep working unchanged.
 *
 * This component intentionally has ZERO dependency on OpenThread headers --
 * it only stores/loads bytes and a length, so it can be host-tested without
 * pulling in the OpenThread stack (see tools/test_power_base_thread_config.c).
 * ALL real OpenThread Dataset semantic validation (otDatasetIsValid()) and
 * actually applying the dataset happen one layer up, in
 * thread_manager_start_as_always_on_ftd_with_dataset()
 * (components/thread_manager) -- this component only enforces storage-level
 * policy: presence, non-zero length, and a hard maximum length.
 *
 * SECRECY: active_tlvs contains the real Thread Network Key. This component
 * (and everything that calls it) must NEVER log its bytes -- only
 * mode/length. See docs/power-base.md's Dataset secrecy requirements.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Must match OpenThread's OT_OPERATIONAL_DATASET_MAX_LENGTH
 * (openthread/dataset.h) and tools/pack_power_base_config.py's own copy of
 * this same constant. Kept as this component's own #define (rather than
 * including openthread/dataset.h) so it stays host-testable with zero
 * OpenThread dependency -- see this header's own top comment. */
#define POWER_BASE_THREAD_CONFIG_MAX_TLV_LEN 254

#define POWER_BASE_THREAD_CONFIG_NAMESPACE "pb_thread"

typedef enum {
    POWER_BASE_THREAD_MODE_STANDALONE = 0,
    POWER_BASE_THREAD_MODE_EXTERNAL_OTBR = 1,
    /* Explicitly prepared for app-based commissioning. Legacy images still
     * default to STANDALONE; only a new cfg image may enter this safe state. */
    POWER_BASE_THREAD_MODE_AWAIT_COMMISSIONING = 2,
} power_base_thread_mode_t;

typedef struct {
    power_base_thread_mode_t mode;
    /* Only meaningful (and only ever populated) when mode ==
     * POWER_BASE_THREAD_MODE_EXTERNAL_OTBR; zero/zero-length otherwise. */
    uint8_t  active_tlvs[POWER_BASE_THREAD_CONFIG_MAX_TLV_LEN];
    uint16_t active_tlvs_len;
} power_base_thread_config_t;

/*
 * Loads and validates STORAGE-LEVEL policy only (see this header's top
 * comment) from the "cfg" NVS partition, namespace
 * POWER_BASE_THREAD_CONFIG_NAMESPACE:
 *
 *   - no "pb_thread" namespace, or namespace present but no "mode" key:
 *     *out set to mode=STANDALONE, active_tlvs_len=0, returns ESP_OK
 *     (legacy-image compatibility -- NOT an error).
 *   - mode=0: *out set to STANDALONE, returns ESP_OK (active_tlvs, if any,
 *     is never read).
 *   - mode=1 (EXTERNAL_OTBR) with a present, non-empty, <=
 *     POWER_BASE_THREAD_CONFIG_MAX_TLV_LEN "active_tlvs" blob: *out set to
 *     EXTERNAL_OTBR with that blob's exact bytes/length, returns ESP_OK.
 *   - mode=2 (AWAIT_COMMISSIONING): returns ESP_OK without reading any
 *     active_tlvs. The caller must not start Thread, the exporter, or the
 *     downstream POGO port until commissioning completes.
 *   - mode=1 but active_tlvs is missing, 0 bytes, or larger than
 *     POWER_BASE_THREAD_CONFIG_MAX_TLV_LEN: FAILS CLOSED -- logs one clear
 *     "CONFIG_ERROR: ..." line (dataset bytes/length-in-error never
 *     logged beyond the byte count itself), zeroes *out, and returns a
 *     non-ESP_OK code. The caller MUST NOT fall back to forming/resuming a
 *     standalone network in this case (see docs/power-base.md) -- same
 *     "fail closed, no silent fallback" policy as power_base_config_load().
 *   - any other mode value: same fail-closed policy (CONFIG_ERROR, zeroed
 *     *out, non-ESP_OK).
 *   - a genuine NVS/partition read error (not "namespace/key absent"):
 *     same fail-closed policy.
 *
 * *out is memset to 0 on EVERY path before anything else, so a caller that
 * (incorrectly) ignores the return code still starts from an all-zero,
 * unambiguously-STANDALONE-shaped struct rather than stale/partial data.
 */
esp_err_t power_base_thread_config_load(power_base_thread_config_t *out);

#ifdef __cplusplus
}
#endif
