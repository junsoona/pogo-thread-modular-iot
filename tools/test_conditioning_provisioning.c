/*
 * Host-compiled regression test for SENSOR_STATUS_CONDITIONING provisioning
 * handling (PMS5003 warm-up): parent_service.c must accept
 * SENSOR_INFO_ACK.sensor_status == CONDITIONING (not just OK) and continue
 * the provisioning sequence through POLL_CONFIG/SET_COLLECTOR/SET_DATASET/
 * Thread join, while any OTHER non-zero status still abandons the session --
 * see parent_service.c's run_provisioning_session() and this test's own
 * PS_SENSOR_STATUS_CONDITIONING comment there.
 *
 * ALSO covers this follow-up's SED CoAP response timing correction's core
 * regression test (real-hardware finding: a LEAF's own Thread SED data-poll
 * period must never be derived from its sensor measurement period -- see
 * components/coap_response_timing and thread_manager.h's comment on
 * thread_manager_join_with_dataset()): PART A's real end-to-end run snoops
 * the real, dynamically-computed polling_period_ms the real parent_service.c
 * actually sends over the wire in POGO_MSG_POLL_CONFIG, and (via
 * tools/host_test_stubs/host_thread_manager_sim.c's new call-recording)
 * confirms the real child_service.c passes thread_data_poll_period_ms=0 to
 * the (stubbed) thread_manager_join_with_dataset() -- proving the two
 * periods are genuinely decoupled using a REAL dynamically-measured value,
 * not a hardcoded literal.
 *
 * PART A links and runs the REAL, UNMODIFIED components/child_service/
 * child_service.c, components/parent_service/parent_service.c,
 * components/sensor_service/sensor_service.c, components/pms5003/pms5003.c +
 * pms5003_parser.c, and TWO REAL components/pogo_port/pogo_port.c instances
 * (POGO_PORT_UPSTREAM as the child's own port, POGO_PORT_DOWNSTREAM as the
 * parent's) bridged together over the same fake-UART infrastructure built
 * for tools/test_pogo_port_hotswap_recovery.c -- an actual end-to-end run of
 * the real provisioning protocol, not a scripted/mocked one, with a fake
 * PMS5003 sensor (tools/host_test_stubs/host_pms5003_fake_sensor.c) standing
 * in for the physical sensor hardware. The child's own real sensor_status is
 * observed by snooping the wire (decoding with the REAL pogo_frame_parser_t/
 * pogo_unpack_sensor_info_ack()), and the outcome is confirmed by the real
 * JOIN_RESULT_ACK actually arriving back.
 *
 * PART B links and runs parent_service.c ALONE (real, reusing PART A's same
 * initialized instance) against a scripted fake child on the real DOWNSTREAM
 * pogo_port, to directly verify the two ends of the sensor_status range this
 * test's PART A cannot easily force: a genuine sensor ERROR status (still
 * rejected) and the ordinary sensor_status==OK path every other driver
 * (SHT45/BH1750/SGP40/SGP41) always reports (no regression).
 *
 * child_service.c also needs a Thread stack (nvs, esp_mac, thread_manager,
 * role_manager) it cannot itself be host-compiled without -- these are
 * stood in for by tools/host_test_stubs/nvs.h, esp_mac.h,
 * host_thread_manager_sim.c and host_role_manager_sim.c, each a stand-in for
 * an external HARDWARE/STACK dependency (same category as the GPIO/UART
 * banks), never a reimplementation of child_service.c's/parent_service.c's
 * own logic, which both run completely unmodified against them.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs/pogo_fast -I tools/host_test_stubs \
 *      -I components/pogo_port/include -I components/pogo_proto/include \
 *      -I components/child_service/include -I components/parent_service/include \
 *      -I components/sensor_service/include -I components/sensor_provider/include \
 *      -I components/sensor_channel/include -I components/pms5003/include \
 *      -I components/module_config/include -I components/role_manager/include \
 *      -I components/thread_manager/include -I components/collector_endpoint/include \
 *      -I components/node_calc/include -I components/sensor_root_aggregator/include \
 *      -o /tmp/test_conditioning_provisioning tools/test_conditioning_provisioning.c \
 *      components/pogo_port/pogo_port.c components/pogo_port/pogo_port_cleanup.c \
 *      components/pogo_proto/pogo_frame.c components/pogo_proto/pogo_crc16.c \
 *      components/pogo_proto/pogo_protocol.c \
 *      components/child_service/child_service.c components/parent_service/parent_service.c \
 *      components/sensor_service/sensor_service.c components/sensor_provider/sensor_provider.c \
 *      components/sensor_provider/mock_sensor_provider.c \
 *      components/pms5003/pms5003.c components/pms5003/pms5003_parser.c \
 *      components/node_calc/node_calc.c components/sensor_root_aggregator/sensor_root_aggregator.c \
 *      components/sensor_channel/sensor_channel.c components/collector_endpoint/collector_endpoint.c \
 *      tools/host_test_stubs/host_gpio_sim.c tools/host_test_stubs/host_uart_sim.c \
 *      tools/host_test_stubs/host_pms5003_fake_sensor.c \
 *      tools/host_test_stubs/host_thread_manager_sim.c tools/host_test_stubs/host_role_manager_sim.c \
 *   && /tmp/test_conditioning_provisioning
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
#include "module_config/module_config.h"
#include "sensor_provider/sensor_provider.h"
#include "sensor_service/sensor_service.h"
#include "pms5003/pms5003.h"
#include "child_service/child_service.h"
#include "parent_service/parent_service.h"
#include "collector_endpoint/collector_endpoint.h"
#include "openthread/thread.h"
#include "host_pms5003_fake_sensor.h"

/* Test-only controls, defined in tools/host_test_stubs/host_thread_manager_sim.c
 * and host_role_manager_sim.c -- not part of any real production header. */
