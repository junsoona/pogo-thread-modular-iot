/*
 * Host-compiled end-to-end regression test for DOWNSTREAM transient-error
 * isolation in an AUTO sensor module (docs/state-machine.md, "DOWNSTREAM
 * 일시 오류 격리").
 *
 * Real-hardware finding this reproduces: Power Base -> SGP41 module -> PMS
 * module. The SGP41 was joined and reporting; the moment the PMS docked and
 * its first handshake timed out, the SGP41 logged DOCK_TIMEOUT, went
 * LEAF -> ROLE_ERROR, stopped its OWN Thread membership, and its sample
 * counter on the hub froze -- while both ports kept retrying every cooldown.
 *
 * This is "one sensor module in a box": the REAL, UNMODIFIED production
 * sources below run exactly as on hardware, on the pthread-backed FreeRTOS
 * stand-in and the in-memory GPIO/UART fakes introduced for
 * tools/test_pogo_port_hotswap_recovery.c --
 *
 *   components/pogo_port (both ports), pogo_proto, role_manager,
 *   module/main/module_wiring.c (the dispatcher / role-change reaction /
 *   provisioning gate app_main.c registers), child_service, parent_service,
 *   sensor_service (+ the mock sensor_provider), sensor_transport,
 *   topology_reporter, topology_gate, collector_endpoint, and -- on the fake
 *   power base's side -- topology_registry.
 *
 * Nothing in this file decides a role, a gate, or a connection state. It
 * only plays the three things that are not this module:
 *   - the power base docked UPSTREAM  (answers HELLO/PING, drives the
 *     provisioning sequence, built with the real pogo_proto packers),
 *   - the module docked DOWNSTREAM    (answers HELLO/PING and
 *     parent_service's requests, or goes silent / answers a legacy HELLO),
 *   - the Thread network + the power base's CoAP resources ("/t" reports
 *     are decoded with the real topology_report_unpack() and applied to a
 *     real topology_registry; "/s" samples are counted), reachable only
 *     while this module's Thread interface is up,
 * plus a thread_manager stand-in that records join/stop calls (there is no
 * host OpenThread stack).
 *
 * The module boots ONCE (its service tasks have no teardown), so the
 * scenarios are phases of one continuous story -- see main().
 *
 * Build & run (from the repo root; takes ~20s of real wall-clock time):
 *   cc -Wall -Wextra -pthread \
 *      -I tools/host_test_stubs/pogo_fast -I tools/host_test_stubs -I module/main \
 *      -I components/pogo_port/include -I components/pogo_proto/include \
 *      -I components/role_manager/include -I components/module_config/include \
 *      -I components/child_service/include -I components/parent_service/include \
 *      -I components/sensor_service/include -I components/sensor_provider/include \
 *      -I components/sensor_channel/include -I components/sensor_root_aggregator/include \
 *      -I components/sensor_transport/include -I components/thread_manager/include \
 *      -I components/collector_endpoint/include -I components/node_calc/include \
 *      -I components/topology_gate/include -I components/topology_reporter/include \
 *      -I components/topology_proto/include -I components/topology_registry/include \
 *      -I components/coap_response_timing/include \
 *      -o /tmp/test_module_downstream_fault_isolation tools/test_module_downstream_fault_isolation.c \
 *      components/pogo_port/pogo_port.c components/pogo_port/pogo_port_cleanup.c \
 *      components/pogo_proto/pogo_frame.c components/pogo_proto/pogo_crc16.c \
 *      components/pogo_proto/pogo_protocol.c \
 *      components/role_manager/role_manager.c module/main/module_wiring.c \
 *      components/child_service/child_service.c components/parent_service/parent_service.c \
 *      components/sensor_service/sensor_service.c components/sensor_provider/sensor_provider.c \
 *      components/sensor_provider/mock_sensor_provider.c \
 *      components/node_calc/node_calc.c components/sensor_root_aggregator/sensor_root_aggregator.c \
 *      components/sensor_channel/sensor_channel.c components/collector_endpoint/collector_endpoint.c \
 *      components/topology_gate/topology_gate.c components/topology_reporter/topology_reporter.c \
 *      components/topology_proto/topology_proto.c components/topology_registry/topology_registry.c \
 *      components/sensor_transport/sensor_transport.c \
 *      components/coap_response_timing/coap_response_timing.c \
 *      tools/host_test_stubs/host_gpio_sim.c tools/host_test_stubs/host_uart_sim.c \
 *   && /tmp/test_module_downstream_fault_isolation
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/coap.h"
#include "openthread/error.h"

#include "pogo_port/pogo_port.h"
#include "pogo_proto/pogo_frame.h"
#include "pogo_proto/pogo_protocol.h"
#include "module_config/module_config.h"
#include "sensor_provider/sensor_provider.h"
#include "sensor_service/sensor_service.h"
#include "sensor_transport/sensor_transport.h"
#include "role_manager/role_manager.h"
#include "thread_manager/thread_manager.h"
#include "child_service/child_service.h"
#include "parent_service/parent_service.h"
#include "topology_reporter/topology_reporter.h"
#include "topology_gate/topology_gate.h"
#include "topology_proto/topology_proto.h"
#include "topology_registry/topology_registry.h"
#include "collector_endpoint/collector_endpoint.h"
#include "module_wiring.h"

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
        fflush(stdout);                                                                                              \
    } while (0)

#define UP_UART 0
#define DOWN_UART 1
#define UP_DETECT_IN 12
#define UP_DETECT_OUT 2
#define DOWN_DETECT_IN 4
#define DOWN_DETECT_OUT 5

static const uint8_t PB_UUID[POGO_UUID_LEN] = {0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x01};
static const uint8_t CHILD_UUID[POGO_UUID_LEN] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x02};

static uint32_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((uint64_t)now.tv_sec * 1000ULL + (uint64_t)now.tv_nsec / 1000000ULL);
}

static void sleep_ms(uint32_t ms)
{
    usleep(ms * 1000u);
}

static bool wait_until(bool (*pred)(void), uint32_t timeout_ms)
{
    uint32_t start = now_ms();
    while (now_ms() - start <= timeout_ms) {
        if (pred()) {
            return true;
        }
        sleep_ms(5);
    }
    return pred();
}

/* ====================================================================
 * thread_manager stand-in: records what the real services asked for.
 * ==================================================================== */

static pthread_mutex_t s_tm_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool s_tm_enabled; /* this module's Thread interface is up (joined, not stopped) */
static otDeviceRole s_tm_role = OT_DEVICE_ROLE_DISABLED;
static bool s_tm_role_change_pending;
static int s_tm_stop_calls;
static int s_tm_join_calls;
static int s_tm_join_leaf_calls;
static int s_tm_join_intermediate_calls;

bool thread_manager_validate_dataset_tlvs(const uint8_t *dataset_tlvs, uint16_t dataset_len)
{
    return dataset_tlvs != NULL && dataset_len > 0;
}

esp_err_t thread_manager_join_with_dataset(thread_manager_role_t role, const uint8_t *dataset_tlvs,
                                            uint16_t dataset_len, uint32_t thread_data_poll_period_ms)
{
    (void)dataset_tlvs;
    (void)dataset_len;
    (void)thread_data_poll_period_ms;
    pthread_mutex_lock(&s_tm_mutex);
    s_tm_join_calls++;
    if (role == THREAD_MANAGER_ROLE_LEAF) {
        s_tm_join_leaf_calls++;
    } else {
        s_tm_join_intermediate_calls++;
    }
    s_tm_enabled = true;
    s_tm_role = OT_DEVICE_ROLE_CHILD;
    s_tm_role_change_pending = true;
    pthread_mutex_unlock(&s_tm_mutex);
    return ESP_OK;
}

bool thread_manager_poll_role_change(otDeviceRole *out_role)
{
    pthread_mutex_lock(&s_tm_mutex);
    bool has = s_tm_role_change_pending;
    if (has) {
        *out_role = s_tm_role;
        s_tm_role_change_pending = false;
    }
    pthread_mutex_unlock(&s_tm_mutex);
    return has;
}

