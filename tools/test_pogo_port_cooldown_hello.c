/*
 * Host-compiled regression test: a POGO port waiting out its transient-error
 * cooldown (RECOVER_COOLDOWN) must still answer a valid PEER_HELLO_REQ and
 * start its own handshake, so two retrying ports whose retry loops are out
 * of phase find each other again without a physical re-dock.
 *
 * Both ports run the same loop: a handshake window of (MAX_RETRY+1) x
 * ACK_TIMEOUT (1.2s), then a cooldown (3s). A port that ignores its UART
 * during the cooldown never hears a peer whose window falls entirely inside
 * that cooldown; with equal periods, once each side's window sits inside the
 * other's cooldown it stays there. That happens after a staggered dock (1-2s
 * apart) and after a one-direction dropout of an established link, on a
 * perfectly good wire.
 *
 * Links the REAL, UNMODIFIED components/pogo_port/pogo_port.c with the REAL
 * components/pogo_proto/Kconfig default timings
 * (tools/host_test_stubs/pogo_kconfig_defaults/sdkconfig.h -- see that
 * file for why pogo_fast's numbers cannot reproduce this), on the same
 * pthread FreeRTOS + fake GPIO/UART stack as
 * tools/test_pogo_port_hotswap_recovery.c.
 *
 *   - pair scenarios: two real pogo_port instances (one module's DOWNSTREAM
 *     face, the next module's UPSTREAM face) wired TX->RX through bridge
 *     threads that can drop either direction.
 *   - single-port scenarios: the test is the peer, reading the port's TX
 *     with the real frame parser and injecting frames built with the real
 *     encoder, to check exactly what the port does with each kind of input
 *     during a cooldown.
 *
 * Runs on real wall-clock time (about 2.5 minutes). An optional argument
 * runs only the scenarios whose name contains it.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs/pogo_kconfig_defaults -I tools/host_test_stubs \
 *      -I components/pogo_port/include -I components/pogo_proto/include \
 *      -o /tmp/test_pogo_port_cooldown_hello tools/test_pogo_port_cooldown_hello.c \
 *      components/pogo_port/pogo_port.c components/pogo_port/pogo_port_cleanup.c \
 *      components/pogo_proto/pogo_frame.c components/pogo_proto/pogo_crc16.c \
 *      components/pogo_proto/pogo_protocol.c \
 *      tools/host_test_stubs/host_gpio_sim.c tools/host_test_stubs/host_uart_sim.c \
 *   && /tmp/test_pogo_port_cooldown_hello
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include "sdkconfig.h"
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

#define HANDSHAKE_MS ((CONFIG_POGO_MAX_RETRY + 1) * CONFIG_POGO_ACK_TIMEOUT_MS) /* one cycle's HELLO window */
#define COOLDOWN_MS CONFIG_POGO_HOTSWAP_COOLDOWN_MS
#define CYCLE_MS (HANDSHAKE_MS + COOLDOWN_MS)
#define DEBOUNCE_MS 30 /* module/power_base app_main.c default */
/* Long enough for an idle link to run PING/PONG and, if it were broken, to
 * declare LIVENESS_LOST (PING interval + MAX_RETRY timeouts). */
#define STABLE_MS (CONFIG_POGO_PING_INTERVAL_MS + (CONFIG_POGO_MAX_RETRY + 1) * CONFIG_POGO_ACK_TIMEOUT_MS + 300)

/* Pair: module A's DOWNSTREAM face docked to module B's UPSTREAM face. */
#define PA POGO_PORT_DOWNSTREAM
#define PB POGO_PORT_UPSTREAM
#define A_UART 1
#define B_UART 0
#define A_DETECT 4
#define B_DETECT 12
/* Single-port scenarios use B's wiring; the test itself is the peer. */
#define PU PB
#define U_UART B_UART
#define U_DETECT B_DETECT

static uint64_t mono_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000ULL + (uint64_t)now.tv_nsec / 1000000ULL;
}

static void sleep_ms(uint32_t ms)
{
    usleep(ms * 1000u);
}

static void sleep_until(uint64_t t)
{
    uint64_t now = mono_ms();
    if (t > now) {
        sleep_ms((uint32_t)(t - now));
    }
}