extern void host_thread_manager_test_reset(void);
extern void host_thread_manager_test_set_join_outcome(bool fail, otDeviceRole success_role, unsigned delay_ms);
/* This follow-up (SED CoAP response timing correction): what the LAST real
 * thread_manager_join_with_dataset() call actually received. */
extern uint32_t host_thread_manager_test_get_last_poll_period_ms(void);
extern uint16_t host_thread_manager_test_get_last_dataset_len(void);
extern int host_thread_manager_test_get_join_call_count(void);
extern void host_role_manager_test_set_role(module_role_t role);

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

#define CHILD_UART 0
#define PARENT_UART 1
#define PMS_UART 2
#define PMS_WARMUP_MS 1500u

/* --- PART A: real child<->parent bridge with wire snooping --- */

static volatile bool s_bridge_running = true;
static pthread_t s_bridge_c2p, s_bridge_p2c;
static pthread_mutex_t s_observe_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool s_saw_sensor_info_ack = false;
static uint8_t s_sensor_info_ack_status = 0xFF;
static bool s_saw_join_result_ack = false;
/* This follow-up (SED CoAP response timing correction): the real
 * polling_period_ms the real parent_service.c actually computed (from a
 * real measured response_time_ms, node_calc_polling_period_ms()) and sent
 * over the wire in POGO_MSG_POLL_CONFIG -- captured to prove it is NEVER
 * the same value handed to thread_manager_join_with_dataset() (see
 * host_thread_manager_test_get_last_poll_period_ms()). */
static bool s_saw_poll_config = false;
static uint32_t s_poll_config_period_ms = 0;
static uint16_t s_dataset_len_from_set_dataset = 0;

static void *bridge_child_to_parent(void *arg)
{
    (void)arg;
    pogo_frame_parser_t parser;
    pogo_frame_parser_init(&parser);
    while (s_bridge_running) {
        uint8_t b;
        if (!host_uart_test_take_tx_byte(CHILD_UART, &b, 50)) {
            continue;
        }
        host_uart_test_push_rx(PARENT_UART, &b, 1);
        pogo_frame_t frame;
        if (pogo_frame_parser_feed(&parser, b, &frame) && frame.type == POGO_MSG_SENSOR_INFO_ACK) {
            pogo_sensor_info_ack_t ack;
            if (pogo_unpack_sensor_info_ack(frame.payload, frame.len, &ack)) {
                pthread_mutex_lock(&s_observe_mutex);
                s_saw_sensor_info_ack = true;
                s_sensor_info_ack_status = ack.sensor_status;
                pthread_mutex_unlock(&s_observe_mutex);
            }
        }
    }
    return NULL;
}

