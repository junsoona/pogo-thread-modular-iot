/*
 * Host-compiled verification of POGO_MSG_SET_COLLECTOR's wire format
 * (components/pogo_proto/pogo_protocol.c's pogo_pack_set_collector()/
 * pogo_unpack_set_collector()) and its address/port validators
 * (pogo_collector_endpoint_is_valid()/pogo_ipv6_addr_is_valid_collector(),
 * both inline in pogo_protocol.h) -- test items 7/8 of this follow-up's
 * required scenarios: "collector payload pack/unpack round trip",
 * "잘못된 길이/address/port 거부".
 *
 * Links against the REAL, unmodified components/pogo_proto/pogo_protocol.c
 * (no ESP-IDF/FreeRTOS dependency).
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/pogo_proto/include \
 *      -o /tmp/test_collector_provisioning_pack tools/test_collector_provisioning_pack.c \
 *      components/pogo_proto/pogo_protocol.c \
 *   && /tmp/test_collector_provisioning_pack
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "pogo_proto/pogo_protocol.h"

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

/* A plausible Thread Mesh-Local EID: fd00::/8 ULA prefix, non-zero IID. */
static void make_ml_eid(uint8_t out[16], uint8_t last_byte)
{
    memset(out, 0, 16);
    out[0] = 0xfd;
    out[1] = 0x11;
    out[2] = 0x22;
    out[15] = last_byte;
}

static void scenario_pack_unpack_round_trip(void)
{
    printf("\n-- scenario: SET_COLLECTOR pack/unpack round trip --\n");

    pogo_collector_endpoint_t in = {0};
    for (int i = 0; i < POGO_UUID_LEN; i++) {
        in.power_base_uuid[i] = (uint8_t)(0x10 + i);
    }
    make_ml_eid(in.addr, 0x42);
    in.port = 5683;
    in.generation = 0xDEADBEEFu;

    uint8_t wire[POGO_COLLECTOR_ENDPOINT_WIRE_LEN];
    size_t len = pogo_pack_set_collector(&in, wire, sizeof(wire));
    CHECK(len == POGO_COLLECTOR_ENDPOINT_WIRE_LEN, "pack produces exactly the documented 30-byte wire length");
    CHECK(POGO_COLLECTOR_ENDPOINT_WIRE_LEN == 30, "POGO_COLLECTOR_ENDPOINT_WIRE_LEN is 30 (8 uuid + 16 addr + 2 port + 4 generation)");

    /* Byte order: port and generation are big-endian (network byte order),
     * matching every other multi-byte field in this protocol
     * (pogo_pack_poll_config, pogo_pack_sensor_info_ack, ...). */
    CHECK(wire[POGO_UUID_LEN + POGO_COLLECTOR_ADDR_LEN] == 0x16, "port high byte (5683 = 0x1633) is at the expected offset, big-endian");
    CHECK(wire[POGO_UUID_LEN + POGO_COLLECTOR_ADDR_LEN + 1] == 0x33, "port low byte is at the expected offset, big-endian");
    size_t gen_off = POGO_UUID_LEN + POGO_COLLECTOR_ADDR_LEN + 2;
    CHECK(wire[gen_off] == 0xDE && wire[gen_off + 1] == 0xAD && wire[gen_off + 2] == 0xBE && wire[gen_off + 3] == 0xEF,
          "generation (0xDEADBEEF) is encoded big-endian at the expected offset");

    pogo_collector_endpoint_t out = {0};
    bool ok = pogo_unpack_set_collector(wire, (uint16_t)len, &out);
    CHECK(ok, "unpack succeeds on the exact wire length");
    CHECK(memcmp(out.power_base_uuid, in.power_base_uuid, POGO_UUID_LEN) == 0, "power_base_uuid round-trips exactly");
    CHECK(memcmp(out.addr, in.addr, POGO_COLLECTOR_ADDR_LEN) == 0, "addr round-trips exactly");
    CHECK(out.port == in.port, "port round-trips exactly");
    CHECK(out.generation == in.generation, "generation round-trips exactly");
    CHECK(pogo_collector_endpoint_is_valid(&out), "the round-tripped endpoint is itself valid");
}