static uint32_t cpu_ms(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (uint32_t)((ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000L +
                      (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000L);
}

/* --- Log capture: pogo_port.c logs to stderr (tools/host_test_stubs/esp_log.h),
 * which main() points at a temp file so a scenario can count the lines it
 * produced. POGO_TEST_SHOW_LOG=1 prints the whole log at the end. --- */
static FILE *s_log;

static long log_mark(void)
{
    fflush(stderr);
    struct stat st;
    return (s_log != NULL && fstat(fileno(s_log), &st) == 0) ? (long)st.st_size : 0;
}

static int log_lines_since(long mark, const char *needle)
{
    long end = log_mark();
    if (s_log == NULL || end <= mark) {
        return 0;
    }
    size_t len = (size_t)(end - mark);
    char *buf = malloc(len + 1);
    if (buf == NULL) {
        return -1;
    }
    size_t got = (size_t)pread(fileno(s_log), buf, len, mark);
    buf[got] = '\0';
    int n = 0;
    for (char *line = strtok(buf, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        if (strstr(line, needle) != NULL) {
            n++;
        }
    }
    free(buf);
    return n;
}

/* --- Event log (both ports) --- */
#define EVT_CAP 1024
typedef struct {
    pogo_port_id_t port;
    pogo_port_event_t ev;
    pogo_port_event_data_t data;
    uint64_t t;
} evt_t;

static pthread_mutex_t s_evt_mutex = PTHREAD_MUTEX_INITIALIZER;
static evt_t s_evts[EVT_CAP];
static int s_nevts;

static const char *port_name(pogo_port_id_t id)
{
    return id == PA ? "A(downstream face)" : "B(upstream face)  ";
}

static const char *err_name(pogo_port_error_t e)
{
    switch (e) {
    case POGO_PORT_ERR_DOCK_TIMEOUT: return "DOCK_TIMEOUT";
    case POGO_PORT_ERR_LIVENESS_LOST: return "LIVENESS_LOST";
    case POGO_PORT_ERR_PEER_TYPE_CONFLICT: return "PEER_TYPE_CONFLICT";
    case POGO_PORT_ERR_PROTOCOL_VERSION: return "PROTOCOL_VERSION";
    case POGO_PORT_ERR_CAPABILITY_MISMATCH: return "CAPABILITY_MISMATCH";
    default: return "?";
    }
}

static uint64_t s_scenario_t0;
static atomic_bool s_trace = false; /* print events as they happen (pair scenarios) */

static void on_event(pogo_port_id_t id, pogo_port_event_t ev, const pogo_port_event_data_t *data, void *ctx)
{
    (void)ctx;
    uint64_t now = mono_ms();
    pthread_mutex_lock(&s_evt_mutex);
    if (s_nevts < EVT_CAP) {
        s_evts[s_nevts].port = id;
        s_evts[s_nevts].ev = ev;
        s_evts[s_nevts].data = data ? *data : (pogo_port_event_data_t){0};
        s_evts[s_nevts].t = now;
        s_nevts++;
    }
    pthread_mutex_unlock(&s_evt_mutex);
    if (atomic_load(&s_trace) && ev != POGO_PORT_EVENT_FRAME_RECEIVED) {
        static const char *names[] = {"ATTACHED", "DETACHED", "PEER_READY", "FRAME", "ERROR"};
        printf("      t=%6u ms  %s %s%s%s\n", (unsigned)(now - s_scenario_t0), port_name(id), names[ev],
               ev == POGO_PORT_EVENT_ERROR ? " " : "", ev == POGO_PORT_EVENT_ERROR ? err_name(data->error_code) : "");
    }
}

static void evt_reset(void)
{
    pthread_mutex_lock(&s_evt_mutex);
    s_nevts = 0;
    pthread_mutex_unlock(&s_evt_mutex);
}

static int evt_mark(void)
{
    pthread_mutex_lock(&s_evt_mutex);
    int n = s_nevts;
    pthread_mutex_unlock(&s_evt_mutex);
    return n;
}

/* port < 0: any port. code < 0: any error code. */
static int count_since(int mark, int port, pogo_port_event_t ev, int code)
{
    int n = 0;
    pthread_mutex_lock(&s_evt_mutex);
    for (int i = mark; i < s_nevts; i++) {
        if ((port < 0 || (int)s_evts[i].port == port) && s_evts[i].ev == ev &&
            (code < 0 || (int)s_evts[i].data.error_code == code)) {
            n++;
        }
    }
    pthread_mutex_unlock(&s_evt_mutex);
    return n;
}

static bool wait_event_since(int mark, int port, pogo_port_event_t ev, uint32_t timeout_ms, evt_t *out)
{
    uint64_t deadline = mono_ms() + timeout_ms;
    do {
        pthread_mutex_lock(&s_evt_mutex);
        for (int i = mark; i < s_nevts; i++) {
            if ((port < 0 || (int)s_evts[i].port == port) && s_evts[i].ev == ev) {
                if (out != NULL) {
                    *out = s_evts[i];
                }
                pthread_mutex_unlock(&s_evt_mutex);
                return true;
            }
        }
        pthread_mutex_unlock(&s_evt_mutex);
        sleep_ms(5);
    } while (mono_ms() < deadline);
    return false;
}

/* ===================================================================== */
/* Kconfig guard                                                         */
/* ===================================================================== */

static bool kconfig_default(const char *sym, long *out)
{
    FILE *f = fopen("components/pogo_proto/Kconfig", "r");
    if (f == NULL) {
        return false;
    }
    char line[256];
    bool in_sym = false;
    bool found = false;
    while (fgets(line, sizeof(line), f) != NULL) {
        char word[64], name[64];
        if (sscanf(line, " %63s %63s", word, name) == 2 && strcmp(word, "config") == 0) {
            in_sym = (strcmp(name, sym) == 0);
            continue;
        }
        if (in_sym && sscanf(line, " default %ld", out) == 1) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

static void check_timing_is_kconfig_default(void)
{
    printf("\n-- the timing under test is the real Kconfig default --\n");
    const struct {
        const char *sym;
        long value;
    } want[] = {
        {"POGO_ACK_TIMEOUT_MS", CONFIG_POGO_ACK_TIMEOUT_MS},
        {"POGO_MAX_RETRY", CONFIG_POGO_MAX_RETRY},
        {"POGO_HOTSWAP_COOLDOWN_MS", CONFIG_POGO_HOTSWAP_COOLDOWN_MS},
        {"POGO_PING_INTERVAL_MS", CONFIG_POGO_PING_INTERVAL_MS},
        {"POGO_ATTACH_SETTLE_MS", CONFIG_POGO_ATTACH_SETTLE_MS},
        {"POGO_DETECT_RECONCILE_MS", CONFIG_POGO_DETECT_RECONCILE_MS},
    };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        long v = -1;
        char msg[128];
        snprintf(msg, sizeof(msg), "CONFIG_%s=%ld matches components/pogo_proto/Kconfig", want[i].sym, want[i].value);
        CHECK(kconfig_default(want[i].sym, &v) && v == want[i].value, msg);
    }
    CHECK(HANDSHAKE_MS < COOLDOWN_MS,
          "handshake window shorter than the cooldown (the precondition for a retry-phase lockout)");
}

/* ===================================================================== */
/* Pair harness                                                          */
/* ===================================================================== */

static atomic_bool s_a_to_b;
static atomic_bool s_b_to_a;
static atomic_bool s_bridge_stop;
static pthread_t s_bridge_threads[2];

static void *bridge(void *arg)
{
    bool a_to_b = (arg != NULL);
    int src = a_to_b ? A_UART : B_UART;
    int dst = a_to_b ? B_UART : A_UART;
    while (!atomic_load(&s_bridge_stop)) {
        uint8_t b;
        if (!host_uart_test_take_tx_byte(src, &b, 5)) {
            continue;
        }
        if (a_to_b ? atomic_load(&s_a_to_b) : atomic_load(&s_b_to_a)) {
            host_uart_test_push_rx(dst, &b, 1);
        }
    }
    return NULL;
}

static void wire(bool a_to_b, bool b_to_a)
{
    atomic_store(&s_a_to_b, a_to_b);
    atomic_store(&s_b_to_a, b_to_a);
    if (atomic_load(&s_trace)) {
        printf("      t=%6u ms  -- wire: A->B %s, B->A %s --\n", (unsigned)(mono_ms() - s_scenario_t0),
               a_to_b ? "ok" : "BROKEN", b_to_a ? "ok" : "BROKEN");
    }
}

static pogo_port_config_t port_cfg(pogo_port_id_t id)
{
    if (id == PA) {
        return (pogo_port_config_t){.enabled = true, .uart_num = A_UART, .tx_gpio = 22, .rx_gpio = 23,
                                    .detect_in_gpio = A_DETECT, .detect_out_gpio = 5, .base_id_gpio = -1,
                                    .detect_out_active_low = true, .debounce_ms = DEBOUNCE_MS,
                                    .baud_rate = 115200};
    }
    /* UPSTREAM face with a real BASE_ID input left high (peer is a MODULE). */
    return (pogo_port_config_t){.enabled = true, .uart_num = B_UART, .tx_gpio = 10, .rx_gpio = 11,
                                .detect_in_gpio = B_DETECT, .detect_out_gpio = 2, .base_id_gpio = 14,
                                .detect_out_active_low = true, .debounce_ms = DEBOUNCE_MS, .baud_rate = 115200};
}

static void start_port(pogo_port_id_t id)
{
    pogo_port_config_t cfg = port_cfg(id);
    CHECK(pogo_port_init(id, &cfg, on_event, NULL) == ESP_OK && pogo_port_enable(id, POGO_PEER_TYPE_MODULE) == ESP_OK,
          "port init + enable");
}

/* Undock first so the port task is idle in WAIT_ATTACH when it is deleted. */
static void stop_port(pogo_port_id_t id, int detect)
{
    int mark = evt_mark();
    host_gpio_test_edge(detect, 1);
    if (pogo_port_is_attached(id)) {
        wait_event_since(mark, (int)id, POGO_PORT_EVENT_DETACHED, 500, NULL);
    }
    CHECK(pogo_port_disable(id) == ESP_OK, "port disabled cleanly");
}

static void pair_setup(const char *title)
{
    printf("\n-- pair: %s --\n", title);
    host_gpio_test_reset();
    host_uart_test_reset();
    evt_reset();
    s_scenario_t0 = mono_ms();
    atomic_store(&s_trace, true);
    wire(true, true);
    atomic_store(&s_bridge_stop, false);
    pthread_create(&s_bridge_threads[0], NULL, bridge, (void *)1);
    pthread_create(&s_bridge_threads[1], NULL, bridge, NULL);
    start_port(PA);
    start_port(PB);
}

static void pair_teardown(void)
{
    atomic_store(&s_trace, false);
    stop_port(PA, A_DETECT);
    stop_port(PB, B_DETECT);
    atomic_store(&s_bridge_stop, true);
    pthread_join(s_bridge_threads[0], NULL);
    pthread_join(s_bridge_threads[1], NULL);
}

static void dock_pair(uint32_t offset_ms)
{
    host_gpio_test_edge(A_DETECT, 0);
    if (offset_ms > 0) {
        sleep_ms(offset_ms);
    }
    host_gpio_test_edge(B_DETECT, 0);
}

/* Both sides reached PEER_READY since `mark` and are PEER_READY right now. */
static bool wait_paired(int mark, uint32_t timeout_ms, uint32_t *took_ms)
{
    uint64_t start = mono_ms();
    do {
        if (count_since(mark, PA, POGO_PORT_EVENT_PEER_READY, -1) > 0 &&
            count_since(mark, PB, POGO_PORT_EVENT_PEER_READY, -1) > 0 && pogo_port_is_peer_ready(PA) &&
            pogo_port_is_peer_ready(PB)) {
            *took_ms = (uint32_t)(mono_ms() - start);
            return true;
        }
        sleep_ms(10);
    } while (mono_ms() - start <= timeout_ms);
    *took_ms = (uint32_t)(mono_ms() - start);
    return false;
}

/* The link is not merely flapping through PEER_READY: it stays up through
 * PING/PONG with no ERROR on either side. */
static bool stays_paired(void)
{
    int mark = evt_mark();
    sleep_ms(STABLE_MS);
    return count_since(mark, -1, POGO_PORT_EVENT_ERROR, -1) == 0 && pogo_port_is_peer_ready(PA) &&
           pogo_port_is_peer_ready(PB);
}

static void check_pairs_after(int mark, uint32_t timeout_ms, const char *what)
{
    uint32_t took = 0;
    bool paired = wait_paired(mark, timeout_ms, &took);
    char msg[200];
    if (paired) {
        snprintf(msg, sizeof(msg), "%s: both faces PEER_READY again after %u ms, no re-dock", what, (unsigned)took);
    } else {
        snprintf(msg, sizeof(msg), "%s: both faces PEER_READY again within %u ms, no re-dock (NEVER PAIRED)", what,
                 (unsigned)timeout_ms);
    }
    CHECK(paired, msg);
    if (paired) {
        CHECK(stays_paired(), "link stays up through PING/PONG afterwards (no ERROR on either side)");
    }
}

/* --- pair scenarios --- */

static void pair_dock_with_healthy_wire(uint32_t offset_ms)
{
    char title[128];
    snprintf(title, sizeof(title), "docked %u ms apart, wire healthy from the start", (unsigned)offset_ms);
    pair_setup(title);
    int mark = evt_mark();
    dock_pair(offset_ms);
    check_pairs_after(mark, 2 * CYCLE_MS, "healthy wire");
    pair_teardown();
}

/* Both ports fail at least one whole cycle (wire not yet carrying data,
 * e.g. contacts still seating), then the wire starts working. */
static void pair_dock_then_wire_heals(uint32_t offset_ms)
{
    char title[128];
    snprintf(title, sizeof(title), "docked %u ms apart, wire dead until both failed a cycle", (unsigned)offset_ms);
    pair_setup(title);
    wire(false, false);
    int mark = evt_mark();
    dock_pair(offset_ms);
    uint64_t t_last_dock = mono_ms();
    CHECK(wait_event_since(mark, PA, POGO_PORT_EVENT_ERROR, CYCLE_MS, NULL) &&
              wait_event_since(mark, PB, POGO_PORT_EVENT_ERROR, CYCLE_MS, NULL),
          "both faces report DOCK_TIMEOUT while the wire is dead");
    sleep_until(t_last_dock + CYCLE_MS + 1500); /* both well into their retry loops */
    mark = evt_mark();
    wire(true, true);
    check_pairs_after(mark, 2 * CYCLE_MS, "wire healed");
    pair_teardown();
}

/* An established link loses ONE direction long enough for both sides to
 * declare LIVENESS_LOST (the receiving side first, the other side about one
 * liveness period later), then the direction comes back. */
static void pair_one_direction_dropout(bool drop_a_to_b, uint32_t restore_after_ms)
{
    char title[160];
    snprintf(title, sizeof(title), "established link, %s dropped, restored %u ms after both lost liveness",
             drop_a_to_b ? "A->B" : "B->A", (unsigned)restore_after_ms);
    pair_setup(title);
    int mark = evt_mark();
    dock_pair(0);
    uint32_t took;
    CHECK(wait_paired(mark, 1500, &took), "initial dock pairs");

    mark = evt_mark();
    wire(!drop_a_to_b, drop_a_to_b);
    evt_t ea, eb;
    bool a_lost = wait_event_since(mark, PA, POGO_PORT_EVENT_ERROR, 3 * CYCLE_MS, &ea);
    bool b_lost = wait_event_since(mark, PB, POGO_PORT_EVENT_ERROR, 3 * CYCLE_MS, &eb);
    CHECK(a_lost && b_lost && ea.data.error_code == POGO_PORT_ERR_LIVENESS_LOST &&
              eb.data.error_code == POGO_PORT_ERR_LIVENESS_LOST,
          "both faces declare LIVENESS_LOST while one direction is down");
    if (a_lost && b_lost) {
        uint64_t later = ea.t > eb.t ? ea.t : eb.t;
        uint64_t earlier = ea.t > eb.t ? eb.t : ea.t;
        printf("info: LIVENESS_LOST %u ms apart\n", (unsigned)(later - earlier));
        sleep_until(later + restore_after_ms);
    }

    mark = evt_mark();
    wire(true, true);
    check_pairs_after(mark, 2 * CYCLE_MS, "direction restored");
    pair_teardown();
}

/* One direction stays down for several full cycles: both faces keep cycling
 * at the normal rate (one ERROR per handshake+cooldown), neither reaches
 * PEER_READY on a link that cannot carry its own HELLO, nothing spins and
 * nothing floods the log; then the direction comes back and they pair. */
static void pair_one_direction_stays_down(void)
{
    pair_setup("one direction (A->B) stays down for 3 cycles, then comes back");
    int mark = evt_mark();
    dock_pair(0);
    uint32_t took;
    CHECK(wait_paired(mark, 1500, &took), "initial dock pairs");

    wire(false, true);
    mark = evt_mark();
    CHECK(wait_event_since(mark, PA, POGO_PORT_EVENT_ERROR, 3 * CYCLE_MS, NULL) &&
              wait_event_since(mark, PB, POGO_PORT_EVENT_ERROR, 3 * CYCLE_MS, NULL),
          "both faces lose the link");

    atomic_store(&s_trace, false);
    const uint32_t window = 3 * CYCLE_MS;
    mark = evt_mark();
    long log0 = log_mark();
    uint32_t cpu0 = cpu_ms();
    sleep_ms(window);
    uint32_t cpu = cpu_ms() - cpu0;
    int err_a = count_since(mark, PA, POGO_PORT_EVENT_ERROR, -1);
    int err_b = count_since(mark, PB, POGO_PORT_EVENT_ERROR, -1);
    int ready = count_since(mark, -1, POGO_PORT_EVENT_PEER_READY, -1);
    int lines = log_lines_since(log0, "(pogo_port)");
    printf("info: %u ms one-way: ERROR A=%d B=%d, PEER_READY=%d, pogo_port log lines=%d, cpu=%u ms\n",
           (unsigned)window, err_a, err_b, ready, lines, (unsigned)cpu);
    CHECK(err_a >= 2 && err_a <= 4 && err_b >= 2 && err_b <= 4,
          "each face reports about one ERROR per handshake+cooldown cycle (no faster retry loop)");
    CHECK(ready == 0, "no PEER_READY on a link that cannot carry this port's own HELLO_REQ");
    CHECK(lines <= 2 * 4 * 4, "log output bounded (at most 4 pogo_port lines per port per cycle)");
    CHECK(cpu < window / 10, "no busy loop (process CPU time < 10% of wall time)");
    atomic_store(&s_trace, true);

    mark = evt_mark();
    wire(true, true);
    check_pairs_after(mark, 2 * CYCLE_MS, "direction restored after 3 cycles");
    pair_teardown();
}

/* ===================================================================== */
/* Single-port harness: the test is the peer                             */
/* ===================================================================== */

static pogo_frame_parser_t s_peer_parser;

static void single_setup(const char *title)
{
    printf("\n-- single port: %s --\n", title);
    host_gpio_test_reset();
    host_uart_test_reset();
    evt_reset();
    pogo_frame_parser_init(&s_peer_parser);
    s_scenario_t0 = mono_ms();
    start_port(PU);
}

static void single_teardown(void)
{
    stop_port(PU, U_DETECT);
}

static bool peer_read_frame(uint32_t timeout_ms, pogo_frame_t *out)
{
    uint64_t deadline = mono_ms() + timeout_ms;
    do {
        uint8_t b;
        if (host_uart_test_take_tx_byte(U_UART, &b, 10) && pogo_frame_parser_feed(&s_peer_parser, b, out)) {
            return true;
        }
    } while (mono_ms() < deadline);
    return false;
}

static void peer_drain_tx(void)
{
    uint8_t b;
    while (host_uart_test_take_tx_byte(U_UART, &b, 20)) {
    }
    pogo_frame_parser_init(&s_peer_parser);
}

static size_t encode(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len, uint8_t *buf)
{
    return pogo_frame_encode(type, seq, payload, len, buf, POGO_FRAME_MAX_SIZE);
}

static void peer_send(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    uint8_t buf[POGO_FRAME_MAX_SIZE];
    size_t n = encode(type, seq, payload, len, buf);
    host_uart_test_push_rx(U_UART, buf, n);
}

static void peer_send_hello(uint8_t type, uint8_t seq, uint8_t version, uint8_t peer_type, uint8_t caps)
{
    pogo_peer_hello_t h = {.protocol_version = version, .peer_type = peer_type, .capabilities = caps};
    uint8_t payload[3];
    size_t len = pogo_pack_peer_hello(&h, payload, sizeof(payload));
    peer_send(type, seq, payload, (uint16_t)len);
}

static void peer_send_valid_hello(uint8_t type, uint8_t seq)
{
    peer_send_hello(type, seq, POGO_FRAME_VERSION, POGO_PEER_TYPE_MODULE, POGO_HELLO_SUPPORTED_CAPABILITIES);
}

/* Docks the port with a silent peer and waits for the first cycle's
 * DOCK_TIMEOUT; *t_cooldown is when the cooldown began. */
static bool dock_into_cooldown(uint64_t *t_cooldown)
{
    int mark = evt_mark();
    host_gpio_test_edge(U_DETECT, 0);
    evt_t e;
    bool ok = wait_event_since(mark, PU, POGO_PORT_EVENT_ATTACHED, 500, NULL) &&
              wait_event_since(mark, PU, POGO_PORT_EVENT_ERROR, CONFIG_POGO_ATTACH_SETTLE_MS + HANDSHAKE_MS + 500, &e) &&
              e.data.error_code == POGO_PORT_ERR_DOCK_TIMEOUT;
    *t_cooldown = ok ? e.t : mono_ms();
    peer_drain_tx();
    return ok;
}

/* Reattach to clear a permanent error, then complete a normal handshake. */
static void reattach_and_recover(void)
{
    int mark = evt_mark();
    host_gpio_test_edge(U_DETECT, 1);
    CHECK(wait_event_since(mark, PU, POGO_PORT_EVENT_DETACHED, 300, NULL), "physical detach observed");
    peer_drain_tx();
    mark = evt_mark();
    host_gpio_test_edge(U_DETECT, 0);
    pogo_frame_t f;
    bool req = peer_read_frame(CONFIG_POGO_ATTACH_SETTLE_MS + 500, &f) && f.type == POGO_MSG_PEER_HELLO_REQ;
    if (req) {
        peer_send_valid_hello(POGO_MSG_PEER_HELLO_RESP, f.seq);
    }
    CHECK(req && wait_event_since(mark, PU, POGO_PORT_EVENT_PEER_READY, 300, NULL),
          "after a physical re-dock a fresh handshake reaches PEER_READY");
}

/* --- single-port scenarios --- */

static void single_valid_hello_req_in_cooldown(void)
{
    single_setup("valid HELLO_REQ during the cooldown");
    uint64_t t0;
    CHECK(dock_into_cooldown(&t0), "silent peer: first cycle ends in DOCK_TIMEOUT -> cooldown");
    sleep_until(t0 + 500);
    pogo_frame_t f;
    CHECK(!peer_read_frame(200, &f), "port transmits nothing while cooling down with nothing received");

    int mark = evt_mark();
    uint64_t t_req = mono_ms();
    peer_send_valid_hello(POGO_MSG_PEER_HELLO_REQ, 0x42);
    pogo_peer_hello_t h = {0};
    bool resp = peer_read_frame(200, &f) && f.type == POGO_MSG_PEER_HELLO_RESP && f.seq == 0x42 &&
                pogo_unpack_peer_hello(f.payload, f.len, &h);
    CHECK(resp, "answered with HELLO_RESP carrying the request's seq");
    CHECK(resp && h.protocol_version == POGO_FRAME_VERSION && h.peer_type == POGO_PEER_TYPE_MODULE &&
              (h.capabilities & POGO_CAP_COLLECTOR_PROVISIONING) != 0,
          "HELLO_RESP payload is this port's own version/peer_type/capabilities");

    bool own_req = peer_read_frame(200, &f) && f.type == POGO_MSG_PEER_HELLO_REQ && f.seq == 1;
    uint32_t latency = (uint32_t)(mono_ms() - t_req);
    char msg[160];
    snprintf(msg, sizeof(msg), "starts its own handshake (HELLO_REQ seq 1) at once: %u ms after the peer's REQ, "
                               "not at the end of the cooldown",
             (unsigned)latency);
    CHECK(own_req && latency < 200, msg);

    CHECK(!pogo_port_is_peer_ready(PU) && count_since(mark, PU, POGO_PORT_EVENT_PEER_READY, -1) == 0,
          "answering the peer does NOT make this port PEER_READY");
    CHECK(pogo_port_send(PU, POGO_MSG_SET_COLLECTOR, 1, NULL, 0) == ESP_ERR_INVALID_STATE,
          "provisioning traffic still refused (pogo_port_send -> ESP_ERR_INVALID_STATE)");
    sleep_ms(CONFIG_POGO_ACK_TIMEOUT_MS / 2);
    CHECK(!pogo_port_is_peer_ready(PU), "still not PEER_READY while its own HELLO_REQ is unanswered");

    if (own_req) {
        peer_send_valid_hello(POGO_MSG_PEER_HELLO_RESP, f.seq);
    }
    evt_t e;
    CHECK(wait_event_since(mark, PU, POGO_PORT_EVENT_PEER_READY, 300, &e) && e.data.peer_type == POGO_PEER_TYPE_MODULE,
          "PEER_READY only once the peer answers this port's own HELLO_REQ");
    CHECK(count_since(mark, PU, POGO_PORT_EVENT_ERROR, -1) == 0, "no ERROR along the way");
    single_teardown();
}

static void single_valid_hello_req_then_peer_silent(void)
{
    single_setup("HELLO_REQ during the cooldown, then the peer never answers");
    uint64_t t0;
    CHECK(dock_into_cooldown(&t0), "first cycle ends in DOCK_TIMEOUT -> cooldown");
    sleep_until(t0 + 400);
    int mark = evt_mark();
    uint64_t t_req = mono_ms();
    peer_send_valid_hello(POGO_MSG_PEER_HELLO_REQ, 7);

    int reqs = 0;
    bool got_resp = false;
    uint64_t deadline = t_req + HANDSHAKE_MS + 500;
    pogo_frame_t f;
    while (mono_ms() < deadline && count_since(mark, PU, POGO_PORT_EVENT_ERROR, -1) == 0) {
        if (peer_read_frame(20, &f)) {
            reqs += (f.type == POGO_MSG_PEER_HELLO_REQ);
            got_resp |= (f.type == POGO_MSG_PEER_HELLO_RESP && f.seq == 7);
        }
    }
    evt_t e;
    bool err = wait_event_since(mark, PU, POGO_PORT_EVENT_ERROR, 100, &e);
    uint32_t window = err ? (uint32_t)(e.t - t_req) : 0;
    char msg[160];
    CHECK(got_resp, "the peer's HELLO_REQ is answered");
    snprintf(msg, sizeof(msg), "own handshake sends exactly MAX_RETRY+1 HELLO_REQs (got %d)", reqs);
    CHECK(reqs == CONFIG_POGO_MAX_RETRY + 1, msg);
    snprintf(msg, sizeof(msg), "then a normal DOCK_TIMEOUT after the full handshake window (%u ms)", (unsigned)window);
    CHECK(err && e.data.error_code == POGO_PORT_ERR_DOCK_TIMEOUT && window >= HANDSHAKE_MS - 50 &&
              window <= HANDSHAKE_MS + 300,
          msg);
    CHECK(count_since(mark, PU, POGO_PORT_EVENT_PEER_READY, -1) == 0, "never PEER_READY");

    peer_drain_tx();
    CHECK(!peer_read_frame((uint32_t)COOLDOWN_MS - 250, &f), "and a full, normal cooldown follows (no transmission)");
    bool next = peer_read_frame(600, &f) && f.type == POGO_MSG_PEER_HELLO_REQ && f.seq == 1;
    uint32_t gap = err ? (uint32_t)(mono_ms() - e.t) : 0;
    snprintf(msg, sizeof(msg), "next cycle's HELLO_REQ %u ms after that DOCK_TIMEOUT (cooldown %u ms)", (unsigned)gap,
             (unsigned)COOLDOWN_MS);
    CHECK(next && gap >= COOLDOWN_MS - 50 && gap <= COOLDOWN_MS + 300, msg);
    single_teardown();
}

static void single_ignored_input_in_cooldown(void)
{
    single_setup("corrupted/irrelevant frames during the cooldown are ignored");
    uint64_t t0;
    CHECK(dock_into_cooldown(&t0), "first cycle ends in DOCK_TIMEOUT -> cooldown");
    sleep_until(t0 + 300);
    int mark = evt_mark();
    uint8_t buf[POGO_FRAME_MAX_SIZE];
    uint8_t hello[3] = {POGO_FRAME_VERSION, POGO_PEER_TYPE_MODULE, POGO_HELLO_SUPPORTED_CAPABILITIES};
    size_t n;

    n = encode(POGO_MSG_PEER_HELLO_REQ, 1, hello, 3, buf);
    buf[n - 1] ^= 0x5A; /* CRC broken */
    host_uart_test_push_rx(U_UART, buf, n);
    n = encode(POGO_MSG_PEER_HELLO_REQ, 2, hello, 3, buf);
    buf[1] = POGO_FRAME_VERSION + 1; /* frame-level version byte wrong: parser drops it */
    host_uart_test_push_rx(U_UART, buf, n);
    peer_send(POGO_MSG_PEER_HELLO_REQ, 3, hello, 1); /* 1-byte payload */
    uint8_t long_hello[4] = {POGO_FRAME_VERSION, POGO_PEER_TYPE_MODULE, POGO_HELLO_SUPPORTED_CAPABILITIES, 0};
    peer_send(POGO_MSG_PEER_HELLO_REQ, 4, long_hello, 4); /* 4-byte payload */
    peer_send_valid_hello(POGO_MSG_PEER_HELLO_RESP, 1); /* stale RESP, the seq the next cycle will use */
    peer_send_valid_hello(POGO_MSG_PEER_HELLO_RESP, 4); /* stale RESP to the last cycle's last REQ */
    peer_send(POGO_MSG_PING, 9, NULL, 0);
    for (int i = 0; i < 64; i++) {
        buf[i] = (uint8_t)(i * 37 + 11);
    }
    host_uart_test_push_rx(U_UART, buf, 64);

    pogo_frame_t f;
    uint64_t now = mono_ms();
    uint32_t rest = (uint32_t)(t0 + COOLDOWN_MS - 150 - now);
    CHECK(!peer_read_frame(rest, &f), "nothing is answered and no handshake starts early");
    CHECK(evt_mark() == mark, "no event at all (no ERROR, no PEER_READY)");
    CHECK(!pogo_port_is_peer_ready(PU), "not PEER_READY");

    bool next = peer_read_frame(600, &f) && f.type == POGO_MSG_PEER_HELLO_REQ && f.seq == 1;
    uint32_t gap = (uint32_t)(mono_ms() - t0);
    char msg[160];
    snprintf(msg, sizeof(msg), "fresh HELLO_REQ only after the full cooldown (%u ms after DOCK_TIMEOUT)", (unsigned)gap);
    CHECK(next && gap >= COOLDOWN_MS - 50, msg);
    sleep_ms(150);
    CHECK(!pogo_port_is_peer_ready(PU) && count_since(mark, PU, POGO_PORT_EVENT_PEER_READY, -1) == 0,
          "the stale HELLO_RESP (seq 1) heard during the cooldown does not complete the new handshake");
    if (next) {
        peer_send_valid_hello(POGO_MSG_PEER_HELLO_RESP, f.seq);
    }
    CHECK(wait_event_since(mark, PU, POGO_PORT_EVENT_PEER_READY, 300, NULL), "a real answer still completes it");
    single_teardown();
}

typedef struct {
    const char *name;
    uint8_t version;
    uint8_t peer_type;
    uint8_t caps;
    bool legacy_2byte;
    pogo_port_error_t want;
    bool full_wait;
} bad_hello_t;

static void single_bad_hello_req_in_cooldown(const bad_hello_t *c)
{
    char title[128];
    snprintf(title, sizeof(title), "HELLO_REQ with %s during the cooldown", c->name);
    single_setup(title);
    uint64_t t0;
    CHECK(dock_into_cooldown(&t0), "first cycle ends in DOCK_TIMEOUT -> cooldown");
    sleep_until(t0 + 300);
    int mark = evt_mark();
    if (c->legacy_2byte) {
        uint8_t legacy[2] = {c->version, c->peer_type};
        peer_send(POGO_MSG_PEER_HELLO_REQ, 5, legacy, 2);
    } else {
        peer_send_hello(POGO_MSG_PEER_HELLO_REQ, 5, c->version, c->peer_type, c->caps);
    }
    evt_t e;
    char msg[160];
    snprintf(msg, sizeof(msg), "rejected by the same HELLO validation as in a handshake -> ERROR %s", err_name(c->want));
    CHECK(wait_event_since(mark, PU, POGO_PORT_EVENT_ERROR, 300, &e) && e.data.error_code == c->want, msg);

    pogo_frame_t f;
    uint32_t quiet = c->full_wait ? (uint32_t)(t0 + COOLDOWN_MS + HANDSHAKE_MS - mono_ms()) : 600;
    snprintf(msg, sizeof(msg), "no HELLO_RESP and no automatic retry (watched %u ms%s)", (unsigned)quiet,
             c->full_wait ? ", past where the cooldown would have retried" : "");
    CHECK(!peer_read_frame(quiet, &f), msg);
    CHECK(pogo_port_is_attached(PU) && !pogo_port_is_peer_ready(PU) &&
              count_since(mark, PU, POGO_PORT_EVENT_PEER_READY, -1) == 0,
          "stays in ERROR (attached, not PEER_READY) until a physical re-dock");
    reattach_and_recover();
    single_teardown();
}

/* --- garbage feeder: a peer streaming junk at line rate --- */
static atomic_bool s_feed_stop;

static void *garbage_feeder(void *arg)
{
    (void)arg;
    uint32_t x = 0x12345678u;
    uint8_t corrupt_hello[POGO_FRAME_MAX_SIZE];
    uint8_t hello[3] = {POGO_FRAME_VERSION, POGO_PEER_TYPE_MODULE, POGO_HELLO_SUPPORTED_CAPABILITIES};
    size_t hn = encode(POGO_MSG_PEER_HELLO_REQ, 1, hello, 3, corrupt_hello);
    corrupt_hello[hn - 2] ^= 0xFF;
    int round = 0;
    while (!atomic_load(&s_feed_stop)) {
        uint8_t chunk[12]; /* ~115200 baud = ~11.5 bytes per ms */
        for (size_t i = 0; i < sizeof(chunk); i++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            chunk[i] = (uint8_t)x;
        }
        host_uart_test_push_rx(U_UART, chunk, sizeof(chunk));
        if (++round % 10 == 0) {
            host_uart_test_push_rx(U_UART, corrupt_hello, hn); /* a HELLO_REQ with a bad CRC every 10 ms */
        }
        sleep_ms(1);
    }
    return NULL;
}

static pthread_t start_feeder(void)
{
    pthread_t t;
    atomic_store(&s_feed_stop, false);
    pthread_create(&t, NULL, garbage_feeder, NULL);
    return t;
}

static void stop_feeder(pthread_t t)
{
    atomic_store(&s_feed_stop, true);
    pthread_join(t, NULL);
}

static void single_detach_during_cooldown(void)
{
    char msg[160];
    pogo_frame_t f;
    evt_t e;

    single_setup("DETACHED during the cooldown (idle line)");
    uint64_t t0;
    CHECK(dock_into_cooldown(&t0), "first cycle ends in DOCK_TIMEOUT -> cooldown");
    sleep_until(t0 + 500);
    int mark = evt_mark();
    uint64_t t_edge = mono_ms();
    host_gpio_test_edge(U_DETECT, 1);
    bool det = wait_event_since(mark, PU, POGO_PORT_EVENT_DETACHED, 500, &e);
    snprintf(msg, sizeof(msg), "DETACHED %u ms after the edge (debounce %u ms)", det ? (unsigned)(e.t - t_edge) : 0,
             (unsigned)DEBOUNCE_MS);
    CHECK(det && e.t - t_edge <= DEBOUNCE_MS + 60, msg);
    CHECK(!pogo_port_is_attached(PU), "port reads detached");
    CHECK(!peer_read_frame(COOLDOWN_MS + 300, &f), "the pending retry is cancelled (no HELLO_REQ afterwards)");
    single_teardown();

    single_setup("DETACHED during the cooldown while the line streams garbage");
    CHECK(dock_into_cooldown(&t0), "first cycle ends in DOCK_TIMEOUT -> cooldown");
    pthread_t feeder = start_feeder();
    sleep_until(t0 + 800);
    mark = evt_mark();
    t_edge = mono_ms();
    host_gpio_test_edge(U_DETECT, 1);
    det = wait_event_since(mark, PU, POGO_PORT_EVENT_DETACHED, 500, &e);
    snprintf(msg, sizeof(msg), "DETACHED %u ms after the edge, despite continuous RX",
             det ? (unsigned)(e.t - t_edge) : 0);
    CHECK(det && e.t - t_edge <= DEBOUNCE_MS + 60, msg);
    stop_feeder(feeder);
    peer_drain_tx();
    CHECK(!peer_read_frame(COOLDOWN_MS + 300, &f), "no HELLO_REQ afterwards");
    single_teardown();

    single_setup("DETACHED right after a HELLO_REQ answered during the cooldown");
    CHECK(dock_into_cooldown(&t0), "first cycle ends in DOCK_TIMEOUT -> cooldown");
    sleep_until(t0 + 500);
    peer_send_valid_hello(POGO_MSG_PEER_HELLO_REQ, 3);
    bool resp = peer_read_frame(200, &f) && f.type == POGO_MSG_PEER_HELLO_RESP;
    bool req = resp && peer_read_frame(200, &f) && f.type == POGO_MSG_PEER_HELLO_REQ;
    CHECK(resp && req, "answered and started its own handshake");
    mark = evt_mark();
    t_edge = mono_ms();
    host_gpio_test_edge(U_DETECT, 1);
    det = wait_event_since(mark, PU, POGO_PORT_EVENT_DETACHED, 500, &e);
    snprintf(msg, sizeof(msg), "DETACHED %u ms after the edge, mid-handshake", det ? (unsigned)(e.t - t_edge) : 0);
    CHECK(det && e.t - t_edge <= DEBOUNCE_MS + 60, msg);
    peer_drain_tx();
    CHECK(!peer_read_frame(HANDSHAKE_MS + 300, &f), "no further HELLO_REQ after the detach");
    CHECK(count_since(mark, PU, POGO_PORT_EVENT_ERROR, -1) == 0 &&
              count_since(mark, PU, POGO_PORT_EVENT_PEER_READY, -1) == 0,
          "no ERROR/PEER_READY after the detach");
    single_teardown();
}

static void single_garbage_through_cooldown(void)
{
    single_setup("continuous garbage (incl. bad-CRC HELLO_REQs) through the whole cooldown");
    uint64_t t0;
    CHECK(dock_into_cooldown(&t0), "first cycle ends in DOCK_TIMEOUT -> cooldown");
    int mark = evt_mark();
    long log0 = log_mark();
    uint32_t cpu0 = cpu_ms();
    uint64_t w0 = mono_ms();
    pthread_t feeder = start_feeder();
    pogo_frame_t f;
    uint32_t rest = (uint32_t)(t0 + COOLDOWN_MS - 150 - mono_ms());
    bool tx = peer_read_frame(rest, &f);
    stop_feeder(feeder);
    uint32_t cpu = cpu_ms() - cpu0;
    uint32_t wall = (uint32_t)(mono_ms() - w0);
    int lines = log_lines_since(log0, "(pogo_port)");
    printf("info: %u ms of garbage: cpu=%u ms (feeder thread included), pogo_port log lines=%d\n", (unsigned)wall,
           (unsigned)cpu, lines);
    CHECK(!tx, "nothing transmitted in response to garbage");
    CHECK(evt_mark() == mark, "no event");
    CHECK(lines == 0, "no log line per garbage byte or per corrupted frame");
    CHECK(cpu < wall / 4, "no busy loop (process CPU time < 25% of wall time while streaming)");
    bool next = peer_read_frame(600, &f) && f.type == POGO_MSG_PEER_HELLO_REQ;
    uint32_t gap = (uint32_t)(mono_ms() - t0);
    char msg[160];
    snprintf(msg, sizeof(msg), "cooldown length unchanged (next HELLO_REQ %u ms after DOCK_TIMEOUT)", (unsigned)gap);
    CHECK(next && gap >= COOLDOWN_MS - 50 && gap <= COOLDOWN_MS + 300, msg);
    single_teardown();
}

static void single_silent_peer_cycles(void)
{
    single_setup("peer stays silent: two full retry cycles");
    int mark = evt_mark();
    host_gpio_test_edge(U_DETECT, 0);
    evt_t e1;
    CHECK(wait_event_since(mark, PU, POGO_PORT_EVENT_ERROR, CONFIG_POGO_ATTACH_SETTLE_MS + HANDSHAKE_MS + 500, &e1),
          "first DOCK_TIMEOUT");
    peer_drain_tx();
    long log0 = log_mark();
    uint32_t cpu0 = cpu_ms();
    int mark1 = evt_mark();
    int reqs = 0;
    uint64_t deadline = e1.t + 2 * CYCLE_MS + 600;
    pogo_frame_t f;
    while (mono_ms() < deadline && count_since(mark1, PU, POGO_PORT_EVENT_ERROR, -1) < 2) {
        if (peer_read_frame(20, &f) && f.type == POGO_MSG_PEER_HELLO_REQ) {
            reqs++;
        }
    }
    uint32_t cpu = cpu_ms() - cpu0;
    int lines = log_lines_since(log0, "(pogo_port)");
    pthread_mutex_lock(&s_evt_mutex);
    uint64_t t_err[2] = {0, 0};
    int k = 0;
    for (int i = mark1; i < s_nevts && k < 2; i++) {
        if (s_evts[i].ev == POGO_PORT_EVENT_ERROR) {
            t_err[k++] = s_evts[i].t;
        }
    }
    pthread_mutex_unlock(&s_evt_mutex);
    uint32_t c1 = k >= 1 ? (uint32_t)(t_err[0] - e1.t) : 0;
    uint32_t c2 = k >= 2 ? (uint32_t)(t_err[1] - t_err[0]) : 0;
    uint32_t wall = k >= 2 ? (uint32_t)(t_err[1] - e1.t) : 1;
    printf("info: cycles %u ms and %u ms (expected %u), HELLO_REQs=%d, log lines=%d, cpu=%u ms\n", (unsigned)c1,
           (unsigned)c2, (unsigned)CYCLE_MS, reqs, lines, (unsigned)cpu);
    CHECK(k == 2 && c1 >= CYCLE_MS - 50 && c1 <= CYCLE_MS + 300 && c2 >= CYCLE_MS - 50 && c2 <= CYCLE_MS + 300,
          "retry period unchanged: one DOCK_TIMEOUT per handshake+cooldown");
    CHECK(reqs == 2 * (CONFIG_POGO_MAX_RETRY + 1), "exactly MAX_RETRY+1 HELLO_REQs per cycle");
    CHECK(lines <= 2 * 4, "at most 4 pogo_port log lines per cycle");
    CHECK(cpu < wall / 20, "idle retry loop uses < 5% CPU");
    CHECK(count_since(mark, PU, POGO_PORT_EVENT_PEER_READY, -1) == 0, "never PEER_READY");
    single_teardown();
}

/* ===================================================================== */

static const char *s_filter;

static bool want(const char *name)
{
    return s_filter == NULL || strstr(name, s_filter) != NULL;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    s_filter = argc > 1 ? argv[1] : NULL;
    s_log = tmpfile();
    if (s_log != NULL) {
        dup2(fileno(s_log), 2);
    }
    uint64_t start = mono_ms();

    check_timing_is_kconfig_default();

    if (want("single")) {
        single_valid_hello_req_in_cooldown();
        single_valid_hello_req_then_peer_silent();
        single_ignored_input_in_cooldown();
        const bad_hello_t bad[] = {
            {"protocol_version+1", POGO_FRAME_VERSION + 1, POGO_PEER_TYPE_MODULE, POGO_HELLO_SUPPORTED_CAPABILITIES,
             false, POGO_PORT_ERR_PROTOCOL_VERSION, true},
            {"no COLLECTOR_PROVISIONING capability", POGO_FRAME_VERSION, POGO_PEER_TYPE_MODULE, 0, false,
             POGO_PORT_ERR_CAPABILITY_MISMATCH, false},
            {"a legacy 2-byte payload", POGO_FRAME_VERSION, POGO_PEER_TYPE_MODULE, 0, true,
             POGO_PORT_ERR_CAPABILITY_MISMATCH, false},
            {"an invalid peer_type", POGO_FRAME_VERSION, 0x7F, POGO_HELLO_SUPPORTED_CAPABILITIES, false,
             POGO_PORT_ERR_PEER_TYPE_CONFLICT, false},
            {"peer_type POWER_BASE against BASE_ID high", POGO_FRAME_VERSION, POGO_PEER_TYPE_POWER_BASE,
             POGO_HELLO_SUPPORTED_CAPABILITIES, false, POGO_PORT_ERR_PEER_TYPE_CONFLICT, false},
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            single_bad_hello_req_in_cooldown(&bad[i]);
        }
        single_detach_during_cooldown();
        single_garbage_through_cooldown();
        single_silent_peer_cycles();
    }

    if (want("pair")) {
        pair_dock_with_healthy_wire(0);
        pair_dock_with_healthy_wire(1000);
        pair_dock_with_healthy_wire(2000);
        pair_dock_then_wire_heals(0);
        pair_dock_then_wire_heals(1000);
        pair_dock_then_wire_heals(2000);
        pair_one_direction_dropout(true, 1000);
        pair_one_direction_dropout(false, 1000);
        pair_one_direction_stays_down();
    }

    printf("\n%d checks, %d failures (%u s)\n", g_checks, g_failures, (unsigned)((mono_ms() - start) / 1000));
    if (getenv("POGO_TEST_SHOW_LOG") != NULL && s_log != NULL) {
        fflush(stderr);
        rewind(s_log);
        char line[512];
        while (fgets(line, sizeof(line), s_log) != NULL) {
            fputs(line, stdout);
        }
    }
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