static void *bridge_parent_to_child(void *arg)
{
    (void)arg;
    pogo_frame_parser_t parser;
    pogo_frame_parser_init(&parser);
    while (s_bridge_running) {
        uint8_t b;
        if (!host_uart_test_take_tx_byte(PARENT_UART, &b, 50)) {
            continue;
        }
        host_uart_test_push_rx(CHILD_UART, &b, 1);
        pogo_frame_t frame;
        if (pogo_frame_parser_feed(&parser, b, &frame)) {
            if (frame.type == POGO_MSG_JOIN_RESULT_ACK) {
                pthread_mutex_lock(&s_observe_mutex);
                s_saw_join_result_ack = true;
                pthread_mutex_unlock(&s_observe_mutex);
            } else if (frame.type == POGO_MSG_POLL_CONFIG) {
                pogo_poll_config_t poll_msg;
                if (pogo_unpack_poll_config(frame.payload, frame.len, &poll_msg)) {
                    pthread_mutex_lock(&s_observe_mutex);
                    s_saw_poll_config = true;
                    s_poll_config_period_ms = poll_msg.polling_period_ms;
                    pthread_mutex_unlock(&s_observe_mutex);
                }
            } else if (frame.type == POGO_MSG_SET_DATASET) {
                pthread_mutex_lock(&s_observe_mutex);
                s_dataset_len_from_set_dataset = frame.len;
                pthread_mutex_unlock(&s_observe_mutex);
            }
        }
    }
    return NULL;
}

static void test_port_event_dispatcher(pogo_port_id_t port_id, pogo_port_event_t event,
                                        const pogo_port_event_data_t *data, void *user_ctx)
{
    (void)user_ctx;
    if (port_id == POGO_PORT_UPSTREAM) {
        child_service_on_port_event(port_id, event, data);
    } else {
        parent_service_on_port_event(port_id, event, data);
    }
}

static uint8_t s_test_parent_uuid[POGO_UUID_LEN] = {0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x01};

static bool pt_provisioning_allowed(void *ctx)
{
    (void)ctx;
    return true;
}
static uint8_t pt_get_own_depth(void *ctx)
{
    (void)ctx;
    return 0;
}
static void pt_get_own_uuid(uint8_t out[POGO_UUID_LEN], void *ctx)
{
    (void)ctx;
    memcpy(out, s_test_parent_uuid, POGO_UUID_LEN);
}
static const char *pt_role_name(void *ctx)
{
    (void)ctx;
    return "TEST_PARENT";
}
static bool pt_get_collector(pogo_collector_endpoint_t *out, void *ctx)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    memcpy(out->power_base_uuid, s_test_parent_uuid, POGO_UUID_LEN);
    out->addr[0] = 0xFD; /* ULA-range -- not unspecified/multicast/link-local */
    out->addr[15] = 0x01;
    out->port = 5683;
    out->generation = 1;
    return true;
}

static const parent_service_context_t s_parent_ctx = {
    .provisioning_allowed = pt_provisioning_allowed,
    .get_own_depth = pt_get_own_depth,
    .get_own_uuid = pt_get_own_uuid,
    .role_name = pt_role_name,
    .is_power_base = true,
    .get_collector = pt_get_collector,
    .ack_timeout_ms = 0,
    .ctx = NULL,
};

