/*
 * Host-compiled verification of the REAL, unmodified
 * components/power_base_thread_config/power_base_thread_config.c, linked
 * against a functional in-memory NVS fake (tools/host_test_stubs/nvs_sim/)
 * instead of a re-implementation of its decision logic -- per this
 * follow-up's own test-plan requirement ("가능하면 실제 production loader를
 * NVS stub과 링크하라. 별도 mirror 구현만 테스트하지 마라").
 *
 * Covers this follow-up's minimum loader test list (section 8.4):
 *   - legacy namespace missing -> STANDALONE
 *   - namespace present but "mode" key missing -> STANDALONE
 *   - explicit STANDALONE (mode=0)
 *   - valid EXTERNAL_OTBR (mode=1, active_tlvs present) -- exact byte match
 *   - EXTERNAL_OTBR but active_tlvs missing -> fail closed
 *   - invalid mode value -> fail closed
 *   - EXTERNAL_OTBR but active_tlvs empty (0 bytes) -> fail closed
 *   - EXTERNAL_OTBR but active_tlvs oversized (> POWER_BASE_THREAD_CONFIG_MAX_TLV_LEN) -> fail closed
 *   - a genuine NVS/partition read error -> fail closed
 * Every "fail closed" case also asserts *out was zeroed and mode reads back
 * as the numeric STANDALONE value (0) -- the loader never leaves a caller
 * with ambiguous/partial state on a non-ESP_OK return.
 *
 * The TLV bytes used below are entirely made up (never a real/user Dataset)
 * -- see this project's Dataset secrecy policy (docs/power-base.md); this
 * file never logs them either (stderr from the real component's ESP_LOGx
 * calls only ever prints mode/length, by construction of
 * power_base_thread_config.c itself).
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra \
 *      -I tools/host_test_stubs/nvs_sim -I tools/host_test_stubs \
 *      -I components/power_base_thread_config/include \
 *      -o /tmp/test_power_base_thread_config tools/test_power_base_thread_config.c \
 *      components/power_base_thread_config/power_base_thread_config.c \
 *      tools/host_test_stubs/nvs_sim/nvs_sim.c \
 *   && /tmp/test_power_base_thread_config
 */
#include <stdio.h>
#include <string.h>

#include "power_base_thread_config/power_base_thread_config.h"
#include "nvs.h" /* ESP_ERR_NVS_NOT_INITIALIZED, for scenario_nvs_read_error() */
#include "nvs_sim.h"

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

#define PARTITION "cfg"
#define NS "pb_thread"

static void check_zeroed_standalone(const power_base_thread_config_t *out, const char *ctx)
{
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: mode reads back as STANDALONE(0) on failure", ctx);
    CHECK(out->mode == POWER_BASE_THREAD_MODE_STANDALONE, msg);
    snprintf(msg, sizeof(msg), "%s: active_tlvs_len is 0 on failure", ctx);
    CHECK(out->active_tlvs_len == 0, msg);
    static const uint8_t zero[POWER_BASE_THREAD_CONFIG_MAX_TLV_LEN] = {0};
    snprintf(msg, sizeof(msg), "%s: active_tlvs buffer is all-zero on failure", ctx);
    CHECK(memcmp(out->active_tlvs, zero, sizeof(zero)) == 0, msg);
}

static void scenario_legacy_namespace_missing(void)
{
    printf("\n-- scenario: no 'pb_thread' namespace at all (legacy image) -> STANDALONE --\n");
    nvs_sim_reset();
    /* Only the OTHER namespace ("power_base_cfg") exists in a real legacy
     * image -- simulate that here too, to confirm this loader looks
     * specifically for its own namespace and does not get confused. */
    nvs_sim_create_namespace(PARTITION, "power_base_cfg");

    power_base_thread_config_t out;
    esp_err_t err = power_base_thread_config_load(&out);
    CHECK(err == ESP_OK, "load succeeds when 'pb_thread' namespace is entirely absent");
    CHECK(out.mode == POWER_BASE_THREAD_MODE_STANDALONE, "mode is STANDALONE when the namespace is absent");
    CHECK(out.active_tlvs_len == 0, "active_tlvs_len is 0 when the namespace is absent");
}

