/*
 * Host-compiled verification of the REAL, unmodified
 * components/power_base_bridge_config/power_base_bridge_config.c (this
 * follow-up: Power Base -> Linux OTBR northbound bridge transport), linked
 * against the functional in-memory NVS fake (tools/host_test_stubs/nvs_sim/)
 * instead of a re-implementation of its decision logic -- same policy as
 * tools/test_power_base_thread_config.c.
 *
 * Covers:
 *   - no "pb_bridge" namespace at all -> disabled (legacy-image compatible)
 *   - namespace present, enabled=false -> disabled, host_addr/port never validated
 *   - valid enabled=true config -> exact byte/port match
 *   - blob missing / wrong size / CRC mismatch / unsupported schema_version -> fail closed
 *   - enabled=true with an unspecified (::) host_addr -> fail closed
 *   - enabled=true with a multicast host_addr -> fail closed
 *   - enabled=true with a link-local host_addr -> fail closed
 *   - enabled=true with port=0 -> fail closed
 *   - every fail-closed case also asserts *out was zeroed (enabled=false)
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra \
 *      -I tools/host_test_stubs/nvs_sim -I tools/host_test_stubs \
 *      -I components/power_base_bridge_config/include -I components/pogo_proto/include \
 *      -o /tmp/test_power_base_bridge_config tools/test_power_base_bridge_config.c \
 *      components/power_base_bridge_config/power_base_bridge_config.c \
 *      components/pogo_proto/pogo_crc16.c \
 *      tools/host_test_stubs/nvs_sim/nvs_sim.c \
 *   && /tmp/test_power_base_bridge_config
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "power_base_bridge_config/power_base_bridge_config.h"
#include "pogo_proto/pogo_crc16.h"
#include "nvs.h"
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
#define NS "pb_bridge"
#define BLOB_LEN 24

static void check_zeroed_disabled(const power_base_bridge_config_t *out, const char *ctx)
{
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: enabled reads back as false on failure", ctx);
    CHECK(out->enabled == false, msg);
    snprintf(msg, sizeof(msg), "%s: port is 0 on failure", ctx);
    CHECK(out->port == 0, msg);
    static const uint8_t zero[POWER_BASE_BRIDGE_CONFIG_ADDR_LEN] = {0};
    snprintf(msg, sizeof(msg), "%s: host_addr is all-zero on failure", ctx);
    CHECK(memcmp(out->host_addr, zero, sizeof(zero)) == 0, msg);
}

/* Builds a valid 24-byte blob + its correct CRC and stores both under NS. */
static void put_valid_blob(uint32_t schema_version, bool enabled, const uint8_t addr[16], uint16_t port)
{
    uint8_t blob[BLOB_LEN] = {0};
    blob[0] = (uint8_t)(schema_version >> 24);
    blob[1] = (uint8_t)(schema_version >> 16);
    blob[2] = (uint8_t)(schema_version >> 8);
    blob[3] = (uint8_t)schema_version;
    blob[4] = enabled ? 1 : 0;
    blob[5] = 0;
    if (addr != NULL) {
        memcpy(blob + 6, addr, 16);
    }
    blob[22] = (uint8_t)(port >> 8);
    blob[23] = (uint8_t)port;

    nvs_sim_put_blob(PARTITION, NS, "blob", blob, BLOB_LEN);
    nvs_sim_put_u16(PARTITION, NS, "crc", pogo_crc16_ccitt(blob, BLOB_LEN));
}

static void scenario_namespace_missing(void)
{
    printf("\n-- scenario: no 'pb_bridge' namespace at all -> disabled (legacy-image compatible) --\n");
    nvs_sim_reset();
    nvs_sim_create_namespace(PARTITION, "power_base_cfg");

    power_base_bridge_config_t out;
    esp_err_t err = power_base_bridge_config_load(&out);
    CHECK(err == ESP_OK, "load succeeds when 'pb_bridge' namespace is entirely absent");
    CHECK(out.enabled == false, "enabled is false when the namespace is absent");
}