static void part_a_real_end_to_end(void)
{
    printf("\n-- PART A: real child_service+parent_service+sensor_service+pms5003+pogo_port x2, end to end --\n");

    host_gpio_test_reset();
    host_uart_test_reset();
    host_thread_manager_test_reset();
    host_role_manager_test_set_role(MODULE_ROLE_LEAF);

    pthread_create(&s_bridge_c2p, NULL, bridge_child_to_parent, NULL);
    pthread_create(&s_bridge_p2c, NULL, bridge_parent_to_child, NULL);

    /* PASSIVE mode, matching configs/prototype_pms5003.json -- the fake
     * sensor answers each READ command with one valid frame. */
    host_pms5003_fake_sensor_start(PMS_UART, false, 10, 20, 30);

    CHECK(collector_endpoint_init() == ESP_OK, "collector_endpoint_init succeeds");
    sensor_provider_register(SENSOR_DRIVER_ID_PMS5003, &g_pms5003_provider_ops);

    module_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = MODULE_CONFIG_SCHEMA_VERSION;
    cfg.driver_id = SENSOR_DRIVER_ID_PMS5003;
    cfg.bus_type = MODULE_BUS_UART;
    cfg.bus_number = PMS_UART;
    cfg.i2c_sda_gpio = cfg.i2c_scl_gpio = -1;
    cfg.spi_mosi_gpio = cfg.spi_miso_gpio = cfg.spi_sclk_gpio = cfg.spi_cs_gpio = -1;
    cfg.uart_tx_gpio = 20;
    cfg.uart_rx_gpio = 21;
    cfg.uart_baud_rate = 9600;
    cfg.uart_mode = MODULE_UART_MODE_PASSIVE;
    /*
     * Scaled down from configs/prototype_pms5003.json's real 30000ms so this
     * host test finishes in a few seconds -- the MECHANISM under test
     * (elapsed time since init determines CONDITIONING -> OK, see
     * pms5003.c's pms5003_read_measurement()) is identical regardless of the
     * configured duration; the actual 30s production value is separately
     * just a config data point, unverified on real hardware by this test
     * (see this session's final report).
     */
    cfg.uart_warmup_ms = PMS_WARMUP_MS;
    cfg.measurement_timeout_ms = 500;
    cfg.downstream_port.uart_num = -1; /* required: PMS5003 is terminal-only */

    esp_err_t err = sensor_service_init(&cfg);
    CHECK(err == ESP_OK, "sensor_service_init (real pms5003.c) succeeds");
    CHECK(sensor_service_get_status() == SENSOR_STATUS_CONDITIONING,
          "immediately after init, the real driver reports CONDITIONING (within the warm-up window)");

    err = child_service_init(&cfg);
    CHECK(err == ESP_OK, "child_service_init succeeds");
    err = parent_service_init(&s_parent_ctx);
    CHECK(err == ESP_OK, "parent_service_init succeeds");

    pogo_port_config_t child_port_cfg = {
        .enabled = true, .uart_num = CHILD_UART, .tx_gpio = 10, .rx_gpio = 11,
        .detect_in_gpio = -1, .detect_out_gpio = -1, .base_id_gpio = -1,
        .detect_out_active_low = true, .debounce_ms = 10, .baud_rate = 115200,
    };
    pogo_port_config_t parent_port_cfg = {
        .enabled = true, .uart_num = PARENT_UART, .tx_gpio = 30, .rx_gpio = 31,
        .detect_in_gpio = -1, .detect_out_gpio = -1, .base_id_gpio = -1,
        .detect_out_active_low = true, .debounce_ms = 10, .baud_rate = 115200,
    };
    CHECK(pogo_port_init(POGO_PORT_UPSTREAM, &child_port_cfg, test_port_event_dispatcher, NULL) == ESP_OK,
          "child-side pogo_port_init");
    CHECK(pogo_port_init(POGO_PORT_DOWNSTREAM, &parent_port_cfg, test_port_event_dispatcher, NULL) == ESP_OK,
          "parent-side pogo_port_init");
    CHECK(pogo_port_enable(POGO_PORT_UPSTREAM, POGO_PEER_TYPE_MODULE) == ESP_OK, "child-side pogo_port_enable");
    CHECK(pogo_port_enable(POGO_PORT_DOWNSTREAM, POGO_PEER_TYPE_POWER_BASE) == ESP_OK, "parent-side pogo_port_enable");

    /* Real HELLO handshake (dev-compat/no-DETECT mode on both sides, so both
     * start immediately), then the real full provisioning sequence, all the
     * way through JOIN_RESULT_ACK -- polled for up to a few seconds. */
    bool saw_join_ack = false;
    for (int waited_ms = 0; waited_ms < 5000; waited_ms += 20) {
        pthread_mutex_lock(&s_observe_mutex);
        saw_join_ack = s_saw_join_result_ack;
        pthread_mutex_unlock(&s_observe_mutex);
        if (saw_join_ack) {
            break;
        }
        usleep(20 * 1000);
    }

    pthread_mutex_lock(&s_observe_mutex);
    bool saw_sensor_ack = s_saw_sensor_info_ack;
    uint8_t observed_status = s_sensor_info_ack_status;
    pthread_mutex_unlock(&s_observe_mutex);

    CHECK(saw_sensor_ack, "a real SENSOR_INFO_ACK crossed the wire during provisioning");
    CHECK(observed_status == SENSOR_STATUS_CONDITIONING,
          "the child's real SENSOR_INFO_ACK carried sensor_status=CONDITIONING (still warming up)");
    CHECK(saw_join_ack,
          "the parent accepted CONDITIONING and drove the ENTIRE real sequence through to JOIN_RESULT_ACK "
          "(POLL_CONFIG, SET_COLLECTOR, SET_DATASET, and a real Thread join all happened)");

    /*
     * This follow-up (SED CoAP response timing correction, real-hardware
     * finding: a LEAF's own Thread SED data-poll period must never be
     * derived from its sensor measurement period): the real parent_service.c
     * computed and sent a real, dynamically-derived polling_period_ms over
     * the wire (node_calc_polling_period_ms(), from a REAL measured
     * response_time_ms in this same process -- not a hardcoded literal, so
     * this proves the general decoupling property rather than one magic
     * number). The real child_service.c must accept that value for its OWN
     * sensor measurement period, while passing thread_data_poll_period_ms=0
     * (this component's Kconfig default -- see
     * CONFIG_THREAD_MANAGER_LEAF_DATA_POLL_PERIOD_MS,
     * components/thread_manager) to the REAL
     * thread_manager_join_with_dataset() call -- never the sensor period.
     */
    pthread_mutex_lock(&s_observe_mutex);
    bool saw_poll_config = s_saw_poll_config;
    uint32_t sent_polling_period_ms = s_poll_config_period_ms;
    uint16_t dataset_len_observed = s_dataset_len_from_set_dataset;
    pthread_mutex_unlock(&s_observe_mutex);

    CHECK(saw_poll_config, "a real POGO_MSG_POLL_CONFIG crossed the wire during provisioning");
    CHECK(sent_polling_period_ms > 0,
          "the real parent_service.c sent a real, dynamically-computed (non-zero) sensor measurement period");
    CHECK(host_thread_manager_test_get_join_call_count() >= 1,
          "the real child_service.c actually called thread_manager_join_with_dataset() at least once");
    CHECK(host_thread_manager_test_get_last_poll_period_ms() == 0,
          "child_service.c passes thread_data_poll_period_ms=0 (this component's own Kconfig default resolution, "
          "CONFIG_THREAD_MANAGER_LEAF_DATA_POLL_PERIOD_MS) to thread_manager_join_with_dataset() -- NEVER the "
          "sensor's own polling_period_ms, regardless of what value POLL_CONFIG carried");
    CHECK(sent_polling_period_ms != host_thread_manager_test_get_last_poll_period_ms(),
          "the sensor measurement period actually used (from the wire) and the Thread data poll period actually "
          "applied are never the same value -- the two are genuinely decoupled, not just renamed");
    CHECK(dataset_len_observed > 0, "a real, non-empty SET_DATASET payload crossed the wire");
    CHECK(host_thread_manager_test_get_last_dataset_len() == dataset_len_observed,
          "the Dataset TLV length passed to thread_manager_join_with_dataset() still matches the real "
          "SET_DATASET payload actually received -- decoupling the poll period changed nothing about Dataset "
          "delivery itself");

    /* Bullet: after the warm-up window elapses, the SAME session's driver
     * autonomously reports OK -- no re-provisioning, no reboot. */
    usleep((PMS_WARMUP_MS + 300) * 1000);
    sensor_measurement_t meas;
    err = sensor_service_measure_once(&meas, 500);
    CHECK(err == ESP_OK, "a later measurement in the SAME session still succeeds");
    CHECK(sensor_service_get_status() == SENSOR_STATUS_OK,
          "once the warm-up window has elapsed, status becomes OK automatically, in the same session");

    s_bridge_running = false;
    pthread_join(s_bridge_c2p, NULL);
    pthread_join(s_bridge_p2c, NULL);
    host_pms5003_fake_sensor_stop();
    pogo_port_disable(POGO_PORT_UPSTREAM);
    pogo_port_disable(POGO_PORT_DOWNSTREAM);
}

