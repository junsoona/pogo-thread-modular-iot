#include "pogo_port/pogo_port.h"
#include "pogo_port/pogo_port_cleanup.h"

#include <string.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "pogo_port";

#define PORT_TASK_STACK 4096
#define PORT_TASK_PRIO   5
#define DETECT_QUEUE_LEN 8
#define GPIO_ISR_FLAGS   0
/* WAIT_ATTACH/ERROR are intentionally quiet steady states. Emit a sparse
 * heartbeat so a monitor attached after boot can distinguish a live port
 * task from a stalled one without flooding the console. */
#define PORT_STATUS_LOG_INTERVAL_MS 15000

typedef enum {
    PORT_STATE_DISABLED = 0,
    PORT_STATE_WAIT_ATTACH,
    PORT_STATE_ATTACH_SETTLE,   /* DETECT_IN just debounced attached; short settle before the first handshake */
    PORT_STATE_HANDSHAKE,
    PORT_STATE_PEER_READY,
    PORT_STATE_RECOVER_COOLDOWN, /* AUTO mode only: a transient error (DOCK_TIMEOUT/LIVENESS_LOST) while DETECT_IN
                                     still reads attached -- waiting out CONFIG_POGO_HOTSWAP_COOLDOWN_MS before a
                                     fresh handshake cycle, without requiring a new DETECT_IN edge */
    PORT_STATE_ERROR,           /* permanent until the next DETACHED: PEER_TYPE_CONFLICT/PROTOCOL_VERSION/
                                    CAPABILITY_MISMATCH, or (dev compat mode only) a transient error already
                                    handled by that mode's own inline retry below */
} port_internal_state_t;

typedef struct {
    TickType_t tick;
    int level;
} detect_edge_evt_t;

typedef struct {
    pogo_port_id_t       id;
    pogo_port_config_t   cfg;
    pogo_port_event_cb_t cb;
    void                 *user_ctx;
    pogo_peer_type_t      own_peer_type;

    QueueHandle_t         detect_queue;
    TaskHandle_t          task;

    /* Debounce bookkeeping (owned by the port task only). */
    int       committed_level;   /* 1 = idle/high (detached), 0 = asserted/low (attached) */
    bool      pending_valid;
    int       pending_level;
    TickType_t pending_since;
    TickType_t last_reconcile;   /* last time process_debounce() direct-read DETECT_IN as a missed-ISR backstop */

    /* Handshake bookkeeping. */
    port_internal_state_t state;
    TickType_t state_deadline;   /* PORT_STATE_ATTACH_SETTLE / PORT_STATE_RECOVER_COOLDOWN only */
    pogo_frame_parser_t   parser;
    uint8_t   hello_seq;
    int       hello_retries;
    TickType_t hello_deadline;
    pogo_peer_type_t peer_type;

    /*
     * Idle PING/PONG liveness (section 9 of the follow-up spec), owned
     * entirely by this port's own task -- the same task that already owns
     * every UART read via process_peer_ready(), so a periodic liveness PING
     * and a higher layer's provisioning frame wait (pogo_port_send() +
     * FRAME_RECEIVED) never race for who gets to read the reply: there is
     * exactly one reader. PING/PONG frames themselves are consumed HERE and
     * never forwarded up as FRAME_RECEIVED (see process_peer_ready()) -- to
     * a higher layer, this component's PEER_READY state is either "still
     * ready" or an ERROR event, PING/PONG is invisible plumbing.
     */
    TickType_t last_activity;     /* last time ANY byte was successfully parsed while PEER_READY */
    bool       ping_outstanding;
    uint8_t    ping_seq;
    TickType_t ping_deadline;
    int        liveness_fail_count;

    /* Rate-limited task-alive/status diagnostic. */
    TickType_t last_status_log;

    bool inited;
    bool enabled; /* true from a successful pogo_port_enable() until pogo_port_disable() -- see that function's doc comment on idempotency */
    uint32_t detect_isr_drops; /* xQueueSendFromISR() failures in detect_isr_handler() -- diagnostic only, see that function's comment */
} pogo_port_ctx_t;

static pogo_port_ctx_t s_ports[POGO_PORT_ID_COUNT];
static bool s_isr_service_installed = false;

static void notify(pogo_port_ctx_t *p, pogo_port_event_t ev, const pogo_port_event_data_t *data)
{
    if (p->cb == NULL) {
        return;
    }
    pogo_port_event_data_t empty = {0};
    p->cb(p->id, ev, data ? data : &empty, p->user_ctx);
}