otDeviceRole thread_manager_get_role(void)
{
    pthread_mutex_lock(&s_tm_mutex);
    otDeviceRole r = s_tm_role;
    pthread_mutex_unlock(&s_tm_mutex);
    return r;
}

esp_err_t thread_manager_get_active_dataset_tlvs(otOperationalDatasetTlvs *out_tlvs)
{
    memset(out_tlvs->mTlvs, 0xAA, 4);
    out_tlvs->mLength = 4;
    return ESP_OK;
}

esp_err_t thread_manager_stop(void)
{
    pthread_mutex_lock(&s_tm_mutex);
    s_tm_stop_calls++;
    s_tm_enabled = false;
    s_tm_role = OT_DEVICE_ROLE_DISABLED;
    s_tm_role_change_pending = false;
    pthread_mutex_unlock(&s_tm_mutex);
    return ESP_OK;
}

esp_err_t thread_manager_start_as_root(void)
{
    return ESP_OK; /* DEV_ROOT fixture only -- never reached in AUTO mode */
}

esp_err_t thread_manager_get_mesh_local_eid(otIp6Address *out_addr)
{
    memset(out_addr, 0, sizeof(*out_addr));
    return ESP_FAIL; /* DEV_ROOT fixture only */
}

void thread_manager_check_parent_for_sample(otInstance *instance, uint32_t sample_seq)
{
    (void)instance;
    (void)sample_seq;
}

const char *thread_manager_role_to_str(otDeviceRole role)
{
    switch (role) {
    case OT_DEVICE_ROLE_DISABLED: return "DISABLED";
    case OT_DEVICE_ROLE_DETACHED: return "DETACHED";
    case OT_DEVICE_ROLE_CHILD: return "CHILD";
    case OT_DEVICE_ROLE_ROUTER: return "ROUTER";
    case OT_DEVICE_ROLE_LEADER: return "LEADER";
    default: return "UNKNOWN";
    }
}

static int tm_stop_calls(void)
{
    pthread_mutex_lock(&s_tm_mutex);
    int v = s_tm_stop_calls;
    pthread_mutex_unlock(&s_tm_mutex);
    return v;
}

static int tm_join_calls(void)
{
    pthread_mutex_lock(&s_tm_mutex);
    int v = s_tm_join_calls;
    pthread_mutex_unlock(&s_tm_mutex);
    return v;
}

static int tm_join_leaf_calls(void)
{
    pthread_mutex_lock(&s_tm_mutex);
    int v = s_tm_join_leaf_calls;
    pthread_mutex_unlock(&s_tm_mutex);
    return v;
}

static int tm_join_intermediate_calls(void)
{
    pthread_mutex_lock(&s_tm_mutex);
    int v = s_tm_join_intermediate_calls;
    pthread_mutex_unlock(&s_tm_mutex);
    return v;
}

static bool tm_enabled(void)
{
    pthread_mutex_lock(&s_tm_mutex);
    bool v = s_tm_enabled;
    pthread_mutex_unlock(&s_tm_mutex);
    return v;
}

/* ====================================================================
 * The Thread network + the power base's CoAP resources.
 * A request only gets through while this module's Thread interface is up;
 * "/t" goes into a REAL topology_registry, "/s" is counted.
 * ==================================================================== */

#define REPORT_LOG_CAP 256

static pthread_mutex_t s_pb_mutex = PTHREAD_MUTEX_INITIALIZER;
static topology_registry_t s_registry;
static topology_report_t s_reports[REPORT_LOG_CAP]; /* every "/t" report the registry accepted, in order */
static int s_report_count;
static int s_sample_count; /* "/s" samples delivered */

struct otMessage {
    otCoapType type;
    otCoapCode code;
    char uri[8];
    uint8_t payload[160];
    uint16_t len;
};

otInstance *esp_openthread_get_instance(void)
{
    static int dummy_instance;
    return (otInstance *)&dummy_instance;
}

bool esp_openthread_lock_acquire(uint32_t timeout_ticks)
{
    (void)timeout_ticks;
    return true;
}

void esp_openthread_lock_release(void)
{
}

const char *otThreadErrorToString(otError error)
{
    return (error == OT_ERROR_NONE) ? "None" : "Error";
}

otError otCoapStart(otInstance *instance, uint16_t port)
{
    (void)instance;
    (void)port;
    return OT_ERROR_NONE;
}

void otCoapAddResource(otInstance *instance, otCoapResource *resource)
{
    (void)instance;
    (void)resource;
}

otMessage *otCoapNewMessage(otInstance *instance, const void *settings)
{
    (void)instance;
    (void)settings;
    return (otMessage *)calloc(1, sizeof(otMessage));
}

void otMessageFree(otMessage *message)
{
    free(message);
}

void otCoapMessageInit(otMessage *message, otCoapType type, otCoapCode code)
{
    message->type = type;
    message->code = code;
}

otError otCoapMessageAppendUriPathOptions(otMessage *message, const char *uriPath)
{
    snprintf(message->uri, sizeof(message->uri), "%s", uriPath);
    return OT_ERROR_NONE;
}

otError otCoapMessageSetPayloadMarker(otMessage *message)
{
    (void)message;
    return OT_ERROR_NONE;
}

otError otMessageAppend(otMessage *message, const void *data, uint16_t length)
{
    if ((size_t)message->len + length > sizeof(message->payload)) {
        return OT_ERROR_NO_BUFS;
    }
    memcpy(message->payload + message->len, data, length);
    message->len = (uint16_t)(message->len + length);
    return OT_ERROR_NONE;
}

otCoapCode otCoapMessageGetCode(const otMessage *message)
{
    return message->code;
}

otCoapType otCoapMessageGetType(const otMessage *message)
{
    return message->type;
}

uint16_t otMessageGetLength(const otMessage *message)
{
    return message->len;
}

uint16_t otMessageGetOffset(const otMessage *message)
{
    (void)message;
    return 0;
}

uint16_t otMessageRead(const otMessage *message, uint16_t offset, void *buf, uint16_t length)
{
    if (offset >= message->len) {
        return 0;
    }
    uint16_t n = (uint16_t)(message->len - offset);
    if (n > length) {
        n = length;
    }
    memcpy(buf, message->payload + offset, n);
    return n;
}

otError otCoapMessageInitResponse(otMessage *response, const otMessage *request, otCoapType type, otCoapCode code)
{
    (void)request;
    response->type = type;
    response->code = code;
    return OT_ERROR_NONE;
}

otError otCoapSendResponse(otInstance *instance, otMessage *message, const otMessageInfo *messageInfo)
{
    (void)instance;
    (void)messageInfo;
    free(message);
    return OT_ERROR_NONE;
}

otError otCoapSendRequestWithParameters(otInstance *instance, otMessage *message, const otMessageInfo *messageInfo,
                                         otCoapResponseHandler handler, void *context,
                                         const otCoapTxParameters *txParameters)
{
    (void)instance;
    (void)txParameters;
    if (!tm_enabled()) {
        /* Thread interface down: the request never leaves this module (the
         * caller keeps ownership of `message` and frees it, as with a real
         * send-level failure). */
        return OT_ERROR_NO_ROUTE;
    }

    otCoapCode code = OT_COAP_CODE_BAD_REQUEST;
    pthread_mutex_lock(&s_pb_mutex);
    if (strcmp(message->uri, "t") == 0) {
        topology_report_t rep;
        if (topology_report_unpack(message->payload, message->len, &rep)) {
            topology_registry_result_t r = topology_registry_apply_report(&s_registry, &rep, PB_UUID, now_ms());
            if (topology_registry_result_is_ok(r)) {
                code = OT_COAP_CODE_CHANGED;
                if (s_report_count < REPORT_LOG_CAP) {
                    s_reports[s_report_count++] = rep;
                }
            } else {
                code = OT_COAP_CODE_PRECONDITION_FAILED;
            }
        }
    } else if (strcmp(message->uri, "s") == 0) {
        s_sample_count++;
        code = OT_COAP_CODE_CHANGED;
    }
    pthread_mutex_unlock(&s_pb_mutex);

    /* Synchronous delivery of the response -- see
     * tools/host_test_stubs/host_ot_coap_sim.c's header comment for why this
     * is equivalent to the real asynchronous callback for both senders. */
    otMessage response = {.type = OT_COAP_TYPE_ACKNOWLEDGMENT, .code = code};
    handler(context, &response, messageInfo, OT_ERROR_NONE);
    free(message);
    return OT_ERROR_NONE;
}

