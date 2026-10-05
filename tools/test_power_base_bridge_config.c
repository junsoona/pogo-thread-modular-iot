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

/* ---- power_base_bridge_config_apply_delivered(): the endpoint an
 *      enrollment delivered replaces whatever the prepared image said ---- */

static void scenario_apply_delivered(void)
{
    printf("\n-- scenario: a delivered endpoint replaces the prepared image's pb_bridge, in RAM only --\n");
    static const uint8_t image_addr[16] = {0xfd, 0x00, 0xAB, 0xCD, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
    static const uint8_t delivered[16] = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 0x8c, 0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79};

    /* Address-less image (pb_bridge enabled=0). */
    nvs_sim_reset();
    put_valid_blob(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, false, NULL, 0);
    power_base_bridge_config_t cfg;
    CHECK(power_base_bridge_config_load(&cfg) == ESP_OK && !cfg.enabled, "(setup) the address-less image loads as disabled");
    CHECK(power_base_bridge_config_apply_delivered(&cfg, delivered, 5701) == ESP_OK && cfg.enabled &&
              memcmp(cfg.host_addr, delivered, 16) == 0 && cfg.port == 5701,
          "disabled image + delivered endpoint: enabled with exactly the delivered address and port");

    /* No pb_bridge namespace at all. */
    nvs_sim_reset();
    nvs_sim_create_namespace(PARTITION, "power_base_cfg");
    CHECK(power_base_bridge_config_load(&cfg) == ESP_OK && !cfg.enabled, "(setup) no pb_bridge namespace: disabled");
    CHECK(power_base_bridge_config_apply_delivered(&cfg, delivered, 5701) == ESP_OK && cfg.enabled &&
              memcmp(cfg.host_addr, delivered, 16) == 0,
          "no pb_bridge namespace + delivered endpoint: enabled with the delivered address");

    /* Image with its own static address. */
    nvs_sim_reset();
    put_valid_blob(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, true, image_addr, 5684);
    CHECK(power_base_bridge_config_load(&cfg) == ESP_OK && cfg.enabled && memcmp(cfg.host_addr, image_addr, 16) == 0,
          "(setup) an image with a static address loads it");
    CHECK(power_base_bridge_config_apply_delivered(&cfg, delivered, 5701) == ESP_OK &&
              memcmp(cfg.host_addr, delivered, 16) == 0 && cfg.port == 5701,
          "static image + delivered endpoint: the delivered one is used, not the image's");
    power_base_bridge_config_t again;
    CHECK(power_base_bridge_config_load(&again) == ESP_OK && again.enabled &&
              memcmp(again.host_addr, image_addr, 16) == 0 && again.port == 5684,
          "the cfg partition itself was not changed (it is a read-only prepared image)");

    /* A damaged pb_bridge in the image does not matter to a delivered one. */
    nvs_sim_reset();
    put_valid_blob(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, true, image_addr, 5684);
    nvs_sim_put_u16(PARTITION, NS, "crc", 0x1234);
    CHECK(power_base_bridge_config_load(&cfg) != ESP_OK && !cfg.enabled, "(setup) the image's pb_bridge is damaged");
    CHECK(power_base_bridge_config_apply_delivered(&cfg, delivered, 5701) == ESP_OK && cfg.enabled &&
              memcmp(cfg.host_addr, delivered, 16) == 0,
          "damaged image pb_bridge + delivered endpoint: the delivered one is used");

    printf("\n-- scenario: an unusable delivered endpoint disables the exporter -- never back to the image's address --\n");
    static const struct {
        uint8_t addr[16];
        uint16_t port;
        const char *what;
    } bad[] = {
        {{0}, 5701, "unspecified address"},
        {{0xff, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 5701, "multicast address"},
        {{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8}, 5701, "link-local address"},
        {{0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 0x8c, 0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79}, 0, "port 0"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        nvs_sim_reset();
        put_valid_blob(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, true, image_addr, 5684);
        power_base_bridge_config_load(&cfg);
        char msg[160];
        snprintf(msg, sizeof(msg), "%s: refused", bad[i].what);
        CHECK(power_base_bridge_config_apply_delivered(&cfg, bad[i].addr, bad[i].port) != ESP_OK, msg);
        check_zeroed_disabled(&cfg, bad[i].what);
    }
}

/* ---- power_base_bridge_config_load_enroll_policy(): the explicit
 *      "bridge endpoint required at enrollment" policy (blob byte 5) ---- */

/* A 24-byte blob with every field chosen by the caller, sealed with its CRC. */
static void put_blob_with_policy(uint32_t schema_version, bool enabled, uint8_t policy_byte, const uint8_t addr[16],
                                 uint16_t port)
{
    uint8_t blob[BLOB_LEN] = {0};
    blob[0] = (uint8_t)(schema_version >> 24);
    blob[1] = (uint8_t)(schema_version >> 16);
    blob[2] = (uint8_t)(schema_version >> 8);
    blob[3] = (uint8_t)schema_version;
    blob[4] = enabled ? 1 : 0;
    blob[5] = policy_byte;
    if (addr != NULL) {
        memcpy(blob + 6, addr, 16);
    }
    blob[22] = (uint8_t)(port >> 8);
    blob[23] = (uint8_t)port;
    nvs_sim_put_blob(PARTITION, NS, "blob", blob, BLOB_LEN);
    nvs_sim_put_u16(PARTITION, NS, "crc", pogo_crc16_ccitt(blob, BLOB_LEN));
}

static void scenario_enroll_policy(void)
{
    printf("\n-- scenario: enrollment endpoint policy -- explicit, never guessed from enabled=0 --\n");
    static const uint8_t addr[16] = {0xfd, 0x00, 0xAB, 0xCD, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
    power_base_bridge_config_t cfg;

    /* The cfg partition cannot be initialised. This must be the first load
     * of the process: the component remembers a successful init. An
     * unreadable policy is never "no policy". */
    nvs_sim_reset();
    nvs_sim_create_namespace(PARTITION, "power_base_cfg");
    nvs_sim_force_next_init_error(ESP_FAIL);
    CHECK(power_base_bridge_config_load_enroll_policy() == POWER_BASE_BRIDGE_ENROLL_POLICY_INVALID,
          "the cfg partition cannot be read: invalid (not 'no policy')");

    /* What exists today: no policy. */
    nvs_sim_reset();
    nvs_sim_create_namespace(PARTITION, "power_base_cfg");
    CHECK(power_base_bridge_config_load_enroll_policy() == POWER_BASE_BRIDGE_ENROLL_POLICY_NONE,
          "no pb_bridge namespace at all (legacy image): no policy");
    nvs_sim_reset();
    put_valid_blob(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, true, addr, 5684);
    CHECK(power_base_bridge_config_load_enroll_policy() == POWER_BASE_BRIDGE_ENROLL_POLICY_NONE,
          "an image with a static address (policy byte 0): no policy");
    nvs_sim_reset();
    put_valid_blob(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, false, NULL, 0);
    CHECK(power_base_bridge_config_load_enroll_policy() == POWER_BASE_BRIDGE_ENROLL_POLICY_NONE,
          "an image with the exporter switched off on purpose (enabled=0, policy byte 0): NO policy -- 'disabled' "
          "is not 'endpoint required'");
    nvs_sim_reset();
    static const uint8_t garbage_multicast[16] = {0xFF, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    put_valid_blob(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, false, garbage_multicast, 0);
    CHECK(power_base_bridge_config_load_enroll_policy() == POWER_BASE_BRIDGE_ENROLL_POLICY_NONE,
          "disabled with leftover address bytes (never validated, as before): no policy");

    /* The address-less image of gen_power_base_ble_config.py --bridge-from-enrollment. */
    nvs_sim_reset();
    put_blob_with_policy(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, false, 1, NULL, 0);
    CHECK(power_base_bridge_config_load_enroll_policy() == POWER_BASE_BRIDGE_ENROLL_POLICY_ENDPOINT_REQUIRED,
          "enabled=0 with policy byte 1: endpoint required");
    CHECK(power_base_bridge_config_load(&cfg) == ESP_OK && !cfg.enabled,
          "the ordinary loader reads the same image exactly as before: disabled, no error");

    /* A policy that is there but is not one this firmware knows. */
    for (unsigned v = 2; v <= 255; v += (v < 4 ? 1 : 251)) {
        nvs_sim_reset();
        put_blob_with_policy(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, false, (uint8_t)v, NULL, 0);
        CHECK(power_base_bridge_config_load_enroll_policy() == POWER_BASE_BRIDGE_ENROLL_POLICY_INVALID,
              "an unknown policy value (2, 3, 4, 255): invalid");
    }
    nvs_sim_reset();
    put_blob_with_policy(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, true, 1, addr, 5684);
    CHECK(power_base_bridge_config_load_enroll_policy() == POWER_BASE_BRIDGE_ENROLL_POLICY_INVALID,
          "'endpoint required' together with a static address (enabled=1): invalid");
    CHECK(power_base_bridge_config_load(&cfg) == ESP_OK && cfg.enabled && memcmp(cfg.host_addr, addr, 16) == 0,
          "the ordinary loader (modes 0/1) does not look at the policy byte: that image still loads as enabled");

    /* A policy that cannot be trusted because the blob around it cannot. */
    nvs_sim_reset();
    put_blob_with_policy(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, false, 1, NULL, 0);
    nvs_sim_put_u16(PARTITION, NS, "crc", 0x1234);
    CHECK(power_base_bridge_config_load_enroll_policy() == POWER_BASE_BRIDGE_ENROLL_POLICY_INVALID,
          "CRC mismatch: invalid");
    nvs_sim_reset();
    put_blob_with_policy(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, false, 0, NULL, 0);
    {
        /* One flipped bit in the policy byte itself, CRC left as it was. */
        uint8_t blob[BLOB_LEN] = {0, 0, 0, 1, 0, 1};
        nvs_sim_put_blob(PARTITION, NS, "blob", blob, BLOB_LEN);
    }
    CHECK(power_base_bridge_config_load_enroll_policy() == POWER_BASE_BRIDGE_ENROLL_POLICY_INVALID,
          "a policy byte changed without its CRC (0 -> 1): invalid, not 'required'");
    nvs_sim_reset();
    put_blob_with_policy(POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION, false, 1, NULL, 0);
    {
        uint8_t blob[BLOB_LEN] = {0, 0, 0, 1, 0, 0};
        nvs_sim_put_blob(PARTITION, NS, "blob", blob, BLOB_LEN);
    }
    CHECK(power_base_bridge_config_load_enroll_policy() == POWER_BASE_BRIDGE_ENROLL_POLICY_INVALID,
          "a policy byte changed without its CRC (1 -> 0): invalid, not 'no policy'");
    nvs_sim_reset();
    put_blob_with_policy(2, false, 1, NULL, 0);
    CHECK(power_base_bridge_config_load_enroll_policy() == POWER_BASE_BRIDGE_ENROLL_POLICY_INVALID,
          "another schema version: invalid");
    nvs_sim_reset();
    {
        uint8_t short_blob[10] = {0, 0, 0, 1, 0, 1};
        nvs_sim_put_blob(PARTITION, NS, "blob", short_blob, sizeof(short_blob));
        nvs_sim_put_u16(PARTITION, NS, "crc", pogo_crc16_ccitt(short_blob, sizeof(short_blob)));
    }
    CHECK(power_base_bridge_config_load_enroll_policy() == POWER_BASE_BRIDGE_ENROLL_POLICY_INVALID,
          "a blob of another size: invalid");
    nvs_sim_reset();
    nvs_sim_create_namespace(PARTITION, NS);
    CHECK(power_base_bridge_config_load_enroll_policy() == POWER_BASE_BRIDGE_ENROLL_POLICY_INVALID,
          "the namespace without its blob: invalid");
    nvs_sim_reset();
    {
        uint8_t blob[BLOB_LEN] = {0, 0, 0, 1, 0, 1};
        nvs_sim_put_blob(PARTITION, NS, "blob", blob, BLOB_LEN);
    }
    CHECK(power_base_bridge_config_load_enroll_policy() == POWER_BASE_BRIDGE_ENROLL_POLICY_INVALID,
          "the blob without its CRC key: invalid");
    nvs_sim_reset();
}

int main(void)
{
    scenario_enroll_policy();
    scenario_apply_delivered();
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
