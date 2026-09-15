/*
 * Host-compiled regression test for the AUTO-mode POGO hotswap recovery
 * policy in components/pogo_port/pogo_port.c: DETECT closing before UART/GND
 * fully seats (or a peer taking a while to boot) must no longer wedge a real
 * port in ERROR forever -- DOCK_TIMEOUT/LIVENESS_LOST are transient and now
 * auto-retry after CONFIG_POGO_HOTSWAP_COOLDOWN_MS as long as DETECT_IN still
 * reads attached, while configuration/compatibility errors (PEER_TYPE_CONFLICT,
 * PROTOCOL_VERSION_ERROR, CAPABILITY_MISMATCH) still require a physical
 * detach/reattach. See docs/pogo-interface.md and pogo_port.h's own comments.
 *
 * Unlike every other pogo_port-related host test in this directory (which
 * mirror pogo_port.c's logic by hand because its real GPIO/UART/FreeRTOS
 * calls could not previously be host-compiled), THIS test links the REAL,
 * UNMODIFIED components/pogo_port/pogo_port.c and pogo_port_cleanup.c --
 * made possible by the new tools/host_test_stubs/driver/{gpio,uart}.h +
 * freertos/{task,queue}.h stack, which provides real fake hardware (an
 * in-memory GPIO bank and UART RX/TX rings, see those files' own comments)
 * and real pthread-backed FreeRTOS task/queue semantics so pogo_port.c's
 * REAL port_task runs exactly as it does on real hardware -- real
 * concurrency, real wall-clock delays (scaled down via
 * tools/host_test_stubs/pogo_fast/sdkconfig.h so the whole suite still runs
 * in a few seconds), real retry/cooldown/reconciliation timing.
 *
 * The test acts as "the peer board at the other end of the wire": it reads
 * what pogo_port.c's port_task wrote via host_uart_test_take_tx_byte() and
 * decodes it with the REAL pogo_frame_parser_t/pogo_unpack_peer_hello() (no
 * hand-rolled protocol decoding), and injects bytes via
 * host_uart_test_push_rx() built with the REAL pogo_frame_encode()/
 * pogo_pack_peer_hello(). DETECT_IN edges are driven via
 * host_gpio_test_edge() (a normal, working interrupt) or
 * host_gpio_test_set_level() (a missed interrupt, for the reconciliation
 * scenario) -- see driver/gpio.h's own comment on that distinction.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs/pogo_fast -I tools/host_test_stubs \
 *      -I components/pogo_port/include -I components/pogo_proto/include \
 *      -o /tmp/test_pogo_port_hotswap_recovery tools/test_pogo_port_hotswap_recovery.c \
 *      components/pogo_port/pogo_port.c components/pogo_port/pogo_port_cleanup.c \
 *      components/pogo_proto/pogo_frame.c components/pogo_proto/pogo_crc16.c \
 *      components/pogo_proto/pogo_protocol.c \
 *      tools/host_test_stubs/host_gpio_sim.c tools/host_test_stubs/host_uart_sim.c \
 *   && /tmp/test_pogo_port_hotswap_recovery
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include "pogo_port/pogo_port.h"
#include "pogo_proto/pogo_frame.h"
#include "pogo_proto/pogo_protocol.h"
#include "driver/gpio.h"
#include "driver/uart.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        g_checks++;                                                                                                  \
        if (!(cond)) {                                                                                               \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                                   \
            g_failures++;                                                                                            \
        } else {                                                                                                     \
            printf("ok:   %s\n", msg);                                                                               \
        }                                                                                                            \
    } while (0)

#define TEST_UART_NUM 0
#define TEST_DETECT_IN_PIN 12
#define TEST_DETECT_OUT_PIN 2
#define TEST_TX_GPIO 10
#define TEST_RX_GPIO 11

/* --- Event log: every callback from pogo_port_init()'s cb, consumed
 * in-order by wait_for_next_event() below --- */
#define EVT_LOG_CAP 128
typedef struct {
    pogo_port_event_t event;
    pogo_port_event_data_t data;
} logged_evt_t;

