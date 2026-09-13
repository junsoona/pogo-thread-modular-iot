#include "pogo_port/pogo_port.h"

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

typedef enum {
    PORT_STATE_DISABLED = 0,
    PORT_STATE_WAIT_ATTACH,
    PORT_STATE_HANDSHAKE,
    PORT_STATE_PEER_READY,
    PORT_STATE_ERROR,
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

    /* Handshake bookkeeping. */
    port_internal_state_t state;
    pogo_frame_parser_t   parser;
    uint8_t   hello_seq;
    int       hello_retries;
    TickType_t hello_deadline;
    pogo_peer_type_t peer_type;

    bool inited;
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
    xQueueSendFromISR(p->detect_queue, &evt, &hpw);
    if (hpw) {
        portYIELD_FROM_ISR();
    }
}

static void set_detect_out(pogo_port_ctx_t *p, bool asserted)
{
    if (p->cfg.detect_out_gpio < 0) {
        return;
    }
    /* Open-drain: level 0 drives Low (asserted, "I am here"); level 1
     * releases the line so the external pull-up takes it High. */
    gpio_set_level((gpio_num_t)p->cfg.detect_out_gpio, asserted ? 0 : 1);
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

static void enter_wait_attach(pogo_port_ctx_t *p)
{
    p->state = PORT_STATE_WAIT_ATTACH;
    p->peer_type = POGO_PEER_TYPE_UNKNOWN;
    pogo_frame_parser_init(&p->parser);
}

static void send_hello_req(pogo_port_ctx_t *p, uint8_t seq)
{
    pogo_peer_hello_t hello = {.protocol_version = POGO_FRAME_VERSION, .peer_type = (uint8_t)p->own_peer_type};
    uint8_t payload[2];
    size_t len = pogo_pack_peer_hello(&hello, payload, sizeof(payload));
    pogo_transport_send_on(p, POGO_MSG_PEER_HELLO_REQ, seq, payload, len);
}

static void begin_handshake(pogo_port_ctx_t *p)
{
    p->state = PORT_STATE_HANDSHAKE;
    pogo_frame_parser_init(&p->parser);
    p->hello_seq = 1;
    p->hello_retries = 0;
    send_hello_req(p, p->hello_seq);
    p->hello_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CONFIG_POGO_ACK_TIMEOUT_MS);
}