static void IRAM_ATTR detect_isr_handler(void *arg)
{
    pogo_port_ctx_t *p = (pogo_port_ctx_t *)arg;
    detect_edge_evt_t evt;
    evt.tick = xTaskGetTickCountFromISR();
    evt.level = gpio_get_level((gpio_num_t)p->cfg.detect_in_gpio);
    BaseType_t hpw = pdFALSE;
    if (xQueueSendFromISR(p->detect_queue, &evt, &hpw) != pdTRUE) {
        /* Queue full (DETECT_QUEUE_LEN edges without the port task getting
         * a chance to drain any of them -- should not happen in practice,
         * process_debounce() drains unconditionally every loop iteration).
         * Nothing safe to do from ISR context beyond counting it -- surfaced
         * the next time the port task runs process_debounce() (diagnostic
         * only; nothing to log-flood from an ISR either way, no state edge
         * is guaranteed to have been lost since the queue already held at
         * least DETECT_QUEUE_LEN more-recent edges). */
        p->detect_isr_drops++;
    }
    if (hpw) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t set_detect_out(pogo_port_ctx_t *p, bool asserted)
{
    if (p->cfg.detect_out_gpio < 0) {
        return ESP_OK;
    }
    /* Open-drain: level 0 drives Low (asserted, "I am here"); level 1
     * releases the line so the external pull-up takes it High. */
    esp_err_t err = gpio_set_level((gpio_num_t)p->cfg.detect_out_gpio, asserted ? 0 : 1);
    if (err != ESP_OK) {
        /* Logged, not escalated to a hard failure here: gpio_set_level()
         * failing on a pin whose gpio_config() just succeeded moments
         * earlier would mean a driver-internal problem, not a config
         * mistake this code can meaningfully recover from -- see this
         * function's callers for how each one decides whether that is
         * fatal for ITS OWN operation. */
        ESP_LOGW(TAG, "port %d: gpio_set_level(DETECT_OUT=%d, %s) failed: %s", p->id, p->cfg.detect_out_gpio,
                 asserted ? "asserted" : "released", esp_err_to_name(err));
    }
    return err;
}

/*
 * Adapters binding pogo_port_run_cleanup_ops()'s generic `pogo_port_cleanup_op_t`
 * signature to this component's real hardware calls -- used by BOTH
 * pogo_port_enable()'s failure-unwind path and pogo_port_disable(), so the
 * "attempt everything, keep the first error" policy in pogo_port_cleanup.c
 * is exercised by real hardware calls here and directly, with fault
 * injection, by tools/test_pogo_port_cleanup_ops.c.
 */
static esp_err_t cleanup_op_isr_remove(void *ctx)
{
    pogo_port_ctx_t *p = (pogo_port_ctx_t *)ctx;
    return gpio_isr_handler_remove((gpio_num_t)p->cfg.detect_in_gpio);
}

static esp_err_t cleanup_op_detect_release(void *ctx)
{
    pogo_port_ctx_t *p = (pogo_port_ctx_t *)ctx;
    return set_detect_out(p, false);
}

static esp_err_t cleanup_op_uart_delete(void *ctx)
{
    pogo_port_ctx_t *p = (pogo_port_ctx_t *)ctx;
    return uart_driver_delete((uart_port_t)p->cfg.uart_num);
}

static bool base_id_present(const pogo_port_ctx_t *p)
{
    return p->cfg.base_id_gpio >= 0;
}

static bool base_id_reads_power_base(const pogo_port_ctx_t *p)
{
    /* Active-low: a power base drives this Low; a normal module does not
     * drive it at all (external pull-up keeps it High). */
    return base_id_present(p) && gpio_get_level((gpio_num_t)p->cfg.base_id_gpio) == 0;
}

static esp_err_t pogo_transport_send_on(pogo_port_ctx_t *p, uint8_t type, uint8_t seq, const uint8_t *payload,
                                         uint16_t len)
{
    uint8_t buf[POGO_FRAME_MAX_SIZE];
    size_t n = pogo_frame_encode(type, seq, payload, len, buf, sizeof(buf));
    if (n == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int written = uart_write_bytes((uart_port_t)p->cfg.uart_num, (const char *)buf, n);
    return (written >= 0 && (size_t)written == n) ? ESP_OK : ESP_FAIL;
}

/* Consecutive idle-PING failures tolerated before liveness is declared lost
 * (section 9) -- reuses CONFIG_POGO_MAX_RETRY as the same bounded-retry
 * budget every other request/response exchange in this codebase already
 * uses, rather than inventing a second retry-count knob. */
#define LIVENESS_MAX_CONSECUTIVE_FAILURES CONFIG_POGO_MAX_RETRY

static void reset_liveness(pogo_port_ctx_t *p)
{
    p->last_activity = xTaskGetTickCount();
    p->ping_outstanding = false;
    p->ping_seq = 0;
    p->liveness_fail_count = 0;
}

static void enter_wait_attach(pogo_port_ctx_t *p)
{
    p->state = PORT_STATE_WAIT_ATTACH;
    p->peer_type = POGO_PEER_TYPE_UNKNOWN;
    pogo_frame_parser_init(&p->parser);
    reset_liveness(p);
    p->last_status_log = xTaskGetTickCount();
    ESP_LOGI(TAG, "port %d: WAIT_ATTACH (DETECT_IN=%d)", p->id,
             p->cfg.detect_in_gpio >= 0 ? gpio_get_level((gpio_num_t)p->cfg.detect_in_gpio) : -1);
}

/* Real-DETECT_IN attach path only (dev compat mode's port_task() startup
 * calls begin_handshake() directly -- there is no physical contact to
 * settle). A short, bounded, explicit wait (CONFIG_POGO_ATTACH_SETTLE_MS,
 * not a busy loop -- see process_attach_settle()) before the first HELLO_REQ,
 * so a dock motion where DETECT_IN closes slightly before UART/GND fully
 * seat does not race the handshake against still-settling contacts. A detach
 * during this window is handled the same way as during HANDSHAKE/
 * RECOVER_COOLDOWN: process_debounce() (called every port_task loop
 * iteration, unconditionally, before the state switch) already transitions
 * straight to WAIT_ATTACH on its own, which simply pre-empts whatever this
 * state was about to do next. */
static void enter_attach_settle(pogo_port_ctx_t *p)
{
    p->state = PORT_STATE_ATTACH_SETTLE;
    p->state_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CONFIG_POGO_ATTACH_SETTLE_MS);
}

static const char *port_state_to_str(port_internal_state_t state)
{
    switch (state) {
    case PORT_STATE_WAIT_ATTACH:
        return "WAIT_ATTACH";
    case PORT_STATE_ATTACH_SETTLE:
        return "ATTACH_SETTLE";
    case PORT_STATE_HANDSHAKE:
        return "HANDSHAKE";
    case PORT_STATE_PEER_READY:
        return "PEER_READY";
    case PORT_STATE_RECOVER_COOLDOWN:
        return "RECOVER_COOLDOWN";
    case PORT_STATE_ERROR:
        return "ERROR";
    case PORT_STATE_DISABLED:
    default:
        return "DISABLED";
    }
}

static void maybe_log_idle_status(pogo_port_ctx_t *p)
{
    if (p->state != PORT_STATE_WAIT_ATTACH && p->state != PORT_STATE_ERROR &&
        p->state != PORT_STATE_RECOVER_COOLDOWN) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if ((now - p->last_status_log) < pdMS_TO_TICKS(PORT_STATUS_LOG_INTERVAL_MS)) {
        return;
    }
    p->last_status_log = now;

    int detect_level =
        p->cfg.detect_in_gpio >= 0 ? gpio_get_level((gpio_num_t)p->cfg.detect_in_gpio) : -1;
    uint32_t uptime_ms = (uint32_t)(now * portTICK_PERIOD_MS);
    ESP_LOGI(TAG,
             "port %d: alive state=%s DETECT_IN=%d own_peer_type=%d uptime=%lums detect_isr_drops=%lu",
             p->id, port_state_to_str(p->state), detect_level, (int)p->own_peer_type,
             (unsigned long)uptime_ms, (unsigned long)p->detect_isr_drops);
}

static void send_hello_req(pogo_port_ctx_t *p, uint8_t seq)
{
    pogo_peer_hello_t hello = {.protocol_version = POGO_FRAME_VERSION,
                                .peer_type = (uint8_t)p->own_peer_type,
                                .capabilities = POGO_HELLO_SUPPORTED_CAPABILITIES};
    uint8_t payload[3];
    size_t len = pogo_pack_peer_hello(&hello, payload, sizeof(payload));
    pogo_transport_send_on(p, POGO_MSG_PEER_HELLO_REQ, seq, payload, len);
}

static void begin_handshake(pogo_port_ctx_t *p)
{
    /* Clears any bytes/partial frame left over from a previous failed
     * session (stale PING/PONG, a truncated frame) before this new
     * handshake's parser starts listening -- otherwise a leftover byte
     * could desync the very first HELLO_REQ/RESP exchange of the new
     * cycle. Safe to call unconditionally: uart_flush_input() is a no-op
     * on an already-empty RX buffer, and the UART driver is always
     * installed by the time begin_handshake() can ever run (from
     * pogo_port_enable() onward). */
    uart_flush_input((uart_port_t)p->cfg.uart_num);
    p->state = PORT_STATE_HANDSHAKE;
    pogo_frame_parser_init(&p->parser);
    p->hello_seq = 1;
    p->hello_retries = 0;
    send_hello_req(p, p->hello_seq);
    p->hello_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CONFIG_POGO_ACK_TIMEOUT_MS);
}