static pthread_mutex_t s_evt_mutex = PTHREAD_MUTEX_INITIALIZER;
static logged_evt_t s_evt_log[EVT_LOG_CAP];
static int s_evt_count;
static int s_evt_cursor;

static void port_event_cb(pogo_port_id_t port_id, pogo_port_event_t event, const pogo_port_event_data_t *data,
                           void *user_ctx)
{
    (void)port_id;
    (void)user_ctx;
    pthread_mutex_lock(&s_evt_mutex);
    if (s_evt_count < EVT_LOG_CAP) {
        s_evt_log[s_evt_count].event = event;
        s_evt_log[s_evt_count].data = data ? *data : (pogo_port_event_data_t){0};
        s_evt_count++;
    }
    pthread_mutex_unlock(&s_evt_mutex);
}

static void reset_events(void)
{
    pthread_mutex_lock(&s_evt_mutex);
    s_evt_count = 0;
    s_evt_cursor = 0;
    pthread_mutex_unlock(&s_evt_mutex);
}

static int count_events(pogo_port_event_t want)
{
    int n = 0;
    pthread_mutex_lock(&s_evt_mutex);
    for (int i = 0; i < s_evt_count; i++) {
        if (s_evt_log[i].event == want) {
            n++;
        }
    }
    pthread_mutex_unlock(&s_evt_mutex);
    return n;
}

/* Waits up to timeout_ms for the NEXT (in log order, past whatever this
 * function has already consumed) occurrence of `want`, advancing the shared
 * cursor past it on success -- lets scenario code read a sequence of
 * expected events in order without re-matching ones already checked. */
static bool wait_for_next_event(pogo_port_event_t want, uint32_t timeout_ms, pogo_port_event_data_t *out_data)
{
    uint32_t waited_ms = 0;
    while (waited_ms <= timeout_ms) {
        pthread_mutex_lock(&s_evt_mutex);
        bool found = false;
        pogo_port_event_data_t data = {0};
        for (int i = s_evt_cursor; i < s_evt_count; i++) {
            if (s_evt_log[i].event == want) {
                found = true;
                data = s_evt_log[i].data;
                s_evt_cursor = i + 1;
                break;
            }
        }
        pthread_mutex_unlock(&s_evt_mutex);
        if (found) {
            if (out_data != NULL) {
                *out_data = data;
            }
            return true;
        }
        usleep(5000);
        waited_ms += 5;
    }
    return false;
}

static uint32_t elapsed_ms_since(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((now.tv_sec - start->tv_sec) * 1000L + (now.tv_nsec - start->tv_nsec) / 1000000L);
}

/* Reads bytes the port under test transmitted (via host_uart_test_take_tx_byte)
 * and decodes them with the REAL pogo_frame_parser_t, up to timeout_ms total.
 * Returns true and fills *out on the first complete frame. */
static bool fake_peer_read_frame(uint32_t timeout_ms, pogo_frame_t *out)
{
    pogo_frame_parser_t parser;
    pogo_frame_parser_init(&parser);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (elapsed_ms_since(&start) < timeout_ms) {
        uint8_t b;
        if (host_uart_test_take_tx_byte(TEST_UART_NUM, &b, 20)) {
            if (pogo_frame_parser_feed(&parser, b, out)) {
                return true;
            }
        }
    }
    return false;
}

/* Discards any bytes already sitting in the port's TX ring (e.g. an earlier,
 * deliberately-ignored cycle's retries) so the next fake_peer_read_frame()
 * call cleanly captures the NEXT cycle's own first HELLO_REQ, not a stale
 * leftover one. */
static void fake_peer_drain_tx(void)
{
    uint8_t b;
    while (host_uart_test_take_tx_byte(TEST_UART_NUM, &b, 20)) {
        /* discard */
    }
}