/* --- PART B: parent_service.c alone (real, reusing s_parent_ctx above)
 * against a scripted fake child, for the two ends PART A cannot force:
 * a genuine sensor ERROR (still rejected) and ordinary OK (no regression
 * for SHT45/BH1750/SGP40/SGP41, which never report anything but OK). --- */

static bool fake_wait_frame(int uart_num, uint8_t want_type, uint32_t timeout_ms, pogo_frame_t *out)
{
    pogo_frame_parser_t parser;
    pogo_frame_parser_init(&parser);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint32_t elapsed = (uint32_t)((now.tv_sec - start.tv_sec) * 1000L + (now.tv_nsec - start.tv_nsec) / 1000000L);
        if (elapsed >= timeout_ms) {
            return false;
        }
        uint8_t b;
        if (!host_uart_test_take_tx_byte(uart_num, &b, 20)) {
            continue;
        }
        if (pogo_frame_parser_feed(&parser, b, out) && out->type == want_type) {
            return true;
        }
    }
}

static uint32_t elapsed_ms_since_ts(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((now.tv_sec - start->tv_sec) * 1000L + (now.tv_nsec - start->tv_nsec) / 1000000L);
}

/* Waits up to timeout_ms for ANY complete frame (no type filter) -- used to
 * confirm either "something arrived" or, combined with a PING/PONG filter at
 * the call site, "nothing but idle-liveness traffic arrived". */