/* AUTO mode only (detect_in_gpio >= 0): DOCK_TIMEOUT/LIVENESS_LOST are
 * transient -- if DETECT_IN still reads attached, retry a fresh handshake
 * cycle automatically after a bounded cooldown instead of requiring a new
 * DETECT_IN edge (a real dock where DETECT closes before UART/GND would
 * otherwise wedge in ERROR forever, since no further edge is coming). See
 * this file's own docs/pogo-interface.md section and CONFIG_POGO_HOTSWAP_COOLDOWN_MS's
 * Kconfig help text. Configuration/compatibility errors (PEER_TYPE_CONFLICT,
 * PROTOCOL_VERSION_ERROR, CAPABILITY_MISMATCH) never call this -- they stay
 * in PORT_STATE_ERROR until the next physical detach, unchanged. */
static void enter_recover_cooldown(pogo_port_ctx_t *p)
{
    p->state = PORT_STATE_RECOVER_COOLDOWN;
    p->state_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CONFIG_POGO_HOTSWAP_COOLDOWN_MS);
    p->last_status_log = xTaskGetTickCount();
    ESP_LOGI(TAG, "port %d: transient error, DETECT_IN still attached -- cooldown %dms before retrying handshake",
             p->id, CONFIG_POGO_HOTSWAP_COOLDOWN_MS);
}

static void handle_dock_timeout_or_silent_power_base(pogo_port_ctx_t *p)
{
    if (base_id_reads_power_base(p)) {
        /* A passive power base need not speak POGO UART at all -- BASE_ID
         * hardware alone is authoritative for this case. */
        p->peer_type = POGO_PEER_TYPE_POWER_BASE;
        p->state = PORT_STATE_PEER_READY;
        reset_liveness(p);
        pogo_port_event_data_t data = {.peer_type = p->peer_type};
        ESP_LOGI(TAG, "port %d: no HELLO_RESP reply, but BASE_ID reads POWER_BASE -> PEER_READY", p->id);
        notify(p, POGO_PORT_EVENT_PEER_READY, &data);
        return;
    }

    p->state = PORT_STATE_ERROR;
    pogo_port_event_data_t data = {.error_code = POGO_PORT_ERR_DOCK_TIMEOUT};
    ESP_LOGW(TAG, "port %d: DOCK_TIMEOUT (no HELLO_RESP after %d retries)", p->id, CONFIG_POGO_MAX_RETRY);
    notify(p, POGO_PORT_EVENT_ERROR, &data);

    if (p->cfg.detect_in_gpio < 0) {
        /* Dev compat mode: there is no DETECT hardware to wait on, so
         * staying in ERROR forever would wedge this port permanently the
         * first time a retry cycle fails (e.g. the peer board hasn't
         * finished booting yet). Recover automatically: wait one PING
         * interval (bounded, not a hot loop) and start a fresh handshake
         * cycle, still capped at CONFIG_POGO_MAX_RETRY per cycle. */
        vTaskDelay(pdMS_TO_TICKS(CONFIG_POGO_PING_INTERVAL_MS));
        ESP_LOGI(TAG, "port %d: dev compat mode -- retrying handshake cycle", p->id);
        begin_handshake(p);
    } else {
        /* AUTO mode (real DETECT_IN): DOCK_TIMEOUT is transient -- retry
         * automatically after a cooldown as long as DETECT_IN still reads
         * attached (see enter_recover_cooldown()'s doc comment). */
        enter_recover_cooldown(p);
    }
}