static void fake_peer_send_hello(uint8_t msg_type, uint8_t seq, uint8_t protocol_version, uint8_t peer_type,
                                  uint8_t capabilities, bool legacy_2byte_payload)
{
    uint8_t payload[3];
    size_t len;
    if (legacy_2byte_payload) {
        payload[0] = protocol_version;
        payload[1] = peer_type;
        len = 2;
    } else {
        pogo_peer_hello_t h = {.protocol_version = protocol_version, .peer_type = peer_type,
                                .capabilities = capabilities};
        len = pogo_pack_peer_hello(&h, payload, sizeof(payload));
    }
    uint8_t buf[POGO_FRAME_MAX_SIZE];
    size_t n = pogo_frame_encode(msg_type, seq, payload, len, buf, sizeof(buf));
    host_uart_test_push_rx(TEST_UART_NUM, buf, n);
}

/* Waits for the port's next HELLO_REQ and answers it with a normal,
 * capability-complete HELLO_RESP (same seq) -- the common "peer behaves
 * correctly" step used by several scenarios below. */
static bool fake_peer_answer_next_hello_req(uint32_t wait_ms)
{
    pogo_frame_t frame;
    if (!fake_peer_read_frame(wait_ms, &frame) || frame.type != POGO_MSG_PEER_HELLO_REQ) {
        return false;
    }
    fake_peer_send_hello(POGO_MSG_PEER_HELLO_RESP, frame.seq, POGO_FRAME_VERSION, POGO_PEER_TYPE_MODULE,
                          POGO_HELLO_SUPPORTED_CAPABILITIES, false);
    return true;
}

static void scenario_setup(void)
{
    host_gpio_test_reset();
    host_uart_test_reset();
    reset_events();

    pogo_port_config_t cfg = {
        .enabled = true,
        .uart_num = TEST_UART_NUM,
        .tx_gpio = TEST_TX_GPIO,
        .rx_gpio = TEST_RX_GPIO,
        .detect_in_gpio = TEST_DETECT_IN_PIN,
        .detect_out_gpio = TEST_DETECT_OUT_PIN,
        .base_id_gpio = -1,
        .detect_out_active_low = true,
        .debounce_ms = 10,
        .baud_rate = 115200,
    };
    esp_err_t err = pogo_port_init(POGO_PORT_UPSTREAM, &cfg, port_event_cb, NULL);
    CHECK(err == ESP_OK, "pogo_port_init succeeds");
    err = pogo_port_enable(POGO_PORT_UPSTREAM, POGO_PEER_TYPE_MODULE);
    CHECK(err == ESP_OK, "pogo_port_enable succeeds");
}

static void scenario_teardown(void)
{
    esp_err_t err = pogo_port_disable(POGO_PORT_UPSTREAM);
    CHECK(err == ESP_OK, "pogo_port_disable succeeds (also proves the port task stopped cleanly)");
}

/* --- Scenario 1: boot detached, attach later --- */
static void scenario_boot_detached_then_attach_later(void)
{
    printf("\n-- scenario: boots fully detached, docks later --\n");
    scenario_setup();

    usleep(100 * 1000); /* well past ATTACH_SETTLE/ACK_TIMEOUT if it were (wrongly) attaching on its own */
    CHECK(!pogo_port_is_attached(POGO_PORT_UPSTREAM), "stays un-attached while DETECT_IN reads detached at boot");
    CHECK(count_events(POGO_PORT_EVENT_ATTACHED) == 0, "no spurious ATTACHED before any real edge");

    host_gpio_test_edge(TEST_DETECT_IN_PIN, 0);
    CHECK(wait_for_next_event(POGO_PORT_EVENT_ATTACHED, 500, NULL), "ATTACHED fires once DETECT_IN debounces Low");
    CHECK(fake_peer_answer_next_hello_req(500), "HELLO_REQ sent and answered");
    pogo_port_event_data_t ready;
    CHECK(wait_for_next_event(POGO_PORT_EVENT_PEER_READY, 300, &ready), "reaches PEER_READY");
    CHECK(ready.peer_type == POGO_PEER_TYPE_MODULE, "peer_type recorded correctly");

    scenario_teardown();
}

