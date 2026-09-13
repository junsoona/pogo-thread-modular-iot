/*
 * Host-compiled test for the topology report wire format
 * (components/topology_proto/topology_proto.c) -- required test items 11/12
 * of the power-base follow-up spec: "topology report pack/unpack", "짧은
 * 패킷, 긴 패킷, 잘못된 version 거부".
 *
 * Links against the REAL, unmodified topology_proto.c (no ESP-IDF/FreeRTOS
 * dependency), same approach as tools/test_sensor_channel_pack.c for
 * sensor_channel.c.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/topology_proto/include \
 *      -o /tmp/test_topology_proto_pack tools/test_topology_proto_pack.c \
 *      components/topology_proto/topology_proto.c \
 *   && /tmp/test_topology_proto_pack
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "topology_proto/topology_proto.h"

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

static topology_report_t make_report(void)
{
    topology_report_t r = {0};
    r.payload_version = TOPOLOGY_REPORT_PAYLOAD_VERSION;
    for (int i = 0; i < TOPOLOGY_UUID_LEN; i++) {
        r.module_uuid[i] = (uint8_t)(0x10 + i);
        r.parent_uuid[i] = (uint8_t)(0x20 + i);
    }
    r.topology_depth = 2;
    r.role = TOPOLOGY_ROLE_INTERMEDIATE;
    r.sensor_type_bitmap = 0x0003;
    r.boot_session_generation = 0xDEADBEEFu;
    r.sequence = 42;
    r.upstream_connected = true;
    r.downstream_connected = false;
    r.report_type = TOPOLOGY_REPORT_UPDATED;
    return r;
}

static void scenario_round_trip(void)
{
    printf("\n-- scenario: pack/unpack round trip --\n");
    topology_report_t r = make_report();
    uint8_t buf[TOPOLOGY_REPORT_WIRE_LEN];
    size_t n = topology_report_pack(&r, buf, sizeof(buf));
    CHECK(n == TOPOLOGY_REPORT_WIRE_LEN, "pack returns exactly TOPOLOGY_REPORT_WIRE_LEN bytes");

    topology_report_t out;
    bool ok = topology_report_unpack(buf, (uint16_t)n, &out);
    CHECK(ok, "unpack succeeds on a freshly packed buffer");
    CHECK(memcmp(out.module_uuid, r.module_uuid, TOPOLOGY_UUID_LEN) == 0, "module_uuid round-trips");
    CHECK(memcmp(out.parent_uuid, r.parent_uuid, TOPOLOGY_UUID_LEN) == 0, "parent_uuid round-trips");
    CHECK(out.topology_depth == r.topology_depth, "topology_depth round-trips");
    CHECK(out.role == r.role, "role round-trips");
    CHECK(out.sensor_type_bitmap == r.sensor_type_bitmap, "sensor_type_bitmap round-trips");
    CHECK(out.boot_session_generation == r.boot_session_generation, "boot_session_generation round-trips");
    CHECK(out.sequence == r.sequence, "sequence round-trips");
    CHECK(out.upstream_connected == r.upstream_connected, "upstream_connected round-trips");
    CHECK(out.downstream_connected == r.downstream_connected, "downstream_connected round-trips");
    CHECK(out.report_type == r.report_type, "report_type round-trips");
}

static void scenario_short_and_long_packets_rejected(void)
{
    printf("\n-- scenario: short/long packets rejected --\n");
    topology_report_t r = make_report();
    uint8_t buf[TOPOLOGY_REPORT_WIRE_LEN];
    size_t n = topology_report_pack(&r, buf, sizeof(buf));

    topology_report_t out;
    CHECK(!topology_report_unpack(buf, (uint16_t)(n - 1), &out), "one byte short is rejected");
    CHECK(!topology_report_unpack(buf, 0, &out), "zero length is rejected");

    uint8_t long_buf[TOPOLOGY_REPORT_WIRE_LEN + 4] = {0};
    memcpy(long_buf, buf, n);
    CHECK(!topology_report_unpack(long_buf, (uint16_t)(n + 1), &out), "one byte too long (trailing garbage) is rejected");

    uint8_t too_small_out[4];
    CHECK(topology_report_pack(&r, too_small_out, sizeof(too_small_out)) == 0, "pack refuses an undersized output buffer");
}

static void scenario_bad_version_rejected(void)
{
    printf("\n-- scenario: unsupported payload_version is rejected --\n");
    topology_report_t r = make_report();
    uint8_t buf[TOPOLOGY_REPORT_WIRE_LEN];
    size_t n = topology_report_pack(&r, buf, sizeof(buf));
    buf[0] = TOPOLOGY_REPORT_PAYLOAD_VERSION + 1;

    topology_report_t out;
    CHECK(!topology_report_unpack(buf, (uint16_t)n, &out), "a mismatched payload_version is rejected outright");
}

static void scenario_bad_role_and_report_type_rejected(void)
{
    printf("\n-- scenario: invalid role/report_type bytes are rejected --\n");
    topology_report_t r = make_report();

    r.role = 5; /* one past TOPOLOGY_ROLE_ERROR */
    uint8_t buf[TOPOLOGY_REPORT_WIRE_LEN];
    CHECK(topology_report_pack(&r, buf, sizeof(buf)) == 0, "pack refuses an out-of-range role");

    r = make_report();
    r.report_type = 4; /* one past TOPOLOGY_REPORT_HEARTBEAT */
    CHECK(topology_report_pack(&r, buf, sizeof(buf)) == 0, "pack refuses an out-of-range report_type");

    /* A well-formed-length buffer with a garbage role/report_type byte
     * written directly (bypassing pack()) must also be rejected by unpack --
     * a corrupt/malicious sender must not get a free pass just because pack()
     * itself would have refused it. */
    r = make_report();
    size_t n = topology_report_pack(&r, buf, sizeof(buf));
    buf[18] = 5; /* role byte offset */
    topology_report_t out;
    CHECK(!topology_report_unpack(buf, (uint16_t)n, &out), "unpack rejects an out-of-range role byte even at the right length");

    n = topology_report_pack(&r, buf, sizeof(buf));
    buf[31] = 4; /* report_type byte offset */
    CHECK(!topology_report_unpack(buf, (uint16_t)n, &out), "unpack rejects an out-of-range report_type byte even at the right length");
}

int main(void)
{
    scenario_round_trip();
    scenario_short_and_long_packets_rejected();
    scenario_bad_version_rejected();
    scenario_bad_role_and_report_type_rejected();

    printf("\n");
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