/* Mirrors handle_dock_timeout_or_silent_power_base()'s recovery policy, for
 * the idle-liveness-lost case instead of the initial-handshake one (section
 * 9): fire the ERROR event either way, then auto-retry a fresh handshake --
 * immediately (dev compat mode, no DETECT_IN) or after a cooldown as long as
 * DETECT_IN still reads attached (AUTO mode -- see enter_recover_cooldown()).
 * LIVENESS_LOST is transient by definition: PING/PONG failing while the wire
 * still reads "attached" means the peer stopped answering, which a fresh
 * handshake (possibly after the peer itself reboots/recovers) can resolve
 * without requiring a physical detach/reattach. */
static void handle_liveness_lost(pogo_port_ctx_t *p)
{
    p->state = PORT_STATE_ERROR;
    pogo_port_event_data_t data = {.error_code = POGO_PORT_ERR_LIVENESS_LOST};
    ESP_LOGW(TAG, "port %d: LIVENESS_LOST (idle PING/PONG failed %d times in a row)", p->id,
             p->liveness_fail_count);
    notify(p, POGO_PORT_EVENT_ERROR, &data);

    if (p->cfg.detect_in_gpio < 0) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_POGO_PING_INTERVAL_MS));
        ESP_LOGI(TAG, "port %d: dev compat mode -- retrying handshake cycle after liveness loss", p->id);
        begin_handshake(p);
    } else {
        enter_recover_cooldown(p);
    }
}

/* Validates a received hello (REQ or RESP) payload against protocol version
 * and peer_type, cross-checking peer_type against BASE_ID where present.
 * Returns true (and fills *out_peer_type) if valid; on failure it has
 * already moved the port to ERROR and fired POGO_PORT_EVENT_ERROR. */
static bool validate_incoming_hello(pogo_port_ctx_t *p, const pogo_frame_t *frame, pogo_peer_type_t *out_peer_type)
{
    pogo_peer_hello_t hello;
    if (!pogo_unpack_peer_hello(frame->payload, frame->len, &hello)) {
        if (frame->len == 2) {
            /* Exactly the OLD (pre-capability-negotiation) HELLO payload
             * shape -- a peer running firmware from before this follow-up,
             * which never knows POGO_MSG_SET_COLLECTOR or the provisioning
             * ordering that requires it. Reject this explicitly and loudly
             * here, at the handshake, instead of silently ignoring it (the
             * generic "malformed, let retry/timeout proceed" path below)
             * and letting it fail confusingly partway through provisioning
             * once SET_COLLECTOR is reached. */
            p->state = PORT_STATE_ERROR;
            pogo_port_event_data_t data = {.error_code = POGO_PORT_ERR_CAPABILITY_MISMATCH};
            ESP_LOGE(TAG, "port %d: peer sent a legacy 2-byte HELLO (no capabilities field) -- this firmware "
                          "requires POGO_CAP_COLLECTOR_PROVISIONING on both sides; refusing handshake",
                     p->id);
            notify(p, POGO_PORT_EVENT_ERROR, &data);
        }
        return false; /* otherwise: malformed payload, ignore, let retry/timeout logic proceed */
    }
    if (hello.protocol_version != POGO_FRAME_VERSION) {
        p->state = PORT_STATE_ERROR;
        pogo_port_event_data_t data = {.error_code = POGO_PORT_ERR_PROTOCOL_VERSION};
        ESP_LOGE(TAG, "port %d: hello protocol_version=%u (expected %u)", p->id, hello.protocol_version,
                 POGO_FRAME_VERSION);
        notify(p, POGO_PORT_EVENT_ERROR, &data);
        return false;
    }
    if (!pogo_peer_type_is_valid(hello.peer_type)) {
        p->state = PORT_STATE_ERROR;
        pogo_port_event_data_t data = {.error_code = POGO_PORT_ERR_PEER_TYPE_CONFLICT};
        ESP_LOGE(TAG, "port %d: peer declared invalid peer_type=%u (must be MODULE or POWER_BASE)", p->id,
                 hello.peer_type);
        notify(p, POGO_PORT_EVENT_ERROR, &data);
        return false;
    }

    bool base_id_says_power_base = base_id_reads_power_base(p);
    bool peer_says_power_base = (hello.peer_type == POGO_PEER_TYPE_POWER_BASE);
    if (base_id_present(p) && (base_id_says_power_base != peer_says_power_base)) {
        p->state = PORT_STATE_ERROR;
        pogo_port_event_data_t data = {.error_code = POGO_PORT_ERR_PEER_TYPE_CONFLICT};
        ESP_LOGE(TAG, "port %d: PEER_TYPE_CONFLICT (BASE_ID=%s, hello=%s)", p->id,
                 base_id_says_power_base ? "POWER_BASE" : "MODULE", peer_says_power_base ? "POWER_BASE" : "MODULE");
        notify(p, POGO_PORT_EVENT_ERROR, &data);
        return false;
    }

    if ((hello.capabilities & POGO_CAP_COLLECTOR_PROVISIONING) == 0) {
        p->state = PORT_STATE_ERROR;
        pogo_port_event_data_t data = {.error_code = POGO_PORT_ERR_CAPABILITY_MISMATCH};
        ESP_LOGE(TAG, "port %d: peer's HELLO capabilities=0x%02x is missing POGO_CAP_COLLECTOR_PROVISIONING -- "
                      "refusing handshake (both sides must be built from this firmware revision)",
                 p->id, hello.capabilities);
        notify(p, POGO_PORT_EVENT_ERROR, &data);
        return false;
    }

    *out_peer_type = (pogo_peer_type_t)hello.peer_type;
    return true;
}

/* Answers a HELLO_REQ with HELLO_RESP (same seq). Called regardless of this
 * port's own state -- HANDSHAKE (simultaneous/staggered boot) or already
 * PEER_READY (the peer rebooted while still docked, section 1 items 4-5). */