static void scenario_bad_length_rejected(void)
{
    printf("\n-- scenario: SET_COLLECTOR unpack rejects a wrong length --\n");

    pogo_collector_endpoint_t ep;
    uint8_t short_buf[POGO_COLLECTOR_ENDPOINT_WIRE_LEN - 1] = {0};
    CHECK(!pogo_unpack_set_collector(short_buf, sizeof(short_buf), &ep), "one byte too short is rejected");

    uint8_t long_buf[POGO_COLLECTOR_ENDPOINT_WIRE_LEN + 1] = {0};
    CHECK(!pogo_unpack_set_collector(long_buf, sizeof(long_buf), &ep), "one byte too long is rejected");

    CHECK(!pogo_unpack_set_collector(short_buf, 0, &ep), "zero length is rejected");

    /* pack() itself refuses to write into a too-small buffer. */
    pogo_collector_endpoint_t in = {0};
    make_ml_eid(in.addr, 1);
    memset(in.power_base_uuid, 0xAB, POGO_UUID_LEN);
    in.port = 1234;
    uint8_t small_out[POGO_COLLECTOR_ENDPOINT_WIRE_LEN - 1];
    CHECK(pogo_pack_set_collector(&in, small_out, sizeof(small_out)) == 0,
          "pack refuses to write into a buffer one byte too small");
}

static void scenario_bad_address_rejected(void)
{
    printf("\n-- scenario: bad addresses are rejected by pogo_collector_endpoint_is_valid() --\n");

    pogo_collector_endpoint_t ep = {0};
    memset(ep.power_base_uuid, 0x01, POGO_UUID_LEN);
    ep.port = 5683;

    /* Unspecified (::). */
    memset(ep.addr, 0, sizeof(ep.addr));
    CHECK(!pogo_collector_endpoint_is_valid(&ep), "the unspecified address (::) is rejected");
    CHECK(!pogo_ipv6_addr_is_valid_collector(ep.addr), "pogo_ipv6_addr_is_valid_collector() also rejects it directly");

    /* Multicast (ff00::/8). */
    memset(ep.addr, 0, sizeof(ep.addr));
    ep.addr[0] = 0xFF;
    ep.addr[1] = 0x02; /* ff02::1, a common all-nodes multicast address */
    ep.addr[15] = 0x01;
    CHECK(!pogo_collector_endpoint_is_valid(&ep), "a multicast address (ff02::1) is rejected");

    /* Link-local (fe80::/10). */
    memset(ep.addr, 0, sizeof(ep.addr));
    ep.addr[0] = 0xFE;
    ep.addr[1] = 0x80;
    ep.addr[15] = 0x01;
    CHECK(!pogo_collector_endpoint_is_valid(&ep), "a link-local address (fe80::1) is rejected");

    /* A borderline non-link-local address just past fe80::/10 (fec0:: was
     * the old deprecated site-local range, distinct from fe80::/10) must
     * NOT be rejected by the link-local check specifically. */
    memset(ep.addr, 0, sizeof(ep.addr));
    ep.addr[0] = 0xFE;
    ep.addr[1] = 0xC0;
    ep.addr[15] = 0x01;
    CHECK(pogo_ipv6_addr_is_valid_collector(ep.addr),
          "fec0::1 (outside fe80::/10) is not rejected by the link-local check");

    /* A real-shaped Mesh-Local EID (fd00::/8 ULA) is accepted. */
    make_ml_eid(ep.addr, 0x55);
    CHECK(pogo_collector_endpoint_is_valid(&ep), "a plausible Mesh-Local EID (fd11:2200::55) is accepted");
}

static void scenario_bad_port_and_uuid_rejected(void)
{
    printf("\n-- scenario: port 0 and an all-zero power_base_uuid are rejected --\n");

    pogo_collector_endpoint_t ep = {0};
    memset(ep.power_base_uuid, 0x01, POGO_UUID_LEN);
    make_ml_eid(ep.addr, 0x02);
    ep.port = 0;
    CHECK(!pogo_collector_endpoint_is_valid(&ep), "port 0 is rejected");

    ep.port = 5683;
    memset(ep.power_base_uuid, 0, POGO_UUID_LEN);
    CHECK(!pogo_collector_endpoint_is_valid(&ep), "an all-zero power_base_uuid is rejected");

    ep.power_base_uuid[0] = 0x01;
    CHECK(pogo_collector_endpoint_is_valid(&ep), "a non-all-zero uuid + valid address + nonzero port is accepted");

    CHECK(!pogo_collector_endpoint_is_valid(NULL), "a NULL endpoint pointer is rejected, not dereferenced");
}

int main(void)
{
    scenario_pack_unpack_round_trip();
    scenario_bad_length_rejected();
    scenario_bad_address_rejected();
    scenario_bad_port_and_uuid_rejected();

    printf("\n");
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