static int sample_count(void)
{
    pthread_mutex_lock(&s_pb_mutex);
    int v = s_sample_count;
    pthread_mutex_unlock(&s_pb_mutex);
    return v;
}

static int report_count(void)
{
    pthread_mutex_lock(&s_pb_mutex);
    int v = s_report_count;
    pthread_mutex_unlock(&s_pb_mutex);
    return v;
}

/* Index of the first accepted "/t" report at or after `from` whose
 * downstream_connected equals `connected`, or -1. */
static int find_report(int from, bool connected, topology_report_t *out)
{
    int found = -1;
    pthread_mutex_lock(&s_pb_mutex);
    for (int i = from; i < s_report_count; i++) {
        if (s_reports[i].downstream_connected == connected) {
            found = i;
            if (out != NULL) {
                *out = s_reports[i];
            }
            break;
        }
    }
    pthread_mutex_unlock(&s_pb_mutex);
    return found;
}

static bool registry_get(const uint8_t uuid[TOPOLOGY_UUID_LEN], topology_registry_entry_t *out)
{
    bool found = false;
    pthread_mutex_lock(&s_pb_mutex);
    for (size_t i = 0; i < TOPOLOGY_REGISTRY_MAX_NODES; i++) {
        topology_registry_entry_t e;
        if (topology_registry_get(&s_registry, i, &e) && memcmp(e.module_uuid, uuid, TOPOLOGY_UUID_LEN) == 0) {
            *out = e;
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&s_pb_mutex);
    return found;
}

/* The DOWNSTREAM module's OWN "/t" report, as its own firmware would send it
 * over Thread once joined (that module is not simulated beyond its POGO
 * face) -- applied to the same real registry the module under test reports
 * to, so the registry's verdict on the subtree is the real one. */
static topology_registry_result_t child_reports_to_power_base(const uint8_t parent_uuid[TOPOLOGY_UUID_LEN],
                                                              topology_report_type_t type)
{
    static uint32_t seq;
    topology_report_t rep = {0};
    rep.payload_version = TOPOLOGY_REPORT_PAYLOAD_VERSION;
    memcpy(rep.module_uuid, CHILD_UUID, TOPOLOGY_UUID_LEN);
    memcpy(rep.parent_uuid, parent_uuid, TOPOLOGY_UUID_LEN);
    rep.topology_depth = 2;
    rep.role = TOPOLOGY_ROLE_LEAF;
    rep.sensor_type_bitmap = 0x0001;
    rep.boot_session_generation = 7;
    rep.sequence = ++seq;
    rep.upstream_connected = true;
    rep.downstream_connected = false;
    rep.report_type = (uint8_t)type;
    pthread_mutex_lock(&s_pb_mutex);
    topology_registry_result_t r = topology_registry_apply_report(&s_registry, &rep, PB_UUID, now_ms());
    pthread_mutex_unlock(&s_pb_mutex);
    return r;
}

/* ====================================================================
 * Port-event log. Recorded AFTER the real dispatcher has run, so by the
 * time a waiter sees an event role_manager has already processed it.
 * ==================================================================== */

#define EVT_LOG_CAP 1024
typedef struct {
    pogo_port_id_t port;
    pogo_port_event_t event;
    pogo_port_error_t error;
} logged_evt_t;

static pthread_mutex_t s_evt_mutex = PTHREAD_MUTEX_INITIALIZER;
static logged_evt_t s_evt_log[EVT_LOG_CAP];
static int s_evt_count;

static void recording_dispatcher(pogo_port_id_t port_id, pogo_port_event_t event, const pogo_port_event_data_t *data,
                                  void *user_ctx)
{
    module_wiring_port_event_dispatcher(port_id, event, data, user_ctx); /* the REAL fan-out */
    if (event == POGO_PORT_EVENT_FRAME_RECEIVED) {
        return;
    }
    pthread_mutex_lock(&s_evt_mutex);
    if (s_evt_count < EVT_LOG_CAP) {
        s_evt_log[s_evt_count].port = port_id;
        s_evt_log[s_evt_count].event = event;
        s_evt_log[s_evt_count].error = (event == POGO_PORT_EVENT_ERROR) ? data->error_code : POGO_PORT_ERR_NONE;
        s_evt_count++;
    }
    pthread_mutex_unlock(&s_evt_mutex);
}

static int evt_mark(void)
{
    pthread_mutex_lock(&s_evt_mutex);
    int v = s_evt_count;
    pthread_mutex_unlock(&s_evt_mutex);
    return v;
}

static int count_events_since(int from, pogo_port_id_t port, pogo_port_event_t event, pogo_port_error_t error)
{
    int n = 0;
    pthread_mutex_lock(&s_evt_mutex);
    for (int i = from; i < s_evt_count; i++) {
        if (s_evt_log[i].port == port && s_evt_log[i].event == event && s_evt_log[i].error == error) {
            n++;
        }
    }
    pthread_mutex_unlock(&s_evt_mutex);
    return n;
}

static bool wait_for_events(int from, pogo_port_id_t port, pogo_port_event_t event, pogo_port_error_t error,
                             int at_least, uint32_t timeout_ms)
{
    uint32_t start = now_ms();
    while (now_ms() - start <= timeout_ms) {
        if (count_events_since(from, port, event, error) >= at_least) {
            return true;
        }
        sleep_ms(5);
    }
    return count_events_since(from, port, event, error) >= at_least;
}

/* ====================================================================
 * The two boards at the other ends of the POGO wires.
 * ==================================================================== */

#define PEER_INBOX_CAP 16

typedef struct {
    int uart;
    uint8_t peer_type;          /* what this board declares in HELLO */
    atomic_bool alive;          /* answers HELLO_REQ and PING at all (set by the test, read by peer_thread) */
    atomic_bool legacy_hello;   /* answers HELLO_REQ with a pre-capability 2-byte payload */
    bool auto_child;            /* answers parent_service's provisioning requests on its own */

    pthread_mutex_t mutex;
    pogo_frame_t inbox[PEER_INBOX_CAP]; /* non-HELLO/PING frames, for the scripted power base */
    int inbox_count;

    /* What the auto child observed from parent_service. */
    int identify_reqs;
    int provisioned; /* JOIN_RESULT_ACKs received: completed provisioning sessions */
    bool has_collector;
    pogo_collector_endpoint_t collector;
    uint8_t parent_uuid[POGO_UUID_LEN];
    uint8_t assigned_depth;
    uint8_t join_result_seq;

    pthread_t thread;
} fake_peer_t;

static fake_peer_t s_pb = {.uart = UP_UART, .peer_type = POGO_PEER_TYPE_POWER_BASE};
static fake_peer_t s_child = {.uart = DOWN_UART, .peer_type = POGO_PEER_TYPE_MODULE, .auto_child = true};

static void peer_send(fake_peer_t *p, uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    uint8_t buf[POGO_FRAME_MAX_SIZE];
    size_t n = pogo_frame_encode(type, seq, payload, len, buf, sizeof(buf));
    host_uart_test_push_rx(p->uart, buf, n);
}

static void peer_send_ack(fake_peer_t *p, uint8_t seq, uint8_t acked_type)
{
    pogo_ack_t ack = {.acked_type = acked_type};
    uint8_t payload[2];
    size_t len = pogo_pack_ack(&ack, payload, sizeof(payload));
    peer_send(p, POGO_MSG_ACK, seq, payload, (uint16_t)len);
}

/* A docked sensor module's side of the provisioning sequence
 * parent_service.c drives (the order child_service.c implements). */
static void auto_child_handle(fake_peer_t *p, const pogo_frame_t *f)
{
    uint8_t payload[16];
    size_t len;
    switch (f->type) {
    case POGO_MSG_IDENTIFY_REQ: {
        pthread_mutex_lock(&p->mutex);
        p->identify_reqs++;
        pthread_mutex_unlock(&p->mutex);
        pogo_identify_resp_t resp = {.mac = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE}, .fw_version = 1, .sensor_bitmap = 1};
        len = pogo_pack_identify_resp(&resp, payload, sizeof(payload));
        peer_send(p, POGO_MSG_IDENTIFY_RESP, f->seq, payload, (uint16_t)len);
        break;
    }
    case POGO_MSG_REQUEST_UUID: {
        pogo_propose_uuid_t msg;
        memcpy(msg.uuid, CHILD_UUID, POGO_UUID_LEN);
        len = pogo_pack_propose_uuid(&msg, payload, sizeof(payload));
        peer_send(p, POGO_MSG_PROPOSE_UUID, f->seq, payload, (uint16_t)len);
        break;
    }
    case POGO_MSG_ASSIGN_SLOT: {
        pogo_assign_slot_t slot;
        if (pogo_unpack_assign_slot(f->payload, f->len, &slot)) {
            pthread_mutex_lock(&p->mutex);
            p->assigned_depth = slot.topology_depth;
            pthread_mutex_unlock(&p->mutex);
        }
        peer_send_ack(p, f->seq, f->type);
        break;
    }
    case POGO_MSG_ASSIGN_PARENT_INFO: {
        pogo_assign_parent_info_t info;
        if (pogo_unpack_assign_parent_info(f->payload, f->len, &info)) {
            pthread_mutex_lock(&p->mutex);
            memcpy(p->parent_uuid, info.parent_uuid, POGO_UUID_LEN);
            pthread_mutex_unlock(&p->mutex);
        }
        peer_send_ack(p, f->seq, f->type);
        break;
    }
    case POGO_MSG_SENSOR_HELLO: {
        pogo_sensor_info_ack_t ack = {.sensor_type_bitmap = 0x0001, .response_time_ms = 100, .fw_version = 1,
                                       .sensor_status = 0};
        len = pogo_pack_sensor_info_ack(&ack, payload, sizeof(payload));
        peer_send(p, POGO_MSG_SENSOR_INFO_ACK, f->seq, payload, (uint16_t)len);
        break;
    }
    case POGO_MSG_SET_COLLECTOR: {
        pogo_collector_endpoint_t ep;
        if (pogo_unpack_set_collector(f->payload, f->len, &ep)) {
            pthread_mutex_lock(&p->mutex);
            p->collector = ep;
            p->has_collector = true;
            pthread_mutex_unlock(&p->mutex);
        }
        peer_send_ack(p, f->seq, f->type);
        break;
    }
    case POGO_MSG_SET_DATASET: {
        peer_send_ack(p, f->seq, f->type);
        p->join_result_seq++;
        if (p->join_result_seq == 0) {
            p->join_result_seq = 1;
        }
        pogo_join_result_t result = {.success = 1, .role = (uint8_t)OT_DEVICE_ROLE_CHILD};
        len = pogo_pack_join_result(&result, payload, sizeof(payload));
        peer_send(p, POGO_MSG_JOIN_RESULT, p->join_result_seq, payload, (uint16_t)len);
        break;
    }
    case POGO_MSG_JOIN_RESULT_ACK:
        pthread_mutex_lock(&p->mutex);
        p->provisioned++;
        pthread_mutex_unlock(&p->mutex);
        break;
    case POGO_MSG_ASSIGN_UUID:
    case POGO_MSG_POLL_CONFIG:
        peer_send_ack(p, f->seq, f->type);
        break;
    default:
        break;
    }
}