static void handle_incoming_hello_req(pogo_port_ctx_t *p, const pogo_frame_t *frame)
{
    pogo_peer_type_t peer_type;
    if (!validate_incoming_hello(p, frame, &peer_type)) {
        return; /* malformed: ignored; invalid/conflicting: already moved to ERROR */
    }
    p->peer_type = peer_type; /* opportunistic refresh; does not by itself change PORT_STATE_PEER_READY */

    pogo_peer_hello_t resp = {.protocol_version = POGO_FRAME_VERSION,
                               .peer_type = (uint8_t)p->own_peer_type,
                               .capabilities = POGO_HELLO_SUPPORTED_CAPABILITIES};
    uint8_t payload[3];
    size_t len = pogo_pack_peer_hello(&resp, payload, sizeof(payload));
    pogo_transport_send_on(p, POGO_MSG_PEER_HELLO_RESP, frame->seq, payload, len);
}

/* Completes THIS port's own initiator role once a HELLO_RESP matching our
 * outstanding hello_seq arrives. Only meaningful while in HANDSHAKE. */
static void handle_incoming_hello_resp(pogo_port_ctx_t *p, const pogo_frame_t *frame)
{
    pogo_peer_type_t peer_type;
    if (!validate_incoming_hello(p, frame, &peer_type)) {
        return;
    }
    p->peer_type = peer_type;
    p->state = PORT_STATE_PEER_READY;
    reset_liveness(p);
    pogo_port_event_data_t data = {.peer_type = p->peer_type};
    ESP_LOGI(TAG, "port %d: PEER_READY (peer_type=%d)", p->id, (int)p->peer_type);
    notify(p, POGO_PORT_EVENT_PEER_READY, &data);
}

/* AUTO mode only, called from process_debounce() below: a backstop against a
 * missed GPIO ISR firing or the detect_queue overflowing (both would
 * otherwise leave committed_level stale until some unrelated later edge
 * happens to correct it). At most once every CONFIG_POGO_DETECT_RECONCILE_MS,
 * directly re-reads DETECT_IN's raw level and, if it disagrees with whatever
 * this port currently believes (a pending-but-not-yet-committed level, or the
 * last committed one), injects a synthetic edge with the CURRENT tick --
 * i.e. it still has to survive a full debounce window before being
 * committed, exactly like a real ISR-sourced edge, so a momentary read glitch
 * cannot itself cause a spurious transition. */
static void reconcile_detect_level(pogo_port_ctx_t *p)
{
    TickType_t now = xTaskGetTickCount();
    if ((now - p->last_reconcile) < pdMS_TO_TICKS(CONFIG_POGO_DETECT_RECONCILE_MS)) {
        return;
    }
    p->last_reconcile = now;
    int real_level = gpio_get_level((gpio_num_t)p->cfg.detect_in_gpio);
    int believed_level = p->pending_valid ? p->pending_level : p->committed_level;
    if (real_level == believed_level) {
        return;
    }
    ESP_LOGW(TAG,
             "port %d: DETECT_IN reconciliation mismatch (raw=%d believed=%d, detect_isr_drops=%lu) -- injecting "
             "synthetic edge (missed ISR?)",
             p->id, real_level, believed_level, (unsigned long)p->detect_isr_drops);
    p->pending_level = real_level;
    p->pending_since = now;
    p->pending_valid = true;
}

static void process_debounce(pogo_port_ctx_t *p)
{
    if (p->detect_queue == NULL) {
        return; /* dev compat mode: no DETECT hardware, nothing to debounce */
    }
    detect_edge_evt_t evt;
    bool got_one = false;
    /* Drain every queued edge without blocking; only the most recent one
     * (and its timestamp) matters for the debounce window. */
    while (xQueueReceive(p->detect_queue, &evt, 0) == pdTRUE) {
        p->pending_level = evt.level;
        p->pending_since = evt.tick;
        p->pending_valid = true;
        got_one = true;
    }
    (void)got_one;

    reconcile_detect_level(p);

    if (!p->pending_valid) {
        return;
    }
    TickType_t elapsed = xTaskGetTickCount() - p->pending_since;
    if (elapsed < pdMS_TO_TICKS(p->cfg.debounce_ms)) {
        return; /* still settling */
    }
    p->pending_valid = false;
    if (p->pending_level == p->committed_level) {
        return; /* bounced back to the same state: not a real transition */
    }
    p->committed_level = p->pending_level;

    if (p->committed_level == 0) {
        ESP_LOGI(TAG, "port %d: ATTACHED (DETECT_IN debounced Low)", p->id);
        notify(p, POGO_PORT_EVENT_ATTACHED, NULL);
        enter_attach_settle(p);
    } else {
        ESP_LOGI(TAG, "port %d: DETACHED (DETECT_IN debounced High)", p->id);
        notify(p, POGO_PORT_EVENT_DETACHED, NULL);
        enter_wait_attach(p);
    }
}

/* PORT_STATE_ATTACH_SETTLE: bounded poll (not a busy loop) for
 * state_deadline, then starts the first handshake. If DETECT_IN debounces
 * back to detached during this wait, process_debounce() (called every loop
 * iteration before the state switch in port_task()) has already moved
 * p->state to PORT_STATE_WAIT_ATTACH by the time this would otherwise run --
 * so this function is simply never reached for that iteration. */
static void process_attach_settle(pogo_port_ctx_t *p)
{
    if ((int32_t)(xTaskGetTickCount() - p->state_deadline) < 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }
    begin_handshake(p);
}

/* PORT_STATE_RECOVER_COOLDOWN: same bounded-poll shape as
 * process_attach_settle() above, and the same detach-cancels-it guarantee
 * (process_debounce() pre-empts p->state before this ever runs). Once
 * state_deadline elapses, begin_handshake() starts a fresh cycle -- still
 * capped at CONFIG_POGO_MAX_RETRY retries internally, exactly like the very
 * first handshake after ATTACHED. */
static void process_recover_cooldown(pogo_port_ctx_t *p)
{
    if ((int32_t)(xTaskGetTickCount() - p->state_deadline) < 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
        return;
    }
    ESP_LOGI(TAG, "port %d: cooldown elapsed -- starting a fresh handshake cycle", p->id);
    begin_handshake(p);
}

