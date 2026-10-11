/*
 * Host-compiled regression test for "a new UPSTREAM provisioning generation
 * must reach everything docked DOWNSTREAM, without a physical re-dock"
 * (docs/state-machine.md, "UPSTREAM 세대 변경과 DOWNSTREAM 재설정").
 *
 * Real-hardware finding this reproduces: Power Base -> SGP41 -> PMS5003. The
 * Base was removed and enrolled again. The SGP41 saw its UPSTREAM port
 * detach and come back, was provisioned from scratch (new collector, new
 * Dataset) and joined -- and never said a word to the PMS, which stayed
 * docked the whole time: its DOWNSTREAM session was still "JOINED" from
 * before. The PMS kept sending to a collector that no longer existed until
 * somebody pulled it off and put it back.
 *
 * PART 1 is "one sensor module in a box", exactly as
 * tools/test_module_downstream_fault_isolation.c builds it: the REAL,
 * UNMODIFIED production sources run on the pthread-backed FreeRTOS stand-in
 * and the in-memory GPIO/UART fakes --
 *
 *   components/pogo_port (both ports), pogo_proto, role_manager,
 *   module/main/module_wiring.c, child_service, parent_service,
 *   sensor_service (+ the mock sensor_provider), sensor_transport,
 *   topology_reporter, topology_gate, collector_endpoint.
 *
 * The test only plays what is not this module: the board docked UPSTREAM
 * (drives the provisioning sequence with the real pogo_proto packers), the
 * board docked DOWNSTREAM (answers parent_service's requests and records
 * what it was given, in which order), a thread_manager stand-in that keeps
 * the Dataset it was joined with (so what this module hands down is what it
 * received), and a CoAP stand-in that records where "/t" and "/s" were sent.
 * No role, gate, generation or session decision is made in this file.
 *
 * PART 2 is a real chain: three copies of this same program, one process per
 * board (the production components keep their state in file-scope statics,
 * so one process is one board), wired UART-to-UART and DETECT-to-DETECT over
 * socket pairs:
 *
 *   scripted Power Base -> M1 -> M2 -> M3
 *
 * M1's parent_service provisions M2's child_service, M2's provisions M3's --
 * real code on both ends of both module-to-module links. Only M1 ever sees a
 * physical UPSTREAM detach; M2 and M3 must still end up with the new
 * collector, Dataset, depth and parent.
 *
 * What this does NOT show: OpenThread itself (a join always succeeds at
 * once here), radio timing, NVS contents, or the real Power Base firmware
 * (its side of the wire is scripted from the same packers it uses).
 *
 * Phases 10-16 of PART 1 are about ONE question: which JOIN_RESULT is a
 * DOWNSTREAM session's own (parent_service.c, s_child_results). The board
 * below is scripted there to do what a module running child_service does not
 * do -- repeat an old result in a later session's join wait, deliver the
 * result of a dropped session late, lose the ACK of SET_DATASET, restart
 * with and without a port event -- and phase 10 first shows, on the REAL
 * child_service of this module, the two facts the parent's rule rests on.
 *
 * Build & run (from the repo root; about 80 s of wall-clock time):
 *   cc -Wall -Wextra -pthread -DHOST_ESP_MAC_LAST_BYTE_ENV='"HOST_ESP_MAC_LAST_BYTE"' \
 *      -I tools/host_test_stubs/pogo_fast_long_join -I tools/host_test_stubs -I module/main \
 *      -I components/pogo_port/include -I components/pogo_proto/include \
 *      -I components/role_manager/include -I components/module_config/include \
 *      -I components/child_service/include -I components/parent_service/include \
 *      -I components/sensor_service/include -I components/sensor_provider/include \
 *      -I components/sensor_channel/include -I components/sensor_root_aggregator/include \
 *      -I components/sensor_transport/include -I components/thread_manager/include \
 *      -I components/collector_endpoint/include -I components/node_calc/include \
 *      -I components/topology_gate/include -I components/topology_reporter/include \
 *      -I components/topology_proto/include -I components/coap_response_timing/include \
 *      -o /tmp/test_module_upstream_regeneration tools/test_module_upstream_regeneration.c \
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
 *      components/topology_proto/topology_proto.c components/sensor_transport/sensor_transport.c \
 *      components/coap_response_timing/coap_response_timing.c \
 *      tools/host_test_stubs/host_gpio_sim.c tools/host_test_stubs/host_uart_sim.c \
 *   && /tmp/test_module_upstream_regeneration
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
static const uint8_t OTHER_PARENT_UUID[POGO_UUID_LEN] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
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
 * Chain-node status channel (PART 2). -1 in PART 1.
 * ==================================================================== */

static int s_status_fd = -1;
static pthread_mutex_t s_status_mutex = PTHREAD_MUTEX_INITIALIZER;

static void status_line(const char *fmt, ...)
{
    if (s_status_fd < 0) {
        return;
    }
    char line[96];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0 || (size_t)n >= sizeof(line)) {
        return;
    }
    pthread_mutex_lock(&s_status_mutex);
    ssize_t w = write(s_status_fd, line, (size_t)n);
    (void)w;
    pthread_mutex_unlock(&s_status_mutex);
}

/* ====================================================================
 * thread_manager stand-in. A join succeeds at once and keeps the Dataset it
 * was given: that is what this module's own parent_service reads back and
 * hands to the board below.
 * ==================================================================== */

static pthread_mutex_t s_tm_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool s_tm_enabled;
static otDeviceRole s_tm_role = OT_DEVICE_ROLE_DISABLED;
static bool s_tm_role_change_pending;
static int s_tm_stop_calls;
static int s_tm_join_calls;
static uint8_t s_tm_dataset[OT_OPERATIONAL_DATASET_MAX_LENGTH];
static uint16_t s_tm_dataset_len;

bool thread_manager_validate_dataset_tlvs(const uint8_t *dataset_tlvs, uint16_t dataset_len)
{
    return dataset_tlvs != NULL && dataset_len > 0 && dataset_len <= OT_OPERATIONAL_DATASET_MAX_LENGTH;
}

esp_err_t thread_manager_join_with_dataset(thread_manager_role_t role, const uint8_t *dataset_tlvs,
                                            uint16_t dataset_len, uint32_t thread_data_poll_period_ms)
{
    (void)thread_data_poll_period_ms;
    pthread_mutex_lock(&s_tm_mutex);
    s_tm_join_calls++;
    memcpy(s_tm_dataset, dataset_tlvs, dataset_len);
    s_tm_dataset_len = dataset_len;
    s_tm_enabled = true;
    s_tm_role = OT_DEVICE_ROLE_CHILD;
    s_tm_role_change_pending = true;
    pthread_mutex_unlock(&s_tm_mutex);
    status_line("J %d %u %u\n", (int)role, (unsigned)dataset_len, (unsigned)dataset_tlvs[0]);
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
    esp_err_t err = ESP_FAIL;
    pthread_mutex_lock(&s_tm_mutex);
    if (s_tm_enabled && s_tm_dataset_len > 0) {
        memcpy(out_tlvs->mTlvs, s_tm_dataset, s_tm_dataset_len);
        out_tlvs->mLength = (uint8_t)s_tm_dataset_len;
        err = ESP_OK;
    }
    pthread_mutex_unlock(&s_tm_mutex);
    return err;
}

esp_err_t thread_manager_stop(void)
{
    pthread_mutex_lock(&s_tm_mutex);
    s_tm_stop_calls++;
    s_tm_enabled = false;
    s_tm_role = OT_DEVICE_ROLE_DISABLED;
    s_tm_role_change_pending = false;
    pthread_mutex_unlock(&s_tm_mutex);
    status_line("X\n");
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

/* What OpenThread does on its own while the membership stays the same: a
 * ROUTER promotion, a short radio detach and re-attach. */
static void tm_radio_role_event(otDeviceRole role)
{
    pthread_mutex_lock(&s_tm_mutex);
    s_tm_role = role;
    s_tm_role_change_pending = true;
    pthread_mutex_unlock(&s_tm_mutex);
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

static bool tm_enabled(void)
{
    pthread_mutex_lock(&s_tm_mutex);
    bool v = s_tm_enabled;
    pthread_mutex_unlock(&s_tm_mutex);
    return v;
}

/* ====================================================================
 * The Thread network + the collector's CoAP resources. A request gets
 * through only while this module's Thread interface is up; what is recorded
 * is where it was sent (the collector address this module used).
 * ==================================================================== */

#define REPORT_LOG_CAP 256

typedef struct {
    topology_report_t rep;
    uint8_t dest_last; /* last byte of the collector address it was sent to */
} sent_report_t;

static pthread_mutex_t s_net_mutex = PTHREAD_MUTEX_INITIALIZER;
static sent_report_t s_reports[REPORT_LOG_CAP];
static int s_report_count;
static int s_samples_to[256]; /* "/s" samples, by last byte of the collector address */

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
    (void)resource; /* the ROOT-side "/s" server: never called in this test */
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

otError otCoapSendRequestWithParameters(otInstance *instance, otMessage *message, const otMessageInfo *messageInfo,
                                         otCoapResponseHandler handler, void *context,
                                         const otCoapTxParameters *txParameters)
{
    (void)instance;
    (void)txParameters;
    if (!tm_enabled()) {
        return OT_ERROR_NO_ROUTE; /* the caller keeps and frees `message` */
    }

    uint8_t dest_last = messageInfo->mPeerAddr.mFields.m8[15];
    otCoapCode code = OT_COAP_CODE_BAD_REQUEST;
    if (strcmp(message->uri, "t") == 0) {
        topology_report_t rep;
        if (topology_report_unpack(message->payload, message->len, &rep)) {
            code = OT_COAP_CODE_CHANGED;
            pthread_mutex_lock(&s_net_mutex);
            if (s_report_count < REPORT_LOG_CAP) {
                s_reports[s_report_count].rep = rep;
                s_reports[s_report_count].dest_last = dest_last;
                s_report_count++;
            }
            pthread_mutex_unlock(&s_net_mutex);
            status_line("T %u %u %u %u %u\n", (unsigned)rep.report_type, (unsigned)rep.topology_depth,
                        (unsigned)rep.parent_uuid[POGO_UUID_LEN - 1], (unsigned)dest_last,
                        (unsigned)rep.downstream_connected);
        }
    } else if (strcmp(message->uri, "s") == 0) {
        code = OT_COAP_CODE_CHANGED;
        pthread_mutex_lock(&s_net_mutex);
        s_samples_to[dest_last]++;
        pthread_mutex_unlock(&s_net_mutex);
        status_line("S %u\n", (unsigned)dest_last);
    }

    otMessage response = {.type = OT_COAP_TYPE_ACKNOWLEDGMENT, .code = code};
    handler(context, &response, messageInfo, OT_ERROR_NONE);
    free(message);
    return OT_ERROR_NONE;
}

static int samples_to(uint8_t dest_last)
{
    pthread_mutex_lock(&s_net_mutex);
    int v = s_samples_to[dest_last];
    pthread_mutex_unlock(&s_net_mutex);
    return v;
}

static int report_count(void)
{
    pthread_mutex_lock(&s_net_mutex);
    int v = s_report_count;
    pthread_mutex_unlock(&s_net_mutex);
    return v;
}

/* First ATTACHED "/t" report at or after `from` that went to `dest_last`. */
static bool find_attached_report(int from, uint8_t dest_last, topology_report_t *out)
{
    bool found = false;
    pthread_mutex_lock(&s_net_mutex);
    for (int i = from; i < s_report_count && !found; i++) {
        if (s_reports[i].rep.report_type == TOPOLOGY_REPORT_ATTACHED && s_reports[i].dest_last == dest_last) {
            *out = s_reports[i].rep;
            found = true;
        }
    }
    pthread_mutex_unlock(&s_net_mutex);
    return found;
}

/* ====================================================================
 * A wire to a neighbouring board: the in-memory fake UART (PART 1), or a
 * socket carrying 'U' <len16> <bytes> and 'D' <level> messages (PART 2).
 * ==================================================================== */

typedef struct {
    int uart; /* used when fd < 0 */
    int fd;
    pthread_mutex_t wmutex;
    uint8_t rbuf[512];
    size_t rpos, rlen;
} wire_t;

static bool fd_write_all(int fd, const uint8_t *buf, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        buf += w;
        n -= (size_t)w;
    }
    return true;
}

static bool fd_read_all(int fd, uint8_t *buf, size_t n)
{
    while (n > 0) {
        ssize_t r = read(fd, buf, n);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return false;
        }
        buf += r;
        n -= (size_t)r;
    }
    return true;
}

