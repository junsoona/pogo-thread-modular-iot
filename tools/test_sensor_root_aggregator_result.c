/*
 * Host-compiled test for the NEW sensor_root_aggregator_record_sample_result()
 * API (correction pass on the northbound bridge transport follow-up) --
 * links against the REAL, unmodified sensor_root_aggregator.c, no mirrored
 * logic. Covers STORED/DUPLICATE/FULL directly (BUSY requires genuine
 * cross-thread mutex contention and is exercised indirectly via
 * tools/test_power_base_transport_bridge_events.c's own FULL scenario and
 * by code inspection: BUSY and FULL share the exact same
 * "xSemaphoreTake failed / table full -> do not store" code shape).
 *
 * Also confirms the back-compat void wrapper
 * sensor_root_aggregator_record_sample() still behaves identically to
 * before this follow-up (same dedup rule, same storage), for every
 * pre-existing caller (sensor_service.c's MODULE_ROLE_ROOT/DEV_ROOT path).
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I tools/host_test_stubs -I components/sensor_root_aggregator/include \
 *      -I components/sensor_channel/include \
 *      -o /tmp/test_sensor_root_aggregator_result tools/test_sensor_root_aggregator_result.c \
 *      components/sensor_root_aggregator/sensor_root_aggregator.c components/sensor_channel/sensor_channel.c \
 *   && /tmp/test_sensor_root_aggregator_result
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "sensor_root_aggregator/sensor_root_aggregator.h"

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

static void make_sample(sensor_sample_t *s, uint8_t uuid_tag, uint32_t seq)
{
    memset(s, 0, sizeof(*s));
    s->payload_version = SENSOR_CHANNEL_PAYLOAD_VERSION;
    memset(s->module_uuid, uuid_tag, SENSOR_UUID_LEN);
    s->sample_seq = seq;
    s->uptime_ms = 1;
    s->sensor_bitmap = 0x0001;
    s->channel_count = 1;
    s->channels[0].type = SENSOR_CHANNEL_TEMPERATURE;
    s->channels[0].scale = SENSOR_SCALE_MILLI;
    s->channels[0].status = SENSOR_CHANNEL_STATUS_OK;
    s->channels[0].value = 100;
}

int main(void)
{
    printf("\n-- scenario: a brand-new (uuid, seq) is STORED --\n");
    {
        sensor_root_aggregator_init();
        sensor_sample_t s;
        make_sample(&s, 0x01, 1);
        CHECK(sensor_root_aggregator_record_sample_result(&s) == SENSOR_ROOT_AGGREGATOR_STORED,
              "a brand-new sample returns STORED");
        CHECK(sensor_root_aggregator_get_node_count() == 1, "the node count reflects the stored sample");
    }

    printf("\n-- scenario: an exact repeat (same uuid, same seq) is DUPLICATE, never re-stored --\n");
    {
        sensor_root_aggregator_init();
        sensor_sample_t s;
        make_sample(&s, 0x02, 5);
        CHECK(sensor_root_aggregator_record_sample_result(&s) == SENSOR_ROOT_AGGREGATOR_STORED, "first: STORED");
        CHECK(sensor_root_aggregator_record_sample_result(&s) == SENSOR_ROOT_AGGREGATOR_DUPLICATE,
              "exact repeat: DUPLICATE");

        sensor_sample_t different_seq;
        make_sample(&different_seq, 0x02, 6);
        CHECK(sensor_root_aggregator_record_sample_result(&different_seq) == SENSOR_ROOT_AGGREGATOR_STORED,
              "a genuinely different seq for the same uuid: STORED again");
    }

    printf("\n-- scenario: a brand-new uuid past SENSOR_ROOT_AGGREGATOR_MAX_NODES is FULL, never stored --\n");
    {
        sensor_root_aggregator_init();
        for (int i = 0; i < SENSOR_ROOT_AGGREGATOR_MAX_NODES; i++) {
            sensor_sample_t s;
            make_sample(&s, (uint8_t)i, 1);
            char msg[80];
            snprintf(msg, sizeof(msg), "filler uuid %d: STORED", i);
            CHECK(sensor_root_aggregator_record_sample_result(&s) == SENSOR_ROOT_AGGREGATOR_STORED, msg);
        }
        CHECK(sensor_root_aggregator_get_node_count() == SENSOR_ROOT_AGGREGATOR_MAX_NODES, "the table is now full");

        sensor_sample_t overflow;
        make_sample(&overflow, 0xFE, 1);
        CHECK(sensor_root_aggregator_record_sample_result(&overflow) == SENSOR_ROOT_AGGREGATOR_FULL,
              "a brand-new uuid past capacity returns FULL");
        CHECK(sensor_root_aggregator_get_node_count() == SENSOR_ROOT_AGGREGATOR_MAX_NODES,
              "the table size never exceeds its bound after a FULL result");

        /* An update to an ALREADY-known uuid must still work while full. */
        sensor_sample_t existing_update;
        make_sample(&existing_update, 0x00, 2);
        CHECK(sensor_root_aggregator_record_sample_result(&existing_update) == SENSOR_ROOT_AGGREGATOR_STORED,
              "a new sample for an already-tracked uuid still succeeds while the table is full");
    }

    printf("\n-- scenario: sample == NULL is INVALID --\n");
    {
        sensor_root_aggregator_init();
        CHECK(sensor_root_aggregator_record_sample_result(NULL) == SENSOR_ROOT_AGGREGATOR_INVALID,
              "a NULL sample pointer returns INVALID");
    }

    printf("\n-- scenario: the back-compat void wrapper still stores exactly like before --\n");
    {
        sensor_root_aggregator_init();
        sensor_sample_t s;
        make_sample(&s, 0x09, 3);
        sensor_root_aggregator_record_sample(&s); /* old void API -- must not crash, must still store */
        CHECK(sensor_root_aggregator_get_node_count() == 1, "the void wrapper still stores a brand-new sample");

        sensor_root_aggregator_node_t node;
        CHECK(sensor_root_aggregator_get_node(0, &node) && node.has_sample && node.last_sample_seq == 3,
              "the stored sample's fields are correct via the void wrapper");

        sensor_root_aggregator_record_sample(&s); /* exact duplicate via the void wrapper */
        CHECK(sensor_root_aggregator_get_node_count() == 1,
              "the void wrapper's duplicate call does not create a second node or corrupt state");
    }

    printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
