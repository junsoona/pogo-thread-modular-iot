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

int main(void)
{
    scenario_pskd_format();
    scenario_boot_rules();
    scenario_join_rejections();
    scenario_joiner_failures();
    scenario_join_success_and_attach();
    scenario_pskd_never_logged();
    scenario_reboots();

    printf("\n%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