static bool fake_wait_frame_short(int uart_num, uint32_t timeout_ms, pogo_frame_t *out)
{
    pogo_frame_parser_t parser;
    pogo_frame_parser_init(&parser);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (elapsed_ms_since_ts(&start) < timeout_ms) {
        uint8_t b;
        if (!host_uart_test_take_tx_byte(uart_num, &b, 20)) {
            continue;
        }
        if (pogo_frame_parser_feed(&parser, b, out)) {
            return true;
        }
    }
    return false;
}

static void fake_send(int uart_num, uint8_t type, uint8_t seq, const uint8_t *payload, size_t len)
{
    uint8_t buf[POGO_FRAME_MAX_SIZE];
    size_t n = pogo_frame_encode(type, seq, payload, len, buf, sizeof(buf));
    host_uart_test_push_rx(uart_num, buf, n);
}

static void fake_answer_identify_through_sensor_hello(pogo_frame_t *frame, uint8_t sensor_status)
{
    uint8_t buf[64];
    size_t n;
    pogo_identify_resp_t ident = {.mac = {1, 2, 3, 4, 5, 6}, .fw_version = 1, .sensor_bitmap = 0x07};
    n = pogo_pack_identify_resp(&ident, buf, sizeof(buf));
    fake_send(PARENT_UART, POGO_MSG_IDENTIFY_RESP, frame->seq, buf, n);

    if (!fake_wait_frame(PARENT_UART, POGO_MSG_REQUEST_UUID, 500, frame)) return;
    pogo_propose_uuid_t prop;
    memset(prop.uuid, 0, POGO_UUID_LEN);
    prop.uuid[7] = 0x42;
    n = pogo_pack_propose_uuid(&prop, buf, sizeof(buf));
    fake_send(PARENT_UART, POGO_MSG_PROPOSE_UUID, frame->seq, buf, n);

    if (!fake_wait_frame(PARENT_UART, POGO_MSG_ASSIGN_UUID, 500, frame)) return;
    pogo_ack_t ack = {.acked_type = POGO_MSG_ASSIGN_UUID};
    n = pogo_pack_ack(&ack, buf, sizeof(buf));
    fake_send(PARENT_UART, POGO_MSG_ACK, frame->seq, buf, n);

    if (!fake_wait_frame(PARENT_UART, POGO_MSG_ASSIGN_SLOT, 500, frame)) return;
    ack.acked_type = POGO_MSG_ASSIGN_SLOT;
    n = pogo_pack_ack(&ack, buf, sizeof(buf));
    fake_send(PARENT_UART, POGO_MSG_ACK, frame->seq, buf, n);

    if (!fake_wait_frame(PARENT_UART, POGO_MSG_ASSIGN_PARENT_INFO, 500, frame)) return;
    ack.acked_type = POGO_MSG_ASSIGN_PARENT_INFO;
    n = pogo_pack_ack(&ack, buf, sizeof(buf));
    fake_send(PARENT_UART, POGO_MSG_ACK, frame->seq, buf, n);

    if (!fake_wait_frame(PARENT_UART, POGO_MSG_SENSOR_HELLO, 500, frame)) return;
    pogo_sensor_info_ack_t sinfo = {
        .sensor_type_bitmap = 0x07, .response_time_ms = 50, .fw_version = 1, .sensor_status = sensor_status};
    n = pogo_pack_sensor_info_ack(&sinfo, buf, sizeof(buf));
    fake_send(PARENT_UART, POGO_MSG_SENSOR_INFO_ACK, frame->seq, buf, n);
}