static void scenario_explicit_disabled(void)
{
    printf("\n-- scenario: namespace present, enabled=false -- garbage host_addr/port never validated --\n");
    nvs_sim_reset();
    static const uint8_t garbage_multicast[16] = {0xFF, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    put_valid_blob(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, false, garbage_multicast, 0);

    power_base_bridge_config_t out;
    esp_err_t err = power_base_bridge_config_load(&out);
    CHECK(err == ESP_OK, "load succeeds for enabled=false even though host_addr/port would fail validation");
    CHECK(out.enabled == false, "enabled reads back as false");
}

static void scenario_valid_enabled(void)
{
    printf("\n-- scenario: valid enabled=true config -- exact byte/port match --\n");
    nvs_sim_reset();
    /* A fabricated, non-real Mesh-Local-shaped IPv6 address (fd00::/8 ULA
     * range) -- never a real OTBR MLEID, see this follow-up's own
     * secrecy/no-hardcoding policy. */
    static const uint8_t fake_mleid[16] = {0xfd, 0x00, 0xAB, 0xCD, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
    put_valid_blob(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, true, fake_mleid, 5684);

    power_base_bridge_config_t out;
    esp_err_t err = power_base_bridge_config_load(&out);
    CHECK(err == ESP_OK, "load succeeds for a valid enabled config");
    CHECK(out.enabled == true, "enabled reads back as true");
    CHECK(memcmp(out.host_addr, fake_mleid, 16) == 0, "host_addr matches the stored blob exactly, byte-for-byte");
    CHECK(out.port == 5684, "port matches the stored blob exactly");
}

static void scenario_blob_missing(void)
{
    printf("\n-- scenario: namespace present but 'blob' key entirely missing -> fail closed --\n");
    nvs_sim_reset();
    nvs_sim_create_namespace(PARTITION, NS);

    power_base_bridge_config_t out;
    esp_err_t err = power_base_bridge_config_load(&out);
    CHECK(err != ESP_OK, "load FAILS when 'blob' is missing");
    check_zeroed_disabled(&out, "missing blob");
}

static void scenario_blob_wrong_size(void)
{
    printf("\n-- scenario: 'blob' present but wrong size -> fail closed --\n");
    nvs_sim_reset();
    static const uint8_t too_short[10] = {0};
    nvs_sim_put_blob(PARTITION, NS, "blob", too_short, sizeof(too_short));
    nvs_sim_put_u16(PARTITION, NS, "crc", pogo_crc16_ccitt(too_short, sizeof(too_short)));

    power_base_bridge_config_t out;
    esp_err_t err = power_base_bridge_config_load(&out);
    CHECK(err != ESP_OK, "load FAILS when 'blob' is the wrong size");
    check_zeroed_disabled(&out, "wrong-size blob");
}

static void scenario_crc_mismatch(void)
{
    printf("\n-- scenario: 'blob'/'crc' both present but mismatched -> fail closed --\n");
    nvs_sim_reset();
    static const uint8_t fake_addr[16] = {0xfd, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    put_valid_blob(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, true, fake_addr, 5684);
    nvs_sim_put_u16(PARTITION, NS, "crc", 0x1234); /* overwrite with a wrong CRC */

    power_base_bridge_config_t out;
    esp_err_t err = power_base_bridge_config_load(&out);
    CHECK(err != ESP_OK, "load FAILS on a CRC mismatch");
    check_zeroed_disabled(&out, "CRC mismatch");
}

static void scenario_wrong_schema_version(void)
{
    printf("\n-- scenario: unsupported schema_version -> fail closed --\n");
    nvs_sim_reset();
    static const uint8_t fake_addr[16] = {0xfd, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    put_valid_blob(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION + 1, true, fake_addr, 5684);

    power_base_bridge_config_t out;
    esp_err_t err = power_base_bridge_config_load(&out);
    CHECK(err != ESP_OK, "load FAILS on an unsupported schema_version");
    check_zeroed_disabled(&out, "wrong schema_version");
}

static void scenario_enabled_unspecified_addr(void)
{
    printf("\n-- scenario: enabled=true but host_addr is unspecified (::) -> fail closed --\n");
    nvs_sim_reset();
    static const uint8_t unspecified[16] = {0};
    put_valid_blob(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, true, unspecified, 5684);

    power_base_bridge_config_t out;
    esp_err_t err = power_base_bridge_config_load(&out);
    CHECK(err != ESP_OK, "load FAILS for an unspecified host_addr while enabled");
    check_zeroed_disabled(&out, "unspecified host_addr");
}

static void scenario_enabled_multicast_addr(void)
{
    printf("\n-- scenario: enabled=true but host_addr is multicast (ff00::/8) -> fail closed --\n");
    nvs_sim_reset();
    static const uint8_t multicast[16] = {0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
    put_valid_blob(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, true, multicast, 5684);

    power_base_bridge_config_t out;
    esp_err_t err = power_base_bridge_config_load(&out);
    CHECK(err != ESP_OK, "load FAILS for a multicast host_addr while enabled");
    check_zeroed_disabled(&out, "multicast host_addr");
}

static void scenario_enabled_link_local_addr(void)
{
    printf("\n-- scenario: enabled=true but host_addr is link-local (fe80::/10) -> fail closed --\n");
    nvs_sim_reset();
    static const uint8_t link_local[16] = {0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
    put_valid_blob(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, true, link_local, 5684);

    power_base_bridge_config_t out;
    esp_err_t err = power_base_bridge_config_load(&out);
    CHECK(err != ESP_OK, "load FAILS for a link-local host_addr while enabled");
    check_zeroed_disabled(&out, "link-local host_addr");

    printf("\n-- boundary: fe7f::... (just below the fe80::/10 range) is NOT rejected as link-local --\n");
    nvs_sim_reset();
    static const uint8_t just_below[16] = {0xFE, 0x7F, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
    put_valid_blob(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, true, just_below, 5684);
    esp_err_t err2 = power_base_bridge_config_load(&out);
    CHECK(err2 == ESP_OK, "fe7f::.../ ... does not trip the link-local check");

    printf("\n-- boundary: fec0::... (just above the fe80::/10 range) is NOT rejected as link-local --\n");
    nvs_sim_reset();
    static const uint8_t just_above[16] = {0xFE, 0xC0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
    put_valid_blob(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, true, just_above, 5684);
    esp_err_t err3 = power_base_bridge_config_load(&out);
    CHECK(err3 == ESP_OK, "fec0::.../ ... does not trip the link-local check");
}

static void scenario_enabled_port_zero(void)
{
    printf("\n-- scenario: enabled=true but port=0 -> fail closed --\n");
    nvs_sim_reset();
    static const uint8_t fake_addr[16] = {0xfd, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    put_valid_blob(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, true, fake_addr, 0);

    power_base_bridge_config_t out;
    esp_err_t err = power_base_bridge_config_load(&out);
    CHECK(err != ESP_OK, "load FAILS for port=0 while enabled");
    check_zeroed_disabled(&out, "port=0");
}

int main(void)
{
    scenario_namespace_missing();
    scenario_explicit_disabled();
    scenario_valid_enabled();
    scenario_blob_missing();
    scenario_blob_wrong_size();
    scenario_crc_mismatch();
    scenario_wrong_schema_version();
    scenario_enabled_unspecified_addr();
    scenario_enabled_multicast_addr();
    scenario_enabled_link_local_addr();
    scenario_enabled_port_zero();

    printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