static void process_handshake(pogo_port_ctx_t *p)
{
    uint8_t byte;
    TickType_t now = xTaskGetTickCount();
    TickType_t remaining = ((int32_t)(p->hello_deadline - now) > 0) ? (p->hello_deadline - now) : 0;
    TickType_t wait = remaining < pdMS_TO_TICKS(20) ? remaining : pdMS_TO_TICKS(20);

    int n = uart_read_bytes((uart_port_t)p->cfg.uart_num, &byte, 1, wait);
    if (n == 1) {
        pogo_frame_t frame;
        if (pogo_frame_parser_feed(&p->parser, byte, &frame)) {
            if (frame.type == POGO_MSG_PEER_HELLO_REQ) {
                /* Answer regardless of our own initiator progress -- the
                 * peer may be the initiator too (simultaneous boot), or may
                 * simply be slower to attach (staggered boot). */
                handle_incoming_hello_req(p, &frame);
            } else if (frame.type == POGO_MSG_PEER_HELLO_RESP && frame.seq == p->hello_seq) {
                handle_incoming_hello_resp(p, &frame);
            } else {
                ESP_LOGD(TAG, "port %d: ignoring frame type=0x%02x during handshake", p->id, frame.type);
            }
            if (p->state != PORT_STATE_HANDSHAKE) {
                return; /* moved to PEER_READY/ERROR by the frame just processed */
            }
        }
    }

    if ((int32_t)(xTaskGetTickCount() - p->hello_deadline) >= 0) {
        p->hello_retries++;
        if (p->hello_retries > CONFIG_POGO_MAX_RETRY) {
            handle_dock_timeout_or_silent_power_base(p);
            return;
        }
        p->hello_seq++;
        send_hello_req(p, p->hello_seq);
        p->hello_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CONFIG_POGO_ACK_TIMEOUT_MS);
    }
}

/*
 * Idle liveness (section 9): while PEER_READY and nothing else has been
 * heard for CONFIG_POGO_PING_INTERVAL_MS, send one PING (own dedicated seq
 * space -- p->ping_seq -- distinct from the handshake's hello_seq and from
 * whatever seq numbers higher layers use for provisioning, so none of them
 * can collide in meaning). This never competes with a higher layer's own
 * pogo_port_send()-driven request/response: sending is fire-and-forget from
 * this port task's point of view (uart_write_bytes has its own buffering),
 * and RECEIVING is exclusively this task's job either way (process_peer_ready
 * is the only reader of this port's UART), so there is exactly one place
 * that ever decides "is this byte a PING/PONG or a provisioning frame".
 */
static void maybe_send_idle_ping(pogo_port_ctx_t *p)
{
    TickType_t now = xTaskGetTickCount();
    if (p->ping_outstanding) {
        if ((int32_t)(now - p->ping_deadline) >= 0) {
            p->ping_outstanding = false;
            p->liveness_fail_count++;
            ESP_LOGD(TAG, "port %d: PING timeout (%d/%d consecutive)", p->id, p->liveness_fail_count,
                     LIVENESS_MAX_CONSECUTIVE_FAILURES);
            if (p->liveness_fail_count >= LIVENESS_MAX_CONSECUTIVE_FAILURES) {
                handle_liveness_lost(p);
            }
        }
        return;
    }
    if ((now - p->last_activity) < pdMS_TO_TICKS(CONFIG_POGO_PING_INTERVAL_MS)) {
        return;
    }
    p->ping_seq++;
    pogo_transport_send_on(p, POGO_MSG_PING, p->ping_seq, NULL, 0);
    p->ping_outstanding = true;
    p->ping_deadline = now + pdMS_TO_TICKS(CONFIG_POGO_ACK_TIMEOUT_MS);
}

static void process_peer_ready(pogo_port_ctx_t *p)
{
    uint8_t byte;
    int n = uart_read_bytes((uart_port_t)p->cfg.uart_num, &byte, 1, pdMS_TO_TICKS(20));
    if (n != 1) {
        maybe_send_idle_ping(p);
        return;
    }
    pogo_frame_t frame;
    if (!pogo_frame_parser_feed(&p->parser, byte, &frame)) {
        return; /* mid-frame, or a dropped malformed frame -- not "activity" yet */
    }

    /* Any complete, valid frame -- including PING/PONG -- counts as real
     * link activity, resetting the idle countdown to the next PING. */
    p->last_activity = xTaskGetTickCount();

    if (frame.type == POGO_MSG_PEER_HELLO_REQ) {
        /* The peer (re-)sent HELLO_REQ, e.g. it rebooted while still
         * docked and is starting its own handshake fresh. We are already
         * PEER_READY -- still answer, per section 1 item 4. */
        handle_incoming_hello_req(p, &frame);
        return;
    }
    if (frame.type == POGO_MSG_PEER_HELLO_RESP) {
        /* Late/duplicate reply to an old request of ours (e.g. after we
         * already reached PEER_READY via a different retry). No outstanding
         * request to match it against here; ignore. */
        return;
    }
    if (frame.type == POGO_MSG_PING) {
        /* Answered here, not forwarded up -- see this function's top
         * comment. Higher layers never see a PING/PONG frame. */
        pogo_transport_send_on(p, POGO_MSG_PONG, frame.seq, NULL, 0);
        return;
    }
    if (frame.type == POGO_MSG_PONG) {
        if (p->ping_outstanding && frame.seq == p->ping_seq) {
            p->ping_outstanding = false;
            p->liveness_fail_count = 0;
        }
        /* A stray/late PONG that doesn't match our current outstanding PING
         * is simply ignored -- not an error, just a race with a just-timed-
         * out PING. */
        return;
    }
    pogo_port_event_data_t data = {.frame = frame};
    notify(p, POGO_PORT_EVENT_FRAME_RECEIVED, &data);
}