/*
 * For a REJECTED sensor_status: parent_service.c's own retry policy (see
 * run_provisioning_session()'s caller in parent_service_task()) restarts the
 * WHOLE sequence from IDENTIFY_REQ every CONFIG_POGO_PING_INTERVAL_MS while
 * the peer is still docked -- so simply "no more frames arrive" is the WRONG
 * check (a retried IDENTIFY_REQ is expected and legitimate). What must hold
 * is narrower: the sequence never PROGRESSES PAST SENSOR_HELLO to
 * POLL_CONFIG (or later) on any of those retries, for the whole window.
 */
static bool confirm_sensor_error_blocks_progress(uint8_t sensor_status, uint32_t total_timeout_ms)
{
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (elapsed_ms_since_ts(&start) < total_timeout_ms) {
        pogo_frame_t frame;
        if (!fake_wait_frame_short(PARENT_UART, 100, &frame)) {
            continue;
        }
        switch (frame.type) {
        case POGO_MSG_PING:
            fake_send(PARENT_UART, POGO_MSG_PONG, frame.seq, NULL, 0);
            break;
        case POGO_MSG_IDENTIFY_REQ:
            fake_answer_identify_through_sensor_hello(&frame, sensor_status);
            break;
        default:
            return false; /* something past the gate arrived -- it did NOT hold */
        }
    }
    return true;
}

/* Drives the real parent_service.c through one full, successful session over
 * POGO_PORT_DOWNSTREAM, playing "the child" by hand with the real
 * pogo_proto pack/unpack functions -- used only for the sensor_status values
 * that must let provisioning continue (OK/CONDITIONING). */
static bool fake_child_run_session(uint8_t sensor_status)
{
    pogo_frame_t frame;
    uint8_t buf[64];
    size_t n;

    /* pogo_port's own PEER_HELLO handshake must complete first -- parent_service
     * only starts the provisioning sequence once its DOWNSTREAM port reaches
     * PEER_READY (see pogo_port.c). */
    if (!fake_wait_frame(PARENT_UART, POGO_MSG_PEER_HELLO_REQ, 500, &frame)) return false;
    pogo_peer_hello_t hello = {.protocol_version = POGO_FRAME_VERSION, .peer_type = POGO_PEER_TYPE_MODULE,
                                .capabilities = POGO_HELLO_SUPPORTED_CAPABILITIES};
    n = pogo_pack_peer_hello(&hello, buf, sizeof(buf));
    fake_send(PARENT_UART, POGO_MSG_PEER_HELLO_RESP, frame.seq, buf, n);

    if (!fake_wait_frame(PARENT_UART, POGO_MSG_IDENTIFY_REQ, 500, &frame)) return false;
    fake_answer_identify_through_sensor_hello(&frame, sensor_status);
    /* frame now holds SENSOR_HELLO's own seq (fake_answer_identify_through_sensor_hello
     * leaves it at whatever the last fake_wait_frame() inside it matched). */

    if (!fake_wait_frame(PARENT_UART, POGO_MSG_POLL_CONFIG, 500, &frame)) return false;
    pogo_ack_t ack = {.acked_type = POGO_MSG_POLL_CONFIG};
    n = pogo_pack_ack(&ack, buf, sizeof(buf));
    fake_send(PARENT_UART, POGO_MSG_ACK, frame.seq, buf, n);

    if (!fake_wait_frame(PARENT_UART, POGO_MSG_SET_COLLECTOR, 500, &frame)) return false;
    ack.acked_type = POGO_MSG_SET_COLLECTOR;
    n = pogo_pack_ack(&ack, buf, sizeof(buf));
    fake_send(PARENT_UART, POGO_MSG_ACK, frame.seq, buf, n);

    if (!fake_wait_frame(PARENT_UART, POGO_MSG_SET_DATASET, 500, &frame)) return false;
    ack.acked_type = POGO_MSG_SET_DATASET;
    n = pogo_pack_ack(&ack, buf, sizeof(buf));
    fake_send(PARENT_UART, POGO_MSG_ACK, frame.seq, buf, n);

    pogo_join_result_t jr = {.success = 1, .role = (uint8_t)OT_DEVICE_ROLE_CHILD};
    n = pogo_pack_join_result(&jr, buf, sizeof(buf));
    const uint8_t jr_seq = 77;
    fake_send(PARENT_UART, POGO_MSG_JOIN_RESULT, jr_seq, buf, n);

    return fake_wait_frame(PARENT_UART, POGO_MSG_JOIN_RESULT_ACK, 500, &frame) && frame.seq == jr_seq;
}