/* --- Scenario 2: DETECT closes first, UART/peer arrives late enough that
 * the first cycle times out; recovers with NO new DETECT edge --- */
static void scenario_dock_timeout_then_recover_without_new_edge(void)
{
    printf("\n-- scenario: first handshake cycle times out (no peer yet); auto-recovers with NO new DETECT edge "
           "--\n");
    scenario_setup();

    host_gpio_test_edge(TEST_DETECT_IN_PIN, 0);
    CHECK(wait_for_next_event(POGO_PORT_EVENT_ATTACHED, 500, NULL), "ATTACHED fires");

    pogo_port_event_data_t err_data;
    CHECK(wait_for_next_event(POGO_PORT_EVENT_ERROR, 800, &err_data), "unanswered first cycle -> ERROR");
    CHECK(err_data.error_code == POGO_PORT_ERR_DOCK_TIMEOUT, "reported error is DOCK_TIMEOUT");
    fake_peer_drain_tx(); /* discard cycle 1's own unanswered HELLO_REQ retries */

    CHECK(fake_peer_answer_next_hello_req(800),
          "a fresh HELLO_REQ is sent automatically after the cooldown, with NO new DETECT edge, and is answered");
    pogo_port_event_data_t ready;
    CHECK(wait_for_next_event(POGO_PORT_EVENT_PEER_READY, 300, &ready), "second cycle reaches PEER_READY");
    CHECK(ready.peer_type == POGO_PEER_TYPE_MODULE, "peer_type recorded correctly");

    scenario_teardown();
}

/* --- Scenario 3: transient timeout, peer boots even later (two failed
 * cycles before the peer finally answers) --- */
static void scenario_transient_timeout_peer_boots_late(void)
{
    printf("\n-- scenario: transient timeout, peer boots late (recovers on the 3rd cycle) --\n");
    scenario_setup();

    host_gpio_test_edge(TEST_DETECT_IN_PIN, 0);
    CHECK(wait_for_next_event(POGO_PORT_EVENT_ATTACHED, 500, NULL), "ATTACHED fires");

    for (int cycle = 1; cycle <= 2; cycle++) {
        pogo_port_event_data_t err_data;
        char msg[96];
        snprintf(msg, sizeof(msg), "cycle %d times out (peer still not booted) -> ERROR", cycle);
        CHECK(wait_for_next_event(POGO_PORT_EVENT_ERROR, 800, &err_data), msg);
        CHECK(err_data.error_code == POGO_PORT_ERR_DOCK_TIMEOUT, "reported error is DOCK_TIMEOUT");
        fake_peer_drain_tx();
    }

    CHECK(fake_peer_answer_next_hello_req(800), "peer finally boots and answers cycle 3's HELLO_REQ");
    CHECK(wait_for_next_event(POGO_PORT_EVENT_PEER_READY, 300, NULL), "reaches PEER_READY once the peer is up");

    scenario_teardown();
}

/* --- Scenario 4: detach during cooldown cancels the pending retry --- */
static void scenario_detach_during_cooldown_cancels_retry(void)
{
    printf("\n-- scenario: DETECT detaches during the cooldown -- pending retry is cancelled --\n");
    scenario_setup();

    host_gpio_test_edge(TEST_DETECT_IN_PIN, 0);
    CHECK(wait_for_next_event(POGO_PORT_EVENT_ATTACHED, 500, NULL), "ATTACHED fires");
    CHECK(wait_for_next_event(POGO_PORT_EVENT_ERROR, 800, NULL), "first cycle times out -> ERROR, cooldown begins");
    fake_peer_drain_tx();

    usleep(50 * 1000); /* partway through CONFIG_POGO_HOTSWAP_COOLDOWN_MS (150ms) */
    host_gpio_test_edge(TEST_DETECT_IN_PIN, 1);
    CHECK(wait_for_next_event(POGO_PORT_EVENT_DETACHED, 300, NULL),
          "DETACHED fires, pre-empting the in-progress cooldown");

    pogo_frame_t frame;
    CHECK(!fake_peer_read_frame(500, &frame),
          "no HELLO_REQ is ever sent afterward -- the cooldown's retry was genuinely cancelled, not just delayed");

    scenario_teardown();
}