static void *peer_thread(void *arg)
{
    fake_peer_t *p = (fake_peer_t *)arg;
    pogo_frame_parser_t parser;
    pogo_frame_parser_init(&parser);
    while (1) {
        uint8_t b;
        if (!host_uart_test_take_tx_byte(p->uart, &b, 5)) {
            continue;
        }
        pogo_frame_t frame;
        if (!pogo_frame_parser_feed(&parser, b, &frame)) {
            continue;
        }
        if (!p->alive) {
            continue; /* a silent board: the bytes arrive, nothing ever comes back */
        }
        if (frame.type == POGO_MSG_PEER_HELLO_REQ) {
            uint8_t payload[3];
            size_t len;
            if (p->legacy_hello) {
                payload[0] = POGO_FRAME_VERSION;
                payload[1] = p->peer_type;
                len = 2;
            } else {
                pogo_peer_hello_t hello = {.protocol_version = POGO_FRAME_VERSION, .peer_type = p->peer_type,
                                            .capabilities = POGO_HELLO_SUPPORTED_CAPABILITIES};
                len = pogo_pack_peer_hello(&hello, payload, sizeof(payload));
            }
            peer_send(p, POGO_MSG_PEER_HELLO_RESP, frame.seq, payload, (uint16_t)len);
        } else if (frame.type == POGO_MSG_PING) {
            peer_send(p, POGO_MSG_PONG, frame.seq, NULL, 0);
        } else if (frame.type == POGO_MSG_PONG || frame.type == POGO_MSG_PEER_HELLO_RESP) {
            /* nothing to do */
        } else if (p->auto_child) {
            auto_child_handle(p, &frame);
        } else {
            pthread_mutex_lock(&p->mutex);
            if (p->inbox_count < PEER_INBOX_CAP) {
                p->inbox[p->inbox_count++] = frame;
            }
            pthread_mutex_unlock(&p->mutex);
        }
    }
    return NULL;
}

static void peer_start(fake_peer_t *p)
{
    pthread_mutex_init(&p->mutex, NULL);
    pthread_create(&p->thread, NULL, peer_thread, p);
}

static int child_identify_reqs(void)
{
    pthread_mutex_lock(&s_child.mutex);
    int v = s_child.identify_reqs;
    pthread_mutex_unlock(&s_child.mutex);
    return v;
}

static int child_provisioned(void)
{
    pthread_mutex_lock(&s_child.mutex);
    int v = s_child.provisioned;
    pthread_mutex_unlock(&s_child.mutex);
    return v;
}

/* --- the power base's scripted side of UPSTREAM provisioning --- */

static uint8_t s_pb_seq;
static uint32_t s_pb_collector_generation;
static pogo_collector_endpoint_t s_pb_collector; /* what the power base last handed out */

static void pb_clear_inbox(void)
{
    pthread_mutex_lock(&s_pb.mutex);
    s_pb.inbox_count = 0;
    pthread_mutex_unlock(&s_pb.mutex);
}

static bool pb_take_frame(uint8_t type, bool match_seq, uint8_t seq, pogo_frame_t *out, uint32_t timeout_ms)
{
    uint32_t start = now_ms();
    while (now_ms() - start <= timeout_ms) {
        bool found = false;
        pthread_mutex_lock(&s_pb.mutex);
        for (int i = 0; i < s_pb.inbox_count; i++) {
            if (s_pb.inbox[i].type == type && (!match_seq || s_pb.inbox[i].seq == seq)) {
                *out = s_pb.inbox[i];
                memmove(&s_pb.inbox[i], &s_pb.inbox[i + 1], sizeof(pogo_frame_t) * (size_t)(s_pb.inbox_count - i - 1));
                s_pb.inbox_count--;
                found = true;
                break;
            }
        }
        pthread_mutex_unlock(&s_pb.mutex);
        if (found) {
            return true;
        }
        sleep_ms(2);
    }
    return false;
}

static bool pb_request(uint8_t type, const uint8_t *payload, size_t len, uint8_t expect, pogo_frame_t *out,
                        uint32_t timeout_ms)
{
    uint8_t seq = ++s_pb_seq;
    peer_send(&s_pb, type, seq, payload, (uint16_t)len);
    return pb_take_frame(expect, true, seq, out, timeout_ms);
}

/* IDENTIFY .. SET_DATASET .. JOIN_RESULT_ACK, in the order parent_service.c
 * (the real power base's own provisioning code) sends them. Returns false
 * with a diagnostic at the first step the module under test did not answer
 * as a healthy child_service does. */
