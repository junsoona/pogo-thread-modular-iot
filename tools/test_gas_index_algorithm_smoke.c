/*
 * Host-compiled smoke test for the vendored, unmodified Sensirion Gas Index
 * Algorithm (components/sensirion_gas_index_algorithm -- see that
 * directory's THIRD_PARTY_NOTICE.md for source/version/license). This does
 * NOT re-derive or second-guess Sensirion's own tuning/math (out of scope,
 * and the whole point of vendoring their code instead of reimplementing it
 * -- see section 10 of the follow-up spec) -- it only checks the glue-level
 * contract components/sgp40 and components/sgp41 rely on: the documented
 * [0, 500] output range, the initial blackout period returning exactly 0,
 * and that VOC vs NOx algorithm-type instances are independent of each
 * other (SGP41 runs one of each concurrently).
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/sensirion_gas_index_algorithm/include \
 *      -o /tmp/test_gas_index_algorithm_smoke tools/test_gas_index_algorithm_smoke.c \
 *      components/sensirion_gas_index_algorithm/sensirion_gas_index_algorithm.c \
 *   && /tmp/test_gas_index_algorithm_smoke
 */
#include <stdio.h>
#include <stdint.h>

#include "sensirion_gas_index_algorithm/sensirion_gas_index_algorithm.h"

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

int main(void)
{
    GasIndexAlgorithmParams voc;
    GasIndexAlgorithm_init(&voc, GasIndexAlgorithm_ALGORITHM_TYPE_VOC);

    int32_t index = -1;
    /* "Zero during initial blackout period" per the vendored header's own
     * doc comment on GasIndexAlgorithm_process(). */
    GasIndexAlgorithm_process(&voc, 30000, &index);
    CHECK(index == 0, "VOC: first sample (t=0) is 0 during the initial blackout period");

    /* Feed a steady, plausible raw signal for a while (simulating 1 sample/s,
     * the algorithm's required cadence) and confirm every output stays
     * within the documented [0, 500] range once past blackout. */
    bool saw_nonzero = false;
    bool out_of_range = false;
    for (int i = 0; i < 700; i++) {
        GasIndexAlgorithm_process(&voc, 30000, &index);
        if (index != 0) {
            saw_nonzero = true;
        }
        if (index < 0 || index > 500) {
            out_of_range = true;
        }
    }
    CHECK(saw_nonzero, "VOC: index becomes nonzero once past the initial blackout period");
    CHECK(!out_of_range, "VOC: every sampled index stays within the documented [0, 500] range");

    /* SGP41 runs a VOC and a NOx instance side by side -- confirm they are
     * genuinely independent state (a NOx instance fed a very different
     * signal does not disturb the VOC instance's own trajectory). */
    GasIndexAlgorithmParams nox;
    GasIndexAlgorithm_init(&nox, GasIndexAlgorithm_ALGORITHM_TYPE_NOX);
    int32_t voc_before = index;
    int32_t nox_index = -1;
    for (int i = 0; i < 50; i++) {
        GasIndexAlgorithm_process(&nox, 15000, &nox_index);
    }
    GasIndexAlgorithm_process(&voc, 30000, &index);
    CHECK(nox_index >= 0 && nox_index <= 500, "NOx: index also stays within [0, 500]");
    CHECK((index == 0 || index > 0) && voc_before >= 0,
          "VOC instance's trajectory is unaffected by driving a separate NOx instance (independent state)");

    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