static void scenario_namespace_present_mode_missing(void)
{
    printf("\n-- scenario: 'pb_thread' namespace exists but has no 'mode' key -> STANDALONE --\n");
    nvs_sim_reset();
    nvs_sim_create_namespace(PARTITION, NS);

    power_base_thread_config_t out;
    esp_err_t err = power_base_thread_config_load(&out);
    CHECK(err == ESP_OK, "load succeeds when the namespace exists but 'mode' is absent");
    CHECK(out.mode == POWER_BASE_THREAD_MODE_STANDALONE, "mode is STANDALONE when 'mode' key is absent");
}

static void scenario_explicit_standalone(void)
{
    printf("\n-- scenario: explicit mode=0 (STANDALONE) --\n");
    nvs_sim_reset();
    nvs_sim_put_u8(PARTITION, NS, "mode", (uint8_t)POWER_BASE_THREAD_MODE_STANDALONE);

    power_base_thread_config_t out;
    esp_err_t err = power_base_thread_config_load(&out);
    CHECK(err == ESP_OK, "load succeeds for explicit mode=0");
    CHECK(out.mode == POWER_BASE_THREAD_MODE_STANDALONE, "mode reads back as STANDALONE");
    CHECK(out.active_tlvs_len == 0, "active_tlvs_len is 0 for explicit STANDALONE (active_tlvs never read)");
}

static void scenario_explicit_standalone_ignores_stray_active_tlvs(void)
{
    printf("\n-- scenario: mode=0 but a stray 'active_tlvs' key also exists -- must still be STANDALONE, "
           "active_tlvs never read --\n");
    nvs_sim_reset();
    nvs_sim_put_u8(PARTITION, NS, "mode", (uint8_t)POWER_BASE_THREAD_MODE_STANDALONE);
    static const uint8_t stray[] = {0xAA, 0xBB, 0xCC};
    nvs_sim_put_blob(PARTITION, NS, "active_tlvs", stray, sizeof(stray));

    power_base_thread_config_t out;
    esp_err_t err = power_base_thread_config_load(&out);
    CHECK(err == ESP_OK, "load succeeds");
    CHECK(out.mode == POWER_BASE_THREAD_MODE_STANDALONE, "mode is still STANDALONE");
    CHECK(out.active_tlvs_len == 0, "a stray active_tlvs blob is never surfaced while mode=STANDALONE");
}

static void scenario_valid_external(void)
{
    printf("\n-- scenario: valid EXTERNAL_OTBR (mode=1, active_tlvs present) -- exact byte match --\n");
    nvs_sim_reset();
    nvs_sim_put_u8(PARTITION, NS, "mode", (uint8_t)POWER_BASE_THREAD_MODE_EXTERNAL_OTBR);
    /* Fake test-only TLV bytes -- never a real Dataset (see this file's top comment). */
    static const uint8_t fake_tlvs[] = {0x0E, 0x08, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x00, 0x03, 0x00, 0x00,
                                         0x01, 0x35, 0x06, 0x00, 0x04, 0x00, 0x1F, 0xFF, 0xE0};
    nvs_sim_put_blob(PARTITION, NS, "active_tlvs", fake_tlvs, sizeof(fake_tlvs));

    power_base_thread_config_t out;
    esp_err_t err = power_base_thread_config_load(&out);
    CHECK(err == ESP_OK, "load succeeds for a valid EXTERNAL_OTBR config");
    CHECK(out.mode == POWER_BASE_THREAD_MODE_EXTERNAL_OTBR, "mode reads back as EXTERNAL_OTBR");
    CHECK(out.active_tlvs_len == sizeof(fake_tlvs), "active_tlvs_len matches the stored blob's length exactly");
    CHECK(memcmp(out.active_tlvs, fake_tlvs, sizeof(fake_tlvs)) == 0,
          "active_tlvs bytes match the stored blob exactly, byte-for-byte");
}