static void port_task(void *arg)
{
    pogo_port_ctx_t *p = (pogo_port_ctx_t *)arg;

    p->last_reconcile = xTaskGetTickCount();

    if (p->cfg.detect_in_gpio < 0) {
        /* Dev compat mode: no DETECT hardware at all -- the port is
         * considered attached the instant it is enabled (see
         * docs/pogo-interface.md "development compat mode"). */
        ESP_LOGW(TAG, "port %d: no DETECT_IN configured -- treating as permanently attached (dev compat mode)",
                 p->id);
        notify(p, POGO_PORT_EVENT_ATTACHED, NULL);
        begin_handshake(p);
    } else {
        enter_wait_attach(p);
    }

    while (1) {
        process_debounce(p);
        maybe_log_idle_status(p);

        switch (p->state) {
        case PORT_STATE_WAIT_ATTACH:
            vTaskDelay(pdMS_TO_TICKS(20));
            break;
        case PORT_STATE_ATTACH_SETTLE:
            process_attach_settle(p);
            break;
        case PORT_STATE_HANDSHAKE:
            process_handshake(p);
            break;
        case PORT_STATE_PEER_READY:
            process_peer_ready(p);
            break;
        case PORT_STATE_RECOVER_COOLDOWN:
            process_recover_cooldown(p);
            break;
        case PORT_STATE_ERROR:
        case PORT_STATE_DISABLED:
        default:
            vTaskDelay(pdMS_TO_TICKS(50));
            break;
        }
    }
}

esp_err_t pogo_port_init(pogo_port_id_t id, const pogo_port_config_t *cfg, pogo_port_event_cb_t cb, void *user_ctx)
{
    if (id >= POGO_PORT_ID_COUNT || cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pogo_port_ctx_t *p = &s_ports[id];
    memset(p, 0, sizeof(*p));
    p->id = id;
    p->cfg = *cfg;
    p->cb = cb;
    p->user_ctx = user_ctx;
    p->state = PORT_STATE_DISABLED;
    p->committed_level = 1; /* idle/detached */
    p->inited = true;

    if (cfg->detect_in_gpio >= 0) {
        p->detect_queue = xQueueCreate(DETECT_QUEUE_LEN, sizeof(detect_edge_evt_t));
        if (p->detect_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

/*
 * Every ESP-IDF/FreeRTOS call below has its return value checked (code
 * review follow-up section 6). On ANY failure, everything already set up in
 * THIS call is unwound in reverse order (the `fail`/`fail_no_uart` labels)
 * before returning -- so a caller that fixes whatever failed and calls
 * pogo_port_enable() again always starts from a clean, fully-DISABLED
 * state, never a half-initialized one (DETECT_OUT released, any installed
 * ISR handler removed, the UART driver removed, no task left running,
 * `enabled` left false).
 */
esp_err_t pogo_port_enable(pogo_port_id_t id, pogo_peer_type_t own_peer_type)
{
    if (id >= POGO_PORT_ID_COUNT || !s_ports[id].inited) {
        return ESP_ERR_INVALID_STATE;
    }
    pogo_port_ctx_t *p = &s_ports[id];
    if (!p->cfg.enabled) {
        return ESP_ERR_NOT_SUPPORTED; /* logical port not wired on this board */
    }
    if (p->enabled) {
        return ESP_ERR_INVALID_STATE; /* already enabled -- call pogo_port_disable() first */
    }
    p->own_peer_type = own_peer_type;
    bool isr_added = false;

    uart_config_t uart_cfg = {
        .baud_rate = p->cfg.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install((uart_port_t)p->cfg.uart_num, 512, 512, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "port %d: uart_driver_install failed: %s", id, esp_err_to_name(err));
        goto fail_no_uart;
    }
    err = uart_param_config((uart_port_t)p->cfg.uart_num, &uart_cfg);
    if (err == ESP_OK) {
        err = uart_set_pin((uart_port_t)p->cfg.uart_num, p->cfg.tx_gpio, p->cfg.rx_gpio, UART_PIN_NO_CHANGE,
                            UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "port %d: UART config failed: %s", id, esp_err_to_name(err));
        goto fail;
    }

    if (p->cfg.detect_out_gpio >= 0) {
        gpio_config_t out_cfg = {
            .pin_bit_mask = 1ULL << p->cfg.detect_out_gpio,
            .mode = GPIO_MODE_OUTPUT_OD,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&out_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "port %d: DETECT_OUT gpio_config failed: %s", id, esp_err_to_name(err));
            goto fail;
        }
        err = set_detect_out(p, true); /* announce presence: drive Low */
        if (err != ESP_OK) {
            /* Do NOT return success with DETECT_OUT left unasserted -- a
             * peer would never see "I am here" even though pogo_port_enable()
             * claimed to have brought the port up. */
            ESP_LOGE(TAG, "port %d: failed to assert DETECT_OUT during enable: %s", id, esp_err_to_name(err));
            goto fail;
        }
    }

    if (p->cfg.base_id_gpio >= 0) {
        gpio_config_t base_cfg = {
            .pin_bit_mask = 1ULL << p->cfg.base_id_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&base_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "port %d: BASE_ID gpio_config failed: %s", id, esp_err_to_name(err));
            goto fail;
        }
    }

    if (p->cfg.detect_in_gpio >= 0) {
        gpio_config_t in_cfg = {
            .pin_bit_mask = 1ULL << p->cfg.detect_in_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_ANYEDGE,
        };
        err = gpio_config(&in_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "port %d: DETECT_IN gpio_config failed: %s", id, esp_err_to_name(err));
            goto fail;
        }

        if (!s_isr_service_installed) {
            esp_err_t isr_err = gpio_install_isr_service(GPIO_ISR_FLAGS);
            if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_err));
                err = isr_err;
                goto fail;
            }
            s_isr_service_installed = true;
        }
        err = gpio_isr_handler_add((gpio_num_t)p->cfg.detect_in_gpio, detect_isr_handler, p);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "port %d: gpio_isr_handler_add failed: %s", id, esp_err_to_name(err));
            goto fail;
        }
        isr_added = true;

        /* Seed the debounce state from whatever DETECT_IN reads right now,
         * so an already-docked module is detected on enable, not only on
         * the next edge. A dropped seed is non-fatal (a fresh queue should
         * never actually be full) -- the port simply picks up the real
         * level on the next genuine edge instead; logged, not unwound. */
        detect_edge_evt_t seed = {.tick = xTaskGetTickCount(),
                                   .level = gpio_get_level((gpio_num_t)p->cfg.detect_in_gpio)};
        if (xQueueSend(p->detect_queue, &seed, 0) != pdTRUE) {
            ESP_LOGW(TAG, "port %d: failed to seed initial DETECT_IN level (queue full?) -- will pick up the "
                          "next edge instead",
                     id);
        }
    }

    char name[16];
    snprintf(name, sizeof(name), "pogo_port%d", (int)id);
    p->state = PORT_STATE_DISABLED; /* port_task moves this forward on entry */
    BaseType_t ok = xTaskCreate(port_task, name, PORT_TASK_STACK, p, PORT_TASK_PRIO, &p->task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "port %d: xTaskCreate failed", id);
        err = ESP_ERR_NO_MEM;
        p->task = NULL;
        goto fail;
    }

    p->enabled = true;
    return ESP_OK;

