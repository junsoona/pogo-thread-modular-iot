/*
 * Host test for the REAL components/power_base_enrollment/power_base_enrollment.c
 * (pb_thread:mode=2 enrollment rules), linked against the functional NVS
 * simulator (tools/host_test_stubs/nvs_sim) and a fake of the OpenThread
 * operations that power_base/main/enrollment_runtime.c supplies.
 *
 * The fake OpenThread keeps only what matters to the rules: whether a
 * Network Key / a complete Active Dataset is stored and which network it
 * names, whether Thread is enabled, and whether a Joiner is running. It
 * refuses to enable Thread without a stored key, like
 * thread_manager_start_as_always_on_ftd_stored().
 *
 * Also the Linux bridge endpoint an enrollment may deliver: what it must be,
 * that it stays in RAM until the board is enrolled, the version 2 record it
 * is stored in together with the marker, and what a later boot does with an
 * intact, damaged, foreign or mismatching record.
 *
 * Reboots and power loss are modelled by running each boot in a forked child
 * that loads the simulated flash from a file (nvs_sim_set_backing_file) and
 * exits at the chosen point: whatever was not committed is gone.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I tools/host_test_stubs/nvs_sim -I tools/host_test_stubs \
 *      -I components/power_base_enrollment/include \
 *      -o /tmp/test_power_base_enrollment tools/test_power_base_enrollment.c \
 *      components/power_base_enrollment/power_base_enrollment.c tools/host_test_stubs/nvs_sim/nvs_sim.c \
 *   && /tmp/test_power_base_enrollment
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nvs.h"
#include "nvs_sim.h"
#include "power_base_enrollment/power_base_enrollment.h"

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

static const uint8_t EUI_A[8] = {0x60, 0x55, 0xf9, 0xff, 0xfe, 0x00, 0x12, 0x34};
static const uint8_t EUI_B[8] = {0x60, 0x55, 0xf9, 0xff, 0xfe, 0x00, 0x56, 0x78};
/* Made-up identifiers; nothing here is a real network. */
static const pb_enroll_network_id_t NET_OTBR = {
    .ext_pan_id = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88},
    .mesh_local_prefix = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00},
};
/* What an old STANDALONE test leaves behind (the Kconfig defaults). */
static const pb_enroll_network_id_t NET_STANDALONE = {
    .ext_pan_id = {0xde, 0xad, 0x00, 0xbe, 0xef, 0x00, 0xca, 0xfe},
    .mesh_local_prefix = {0xfd, 0xde, 0xad, 0x00, 0xbe, 0xef, 0x00, 0x00},
};
static const char *PSKD = "J01NME7K";

/* ---- fake OpenThread ---- */

typedef struct {
    /* stored Active Dataset */
    bool has_key;
    bool complete;
    pb_enroll_network_id_t id;
    bool read_fails;
    /* runtime */
    bool thread_enabled;
    bool joiner_running;
    esp_err_t joiner_start_ret;
    esp_err_t thread_start_ret;
    int joiner_start_calls;
    int joiner_stop_calls;
    int thread_start_calls;
    int thread_start_incomplete_calls; /* require_complete=false */
    int thread_stop_calls;
    uint32_t last_attempt;
    bool pskd_matched;
    pb_enroll_marker_status_t marker_at_joiner_start;
} fake_ot_t;

static fake_ot_t g_ot;

static esp_err_t f_joiner_start(const char *pskd, uint32_t attempt, void *ctx)
{
    (void)ctx;
    g_ot.joiner_start_calls++;
    g_ot.last_attempt = attempt;
    g_ot.pskd_matched = strcmp(pskd, PSKD) == 0;
    pb_enroll_network_id_t ignored;
    g_ot.marker_at_joiner_start = pb_enroll_marker_load(EUI_A, &ignored);
    if (g_ot.joiner_start_ret == ESP_OK) {
        g_ot.joiner_running = true;
    }
    return g_ot.joiner_start_ret;
}

static void f_joiner_stop(void *ctx)
{
    (void)ctx;
    g_ot.joiner_stop_calls++;
    g_ot.joiner_running = false;
}

static esp_err_t f_thread_start_stored(bool require_complete, void *ctx)
{
    (void)ctx;
    g_ot.thread_start_calls++;
    if (!require_complete) {
        g_ot.thread_start_incomplete_calls++;
    }
    if (g_ot.thread_start_ret != ESP_OK) {
        return g_ot.thread_start_ret;
    }
    if (!g_ot.has_key || (require_complete && !g_ot.complete)) {
        return ESP_ERR_INVALID_STATE;
    }
    g_ot.thread_enabled = true;
    return ESP_OK;
}

static void f_thread_stop(void *ctx)
{
    (void)ctx;
    g_ot.thread_stop_calls++;
    g_ot.thread_enabled = false;
}

static bool f_read_dataset(pb_enroll_dataset_info_t *out, void *ctx)
{
    (void)ctx;
    if (g_ot.read_fails) {
        return false;
    }
    out->has_network_key = g_ot.has_key;
    out->complete = g_ot.complete;
    if (g_ot.complete) {
        out->id = g_ot.id;
    }
    return true;
}

static const pb_enroll_ops_t k_ops = {
    .joiner_start = f_joiner_start,
    .joiner_stop = f_joiner_stop,
    .thread_start_stored = f_thread_start_stored,
    .thread_stop = f_thread_stop,
    .read_dataset = f_read_dataset,
    .ctx = NULL,
};

static void ot_reset(void)
{
    memset(&g_ot, 0, sizeof(g_ot));
}

static void ot_store_complete(const pb_enroll_network_id_t *id)
{
    g_ot.has_key = true;
    g_ot.complete = true;
    g_ot.id = *id;
}

/* What OpenThread's Joiner::HandleTmf<kUriJoinerEntrust> leaves: the local
 * Active Dataset is replaced by Network Key + Channel + PAN ID only. */
static void ot_store_joiner_partial(void)
{
    g_ot.has_key = true;
    g_ot.complete = false;
    memset(&g_ot.id, 0, sizeof(g_ot.id));
}

static void put_valid_marker(const uint8_t eui[8], const pb_enroll_network_id_t *id)
{
    CHECK(pb_enroll_marker_store(eui, id) == ESP_OK, "(setup) marker stored");
}

static void fill_pskd(char buf[PB_ENROLL_PSKD_BUF_LEN], const char *s)
{
    memset(buf, 0, PB_ENROLL_PSKD_BUF_LEN);
    strncpy(buf, s, PB_ENROLL_PSKD_BUF_LEN - 1);
}