static void wire_send_uart(wire_t *w, const uint8_t *data, size_t n)
{
    if (w->fd < 0) {
        host_uart_test_push_rx(w->uart, data, n);
        return;
    }
    uint8_t hdr[3] = {'U', (uint8_t)(n & 0xFF), (uint8_t)(n >> 8)};
    pthread_mutex_lock(&w->wmutex);
    fd_write_all(w->fd, hdr, sizeof(hdr));
    fd_write_all(w->fd, data, n);
    pthread_mutex_unlock(&w->wmutex);
}

static void wire_send_detect(wire_t *w, int level)
{
    uint8_t msg[2] = {'D', (uint8_t)level};
    pthread_mutex_lock(&w->wmutex);
    fd_write_all(w->fd, msg, sizeof(msg));
    pthread_mutex_unlock(&w->wmutex);
}

/* One UART byte from the neighbour, or false after timeout_ms. A 'D' message
 * (the neighbour's DETECT line) is reported through *detect_level when the
 * caller asked for it, and skipped otherwise. */
static bool wire_take_uart_byte(wire_t *w, uint8_t *out, uint32_t timeout_ms, int *detect_level)
{
    if (w->fd < 0) {
        return host_uart_test_take_tx_byte(w->uart, out, timeout_ms);
    }
    while (w->rpos == w->rlen) {
        struct pollfd pfd = {.fd = w->fd, .events = POLLIN};
        if (poll(&pfd, 1, (int)timeout_ms) <= 0) {
            return false;
        }
        uint8_t type;
        if (!fd_read_all(w->fd, &type, 1)) {
            sleep_ms(timeout_ms);
            return false;
        }
        if (type == 'D') {
            uint8_t level;
            if (fd_read_all(w->fd, &level, 1) && detect_level != NULL) {
                *detect_level = level;
            }
            continue;
        }
        uint8_t len2[2];
        if (!fd_read_all(w->fd, len2, 2)) {
            return false;
        }
        size_t n = (size_t)len2[0] | ((size_t)len2[1] << 8);
        if (n > sizeof(w->rbuf) || !fd_read_all(w->fd, w->rbuf, n)) {
            return false;
        }
        w->rpos = 0;
        w->rlen = n;
    }
    *out = w->rbuf[w->rpos++];
    return true;
}

/* ====================================================================
 * Port-event log (PART 1) / status lines (PART 2). Recorded AFTER the real
 * dispatcher has run.
 * ==================================================================== */

#define EVT_LOG_CAP 2048
typedef struct {
    pogo_port_id_t port;
    pogo_port_event_t event;
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
        s_evt_count++;
    }
    pthread_mutex_unlock(&s_evt_mutex);
    status_line("E %d %d\n", (int)port_id, (int)event);
}

static int evt_mark(void)
{
    pthread_mutex_lock(&s_evt_mutex);
    int v = s_evt_count;
    pthread_mutex_unlock(&s_evt_mutex);
    return v;
}

static int count_events_since(int from, pogo_port_id_t port, pogo_port_event_t event)
{
    int n = 0;
    pthread_mutex_lock(&s_evt_mutex);
    for (int i = from; i < s_evt_count; i++) {
        if (s_evt_log[i].port == port && s_evt_log[i].event == event) {
            n++;
        }
    }
    pthread_mutex_unlock(&s_evt_mutex);
    return n;
}

/* Every non-frame event on `port` since `from`: 0 means the port's physical
 * and handshake state never changed. */
static int count_port_events_since(int from, pogo_port_id_t port)
{
    int n = 0;
    pthread_mutex_lock(&s_evt_mutex);
    for (int i = from; i < s_evt_count; i++) {
        if (s_evt_log[i].port == port) {
            n++;
        }
    }
    pthread_mutex_unlock(&s_evt_mutex);
    return n;
}

static bool wait_for_event(int from, pogo_port_id_t port, pogo_port_event_t event, uint32_t timeout_ms)
{
    uint32_t start = now_ms();
    while (now_ms() - start <= timeout_ms) {
        if (count_events_since(from, port, event) >= 1) {
            return true;
        }
        sleep_ms(5);
    }
    return count_events_since(from, port, event) >= 1;
}

/* ====================================================================
 * The boards at the other ends of the POGO wires.
 * ==================================================================== */

#define PEER_INBOX_CAP 16
#define ORDER_CAP 24

typedef enum {
    CHILD_JOIN_NORMAL = 0, /* JOIN_RESULT (success, a fresh seq) after join_delay_ms */
    CHILD_JOIN_NEVER,      /* SET_DATASET is ACKed, no JOIN_RESULT follows (a join still under way) */
    CHILD_JOIN_REPLAY_OLD, /* answers with the PREVIOUS session's JOIN_RESULT, seq and all */
    CHILD_JOIN_REPLAY_SEQ, /* answers with a JOIN_RESULT of the seq in replay_seq (not this session's) */
} child_join_mode_t;

/* What one provisioning session gave the DOWNSTREAM board. */
typedef struct {
    pogo_collector_endpoint_t collector;
    bool has_collector;
    uint8_t parent_uuid[POGO_UUID_LEN];
    bool parent_is_power_base;
    uint8_t depth;
    uint8_t dataset0;
    uint16_t dataset_len;
    uint8_t order[ORDER_CAP]; /* message types, in arrival order */
    int order_len;
} child_session_t;

typedef struct {
    wire_t wire;
    uint8_t peer_type;
    atomic_bool alive;             /* answers HELLO_REQ and PING at all */
    bool auto_child;               /* answers parent_service's provisioning requests on its own */
    atomic_bool mute_provisioning; /* auto child: HELLO/PING only, every provisioning request unanswered */
    atomic_int join_mode;
    atomic_uint join_delay_ms;
    atomic_uint replay_seq;             /* CHILD_JOIN_REPLAY_SEQ */
    /* One shot: the next new SET_DATASET is not ACKed (as if its ACK were
     * lost on the wire) and the JOIN_RESULT goes out at once. The parent's
     * repeat of that SET_DATASET is ACKed. */
    atomic_bool withhold_dataset_ack;
    /* Like child_service: a JOIN_RESULT that was not ACKed is sent again
     * with the same seq. */
    atomic_bool resend_unacked;

    pthread_mutex_t mutex;
    pogo_frame_t inbox[PEER_INBOX_CAP];
    int inbox_count;

    /* auto child */
    int identify_frames;   /* every IDENTIFY_REQ, retransmissions included */
    int identify_sessions; /* IDENTIFY_REQs with a new seq: provisioning sessions the parent started */
    bool have_identify_seq;
    uint8_t last_identify_seq;
    uint32_t last_identify_ms;
    int set_datasets;
    child_session_t cur;
    child_session_t last_done;
    int sessions_done;         /* JOIN_RESULT_ACKs for the session's own JOIN_RESULT */
    int done_with[256];        /* ... by last byte of that session's collector address */
    int join_acks_total;       /* every JOIN_RESULT_ACK, whatever its seq */
    uint8_t join_seq_counter;
    uint8_t cur_join_seq;
    bool cur_join_sent;
    uint8_t prev_done_join_seq;
    bool join_due;
    uint32_t join_due_ms;
    uint8_t join_due_seq;
    bool have_dataset_seq; /* a repeated SET_DATASET (same seq) is the same request */
    uint8_t last_dataset_seq;
    uint32_t join_sent_ms;
    int join_resends;

    /* scripted board: arrival numbers of the frames in `inbox` */
    uint32_t inbox_arrival[PEER_INBOX_CAP];
    uint32_t arrivals;

    pthread_t thread;
} fake_peer_t;

static fake_peer_t s_pb = {.wire = {.uart = UP_UART, .fd = -1}, .peer_type = POGO_PEER_TYPE_POWER_BASE};
static fake_peer_t s_child = {.wire = {.uart = DOWN_UART, .fd = -1}, .peer_type = POGO_PEER_TYPE_MODULE,
                              .auto_child = true};

static void peer_send(fake_peer_t *p, uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    uint8_t buf[POGO_FRAME_MAX_SIZE];
    size_t n = pogo_frame_encode(type, seq, payload, len, buf, sizeof(buf));
    wire_send_uart(&p->wire, buf, n);
}

static void peer_send_ack(fake_peer_t *p, uint8_t seq, uint8_t acked_type)
{
    pogo_ack_t ack = {.acked_type = acked_type};
    uint8_t payload[2];
    size_t len = pogo_pack_ack(&ack, payload, sizeof(payload));
    peer_send(p, POGO_MSG_ACK, seq, payload, (uint16_t)len);
}

static void peer_send_join_result(fake_peer_t *p, uint8_t seq)
{
    pogo_join_result_t result = {.success = 1, .role = (uint8_t)OT_DEVICE_ROLE_CHILD};
    uint8_t payload[2];
    size_t len = pogo_pack_join_result(&result, payload, sizeof(payload));
    peer_send(p, POGO_MSG_JOIN_RESULT, seq, payload, (uint16_t)len);
}

/* Must hold p->mutex. */
static void child_note(fake_peer_t *p, uint8_t type)
{
    if (p->cur.order_len < ORDER_CAP) {
        p->cur.order[p->cur.order_len++] = type;
    }
}

/* A docked sensor module's side of the provisioning sequence, in the order
 * child_service.c implements it. Like child_service.c, a new IDENTIFY_REQ
 * starts over and drops a JOIN_RESULT still waiting to be sent. */