fail: {
    /* Preserve the ORIGINAL failure (`err`, already set above) as the
     * function's return value even if cleanup itself also fails -- a
     * cleanup failure is logged, never allowed to overwrite or hide what
     * actually caused pogo_port_enable() to fail in the first place. Every
     * requested step is still attempted (pogo_port_run_cleanup_ops()'s own
     * contract) -- e.g. a failing DETECT_OUT release must not skip the UART
     * teardown that follows it. */
    esp_err_t original_err = err;
    esp_err_t cleanup_err =
        pogo_port_run_cleanup_ops(isr_added, cleanup_op_isr_remove, cleanup_op_detect_release,
                                   /* uart_driver_install() always succeeded by the time any `goto fail;` above is
                                    * reached (the only path that skips this is `fail_no_uart:`, taken directly on
                                    * uart_driver_install()'s own failure) -- so a UART delete is always in order here. */
                                   true, cleanup_op_uart_delete, p);
    if (cleanup_err != ESP_OK) {
        ESP_LOGE(TAG, "port %d: cleanup after enable() failure ALSO failed: %s (original failure preserved: %s)", id,
                 esp_err_to_name(cleanup_err), esp_err_to_name(original_err));
    }
    err = original_err;
}
fail_no_uart:
    p->task = NULL;
    p->state = PORT_STATE_DISABLED;
    p->enabled = false;
    return err;
}

/*
 * Idempotent: a second (or Nth) call while already disabled (or never
 * enabled) is a safe no-op that still leaves DETECT_OUT released -- it does
 * not re-run vTaskDelete/gpio_isr_handler_remove/uart_driver_delete against
 * resources that were already torn down (which the underlying ESP-IDF
 * drivers would otherwise just log an error for, but this avoids that noise
 * and keeps the "safe to call repeatedly" contract explicit rather than
 * incidental).
 *
 * Returns the FIRST error encountered among ISR removal / DETECT_OUT
 * release / UART deletion (ESP_OK if all succeeded) -- but every one of
 * those steps is still attempted regardless of an earlier failure, via the
 * same pogo_port_run_cleanup_ops() policy pogo_port_enable()'s
 * failure-unwind path uses (see that function and
 * tools/test_pogo_port_cleanup_ops.c).
 */
esp_err_t pogo_port_disable(pogo_port_id_t id)
{
    if (id >= POGO_PORT_ID_COUNT || !s_ports[id].inited) {
        return ESP_ERR_INVALID_STATE;
    }
    pogo_port_ctx_t *p = &s_ports[id];

    if (!p->enabled) {
        /* Always ensure DETECT_OUT is released, even if enable() was never
         * called -- still reported (not silently swallowed) even in this
         * already-disabled shortcut path. */
        esp_err_t err = set_detect_out(p, false);
        p->state = PORT_STATE_DISABLED;
        p->peer_type = POGO_PEER_TYPE_UNKNOWN;
        return err;
    }

    if (p->task != NULL) {
        vTaskDelete(p->task);
        p->task = NULL;
    }

    esp_err_t err = pogo_port_run_cleanup_ops(p->cfg.detect_in_gpio >= 0, cleanup_op_isr_remove,
                                               cleanup_op_detect_release, true, cleanup_op_uart_delete, p);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "port %d: pogo_port_disable() cleanup encountered an error (first: %s) -- every cleanup step "
                      "was still attempted",
                 id, esp_err_to_name(err));
    }
    p->state = PORT_STATE_DISABLED;
    p->peer_type = POGO_PEER_TYPE_UNKNOWN;
    p->enabled = false;
    return err;
}

bool pogo_port_is_attached(pogo_port_id_t id)
{
    if (id >= POGO_PORT_ID_COUNT) {
        return false;
    }
    port_internal_state_t st = s_ports[id].state;
    return st == PORT_STATE_ATTACH_SETTLE || st == PORT_STATE_HANDSHAKE || st == PORT_STATE_PEER_READY ||
           st == PORT_STATE_RECOVER_COOLDOWN || st == PORT_STATE_ERROR;
}

pogo_peer_type_t pogo_port_get_peer_type(pogo_port_id_t id)
{
    if (id >= POGO_PORT_ID_COUNT) {
        return POGO_PEER_TYPE_UNKNOWN;
    }
    return s_ports[id].peer_type;
}

esp_err_t pogo_port_send(pogo_port_id_t id, uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    if (id >= POGO_PORT_ID_COUNT || !s_ports[id].inited) {
        return ESP_ERR_INVALID_ARG;
    }
    pogo_port_ctx_t *p = &s_ports[id];
    if (p->state != PORT_STATE_PEER_READY) {
        return ESP_ERR_INVALID_STATE;
    }
    return pogo_transport_send_on(p, type, seq, payload, len);
}