static void run_part_b_ok_session(const char *label, uint8_t sensor_status)
{
    printf("\n-- PART B: %s --\n", label);

    pogo_port_config_t parent_port_cfg = {
        .enabled = true, .uart_num = PARENT_UART, .tx_gpio = 30, .rx_gpio = 31,
        .detect_in_gpio = -1, .detect_out_gpio = -1, .base_id_gpio = -1,
        .detect_out_active_low = true, .debounce_ms = 10, .baud_rate = 115200,
    };
    CHECK(pogo_port_init(POGO_PORT_DOWNSTREAM, &parent_port_cfg, test_port_event_dispatcher, NULL) == ESP_OK,
          "pogo_port_init (parent side) for this session");
    CHECK(pogo_port_enable(POGO_PORT_DOWNSTREAM, POGO_PEER_TYPE_POWER_BASE) == ESP_OK, "pogo_port_enable");

    bool ok = fake_child_run_session(sensor_status);
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: full sequence reaches JOIN_RESULT_ACK", label);
    CHECK(ok, msg);

    pogo_port_disable(POGO_PORT_DOWNSTREAM);
}

static void run_part_b_error_session(const char *label, uint8_t sensor_status)
{
    printf("\n-- PART B: %s --\n", label);

    pogo_port_config_t parent_port_cfg = {
        .enabled = true, .uart_num = PARENT_UART, .tx_gpio = 30, .rx_gpio = 31,
        .detect_in_gpio = -1, .detect_out_gpio = -1, .base_id_gpio = -1,
        .detect_out_active_low = true, .debounce_ms = 10, .baud_rate = 115200,
    };
    CHECK(pogo_port_init(POGO_PORT_DOWNSTREAM, &parent_port_cfg, test_port_event_dispatcher, NULL) == ESP_OK,
          "pogo_port_init (parent side) for this session");
    CHECK(pogo_port_enable(POGO_PORT_DOWNSTREAM, POGO_PEER_TYPE_POWER_BASE) == ESP_OK, "pogo_port_enable");

    pogo_frame_t frame;
    uint8_t buf[8];
    bool got_hello = fake_wait_frame(PARENT_UART, POGO_MSG_PEER_HELLO_REQ, 500, &frame);
    CHECK(got_hello, "PEER_HELLO_REQ received");
    if (got_hello) {
        pogo_peer_hello_t hello = {.protocol_version = POGO_FRAME_VERSION, .peer_type = POGO_PEER_TYPE_MODULE,
                                    .capabilities = POGO_HELLO_SUPPORTED_CAPABILITIES};
        size_t n = pogo_pack_peer_hello(&hello, buf, sizeof(buf));
        fake_send(PARENT_UART, POGO_MSG_PEER_HELLO_RESP, frame.seq, buf, n);
    }

    bool ok = got_hello && confirm_sensor_error_blocks_progress(sensor_status, 600);
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: the sequence never progresses past SENSOR_HELLO, across retries", label);
    CHECK(ok, msg);

    pogo_port_disable(POGO_PORT_DOWNSTREAM);
}

int main(void)
{
    part_a_real_end_to_end();

    run_part_b_error_session("a real sensor ERROR status (e.g. BUS_ERROR) is still rejected -- provisioning "
                              "abandoned",
                              3 /* SENSOR_STATUS_BUS_ERROR */);
    run_part_b_ok_session("ordinary sensor_status==OK path (representative of SHT45/BH1750/SGP40/SGP41) -- no "
                           "regression",
                           0 /* SENSOR_STATUS_OK */);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