static void auto_child_handle(fake_peer_t *p, const pogo_frame_t *f)
{
    uint8_t payload[16];
    size_t len;
    if (f->type == POGO_MSG_JOIN_RESULT_ACK) {
        pthread_mutex_lock(&p->mutex);
        p->join_acks_total++;
        if (p->cur_join_sent && f->seq == p->cur_join_seq) {
            p->cur_join_sent = false;
            p->sessions_done++;
            p->last_done = p->cur;
            p->done_with[p->cur.collector.addr[POGO_COLLECTOR_ADDR_LEN - 1]]++;
            p->prev_done_join_seq = f->seq;
        }
        pthread_mutex_unlock(&p->mutex);
        return;
    }
    if (p->mute_provisioning) {
        if (f->type == POGO_MSG_IDENTIFY_REQ) {
            pthread_mutex_lock(&p->mutex);
            p->identify_frames++;
            if (!p->have_identify_seq || p->last_identify_seq != f->seq) {
                p->identify_sessions++;
                p->have_identify_seq = true;
                p->last_identify_seq = f->seq;
                p->last_identify_ms = now_ms();
            }
            pthread_mutex_unlock(&p->mutex);
        }
        return;
    }

    pthread_mutex_lock(&p->mutex);
    if (f->type == POGO_MSG_IDENTIFY_REQ) {
        p->identify_frames++;
        if (!p->have_identify_seq || p->last_identify_seq != f->seq) {
            p->identify_sessions++;
            p->have_identify_seq = true;
            p->last_identify_seq = f->seq;
            p->last_identify_ms = now_ms();
            memset(&p->cur, 0, sizeof(p->cur));
            p->join_due = false;
            p->cur_join_sent = false;
            p->have_dataset_seq = false;
        }
    }
    child_note(p, f->type);
    pthread_mutex_unlock(&p->mutex);

    switch (f->type) {
    case POGO_MSG_IDENTIFY_REQ: {
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
            p->cur.depth = slot.topology_depth;
            pthread_mutex_unlock(&p->mutex);
        }
        peer_send_ack(p, f->seq, f->type);
        break;
    }
    case POGO_MSG_ASSIGN_PARENT_INFO: {
        pogo_assign_parent_info_t info;
        if (pogo_unpack_assign_parent_info(f->payload, f->len, &info)) {
            pthread_mutex_lock(&p->mutex);
            memcpy(p->cur.parent_uuid, info.parent_uuid, POGO_UUID_LEN);
            p->cur.parent_is_power_base = info.parent_is_power_base != 0;
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
            p->cur.collector = ep;
            p->cur.has_collector = true;
            pthread_mutex_unlock(&p->mutex);
        }
        peer_send_ack(p, f->seq, f->type);
        break;
    }
    case POGO_MSG_SET_DATASET: {
        pthread_mutex_lock(&p->mutex);
        const bool again = p->have_dataset_seq && p->last_dataset_seq == f->seq;
        pthread_mutex_unlock(&p->mutex);
        const bool withhold = !again && atomic_exchange(&p->withhold_dataset_ack, false);
        if (!withhold) {
            peer_send_ack(p, f->seq, f->type);
        }
        if (again) {
            break;
        }
        pthread_mutex_lock(&p->mutex);
        p->have_dataset_seq = true;
        p->last_dataset_seq = f->seq;
        p->set_datasets++;
        p->cur.dataset0 = (f->len > 0) ? f->payload[0] : 0;
        p->cur.dataset_len = f->len;
        int mode = p->join_mode;
        if (mode == CHILD_JOIN_NORMAL) {
            p->join_seq_counter++;
            if (p->join_seq_counter == 0) {
                p->join_seq_counter = 1;
            }
            p->cur_join_seq = p->join_seq_counter;
            p->join_due = true;
            p->join_due_seq = p->cur_join_seq;
            p->join_due_ms = now_ms() + (withhold ? 0 : p->join_delay_ms);
        } else if (mode == CHILD_JOIN_REPLAY_OLD) {
            p->join_due = true;
            p->join_due_seq = p->prev_done_join_seq;
            p->join_due_ms = now_ms();
        } else if (mode == CHILD_JOIN_REPLAY_SEQ) {
            p->join_due = true;
            p->join_due_seq = (uint8_t)p->replay_seq;
            p->join_due_ms = now_ms();
        }
        pthread_mutex_unlock(&p->mutex);
        break;
    }
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
        if (p->auto_child) {
            bool send = false;
            uint8_t seq = 0;
            pthread_mutex_lock(&p->mutex);
            if (p->join_due && (int32_t)(now_ms() - p->join_due_ms) >= 0) {
                p->join_due = false;
                send = true;
                seq = p->join_due_seq;
                if (seq == p->cur_join_seq && p->join_mode == CHILD_JOIN_NORMAL) {
                    p->cur_join_sent = true;
                    p->join_sent_ms = now_ms();
                    p->join_resends = 0;
                }
            } else if (p->resend_unacked && p->cur_join_sent && p->join_resends < CONFIG_POGO_MAX_RETRY &&
                       (int32_t)(now_ms() - p->join_sent_ms) >= 2 * CONFIG_POGO_ACK_TIMEOUT_MS) {
                send = true;
                seq = p->cur_join_seq;
                p->join_sent_ms = now_ms();
                p->join_resends++;
            }
            pthread_mutex_unlock(&p->mutex);
            if (send) {
                peer_send_join_result(p, seq);
            }
        }
        uint8_t b;
        if (!wire_take_uart_byte(&p->wire, &b, 5, NULL)) {
            continue;
        }
        pogo_frame_t frame;
        if (!pogo_frame_parser_feed(&parser, b, &frame)) {
            continue;
        }
        if (!p->alive) {
            continue;
        }
        if (frame.type == POGO_MSG_PEER_HELLO_REQ) {
            uint8_t payload[3];
            pogo_peer_hello_t hello = {.protocol_version = POGO_FRAME_VERSION, .peer_type = p->peer_type,
                                        .capabilities = POGO_HELLO_SUPPORTED_CAPABILITIES};
            size_t len = pogo_pack_peer_hello(&hello, payload, sizeof(payload));
            peer_send(p, POGO_MSG_PEER_HELLO_RESP, frame.seq, payload, (uint16_t)len);
        } else if (frame.type == POGO_MSG_PING) {
            peer_send(p, POGO_MSG_PONG, frame.seq, NULL, 0);
        } else if (frame.type == POGO_MSG_PONG || frame.type == POGO_MSG_PEER_HELLO_RESP) {
            /* nothing to do */
        } else if (p->auto_child) {
            auto_child_handle(p, &frame);
        } else {
            pthread_mutex_lock(&p->mutex);
            p->arrivals++;
            if (p->inbox_count < PEER_INBOX_CAP) {
                p->inbox_arrival[p->inbox_count] = p->arrivals;
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
    pthread_mutex_init(&p->wire.wmutex, NULL);
    pthread_create(&p->thread, NULL, peer_thread, p);
}

#define CHILD_GET(type, name, expr)                                                                                  \
    static type name(void)                                                                                           \
    {                                                                                                                \
        pthread_mutex_lock(&s_child.mutex);                                                                          \
        type v = (expr);                                                                                             \
        pthread_mutex_unlock(&s_child.mutex);                                                                        \
        return v;                                                                                                    \
    }

CHILD_GET(int, child_identify_sessions, s_child.identify_sessions)
CHILD_GET(int, child_sessions_done, s_child.sessions_done)
CHILD_GET(int, child_set_datasets, s_child.set_datasets)
CHILD_GET(int, child_join_acks_total, s_child.join_acks_total)
CHILD_GET(uint32_t, child_last_identify_ms, s_child.last_identify_ms)
CHILD_GET(child_session_t, child_last_done, s_child.last_done)
CHILD_GET(uint8_t, child_prev_done_join_seq, s_child.prev_done_join_seq)

/* The board below as if it had restarted, or run for a long time: where its
 * JOIN_RESULT numbering stands. */
static void child_set_join_counter(uint8_t value)
{
    pthread_mutex_lock(&s_child.mutex);
    s_child.join_seq_counter = value;
    pthread_mutex_unlock(&s_child.mutex);
}

/* The number of a JOIN_RESULT the board below made for the session it is in
 * (and that nobody has seen yet). */
static uint8_t child_take_join_seq(void)
{
    pthread_mutex_lock(&s_child.mutex);
    s_child.join_seq_counter++;
    if (s_child.join_seq_counter == 0) {
        s_child.join_seq_counter = 1;
    }
    uint8_t seq = s_child.join_seq_counter;
    pthread_mutex_unlock(&s_child.mutex);
    return seq;
}

static int child_done_with(uint8_t dest_last)
{
    pthread_mutex_lock(&s_child.mutex);
    int v = s_child.done_with[dest_last];
    pthread_mutex_unlock(&s_child.mutex);
    return v;
}

/* --- the scripted UPSTREAM board's side of provisioning --- */

typedef struct {
    uint8_t depth;             /* the depth THIS module is assigned */
    const uint8_t *parent_uuid;
    bool parent_is_power_base;
    uint8_t collector_last;    /* last byte of the collector address */
    uint8_t dataset0;          /* first Dataset byte */
    uint8_t dataset_len;
} upstream_gen_t;

static uint8_t s_pb_seq;
static uint32_t s_pb_collector_generation;
static uint32_t s_pb_taken_arrival; /* arrival number of the frame pb_take_frame() returned last */
static uint8_t s_pb_join_seq;       /* seq of the JOIN_RESULT the last pb_provision*() received */

static bool pb_take_frame(fake_peer_t *pb, uint8_t type, bool match_seq, uint8_t seq, pogo_frame_t *out,
                           uint32_t timeout_ms)
{
    uint32_t start = now_ms();
    while (now_ms() - start <= timeout_ms) {
        bool found = false;
        pthread_mutex_lock(&pb->mutex);
        for (int i = 0; i < pb->inbox_count; i++) {
            if (pb->inbox[i].type == type && (!match_seq || pb->inbox[i].seq == seq)) {
                *out = pb->inbox[i];
                s_pb_taken_arrival = pb->inbox_arrival[i];
                memmove(&pb->inbox[i], &pb->inbox[i + 1], sizeof(pogo_frame_t) * (size_t)(pb->inbox_count - i - 1));
                memmove(&pb->inbox_arrival[i], &pb->inbox_arrival[i + 1],
                        sizeof(uint32_t) * (size_t)(pb->inbox_count - i - 1));
                pb->inbox_count--;
                found = true;
                break;
            }
        }
        pthread_mutex_unlock(&pb->mutex);
        if (found) {
            return true;
        }
        sleep_ms(2);
    }
    return false;
}

static bool pb_request(fake_peer_t *pb, uint8_t type, const uint8_t *payload, size_t len, uint8_t expect,
                        pogo_frame_t *out, uint32_t timeout_ms)
{
    uint8_t seq = ++s_pb_seq;
    peer_send(pb, type, seq, payload, (uint16_t)len);
    return pb_take_frame(pb, expect, true, seq, out, timeout_ms);
}

/* IDENTIFY .. SET_DATASET .. JOIN_RESULT_ACK, in parent_service.c's order.
 * `ack_join_result` false: the JOIN_RESULT is received and left unACKed. */
static bool pb_provision_opt(fake_peer_t *pb, const upstream_gen_t *g, bool ack_join_result)
{
    pogo_frame_t f;
    uint8_t payload[OT_OPERATIONAL_DATASET_MAX_LENGTH];
    size_t len;
    pthread_mutex_lock(&pb->mutex);
    pb->inbox_count = 0;
    pthread_mutex_unlock(&pb->mutex);

#define PB_STEP(ok, what)                                                                                            \
    do {                                                                                                             \
        if (!(ok)) {                                                                                                 \
            printf("info: upstream provisioning stopped at %s\n", what);                                             \
            return false;                                                                                            \
        }                                                                                                            \
    } while (0)

    PB_STEP(pb_request(pb, POGO_MSG_IDENTIFY_REQ, NULL, 0, POGO_MSG_IDENTIFY_RESP, &f, 800), "IDENTIFY");
    PB_STEP(pb_request(pb, POGO_MSG_REQUEST_UUID, NULL, 0, POGO_MSG_PROPOSE_UUID, &f, 800), "REQUEST_UUID");
    pogo_propose_uuid_t proposed;
    PB_STEP(pogo_unpack_propose_uuid(f.payload, f.len, &proposed), "PROPOSE_UUID decode");

    pogo_assign_uuid_t assign;
    memcpy(assign.uuid, proposed.uuid, POGO_UUID_LEN);
    len = pogo_pack_assign_uuid(&assign, payload, sizeof(payload));
    PB_STEP(pb_request(pb, POGO_MSG_ASSIGN_UUID, payload, len, POGO_MSG_ACK, &f, 800), "ASSIGN_UUID");

    pogo_assign_slot_t slot = {.slot_id = 0, .topology_depth = g->depth};
    len = pogo_pack_assign_slot(&slot, payload, sizeof(payload));
    PB_STEP(pb_request(pb, POGO_MSG_ASSIGN_SLOT, payload, len, POGO_MSG_ACK, &f, 800), "ASSIGN_SLOT");

    pogo_assign_parent_info_t pinfo = {.parent_is_power_base = g->parent_is_power_base ? 1 : 0};
    memcpy(pinfo.parent_uuid, g->parent_uuid, POGO_UUID_LEN);
    len = pogo_pack_assign_parent_info(&pinfo, payload, sizeof(payload));
    PB_STEP(pb_request(pb, POGO_MSG_ASSIGN_PARENT_INFO, payload, len, POGO_MSG_ACK, &f, 800), "ASSIGN_PARENT_INFO");

    PB_STEP(pb_request(pb, POGO_MSG_SENSOR_HELLO, NULL, 0, POGO_MSG_SENSOR_INFO_ACK, &f, 2000), "SENSOR_HELLO");

    pogo_poll_config_t poll = {.polling_period_ms = 100};
    len = pogo_pack_poll_config(&poll, payload, sizeof(payload));
    PB_STEP(pb_request(pb, POGO_MSG_POLL_CONFIG, payload, len, POGO_MSG_ACK, &f, 800), "POLL_CONFIG");

    pogo_collector_endpoint_t collector;
    memset(&collector, 0, sizeof(collector));
    memcpy(collector.power_base_uuid, PB_UUID, POGO_UUID_LEN);
    collector.addr[0] = 0xFD; /* ULA: not unspecified/multicast/link-local */
    collector.addr[POGO_COLLECTOR_ADDR_LEN - 1] = g->collector_last;
    collector.port = 5683;
    collector.generation = ++s_pb_collector_generation;
    len = pogo_pack_set_collector(&collector, payload, sizeof(payload));
    PB_STEP(pb_request(pb, POGO_MSG_SET_COLLECTOR, payload, len, POGO_MSG_ACK, &f, 800), "SET_COLLECTOR");

    memset(payload, g->dataset0, g->dataset_len);
    PB_STEP(pb_request(pb, POGO_MSG_SET_DATASET, payload, g->dataset_len, POGO_MSG_ACK, &f, 800), "SET_DATASET");

    PB_STEP(pb_take_frame(pb, POGO_MSG_JOIN_RESULT, false, 0, &f, 2000), "JOIN_RESULT");
    pogo_join_result_t result;
    PB_STEP(pogo_unpack_join_result(f.payload, f.len, &result) && result.success == 1, "JOIN_RESULT success");
    s_pb_join_seq = f.seq;
    if (ack_join_result) {
        peer_send(pb, POGO_MSG_JOIN_RESULT_ACK, f.seq, NULL, 0);
    }
#undef PB_STEP
    return true;
}

static bool pb_provision(fake_peer_t *pb, const upstream_gen_t *g)
{
    return pb_provision_opt(pb, g, true);
}

/* JOIN_RESULT frames in the scripted board's inbox that arrived after frame
 * number `after`. */
static int pb_join_results_after(fake_peer_t *pb, uint32_t after)
{
    int n = 0;
    pthread_mutex_lock(&pb->mutex);
    for (int i = 0; i < pb->inbox_count; i++) {
        if (pb->inbox[i].type == POGO_MSG_JOIN_RESULT && pb->inbox_arrival[i] > after) {
            n++;
        }
    }
    pthread_mutex_unlock(&pb->mutex);
    return n;
}

/* ====================================================================
 * Boot: the same order module/main/app_main.c uses.
 * ==================================================================== */

static uint8_t s_module_uuid[POGO_UUID_LEN];

static bool boot_module(void)
{
    host_gpio_test_reset();
    host_uart_test_reset();

    module_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = MODULE_CONFIG_SCHEMA_VERSION;
    cfg.driver_id = SENSOR_DRIVER_ID_MOCK;
    cfg.sensor_type_bitmap = 0x0001;
    cfg.measurement_timeout_ms = 500;

    bool ok = sensor_service_init(&cfg) == ESP_OK;
    ok = ok && topology_gate_init() == ESP_OK;
    ok = ok && collector_endpoint_init() == ESP_OK;
    ok = ok && sensor_transport_init(true) == ESP_OK;
    sensor_service_set_sample_ready_cb(sensor_transport_send_sample);
    ok = ok && role_manager_init(MODULE_DEV_MODE_AUTO, module_wiring_on_role_change, NULL) == ESP_OK;

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
    ok = ok && pogo_port_init(POGO_PORT_UPSTREAM, &up_cfg, recording_dispatcher, NULL) == ESP_OK;
    ok = ok && pogo_port_init(POGO_PORT_DOWNSTREAM, &down_cfg, recording_dispatcher, NULL) == ESP_OK;
    ok = ok && child_service_init(&cfg) == ESP_OK;
    ok = ok && parent_service_init(module_wiring_parent_context()) == ESP_OK;
    ok = ok && topology_reporter_init(&cfg) == ESP_OK;
    ok = ok && pogo_port_enable(POGO_PORT_UPSTREAM, POGO_PEER_TYPE_MODULE) == ESP_OK;
    ok = ok && pogo_port_enable(POGO_PORT_DOWNSTREAM, POGO_PEER_TYPE_MODULE) == ESP_OK;
    sensor_service_get_module_uuid(s_module_uuid);
    return ok;
}

/* ====================================================================
 * PART 1 phases
 * ==================================================================== */

static const uint8_t EXPECTED_ORDER[] = {
    POGO_MSG_IDENTIFY_REQ, POGO_MSG_REQUEST_UUID,       POGO_MSG_ASSIGN_UUID,
    POGO_MSG_ASSIGN_SLOT,  POGO_MSG_ASSIGN_PARENT_INFO, POGO_MSG_SENSOR_HELLO,
    POGO_MSG_POLL_CONFIG,  POGO_MSG_SET_COLLECTOR,      POGO_MSG_SET_DATASET,
};

/* The DOWNSTREAM board's last completed session carries exactly what `g`
 * gave this module, one level further down, in the provisioning order. */
static bool child_has_generation(const upstream_gen_t *g)
{
    child_session_t s = child_last_done();
    if (!s.has_collector || s.collector.addr[POGO_COLLECTOR_ADDR_LEN - 1] != g->collector_last ||
        memcmp(s.collector.power_base_uuid, PB_UUID, POGO_UUID_LEN) != 0 || s.collector.port != 5683) {
        return false;
    }
    if (s.dataset0 != g->dataset0 || s.dataset_len != g->dataset_len) {
        return false;
    }
    if (s.depth != (uint8_t)(g->depth + 1) || s.parent_is_power_base ||
        memcmp(s.parent_uuid, s_module_uuid, POGO_UUID_LEN) != 0) {
        return false;
    }
    /* A retransmitted request shows up twice; the order of first arrivals is
     * what counts. */
    size_t want = 0;
    for (int i = 0; i < s.order_len && want < sizeof(EXPECTED_ORDER); i++) {
        if (s.order[i] == EXPECTED_ORDER[want]) {
            want++;
        } else if (want == 0 || s.order[i] != EXPECTED_ORDER[want - 1]) {
            return false;
        }
    }
    return want == sizeof(EXPECTED_ORDER);
}

static int s_want_sessions_done;
static bool pred_child_sessions_done(void)
{
    return child_sessions_done() >= s_want_sessions_done;
}

static bool wait_child_sessions_done(int n, uint32_t timeout_ms)
{
    s_want_sessions_done = n;
    return wait_until(pred_child_sessions_done, timeout_ms);
}

static int s_want_identify_sessions;
static bool pred_child_identify_sessions(void)
{
    return child_identify_sessions() >= s_want_identify_sessions;
}

static int s_want_set_datasets;
static bool pred_child_set_datasets(void)
{
    return child_set_datasets() >= s_want_set_datasets;
}

static bool pred_operational(void)
{
    return role_manager_is_upstream_ready() && topology_gate_is_open();
}

static uint8_t s_want_sample_dest;
static int s_want_sample_floor;
static bool pred_samples_to_dest(void)
{
    return samples_to(s_want_sample_dest) > s_want_sample_floor;
}

/* New "/s" samples reach the collector at ...:dest_last. */
static bool samples_reach(uint8_t dest_last, uint32_t timeout_ms)
{
    s_want_sample_dest = dest_last;
    s_want_sample_floor = samples_to(dest_last);
    return wait_until(pred_samples_to_dest, timeout_ms);
}

static const upstream_gen_t GEN_A = {.depth = 1, .parent_uuid = PB_UUID, .parent_is_power_base = true,
                                     .collector_last = 0x01, .dataset0 = 0xA1, .dataset_len = 4};
static const upstream_gen_t GEN_B = {.depth = 1, .parent_uuid = PB_UUID, .parent_is_power_base = true,
                                     .collector_last = 0x02, .dataset0 = 0xB2, .dataset_len = 5};
static const upstream_gen_t GEN_C = {.depth = 2, .parent_uuid = OTHER_PARENT_UUID, .parent_is_power_base = false,
                                     .collector_last = 0x03, .dataset0 = 0xC3, .dataset_len = 6};
static const upstream_gen_t GEN_D = {.depth = 1, .parent_uuid = PB_UUID, .parent_is_power_base = true,
                                     .collector_last = 0x04, .dataset0 = 0xD4, .dataset_len = 7};
static const upstream_gen_t GEN_E = {.depth = 1, .parent_uuid = PB_UUID, .parent_is_power_base = true,
                                     .collector_last = 0x05, .dataset0 = 0xE5, .dataset_len = 8};
static const upstream_gen_t GEN_F = {.depth = 1, .parent_uuid = PB_UUID, .parent_is_power_base = true,
                                     .collector_last = 0x06, .dataset0 = 0xF6, .dataset_len = 9};
static const upstream_gen_t GEN_G = {.depth = 1, .parent_uuid = PB_UUID, .parent_is_power_base = true,
                                     .collector_last = 0x07, .dataset0 = 0x17, .dataset_len = 10};
static const upstream_gen_t GEN_H = {.depth = 1, .parent_uuid = PB_UUID, .parent_is_power_base = true,
                                     .collector_last = 0x08, .dataset0 = 0x18, .dataset_len = 11};
#define GEN_MORE(name, last, d0, dlen)                                                                               \
    static const upstream_gen_t name = {.depth = 1, .parent_uuid = PB_UUID, .parent_is_power_base = true,           \
                                        .collector_last = last, .dataset0 = d0, .dataset_len = dlen}
GEN_MORE(GEN_I, 0x09, 0x19, 12);
GEN_MORE(GEN_J, 0x0a, 0x1a, 13);
GEN_MORE(GEN_K, 0x0b, 0x1b, 14);
GEN_MORE(GEN_L, 0x0c, 0x1c, 15);
GEN_MORE(GEN_M, 0x0d, 0x1d, 16);
GEN_MORE(GEN_N, 0x0e, 0x1e, 17);
GEN_MORE(GEN_O, 0x0f, 0x1f, 18);
GEN_MORE(GEN_P, 0x10, 0x20, 19);
GEN_MORE(GEN_Q, 0x11, 0x21, 20);
GEN_MORE(GEN_R, 0x12, 0x22, 21);
#undef GEN_MORE

static void phase_1_first_connection(void)
{
    printf("\n-- 1: first connection -- Base provisions this module, this module provisions the one below --\n");
    int mark = evt_mark();
    s_pb.alive = true;
    host_gpio_test_edge(UP_DETECT_IN, 0);
    CHECK(wait_for_event(mark, POGO_PORT_UPSTREAM, POGO_PORT_EVENT_PEER_READY, 1500), "UPSTREAM reaches PEER_READY");
    CHECK(pb_provision(&s_pb, &GEN_A), "UPSTREAM generation A is delivered and joined (real child_service)");
    CHECK(wait_until(pred_operational, 3000), "joined and registered, topology gate open");
    CHECK(role_manager_get_role() == MODULE_ROLE_LEAF, "role is LEAF with nothing below");

    s_child.alive = true;
    host_gpio_test_edge(DOWN_DETECT_IN, 0);
    CHECK(wait_for_event(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_PEER_READY, 1500), "DOWNSTREAM reaches PEER_READY");
    CHECK(wait_child_sessions_done(1, 3000), "the DOWNSTREAM board is provisioned to completion (JOIN_RESULT_ACK)");
    CHECK(child_has_generation(&GEN_A), "it received generation A: collector, Dataset, depth 2, this module as parent");
    CHECK(child_identify_sessions() == 1, "in exactly one provisioning session");
    CHECK(role_manager_get_role() == MODULE_ROLE_INTERMEDIATE, "role is INTERMEDIATE");
    CHECK(samples_reach(GEN_A.collector_last, 2000), "this module's own samples reach collector A");
}

static void phase_2_same_generation_is_left_alone(void)
{
    printf("\n-- 2: same generation -- radio role changes, time passing, a duplicate JOIN_RESULT --\n");
    int mark = evt_mark();
    int sessions = child_identify_sessions();
    int done = child_sessions_done();
    int joins = tm_join_calls();
    int reports = report_count();

    tm_radio_role_event(OT_DEVICE_ROLE_ROUTER); /* promoted */
    sleep_ms(300);
    tm_radio_role_event(OT_DEVICE_ROLE_DETACHED); /* short radio loss */
    sleep_ms(200);
    tm_radio_role_event(OT_DEVICE_ROLE_ROUTER);
    sleep_ms(1500); /* several parent_service / topology_reporter ticks */

    CHECK(child_identify_sessions() == sessions, "no new DOWNSTREAM provisioning session was started");
    CHECK(child_sessions_done() == done, "no session was completed again");
    CHECK(tm_join_calls() == joins, "this module did not re-join Thread");
    topology_report_t rep;
    CHECK(!find_attached_report(reports, GEN_A.collector_last, &rep), "no new ATTACHED topology report was sent");
    CHECK(count_port_events_since(mark, POGO_PORT_DOWNSTREAM) == 0 &&
              count_port_events_since(mark, POGO_PORT_UPSTREAM) == 0,
          "neither port saw an event");

    /* JOIN_RESULT re-ACK: the child never saw the ACK and sends it again. */
    int acks = child_join_acks_total();
    peer_send_join_result(&s_child, child_prev_done_join_seq());
    sleep_ms(400);
    CHECK(child_join_acks_total() == acks + 1, "a duplicate JOIN_RESULT is ACKed again");
    CHECK(child_identify_sessions() == sessions && child_sessions_done() == done,
          "and starts nothing new");
}

/* The defect: UPSTREAM is physically re-docked and provisioned again while
 * DOWNSTREAM stays docked. */
static void phase_3_upstream_redock_new_generation(void)
{
    printf("\n-- 3: Base removed and enrolled again: UPSTREAM detaches, returns, new generation B; "
           "DOWNSTREAM never moves --\n");
    int sessions = child_identify_sessions();
    int done = child_sessions_done();
    int stops = tm_stop_calls();
    int reports = report_count();

    int mark = evt_mark();
    host_gpio_test_edge(UP_DETECT_IN, 1);
    CHECK(wait_for_event(mark, POGO_PORT_UPSTREAM, POGO_PORT_EVENT_DETACHED, 1500), "UPSTREAM DETACHED");
    sleep_ms(300);
    CHECK(role_manager_get_role() == MODULE_ROLE_UNASSIGNED && tm_stop_calls() == stops + 1,
          "role UNASSIGNED, this module's Thread stopped (no auto-rejoin)");
    CHECK(child_identify_sessions() == sessions, "nothing is sent DOWNSTREAM while there is no UPSTREAM generation");

    int mark2 = evt_mark();
    host_gpio_test_edge(UP_DETECT_IN, 0);
    CHECK(wait_for_event(mark2, POGO_PORT_UPSTREAM, POGO_PORT_EVENT_PEER_READY, 1500), "UPSTREAM PEER_READY again");
    CHECK(pb_provision(&s_pb, &GEN_B), "UPSTREAM generation B is delivered and joined");

    CHECK(wait_child_sessions_done(done + 1, 3000),
          "the DOWNSTREAM board is provisioned again without being re-docked");
    CHECK(child_has_generation(&GEN_B), "it received generation B: new collector, new Dataset, depth, parent");
    sleep_ms(600);
    CHECK(child_identify_sessions() == sessions + 1, "in exactly one new provisioning session");
    CHECK(count_port_events_since(mark, POGO_PORT_DOWNSTREAM) == 0,
          "the DOWNSTREAM port saw no DETACHED / ATTACHED / PEER_READY / ERROR at all");
    topology_report_t rep;
    CHECK(wait_until(pred_operational, 3000) && find_attached_report(reports, GEN_B.collector_last, &rep),
          "this module registered again (ATTACHED) with collector B");
    CHECK(samples_reach(GEN_B.collector_last, 2000), "this module's own samples reach collector B");
}

/* What a module further down the chain sees: its parent starts provisioning
 * over with a new IDENTIFY_REQ, no physical change at all. */
static void phase_4_upstream_restart_without_detach(void)
{
    printf("\n-- 4: UPSTREAM starts over with a new IDENTIFY_REQ (no detach): generation C, new parent and depth --\n");
    int mark = evt_mark();
    int sessions = child_identify_sessions();
    int done = child_sessions_done();
    int reports = report_count();

    CHECK(pb_provision(&s_pb, &GEN_C), "UPSTREAM generation C is delivered and joined");
    CHECK(wait_child_sessions_done(done + 1, 3000), "the DOWNSTREAM board is provisioned again");
    CHECK(child_has_generation(&GEN_C), "it received generation C: collector, Dataset, depth 3, this module as parent");
    sleep_ms(600);
    CHECK(child_identify_sessions() == sessions + 1, "in exactly one new provisioning session");
    CHECK(count_port_events_since(mark, POGO_PORT_DOWNSTREAM) == 0 &&
              count_port_events_since(mark, POGO_PORT_UPSTREAM) == 0,
          "neither port saw a physical or handshake event");
    topology_report_t rep;
    bool found = false;
    for (int i = 0; i < 600 && !found; i++) {
        found = find_attached_report(reports, GEN_C.collector_last, &rep);
        if (!found) {
            sleep_ms(5);
        }
    }
    CHECK(found && rep.topology_depth == GEN_C.depth &&
              memcmp(rep.parent_uuid, OTHER_PARENT_UUID, POGO_UUID_LEN) == 0,
          "this module's own new ATTACHED report names the new parent and depth, sent to collector C");
    CHECK(samples_reach(GEN_C.collector_last, 2000), "this module's own samples reach collector C");
}

/* UPSTREAM changes again while the DOWNSTREAM session for the previous
 * generation is still waiting for its JOIN_RESULT. */
static void phase_5_change_during_provisioning(void)
{
    printf("\n-- 5: two UPSTREAM generations in a row; the second arrives while DOWNSTREAM is mid-provisioning --\n");
    int mark = evt_mark();
    int done = child_sessions_done();
    int sets = child_set_datasets();

    s_child.join_mode = CHILD_JOIN_NEVER; /* the board below is still joining generation D's network */
    CHECK(pb_provision(&s_pb, &GEN_D), "UPSTREAM generation D is delivered and joined");
    s_want_set_datasets = sets + 1;
    CHECK(wait_until(pred_child_set_datasets, 3000), "DOWNSTREAM got as far as SET_DATASET for generation D");
    s_child.join_mode = CHILD_JOIN_NORMAL;

    int sessions = child_identify_sessions();
    CHECK(pb_provision(&s_pb, &GEN_E), "UPSTREAM generation E is delivered and joined");
    uint32_t e_joined_ms = now_ms();
    s_want_identify_sessions = sessions + 1;
    CHECK(wait_until(pred_child_identify_sessions, 1500),
          "the wait for generation D's JOIN_RESULT is given up at once, not after the 3 s join timeout");
    CHECK((int32_t)(child_last_identify_ms() - e_joined_ms) < 1500, "a session for generation E starts right away");
    CHECK(wait_child_sessions_done(done + 1, 3000), "the DOWNSTREAM board completes a session");
    CHECK(child_has_generation(&GEN_E), "and what it holds is generation E");
    CHECK(child_done_with(GEN_D.collector_last) == 0, "no session was ever completed for the superseded generation D");
    sleep_ms(600);
    CHECK(child_sessions_done() == done + 1, "exactly one session was completed");
    CHECK(count_port_events_since(mark, POGO_PORT_DOWNSTREAM) == 0, "the DOWNSTREAM port saw no event");
}

/* A JOIN_RESULT left over from the previous session must not complete the
 * new one. */
static void phase_6_stale_join_result(void)
{
    printf("\n-- 6: the previous session's JOIN_RESULT arrives again during the new session's join wait --\n");
    int done = child_sessions_done();
    int acks = child_join_acks_total();

    s_child.join_mode = CHILD_JOIN_REPLAY_OLD;
    CHECK(pb_provision(&s_pb, &GEN_F), "UPSTREAM generation F is delivered and joined");
    int sessions_after_start = 0;
    for (int i = 0; i < 400 && child_join_acks_total() == acks; i++) {
        sleep_ms(5);
    }
    sessions_after_start = child_identify_sessions();
    CHECK(child_join_acks_total() == acks + 1, "the old JOIN_RESULT is ACKed (its sender may have missed the ACK)");
    s_child.join_mode = CHILD_JOIN_NORMAL;

    /* If the old result had been taken for the new session's, the parent
     * would now sit in JOINED for good. It must instead time the join out
     * and try again. */
    s_want_identify_sessions = sessions_after_start + 1;
    CHECK(wait_until(pred_child_identify_sessions, CONFIG_POGO_JOIN_TIMEOUT_MS + 2000),
          "the session it arrived in is not completed by it: after the join timeout a new session starts");
    CHECK(wait_child_sessions_done(done + 1, 3000), "which completes on its own, fresh JOIN_RESULT");
    CHECK(child_has_generation(&GEN_F), "with generation F");
}

/* The board below stops answering provisioning requests (PING still works):
 * bounded, slowing retries; recovery once it answers again. */
static void phase_7_retry_then_recover(void)
{
    printf("\n-- 7: DOWNSTREAM does not answer the new generation's provisioning; later it does --\n");
    int mark = evt_mark();
    int done = child_sessions_done();

    s_child.mute_provisioning = true;
    CHECK(pb_provision(&s_pb, &GEN_G), "UPSTREAM generation G is delivered and joined");
    int sessions = child_identify_sessions();
    sleep_ms(3000);
    int attempts = child_identify_sessions() - sessions;
    printf("info: %d provisioning sessions attempted in 3 s against a silent board\n", attempts);
    /* One failed session takes 4 x 40 ms here. At the fixed 60 ms pause this
     * would be about 13 sessions in 3 s; the pause doubles up to 16 x. */
    CHECK(attempts >= 3, "provisioning is retried");
    CHECK(attempts <= 8, "with a growing pause between sessions, not at a fixed fast rate");
    CHECK(child_sessions_done() == done, "nothing was completed meanwhile");
    CHECK(role_manager_is_upstream_ready() && topology_gate_is_open() && samples_reach(GEN_G.collector_last, 2000),
          "this module's own path is unaffected: joined, registered, samples reach collector G");

    s_child.mute_provisioning = false;
    CHECK(wait_child_sessions_done(done + 1, 60u * 16u + 2500u),
          "once the board answers, the next retry provisions it");
    CHECK(child_has_generation(&GEN_G), "with generation G");
    CHECK(count_port_events_since(mark, POGO_PORT_DOWNSTREAM) == 0, "the DOWNSTREAM port saw no event");
}

/* LEAF <-> INTERMEDIATE re-applies the Thread link mode (a re-join with the
 * SAME Dataset). That is not a new generation. */
static void phase_8_role_only_change(void)
{
    printf("\n-- 8: DOWNSTREAM undocks and docks again: link mode re-applied twice, ONE new session --\n");
    int mark = evt_mark();
    int done = child_sessions_done();
    int sessions = child_identify_sessions();
    int joins = tm_join_calls();
    int stops = tm_stop_calls();

    host_gpio_test_edge(DOWN_DETECT_IN, 1);
    CHECK(wait_for_event(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_DETACHED, 1500), "DOWNSTREAM DETACHED");
    sleep_ms(400);
    CHECK(role_manager_get_role() == MODULE_ROLE_LEAF && tm_join_calls() == joins + 1,
          "role LEAF, link mode re-applied with the same Dataset");
    host_gpio_test_edge(DOWN_DETECT_IN, 0);
    CHECK(wait_for_event(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_PEER_READY, 1500), "DOWNSTREAM PEER_READY again");
    CHECK(wait_child_sessions_done(done + 1, 3000), "the re-docked board is provisioned");
    CHECK(child_has_generation(&GEN_G), "with the unchanged generation G");
    sleep_ms(1200);
    CHECK(tm_join_calls() == joins + 2, "link mode re-applied once more for INTERMEDIATE");
    CHECK(child_identify_sessions() == sessions + 1 && child_sessions_done() == done + 1,
          "exactly one session: the two link-mode re-joins did not count as new UPSTREAM generations");
    CHECK(tm_stop_calls() == stops, "this module's Thread was never stopped");
}

/* DOWNSTREAM is pulled while it is being provisioned for a new generation. */
static void phase_9_detach_during_reprovisioning(void)
{
    printf("\n-- 9: DOWNSTREAM is undocked in the middle of the new generation's provisioning --\n");
    int done = child_sessions_done();
    int sets = child_set_datasets();

    s_child.join_mode = CHILD_JOIN_NEVER;
    CHECK(pb_provision(&s_pb, &GEN_H), "UPSTREAM generation H is delivered and joined");
    s_want_set_datasets = sets + 1;
    CHECK(wait_until(pred_child_set_datasets, 3000), "DOWNSTREAM got as far as SET_DATASET for generation H");
    int mark = evt_mark();
    host_gpio_test_edge(DOWN_DETECT_IN, 1);
    CHECK(wait_for_event(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_DETACHED, 1500), "DOWNSTREAM DETACHED");
    int sessions = child_identify_sessions();
    sleep_ms(800);
    CHECK(child_identify_sessions() == sessions && child_sessions_done() == done,
          "the session is dropped; nothing is sent to an empty port");
    CHECK(role_manager_is_upstream_ready() && samples_reach(GEN_H.collector_last, 2000),
          "this module itself keeps running on generation H");

    s_child.join_mode = CHILD_JOIN_NORMAL;
    host_gpio_test_edge(DOWN_DETECT_IN, 0);
    CHECK(wait_child_sessions_done(done + 1, 4000), "docked again: provisioned");
    CHECK(child_has_generation(&GEN_H), "with generation H");
}

/* ---- which JOIN_RESULT is a session's own (parent_service.c, s_child_results) ---- */

static int s_want_join_acks;
static bool pred_child_join_acks(void)
{
    return child_join_acks_total() >= s_want_join_acks;
}

/* One retry pause at its longest, a failed IDENTIFY, and room to spare. */
#define RETRY_SLACK_MS ((uint32_t)(CONFIG_POGO_PING_INTERVAL_MS << 4) + 2500u)

/* Did the parent TAKE the JOIN_RESULT it just ACKed? On the wire an ACK for
 * a result it took and an ACK for one it only acknowledged look the same,
 * and the board below counts either as the end of its session. What differs
 * is what the parent does next: a session whose result it did not take is
 * given up after the join timeout and started again. True when no further
 * session has started by then. */
static bool parent_took_it(int sessions_so_far)
{
    sleep_ms(CONFIG_POGO_JOIN_TIMEOUT_MS + 4u * CONFIG_POGO_PING_INTERVAL_MS + 400u);
    return child_identify_sessions() == sessions_so_far;
}

/* What the parent's rule rests on, shown on the REAL child_service (this
 * module's own UPSTREAM side, driven by the scripted Base): it repeats only
 * its latest JOIN_RESULT, with the same seq; a new session's IDENTIFY_REQ
 * ends that for good; the next result has the next number. */
static void phase_10_what_the_real_child_does(void)
{
    printf("\n-- 10: the real child_service: one repeated JOIN_RESULT, cancelled by a new session, numbered on --\n");
    int done = child_sessions_done();
    pogo_frame_t f;

    CHECK(pb_provision_opt(&s_pb, &GEN_I, false), "UPSTREAM generation I is delivered; its JOIN_RESULT is NOT ACKed");
    const uint8_t first = s_pb_join_seq;
    CHECK(pb_take_frame(&s_pb, POGO_MSG_JOIN_RESULT, false, 0, &f, 6u * CONFIG_POGO_ACK_TIMEOUT_MS) && f.seq == first,
          "the child sends that JOIN_RESULT again, with the same seq");
    /* A new session, right now -- further repeats are still due. */
    CHECK(pb_request(&s_pb, POGO_MSG_IDENTIFY_REQ, NULL, 0, POGO_MSG_IDENTIFY_RESP, &f, 800),
          "a new session's IDENTIFY_REQ is answered");
    const uint32_t identified = s_pb_taken_arrival;
    sleep_ms((CONFIG_POGO_MAX_RETRY + 3) * CONFIG_POGO_ACK_TIMEOUT_MS);
    CHECK(pb_join_results_after(&s_pb, identified) == 0,
          "after that answer the child sends no JOIN_RESULT of the earlier session any more");
    CHECK(pb_provision(&s_pb, &GEN_I), "the session is run to its end");
    CHECK(s_pb_join_seq == (uint8_t)(first + 1) || (first == 255 && s_pb_join_seq == 1),
          "its JOIN_RESULT has the next number");
    CHECK(wait_child_sessions_done(done + 1, 3000) && child_has_generation(&GEN_I),
          "(the board below followed to generation I meanwhile)");
    sleep_ms(1000); /* its last session for I has ended before the next phase counts */
}

/* The review's case, and past it: the same old result in the join wait of
 * attempt after attempt, with sessions in between that fail before they
 * hand out a Dataset. */
static void phase_11_old_result_over_several_attempts(void)
{
    printf("\n-- 11: the same old JOIN_RESULT in every attempt's join wait; then the real one --\n");
    int done = child_sessions_done();
    int acks = child_join_acks_total();
    int sets = child_set_datasets();

    s_child.join_mode = CHILD_JOIN_REPLAY_OLD; /* no result of its own, only the last completed session's */
    CHECK(pb_provision(&s_pb, &GEN_J), "UPSTREAM generation J is delivered and joined");
    s_want_set_datasets = sets + 1;
    CHECK(wait_until(pred_child_set_datasets, 3000), "attempt 1 reaches SET_DATASET");
    s_want_set_datasets = sets + 2;
    CHECK(wait_until(pred_child_set_datasets, CONFIG_POGO_JOIN_TIMEOUT_MS + RETRY_SLACK_MS),
          "attempt 1 is not completed by the old result: it times out and attempt 2 starts");
    s_want_set_datasets = sets + 3;
    CHECK(wait_until(pred_child_set_datasets, CONFIG_POGO_JOIN_TIMEOUT_MS + RETRY_SLACK_MS),
          "attempt 2 is not completed by it either (what is known of it outlived the failed attempt): attempt 3");
    CHECK(child_join_acks_total() >= acks + 2 && child_sessions_done() == done,
          "each time it was ACKed and nothing more");

    /* Attempts that fail at their first step in between. */
    s_child.mute_provisioning = true;
    s_want_identify_sessions = child_identify_sessions() + 2;
    CHECK(wait_until(pred_child_identify_sessions, CONFIG_POGO_JOIN_TIMEOUT_MS + 2u * RETRY_SLACK_MS),
          "attempt 3 times out too, and the next sessions get no answer to IDENTIFY_REQ at all");
    int sets_before = child_set_datasets();
    int acks_before = child_join_acks_total();
    s_child.mute_provisioning = false;
    s_want_set_datasets = sets_before + 1;
    CHECK(wait_until(pred_child_set_datasets, RETRY_SLACK_MS), "the board answers again: an attempt reaches SET_DATASET");
    s_want_join_acks = acks_before + 1;
    CHECK(wait_until(pred_child_join_acks, 1000) && child_sessions_done() == done,
          "after sessions that failed before their Dataset, the old result is still only ACKed");

    s_child.join_mode = CHILD_JOIN_NORMAL;
    CHECK(wait_child_sessions_done(done + 1, CONFIG_POGO_JOIN_TIMEOUT_MS + RETRY_SLACK_MS),
          "with a JOIN_RESULT of its own the next attempt completes");
    CHECK(child_has_generation(&GEN_J), "with generation J");
    sleep_ms(300);
    CHECK(child_sessions_done() == done + 1, "exactly once");
}

/* A session is dropped for a new generation before its JOIN_RESULT was ever
 * seen; that result turns up during the next session. */
static void phase_12_result_of_a_dropped_session(void)
{
    printf("\n-- 12: the JOIN_RESULT of a session that was dropped for a new generation arrives late --\n");
    int done = child_sessions_done();
    int sets = child_set_datasets();

    s_child.join_mode = CHILD_JOIN_NEVER; /* still joining generation K's network */
    CHECK(pb_provision(&s_pb, &GEN_K), "UPSTREAM generation K is delivered and joined");
    s_want_set_datasets = sets + 1;
    CHECK(wait_until(pred_child_set_datasets, 3000), "DOWNSTREAM got as far as SET_DATASET for generation K");
    const uint8_t late = child_take_join_seq(); /* its join for K ends about now; the parent has never seen this */

    /* The next generation's session stalls at its first step, so that where
     * the late frame arrives is not left to chance. */
    s_child.mute_provisioning = true;
    int sessions = child_identify_sessions();
    int acks = child_join_acks_total();
    CHECK(pb_provision(&s_pb, &GEN_L), "UPSTREAM generation L is delivered and joined");
    s_want_identify_sessions = sessions + 1;
    CHECK(wait_until(pred_child_identify_sessions, 1500), "the session for K is dropped, one for L starts");
    peer_send_join_result(&s_child, late); /* generation K's result, now */
    sleep_ms(120);
    CHECK(child_join_acks_total() == acks, "it is not ACKed (no session processed it) ...");

    /* ... and when it comes again in L's join wait, it is not L's. */
    s_child.replay_seq = late;
    s_child.join_mode = CHILD_JOIN_REPLAY_SEQ;
    sets = child_set_datasets();
    s_child.mute_provisioning = false;
    s_want_set_datasets = sets + 1;
    CHECK(wait_until(pred_child_set_datasets, RETRY_SLACK_MS), "a session for L reaches SET_DATASET");
    s_want_join_acks = acks + 1;
    CHECK(wait_until(pred_child_join_acks, 1000), "... and when it comes again in L's join wait it is only ACKed");
    s_child.join_mode = CHILD_JOIN_NORMAL;
    s_want_set_datasets = sets + 2;
    CHECK(wait_until(pred_child_set_datasets, CONFIG_POGO_JOIN_TIMEOUT_MS + RETRY_SLACK_MS) &&
              child_done_with(GEN_K.collector_last) == 0,
          "that session is not completed by it: it times out and a new one starts");
    CHECK(wait_child_sessions_done(done + 1, 3000), "which completes on its own result");
    CHECK(child_has_generation(&GEN_L) && child_prev_done_join_seq() == (uint8_t)(late + 1),
          "with generation L, under the number after the late one");
}

/* The child's numbering wraps from 255 to 1. */
static void phase_13_numbering_wraps(void)
{
    printf("\n-- 13: the child's JOIN_RESULT numbering wraps (255 -> 1) --\n");
    int done = child_sessions_done();
    int acks = child_join_acks_total();

    child_set_join_counter(254);
    CHECK(pb_provision(&s_pb, &GEN_M), "UPSTREAM generation M is delivered and joined");
    CHECK(wait_child_sessions_done(done + 1, 3000) && child_prev_done_join_seq() == 255 && child_has_generation(&GEN_M),
          "DOWNSTREAM completes generation M under seq 255");
    acks = child_join_acks_total();
    s_child.join_mode = CHILD_JOIN_REPLAY_OLD;
    int sessions = child_identify_sessions();
    CHECK(pb_provision(&s_pb, &GEN_N), "UPSTREAM generation N is delivered and joined");
    s_want_join_acks = acks + 1;
    CHECK(wait_until(pred_child_join_acks, 3000), "seq 255 again in N's join wait: ACKed");
    s_child.join_mode = CHILD_JOIN_NORMAL;
    s_want_identify_sessions = sessions + 2;
    CHECK(wait_until(pred_child_identify_sessions, CONFIG_POGO_JOIN_TIMEOUT_MS + RETRY_SLACK_MS),
          "and not taken for N's result: the session times out and a new one starts");
    CHECK(wait_child_sessions_done(done + 2, 3000) && child_prev_done_join_seq() == 1 && child_has_generation(&GEN_N),
          "which completes under seq 1, the number after 255");
}

/* The ACK of SET_DATASET is lost and the child's JOIN_RESULT is here before
 * the parent has that ACK: it is this session's own, not an earlier one's. */
static void phase_14_result_before_the_dataset_ack(void)
{
    printf("\n-- 14: SET_DATASET's ACK is lost; the session's own JOIN_RESULT arrives before the repeated ACK --\n");
    int done = child_sessions_done();
    int sessions = child_identify_sessions();

    s_child.resend_unacked = true;
    s_child.withhold_dataset_ack = true;
    CHECK(pb_provision(&s_pb, &GEN_O), "UPSTREAM generation O is delivered and joined");
    CHECK(wait_child_sessions_done(done + 1, CONFIG_POGO_JOIN_TIMEOUT_MS / 2),
          "the session completes on that result when the child repeats it -- well inside the join timeout");
    CHECK(child_has_generation(&GEN_O), "with generation O");
    CHECK(parent_took_it(sessions + 1),
          "and the parent took it: one session, no second one after the join timeout");
    s_child.resend_unacked = false;
}

/* What is known of a child's results belongs to the handshake with it. */
static void phase_15_a_restarted_child(void)
{
    printf("\n-- 15: the board below restarts and numbers from 1 again --\n");
    int done = child_sessions_done();

    child_set_join_counter(0);
    CHECK(pb_provision(&s_pb, &GEN_P), "UPSTREAM generation P is delivered and joined");
    CHECK(wait_child_sessions_done(done + 1, 3000) && child_prev_done_join_seq() == 1,
          "DOWNSTREAM completes under seq 1: that is now the newest result this parent knows");

    /* (a) pulled and docked again */
    int mark = evt_mark();
    host_gpio_test_edge(DOWN_DETECT_IN, 1);
    CHECK(wait_for_event(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_DETACHED, 1500), "DOWNSTREAM DETACHED");
    child_set_join_counter(0);
    int sessions_docked = child_identify_sessions();
    host_gpio_test_edge(DOWN_DETECT_IN, 0);
    CHECK(wait_for_event(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_PEER_READY, 1500), "DOWNSTREAM PEER_READY again");
    CHECK(wait_child_sessions_done(done + 2, CONFIG_POGO_JOIN_TIMEOUT_MS / 2) && child_prev_done_join_seq() == 1,
          "docked again: its first JOIN_RESULT is seq 1 once more, and is ACKed");
    CHECK(child_identify_sessions() == sessions_docked + 1 && parent_took_it(sessions_docked + 1),
          "and taken: what was known of the board that was pulled is not held against the one that is docked now");

    /* (b) the link dies and comes back (LIVENESS_LOST, then a new handshake) */
    mark = evt_mark();
    int before_error = child_sessions_done();
    s_child.alive = false;
    CHECK(wait_for_event(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_ERROR, 4000), "DOWNSTREAM stops answering: port ERROR");
    child_set_join_counter(0);
    int sessions_error = child_identify_sessions();
    s_child.alive = true;
    CHECK(wait_for_event(mark, POGO_PORT_DOWNSTREAM, POGO_PORT_EVENT_PEER_READY, 4000), "it answers again: PEER_READY");
    CHECK(wait_child_sessions_done(before_error + 1, CONFIG_POGO_JOIN_TIMEOUT_MS / 2) && child_prev_done_join_seq() == 1,
          "after the port error too, its seq 1 is ACKed");
    CHECK(child_has_generation(&GEN_P) && parent_took_it(sessions_error + 1),
          "and taken, with generation P: one session");

    /* (c) a restart the port did not show (pogo_port answers a docked peer's
     * new HELLO_REQ without an event). Its first result has the number this
     * parent still knows: that costs ONE join timeout, and the retry
     * completes. */
    done = child_sessions_done();
    int sessions = child_identify_sessions();
    int acks = child_join_acks_total();
    child_set_join_counter(0);
    uint32_t t0 = now_ms();
    CHECK(pb_provision(&s_pb, &GEN_Q), "UPSTREAM generation Q is delivered and joined");
    /* The board below counts the ACK of its seq 1 as the end of its session
     * (so does child_service); the parent did not take that result, and
     * provisions it once more. */
    CHECK(wait_child_sessions_done(done + 2, CONFIG_POGO_JOIN_TIMEOUT_MS + RETRY_SLACK_MS) &&
              child_has_generation(&GEN_Q) && child_prev_done_join_seq() == 2,
          "a restart without a port event: completed with generation Q under seq 2");
    printf("info: that took %u ms and %d sessions (join timeout %d ms)\n", (unsigned)(now_ms() - t0),
           child_identify_sessions() - sessions, CONFIG_POGO_JOIN_TIMEOUT_MS);
    CHECK(child_identify_sessions() == sessions + 2 && child_join_acks_total() == acks + 2,
          "in two sessions: seq 1 was taken for the old result once, never more often");
}

/* Outside any session. */
static void phase_16_results_outside_a_session(void)
{
    printf("\n-- 16: JOIN_RESULTs while no session is running --\n");
    int done = child_sessions_done();
    int sessions = child_identify_sessions();
    int acks = child_join_acks_total();
    const uint8_t own = child_prev_done_join_seq();
    const uint8_t never = (uint8_t)(own + 60);

    peer_send_join_result(&s_child, own);
    s_want_join_acks = acks + 1;
    CHECK(wait_until(pred_child_join_acks, 800), "the completed session's JOIN_RESULT again: ACKed again");
    peer_send_join_result(&s_child, never);
    sleep_ms(300);
    CHECK(child_join_acks_total() == acks + 1, "a JOIN_RESULT no session ever took: not ACKed");
    CHECK(child_identify_sessions() == sessions && child_sessions_done() == done, "neither starts a session");

    /* ... but it is remembered: the next session does not take it for its own. */
    s_child.replay_seq = never;
    s_child.join_mode = CHILD_JOIN_REPLAY_SEQ;
    CHECK(pb_provision(&s_pb, &GEN_R), "UPSTREAM generation R is delivered and joined");
    s_want_join_acks = acks + 2;
    CHECK(wait_until(pred_child_join_acks, 3000), "the same one in the next session's join wait: ACKed");
    s_child.join_mode = CHILD_JOIN_NORMAL;
    s_want_identify_sessions = sessions + 2;
    CHECK(wait_until(pred_child_identify_sessions, CONFIG_POGO_JOIN_TIMEOUT_MS + RETRY_SLACK_MS),
          "and not taken for that session's result");
    CHECK(wait_child_sessions_done(done + 1, 3000) && child_has_generation(&GEN_R),
          "the next attempt completes with generation R");

    /* The same generation from here on: nothing more happens. */
    sessions = child_identify_sessions();
    int joins = tm_join_calls();
    sleep_ms(1500);
    CHECK(child_identify_sessions() == sessions && tm_join_calls() == joins && child_sessions_done() == done + 1,
          "and then nothing: no further session, no further join, while the generation stays R");
}

static int run_box(void)
{
    printf("== PART 1: one module in a box ==\n-- boot --\n");
    CHECK(boot_module(), "module boot (the order module/main/app_main.c uses)");
    peer_start(&s_pb);
    peer_start(&s_child);

    phase_1_first_connection();
    phase_2_same_generation_is_left_alone();
    phase_3_upstream_redock_new_generation();
    phase_4_upstream_restart_without_detach();
    phase_5_change_during_provisioning();
    phase_6_stale_join_result();
    phase_7_retry_then_recover();
    phase_8_role_only_change();
    phase_9_detach_during_reprovisioning();
    phase_10_what_the_real_child_does();
    phase_11_old_result_over_several_attempts();
    phase_12_result_of_a_dropped_session();
    phase_13_numbering_wraps();
    phase_14_result_before_the_dataset_ack();
    phase_15_a_restarted_child();
    phase_16_results_outside_a_session();
    return g_failures;
}

/* ====================================================================
 * PART 2: a chain of real modules, one process each.
 * ==================================================================== */

/* --- node side: this process IS one module; its two UARTs and DETECT_IN
 * lines are bridged to its neighbours' sockets. --- */

typedef struct {
    wire_t wire;
    int uart;
    int detect_in;
} bridge_t;

static void *bridge_tx_thread(void *arg)
{
    bridge_t *b = (bridge_t *)arg;
    while (1) {
        uint8_t buf[256];
        size_t n = 0;
        if (!host_uart_test_take_tx_byte(b->uart, &buf[0], 20)) {
            continue;
        }
        n = 1;
        while (n < sizeof(buf) && host_uart_test_take_tx_byte(b->uart, &buf[n], 0)) {
            n++;
        }
        wire_send_uart(&b->wire, buf, n);
    }
    return NULL;
}

static void *bridge_rx_thread(void *arg)
{
    bridge_t *b = (bridge_t *)arg;
    while (1) {
        uint8_t type;
        if (!fd_read_all(b->wire.fd, &type, 1)) {
            /* The neighbour is gone: its DETECT line floats High. */
            host_gpio_test_edge(b->detect_in, 1);
            return NULL;
        }
        if (type == 'D') {
            uint8_t level;
            if (fd_read_all(b->wire.fd, &level, 1)) {
                host_gpio_test_edge(b->detect_in, level);
            }
        } else {
            uint8_t len2[2];
            uint8_t buf[512];
            if (!fd_read_all(b->wire.fd, len2, 2)) {
                return NULL;
            }
            size_t n = (size_t)len2[0] | ((size_t)len2[1] << 8);
            if (n > sizeof(buf) || !fd_read_all(b->wire.fd, buf, n)) {
                return NULL;
            }
            host_uart_test_push_rx(b->uart, buf, n);
        }
    }
    return NULL;
}

static void bridge_start(bridge_t *b, int fd, int uart, int detect_in)
{
    b->wire.fd = fd;
    b->wire.uart = uart;
    b->uart = uart;
    b->detect_in = detect_in;
    pthread_mutex_init(&b->wire.wmutex, NULL);
    pthread_t t;
    pthread_create(&t, NULL, bridge_tx_thread, b);
    pthread_create(&t, NULL, bridge_rx_thread, b);
    /* This board is powered and its port enabled: its DETECT_OUT is active. */
    wire_send_detect(&b->wire, 0);
}

static int run_chain_node(int up_fd, int down_fd, int status_fd)
{
    static bridge_t up, down;
    s_status_fd = status_fd;
    if (!boot_module()) {
        status_line("B 0\n");
        return 1;
    }
    bridge_start(&up, up_fd, UP_UART, UP_DETECT_IN);
    if (down_fd >= 0) {
        bridge_start(&down, down_fd, DOWN_UART, DOWN_DETECT_IN);
    }
    status_line("B 1\n");
    while (1) {
        sleep(1);
    }
    return 0;
}

/* --- orchestrator side --- */

#define CHAIN_NODES 3

typedef struct {
    pid_t pid;
    int status_fd;
    pthread_t reader;
    pthread_mutex_t mutex;
    bool booted;
    int joins;
    uint8_t last_join_dataset0;
    uint8_t last_join_len;
    int attached_to[256];           /* ATTACHED "/t" reports, by collector */
    uint8_t last_attached_depth;
    uint8_t last_attached_parent;   /* last byte of parent_uuid */
    uint8_t last_attached_dest;
    int samples_to[256];
    int up_events;                  /* every non-frame UPSTREAM port event */
    int up_detached;
    int stops;
} chain_node_t;

static chain_node_t s_nodes[CHAIN_NODES];
static const uint8_t NODE_UUID_LAST[CHAIN_NODES] = {0xA1, 0xA2, 0xA3};

static void *chain_status_reader(void *arg)
{
    chain_node_t *n = (chain_node_t *)arg;
    FILE *f = fdopen(n->status_fd, "r");
    char line[128];
    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        unsigned a = 0, b = 0, c = 0, d = 0, e = 0;
        pthread_mutex_lock(&n->mutex);
        switch (line[0]) {
        case 'B':
            n->booted = (line[2] == '1');
            break;
        case 'J':
            if (sscanf(line + 1, "%u %u %u", &a, &b, &c) == 3) {
                n->joins++;
                n->last_join_len = (uint8_t)b;
                n->last_join_dataset0 = (uint8_t)c;
            }
            break;
        case 'T':
            if (sscanf(line + 1, "%u %u %u %u %u", &a, &b, &c, &d, &e) == 5 && a == TOPOLOGY_REPORT_ATTACHED) {
                n->attached_to[d & 0xFF]++;
                n->last_attached_depth = (uint8_t)b;
                n->last_attached_parent = (uint8_t)c;
                n->last_attached_dest = (uint8_t)d;
            }
            break;
        case 'S':
            if (sscanf(line + 1, "%u", &a) == 1) {
                n->samples_to[a & 0xFF]++;
            }
            break;
        case 'E':
            if (sscanf(line + 1, "%u %u", &a, &b) == 2 && a == POGO_PORT_UPSTREAM) {
                n->up_events++;
                if (b == POGO_PORT_EVENT_DETACHED) {
                    n->up_detached++;
                }
            }
            break;
        case 'X':
            n->stops++;
            break;
        default:
            break;
        }
        pthread_mutex_unlock(&n->mutex);
    }
    return NULL;
}

static chain_node_t chain_snapshot(int i)
{
    pthread_mutex_lock(&s_nodes[i].mutex);
    chain_node_t v = s_nodes[i];
    pthread_mutex_unlock(&s_nodes[i].mutex);
    return v;
}

/* Node i holds generation g: joined its Dataset, registered at its collector
 * with the right depth and parent, and its samples go there. */
static bool chain_node_has(int i, const upstream_gen_t *g, int samples_floor)
{
    chain_node_t n = chain_snapshot(i);
    uint8_t want_parent = (i == 0) ? PB_UUID[POGO_UUID_LEN - 1] : NODE_UUID_LAST[i - 1];
    return n.last_join_dataset0 == g->dataset0 && n.last_join_len == g->dataset_len &&
           n.attached_to[g->collector_last] > 0 && n.last_attached_dest == g->collector_last &&
           n.last_attached_depth == (uint8_t)(g->depth + i) && n.last_attached_parent == want_parent &&
           n.samples_to[g->collector_last] > samples_floor;
}

static const upstream_gen_t *s_chain_want;
static int s_chain_floor[CHAIN_NODES];
static bool pred_chain_converged(void)
{
    for (int i = 0; i < CHAIN_NODES; i++) {
        if (!chain_node_has(i, s_chain_want, s_chain_floor[i])) {
            return false;
        }
    }
    return true;
}

static bool chain_wait_converged(const upstream_gen_t *g, uint32_t timeout_ms)
{
    s_chain_want = g;
    for (int i = 0; i < CHAIN_NODES; i++) {
        s_chain_floor[i] = chain_snapshot(i).samples_to[g->collector_last];
    }
    return wait_until(pred_chain_converged, timeout_ms);
}

static bool pred_chain_booted(void)
{
    for (int i = 0; i < CHAIN_NODES; i++) {
        if (!chain_snapshot(i).booted) {
            return false;
        }
    }
    return true;
}

static void chain_report(const char *label, const upstream_gen_t *g)
{
    static const char *names[CHAIN_NODES] = {"M1", "M2", "M3"};
    char msg[160];
    for (int i = 0; i < CHAIN_NODES; i++) {
        chain_node_t n = chain_snapshot(i);
        printf("info: %s %s: joins=%d dataset0=0x%02x attached(dest=0x%02x depth=%u parent=..%02x) "
               "samples_to_new=%d up_events=%d\n",
               label, names[i], n.joins, n.last_join_dataset0, n.last_attached_dest, n.last_attached_depth,
               n.last_attached_parent, n.samples_to[g->collector_last], n.up_events);
        snprintf(msg, sizeof(msg), "%s: %s holds the new collector, Dataset, depth %u and parent, and sends there",
                 label, names[i], (unsigned)(g->depth + i));
        CHECK(chain_node_has(i, g, 0), msg);
    }
}

static int run_chain(const char *self)
{
    printf("\n== PART 2: scripted Power Base -> M1 -> M2 -> M3 (three real modules, one process each) ==\n");
    static fake_peer_t pb = {.peer_type = POGO_PEER_TYPE_POWER_BASE};
    int link[CHAIN_NODES][2]; /* link[i]: [0] = upstream end (held by node i-1 or the Base), [1] = node i's UP */
    int status[CHAIN_NODES][2];
    for (int i = 0; i < CHAIN_NODES; i++) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, link[i]) != 0 || pipe(status[i]) != 0) {
            perror("socketpair/pipe");
            return 1;
        }
    }
    for (int i = 0; i < CHAIN_NODES; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            char up[16], down[16], st[16], mac[16];
            snprintf(up, sizeof(up), "%d", link[i][1]);
            snprintf(down, sizeof(down), "%d", (i + 1 < CHAIN_NODES) ? link[i + 1][0] : -1);
            snprintf(st, sizeof(st), "%d", status[i][1]);
            snprintf(mac, sizeof(mac), "0x%02x", NODE_UUID_LAST[i]);
            setenv("HOST_ESP_MAC_LAST_BYTE", mac, 1);
            for (int k = 0; k < CHAIN_NODES; k++) {
                /* keep only this node's own ends */
                if (k != i) {
                    close(link[k][1]);
                }
                if (k != i + 1) {
                    close(link[k][0]);
                }
                close(status[k][0]);
                if (k != i) {
                    close(status[k][1]);
                }
            }
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO); /* the module's own logs */
                dup2(devnull, STDERR_FILENO);
            }
            execl(self, self, "--chain-node", up, down, st, (char *)NULL);
            _exit(127);
        }
        s_nodes[i].pid = pid;
        s_nodes[i].status_fd = status[i][0];
        pthread_mutex_init(&s_nodes[i].mutex, NULL);
    }
    for (int i = 0; i < CHAIN_NODES; i++) {
        close(link[i][1]);
        if (i > 0) {
            close(link[i][0]);
        }
        close(status[i][1]);
        pthread_create(&s_nodes[i].reader, NULL, chain_status_reader, &s_nodes[i]);
    }

    pb.wire.fd = link[0][0];
    pb.alive = true;
    peer_start(&pb);
    CHECK(wait_until(pred_chain_booted, 5000), "three module processes booted");

    static const upstream_gen_t C1 = {.depth = 1, .parent_uuid = PB_UUID, .parent_is_power_base = true,
                                      .collector_last = 0x11, .dataset0 = 0xD1, .dataset_len = 12};
    static const upstream_gen_t C2 = {.depth = 1, .parent_uuid = PB_UUID, .parent_is_power_base = true,
                                      .collector_last = 0x12, .dataset0 = 0xD2, .dataset_len = 13};
    static const upstream_gen_t C3 = {.depth = 1, .parent_uuid = PB_UUID, .parent_is_power_base = true,
                                      .collector_last = 0x13, .dataset0 = 0xD3, .dataset_len = 14};
    static const upstream_gen_t C4 = {.depth = 1, .parent_uuid = PB_UUID, .parent_is_power_base = true,
                                      .collector_last = 0x14, .dataset0 = 0xD4, .dataset_len = 15};

    printf("\n-- chain 1: the Base docks M1 and provisions it; the chain builds itself --\n");
    wire_send_detect(&pb.wire, 0);
    sleep_ms(600);
    CHECK(pb_provision(&pb, &C1), "the Base provisions M1 (generation 1)");
    CHECK(chain_wait_converged(&C1, 10000), "M1, M2 and M3 are all joined, registered and sending");
    chain_report("generation 1", &C1);

    chain_node_t before[CHAIN_NODES];
    for (int i = 0; i < CHAIN_NODES; i++) {
        before[i] = chain_snapshot(i);
    }

    printf("\n-- chain 2: Base removed and enrolled again (M1 sees a detach); M2 and M3 are never touched --\n");
    wire_send_detect(&pb.wire, 1);
    sleep_ms(500);
    CHECK(chain_snapshot(0).up_detached == before[0].up_detached + 1 && chain_snapshot(0).stops == before[0].stops + 1,
          "M1: UPSTREAM DETACHED, its Thread stopped");
    wire_send_detect(&pb.wire, 0);
    sleep_ms(600);
    CHECK(pb_provision(&pb, &C2), "the Base provisions M1 again (generation 2)");
    CHECK(chain_wait_converged(&C2, 10000), "M1, M2 and M3 all move to generation 2");
    chain_report("generation 2", &C2);
    CHECK(chain_snapshot(1).up_events == before[1].up_events && chain_snapshot(2).up_events == before[2].up_events,
          "M2 and M3 saw no UPSTREAM port event at all (no detach, no new handshake)");
    CHECK(chain_snapshot(1).stops == before[1].stops && chain_snapshot(2).stops == before[2].stops,
          "M2 and M3 never stopped Thread; they re-joined with the Dataset they were handed");

    printf("\n-- chain 3: the same again --\n");
    for (int i = 0; i < CHAIN_NODES; i++) {
        before[i] = chain_snapshot(i);
    }
    wire_send_detect(&pb.wire, 1);
    sleep_ms(500);
    wire_send_detect(&pb.wire, 0);
    sleep_ms(600);
    CHECK(pb_provision(&pb, &C3), "the Base provisions M1 again (generation 3)");
    CHECK(chain_wait_converged(&C3, 10000), "M1, M2 and M3 all move to generation 3");
    chain_report("generation 3", &C3);
    CHECK(chain_snapshot(1).up_events == before[1].up_events && chain_snapshot(2).up_events == before[2].up_events,
          "M2 and M3 saw no UPSTREAM port event");

    printf("\n-- chain 4: the Base starts over without any detach (new IDENTIFY_REQ only) --\n");
    for (int i = 0; i < CHAIN_NODES; i++) {
        before[i] = chain_snapshot(i);
    }
    CHECK(pb_provision(&pb, &C4), "the Base provisions M1 again (generation 4)");
    CHECK(chain_wait_converged(&C4, 10000), "M1, M2 and M3 all move to generation 4");
    chain_report("generation 4", &C4);
    CHECK(chain_snapshot(0).up_events == before[0].up_events && chain_snapshot(1).up_events == before[1].up_events &&
              chain_snapshot(2).up_events == before[2].up_events,
          "no module saw an UPSTREAM port event");

    printf("\n-- chain 5: nothing changes for 3 s --\n");
    for (int i = 0; i < CHAIN_NODES; i++) {
        before[i] = chain_snapshot(i);
    }
    sleep_ms(3000);
    bool quiet = true;
    for (int i = 0; i < CHAIN_NODES; i++) {
        chain_node_t n = chain_snapshot(i);
        quiet = quiet && n.joins == before[i].joins &&
                n.attached_to[C4.collector_last] == before[i].attached_to[C4.collector_last] &&
                n.samples_to[C4.collector_last] > before[i].samples_to[C4.collector_last];
    }
    CHECK(quiet, "no module re-joined or re-registered; all three keep sending to the generation 4 collector");

    for (int i = 0; i < CHAIN_NODES; i++) {
        kill(s_nodes[i].pid, SIGKILL);
        waitpid(s_nodes[i].pid, NULL, 0);
    }
    return g_failures;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    if (argc == 5 && strcmp(argv[1], "--chain-node") == 0) {
        return run_chain_node(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    }
    bool only_box = (argc == 2 && strcmp(argv[1], "--box") == 0);
    bool only_chain = (argc == 2 && strcmp(argv[1], "--chain") == 0);

    if (!only_chain) {
        run_box();
    }
    if (!only_box) {
        run_chain(argv[0]);
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf(g_failures == 0 ? "ALL CHECKS PASSED\n" : "%d CHECK(S) FAILED\n", g_failures);
    fflush(stdout);
    /* The module's service tasks have no teardown; leave them running and
     * exit the process directly. */
    _exit(g_failures == 0 ? 0 : 1);
}