static bool pb_provision_module(void)
{
    pogo_frame_t f;
    uint8_t payload[POGO_COLLECTOR_ENDPOINT_WIRE_LEN];
    size_t len;
    pb_clear_inbox();

#define PB_STEP(ok, what)                                                                                            \
    do {                                                                                                             \
        if (!(ok)) {                                                                                                 \
            printf("info: power base provisioning stopped at %s\n", what);                                           \
            return false;                                                                                            \
        }                                                                                                            \
    } while (0)

    PB_STEP(pb_request(POGO_MSG_IDENTIFY_REQ, NULL, 0, POGO_MSG_IDENTIFY_RESP, &f, 500), "IDENTIFY");
    PB_STEP(pb_request(POGO_MSG_REQUEST_UUID, NULL, 0, POGO_MSG_PROPOSE_UUID, &f, 500), "REQUEST_UUID");
    pogo_propose_uuid_t proposed;
    PB_STEP(pogo_unpack_propose_uuid(f.payload, f.len, &proposed), "PROPOSE_UUID decode");

    pogo_assign_uuid_t assign;
    memcpy(assign.uuid, proposed.uuid, POGO_UUID_LEN);
    len = pogo_pack_assign_uuid(&assign, payload, sizeof(payload));
    PB_STEP(pb_request(POGO_MSG_ASSIGN_UUID, payload, len, POGO_MSG_ACK, &f, 500), "ASSIGN_UUID");

    pogo_assign_slot_t slot = {.slot_id = 0, .topology_depth = 1};
    len = pogo_pack_assign_slot(&slot, payload, sizeof(payload));
    PB_STEP(pb_request(POGO_MSG_ASSIGN_SLOT, payload, len, POGO_MSG_ACK, &f, 500), "ASSIGN_SLOT");

    pogo_assign_parent_info_t pinfo = {.parent_is_power_base = 1};
    memcpy(pinfo.parent_uuid, PB_UUID, POGO_UUID_LEN);
    len = pogo_pack_assign_parent_info(&pinfo, payload, sizeof(payload));
    PB_STEP(pb_request(POGO_MSG_ASSIGN_PARENT_INFO, payload, len, POGO_MSG_ACK, &f, 500), "ASSIGN_PARENT_INFO");

    /* The real sensor_service takes one real (mock, ~120ms) measurement here. */
    PB_STEP(pb_request(POGO_MSG_SENSOR_HELLO, NULL, 0, POGO_MSG_SENSOR_INFO_ACK, &f, 2000), "SENSOR_HELLO");

    pogo_poll_config_t poll = {.polling_period_ms = 100};
    len = pogo_pack_poll_config(&poll, payload, sizeof(payload));
    PB_STEP(pb_request(POGO_MSG_POLL_CONFIG, payload, len, POGO_MSG_ACK, &f, 500), "POLL_CONFIG");

    memset(&s_pb_collector, 0, sizeof(s_pb_collector));
    memcpy(s_pb_collector.power_base_uuid, PB_UUID, POGO_UUID_LEN);
    s_pb_collector.addr[0] = 0xFD; /* ULA: not unspecified/multicast/link-local */
    s_pb_collector.addr[15] = 0x01;
    s_pb_collector.port = 5683;
    s_pb_collector.generation = ++s_pb_collector_generation;
    len = pogo_pack_set_collector(&s_pb_collector, payload, sizeof(payload));
    PB_STEP(pb_request(POGO_MSG_SET_COLLECTOR, payload, len, POGO_MSG_ACK, &f, 500), "SET_COLLECTOR");

    static const uint8_t dataset[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    PB_STEP(pb_request(POGO_MSG_SET_DATASET, dataset, sizeof(dataset), POGO_MSG_ACK, &f, 500), "SET_DATASET");

    PB_STEP(pb_take_frame(POGO_MSG_JOIN_RESULT, false, 0, &f, 2000), "JOIN_RESULT");
    pogo_join_result_t result;
    PB_STEP(pogo_unpack_join_result(f.payload, f.len, &result) && result.success == 1, "JOIN_RESULT success");
    peer_send(&s_pb, POGO_MSG_JOIN_RESULT_ACK, f.seq, NULL, 0);
#undef PB_STEP
    return true;
}

/* ====================================================================
 * Observation helpers
 * ==================================================================== */

static bool collector_matches_power_base(void)
{
    collector_endpoint_snapshot_t snap;
    return collector_endpoint_get(&snap) && snap.valid &&
           memcmp(snap.power_base_uuid, PB_UUID, POGO_UUID_LEN) == 0 && snap.port == s_pb_collector.port &&
           snap.generation == s_pb_collector.generation;
}

static bool pred_operational(void)
{
    return role_manager_is_upstream_ready() && topology_gate_is_open();
}

static bool pred_data_plane_closed(void)
{
    return !topology_gate_is_open() && !role_manager_is_upstream_ready();
}

/* True once at least `min_new` more "/s" samples reach the power base. */
static bool samples_keep_arriving(int min_new, uint32_t timeout_ms)
{
    int start_count = sample_count();
    uint32_t start = now_ms();
    while (now_ms() - start <= timeout_ms) {
        if (sample_count() - start_count >= min_new) {
            return true;
        }
        sleep_ms(10);
    }
    return false;
}

/* True if NO "/s" sample reaches the power base for the whole window. */
static bool samples_stay_stopped(uint32_t window_ms)
{
    int start_count = sample_count();
    sleep_ms(window_ms);
    return sample_count() == start_count;
}

/* The module under test still owns everything it needs to keep measuring and
 * sending: Thread never stopped, joined + registered, collector intact, and
 * its own samples are actually reaching the power base. */
#define CHECK_OWN_PATH_ALIVE(label, expected_stops)                                                                  \
    do {                                                                                                             \
        CHECK(tm_stop_calls() == (expected_stops), label ": this module's Thread was NOT stopped");                 \
        CHECK(role_manager_is_upstream_ready(), label ": upstream_ready is kept");                                   \
        CHECK(topology_gate_is_open(), label ": the topology gate stays open");                                      \
        CHECK(collector_matches_power_base(), label ": the collector endpoint is kept");                            \
        CHECK(samples_keep_arriving(2, 2000), label ": its own samples keep reaching the power base");               \
    } while (0)

static uint8_t s_module_uuid[POGO_UUID_LEN];

/* ====================================================================
 * Boot: the same order module/main/app_main.c uses.
 * ==================================================================== */

static void boot_module(void)
{
    host_gpio_test_reset();
    host_uart_test_reset();
    pthread_mutex_lock(&s_pb_mutex);
    topology_registry_init(&s_registry);
    pthread_mutex_unlock(&s_pb_mutex);

    module_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = MODULE_CONFIG_SCHEMA_VERSION;
    cfg.driver_id = SENSOR_DRIVER_ID_MOCK;
    cfg.sensor_type_bitmap = 0x0001;
    cfg.measurement_timeout_ms = 500;

    CHECK(sensor_service_init(&cfg) == ESP_OK, "sensor_service_init");
    CHECK(topology_gate_init() == ESP_OK, "topology_gate_init");
    CHECK(collector_endpoint_init() == ESP_OK, "collector_endpoint_init");
    CHECK(sensor_transport_init(true) == ESP_OK, "sensor_transport_init (AUTO: gated on topology registration)");
    sensor_service_set_sample_ready_cb(sensor_transport_send_sample);

    CHECK(role_manager_init(MODULE_DEV_MODE_AUTO, module_wiring_on_role_change, NULL) == ESP_OK,
          "role_manager_init (AUTO) with the real role-change reaction");

    pogo_port_config_t up_cfg = {
        .enabled = true, .uart_num = UP_UART, .tx_gpio = 10, .rx_gpio = 11,
        .detect_in_gpio = UP_DETECT_IN, .detect_out_gpio = UP_DETECT_OUT, .base_id_gpio = -1,
        .detect_out_active_low = true, .debounce_ms = 10, .baud_rate = 115200,
    };
    pogo_port_config_t down_cfg = {
        .enabled = true, .uart_num = DOWN_UART, .tx_gpio = 22, .rx_gpio = 23,
        .detect_in_gpio = DOWN_DETECT_IN, .detect_out_gpio = DOWN_DETECT_OUT, .base_id_gpio = -1,
        .detect_out_active_low = true, .debounce_ms = 10, .baud_rate = 115200,
    };
    CHECK(pogo_port_init(POGO_PORT_UPSTREAM, &up_cfg, recording_dispatcher, NULL) == ESP_OK, "UPSTREAM pogo_port_init");
    CHECK(pogo_port_init(POGO_PORT_DOWNSTREAM, &down_cfg, recording_dispatcher, NULL) == ESP_OK,
          "DOWNSTREAM pogo_port_init");

    CHECK(child_service_init(&cfg) == ESP_OK, "child_service_init");
    CHECK(parent_service_init(module_wiring_parent_context()) == ESP_OK,
          "parent_service_init with the real module provisioning gate");
    CHECK(topology_reporter_init(&cfg) == ESP_OK, "topology_reporter_init");

    peer_start(&s_pb);
    peer_start(&s_child);

    CHECK(pogo_port_enable(POGO_PORT_UPSTREAM, POGO_PEER_TYPE_MODULE) == ESP_OK, "UPSTREAM pogo_port_enable");
    CHECK(pogo_port_enable(POGO_PORT_DOWNSTREAM, POGO_PEER_TYPE_MODULE) == ESP_OK, "DOWNSTREAM pogo_port_enable");

    sensor_service_get_module_uuid(s_module_uuid);
}

/* ====================================================================
 * Phases
 * ==================================================================== */

/* F: nothing docked UPSTREAM -- DOWNSTREAM events alone never produce a
 * joined / registered / sending module. */
static void phase_f_downstream_only_before_upstream(void)
{
    printf("\n-- F: DOWNSTREAM docks, becomes ready, then fails, with NO UPSTREAM at all --\n");
    int mark = evt_mark();
    s_child.alive = true;
    host_gpio_test_edge(DOWN_DETECT_IN, 0);
    CHECK(wait_for_events(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_PEER_READY, POGO_PORT_ERR_NONE, 1, 1000),
          "DOWNSTREAM reaches PEER_READY");
    sleep_ms(700); /* more than one topology_reporter poll tick */
    CHECK(role_manager_get_role() == MODULE_ROLE_UNASSIGNED, "role stays UNASSIGNED");
    CHECK(!role_manager_is_upstream_ready() && !topology_gate_is_open(), "not joined, gate closed");
    CHECK(tm_join_calls() == 0, "no Thread join was started");
    CHECK(child_identify_reqs() == 0, "no DOWNSTREAM provisioning was started");
    CHECK(sample_count() == 0 && report_count() == 0, "nothing was sent to the power base");

    s_child.alive = false;
    CHECK(wait_for_events(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_ERROR, POGO_PORT_ERR_LIVENESS_LOST, 1, 2000),
          "the DOWNSTREAM peer goes silent -> LIVENESS_LOST");
    sleep_ms(300);
    CHECK(role_manager_get_role() == MODULE_ROLE_UNASSIGNED, "role is still UNASSIGNED after the DOWNSTREAM fault");
    CHECK(tm_join_calls() == 0 && !role_manager_is_upstream_ready() && !topology_gate_is_open(),
          "still not joined, gate closed");
    CHECK(sample_count() == 0 && report_count() == 0, "still nothing sent");

    host_gpio_test_edge(DOWN_DETECT_IN, 1);
    CHECK(wait_for_events(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_DETACHED, POGO_PORT_ERR_NONE, 1, 1000),
          "DOWNSTREAM undocked again");
}

static void phase_1_dock_and_join_as_leaf(void)
{
    printf("\n-- bring-up: dock into the power base, get provisioned, join as LEAF --\n");
    int mark = evt_mark();
    s_pb.alive = true;
    host_gpio_test_edge(UP_DETECT_IN, 0);
    CHECK(wait_for_events(mark, POGO_PORT_UPSTREAM, POGO_PORT_EVENT_PEER_READY, POGO_PORT_ERR_NONE, 1, 1000),
          "UPSTREAM reaches PEER_READY");
    CHECK(pb_provision_module(), "the power base provisions this module (real child_service)");
    CHECK(wait_until(pred_operational, 3000), "joined, registered (ATTACHED accepted), topology gate open");
    CHECK(role_manager_get_role() == MODULE_ROLE_LEAF, "role is LEAF");
    CHECK(tm_join_calls() == 1 && tm_join_leaf_calls() == 1, "exactly one Thread join, as LEAF");
    topology_registry_entry_t e;
    CHECK(registry_get(s_module_uuid, &e) && e.online && e.role == TOPOLOGY_ROLE_LEAF && !e.downstream_connected,
          "power base registry: this module online, LEAF, nothing connected below");
    CHECK_OWN_PATH_ALIVE("baseline", 0);
}

/* A + H: joined LEAF, a module docks below and never answers HELLO. */
static void phase_a_leaf_downstream_dock_timeout(void)
{
    printf("\n-- A: joined LEAF; a module docks DOWNSTREAM and never answers (repeated DOCK_TIMEOUT) --\n");
    int mark = evt_mark();
    int reports_before = report_count();
    int samples_before = sample_count();
    s_child.alive = false;
    host_gpio_test_edge(DOWN_DETECT_IN, 0);
    CHECK(wait_for_events(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_ATTACHED, POGO_PORT_ERR_NONE, 1, 1000),
          "DOWNSTREAM ATTACHED");
    CHECK(wait_for_events(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_ERROR, POGO_PORT_ERR_DOCK_TIMEOUT, 1, 1500),
          "first handshake cycle -> DOCK_TIMEOUT");
    CHECK(tm_stop_calls() == 0, "right after the DOCK_TIMEOUT: Thread is NOT stopped");
    CHECK(role_manager_get_role() == MODULE_ROLE_LEAF, "right after the DOCK_TIMEOUT: role is still LEAF");
    CHECK(role_manager_is_upstream_ready(), "right after the DOCK_TIMEOUT: upstream_ready is kept");

    CHECK(wait_for_events(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_ERROR, POGO_PORT_ERR_DOCK_TIMEOUT, 3, 4000),
          "the port keeps retrying: three DOCK_TIMEOUT cycles so far");
    sleep_ms(700); /* let topology_reporter run at least one more poll tick */

    CHECK(role_manager_get_role() == MODULE_ROLE_LEAF, "after repeated cycles: role is still LEAF");
    CHECK_OWN_PATH_ALIVE("after repeated DOWNSTREAM DOCK_TIMEOUT", 0);
    CHECK(sample_count() - samples_before >= 3, "samples were delivered all through the DOWNSTREAM fault");
    CHECK(tm_join_calls() == 1, "no re-join / link-mode re-apply happened");
    CHECK(child_identify_reqs() == 0, "DOWNSTREAM provisioning never started for the failed link");
    CHECK(find_report(reports_before, true, NULL) < 0, "no report ever claimed downstream_connected");
    topology_registry_entry_t e;
    CHECK(registry_get(s_module_uuid, &e) && e.online && !e.downstream_connected,
          "power base registry: this module online, downstream NOT connected");
}

/* C (from LEAF): the docked module finally answers. */
static void phase_c_recovery_from_leaf(void)
{
    printf("\n-- C1: the DOWNSTREAM module finally answers HELLO -> PEER_READY -> provisioning --\n");
    int mark = evt_mark();
    int reports_before = report_count();
    s_child.alive = true;
    CHECK(wait_for_events(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_PEER_READY, POGO_PORT_ERR_NONE, 1, 2000),
          "a later retry cycle reaches PEER_READY with no new DETECT edge");
    CHECK(role_manager_get_role() == MODULE_ROLE_INTERMEDIATE, "role becomes INTERMEDIATE");
    bool provisioned = false;
    for (int i = 0; i < 600 && !provisioned; i++) {
        provisioned = (child_provisioned() == 1);
        if (!provisioned) {
            sleep_ms(5);
        }
    }
    CHECK(provisioned, "parent_service provisions the DOWNSTREAM module to completion (JOIN_RESULT_ACK)");
    pthread_mutex_lock(&s_child.mutex);
    /* Field by field: the struct has padding, which a whole-struct memcmp
     * would compare too. */
    bool collector_forwarded =
        s_child.has_collector &&
        memcmp(s_child.collector.power_base_uuid, s_pb_collector.power_base_uuid, POGO_UUID_LEN) == 0 &&
        memcmp(s_child.collector.addr, s_pb_collector.addr, POGO_COLLECTOR_ADDR_LEN) == 0 &&
        s_child.collector.port == s_pb_collector.port && s_child.collector.generation == s_pb_collector.generation;
    bool parent_is_this_module = memcmp(s_child.parent_uuid, s_module_uuid, POGO_UUID_LEN) == 0;
    uint8_t depth = s_child.assigned_depth;
    pthread_mutex_unlock(&s_child.mutex);
    CHECK(collector_forwarded, "the child received exactly the collector this module was given (never re-provisioned)");
    CHECK(parent_is_this_module && depth == 2, "the child was told this module is its parent, at depth 2");

    topology_report_t rep;
    bool reported = false;
    for (int i = 0; i < 400 && !reported; i++) {
        reported = find_report(reports_before, true, &rep) >= 0;
        if (!reported) {
            sleep_ms(5);
        }
    }
    CHECK(reported && rep.role == TOPOLOGY_ROLE_INTERMEDIATE, "an INTERMEDIATE / downstream_connected report reaches the power base");
    /* child_service's own task notices the role change on its next loop pass. */
    bool reapplied = false;
    for (int i = 0; i < 200 && !reapplied; i++) {
        reapplied = (tm_join_intermediate_calls() == 1);
        if (!reapplied) {
            sleep_ms(5);
        }
    }
    CHECK(reapplied && tm_join_calls() == 2,
          "one link-mode re-apply for the normal LEAF -> INTERMEDIATE change (the existing path)");
    CHECK_OWN_PATH_ALIVE("after DOWNSTREAM recovery", 0);
    CHECK(child_reports_to_power_base(s_module_uuid, TOPOLOGY_REPORT_ATTACHED) == TOPOLOGY_REGISTRY_OK,
          "power base registry accepts the child under this module");
}

/* B + H: established child stops answering PING. */
static void phase_b_intermediate_downstream_liveness_lost(void)
{
    printf("\n-- B: joined INTERMEDIATE; the established child goes silent (LIVENESS_LOST, then retries) --\n");
    int mark = evt_mark();
    int reports_before = report_count();
    int samples_before = sample_count();
    int identify_before = child_identify_reqs();
    int joins_before = tm_join_calls();
    s_child.alive = false;
    CHECK(wait_for_events(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_ERROR, POGO_PORT_ERR_LIVENESS_LOST, 1, 2000),
          "DOWNSTREAM LIVENESS_LOST");
    CHECK(tm_stop_calls() == 0, "right after LIVENESS_LOST: Thread is NOT stopped");
    CHECK(role_manager_get_role() == MODULE_ROLE_INTERMEDIATE, "right after LIVENESS_LOST: role is still INTERMEDIATE");

    topology_report_t rep;
    int down_report = -1;
    for (int i = 0; i < 400 && down_report < 0; i++) {
        down_report = find_report(reports_before, false, &rep);
        if (down_report < 0) {
            sleep_ms(5);
        }
    }
    CHECK(down_report >= 0, "a report with downstream_connected=false reaches the power base");
    CHECK(down_report >= 0 && rep.role == TOPOLOGY_ROLE_INTERMEDIATE && rep.upstream_connected,
          "that report still describes this module itself as INTERMEDIATE and upstream-connected");

    /* Hold the fault across several retry cycles (HANDSHAKE / ERROR /
     * RECOVER_COOLDOWN, DETECT_IN Low the whole time) and several
     * topology_reporter poll ticks. */
    sleep_ms(1800);
    CHECK(count_events_since(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_ERROR, POGO_PORT_ERR_DOCK_TIMEOUT) >= 2,
          "the port went through at least two more failed retry cycles");
    CHECK(count_events_since(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_DETACHED, POGO_PORT_ERR_NONE) == 0,
          "DETECT_IN stayed attached the whole time");
    CHECK(down_report >= 0 && find_report(down_report + 1, true, NULL) < 0,
          "the failed link is never reported connected again from DETECT_IN / the stale peer_type");
    CHECK(report_count() - reports_before <= 2, "no report flapping while the fault persists");
    CHECK(role_manager_get_role() == MODULE_ROLE_INTERMEDIATE, "role held at INTERMEDIATE through every retry cycle");
    CHECK(tm_join_calls() == joins_before,
          "no LEAF (sleepy end device) link mode applied, no re-join, while a module is still docked");
    CHECK(child_identify_reqs() == identify_before, "no DOWNSTREAM provisioning attempt during the fault");
    CHECK(sample_count() - samples_before >= 3, "this module's own samples were delivered all through the fault");
    CHECK_OWN_PATH_ALIVE("during the DOWNSTREAM liveness fault", 0);

    topology_registry_entry_t e;
    CHECK(registry_get(s_module_uuid, &e) && e.online && !e.downstream_connected,
          "power base registry: this module online, downstream NOT connected");
    CHECK(!registry_get(CHILD_UUID, &e), "power base registry: the child subtree was removed");
    CHECK(child_reports_to_power_base(s_module_uuid, TOPOLOGY_REPORT_HEARTBEAT) == TOPOLOGY_REGISTRY_ERR_PARENT_OFFLINE,
          "power base registry: a late heartbeat from the child cannot resurrect it");
}

/* C (from INTERMEDIATE): the child comes back. */
static void phase_c_recovery_from_intermediate(void)
{
    printf("\n-- C2: the child comes back -> PEER_READY -> provisioning resumes, no rejoin of our own --\n");
    int mark = evt_mark();
    int reports_before = report_count();
    int identify_before = child_identify_reqs();
    int provisioned_before = child_provisioned();
    int joins_before = tm_join_calls();
    s_child.alive = true;
    CHECK(wait_for_events(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_PEER_READY, POGO_PORT_ERR_NONE, 1, 2000),
          "DOWNSTREAM reaches PEER_READY again");
    bool provisioned = false;
    for (int i = 0; i < 600 && !provisioned; i++) {
        provisioned = (child_provisioned() == provisioned_before + 1);
        if (!provisioned) {
            sleep_ms(5);
        }
    }
    CHECK(provisioned, "parent_service re-provisions the child to completion");
    CHECK(child_identify_reqs() == identify_before + 1, "exactly one new provisioning session was started");
    CHECK(tm_join_calls() == joins_before, "this module did not re-join or re-apply its own link mode");
    CHECK(role_manager_get_role() == MODULE_ROLE_INTERMEDIATE, "role is INTERMEDIATE");
    bool reported = false;
    for (int i = 0; i < 400 && !reported; i++) {
        reported = find_report(reports_before, true, NULL) >= 0;
        if (!reported) {
            sleep_ms(5);
        }
    }
    CHECK(reported, "downstream_connected=true is reported again, now that the link really is ready");
    CHECK_OWN_PATH_ALIVE("after the child's recovery", 0);
    CHECK(child_reports_to_power_base(s_module_uuid, TOPOLOGY_REPORT_ATTACHED) == TOPOLOGY_REGISTRY_OK,
          "power base registry accepts the child again");
}

/* D (ERROR): UPSTREAM fails while DOWNSTREAM is already faulted. */
static void phase_d_upstream_error_during_downstream_fault(void)
{
    printf("\n-- D1: DOWNSTREAM faulted, then the power base goes silent (UPSTREAM LIVENESS_LOST) --\n");
    int mark = evt_mark();
    s_child.alive = false;
    CHECK(wait_for_events(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_ERROR, POGO_PORT_ERR_LIVENESS_LOST, 1, 2000),
          "DOWNSTREAM LIVENESS_LOST");
    CHECK(tm_stop_calls() == 0 && role_manager_is_upstream_ready(), "the DOWNSTREAM fault alone changes nothing");

    s_pb.alive = false;
    CHECK(wait_for_events(mark, POGO_PORT_UPSTREAM, POGO_PORT_EVENT_ERROR, POGO_PORT_ERR_LIVENESS_LOST, 1, 2000),
          "UPSTREAM LIVENESS_LOST");
    CHECK(role_manager_get_role() == MODULE_ROLE_ERROR, "role -> ROLE_ERROR");
    CHECK(tm_stop_calls() == 1, "Thread is stopped, exactly once");
    CHECK(!role_manager_is_upstream_ready(), "upstream_ready is cleared");
    CHECK(wait_until(pred_data_plane_closed, 5000), "the topology gate closes");
    bool collector_cleared = false;
    for (int i = 0; i < 200 && !collector_cleared; i++) {
        collector_cleared = !collector_endpoint_is_valid();
        if (!collector_cleared) {
            sleep_ms(5);
        }
    }
    CHECK(collector_cleared, "the collector endpoint is cleared (real child_service)");
    CHECK(samples_stay_stopped(700), "no sample reaches the power base any more");
    CHECK(tm_stop_calls() == 1, "the UPSTREAM retry cycles do not stop Thread again");
}

/* E: DOWNSTREAM recovers while UPSTREAM is still in error. */
static void phase_e_downstream_recovers_while_upstream_failed(void)
{
    printf("\n-- E: UPSTREAM still failed; the DOWNSTREAM module comes back (PEER_READY) --\n");
    int mark = evt_mark();
    int joins_before = tm_join_calls();
    int identify_before = child_identify_reqs();
    int reports_before = report_count();
    s_child.alive = true;
    CHECK(wait_for_events(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_PEER_READY, POGO_PORT_ERR_NONE, 1, 2000),
          "DOWNSTREAM reaches PEER_READY");
    sleep_ms(900);
    CHECK(role_manager_get_role() == MODULE_ROLE_ERROR, "role stays ROLE_ERROR");
    CHECK(!role_manager_is_upstream_ready() && !topology_gate_is_open(), "not joined, gate stays closed");
    CHECK(tm_join_calls() == joins_before, "no Thread join was started by the DOWNSTREAM recovery");
    CHECK(child_identify_reqs() == identify_before, "no DOWNSTREAM provisioning was started");
    CHECK(report_count() == reports_before, "nothing new registered with the power base");
    CHECK(samples_stay_stopped(500), "still no sample reaches the power base");
    CHECK(tm_stop_calls() == 1, "no further Thread stop either");
}

/* D (DETACHED): UPSTREAM recovers through a fresh handshake + provisioning,
 * then is physically undocked while DOWNSTREAM is faulted. */
static void phase_d_upstream_detach_during_downstream_fault(void)
{
    printf("\n-- recovery: the power base answers again -> fresh handshake + provisioning --\n");
    int mark = evt_mark();
    int provisioned_before = child_provisioned();
    s_pb.alive = true;
    CHECK(wait_for_events(mark, POGO_PORT_UPSTREAM, POGO_PORT_EVENT_PEER_READY, POGO_PORT_ERR_NONE, 1, 2000),
          "UPSTREAM reaches PEER_READY again");
    CHECK(pb_provision_module(), "the power base provisions this module again");
    CHECK(wait_until(pred_operational, 3000), "joined, registered, topology gate open again");
    CHECK(role_manager_get_role() == MODULE_ROLE_INTERMEDIATE, "role is INTERMEDIATE (the child is ready below)");
    bool provisioned = false;
    for (int i = 0; i < 800 && !provisioned; i++) {
        provisioned = (child_provisioned() == provisioned_before + 1);
        if (!provisioned) {
            sleep_ms(5);
        }
    }
    CHECK(provisioned, "the waiting DOWNSTREAM child is provisioned once this module is joined again");
    CHECK_OWN_PATH_ALIVE("after UPSTREAM recovery", 1);

    printf("\n-- D2: DOWNSTREAM faulted, then UPSTREAM physically undocked (DETACHED) --\n");
    mark = evt_mark();
    s_child.alive = false;
    CHECK(wait_for_events(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_ERROR, POGO_PORT_ERR_LIVENESS_LOST, 1, 2000),
          "DOWNSTREAM LIVENESS_LOST");
    CHECK(tm_stop_calls() == 1 && role_manager_get_role() == MODULE_ROLE_INTERMEDIATE,
          "the DOWNSTREAM fault alone changes nothing");
    host_gpio_test_edge(UP_DETECT_IN, 1);
    CHECK(wait_for_events(mark, POGO_PORT_UPSTREAM, POGO_PORT_EVENT_DETACHED, POGO_PORT_ERR_NONE, 1, 1000),
          "UPSTREAM DETACHED");
    CHECK(role_manager_get_role() == MODULE_ROLE_UNASSIGNED, "role -> UNASSIGNED");
    CHECK(tm_stop_calls() == 2, "Thread is stopped");
    CHECK(!role_manager_is_upstream_ready(), "upstream_ready is cleared");
    CHECK(wait_until(pred_data_plane_closed, 5000), "the topology gate closes");
    bool collector_cleared = false;
    for (int i = 0; i < 200 && !collector_cleared; i++) {
        collector_cleared = !collector_endpoint_is_valid();
        if (!collector_cleared) {
            sleep_ms(5);
        }
    }
    CHECK(collector_cleared, "the collector endpoint is cleared");
    CHECK(samples_stay_stopped(500), "no sample reaches the power base any more");
}

/* G: a permanent DOWNSTREAM error on a joined module keeps the old policy. */
static void phase_g_permanent_downstream_error(void)
{
    printf("\n-- G: joined again; the DOWNSTREAM module answers with a legacy HELLO (CAPABILITY_MISMATCH) --\n");
    int mark = evt_mark();
    host_gpio_test_edge(UP_DETECT_IN, 0);
    CHECK(wait_for_events(mark, POGO_PORT_UPSTREAM, POGO_PORT_EVENT_PEER_READY, POGO_PORT_ERR_NONE, 1, 1500),
          "UPSTREAM docked again, PEER_READY");
    CHECK(pb_provision_module(), "the power base provisions this module again");
    CHECK(wait_until(pred_operational, 3000), "joined, registered, topology gate open again");
    CHECK(role_manager_get_role() == MODULE_ROLE_LEAF,
          "role is LEAF: the DOWNSTREAM module is docked but has not completed a handshake");
    CHECK_OWN_PATH_ALIVE("joined with a still-faulted DOWNSTREAM", 2);

    mark = evt_mark();
    s_child.legacy_hello = true;
    s_child.alive = true;
    CHECK(wait_for_events(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_ERROR, POGO_PORT_ERR_CAPABILITY_MISMATCH, 1, 2000),
          "DOWNSTREAM CAPABILITY_MISMATCH");
    CHECK(role_manager_get_role() == MODULE_ROLE_ERROR, "role -> ROLE_ERROR (permanent errors are not isolated)");
    CHECK(tm_stop_calls() == 3, "Thread is stopped");
    CHECK(!role_manager_is_upstream_ready(), "upstream_ready is cleared");
    CHECK(wait_until(pred_data_plane_closed, 6000), "the topology gate closes");
    CHECK(samples_stay_stopped(500), "no sample reaches the power base any more");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("-- boot --\n");
    boot_module();

    phase_f_downstream_only_before_upstream();
    phase_1_dock_and_join_as_leaf();
    phase_a_leaf_downstream_dock_timeout();
    phase_c_recovery_from_leaf();
    phase_b_intermediate_downstream_liveness_lost();
    phase_c_recovery_from_intermediate();
    phase_d_upstream_error_during_downstream_fault();
    phase_e_downstream_recovers_while_upstream_failed();
    phase_d_upstream_detach_during_downstream_fault();
    phase_g_permanent_downstream_error();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
    } else {
        printf("%d CHECK(S) FAILED\n", g_failures);
    }
    fflush(stdout);
    /* The module's service tasks have no teardown; leave them running and
     * exit the process directly. */
    _exit(g_failures == 0 ? 0 : 1);
}