static void scenario_external_missing_blob(void)
{
    printf("\n-- scenario: mode=1 but 'active_tlvs' key is entirely missing -> fail closed --\n");
    nvs_sim_reset();
    nvs_sim_put_u8(PARTITION, NS, "mode", (uint8_t)POWER_BASE_THREAD_MODE_EXTERNAL_OTBR);

    power_base_thread_config_t out;
    esp_err_t err = power_base_thread_config_load(&out);
    CHECK(err != ESP_OK, "load FAILS when EXTERNAL_OTBR mode has no active_tlvs blob at all");
    check_zeroed_standalone(&out, "missing active_tlvs");
}

static void scenario_external_empty_blob(void)
{
    printf("\n-- scenario: mode=1 but 'active_tlvs' is present with 0 bytes -> fail closed --\n");
    nvs_sim_reset();
    nvs_sim_put_u8(PARTITION, NS, "mode", (uint8_t)POWER_BASE_THREAD_MODE_EXTERNAL_OTBR);
    nvs_sim_put_blob(PARTITION, NS, "active_tlvs", "", 0);

    power_base_thread_config_t out;
    esp_err_t err = power_base_thread_config_load(&out);
    CHECK(err != ESP_OK, "load FAILS when active_tlvs is present but empty (0 bytes)");
    check_zeroed_standalone(&out, "empty active_tlvs");
}

static void scenario_external_oversized_blob(void)
{
    printf("\n-- scenario: mode=1 but 'active_tlvs' exceeds POWER_BASE_THREAD_CONFIG_MAX_TLV_LEN -> fail "
           "closed --\n");
    nvs_sim_reset();
    nvs_sim_put_u8(PARTITION, NS, "mode", (uint8_t)POWER_BASE_THREAD_MODE_EXTERNAL_OTBR);
    static uint8_t oversized[POWER_BASE_THREAD_CONFIG_MAX_TLV_LEN + 1];
    memset(oversized, 0x42, sizeof(oversized));
    nvs_sim_put_blob(PARTITION, NS, "active_tlvs", oversized, sizeof(oversized));

    power_base_thread_config_t out;
    esp_err_t err = power_base_thread_config_load(&out);
    CHECK(err != ESP_OK, "load FAILS when active_tlvs exceeds POWER_BASE_THREAD_CONFIG_MAX_TLV_LEN");
    check_zeroed_standalone(&out, "oversized active_tlvs");
}

static void scenario_external_max_len_exactly_ok(void)
{
    printf("\n-- scenario: active_tlvs at EXACTLY POWER_BASE_THREAD_CONFIG_MAX_TLV_LEN succeeds (boundary) --\n");
    nvs_sim_reset();
    nvs_sim_put_u8(PARTITION, NS, "mode", (uint8_t)POWER_BASE_THREAD_MODE_EXTERNAL_OTBR);
    static uint8_t max_len[POWER_BASE_THREAD_CONFIG_MAX_TLV_LEN];
    memset(max_len, 0x7A, sizeof(max_len));
    nvs_sim_put_blob(PARTITION, NS, "active_tlvs", max_len, sizeof(max_len));

    power_base_thread_config_t out;
    esp_err_t err = power_base_thread_config_load(&out);
    CHECK(err == ESP_OK, "load succeeds at exactly the maximum allowed length");
    CHECK(out.active_tlvs_len == POWER_BASE_THREAD_CONFIG_MAX_TLV_LEN, "active_tlvs_len is exactly the maximum");
    CHECK(memcmp(out.active_tlvs, max_len, sizeof(max_len)) == 0, "active_tlvs bytes match at the boundary length");
}

static void scenario_invalid_mode_value(void)
{
    printf("\n-- scenario: unsupported mode value (3) -> fail closed --\n");
    nvs_sim_reset();
    nvs_sim_put_u8(PARTITION, NS, "mode", 3);

    power_base_thread_config_t out;
    esp_err_t err = power_base_thread_config_load(&out);
    CHECK(err != ESP_OK, "load FAILS for an unsupported mode value");
    check_zeroed_standalone(&out, "invalid mode value");
}