static void handle_dock_timeout_or_silent_power_base(pogo_port_ctx_t *p)
{
    if (base_id_reads_power_base(p)) {
        /* A passive power base need not speak POGO UART at all -- BASE_ID
         * hardware alone is authoritative for this case. */
        p->peer_type = POGO_PEER_TYPE_POWER_BASE;
        p->state = PORT_STATE_PEER_READY;
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
         * cycle, still capped at CONFIG_POGO_MAX_RETRY per cycle. AUTO mode
         * (real DETECT_IN) keeps the original policy of waiting for a
         * physical detach/attach before trying again. */
        vTaskDelay(pdMS_TO_TICKS(CONFIG_POGO_PING_INTERVAL_MS));
        ESP_LOGI(TAG, "port %d: dev compat mode -- retrying handshake cycle", p->id);
        begin_handshake(p);
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
        return false; /* malformed payload: ignore, let retry/timeout logic proceed */
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

    pogo_peer_hello_t resp = {.protocol_version = POGO_FRAME_VERSION, .peer_type = (uint8_t)p->own_peer_type};
    uint8_t payload[2];
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
    pogo_port_event_data_t data = {.peer_type = p->peer_type};
    ESP_LOGI(TAG, "port %d: PEER_READY (peer_type=%d)", p->id, (int)p->peer_type);
    notify(p, POGO_PORT_EVENT_PEER_READY, &data);
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
        begin_handshake(p);
    } else {
        ESP_LOGI(TAG, "port %d: DETACHED (DETECT_IN debounced High)", p->id);
        notify(p, POGO_PORT_EVENT_DETACHED, NULL);
        enter_wait_attach(p);
    }
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

static void process_peer_ready(pogo_port_ctx_t *p)
{
    uint8_t byte;
    int n = uart_read_bytes((uart_port_t)p->cfg.uart_num, &byte, 1, pdMS_TO_TICKS(20));
    if (n != 1) {
        return;
    }
    pogo_frame_t frame;
    if (!pogo_frame_parser_feed(&p->parser, byte, &frame)) {
        return;
    }
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
    pogo_port_event_data_t data = {.frame = frame};
    notify(p, POGO_PORT_EVENT_FRAME_RECEIVED, &data);
}

static void port_task(void *arg)
{
    pogo_port_ctx_t *p = (pogo_port_ctx_t *)arg;

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

        switch (p->state) {
        case PORT_STATE_WAIT_ATTACH:
            vTaskDelay(pdMS_TO_TICKS(20));
            break;
        case PORT_STATE_HANDSHAKE:
            process_handshake(p);
            break;
        case PORT_STATE_PEER_READY:
            process_peer_ready(p);
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

esp_err_t pogo_port_enable(pogo_port_id_t id, pogo_peer_type_t own_peer_type)
{
    if (id >= POGO_PORT_ID_COUNT || !s_ports[id].inited) {
        return ESP_ERR_INVALID_STATE;
    }
    pogo_port_ctx_t *p = &s_ports[id];
    if (!p->cfg.enabled) {
        return ESP_ERR_NOT_SUPPORTED; /* logical port not wired on this board */
    }
    p->own_peer_type = own_peer_type;

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
        return err;
    }
    err = uart_param_config((uart_port_t)p->cfg.uart_num, &uart_cfg);
    if (err == ESP_OK) {
        err = uart_set_pin((uart_port_t)p->cfg.uart_num, p->cfg.tx_gpio, p->cfg.rx_gpio, UART_PIN_NO_CHANGE,
                            UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "port %d: UART config failed: %s", id, esp_err_to_name(err));
        return err;
    }

    if (p->cfg.detect_out_gpio >= 0) {
        gpio_config_t out_cfg = {
            .pin_bit_mask = 1ULL << p->cfg.detect_out_gpio,
            .mode = GPIO_MODE_OUTPUT_OD,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&out_cfg);
        set_detect_out(p, true); /* announce presence: drive Low */
    }

    if (p->cfg.base_id_gpio >= 0) {
        gpio_config_t base_cfg = {
            .pin_bit_mask = 1ULL << p->cfg.base_id_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&base_cfg);
    }

    if (p->cfg.detect_in_gpio >= 0) {
        gpio_config_t in_cfg = {
            .pin_bit_mask = 1ULL << p->cfg.detect_in_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_ANYEDGE,
        };
        gpio_config(&in_cfg);

        if (!s_isr_service_installed) {
            esp_err_t isr_err = gpio_install_isr_service(GPIO_ISR_FLAGS);
            if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_err));
                return isr_err;
            }
            s_isr_service_installed = true;
        }
        gpio_isr_handler_add((gpio_num_t)p->cfg.detect_in_gpio, detect_isr_handler, p);

        /* Seed the debounce state from whatever DETECT_IN reads right now,
         * so an already-docked module is detected on enable, not only on
         * the next edge. */
        detect_edge_evt_t seed = {.tick = xTaskGetTickCount(),
                                   .level = gpio_get_level((gpio_num_t)p->cfg.detect_in_gpio)};
        xQueueSend(p->detect_queue, &seed, 0);
    }

    char name[16];
    snprintf(name, sizeof(name), "pogo_port%d", (int)id);
    p->state = PORT_STATE_DISABLED; /* port_task moves this forward on entry */
    BaseType_t ok = xTaskCreate(port_task, name, PORT_TASK_STACK, p, PORT_TASK_PRIO, &p->task);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t pogo_port_disable(pogo_port_id_t id)
{
    if (id >= POGO_PORT_ID_COUNT || !s_ports[id].inited) {
        return ESP_ERR_INVALID_STATE;
    }
    pogo_port_ctx_t *p = &s_ports[id];

    if (p->task != NULL) {
        vTaskDelete(p->task);
        p->task = NULL;
    }
    if (p->cfg.detect_in_gpio >= 0) {
        gpio_isr_handler_remove((gpio_num_t)p->cfg.detect_in_gpio);
    }
    set_detect_out(p, false); /* release to High-Z */
    uart_driver_delete((uart_port_t)p->cfg.uart_num);
    p->state = PORT_STATE_DISABLED;
    p->peer_type = POGO_PEER_TYPE_UNKNOWN;
    return ESP_OK;
}

bool pogo_port_is_attached(pogo_port_id_t id)
{
    if (id >= POGO_PORT_ID_COUNT) {
        return false;
    }
    port_internal_state_t st = s_ports[id].state;
    return st == PORT_STATE_HANDSHAKE || st == PORT_STATE_PEER_READY || st == PORT_STATE_ERROR;
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