/* --- Scenario 5: a configuration/compatibility error (CAPABILITY_MISMATCH,
 * representative of PEER_TYPE_CONFLICT/PROTOCOL_VERSION_ERROR too -- all
 * three are raised directly in validate_incoming_hello() and never call
 * enter_recover_cooldown(), unlike DOCK_TIMEOUT/LIVENESS_LOST) never
 * auto-retries; only a real detach/reattach recovers it. --- */
static void scenario_capability_mismatch_never_auto_retries(void)
{
    printf("\n-- scenario: CAPABILITY_MISMATCH never auto-retries; only a physical detach/reattach recovers --\n");
    scenario_setup();

    host_gpio_test_edge(TEST_DETECT_IN_PIN, 0);
    CHECK(wait_for_next_event(POGO_PORT_EVENT_ATTACHED, 500, NULL), "ATTACHED fires");

    pogo_frame_t frame;
    CHECK(fake_peer_read_frame(500, &frame) && frame.type == POGO_MSG_PEER_HELLO_REQ, "HELLO_REQ received");
    /* A legacy 2-byte HELLO payload (pre-capability-negotiation peer). */
    fake_peer_send_hello(POGO_MSG_PEER_HELLO_RESP, frame.seq, POGO_FRAME_VERSION, POGO_PEER_TYPE_MODULE, 0, true);

    pogo_port_event_data_t err_data;
    CHECK(wait_for_next_event(POGO_PORT_EVENT_ERROR, 300, &err_data), "legacy HELLO -> ERROR immediately");
    CHECK(err_data.error_code == POGO_PORT_ERR_CAPABILITY_MISMATCH, "reported error is CAPABILITY_MISMATCH");
    fake_peer_drain_tx();

    CHECK(!fake_peer_read_frame(600, &frame),
          "no auto-retry at all for a configuration/compatibility error, even well past one cooldown+cycle");

    host_gpio_test_edge(TEST_DETECT_IN_PIN, 1);
    CHECK(wait_for_next_event(POGO_PORT_EVENT_DETACHED, 300, NULL), "physical detach observed");
    host_gpio_test_edge(TEST_DETECT_IN_PIN, 0);
    CHECK(wait_for_next_event(POGO_PORT_EVENT_ATTACHED, 300, NULL), "physical reattach observed");
    CHECK(fake_peer_answer_next_hello_req(500), "a fresh handshake starts after the physical detach/reattach");
    CHECK(wait_for_next_event(POGO_PORT_EVENT_PEER_READY, 300, NULL), "recovers to PEER_READY after reattach");

    scenario_teardown();
}

/* --- Scenario 6: liveness loss (PEER_READY, then the peer stops answering
 * PING) recovers once the peer returns, with no physical reset needed --- */
static void scenario_liveness_lost_then_peer_returns(void)
{
    printf("\n-- scenario: liveness lost after PEER_READY; peer returns and recovers without a reset --\n");
    scenario_setup();

    host_gpio_test_edge(TEST_DETECT_IN_PIN, 0);
    CHECK(wait_for_next_event(POGO_PORT_EVENT_ATTACHED, 500, NULL), "ATTACHED fires");
    CHECK(fake_peer_answer_next_hello_req(500), "initial handshake completes");
    CHECK(wait_for_next_event(POGO_PORT_EVENT_PEER_READY, 300, NULL), "reaches PEER_READY");

    /* Stop answering entirely -- idle PING/PONG liveness will time out. */
    pogo_port_event_data_t err_data;
    CHECK(wait_for_next_event(POGO_PORT_EVENT_ERROR, 800, &err_data), "idle PING/PONG failure -> ERROR");
    CHECK(err_data.error_code == POGO_PORT_ERR_LIVENESS_LOST, "reported error is LIVENESS_LOST");
    fake_peer_drain_tx();

    CHECK(fake_peer_answer_next_hello_req(800),
          "peer returns and answers the fresh handshake, with NO new DETECT edge");
    CHECK(wait_for_next_event(POGO_PORT_EVENT_PEER_READY, 300, NULL),
          "recovers to PEER_READY without any physical reset");

    scenario_teardown();
}