static bool all_zero(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        if (b[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool marker_is(pb_enroll_marker_status_t expected)
{
    pb_enroll_network_id_t id;
    return pb_enroll_marker_load(EUI_A, &id) == expected;
}

/* A fresh waiting controller on an empty board. */
static void fresh_waiting(pb_enroll_ctl_t *c)
{
    nvs_sim_reset();
    ot_reset();
    pb_enroll_ctl_init(c, &k_ops, EUI_A);
    pb_enroll_ctl_boot(c);
}

/* Drives a waiting controller to ATTACHING via a successful Joiner. */
static void join_ok(pb_enroll_ctl_t *c, uint32_t now)
{
    char buf[PB_ENROLL_PSKD_BUF_LEN];
    fill_pskd(buf, PSKD);
    pb_enroll_ctl_request_join(c, buf, now);
    ot_store_joiner_partial();
    pb_enroll_ctl_on_joiner_done(c, g_ot.last_attempt, PB_ENROLL_JOINER_OK, now + 5000);
}

/* ---- scenarios ---- */

static void scenario_pskd_format(void)
{
    printf("\n-- PSKd format (Thread rule: 6..32 of 0-9 A-Z without I O Q Z) --\n");
    CHECK(pb_enroll_pskd_is_valid("J01NME", 6), "6 characters accepted");
    CHECK(pb_enroll_pskd_is_valid("ABCDEFGHJKLMNPRSTUVWXY0123456789", 32), "32 characters accepted");
    CHECK(!pb_enroll_pskd_is_valid("J01NM", 5), "5 characters rejected");
    CHECK(!pb_enroll_pskd_is_valid("ABCDEFGHJKLMNPRSTUVWXY01234567890", 33), "33 characters rejected");
    CHECK(!pb_enroll_pskd_is_valid("j01nme7k", 8), "lower case rejected");
    CHECK(!pb_enroll_pskd_is_valid("ABCDEI", 6) && !pb_enroll_pskd_is_valid("ABCDEO", 6) &&
              !pb_enroll_pskd_is_valid("ABCDEQ", 6) && !pb_enroll_pskd_is_valid("ABCDEZ", 6),
          "I, O, Q and Z rejected");
    CHECK(!pb_enroll_pskd_is_valid("ABC-EF", 6) && !pb_enroll_pskd_is_valid("ABC EF", 6),
          "separators and spaces rejected");
}

static void scenario_boot_rules(void)
{
    printf("\n-- boot rule: resume only with a valid marker AND the matching complete Dataset --\n");
    pb_enroll_ctl_t c;

    nvs_sim_reset();
    ot_reset();
    pb_enroll_ctl_init(&c, &k_ops, EUI_A);
    CHECK(pb_enroll_ctl_boot(&c) == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_NONE,
          "empty board: waits");
    CHECK(g_ot.thread_start_calls == 0 && !g_ot.thread_enabled, "empty board: Thread never enabled");

    nvs_sim_reset();
    ot_reset();
    ot_store_complete(&NET_STANDALONE);
    pb_enroll_ctl_init(&c, &k_ops, EUI_A);
    CHECK(pb_enroll_ctl_boot(&c) == PB_ENROLL_STATE_WAITING, "old STANDALONE Dataset without marker: waits");
    CHECK(g_ot.thread_start_calls == 0, "old STANDALONE Dataset is never resumed");
    CHECK(!pb_enroll_ctl_services_allowed(&c), "old STANDALONE Dataset: services not allowed");

    nvs_sim_reset();
    ot_reset();
    ot_store_complete(&NET_OTBR);
    put_valid_marker(EUI_A, &NET_OTBR);
    pb_enroll_ctl_init(&c, &k_ops, EUI_A);
    CHECK(pb_enroll_ctl_boot(&c) == PB_ENROLL_STATE_ENROLLED && c.resumed && c.last == PB_ENROLL_RESULT_RESUMED,
          "valid marker + matching Dataset: resumes");
    CHECK(g_ot.thread_start_calls == 1 && g_ot.thread_start_incomplete_calls == 0 && g_ot.thread_enabled,
          "resume enables Thread once, requiring a complete stored Dataset");
    CHECK(g_ot.joiner_start_calls == 0, "resume never runs the Joiner");

    nvs_sim_reset();
    ot_reset();
    put_valid_marker(EUI_A, &NET_OTBR);
    pb_enroll_ctl_init(&c, &k_ops, EUI_A);
    CHECK(pb_enroll_ctl_boot(&c) == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_DATASET_MISSING,
          "valid marker, no Dataset: waits (dataset_missing)");
    CHECK(g_ot.thread_start_calls == 0, "valid marker, no Dataset: Thread never enabled");

    nvs_sim_reset();
    ot_reset();
    ot_store_joiner_partial();
    put_valid_marker(EUI_A, &NET_OTBR);
    pb_enroll_ctl_init(&c, &k_ops, EUI_A);
    CHECK(pb_enroll_ctl_boot(&c) == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_DATASET_MISSING,
          "valid marker, only the Joiner's partial Dataset: waits");

    nvs_sim_reset();
    ot_reset();
    ot_store_complete(&NET_STANDALONE);
    put_valid_marker(EUI_A, &NET_OTBR);
    pb_enroll_ctl_init(&c, &k_ops, EUI_A);
    CHECK(pb_enroll_ctl_boot(&c) == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_DATASET_MISMATCH,
          "valid marker, Dataset of another network: waits (dataset_mismatch)");
    CHECK(g_ot.thread_start_calls == 0, "mismatch: Thread never enabled");

    nvs_sim_reset();
    ot_reset();
    ot_store_complete(&NET_OTBR);
    put_valid_marker(EUI_B, &NET_OTBR);
    pb_enroll_ctl_init(&c, &k_ops, EUI_A);
    CHECK(pb_enroll_ctl_boot(&c) == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_MARKER_FOREIGN,
          "marker written for another EUI-64 (e.g. a copied NVS image): waits");

    nvs_sim_reset();
    ot_reset();
    ot_store_complete(&NET_OTBR);
    put_valid_marker(EUI_A, &NET_OTBR);
    {
        /* Flip one byte of the stored marker (inside the network id). */
        nvs_handle_t h;
        uint8_t raw[PB_ENROLL_MARKER_LEN];
        size_t len = sizeof(raw);
        nvs_open(PB_ENROLL_NVS_NAMESPACE, NVS_READWRITE, &h);
        nvs_get_blob(h, PB_ENROLL_NVS_KEY, raw, &len);
        raw[20] ^= 0x01;
        nvs_set_blob(h, PB_ENROLL_NVS_KEY, raw, sizeof(raw));
        nvs_commit(h);
        nvs_close(h);
    }
    pb_enroll_ctl_init(&c, &k_ops, EUI_A);
    CHECK(pb_enroll_ctl_boot(&c) == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_MARKER_CORRUPT,
          "marker with a flipped bit (CRC mismatch): waits");

    nvs_sim_reset();
    ot_reset();
    ot_store_complete(&NET_OTBR);
    {
        static const uint8_t short_blob[10] = {'P', 'B', 'E', '1', 1};
        nvs_sim_put_blob("nvs", PB_ENROLL_NVS_NAMESPACE, PB_ENROLL_NVS_KEY, short_blob, sizeof(short_blob));
    }
    pb_enroll_ctl_init(&c, &k_ops, EUI_A);
    CHECK(pb_enroll_ctl_boot(&c) == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_MARKER_CORRUPT,
          "truncated marker: waits");

    nvs_sim_reset();
    ot_reset();
    ot_store_complete(&NET_OTBR);
    put_valid_marker(EUI_A, &NET_OTBR);
    g_ot.thread_start_ret = ESP_FAIL;
    pb_enroll_ctl_init(&c, &k_ops, EUI_A);
    CHECK(pb_enroll_ctl_boot(&c) == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_THREAD_START_FAILED,
          "resume but Thread start fails: waits, services not allowed");
    CHECK(marker_is(PB_ENROLL_MARKER_VALID), "a transient Thread start failure keeps the marker for the next boot");

    nvs_sim_reset();
    ot_reset();
    ot_store_complete(&NET_OTBR);
    g_ot.read_fails = true;
    put_valid_marker(EUI_A, &NET_OTBR);
    pb_enroll_ctl_init(&c, &k_ops, EUI_A);
    CHECK(pb_enroll_ctl_boot(&c) == PB_ENROLL_STATE_WAITING, "Dataset unreadable: treated as missing, waits");
}

static void scenario_join_rejections(void)
{
    printf("\n-- Joiner requests that must not start anything --\n");
    pb_enroll_ctl_t c;
    char buf[PB_ENROLL_PSKD_BUF_LEN];

    fresh_waiting(&c);
    fill_pskd(buf, "j01nme7k");
    CHECK(pb_enroll_ctl_request_join(&c, buf, 0) == ESP_ERR_INVALID_ARG, "malformed PSKd rejected");
    CHECK(all_zero(buf, sizeof(buf)), "malformed PSKd buffer wiped");
    CHECK(g_ot.joiner_start_calls == 0 && c.state == PB_ENROLL_STATE_WAITING, "malformed PSKd: no Joiner, still waiting");

    fresh_waiting(&c);
    memset(buf, 'A', sizeof(buf)); /* not NUL-terminated within the buffer */
    CHECK(pb_enroll_ctl_request_join(&c, buf, 0) == ESP_ERR_INVALID_ARG, "unterminated 33-byte input rejected");
    CHECK(all_zero(buf, sizeof(buf)), "unterminated input wiped");

    fresh_waiting(&c);
    g_ot.joiner_start_ret = ESP_FAIL;
    fill_pskd(buf, PSKD);
    CHECK(pb_enroll_ctl_request_join(&c, buf, 0) == ESP_FAIL && c.last == PB_ENROLL_RESULT_JOINER_START_FAILED,
          "Joiner start failure reported");
    CHECK(all_zero(buf, sizeof(buf)) && c.state == PB_ENROLL_STATE_WAITING && g_ot.thread_stop_calls == 1,
          "Joiner start failure: PSKd wiped, IPv6/Thread stopped, waiting");

    fresh_waiting(&c);
    fill_pskd(buf, PSKD);
    pb_enroll_ctl_request_join(&c, buf, 0);
    fill_pskd(buf, PSKD);
    CHECK(pb_enroll_ctl_request_join(&c, buf, 10) == ESP_ERR_INVALID_STATE && all_zero(buf, sizeof(buf)),
          "second request while joining refused, and wiped");
    CHECK(g_ot.joiner_start_calls == 1, "only one Joiner started");
}

static void scenario_joiner_failures(void)
{
    printf("\n-- Joiner failures: back to the safe wait, nothing persisted, no services --\n");
    static const struct {
        pb_enroll_joiner_outcome_t outcome;
        pb_enroll_result_t expect;
        const char *what;
    } cases[] = {
        {PB_ENROLL_JOINER_ERR_SECURITY, PB_ENROLL_RESULT_JOINER_SECURITY, "wrong PSKd (DTLS/security)"},
        {PB_ENROLL_JOINER_ERR_NOT_FOUND, PB_ENROLL_RESULT_JOINER_NOT_FOUND, "no OTBR / window closed (not found)"},
        {PB_ENROLL_JOINER_ERR_TIMEOUT, PB_ENROLL_RESULT_JOINER_TIMEOUT, "response timeout"},
        {PB_ENROLL_JOINER_ERR_OTHER, PB_ENROLL_RESULT_JOINER_FAILED, "other Joiner error"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        pb_enroll_ctl_t c;
        fresh_waiting(&c);
        char buf[PB_ENROLL_PSKD_BUF_LEN];
        fill_pskd(buf, PSKD);
        CHECK(pb_enroll_ctl_request_join(&c, buf, 0) == ESP_OK && g_ot.pskd_matched && all_zero(buf, sizeof(buf)),
              "Joiner got the PSKd; the caller's copy is wiped right after the start");
        pb_enroll_ctl_on_joiner_done(&c, g_ot.last_attempt, cases[i].outcome, 3000);
        pb_enroll_ctl_tick(&c, true, 4000);
        printf("      (%s)\n", cases[i].what);
        CHECK(c.state == PB_ENROLL_STATE_WAITING && c.last == cases[i].expect, "failure reported, back to waiting");
        CHECK(g_ot.thread_start_calls == 0 && !g_ot.thread_enabled, "Thread never enabled after a failed Joiner");
        CHECK(marker_is(PB_ENROLL_MARKER_ABSENT) && !pb_enroll_ctl_services_allowed(&c),
              "no marker, services not allowed");
    }

    pb_enroll_ctl_t c;
    fresh_waiting(&c);
    char buf[PB_ENROLL_PSKD_BUF_LEN];
    fill_pskd(buf, PSKD);
    pb_enroll_ctl_request_join(&c, buf, 1000);
    pb_enroll_ctl_tick(&c, false, 1000 + PB_ENROLL_JOINER_GUARD_MS - 1);
    CHECK(c.state == PB_ENROLL_STATE_JOINING, "Joiner guard not yet reached: still joining");
    pb_enroll_ctl_tick(&c, false, 1000 + PB_ENROLL_JOINER_GUARD_MS);
    CHECK(c.state == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_JOINER_GUARD_EXPIRED &&
              g_ot.joiner_stop_calls == 1 && !g_ot.joiner_running,
          "no Joiner result at all: guard stops the Joiner and waits");
    pb_enroll_ctl_on_joiner_done(&c, g_ot.last_attempt, PB_ENROLL_JOINER_OK, 1000 + PB_ENROLL_JOINER_GUARD_MS + 5);
    CHECK(c.state == PB_ENROLL_STATE_WAITING && g_ot.thread_start_calls == 0,
          "a late success for the abandoned attempt is ignored");

    fresh_waiting(&c);
    fill_pskd(buf, PSKD);
    pb_enroll_ctl_request_join(&c, buf, 0);
    uint32_t first = g_ot.last_attempt;
    CHECK(pb_enroll_ctl_cancel(&c) == ESP_OK && c.last == PB_ENROLL_RESULT_CANCELLED && g_ot.joiner_stop_calls == 1,
          "cancel while joining stops the Joiner");
    fill_pskd(buf, PSKD);
    pb_enroll_ctl_request_join(&c, buf, 10);
    pb_enroll_ctl_on_joiner_done(&c, first, PB_ENROLL_JOINER_OK, 20);
    CHECK(c.state == PB_ENROLL_STATE_JOINING && g_ot.thread_start_calls == 0,
          "a result from the cancelled attempt does not complete the new one");
}

static void scenario_join_success_and_attach(void)
{
    printf("\n-- Joiner success, attach and marker are separate milestones --\n");
    pb_enroll_ctl_t c;
    fresh_waiting(&c);
    join_ok(&c, 1000);
    CHECK(c.state == PB_ENROLL_STATE_ATTACHING, "Joiner success -> attaching (not enrolled)");
    CHECK(g_ot.thread_start_calls == 1 && g_ot.thread_start_incomplete_calls == 1 && g_ot.thread_enabled,
          "Thread enabled once on the Joiner's partial Dataset (no network formed)");
    CHECK(!pb_enroll_ctl_services_allowed(&c) && marker_is(PB_ENROLL_MARKER_ABSENT),
          "after Joiner success: no services, no marker");

    pb_enroll_ctl_tick(&c, false, 7000);
    CHECK(c.state == PB_ENROLL_STATE_ATTACHING && !pb_enroll_ctl_services_allowed(&c), "detached: still attaching");
    pb_enroll_ctl_tick(&c, true, 8000);
    CHECK(c.state == PB_ENROLL_STATE_ATTACHING && marker_is(PB_ENROLL_MARKER_ABSENT),
          "attached but Dataset still partial: no marker, no services");

    ot_store_complete(&NET_OTBR); /* the Child ID Response delivered it */
    pb_enroll_ctl_tick(&c, true, 9000);
    CHECK(c.state == PB_ENROLL_STATE_ENROLLED && c.last == PB_ENROLL_RESULT_ENROLLED && !c.resumed,
          "attached + complete Dataset -> enrolled");
    pb_enroll_network_id_t id;
    CHECK(pb_enroll_marker_load(EUI_A, &id) == PB_ENROLL_MARKER_VALID && memcmp(&id, &NET_OTBR, sizeof(id)) == 0,
          "marker names this EUI-64 and the joined network");
    CHECK(pb_enroll_ctl_services_allowed(&c), "services allowed only now");

    int starts = g_ot.thread_start_calls;
    pb_enroll_ctl_tick(&c, false, 20000);
    pb_enroll_ctl_tick(&c, true, 30000 + PB_ENROLL_ATTACH_DEADLINE_MS);
    CHECK(c.state == PB_ENROLL_STATE_ENROLLED && g_ot.thread_start_calls == starts && g_ot.thread_stop_calls == 0,
          "later role changes / time never undo enrollment or restart Thread");
    char buf[PB_ENROLL_PSKD_BUF_LEN];
    fill_pskd(buf, PSKD);
    CHECK(pb_enroll_ctl_request_join(&c, buf, 40000) == ESP_ERR_INVALID_STATE && pb_enroll_ctl_cancel(&c) != ESP_OK,
          "enrolled: new Joiner and cancel refused");

    printf("\n-- attach never completes --\n");
    fresh_waiting(&c);
    join_ok(&c, 1000);
    pb_enroll_ctl_tick(&c, true, 1000 + 5000 + PB_ENROLL_ATTACH_DEADLINE_MS - 1);
    CHECK(c.state == PB_ENROLL_STATE_ATTACHING, "before the deadline: still attaching");
    pb_enroll_ctl_tick(&c, false, 1000 + 5000 + PB_ENROLL_ATTACH_DEADLINE_MS);
    CHECK(c.state == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_ATTACH_TIMEOUT && !g_ot.thread_enabled,
          "deadline: Thread stopped, waiting");
    CHECK(marker_is(PB_ENROLL_MARKER_ABSENT) && !pb_enroll_ctl_services_allowed(&c), "attach timeout: no marker");

    printf("\n-- Joiner success but Thread cannot start --\n");
    fresh_waiting(&c);
    char b2[PB_ENROLL_PSKD_BUF_LEN];
    fill_pskd(b2, PSKD);
    pb_enroll_ctl_request_join(&c, b2, 0);
    /* Joiner reported success but nothing usable was stored. */
    pb_enroll_ctl_on_joiner_done(&c, g_ot.last_attempt, PB_ENROLL_JOINER_OK, 100);
    CHECK(c.state == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_THREAD_START_FAILED && !g_ot.thread_enabled,
          "no stored key after 'success': Thread refused, waiting");

    printf("\n-- marker cannot be written --\n");
    fresh_waiting(&c);
    join_ok(&c, 1000);
    ot_store_complete(&NET_OTBR);
    nvs_sim_force_next_commit_error(ESP_FAIL);
    pb_enroll_ctl_tick(&c, true, 9000);
    CHECK(c.state == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_MARKER_WRITE_FAILED && !g_ot.thread_enabled,
          "marker commit failed: Thread stopped, not enrolled");
    CHECK(marker_is(PB_ENROLL_MARKER_ABSENT), "a half-written marker is removed");

    printf("\n-- a new Joiner attempt first removes an old marker --\n");
    nvs_sim_reset();
    ot_reset();
    put_valid_marker(EUI_A, &NET_OTBR); /* Dataset gone: boot waits with a stale marker */
    pb_enroll_ctl_init(&c, &k_ops, EUI_A);
    pb_enroll_ctl_boot(&c);
    fill_pskd(b2, PSKD);
    CHECK(pb_enroll_ctl_request_join(&c, b2, 0) == ESP_OK, "Joiner started from dataset_missing");
    CHECK(g_ot.marker_at_joiner_start == PB_ENROLL_MARKER_ABSENT, "the old marker was gone before the Joiner ran");

    fresh_waiting(&c);
    put_valid_marker(EUI_A, &NET_OTBR);
    nvs_sim_force_next_commit_error(ESP_FAIL);
    fill_pskd(b2, PSKD);
    CHECK(pb_enroll_ctl_request_join(&c, b2, 0) == ESP_FAIL && c.last == PB_ENROLL_RESULT_MARKER_CLEAR_FAILED &&
              g_ot.joiner_start_calls == 0 && all_zero(b2, sizeof(b2)),
          "old marker cannot be removed: Joiner refused, PSKd wiped");
}

static void scenario_pskd_never_logged(void)
{
    printf("\n-- the PSKd never reaches the log output --\n");
    char path[] = "/tmp/pb_enroll_logXXXXXX";
    int fd = mkstemp(path);
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    dup2(fd, STDERR_FILENO);

    pb_enroll_ctl_t c;
    fresh_waiting(&c);
    char buf[PB_ENROLL_PSKD_BUF_LEN];
    fill_pskd(buf, PSKD);
    pb_enroll_ctl_request_join(&c, buf, 0);
    pb_enroll_ctl_on_joiner_done(&c, g_ot.last_attempt, PB_ENROLL_JOINER_ERR_SECURITY, 10);
    join_ok(&c, 100);
    ot_store_complete(&NET_OTBR);
    pb_enroll_ctl_tick(&c, true, 200);
    fill_pskd(buf, "J01NMEI"); /* malformed */
    pb_enroll_ctl_request_join(&c, buf, 300);

    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    char log[16384] = {0};
    lseek(fd, 0, SEEK_SET);
    ssize_t n = read(fd, log, sizeof(log) - 1);
    close(fd);
    unlink(path);
    CHECK(n > 0, "the scenario produced log output");
    CHECK(strstr(log, PSKD) == NULL && strstr(log, "J01NMEI") == NULL, "no PSKd (valid or not) in the log");
}

/* ---- reboots / power loss (forked boots over a simulated flash file) ---- */

enum { STOP_AFTER_JOINER_OK = 1, STOP_AFTER_ATTACH_BEFORE_MARKER, STOP_AFTER_MARKER, STOP_AFTER_JOIN_STARTED };

static char g_flash[64];

/* Runs one boot in a child. `stop_at` 0 = boot only. Returns the child's
 * exit code: 10 + resulting state (after `stop_at`) or 90+ on setup error. */
static int run_boot(int stop_at)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        if (!nvs_sim_set_backing_file(g_flash)) {
            _exit(91);
        }
        pb_enroll_ctl_t c;
        pb_enroll_ctl_init(&c, &k_ops, EUI_A);
        pb_enroll_state_t st = pb_enroll_ctl_boot(&c);
        if (stop_at == 0) {
            _exit(10 + (int)st);
        }
        char buf[PB_ENROLL_PSKD_BUF_LEN];
        fill_pskd(buf, PSKD);
        if (pb_enroll_ctl_request_join(&c, buf, 0) != ESP_OK) {
            _exit(92);
        }
        if (stop_at == STOP_AFTER_JOIN_STARTED) {
            _exit(10 + (int)c.state); /* power lost mid-Joiner */
        }
        ot_store_joiner_partial();
        pb_enroll_ctl_on_joiner_done(&c, g_ot.last_attempt, PB_ENROLL_JOINER_OK, 10);
        if (stop_at == STOP_AFTER_JOINER_OK) {
            _exit(10 + (int)c.state);
        }
        g_ot.complete = true;
        g_ot.id = NET_OTBR;
        if (stop_at == STOP_AFTER_ATTACH_BEFORE_MARKER) {
            _exit(10 + (int)c.state); /* attached, Dataset complete, tick not yet run */
        }
        pb_enroll_ctl_tick(&c, true, 20);
        _exit(10 + (int)c.state);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 99;
}

static void flash_reset(void)
{
    unlink(g_flash);
}

static void scenario_reboots(void)
{
    printf("\n-- reboots and power loss at each step --\n");
    snprintf(g_flash, sizeof(g_flash), "/tmp/pb_enroll_flash_%d", (int)getpid());
    const int WAIT = 10 + PB_ENROLL_STATE_WAITING;
    const int ENROLLED = 10 + PB_ENROLL_STATE_ENROLLED;

    /* Power lost after Joiner success, before attach: OpenThread kept the
     * partial Dataset, no marker. */
    flash_reset();
    ot_reset();
    CHECK(run_boot(STOP_AFTER_JOINER_OK) == 10 + PB_ENROLL_STATE_ATTACHING, "boot 1: Joiner succeeded, then power lost");
    ot_reset();
    ot_store_joiner_partial();
    CHECK(run_boot(0) == WAIT, "boot 2: partial Dataset, no marker -> safe wait");

    /* Power lost after attach with a complete Dataset, before the marker. */
    flash_reset();
    ot_reset();
    CHECK(run_boot(STOP_AFTER_ATTACH_BEFORE_MARKER) == 10 + PB_ENROLL_STATE_ATTACHING,
          "boot 1: attached with a complete Dataset, power lost before the marker");
    ot_reset();
    ot_store_complete(&NET_OTBR);
    CHECK(run_boot(0) == WAIT, "boot 2: complete Dataset but no marker -> safe wait (never guessed as enrolled)");

    /* Marker committed: every later boot resumes. */
    flash_reset();
    ot_reset();
    CHECK(run_boot(STOP_AFTER_MARKER) == ENROLLED, "boot 1: enrolled (marker committed)");
    ot_reset();
    ot_store_complete(&NET_OTBR);
    CHECK(run_boot(0) == ENROLLED, "boot 2: marker + matching Dataset -> rejoins automatically");
    CHECK(run_boot(0) == ENROLLED, "boot 3: still rejoins (marker is not consumed)");

    /* Enrolled board whose OpenThread settings were wiped. */
    ot_reset();
    CHECK(run_boot(0) == WAIT, "boot 4: marker but no Dataset -> safe wait");

    /* A board waiting with its old marker (Dataset mismatch) starts a new
     * attempt and loses power mid-Joiner: the old marker is already gone, so
     * even the old network's Dataset coming back is not resumed. */
    ot_reset();
    ot_store_complete(&NET_STANDALONE);
    CHECK(run_boot(STOP_AFTER_JOIN_STARTED) == 10 + PB_ENROLL_STATE_JOINING,
          "boot 5: waiting (dataset_mismatch), new Joiner started, power lost");
    ot_reset();
    ot_store_complete(&NET_OTBR);
    CHECK(run_boot(0) == WAIT, "boot 6: the old marker was erased before the Joiner -> safe wait");

    flash_reset();
}


/* ==== the delivered bridge endpoint and the version 2 record ==== */

/* A Mesh-Local EID inside NET_OTBR's prefix. Made up. */
static const pb_enroll_bridge_endpoint_t EP_OTBR = {
    .addr = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00, 0x8c, 0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79},
    .port = 5684,
};
/* Another host in the same network. */
static const pb_enroll_bridge_endpoint_t EP_OTBR_2 = {
    .addr = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08},
    .port = 6001,
};

static bool ep_equal(const pb_enroll_bridge_endpoint_t *x, const pb_enroll_bridge_endpoint_t *y)
{
    return memcmp(x->addr, y->addr, sizeof(x->addr)) == 0 && x->port == y->port;
}

static uint32_t test_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static void put_crc(uint8_t *raw, size_t crc_offset)
{
    uint32_t crc = test_crc32(raw, crc_offset);
    raw[crc_offset] = (uint8_t)crc;
    raw[crc_offset + 1] = (uint8_t)(crc >> 8);
    raw[crc_offset + 2] = (uint8_t)(crc >> 16);
    raw[crc_offset + 3] = (uint8_t)(crc >> 24);
}

/* The version 2 layout, written out by hand from the documented offsets --
 * independent of the encoder under test. */
static void make_v2_raw(uint8_t raw[PB_ENROLL_MARKER_V2_LEN], const uint8_t eui[8], const pb_enroll_network_id_t *id,
                        const pb_enroll_bridge_endpoint_t *ep)
{
    memset(raw, 0, PB_ENROLL_MARKER_V2_LEN);
    memcpy(raw, "PBE1", 4);
    raw[4] = 2;
    raw[5] = 0x01;
    memcpy(&raw[8], eui, 8);
    memcpy(&raw[16], id->ext_pan_id, 8);
    memcpy(&raw[24], id->mesh_local_prefix, 8);
    memcpy(&raw[32], ep->addr, 16);
    raw[48] = (uint8_t)(ep->port >> 8);
    raw[49] = (uint8_t)ep->port;
    put_crc(raw, 52);
}

static size_t stored_blob(uint8_t *out, size_t cap)
{
    nvs_handle_t h;
    size_t len = cap;
    if (nvs_open(PB_ENROLL_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    esp_err_t err = nvs_get_blob(h, PB_ENROLL_NVS_KEY, out, &len);
    nvs_close(h);
    return err == ESP_OK ? len : 0;
}

static void put_raw(const uint8_t *raw, size_t len)
{
    nvs_sim_put_blob("nvs", PB_ENROLL_NVS_NAMESPACE, PB_ENROLL_NVS_KEY, raw, len);
}

/* Boots a board that holds `raw` as its record and NET_OTBR's Dataset. */
static pb_enroll_state_t boot_with_raw(pb_enroll_ctl_t *c, const uint8_t *raw, size_t len)
{
    nvs_sim_reset();
    ot_reset();
    ot_store_complete(&NET_OTBR);
    put_raw(raw, len);
    pb_enroll_ctl_init(c, &k_ops, EUI_A);
    return pb_enroll_ctl_boot(c);
}

static void join_ok_with_endpoint(pb_enroll_ctl_t *c, const pb_enroll_bridge_endpoint_t *ep, uint32_t now)
{
    char buf[PB_ENROLL_PSKD_BUF_LEN];
    fill_pskd(buf, PSKD);
    pb_enroll_ctl_request_join_with_endpoint(c, buf, ep, now);
    ot_store_joiner_partial();
    pb_enroll_ctl_on_joiner_done(c, g_ot.last_attempt, PB_ENROLL_JOINER_OK, now + 5000);
}

static void scenario_endpoint_check(void)
{
    printf("\n-- what a delivered bridge endpoint must be: a Mesh-Local EID and a port --\n");
    CHECK(pb_enroll_bridge_endpoint_check(&EP_OTBR) == PB_ENROLL_ENDPOINT_OK, "fd…: an EID with a port is accepted");
    static const struct {
        uint8_t addr[16];
        uint16_t port;
        pb_enroll_endpoint_check_t expect;
        const char *what;
    } cases[] = {
        {{0}, 5684, PB_ENROLL_ENDPOINT_UNSPECIFIED, "unspecified (::)"},
        {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 5684, PB_ENROLL_ENDPOINT_LOOPBACK, "loopback (::1)"},
        {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 10, 0, 0, 1}, 5684, PB_ENROLL_ENDPOINT_IPV4_EMBEDDED,
         "IPv4-mapped (::ffff:10.0.0.1)"},
        {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10, 0, 0, 1}, 5684, PB_ENROLL_ENDPOINT_IPV4_EMBEDDED,
         "IPv4-compatible (::10.0.0.1)"},
        {{0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 5684, PB_ENROLL_ENDPOINT_MULTICAST, "multicast"},
        {{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8}, 5684, PB_ENROLL_ENDPOINT_LINK_LOCAL, "link-local"},
        {{0xfe, 0xbf, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8}, 5684, PB_ENROLL_ENDPOINT_LINK_LOCAL,
         "link-local (top of fe80::/10)"},
        {{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8}, 5684, PB_ENROLL_ENDPOINT_NOT_MESH_LOCAL,
         "a global address"},
        {{0xfc, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8}, 5684, PB_ENROLL_ENDPOINT_NOT_MESH_LOCAL,
         "fc00::/8"},
        {{0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 0, 0, 0, 0xff, 0xfe, 0, 0x4c, 0}, 5684, PB_ENROLL_ENDPOINT_LOCATOR,
         "an RLOC"},
        {{0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 0, 0, 0, 0xff, 0xfe, 0, 0xfc, 0x38}, 5684,
         PB_ENROLL_ENDPOINT_LOCATOR, "an ALOC"},
        {{0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 0x8c, 0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79}, 0,
         PB_ENROLL_ENDPOINT_PORT_ZERO, "port 0"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        pb_enroll_bridge_endpoint_t ep = {.port = cases[i].port};
        memcpy(ep.addr, cases[i].addr, 16);
        printf("      (%s)\n", cases[i].what);
        CHECK(pb_enroll_bridge_endpoint_check(&ep) == cases[i].expect, "refused with its own reason");

        /* And the controller refuses to start anything with it. */
        pb_enroll_ctl_t c;
        fresh_waiting(&c);
        put_valid_marker(EUI_A, &NET_OTBR); /* an old marker a refused request must not erase */
        char buf[PB_ENROLL_PSKD_BUF_LEN];
        fill_pskd(buf, PSKD);
        CHECK(pb_enroll_ctl_request_join_with_endpoint(&c, buf, &ep, 0) == ESP_ERR_INVALID_ARG &&
                  all_zero(buf, sizeof(buf)) && g_ot.joiner_start_calls == 0 && c.state == PB_ENROLL_STATE_WAITING &&
                  !c.pending_endpoint && marker_is(PB_ENROLL_MARKER_VALID),
              "request refused: PSKd wiped, no Joiner, nothing erased");
    }
    pb_enroll_ctl_t c;
    fresh_waiting(&c);
    char buf[PB_ENROLL_PSKD_BUF_LEN];
    fill_pskd(buf, PSKD);
    CHECK(pb_enroll_ctl_request_join_with_endpoint(&c, buf, NULL, 0) == ESP_ERR_INVALID_ARG &&
              all_zero(buf, sizeof(buf)) && g_ot.joiner_start_calls == 0,
          "no endpoint at all: refused (the plain request is the one without)");

    CHECK(pb_enroll_bridge_endpoint_in_network(&EP_OTBR, &NET_OTBR) &&
              !pb_enroll_bridge_endpoint_in_network(&EP_OTBR, &NET_STANDALONE),
          "an endpoint belongs to the network whose Mesh-Local Prefix it starts with");
}

static void scenario_record_versions(void)
{
    printf("\n-- record versions: 1 unchanged, 2 carries the endpoint under the same CRC --\n");
    uint8_t raw[80];

    /* Version 1 is byte for byte what it was. */
    nvs_sim_reset();
    CHECK(pb_enroll_marker_store(EUI_A, &NET_OTBR) == ESP_OK && stored_blob(raw, sizeof(raw)) == 36,
          "a record without an endpoint is 36 bytes");
    uint8_t v1[36] = {'P', 'B', 'E', '1', 1, 0, 0, 0};
    memcpy(&v1[8], EUI_A, 8);
    memcpy(&v1[16], NET_OTBR.ext_pan_id, 8);
    memcpy(&v1[24], NET_OTBR.mesh_local_prefix, 8);
    put_crc(v1, 32);
    CHECK(memcmp(raw, v1, 36) == 0, "with exactly the version 1 bytes (magic, version 1, EUI-64, network, CRC-32)");
    pb_enroll_record_t rec;
    CHECK(pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_VALID && !rec.has_bridge_endpoint &&
              all_zero(&rec.bridge_endpoint, sizeof(rec.bridge_endpoint)),
          "it loads as a record without an endpoint");

    /* Version 2. */
    nvs_sim_reset();
    memset(&rec, 0, sizeof(rec));
    rec.id = NET_OTBR;
    rec.has_bridge_endpoint = true;
    rec.bridge_endpoint = EP_OTBR;
    CHECK(pb_enroll_record_store(EUI_A, &rec) == ESP_OK && stored_blob(raw, sizeof(raw)) == 56,
          "a record with an endpoint is ONE 56-byte blob");
    uint8_t v2[PB_ENROLL_MARKER_V2_LEN];
    make_v2_raw(v2, EUI_A, &NET_OTBR, &EP_OTBR);
    CHECK(memcmp(raw, v2, 56) == 0, "with the documented layout (version 2, flags 01, address at 32, port at 48 big-endian)");
    CHECK(raw[48] == 0x16 && raw[49] == 0x34, "port 5684 is stored as 16 34");
    pb_enroll_record_t back;
    CHECK(pb_enroll_record_load(EUI_A, &back) == PB_ENROLL_MARKER_VALID && back.has_bridge_endpoint &&
              ep_equal(&back.bridge_endpoint, &EP_OTBR) && memcmp(&back.id, &NET_OTBR, sizeof(back.id)) == 0,
          "it loads back with the endpoint and the network");
    pb_enroll_network_id_t id;
    CHECK(pb_enroll_marker_load(EUI_A, &id) == PB_ENROLL_MARKER_VALID && memcmp(&id, &NET_OTBR, sizeof(id)) == 0,
          "the marker part reads the same from either version");

    /* What must never be written. */
    nvs_sim_reset();
    rec.bridge_endpoint.port = 0;
    CHECK(pb_enroll_record_store(EUI_A, &rec) == ESP_ERR_INVALID_ARG && marker_is(PB_ENROLL_MARKER_ABSENT),
          "an endpoint with port 0 is not stored");
    rec.bridge_endpoint = EP_OTBR;
    rec.id = NET_STANDALONE;
    CHECK(pb_enroll_record_store(EUI_A, &rec) == ESP_ERR_INVALID_ARG && marker_is(PB_ENROLL_MARKER_ABSENT),
          "an endpoint outside the record's own Mesh-Local Prefix is not stored");
}

static void scenario_endpoint_attempts(void)
{
    printf("\n-- the endpoint is pending in RAM until enrolled; every other ending drops it --\n");
    pb_enroll_ctl_t c;
    char buf[PB_ENROLL_PSKD_BUF_LEN];
    pb_enroll_record_t rec;

    fresh_waiting(&c);
    fill_pskd(buf, PSKD);
    CHECK(pb_enroll_ctl_request_join_with_endpoint(&c, buf, &EP_OTBR, 0) == ESP_OK && c.state == PB_ENROLL_STATE_JOINING,
          "Joiner started for an attempt with an endpoint");
    CHECK(c.pending_endpoint && ep_equal(&c.pending_ep, &EP_OTBR) && !c.has_bridge_endpoint &&
              marker_is(PB_ENROLL_MARKER_ABSENT),
          "joining: the endpoint is pending in RAM, nothing is stored");
    CHECK(pb_enroll_ctl_cancel(&c) == ESP_OK && !c.pending_endpoint && all_zero(&c.pending_ep, sizeof(c.pending_ep)) &&
              marker_is(PB_ENROLL_MARKER_ABSENT),
          "cancel while joining: the pending endpoint is dropped");

    static const pb_enroll_joiner_outcome_t failures[] = {PB_ENROLL_JOINER_ERR_SECURITY, PB_ENROLL_JOINER_ERR_NOT_FOUND,
                                                          PB_ENROLL_JOINER_ERR_TIMEOUT, PB_ENROLL_JOINER_ERR_OTHER};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        fresh_waiting(&c);
        fill_pskd(buf, PSKD);
        pb_enroll_ctl_request_join_with_endpoint(&c, buf, &EP_OTBR, 0);
        pb_enroll_ctl_on_joiner_done(&c, g_ot.last_attempt, failures[i], 100);
        CHECK(c.state == PB_ENROLL_STATE_WAITING && !c.pending_endpoint && marker_is(PB_ENROLL_MARKER_ABSENT) &&
                  !pb_enroll_ctl_services_allowed(&c),
              "Joiner failure: pending endpoint dropped, nothing stored, no services");
    }

    fresh_waiting(&c);
    fill_pskd(buf, PSKD);
    pb_enroll_ctl_request_join_with_endpoint(&c, buf, &EP_OTBR, 1000);
    pb_enroll_ctl_tick(&c, false, 1000 + PB_ENROLL_JOINER_GUARD_MS);
    CHECK(c.last == PB_ENROLL_RESULT_JOINER_GUARD_EXPIRED && !c.pending_endpoint,
          "Joiner guard expired: pending endpoint dropped");

    fresh_waiting(&c);
    join_ok_with_endpoint(&c, &EP_OTBR, 1000);
    CHECK(c.state == PB_ENROLL_STATE_ATTACHING && c.pending_endpoint && marker_is(PB_ENROLL_MARKER_ABSENT),
          "attaching: still pending in RAM only");
    CHECK(pb_enroll_ctl_cancel(&c) == ESP_OK && !c.pending_endpoint && marker_is(PB_ENROLL_MARKER_ABSENT) &&
              !g_ot.thread_enabled,
          "cancel while attaching: dropped, Thread off, nothing stored");

    fresh_waiting(&c);
    join_ok_with_endpoint(&c, &EP_OTBR, 1000);
    pb_enroll_ctl_tick(&c, false, 1000 + 5000 + PB_ENROLL_ATTACH_DEADLINE_MS);
    CHECK(c.last == PB_ENROLL_RESULT_ATTACH_TIMEOUT && !c.pending_endpoint && marker_is(PB_ENROLL_MARKER_ABSENT),
          "attach timeout: dropped, nothing stored");

    /* Joiner success but Thread cannot be enabled. */
    fresh_waiting(&c);
    fill_pskd(buf, PSKD);
    pb_enroll_ctl_request_join_with_endpoint(&c, buf, &EP_OTBR, 0);
    pb_enroll_ctl_on_joiner_done(&c, g_ot.last_attempt, PB_ENROLL_JOINER_OK, 100);
    CHECK(c.last == PB_ENROLL_RESULT_THREAD_START_FAILED && !c.pending_endpoint, "Thread start failed: dropped");

    /* A replaced attempt: the second attempt's endpoint is the one stored. */
    fresh_waiting(&c);
    fill_pskd(buf, PSKD);
    pb_enroll_ctl_request_join_with_endpoint(&c, buf, &EP_OTBR, 0);
    pb_enroll_ctl_cancel(&c);
    join_ok_with_endpoint(&c, &EP_OTBR_2, 100);
    ot_store_complete(&NET_OTBR);
    pb_enroll_ctl_tick(&c, true, 9000);
    CHECK(c.state == PB_ENROLL_STATE_ENROLLED && c.has_bridge_endpoint && ep_equal(&c.bridge_endpoint, &EP_OTBR_2) &&
              pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_VALID && ep_equal(&rec.bridge_endpoint, &EP_OTBR_2),
          "after a cancelled attempt, the new attempt's endpoint is the one enrolled and stored");

    /* A replaced attempt without an endpoint stores none. */
    fresh_waiting(&c);
    fill_pskd(buf, PSKD);
    pb_enroll_ctl_request_join_with_endpoint(&c, buf, &EP_OTBR, 0);
    pb_enroll_ctl_on_joiner_done(&c, g_ot.last_attempt, PB_ENROLL_JOINER_ERR_TIMEOUT, 10);
    join_ok(&c, 100);
    ot_store_complete(&NET_OTBR);
    pb_enroll_ctl_tick(&c, true, 9000);
    uint8_t raw[80];
    CHECK(c.state == PB_ENROLL_STATE_ENROLLED && !c.has_bridge_endpoint && stored_blob(raw, sizeof(raw)) == 36,
          "a later attempt WITHOUT an endpoint enrolls with a version 1 record: the old endpoint is not reused");

    /* Enrollment with the endpoint. */
    fresh_waiting(&c);
    join_ok_with_endpoint(&c, &EP_OTBR, 1000);
    pb_enroll_ctl_tick(&c, true, 8000);
    CHECK(c.state == PB_ENROLL_STATE_ATTACHING && marker_is(PB_ENROLL_MARKER_ABSENT),
          "attached, Dataset still partial: nothing stored");
    ot_store_complete(&NET_OTBR);
    pb_enroll_ctl_tick(&c, true, 9000);
    CHECK(c.state == PB_ENROLL_STATE_ENROLLED && c.last == PB_ENROLL_RESULT_ENROLLED && c.has_bridge_endpoint &&
              ep_equal(&c.bridge_endpoint, &EP_OTBR) && !c.pending_endpoint,
          "attached + complete Dataset of the endpoint's network: enrolled with it");
    CHECK(pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_VALID && rec.has_bridge_endpoint &&
              ep_equal(&rec.bridge_endpoint, &EP_OTBR) && stored_blob(raw, sizeof(raw)) == 56,
          "one 56-byte record holds the marker and the endpoint");

    /* The joined network is not the endpoint's. */
    fresh_waiting(&c);
    join_ok_with_endpoint(&c, &EP_OTBR, 1000);
    ot_store_complete(&NET_STANDALONE);
    pb_enroll_ctl_tick(&c, true, 9000);
    CHECK(c.state == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_DATASET_MISMATCH && !g_ot.thread_enabled &&
              !pb_enroll_ctl_services_allowed(&c),
          "joined a network the endpoint is not in: not enrolled, Thread stopped (dataset_mismatch)");
    CHECK(marker_is(PB_ENROLL_MARKER_ABSENT) && !c.pending_endpoint && !c.has_bridge_endpoint,
          "no record at all: neither a marker nor an endpoint");

    /* The record cannot be committed. */
    fresh_waiting(&c);
    join_ok_with_endpoint(&c, &EP_OTBR, 1000);
    ot_store_complete(&NET_OTBR);
    nvs_sim_force_next_commit_error(ESP_FAIL);
    pb_enroll_ctl_tick(&c, true, 9000);
    CHECK(c.state == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_MARKER_WRITE_FAILED && !g_ot.thread_enabled &&
              !pb_enroll_ctl_services_allowed(&c) && !c.has_bridge_endpoint,
          "storing failed: not enrolled, no services, no endpoint in use");
    CHECK(marker_is(PB_ENROLL_MARKER_ABSENT), "and no half-written record is left");
}

static void scenario_endpoint_boot(void)
{
    printf("\n-- boot: the endpoint is restored only from an intact record of this board and this network --\n");
    pb_enroll_ctl_t c;
    uint8_t raw[PB_ENROLL_MARKER_V2_LEN];

    make_v2_raw(raw, EUI_A, &NET_OTBR, &EP_OTBR);
    CHECK(boot_with_raw(&c, raw, sizeof(raw)) == PB_ENROLL_STATE_ENROLLED && c.resumed && c.has_bridge_endpoint &&
              ep_equal(&c.bridge_endpoint, &EP_OTBR),
          "intact version 2 record + matching Dataset: resumes with the stored endpoint");
    CHECK(g_ot.thread_start_calls == 1 && g_ot.joiner_start_calls == 0, "Thread enabled once, no Joiner");

    /* A version 1 record still resumes, without any endpoint. */
    nvs_sim_reset();
    ot_reset();
    ot_store_complete(&NET_OTBR);
    put_valid_marker(EUI_A, &NET_OTBR);
    pb_enroll_ctl_init(&c, &k_ops, EUI_A);
    CHECK(pb_enroll_ctl_boot(&c) == PB_ENROLL_STATE_ENROLLED && !c.has_bridge_endpoint &&
              all_zero(&c.bridge_endpoint, sizeof(c.bridge_endpoint)),
          "version 1 record: resumes as before, with no delivered endpoint");

    /* Every single flipped bit of a version 2 record. */
    int resumed = 0, with_endpoint = 0;
    for (size_t bit = 0; bit < PB_ENROLL_MARKER_V2_LEN * 8; bit++) {
        make_v2_raw(raw, EUI_A, &NET_OTBR, &EP_OTBR);
        raw[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        if (boot_with_raw(&c, raw, sizeof(raw)) != PB_ENROLL_STATE_WAITING || g_ot.thread_start_calls != 0) {
            resumed++;
        }
        if (c.has_bridge_endpoint) {
            with_endpoint++;
        }
    }
    CHECK(resumed == 0 && with_endpoint == 0,
          "any one flipped bit (448 cases: header, EUI-64, network, address, port, CRC): waits, no endpoint");

    /* Truncated / extended: a write cut short, or another format. */
    make_v2_raw(raw, EUI_A, &NET_OTBR, &EP_OTBR);
    int bad_sizes = 0;
    for (size_t len = 1; len <= 70; len++) {
        if (len == 56) {
            continue;
        }
        uint8_t blob[70] = {0};
        memcpy(blob, raw, len < sizeof(raw) ? len : sizeof(raw));
        if (boot_with_raw(&c, blob, len) != PB_ENROLL_STATE_WAITING || c.has_bridge_endpoint ||
            g_ot.thread_start_calls != 0) {
            bad_sizes++;
        }
    }
    CHECK(bad_sizes == 0, "the record cut short or padded to any other size (1..70 bytes): waits, no endpoint");
    CHECK(boot_with_raw(&c, raw, 36) == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_MARKER_CORRUPT,
          "its first 36 bytes are not a version 1 record either (marker_corrupt)");

    /* Intact (CRC correct) records this firmware would never have written. */
    make_v2_raw(raw, EUI_A, &NET_OTBR, &EP_OTBR);
    raw[4] = 1; /* version 1 in a 56-byte blob */
    put_crc(raw, 52);
    CHECK(boot_with_raw(&c, raw, sizeof(raw)) == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_MARKER_CORRUPT,
          "56 bytes claiming version 1: waits (marker_corrupt)");
    make_v2_raw(raw, EUI_A, &NET_OTBR, &EP_OTBR);
    raw[4] = 3;
    put_crc(raw, 52);
    CHECK(boot_with_raw(&c, raw, sizeof(raw)) == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_MARKER_CORRUPT,
          "an unknown record version: waits (marker_corrupt)");
    make_v2_raw(raw, EUI_A, &NET_OTBR, &EP_OTBR);
    raw[5] = 0x00; /* "no endpoint" in a version 2 record */
    put_crc(raw, 52);
    CHECK(boot_with_raw(&c, raw, sizeof(raw)) == PB_ENROLL_STATE_WAITING && !c.has_bridge_endpoint,
          "version 2 without the endpoint flag: waits");
    make_v2_raw(raw, EUI_A, &NET_OTBR, &EP_OTBR);
    raw[5] = 0x03;
    put_crc(raw, 52);
    CHECK(boot_with_raw(&c, raw, sizeof(raw)) == PB_ENROLL_STATE_WAITING, "version 2 with an unknown flag: waits");
    make_v2_raw(raw, EUI_A, &NET_OTBR, &EP_OTBR);
    raw[50] = 1;
    put_crc(raw, 52);
    CHECK(boot_with_raw(&c, raw, sizeof(raw)) == PB_ENROLL_STATE_WAITING, "non-zero reserved bytes: waits");

    pb_enroll_bridge_endpoint_t ep = EP_OTBR;
    ep.port = 0;
    make_v2_raw(raw, EUI_A, &NET_OTBR, &ep);
    CHECK(boot_with_raw(&c, raw, sizeof(raw)) == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_MARKER_CORRUPT &&
              !c.has_bridge_endpoint && g_ot.thread_start_calls == 0,
          "intact record with port 0: waits (marker_corrupt), Thread never enabled");
    ep = EP_OTBR;
    memset(&ep.addr[8], 0, 8);
    ep.addr[11] = 0xff;
    ep.addr[12] = 0xfe;
    ep.addr[14] = 0x4c;
    make_v2_raw(raw, EUI_A, &NET_OTBR, &ep);
    CHECK(boot_with_raw(&c, raw, sizeof(raw)) == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_MARKER_CORRUPT,
          "intact record whose address is an RLOC: waits (marker_corrupt)");
    ep = EP_OTBR;
    ep.addr[0] = 0xfe;
    ep.addr[1] = 0x80;
    make_v2_raw(raw, EUI_A, &NET_OTBR, &ep);
    CHECK(boot_with_raw(&c, raw, sizeof(raw)) == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_MARKER_CORRUPT,
          "intact record whose address is link-local: waits (marker_corrupt)");
    ep = EP_OTBR;
    ep.addr[3] ^= 0x10; /* another network's prefix; the record's network is still NET_OTBR */
    make_v2_raw(raw, EUI_A, &NET_OTBR, &ep);
    CHECK(boot_with_raw(&c, raw, sizeof(raw)) == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_MARKER_CORRUPT &&
              !c.has_bridge_endpoint,
          "intact record whose address is outside its own network's prefix: waits (marker_corrupt)");

    /* Another board's record (e.g. a copied NVS image). */
    make_v2_raw(raw, EUI_B, &NET_OTBR, &EP_OTBR);
    CHECK(boot_with_raw(&c, raw, sizeof(raw)) == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_MARKER_FOREIGN &&
              !c.has_bridge_endpoint && g_ot.thread_start_calls == 0,
          "version 2 record of another EUI-64: waits (marker_foreign), its endpoint is not used");

    /* The record's network is not the stored Dataset's. */
    static const pb_enroll_network_id_t NET_OTHER = {
        .ext_pan_id = {0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22},
        .mesh_local_prefix = {0xfd, 0x99, 0x88, 0x77, 0x66, 0x55, 0x00, 0x00},
    };
    pb_enroll_bridge_endpoint_t ep_other = EP_OTBR;
    memcpy(ep_other.addr, NET_OTHER.mesh_local_prefix, 8);
    make_v2_raw(raw, EUI_A, &NET_OTHER, &ep_other);
    CHECK(boot_with_raw(&c, raw, sizeof(raw)) == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_DATASET_MISMATCH &&
              !c.has_bridge_endpoint && g_ot.thread_start_calls == 0,
          "valid record for another network than the stored Dataset: waits (dataset_mismatch), no endpoint");
    /* Same Extended PAN ID, other Mesh-Local Prefix. */
    pb_enroll_network_id_t same_xpan = NET_OTBR;
    memcpy(same_xpan.mesh_local_prefix, NET_OTHER.mesh_local_prefix, 8);
    make_v2_raw(raw, EUI_A, &same_xpan, &ep_other);
    CHECK(boot_with_raw(&c, raw, sizeof(raw)) == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_DATASET_MISMATCH &&
              !c.has_bridge_endpoint,
          "same Extended PAN ID but another Mesh-Local Prefix: waits (dataset_mismatch)");

    /* The Dataset is gone. */
    make_v2_raw(raw, EUI_A, &NET_OTBR, &EP_OTBR);
    nvs_sim_reset();
    ot_reset();
    put_raw(raw, sizeof(raw));
    pb_enroll_ctl_init(&c, &k_ops, EUI_A);
    CHECK(pb_enroll_ctl_boot(&c) == PB_ENROLL_STATE_WAITING && c.last == PB_ENROLL_RESULT_DATASET_MISSING &&
              !c.has_bridge_endpoint,
          "valid version 2 record but no Dataset: waits (dataset_missing), no endpoint");

    /* A board that waits with a damaged record can be enrolled again. */
    make_v2_raw(raw, EUI_A, &NET_OTBR, &EP_OTBR);
    raw[40] ^= 0x01;
    boot_with_raw(&c, raw, sizeof(raw));
    g_ot.has_key = false;
    g_ot.complete = false;
    join_ok_with_endpoint(&c, &EP_OTBR_2, 100);
    CHECK(g_ot.marker_at_joiner_start == PB_ENROLL_MARKER_ABSENT, "a new attempt first removes the damaged record");
    ot_store_complete(&NET_OTBR);
    pb_enroll_ctl_tick(&c, true, 9000);
    pb_enroll_record_t rec;
    CHECK(c.state == PB_ENROLL_STATE_ENROLLED && pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_VALID &&
              ep_equal(&rec.bridge_endpoint, &EP_OTBR_2),
          "and enrolls with the newly delivered endpoint");
}

static void scenario_endpoint_never_logged(void)
{
    printf("\n-- the bridge address never reaches the log output --\n");
    char path[] = "/tmp/pb_enroll_ep_logXXXXXX";
    int fd = mkstemp(path);
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    dup2(fd, STDERR_FILENO);

    pb_enroll_ctl_t c;
    fresh_waiting(&c);
    join_ok_with_endpoint(&c, &EP_OTBR, 100);
    ot_store_complete(&NET_STANDALONE);
    pb_enroll_ctl_tick(&c, true, 200); /* mismatch */
    join_ok_with_endpoint(&c, &EP_OTBR, 300);
    ot_store_complete(&NET_OTBR);
    pb_enroll_ctl_tick(&c, true, 400); /* enrolled */
    pb_enroll_ctl_init(&c, &k_ops, EUI_A);
    pb_enroll_ctl_boot(&c); /* resume */
    uint8_t raw[PB_ENROLL_MARKER_V2_LEN];
    pb_enroll_bridge_endpoint_t ep = EP_OTBR;
    ep.addr[0] = 0xfe;
    ep.addr[1] = 0x80;
    make_v2_raw(raw, EUI_A, &NET_OTBR, &ep);
    boot_with_raw(&c, raw, sizeof(raw)); /* rejected at boot */

    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    char log[16384] = {0};
    lseek(fd, 0, SEEK_SET);
    ssize_t n = read(fd, log, sizeof(log) - 1);
    close(fd);
    unlink(path);
    CHECK(n > 0, "the scenario produced log output");
    CHECK(strstr(log, "8c1f") == NULL && strstr(log, "8c:1f") == NULL && strstr(log, "fd11") == NULL &&
              strstr(log, "6a79") == NULL && strstr(log, "2e3d") == NULL,
          "no part of the bridge address in the log");
}

/* ---- reboots / power loss with a delivered endpoint ---- */

enum {
    EP_STOP_AFTER_JOIN_STARTED = 1,
    EP_STOP_AFTER_JOINER_OK,
    EP_STOP_AFTER_ATTACH_BEFORE_RECORD,
    EP_STOP_AFTER_RECORD,
    EP_STOP_COMMIT_FAILS,
};

/* One boot in a child over the simulated flash file. Exit code:
 *   10 + state                       after `stop_at` (0 = boot only), no endpoint in use
 *   40 + state                       ... with EP_OTBR in use by the enrolled controller
 *   70 + state                       ... with some OTHER endpoint in use (always a failure)
 *   90+                              setup error */
static int run_boot_ep(int stop_at)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        if (!nvs_sim_set_backing_file(g_flash)) {
            _exit(91);
        }
        pb_enroll_ctl_t c;
        pb_enroll_ctl_init(&c, &k_ops, EUI_A);
        pb_enroll_ctl_boot(&c);
        if (stop_at != 0) {
            char buf[PB_ENROLL_PSKD_BUF_LEN];
            fill_pskd(buf, PSKD);
            if (pb_enroll_ctl_request_join_with_endpoint(&c, buf, &EP_OTBR, 0) != ESP_OK) {
                _exit(92);
            }
            if (stop_at != EP_STOP_AFTER_JOIN_STARTED) {
                ot_store_joiner_partial();
                pb_enroll_ctl_on_joiner_done(&c, g_ot.last_attempt, PB_ENROLL_JOINER_OK, 10);
            }
            if (stop_at >= EP_STOP_AFTER_ATTACH_BEFORE_RECORD) {
                g_ot.complete = true;
                g_ot.id = NET_OTBR;
            }
            if (stop_at == EP_STOP_COMMIT_FAILS) {
                nvs_sim_force_next_commit_error(ESP_FAIL);
            }
            if (stop_at >= EP_STOP_AFTER_RECORD) {
                pb_enroll_ctl_tick(&c, true, 20);
            }
        }
        int base = 10;
        if (c.has_bridge_endpoint) {
            base = ep_equal(&c.bridge_endpoint, &EP_OTBR) ? 40 : 70;
        }
        _exit(base + (int)c.state);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 99;
}

static void scenario_endpoint_reboots(void)
{
    printf("\n-- reboots and power loss with a delivered endpoint --\n");
    snprintf(g_flash, sizeof(g_flash), "/tmp/pb_enroll_ep_flash_%d", (int)getpid());
    const int WAIT = 10 + PB_ENROLL_STATE_WAITING;
    const int ENROLLED_WITH_EP = 40 + PB_ENROLL_STATE_ENROLLED;

    /* Power lost while joining: the endpoint was only in RAM. */
    flash_reset();
    ot_reset();
    CHECK(run_boot_ep(EP_STOP_AFTER_JOIN_STARTED) == 10 + PB_ENROLL_STATE_JOINING, "boot 1: joining, then power lost");
    ot_reset();
    CHECK(run_boot_ep(0) == WAIT, "boot 2: waits; the endpoint that was only in RAM is gone");

    /* Power lost after Joiner success. */
    flash_reset();
    ot_reset();
    CHECK(run_boot_ep(EP_STOP_AFTER_JOINER_OK) == 10 + PB_ENROLL_STATE_ATTACHING, "boot 1: Joiner succeeded, power lost");
    ot_reset();
    ot_store_joiner_partial();
    CHECK(run_boot_ep(0) == WAIT, "boot 2: partial Dataset, no record -> safe wait, no endpoint");

    /* Power lost after attach, before the record was written. */
    flash_reset();
    ot_reset();
    CHECK(run_boot_ep(EP_STOP_AFTER_ATTACH_BEFORE_RECORD) == 10 + PB_ENROLL_STATE_ATTACHING,
          "boot 1: attached with a complete Dataset, power lost before the record");
    ot_reset();
    ot_store_complete(&NET_OTBR);
    CHECK(run_boot_ep(0) == WAIT, "boot 2: complete Dataset but no record -> safe wait, no endpoint guessed");

    /* The commit failed. */
    flash_reset();
    ot_reset();
    CHECK(run_boot_ep(EP_STOP_COMMIT_FAILS) == WAIT, "boot 1: the record commit failed -> not enrolled");
    ot_reset();
    ot_store_complete(&NET_OTBR);
    CHECK(run_boot_ep(0) == WAIT, "boot 2: nothing usable in flash -> safe wait");

    /* Power lost in the middle of the record write: flash holds a prefix of
     * the blob. Every possible cut. */
    uint8_t raw[PB_ENROLL_MARKER_V2_LEN];
    make_v2_raw(raw, EUI_A, &NET_OTBR, &EP_OTBR);
    int wrong = 0;
    for (size_t cut = 1; cut < PB_ENROLL_MARKER_V2_LEN; cut++) {
        /* (a) a shorter blob  (b) full length, the tail still erased flash (0xff) */
        for (int variant = 0; variant < 2; variant++) {
            flash_reset();
            fflush(stdout);
            pid_t pid = fork();
            if (pid == 0) {
                if (!nvs_sim_set_backing_file(g_flash)) {
                    _exit(91);
                }
                uint8_t torn[PB_ENROLL_MARKER_V2_LEN];
                memset(torn, 0xff, sizeof(torn));
                memcpy(torn, raw, cut);
                nvs_handle_t h;
                nvs_open(PB_ENROLL_NVS_NAMESPACE, NVS_READWRITE, &h);
                nvs_set_blob(h, PB_ENROLL_NVS_KEY, torn, variant == 0 ? cut : sizeof(torn));
                nvs_commit(h);
                nvs_close(h);
                _exit(0);
            }
            int status = 0;
            waitpid(pid, &status, 0);
            ot_reset();
            ot_store_complete(&NET_OTBR);
            if (run_boot_ep(0) != WAIT) {
                wrong++;
            }
        }
    }
    CHECK(wrong == 0, "a record write cut at any of its 55 byte positions (short or 0xff-filled): next boot waits, "
                      "no endpoint, Thread off");

    /* Committed: every later boot restores the same endpoint. */
    flash_reset();
    ot_reset();
    CHECK(run_boot_ep(EP_STOP_AFTER_RECORD) == ENROLLED_WITH_EP, "boot 1: enrolled with the delivered endpoint");
    ot_reset();
    ot_store_complete(&NET_OTBR);
    CHECK(run_boot_ep(0) == ENROLLED_WITH_EP, "boot 2: resumes and restores exactly that endpoint");
    CHECK(run_boot_ep(0) == ENROLLED_WITH_EP, "boot 3: again (the record is not consumed)");

    /* The OpenThread settings now hold another network. */
    ot_reset();
    ot_store_complete(&NET_STANDALONE);
    CHECK(run_boot_ep(0) == WAIT, "boot 4: Dataset of another network -> safe wait, the endpoint is not used");

    /* A new attempt after that erases the old record before the Joiner. */
    CHECK(run_boot_ep(EP_STOP_AFTER_JOIN_STARTED) == 10 + PB_ENROLL_STATE_JOINING,
          "boot 5: new attempt started, power lost mid-Joiner");
    ot_reset();
    ot_store_complete(&NET_OTBR);
    CHECK(run_boot_ep(0) == WAIT, "boot 6: the old record (and its endpoint) was erased before the Joiner -> safe wait");

    flash_reset();
}


/* ==== endpoint policy: a board whose image requires a delivered endpoint ==== */

/* A waiting controller with `policy`, on a board that holds `raw` (may be
 * NULL) as its record and, if `dataset` is not NULL, that complete Dataset. */
static pb_enroll_state_t boot_with_policy(pb_enroll_ctl_t *c, pb_enroll_endpoint_policy_t policy, const uint8_t *raw,
                                          size_t raw_len, const pb_enroll_network_id_t *dataset)
{
    nvs_sim_reset();
    ot_reset();
    if (dataset != NULL) {
        ot_store_complete(dataset);
    }
    if (raw != NULL) {
        put_raw(raw, raw_len);
    }
    pb_enroll_ctl_init(c, &k_ops, EUI_A);
    pb_enroll_ctl_set_endpoint_policy(c, policy);
    return pb_enroll_ctl_boot(c);
}

static void make_v1_raw(uint8_t raw[PB_ENROLL_MARKER_LEN], const uint8_t eui[8], const pb_enroll_network_id_t *id)
{
    memset(raw, 0, PB_ENROLL_MARKER_LEN);
    memcpy(raw, "PBE1", 4);
    raw[4] = 1;
    memcpy(&raw[8], eui, 8);
    memcpy(&raw[16], id->ext_pan_id, 8);
    memcpy(&raw[24], id->mesh_local_prefix, 8);
    put_crc(raw, 32);
}

/* True when the stored record is exactly `raw` and the fake OpenThread still
 * holds exactly the Dataset `ds` -- i.e. nothing was erased or rewritten. */
static bool record_and_dataset_untouched(const uint8_t *raw, size_t raw_len, const fake_ot_t *before)
{
    uint8_t now[80];
    size_t n = stored_blob(now, sizeof(now));
    return n == raw_len && memcmp(now, raw, raw_len) == 0 && g_ot.has_key == before->has_key &&
           g_ot.complete == before->complete && memcmp(&g_ot.id, &before->id, sizeof(g_ot.id)) == 0;
}

static void scenario_endpoint_policy_requests(void)
{
    printf("\n-- endpoint policy REQUIRED: a request without a bridge endpoint is refused, with no side effect --\n");
    pb_enroll_ctl_t c;
    char buf[PB_ENROLL_PSKD_BUF_LEN];
    uint8_t v1[PB_ENROLL_MARKER_LEN];
    make_v1_raw(v1, EUI_A, &NET_OTBR);

    CHECK(strcmp(pb_enroll_endpoint_policy_name(PB_ENROLL_ENDPOINT_POLICY_OPTIONAL), "optional") == 0 &&
              strcmp(pb_enroll_endpoint_policy_name(PB_ENROLL_ENDPOINT_POLICY_REQUIRED), "required") == 0 &&
              strcmp(pb_enroll_endpoint_policy_name(PB_ENROLL_ENDPOINT_POLICY_INVALID), "invalid") == 0,
          "(policy names)");
    pb_enroll_ctl_init(&c, &k_ops, EUI_A);
    CHECK(c.endpoint_policy == PB_ENROLL_ENDPOINT_POLICY_OPTIONAL, "a controller starts with the policy OPTIONAL");

    /* An unenrolled board. */
    boot_with_policy(&c, PB_ENROLL_ENDPOINT_POLICY_REQUIRED, NULL, 0, NULL);
    pb_enroll_result_t last_before = c.last;
    fill_pskd(buf, PSKD);
    CHECK(pb_enroll_ctl_request_join(&c, buf, 0) == ESP_ERR_NOT_ALLOWED, "request without an endpoint: ESP_ERR_NOT_ALLOWED");
    CHECK(all_zero(buf, sizeof(buf)), "the PSKd buffer is wiped");
    CHECK(g_ot.joiner_start_calls == 0 && g_ot.thread_start_calls == 0 && g_ot.thread_stop_calls == 0 &&
              g_ot.joiner_stop_calls == 0,
          "no Joiner, and OpenThread was not touched at all");
    CHECK(c.state == PB_ENROLL_STATE_WAITING && c.last == last_before && c.attempt == 0 && !c.pending_endpoint,
          "state, last result and attempt counter are unchanged");
    CHECK(marker_is(PB_ENROLL_MARKER_ABSENT), "no record was written");

    /* A malformed PSKd is still told apart only when the request is allowed:
     * the policy answers first, and wipes the buffer all the same. */
    fill_pskd(buf, "j01nme7k");
    CHECK(pb_enroll_ctl_request_join(&c, buf, 0) == ESP_ERR_NOT_ALLOWED && all_zero(buf, sizeof(buf)),
          "a request without an endpoint is refused before its PSKd is even looked at");

    /* The refusal leaves the board fully usable for the right request. */
    fill_pskd(buf, PSKD);
    CHECK(pb_enroll_ctl_request_join_with_endpoint(&c, buf, &EP_OTBR, 100) == ESP_OK && g_ot.joiner_start_calls == 1 &&
              g_ot.pskd_matched && c.attempt == 1,
          "after the refusal a request WITH the endpoint starts the Joiner (attempt 1: the refusal used none)");
    ot_store_joiner_partial();
    pb_enroll_ctl_on_joiner_done(&c, g_ot.last_attempt, PB_ENROLL_JOINER_OK, 200);
    CHECK(marker_is(PB_ENROLL_MARKER_ABSENT), "attaching: nothing stored yet");
    ot_store_complete(&NET_OTBR);
    pb_enroll_ctl_tick(&c, true, 300);
    pb_enroll_record_t rec;
    uint8_t raw[80];
    uint8_t expect_v2[PB_ENROLL_MARKER_V2_LEN];
    make_v2_raw(expect_v2, EUI_A, &NET_OTBR, &EP_OTBR);
    CHECK(c.state == PB_ENROLL_STATE_ENROLLED && c.has_bridge_endpoint && ep_equal(&c.bridge_endpoint, &EP_OTBR),
          "it enrolls with the delivered endpoint");
    CHECK(pb_enroll_record_load(EUI_A, &rec) == PB_ENROLL_MARKER_VALID && rec.has_bridge_endpoint &&
              stored_blob(raw, sizeof(raw)) == PB_ENROLL_MARKER_V2_LEN && memcmp(raw, expect_v2, sizeof(expect_v2)) == 0,
          "marker and endpoint are stored together in ONE version 2 blob, as before");

    /* A board that waits while it still holds an old record and Dataset. */
    printf("\n-- ... also when the board holds an older record and Dataset: neither is erased --\n");
    boot_with_policy(&c, PB_ENROLL_ENDPOINT_POLICY_REQUIRED, v1, sizeof(v1), &NET_STANDALONE);
    CHECK(c.state == PB_ENROLL_STATE_WAITING, "(setup) waiting with a version 1 record and another network's Dataset");
    fake_ot_t before = g_ot;
    last_before = c.last;
    fill_pskd(buf, PSKD);
    CHECK(pb_enroll_ctl_request_join(&c, buf, 0) == ESP_ERR_NOT_ALLOWED && all_zero(buf, sizeof(buf)),
          "request without an endpoint: refused");
    CHECK(record_and_dataset_untouched(v1, sizeof(v1), &before) && g_ot.joiner_start_calls == 0 &&
              g_ot.thread_stop_calls == 0 && c.last == last_before,
          "the stored record and the stored Dataset are byte for byte what they were; no Joiner ran");
    fill_pskd(buf, PSKD);
    CHECK(pb_enroll_ctl_request_join_with_endpoint(&c, buf, &EP_OTBR, 0) == ESP_OK &&
              g_ot.marker_at_joiner_start == PB_ENROLL_MARKER_ABSENT,
          "the request with the endpoint is the one that clears the old record, before its Joiner runs");

    /* While an attempt runs. */
    fill_pskd(buf, PSKD);
    CHECK(pb_enroll_ctl_request_join(&c, buf, 10) == ESP_ERR_INVALID_STATE && all_zero(buf, sizeof(buf)) &&
              c.state == PB_ENROLL_STATE_JOINING && c.pending_endpoint && g_ot.joiner_start_calls == 1,
          "while joining, a second request is refused as before and the running attempt keeps its endpoint");

    printf("\n-- endpoint policy OPTIONAL (no policy in the image): requests work as they always did --\n");
    boot_with_policy(&c, PB_ENROLL_ENDPOINT_POLICY_OPTIONAL, NULL, 0, NULL);
    fill_pskd(buf, PSKD);
    CHECK(pb_enroll_ctl_request_join(&c, buf, 0) == ESP_OK && g_ot.joiner_start_calls == 1,
          "a request without an endpoint starts the Joiner");
    ot_store_joiner_partial();
    pb_enroll_ctl_on_joiner_done(&c, g_ot.last_attempt, PB_ENROLL_JOINER_OK, 10);
    ot_store_complete(&NET_OTBR);
    pb_enroll_ctl_tick(&c, true, 20);
    CHECK(c.state == PB_ENROLL_STATE_ENROLLED && !c.has_bridge_endpoint && stored_blob(raw, sizeof(raw)) == 36 &&
              memcmp(raw, v1, 36) == 0,
          "and enrolls with the same version 1 record as before");
    boot_with_policy(&c, PB_ENROLL_ENDPOINT_POLICY_OPTIONAL, NULL, 0, NULL);
    fill_pskd(buf, PSKD);
    CHECK(pb_enroll_ctl_request_join_with_endpoint(&c, buf, &EP_OTBR, 0) == ESP_OK,
          "a request with an endpoint works there too");

    printf("\n-- endpoint policy INVALID: no new enrollment at all --\n");
    boot_with_policy(&c, PB_ENROLL_ENDPOINT_POLICY_INVALID, v1, sizeof(v1), &NET_STANDALONE);
    before = g_ot;
    last_before = c.last;
    fill_pskd(buf, PSKD);
    CHECK(pb_enroll_ctl_request_join(&c, buf, 0) == ESP_ERR_NOT_ALLOWED && all_zero(buf, sizeof(buf)),
          "request without an endpoint: refused");
    fill_pskd(buf, PSKD);
    CHECK(pb_enroll_ctl_request_join_with_endpoint(&c, buf, &EP_OTBR, 0) == ESP_ERR_NOT_ALLOWED && all_zero(buf, sizeof(buf)),
          "request with an endpoint: refused too");
    CHECK(record_and_dataset_untouched(v1, sizeof(v1), &before) && g_ot.joiner_start_calls == 0 &&
              c.state == PB_ENROLL_STATE_WAITING && c.last == last_before && !c.pending_endpoint,
          "nothing started, nothing erased, nothing kept");

    /* The tick's own guard: no path writes a version 1 record under REQUIRED. */
    printf("\n-- (defence in depth) a policy that appears while an attempt without an endpoint runs --\n");
    boot_with_policy(&c, PB_ENROLL_ENDPOINT_POLICY_OPTIONAL, NULL, 0, NULL);
    join_ok(&c, 0);
    pb_enroll_ctl_set_endpoint_policy(&c, PB_ENROLL_ENDPOINT_POLICY_REQUIRED);
    ot_store_complete(&NET_OTBR);
    pb_enroll_ctl_tick(&c, true, 9000);
    CHECK(c.state == PB_ENROLL_STATE_WAITING && !pb_enroll_ctl_services_allowed(&c) && !g_ot.thread_enabled &&
              marker_is(PB_ENROLL_MARKER_ABSENT),
          "attached without an endpoint under REQUIRED: not enrolled, no version 1 record written, Thread stopped");
}

static void scenario_endpoint_policy_boot(void)
{
    printf("\n-- endpoint policy REQUIRED at boot: a record without an endpoint is not resumed --\n");
    pb_enroll_ctl_t c;
    uint8_t v1[PB_ENROLL_MARKER_LEN];
    uint8_t v2[PB_ENROLL_MARKER_V2_LEN];
    make_v1_raw(v1, EUI_A, &NET_OTBR);
    make_v2_raw(v2, EUI_A, &NET_OTBR, &EP_OTBR);

    CHECK(boot_with_policy(&c, PB_ENROLL_ENDPOINT_POLICY_REQUIRED, v1, sizeof(v1), &NET_OTBR) == PB_ENROLL_STATE_WAITING,
          "valid version 1 record + matching complete Dataset: safe wait");
    CHECK(c.last == PB_ENROLL_RESULT_MARKER_CORRUPT && !c.resumed && !c.has_bridge_endpoint &&
              !pb_enroll_ctl_services_allowed(&c),
          "reported with an existing result value (marker_corrupt); services not allowed");
    CHECK(g_ot.thread_start_calls == 0 && !g_ot.thread_enabled && g_ot.joiner_start_calls == 0 && g_ot.thread_stop_calls == 0,
          "Thread was never enabled and nothing else was called");
    fake_ot_t stored = g_ot;
    CHECK(record_and_dataset_untouched(v1, sizeof(v1), &stored) && marker_is(PB_ENROLL_MARKER_VALID),
          "booting erased nothing: the record is still there, still valid");
    /* Time passing changes nothing either. */
    pb_enroll_ctl_tick(&c, true, 500000);
    CHECK(c.state == PB_ENROLL_STATE_WAITING && g_ot.thread_start_calls == 0, "and ticks do not resume it later");

    CHECK(boot_with_policy(&c, PB_ENROLL_ENDPOINT_POLICY_REQUIRED, v2, sizeof(v2), &NET_OTBR) == PB_ENROLL_STATE_ENROLLED &&
              c.resumed && c.last == PB_ENROLL_RESULT_RESUMED && c.has_bridge_endpoint &&
              ep_equal(&c.bridge_endpoint, &EP_OTBR) && g_ot.thread_start_calls == 1,
          "valid version 2 record + matching Dataset: resumes with the stored endpoint, as before");

    /* The existing defences hold under the policy. */
    uint8_t bad[PB_ENROLL_MARKER_V2_LEN];
    memcpy(bad, v2, sizeof(bad));
    bad[40] ^= 0x04;
    CHECK(boot_with_policy(&c, PB_ENROLL_ENDPOINT_POLICY_REQUIRED, bad, sizeof(bad), &NET_OTBR) == PB_ENROLL_STATE_WAITING &&
              c.last == PB_ENROLL_RESULT_MARKER_CORRUPT && !c.has_bridge_endpoint && g_ot.thread_start_calls == 0,
          "damaged version 2 record: safe wait, no endpoint");
    make_v2_raw(bad, EUI_B, &NET_OTBR, &EP_OTBR);
    CHECK(boot_with_policy(&c, PB_ENROLL_ENDPOINT_POLICY_REQUIRED, bad, sizeof(bad), &NET_OTBR) == PB_ENROLL_STATE_WAITING &&
              c.last == PB_ENROLL_RESULT_MARKER_FOREIGN && !c.has_bridge_endpoint,
          "another board's version 2 record: safe wait (marker_foreign)");
    CHECK(boot_with_policy(&c, PB_ENROLL_ENDPOINT_POLICY_REQUIRED, v2, sizeof(v2), &NET_STANDALONE) == PB_ENROLL_STATE_WAITING &&
              c.last == PB_ENROLL_RESULT_DATASET_MISMATCH && !c.has_bridge_endpoint && g_ot.thread_start_calls == 0,
          "version 2 record for another network than the Dataset: safe wait (dataset_mismatch)");
    CHECK(boot_with_policy(&c, PB_ENROLL_ENDPOINT_POLICY_REQUIRED, v2, sizeof(v2), NULL) == PB_ENROLL_STATE_WAITING &&
              c.last == PB_ENROLL_RESULT_DATASET_MISSING,
          "version 2 record without a Dataset: safe wait (dataset_missing)");
    CHECK(boot_with_policy(&c, PB_ENROLL_ENDPOINT_POLICY_REQUIRED, NULL, 0, &NET_OTBR) == PB_ENROLL_STATE_WAITING &&
              c.last == PB_ENROLL_RESULT_NONE,
          "no record: safe wait (none), as before");
    /* A version 1 record that would not have resumed anyway keeps its own reason. */
    make_v1_raw(bad, EUI_B, &NET_OTBR);
    CHECK(boot_with_policy(&c, PB_ENROLL_ENDPOINT_POLICY_REQUIRED, bad, PB_ENROLL_MARKER_LEN, &NET_OTBR) == PB_ENROLL_STATE_WAITING &&
              c.last == PB_ENROLL_RESULT_MARKER_FOREIGN,
          "another board's version 1 record: marker_foreign, as before");

    printf("\n-- no policy in the image (OPTIONAL): a version 1 record resumes exactly as before --\n");
    CHECK(boot_with_policy(&c, PB_ENROLL_ENDPOINT_POLICY_OPTIONAL, v1, sizeof(v1), &NET_OTBR) == PB_ENROLL_STATE_ENROLLED &&
              c.resumed && c.last == PB_ENROLL_RESULT_RESUMED && !c.has_bridge_endpoint && g_ot.thread_start_calls == 1 &&
              g_ot.thread_start_incomplete_calls == 0 && pb_enroll_ctl_services_allowed(&c),
          "version 1 record + matching Dataset: resumes, Thread enabled once, services allowed, no endpoint");
    CHECK(boot_with_policy(&c, PB_ENROLL_ENDPOINT_POLICY_OPTIONAL, v2, sizeof(v2), &NET_OTBR) == PB_ENROLL_STATE_ENROLLED &&
              c.has_bridge_endpoint,
          "version 2 record: resumes with its endpoint");

    printf("\n-- unusable policy (INVALID): an already enrolled board still boots as before --\n");
    CHECK(boot_with_policy(&c, PB_ENROLL_ENDPOINT_POLICY_INVALID, v1, sizeof(v1), &NET_OTBR) == PB_ENROLL_STATE_ENROLLED &&
              !c.has_bridge_endpoint,
          "version 1 record: resumes (a cfg image that went bad does not take an enrolled board down)");
    CHECK(boot_with_policy(&c, PB_ENROLL_ENDPOINT_POLICY_INVALID, v2, sizeof(v2), &NET_OTBR) == PB_ENROLL_STATE_ENROLLED &&
              c.has_bridge_endpoint && ep_equal(&c.bridge_endpoint, &EP_OTBR),
          "version 2 record: resumes with its endpoint");
}

/* One boot in a child over the simulated flash file, policy REQUIRED.
 * `request`: 0 none, 1 a request without an endpoint, 2 a full enrollment
 * with EP_OTBR. Exit: 10 + state (40 + state when EP_OTBR is in use), 60 if a
 * request without an endpoint was NOT refused. */
static int run_boot_required(int request)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        if (!nvs_sim_set_backing_file(g_flash)) {
            _exit(91);
        }
        pb_enroll_ctl_t c;
        pb_enroll_ctl_init(&c, &k_ops, EUI_A);
        pb_enroll_ctl_set_endpoint_policy(&c, PB_ENROLL_ENDPOINT_POLICY_REQUIRED);
        pb_enroll_ctl_boot(&c);
        char buf[PB_ENROLL_PSKD_BUF_LEN];
        if (request == 1) {
            fill_pskd(buf, PSKD);
            if (pb_enroll_ctl_request_join(&c, buf, 0) != ESP_ERR_NOT_ALLOWED) {
                _exit(60);
            }
        } else if (request == 2) {
            fill_pskd(buf, PSKD);
            if (pb_enroll_ctl_request_join_with_endpoint(&c, buf, &EP_OTBR, 0) != ESP_OK) {
                _exit(92);
            }
            ot_store_joiner_partial();
            pb_enroll_ctl_on_joiner_done(&c, g_ot.last_attempt, PB_ENROLL_JOINER_OK, 10);
            g_ot.complete = true;
            g_ot.id = NET_OTBR;
            pb_enroll_ctl_tick(&c, true, 20);
        }
        _exit((c.has_bridge_endpoint && ep_equal(&c.bridge_endpoint, &EP_OTBR) ? 40 : 10) + (int)c.state);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 99;
}

static void scenario_endpoint_policy_reboots(void)
{
    printf("\n-- endpoint policy REQUIRED across reboots (simulated flash) --\n");
    snprintf(g_flash, sizeof(g_flash), "/tmp/pb_enroll_policy_flash_%d", (int)getpid());
    const int WAIT = 10 + PB_ENROLL_STATE_WAITING;

    /* The board was enrolled the old way (no policy, no endpoint) and its
     * cfg image was then replaced by one that requires the endpoint. */
    flash_reset();
    ot_reset();
    CHECK(run_boot(STOP_AFTER_MARKER) == 10 + PB_ENROLL_STATE_ENROLLED, "(setup) enrolled without an endpoint: version 1 record");
    ot_reset();
    ot_store_complete(&NET_OTBR);
    CHECK(run_boot(0) == 10 + PB_ENROLL_STATE_ENROLLED, "(setup) without the policy that record resumes");
    CHECK(run_boot_required(0) == WAIT, "boot with the policy: safe wait");
    CHECK(run_boot_required(0) == WAIT, "next boot: still waiting (nothing was 'repaired' in between)");
    CHECK(run_boot(0) == 10 + PB_ENROLL_STATE_ENROLLED,
          "the record and the Dataset are still there: without the policy it would resume again -- booting erased nothing");
    CHECK(run_boot_required(1) == WAIT, "a request without an endpoint in that state: refused");
    CHECK(run_boot(0) == 10 + PB_ENROLL_STATE_ENROLLED, "and it erased nothing either");
    ot_reset();
    CHECK(run_boot_required(2) == 40 + PB_ENROLL_STATE_ENROLLED, "a request with the endpoint enrolls (and replaces the old record)");
    ot_reset();
    ot_store_complete(&NET_OTBR);
    CHECK(run_boot_required(0) == 40 + PB_ENROLL_STATE_ENROLLED, "next boot: resumes with the delivered endpoint");
    CHECK(run_boot_required(0) == 40 + PB_ENROLL_STATE_ENROLLED, "and the boot after that");
    flash_reset();
}

int main(void)
{
    scenario_pskd_format();
    scenario_boot_rules();
    scenario_join_rejections();
    scenario_joiner_failures();
    scenario_join_success_and_attach();
    scenario_pskd_never_logged();
    scenario_reboots();
    scenario_endpoint_check();
    scenario_record_versions();
    scenario_endpoint_attempts();
    scenario_endpoint_boot();
    scenario_endpoint_never_logged();
    scenario_endpoint_reboots();
    scenario_endpoint_policy_requests();
    scenario_endpoint_policy_boot();
    scenario_endpoint_policy_reboots();

    printf("\n%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