static void scenario_await_commissioning(void)
{
    printf("\n-- scenario: explicit mode=2 waits without reading a stale dataset --\n");
    nvs_sim_reset();
    nvs_sim_put_u8(PARTITION, NS, "mode", (uint8_t)POWER_BASE_THREAD_MODE_AWAIT_COMMISSIONING);
    static const uint8_t stray[] = {0xAA, 0xBB, 0xCC};
    nvs_sim_put_blob(PARTITION, NS, "active_tlvs", stray, sizeof(stray));

    power_base_thread_config_t out;
    esp_err_t err = power_base_thread_config_load(&out);
    CHECK(err == ESP_OK, "load succeeds for explicit commissioning wait mode");
    CHECK(out.mode == POWER_BASE_THREAD_MODE_AWAIT_COMMISSIONING, "mode remains AWAIT_COMMISSIONING");
    CHECK(out.active_tlvs_len == 0, "stale active_tlvs is not surfaced");
    static const uint8_t zero[POWER_BASE_THREAD_CONFIG_MAX_TLV_LEN] = {0};
    CHECK(memcmp(out.active_tlvs, zero, sizeof(zero)) == 0, "stale dataset is not copied into RAM");
}

static void scenario_nvs_read_error(void)
{
    printf("\n-- scenario: a genuine NVS/partition init error -> fail closed (distinct from 'namespace absent') "
           "--\n");
    nvs_sim_reset();
    nvs_sim_force_next_init_error(ESP_ERR_NVS_NOT_INITIALIZED);

    power_base_thread_config_t out;
    esp_err_t err = power_base_thread_config_load(&out);
    CHECK(err != ESP_OK, "load FAILS when the underlying NVS partition init itself fails");
    CHECK(err == ESP_ERR_NVS_NOT_INITIALIZED, "the underlying NVS error code is propagated, not swallowed");
    check_zeroed_standalone(&out, "NVS init error");
}

static void scenario_out_zeroed_before_anything_else(void)
{
    printf("\n-- scenario: *out is memset to 0 before any lookup even begins (poisoned input) --\n");
    nvs_sim_reset();
    nvs_sim_create_namespace(PARTITION, "power_base_cfg"); /* legacy image, no pb_thread namespace */

    power_base_thread_config_t out;
    memset(&out, 0xFF, sizeof(out)); /* poison */
    esp_err_t err = power_base_thread_config_load(&out);
    CHECK(err == ESP_OK, "load succeeds (legacy compat path)");
    CHECK(out.mode == POWER_BASE_THREAD_MODE_STANDALONE, "poisoned *out is correctly reset to STANDALONE");
    CHECK(out.active_tlvs_len == 0, "poisoned *out's active_tlvs_len is correctly reset to 0");
}

int main(void)
{
    /*
     * MUST run first: power_base_thread_config.c caches partition-init
     * success in a file-scope static (s_partition_ready), exactly like the
     * real firmware only ever calls nvs_flash_init_partition() once per
     * boot -- so a forced init-error injection is only observable on the
     * very first power_base_thread_config_load() call this process ever
     * makes. This mirrors real boot behavior (init failure would only ever
     * happen on the actual first call), not a test artifact to work around.
     */
    scenario_nvs_read_error();

    scenario_legacy_namespace_missing();
    scenario_namespace_present_mode_missing();
    scenario_explicit_standalone();
    scenario_explicit_standalone_ignores_stray_active_tlvs();
    scenario_valid_external();
    scenario_external_missing_blob();
    scenario_external_empty_blob();
    scenario_external_oversized_blob();
    scenario_external_max_len_exactly_ok();
    scenario_await_commissioning();
    scenario_invalid_mode_value();
    scenario_out_zeroed_before_anything_else();

    printf("\n");
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