/* --- Scenario 7: a missed GPIO ISR (or dropped queue edge) still gets
 * corrected by periodic level reconciliation, both for an attach and a
 * detach. --- */
static void scenario_missed_isr_reconciliation(void)
{
    printf("\n-- scenario: missed-ISR DETECT_IN changes are still caught by level reconciliation --\n");
    scenario_setup();

    /* Raw level change, deliberately WITHOUT going through host_gpio_test_edge()
     * (i.e. no ISR fires) -- models a lost interrupt / a detect_queue that
     * overflowed before this edge could be queued. */
    host_gpio_test_set_level(TEST_DETECT_IN_PIN, 0);
    CHECK(wait_for_next_event(POGO_PORT_EVENT_ATTACHED, 700, NULL),
          "reconciliation still reaches ATTACHED despite no ISR ever firing");
    CHECK(fake_peer_answer_next_hello_req(500), "handshake proceeds normally afterward");
    CHECK(wait_for_next_event(POGO_PORT_EVENT_PEER_READY, 300, NULL), "reaches PEER_READY");

    host_gpio_test_set_level(TEST_DETECT_IN_PIN, 1); /* raw detach, again no ISR */
    CHECK(wait_for_next_event(POGO_PORT_EVENT_DETACHED, 700, NULL),
          "reconciliation also catches a missed-ISR detach");

    scenario_teardown();
}

/* --- Scenario 8: repeated recovery cycles stay bounded -- no busy loop, no
 * event storm. (No duplicate task creation is a structural property of
 * pogo_port_enable(), which has exactly one xTaskCreate() call site, ever
 * exercised exactly once per scenario_setup() here -- every recovery cycle
 * below runs inside that SAME task.) --- */
static void scenario_repeated_recovery_bounded(void)
{
    printf("\n-- scenario: repeated recovery cycles are time-bounded and do not flood events --\n");
    scenario_setup();

    host_gpio_test_edge(TEST_DETECT_IN_PIN, 0);
    CHECK(wait_for_next_event(POGO_PORT_EVENT_ATTACHED, 500, NULL), "ATTACHED fires");

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const int cycles = 3;
    for (int i = 1; i <= cycles; i++) {
        pogo_port_event_data_t err_data;
        CHECK(wait_for_next_event(POGO_PORT_EVENT_ERROR, 800, &err_data), "cycle times out with a bounded ERROR event");
        CHECK(err_data.error_code == POGO_PORT_ERR_DOCK_TIMEOUT, "reported error is DOCK_TIMEOUT");
    }
    uint32_t elapsed_ms = elapsed_ms_since(&start);
    printf("info: %d recovery cycles took %ums wall-clock\n", cycles, (unsigned)elapsed_ms);
    CHECK(elapsed_ms > 400, "three recovery cycles take real, non-zero wall-clock time (not a busy loop)");
    CHECK(elapsed_ms < 4000, "three recovery cycles do not run away either (cooldown/backoff stay bounded)");
    CHECK(count_events(POGO_PORT_EVENT_ERROR) == cycles,
          "exactly one ERROR event per cycle -- no duplicate/flooded notifications");
    CHECK(count_events(POGO_PORT_EVENT_ATTACHED) == 1,
          "exactly one ATTACHED for the whole scenario -- no spurious re-attach events from the recovery loop");

    scenario_teardown();
}

int main(void)
{
    scenario_boot_detached_then_attach_later();
    scenario_dock_timeout_then_recover_without_new_edge();
    scenario_transient_timeout_peer_boots_late();
    scenario_detach_during_cooldown_cancels_retry();
    scenario_capability_mismatch_never_auto_retries();
    scenario_liveness_lost_then_peer_returns();
    scenario_missed_isr_reconciliation();
    scenario_repeated_recovery_bounded();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
