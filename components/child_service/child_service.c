#include "child_service/child_service.h"

#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "pogo_proto/pogo_frame.h"
#include "pogo_proto/pogo_protocol.h"
#include "collector_endpoint/collector_endpoint.h"
#include "sensor_service/sensor_service.h"
#include "thread_manager/thread_manager.h"

static const char *TAG = "child_service";

#define CHILD_FW_VERSION 1
#define CHILD_NVS_NAMESPACE "child_dyn"
#define POLL_PERIOD_MIN_MS 1u
#define POLL_PERIOD_MAX_MS 3600000u /* 1 hour: a sane upper sanity bound, not from the spec's RT table */

/*
 * Strict forward sequence (each message type is only accepted in the exact
 * preceding state; anything else is POGO_NACK_BAD_STATE -- see
 * handle_request_uuid/handle_assign_uuid/handle_assign_slot/
 * handle_assign_parent_info/handle_sensor_hello/handle_poll_config/
 * handle_set_dataset below):
 *
 *   READY -> IDENTIFIED -> UUID_PROPOSED -> UUID_ASSIGNED -> SLOT_ASSIGNED
 *         -> PARENT_INFO_SET -> SENSOR_DONE -> POLL_SET -> COLLECTOR_SET
 *         -> JOINING -> JOINED
 *
 * COLLECTOR_SET (POGO_MSG_SET_COLLECTOR) is this follow-up's addition,
 * inserted between POLL_SET and SET_DATASET/JOINING (see this file's header
 * comment and docs/power-base.md): SET_DATASET now requires
 * CS_STATE_COLLECTOR_SET rather than CS_STATE_POLL_SET, so the Thread join
 * -- and this module's own periodic sensor measurement/reporting once
 * joined -- never starts before this module knows where its data should go.
 *
 * UUID_PROPOSED (POGO_MSG_REQUEST_UUID/POGO_MSG_PROPOSE_UUID) is a code
 * review follow-up addition (section 1: "센서 모듈 UUID를 영구 식별자로
 * 변경") inserted between IDENTIFY and ASSIGN_UUID: this module's own
 * module_uuid is now a PERMANENT identifier derived from its own efuse
 * EUI-64 (see handle_request_uuid()), proposed by the child and only ever
 * CONFIRMED (never invented) by the parent via the existing ASSIGN_UUID
 * message -- see handle_assign_uuid()'s mismatch check. Purely additive to
 * the wire protocol (two new message types, ASSIGN_UUID's own wire format
 * unchanged), so upper/lower are unaffected.
 *
 * PARENT_INFO_SET (POGO_MSG_ASSIGN_PARENT_INFO) is a power-base follow-up
 * addition ("부모 UUID 전달", section 7) inserted between ASSIGN_SLOT and
 * SENSOR_HELLO -- purely additive to the wire protocol (new message type,
 * existing ones unchanged), so upper/lower (which never send/expect it) are
 * unaffected; the SAME module firmware now expects it from ANY parent
 * (power base or another sensor module), both sides of which are always
 * this one binary.
 *
 * A retransmission of the exact same (type, seq) is still answered from the
 * dedup cache regardless of the current state (see dedup_resend_if_repeat) --
 * the strict ordering above only applies to a message that is NOT a cached
 * duplicate.
 */
typedef enum {
    CS_STATE_WAIT_PEER = 0,
    CS_STATE_READY,           /* PEER_READY: waiting for IDENTIFY_REQ */
    CS_STATE_IDENTIFIED,      /* IDENTIFY_RESP sent */
    CS_STATE_UUID_PROPOSED,   /* PROPOSE_UUID sent (this module's own permanent, efuse-derived uuid) */
    CS_STATE_UUID_ASSIGNED,   /* parent confirmed the SAME uuid via ASSIGN_UUID */
    CS_STATE_SLOT_ASSIGNED,   /* has slot_id + topology_depth */
    CS_STATE_PARENT_INFO_SET, /* has parent_uuid + parent_is_power_base */
    CS_STATE_SENSOR_DONE,     /* SENSOR_INFO_ACK sent */
    CS_STATE_POLL_SET,        /* POLL_CONFIG acked */
    CS_STATE_COLLECTOR_SET,   /* SET_COLLECTOR stored (durably + in components/collector_endpoint) */
    CS_STATE_JOINING,
    CS_STATE_JOINED,
    CS_STATE_ERROR,
} cs_state_t;

typedef struct {
    pogo_port_event_t event;
    pogo_port_event_data_t data;
} child_evt_t;

#define DEDUP_COUNT 8
typedef struct {
    bool valid;
    uint8_t seq;
    uint8_t resp_type;
    uint8_t resp_payload[8];
    size_t resp_len;
} dedup_entry_t;

static QueueHandle_t s_evt_queue;
/*
 * Owned copy, not a pointer into the caller's storage: child_service_init()
 * is called with the address of a LOCAL module_config_t in
 * module/main/app_main.c's app_main(), which goes out of scope once
 * app_main() returns. Storing that pointer directly (the original bug) left
 * every later s_cfg-> read dereferencing a stale stack address -- reusable
 * by whatever function's frame happened to land there next -- which is why
 * IDENTIFY_RESP's sensor_bitmap and handle_sensor_hello()'s measurement
 * timeout read back as garbage instead of the configured values. Copying
 * the whole struct by value here means its lifetime is this component's own
 * static storage duration, independent of the caller's.
 */
static module_config_t s_cfg;

static cs_state_t s_state = CS_STATE_WAIT_PEER;
static dedup_entry_t s_dedup[DEDUP_COUNT];
static uint8_t s_uuid[POGO_UUID_LEN];
static uint8_t s_slot_id, s_topology_depth;
static uint8_t s_parent_uuid[POGO_UUID_LEN];
static bool s_parent_is_power_base;
static uint32_t s_polling_period_ms;
static uint8_t s_dataset[POGO_FRAME_MAX_PAYLOAD];
static uint16_t s_dataset_len;
static TickType_t s_join_deadline;
static volatile module_role_t s_role_for_join = MODULE_ROLE_UNASSIGNED;
static module_role_t s_last_joined_role = MODULE_ROLE_UNASSIGNED;

/*
 * JOIN_RESULT reliability (section 7 of the follow-up spec): a JOIN_RESULT
 * is no longer a fire-and-forget seq=0 message. It gets a real,
 * monotonically-increasing seq (s_join_result_seq_counter, never reset by a
 * session restart, so a stray old-seq JOIN_RESULT/ACK from a previous
 * generation can never be mistaken for the current one's), and is retried
 * with that SAME seq (so the parent's dedup-by-seq recognizes a retransmit)
 * until a matching POGO_MSG_JOIN_RESULT_ACK arrives or CONFIG_POGO_MAX_RETRY
 * is exhausted. clear_provisioning_temporaries() (ATTACHED/DETACHED, or a
 * fresh provisioning generation via begin_new_provisioning_session()) is the
 * ONLY thing that cancels a still-pending retry -- never a plain timeout by
 * itself continuing forever, and never anything else clearing it.
 */
static uint8_t s_join_result_seq_counter;
static uint8_t s_join_result_seq;
static bool s_join_result_pending;
static bool s_join_result_success;
static uint8_t s_join_result_role;
static int s_join_result_retries;
static TickType_t s_join_result_deadline;

/* See child_service_get_upstream_generation(). Written by this component's
 * own task only, read from others (a single aligned 32-bit load). */
static volatile uint32_t s_upstream_generation;

static void begin_new_upstream_generation(void)
{
    uint32_t next = s_upstream_generation + 1;
    if (next == 0) {
        next = 1; /* 0 stays "no session was ever joined" */
    }
    s_upstream_generation = next;
}

static int dedup_index_for(uint8_t msg_type)
{
    switch (msg_type) {
    case POGO_MSG_ASSIGN_UUID:
        return 0;
    case POGO_MSG_ASSIGN_SLOT:
        return 1;
    case POGO_MSG_SENSOR_HELLO:
        return 2;
    case POGO_MSG_POLL_CONFIG:
        return 3;
    case POGO_MSG_SET_DATASET:
        return 4;
    case POGO_MSG_ASSIGN_PARENT_INFO:
        return 5;
    case POGO_MSG_REQUEST_UUID:
        return 6;
    case POGO_MSG_SET_COLLECTOR:
        return 7;
    default:
        return -1;
    }
}

static bool dedup_resend_if_repeat(int idx, uint8_t seq)
{
    if (idx < 0 || !s_dedup[idx].valid || s_dedup[idx].seq != seq) {
        return false;
    }
    ESP_LOGI(TAG, "duplicate seq=%u for a write message -- resending cached response, not reapplying", seq);
    pogo_port_send(POGO_PORT_UPSTREAM, s_dedup[idx].resp_type, seq, s_dedup[idx].resp_payload, s_dedup[idx].resp_len);
    return true;
}

static void dedup_remember(int idx, uint8_t seq, uint8_t resp_type, const uint8_t *resp_payload, size_t resp_len)
{
    if (idx < 0 || resp_len > sizeof(s_dedup[0].resp_payload)) {
        return;
    }
    s_dedup[idx].valid = true;
    s_dedup[idx].seq = seq;
    s_dedup[idx].resp_type = resp_type;
    s_dedup[idx].resp_len = resp_len;
    memcpy(s_dedup[idx].resp_payload, resp_payload, resp_len);
}

/* Clears every per-session temporary -- dedup cache, uuid, slot/depth,
 * polling_period, staged (not-yet-joined) dataset, join deadline -- without
 * touching s_state or role_manager's upstream_ready. Shared by
 * reset_session() (ATTACHED/DETACHED: goes to WAIT_PEER) and
 * begin_new_provisioning_session() (PEER_READY / a restarting IDENTIFY_REQ:
 * goes straight to READY or IDENTIFIED, see below). */
static void clear_provisioning_temporaries(void)
{
    memset(s_dedup, 0, sizeof(s_dedup));
    memset(s_uuid, 0, sizeof(s_uuid));
    s_slot_id = s_topology_depth = 0;
    memset(s_parent_uuid, 0, sizeof(s_parent_uuid));
    s_parent_is_power_base = false;
    s_polling_period_ms = 0;
    s_dataset_len = 0;
    s_join_deadline = 0;
    /* Cancels any in-flight JOIN_RESULT retry -- see s_join_result_pending's
     * doc comment for why this (ATTACHED/DETACHED or a fresh provisioning
     * generation) is the only thing allowed to do so. */
    s_join_result_pending = false;
    s_join_result_retries = 0;
    /* A new provisioning session (or a detach) immediately invalidates
     * whatever collector endpoint the OLD session may have set -- see
     * components/collector_endpoint's header comment and docs/power-base.md.
     * A brand new SET_COLLECTOR must arrive before this module (or anything
     * that later docks below it) sends data again. */
    collector_endpoint_clear();
}

static void reset_session(void)
{
    s_state = CS_STATE_WAIT_PEER;
    clear_provisioning_temporaries();
}

/*
 * Starts a brand new provisioning session, discarding anything left over
 * from a previous one -- successful (JOINED), half-finished, or CS_STATE_ERROR
 * -- instead of getting stuck in ERROR forever or resuming with stale
 * uuid/slot/dataset. upstream_ready drops back to false until the new
 * session reaches JOINED again (a stale "ready" must not outlive the session
 * that produced it).
 *
 * Two call sites need this, both from the unified module role manager fix:
 *   - POGO_PORT_EVENT_PEER_READY: covers a dev-mode handshake retry that
 *     reaches PEER_READY again after POGO_PORT_EVENT_ERROR left s_state in
 *     CS_STATE_ERROR (the old code only left CS_STATE_WAIT_PEER on this
 *     transition, so a retry after ERROR never became READY and every
 *     later request in the sequence NACKed with BAD_STATE).
 *   - A new IDENTIFY_REQ arriving while already past CS_STATE_IDENTIFIED --
 *     see handle_identify_req(): the ROOT side rebooting alone does not
 *     make THIS side's own PEER_READY re-fire (pogo_port on this side never
 *     saw a detach), so the new IDENTIFY_REQ is the only signal available
 *     that the parent discarded its old session and started over.
 */
static void begin_new_provisioning_session(cs_state_t next_state)
{
    clear_provisioning_temporaries();
    role_manager_set_upstream_ready(false);
    s_state = next_state;
}

/*
 * Persists the received uuid/slot/depth/polling_period/dataset. Every NVS
 * call's result is checked -- a failure anywhere (open, any set, or commit)
 * is reported to the caller so handle_set_dataset() can refuse to ACK and
 * refuse to start a Thread join on a dataset that was never durably saved.
 * Diagnostic/forward-compatibility record only -- see docs/state-machine.md:
 * child_service deliberately does not auto-rejoin from this on boot without
 * a fresh handshake (section 14: a changed upstream parent must not be
 * trusted silently).
 */
static esp_err_t save_dynamic_record(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CHILD_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save_dynamic_record: nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    do {
        err = nvs_set_blob(h, "uuid", s_uuid, POGO_UUID_LEN);
        if (err != ESP_OK) break;
        err = nvs_set_u8(h, "slot", s_slot_id);
        if (err != ESP_OK) break;
        err = nvs_set_u8(h, "depth", s_topology_depth);
        if (err != ESP_OK) break;
        err = nvs_set_u32(h, "poll_ms", s_polling_period_ms);
        if (err != ESP_OK) break;
        err = nvs_set_u16(h, "ds_len", s_dataset_len);
        if (err != ESP_OK) break;
        err = nvs_set_blob(h, "ds_tlv", s_dataset, s_dataset_len);
        if (err != ESP_OK) break;
        err = nvs_commit(h);
    } while (0);

    nvs_close(h);
    if (err != ESP_OK) {
        /* Dataset bytes are never logged, only the fact that storage failed. */
        ESP_LOGE(TAG, "save_dynamic_record failed: %s -- dataset NOT applied, NACKing SET_DATASET", esp_err_to_name(err));
    }
    return err;
}

/*
 * Persists the collector endpoint into the same runtime "nvs" NVS namespace
 * save_dynamic_record() uses (never the fixed "cfg" partition -- section 5
 * of this follow-up: "동적 정보는 cfg 파티션에 넣지 말고 기존 Dataset과 같은
 * 런타임 NVS 영역에 저장하라"). Diagnostic/forward-compatibility only, same
 * policy as save_dynamic_record() -- never read back at boot (see
 * docs/state-machine.md's "No auto-rejoin from stored dynamic NVS on boot"
 * simplification, which applies here identically: a changed upstream parent
 * must not be trusted silently, so this module always waits for a fresh,
 * live SET_COLLECTOR before ever using components/collector_endpoint's
 * in-RAM copy for a real send). Every NVS call's result is checked, same as
 * save_dynamic_record().
 */
static esp_err_t save_collector_dynamic_record(const pogo_collector_endpoint_t *ep)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CHILD_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save_collector_dynamic_record: nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    do {
        err = nvs_set_blob(h, "coll_uuid", ep->power_base_uuid, POGO_UUID_LEN);
        if (err != ESP_OK) break;
        err = nvs_set_blob(h, "coll_addr", ep->addr, POGO_COLLECTOR_ADDR_LEN);
        if (err != ESP_OK) break;
        err = nvs_set_u16(h, "coll_port", ep->port);
        if (err != ESP_OK) break;
        err = nvs_set_u32(h, "coll_gen", ep->generation);
        if (err != ESP_OK) break;
        err = nvs_commit(h);
    } while (0);

    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save_collector_dynamic_record failed: %s -- SET_COLLECTOR NOT applied, NACKing",
                 esp_err_to_name(err));
    }
    return err;
}

static void send_join_result_frame(bool success, otDeviceRole role, uint8_t seq)
{
    pogo_join_result_t result = {.success = success ? 1 : 0, .role = (uint8_t)role};
    uint8_t payload[2];
    size_t len = pogo_pack_join_result(&result, payload, sizeof(payload));
    pogo_port_send(POGO_PORT_UPSTREAM, POGO_MSG_JOIN_RESULT, seq, payload, len);
}

/*
 * Starts (or restarts, for the very first send of a new result) the
 * JOIN_RESULT retry cycle -- see s_join_result_pending's doc comment.
 * Assigns a fresh seq for THIS result (never reused across a role's whole
 * session), distinct from every other message-type's own seq space, so a
 * parent's dedup-by-seq is scoped correctly.
 */
static void begin_join_result_retry(bool success, otDeviceRole role)
{
    s_join_result_seq_counter++;
    if (s_join_result_seq_counter == 0) {
        s_join_result_seq_counter = 1; /* skip 0: keep 0 reserved for the legacy upper/lower always-0 convention */
    }
    s_join_result_seq = s_join_result_seq_counter;
    s_join_result_success = success;
    s_join_result_role = (uint8_t)role;
    s_join_result_pending = true;
    s_join_result_retries = 0;
    send_join_result_frame(success, role, s_join_result_seq);
    s_join_result_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CONFIG_POGO_ACK_TIMEOUT_MS);
}

/* Called every child_service_task loop iteration: retransmits the pending
 * JOIN_RESULT (same seq) after CONFIG_POGO_ACK_TIMEOUT_MS with no ACK, up to
 * CONFIG_POGO_MAX_RETRY times, then gives up (logged, not retried forever --
 * the same bounded-retry policy as every other request/response exchange in
 * this codebase). */
static void poll_join_result_retry(void)
{
    if (!s_join_result_pending) {
        return;
    }
    if ((int32_t)(xTaskGetTickCount() - s_join_result_deadline) < 0) {
        return;
    }
    if (s_join_result_retries >= CONFIG_POGO_MAX_RETRY) {
        ESP_LOGW(TAG, "JOIN_RESULT_ACK never received after %d retries (seq=%u) -- giving up; the parent may "
                      "still re-provision from scratch if it never saw JOIN_RESULT at all",
                 CONFIG_POGO_MAX_RETRY, s_join_result_seq);
        s_join_result_pending = false;
        return;
    }
    s_join_result_retries++;
    ESP_LOGW(TAG, "JOIN_RESULT_ACK not received -- retry %d/%d (seq=%u)", s_join_result_retries, CONFIG_POGO_MAX_RETRY,
             s_join_result_seq);
    send_join_result_frame(s_join_result_success, (otDeviceRole)s_join_result_role, s_join_result_seq);
    s_join_result_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CONFIG_POGO_ACK_TIMEOUT_MS);
}

static void handle_identify_req(const pogo_frame_t *frame)
{
    /* esp_efuse_mac_get_default() always writes a full 8-byte EUI-64 on a
     * chip with IEEE 802.15.4 support (ESP32-H2) -- it takes a bare
     * uint8_t* with no length parameter, so it trusts the caller for the
     * buffer size. Read into a proper 8-byte scratch buffer, not directly
     * into resp.mac (6 bytes): the old code overran resp.mac by 2 bytes,
     * corrupting whatever followed it on the stack (resp.fw_version /
     * resp.sensor_bitmap), which is why sensor_bitmap in the IDENTIFY log
     * changed on every boot instead of reading the configured 0x01.
     */
    uint8_t mac8[8];
    esp_err_t mac_err = esp_efuse_mac_get_default(mac8);
    if (mac_err != ESP_OK) {
        ESP_LOGE(TAG, "IDENTIFY_REQ: esp_efuse_mac_get_default failed: %s -- not sending an uninitialized identity",
                 esp_err_to_name(mac_err));
        return; /* No IDENTIFY_RESP at all; the parent's own retry/timeout re-requests. */
    }

    pogo_identify_resp_t resp = {0};
    /* Wire field is only 6 bytes and is informational only (not used to key
     * any state) -- explicitly take the first 6 of the 8 EUI-64 bytes
     * rather than widening the protocol field. */
    memcpy(resp.mac, mac8, sizeof(resp.mac));
    resp.fw_version = CHILD_FW_VERSION;
    resp.sensor_bitmap = (uint8_t)(s_cfg.sensor_type_bitmap & 0xFF);
    uint8_t payload[8];
    size_t len = pogo_pack_identify_resp(&resp, payload, sizeof(payload));
    pogo_port_send(POGO_PORT_UPSTREAM, POGO_MSG_IDENTIFY_RESP, frame->seq, payload, len);

    if (s_state == CS_STATE_READY) {
        s_state = CS_STATE_IDENTIFIED;
    } else if (s_state >= CS_STATE_UUID_PROPOSED) {
        /* Past IDENTIFIED already (UUID_PROPOSED..JOINED, or ERROR from an
         * earlier failed attempt): a new IDENTIFY_REQ here means the parent
         * itself restarted provisioning from scratch (e.g. ROOT rebooted
         * while this LEAF's own PEER_READY never re-fired -- see
         * begin_new_provisioning_session()'s doc comment). Discard the old
         * session's uuid/slot/dataset instead of leaving them stale, or
         * every following ASSIGN_UUID/etc. from the new session NACKs with
         * BAD_STATE. */
        ESP_LOGI(TAG, "IDENTIFY_REQ while state=%d -- parent restarted, beginning a new provisioning session",
                 (int)s_state);
        begin_new_provisioning_session(CS_STATE_IDENTIFIED);
    }
    /* else: s_state == CS_STATE_IDENTIFIED already -- no side effect to
     * redo, and intentionally still answered without regressing state or
     * NACKing. */
}

/*
 * Code review follow-up, section 1: this module's module_uuid is a PERMANENT
 * identifier owned by this ESP32-H2, derived from its own efuse EUI-64 --
 * never randomly generated, never assigned by whatever it happens to be
 * docked into. Recomputed fresh each provisioning session (cheap,
 * deterministic -- no reason to cache it across a POGO detach/reconnect
 * cycle) rather than relying on anything persisted, so a changed parent,
 * a power base reboot, or this module's own reboot can never make it read
 * back different bytes. Returns false (no PROPOSE_UUID sent at all, mirrors
 * handle_identify_req()'s efuse-failure policy) if the efuse read fails, or
 * -- defensively, should never actually happen on real hardware -- if it
 * reads back as all-zero: an all-zero UUID is never treated as a valid
 * permanent identifier (section 1: "실패 시 all-zero UUID를 정상 식별자로
 * 사용하지 않는다").
 */
static bool compute_permanent_uuid(uint8_t out_uuid[POGO_UUID_LEN])
{
    uint8_t mac8[8];
    esp_err_t err = esp_efuse_mac_get_default(mac8);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "compute_permanent_uuid: esp_efuse_mac_get_default failed: %s", esp_err_to_name(err));
        return false;
    }
    static const uint8_t all_zero[POGO_UUID_LEN] = {0};
    if (memcmp(mac8, all_zero, POGO_UUID_LEN) == 0) {
        ESP_LOGE(TAG, "compute_permanent_uuid: efuse EUI-64 read back as all-zero -- refusing to use it as an "
                      "identifier");
        return false;
    }
    /* POGO_UUID_LEN == 8 == the full EUI-64 width -- every byte is used
     * (unlike IDENTIFY_RESP's informational mac[6], which deliberately
     * truncates). */
    memcpy(out_uuid, mac8, POGO_UUID_LEN);
    return true;
}

static void handle_request_uuid(const pogo_frame_t *frame)
{
    int idx = dedup_index_for(frame->type);
    if (dedup_resend_if_repeat(idx, frame->seq)) {
        return;
    }
    if (s_state != CS_STATE_IDENTIFIED) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_REQUEST_UUID, .reason = POGO_NACK_BAD_STATE};
        uint8_t resp_payload[2];
        size_t resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
        pogo_port_send(POGO_PORT_UPSTREAM, POGO_MSG_NACK, frame->seq, resp_payload, resp_len);
        dedup_remember(idx, frame->seq, POGO_MSG_NACK, resp_payload, resp_len);
        return;
    }

    if (!compute_permanent_uuid(s_uuid)) {
        /* No response at all -- mirrors handle_identify_req()'s policy of
         * never sending an uninitialized/fabricated identity. The parent's
         * own retry/timeout drives recovery (or abandons the session). */
        return;
    }

    pogo_propose_uuid_t msg;
    memcpy(msg.uuid, s_uuid, POGO_UUID_LEN);
    uint8_t resp_payload[POGO_UUID_LEN];
    size_t resp_len = pogo_pack_propose_uuid(&msg, resp_payload, sizeof(resp_payload));
    pogo_port_send(POGO_PORT_UPSTREAM, POGO_MSG_PROPOSE_UUID, frame->seq, resp_payload, resp_len);
    dedup_remember(idx, frame->seq, POGO_MSG_PROPOSE_UUID, resp_payload, resp_len);
    s_state = CS_STATE_UUID_PROPOSED;
}

static void handle_assign_uuid(const pogo_frame_t *frame)
{
    int idx = dedup_index_for(frame->type);
    if (dedup_resend_if_repeat(idx, frame->seq)) {
        return;
    }
    pogo_assign_uuid_t msg;
    uint8_t resp_type, resp_payload[2];
    size_t resp_len;
    if (s_state != CS_STATE_UUID_PROPOSED) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_ASSIGN_UUID, .reason = POGO_NACK_BAD_STATE};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else if (!pogo_unpack_assign_uuid(frame->payload, frame->len, &msg)) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_ASSIGN_UUID, .reason = POGO_NACK_BAD_STATE};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else if (memcmp(msg.uuid, s_uuid, POGO_UUID_LEN) != 0) {
        /* The parent tried to confirm something other than exactly what
         * this module proposed -- a parent must never invent its own uuid
         * (section 1 of the code review follow-up). Rejected, NOT silently
         * accepted -- s_state does not advance. */
        ESP_LOGE(TAG, "ASSIGN_UUID does not match this module's own proposed permanent uuid -- rejecting "
                      "(a parent must never assign a different uuid)");
        pogo_nack_t nack = {.acked_type = POGO_MSG_ASSIGN_UUID, .reason = POGO_NACK_UUID_MISMATCH};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else {
        /* From here on, sensor_service tags this module's own periodic
         * samples with the real (permanent) provisioning UUID instead of
         * deriving its own MAC-based placeholder independently -- see
         * sensor_service_get_module_uuid() (both now compute the exact same
         * bytes, but this keeps a single, explicit "confirmed" moment). */
        sensor_service_set_provisioned_uuid(s_uuid);
        pogo_ack_t ack = {.acked_type = POGO_MSG_ASSIGN_UUID};
        resp_type = POGO_MSG_ACK;
        resp_len = pogo_pack_ack(&ack, resp_payload, sizeof(resp_payload));
        s_state = CS_STATE_UUID_ASSIGNED;
    }
    pogo_port_send(POGO_PORT_UPSTREAM, resp_type, frame->seq, resp_payload, resp_len);
    dedup_remember(idx, frame->seq, resp_type, resp_payload, resp_len);
}

static void handle_assign_slot(const pogo_frame_t *frame)
{
    int idx = dedup_index_for(frame->type);
    if (dedup_resend_if_repeat(idx, frame->seq)) {
        return;
    }
    pogo_assign_slot_t msg;
    uint8_t resp_type, resp_payload[2];
    size_t resp_len;
    if (s_state != CS_STATE_UUID_ASSIGNED) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_ASSIGN_SLOT, .reason = POGO_NACK_BAD_STATE};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else if (pogo_unpack_assign_slot(frame->payload, frame->len, &msg)) {
        s_slot_id = msg.slot_id;
        s_topology_depth = msg.topology_depth;
        pogo_ack_t ack = {.acked_type = POGO_MSG_ASSIGN_SLOT};
        resp_type = POGO_MSG_ACK;
        resp_len = pogo_pack_ack(&ack, resp_payload, sizeof(resp_payload));
        s_state = CS_STATE_SLOT_ASSIGNED;
    } else {
        pogo_nack_t nack = {.acked_type = POGO_MSG_ASSIGN_SLOT, .reason = POGO_NACK_BAD_STATE};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    }
    pogo_port_send(POGO_PORT_UPSTREAM, resp_type, frame->seq, resp_payload, resp_len);
    dedup_remember(idx, frame->seq, resp_type, resp_payload, resp_len);
}

static void handle_assign_parent_info(const pogo_frame_t *frame)
{
    int idx = dedup_index_for(frame->type);
    if (dedup_resend_if_repeat(idx, frame->seq)) {
        return;
    }
    pogo_assign_parent_info_t msg;
    uint8_t resp_type, resp_payload[2];
    size_t resp_len;
    if (s_state != CS_STATE_SLOT_ASSIGNED) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_ASSIGN_PARENT_INFO, .reason = POGO_NACK_BAD_STATE};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else if (pogo_unpack_assign_parent_info(frame->payload, frame->len, &msg)) {
        memcpy(s_parent_uuid, msg.parent_uuid, POGO_UUID_LEN);
        s_parent_is_power_base = msg.parent_is_power_base != 0;
        pogo_ack_t ack = {.acked_type = POGO_MSG_ASSIGN_PARENT_INFO};
        resp_type = POGO_MSG_ACK;
        resp_len = pogo_pack_ack(&ack, resp_payload, sizeof(resp_payload));
        s_state = CS_STATE_PARENT_INFO_SET;
    } else {
        pogo_nack_t nack = {.acked_type = POGO_MSG_ASSIGN_PARENT_INFO, .reason = POGO_NACK_BAD_STATE};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    }
    pogo_port_send(POGO_PORT_UPSTREAM, resp_type, frame->seq, resp_payload, resp_len);
    dedup_remember(idx, frame->seq, resp_type, resp_payload, resp_len);
}

static void handle_sensor_hello(const pogo_frame_t *frame)
{
    int idx = dedup_index_for(frame->type);
    if (dedup_resend_if_repeat(idx, frame->seq)) {
        return;
    }
    if (s_state != CS_STATE_PARENT_INFO_SET) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_SENSOR_HELLO, .reason = POGO_NACK_BAD_STATE};
        uint8_t resp_payload[2];
        size_t resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
        pogo_port_send(POGO_PORT_UPSTREAM, POGO_MSG_NACK, frame->seq, resp_payload, resp_len);
        dedup_remember(idx, frame->seq, POGO_MSG_NACK, resp_payload, resp_len);
        return;
    }

    uint8_t resp_type, resp_payload[8];
    size_t resp_len;

    /* Fixed, permanently-paired sensor: exactly one retry budget for THIS
     * driver, never a fallback to a different one (section 11). sensor_service
     * owns the driver and the mutex that keeps this measurement from
     * racing its own periodic sampling task (section 3). */
    sensor_measurement_t meas = {0};
    esp_err_t err = sensor_service_measure_once(&meas, pdMS_TO_TICKS(s_cfg.measurement_timeout_ms));

    if (err == ESP_OK) {
        pogo_sensor_info_ack_t ack = {
            .sensor_type_bitmap = meas.sensor_type_bitmap,
            .response_time_ms = meas.response_time_ms,
            .fw_version = CHILD_FW_VERSION,
            .sensor_status = (uint8_t)sensor_service_get_status(),
        };
        resp_type = POGO_MSG_SENSOR_INFO_ACK;
        resp_len = pogo_pack_sensor_info_ack(&ack, resp_payload, sizeof(resp_payload));
        s_state = CS_STATE_SENSOR_DONE;
        ESP_LOGI(TAG, "sensor measurement OK: bitmap=0x%04x response_time=%ums", meas.sensor_type_bitmap,
                 (unsigned)meas.response_time_ms);
    } else {
        ESP_LOGW(TAG, "SENSOR_ERROR: %s", esp_err_to_name(err));
        pogo_nack_t nack = {.acked_type = POGO_MSG_SENSOR_HELLO, .reason = POGO_NACK_SENSOR_ERROR};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    }
    pogo_port_send(POGO_PORT_UPSTREAM, resp_type, frame->seq, resp_payload, resp_len);
    dedup_remember(idx, frame->seq, resp_type, resp_payload, resp_len);
}

static void handle_poll_config(const pogo_frame_t *frame)
{
    int idx = dedup_index_for(frame->type);
    if (dedup_resend_if_repeat(idx, frame->seq)) {
        return;
    }
    uint8_t resp_type, resp_payload[2];
    size_t resp_len;

    pogo_poll_config_t cfg_msg;
    if (s_state != CS_STATE_SENSOR_DONE) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_POLL_CONFIG, .reason = POGO_NACK_BAD_STATE};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else if (!pogo_unpack_poll_config(frame->payload, frame->len, &cfg_msg)) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_POLL_CONFIG, .reason = POGO_NACK_BAD_STATE};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else if (cfg_msg.polling_period_ms < POLL_PERIOD_MIN_MS || cfg_msg.polling_period_ms > POLL_PERIOD_MAX_MS) {
        ESP_LOGW(TAG, "POLL_CONFIG_ERROR: polling_period_ms=%u out of range", (unsigned)cfg_msg.polling_period_ms);
        pogo_nack_t nack = {.acked_type = POGO_MSG_POLL_CONFIG, .reason = POGO_NACK_POLL_CONFIG_RANGE};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else {
        /* effective_polling_period_ms = max(parent's dynamic formula result,
         * this module's own driver minimum) -- section 10 of the follow-up
         * spec. s_polling_period_ms feeds ONLY sensor_service's periodic
         * measurement task (set here) -- it is NEVER passed to
         * thread_manager_join_with_dataset() any more (real-hardware
         * finding, this follow-up: reusing a multi-second sensor period as
         * this LEAF's own Thread SED data-poll period made its CoAP client
         * give up waiting for a response before its own next data poll
         * could even retrieve it -- see thread_manager.h's own comment on
         * thread_manager_join_with_dataset()'s thread_data_poll_period_ms
         * parameter, and components/coap_response_timing). The ACK itself
         * is unchanged (no new wire field) to keep POGO_MSG_POLL_CONFIG's
         * existing wire format; a parent-visible "actual period used" echo
         * is future work if ever
         * needed. */
        uint32_t effective_ms = sensor_service_compute_effective_period_ms(cfg_msg.polling_period_ms);
        if (effective_ms != cfg_msg.polling_period_ms) {
            ESP_LOGI(TAG, "POLL_CONFIG raised %u -> %u by this module's own driver minimum",
                     (unsigned)cfg_msg.polling_period_ms, (unsigned)effective_ms);
        }
        s_polling_period_ms = effective_ms;
        sensor_service_set_polling_period_ms(effective_ms);
        pogo_ack_t ack = {.acked_type = POGO_MSG_POLL_CONFIG};
        resp_type = POGO_MSG_ACK;
        resp_len = pogo_pack_ack(&ack, resp_payload, sizeof(resp_payload));
        s_state = CS_STATE_POLL_SET;
        ESP_LOGI(TAG, "polling_period_ms=%u accepted", (unsigned)s_polling_period_ms);
    }
    pogo_port_send(POGO_PORT_UPSTREAM, resp_type, frame->seq, resp_payload, resp_len);
    dedup_remember(idx, frame->seq, resp_type, resp_payload, resp_len);
}

/*
 * SET_COLLECTOR (this follow-up, see child_service.h's header comment):
 * requires CS_STATE_POLL_SET (i.e. AFTER POLL_CONFIG, BEFORE SET_DATASET).
 * A structurally-malformed payload (wrong length) is POGO_NACK_BAD_STATE,
 * the same reason every other malformed-payload case in this file uses;
 * a well-formed but semantically-invalid endpoint (unspecified/multicast/
 * link-local address, port 0, all-zero uuid) is the more specific
 * POGO_NACK_INVALID_COLLECTOR. A storage failure is POGO_NACK_STORAGE_FAILED
 * and does NOT advance s_state or populate components/collector_endpoint --
 * mirrors handle_set_dataset()'s own storage-failure policy exactly.
 */
static void handle_set_collector(const pogo_frame_t *frame)
{
    int idx = dedup_index_for(frame->type);
    if (dedup_resend_if_repeat(idx, frame->seq)) {
        return;
    }
    uint8_t resp_type, resp_payload[2];
    size_t resp_len;

    pogo_collector_endpoint_t ep;
    if (s_state != CS_STATE_POLL_SET) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_SET_COLLECTOR, .reason = POGO_NACK_BAD_STATE};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else if (!pogo_unpack_set_collector(frame->payload, frame->len, &ep)) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_SET_COLLECTOR, .reason = POGO_NACK_BAD_STATE};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else if (!pogo_collector_endpoint_is_valid(&ep)) {
        ESP_LOGW(TAG, "SET_COLLECTOR rejected: invalid endpoint (unspecified/multicast/link-local address, "
                      "port 0, or all-zero power_base_uuid)");
        pogo_nack_t nack = {.acked_type = POGO_MSG_SET_COLLECTOR, .reason = POGO_NACK_INVALID_COLLECTOR};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else if (save_collector_dynamic_record(&ep) != ESP_OK) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_SET_COLLECTOR, .reason = POGO_NACK_STORAGE_FAILED};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else {
        /* IPv6 addresses/Network-topology-identifying UUIDs are logged only
         * as a short suffix, same convention as every other uuid log line in
         * this codebase. */
        esp_err_t set_err =
            collector_endpoint_set(ep.power_base_uuid, ep.addr, ep.port, ep.generation);
        if (set_err != ESP_OK) {
            /* Should not happen (already validated above), but never claim
             * success if the in-RAM cache somehow rejects it. */
            pogo_nack_t nack = {.acked_type = POGO_MSG_SET_COLLECTOR, .reason = POGO_NACK_INVALID_COLLECTOR};
            resp_type = POGO_MSG_NACK;
            resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
        } else {
            pogo_ack_t ack = {.acked_type = POGO_MSG_SET_COLLECTOR};
            resp_type = POGO_MSG_ACK;
            resp_len = pogo_pack_ack(&ack, resp_payload, sizeof(resp_payload));
            s_state = CS_STATE_COLLECTOR_SET;
            ESP_LOGI(TAG, "collector endpoint accepted: power_base_uuid=..%02x%02x port=%u",
                     ep.power_base_uuid[6], ep.power_base_uuid[7], (unsigned)ep.port);
        }
    }
    pogo_port_send(POGO_PORT_UPSTREAM, resp_type, frame->seq, resp_payload, resp_len);
    dedup_remember(idx, frame->seq, resp_type, resp_payload, resp_len);
}

static void handle_set_dataset(const pogo_frame_t *frame)
{
    int idx = dedup_index_for(frame->type);
    if (dedup_resend_if_repeat(idx, frame->seq)) {
        return;
    }
    uint8_t resp_type, resp_payload[2];
    size_t resp_len;

    if (s_state != CS_STATE_COLLECTOR_SET) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_SET_DATASET, .reason = POGO_NACK_BAD_STATE};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else if (!thread_manager_validate_dataset_tlvs(frame->payload, frame->len)) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_SET_DATASET, .reason = POGO_NACK_INVALID_DATASET};
        resp_type = POGO_MSG_NACK;
        resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
    } else {
        /* Stage into the real fields first so save_dynamic_record() writes
         * exactly what we're about to (maybe) join with, but only ACK / only
         * start a join if the write actually succeeded. */
        memcpy(s_dataset, frame->payload, frame->len);
        s_dataset_len = frame->len;
        if (save_dynamic_record() != ESP_OK) {
            s_dataset_len = 0; /* don't leave a half-applied dataset staged */
            pogo_nack_t nack = {.acked_type = POGO_MSG_SET_DATASET, .reason = POGO_NACK_STORAGE_FAILED};
            resp_type = POGO_MSG_NACK;
            resp_len = pogo_pack_nack(&nack, resp_payload, sizeof(resp_payload));
        } else {
            pogo_ack_t ack = {.acked_type = POGO_MSG_SET_DATASET};
            resp_type = POGO_MSG_ACK;
            resp_len = pogo_pack_ack(&ack, resp_payload, sizeof(resp_payload));
            s_state = CS_STATE_JOINING;
        }
    }
    pogo_port_send(POGO_PORT_UPSTREAM, resp_type, frame->seq, resp_payload, resp_len);
    dedup_remember(idx, frame->seq, resp_type, resp_payload, resp_len);

    if (s_state == CS_STATE_JOINING) {
        module_role_t role = role_manager_get_role();
        thread_manager_role_t tm_role =
            (role == MODULE_ROLE_INTERMEDIATE) ? THREAD_MANAGER_ROLE_INTERMEDIATE : THREAD_MANAGER_ROLE_LEAF;
        /* Remember the link mode actually applied, not the raw role: this
         * join can run before role_manager has settled (UPSTREAM resolved
         * while DOWNSTREAM is still mid-handshake leaves it UNASSIGNED), and
         * anything other than INTERMEDIATE joins in LEAF link mode. Storing
         * the raw role made the role later settling to LEAF look like a
         * change and re-joined Thread right after the first join -- see the
         * re-apply check in child_service_task(). */
        s_last_joined_role = (role == MODULE_ROLE_INTERMEDIATE) ? MODULE_ROLE_INTERMEDIATE : MODULE_ROLE_LEAF;
        /* Dataset TLVs are never logged: they contain the Network Key. */
        ESP_LOGI(TAG, "dataset stored (%u bytes) -- starting Thread join as %s", (unsigned)s_dataset_len,
                 role_manager_role_to_str(role));
        /* thread_data_poll_period_ms=0: always use thread_manager's own
         * Kconfig default (CONFIG_THREAD_MANAGER_LEAF_DATA_POLL_PERIOD_MS)
         * -- NEVER s_polling_period_ms (the sensor's own measurement
         * period, logged separately above/in handle_poll_config()). See
         * thread_manager.h's own comment on this parameter. */
        if (thread_manager_join_with_dataset(tm_role, s_dataset, s_dataset_len, 0) == ESP_OK) {
            s_join_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CONFIG_POGO_JOIN_TIMEOUT_MS);
        } else {
            ESP_LOGE(TAG, "failed to apply dataset / start Thread join");
            s_state = CS_STATE_ERROR;
        }
    }
}

static void handle_frame(const pogo_frame_t *frame)
{
    /* POGO_MSG_PING/PONG are answered inside pogo_port itself now (section 9
     * of the follow-up spec: single port-level transaction ownership) and
     * are never forwarded up as FRAME_RECEIVED -- this switch never actually
     * sees them, so there is no PING case here any more. */
    switch (frame->type) {
    case POGO_MSG_IDENTIFY_REQ:
        handle_identify_req(frame);
        break;
    case POGO_MSG_REQUEST_UUID:
        handle_request_uuid(frame);
        break;
    case POGO_MSG_ASSIGN_UUID:
        handle_assign_uuid(frame);
        break;
    case POGO_MSG_ASSIGN_SLOT:
        handle_assign_slot(frame);
        break;
    case POGO_MSG_ASSIGN_PARENT_INFO:
        handle_assign_parent_info(frame);
        break;
    case POGO_MSG_SENSOR_HELLO:
        handle_sensor_hello(frame);
        break;
    case POGO_MSG_POLL_CONFIG:
        handle_poll_config(frame);
        break;
    case POGO_MSG_SET_COLLECTOR:
        handle_set_collector(frame);
        break;
    case POGO_MSG_SET_DATASET:
        handle_set_dataset(frame);
        break;
    case POGO_MSG_JOIN_RESULT_ACK:
        if (s_join_result_pending && frame->seq == s_join_result_seq) {
            s_join_result_pending = false;
            ESP_LOGI(TAG, "JOIN_RESULT_ACK received (seq=%u) -- provisioning session fully confirmed", frame->seq);
        }
        /* else: stale/mismatched ACK (an old generation, or a duplicate of
         * one already cleared) -- ignored, not an error. */
        break;
    default:
        ESP_LOGW(TAG, "ignoring well-formed frame with unexpected type=0x%02x seq=%u", frame->type, frame->seq);
        break;
    }
}

static void child_service_task(void *arg)
{
    (void)arg;
    while (1) {
        if (s_state == CS_STATE_JOINING) {
            /* Judge this join by OpenThread's live role, not by a queued
             * role-change event: the queue is only drained here, so it also
             * holds changes from an earlier membership (an INTERMEDIATE's
             * ROUTER promotion, thread_manager_stop()'s DISABLED, a LEAF
             * re-attaching) and drops new ones once full. The join call
             * itself left Thread DETACHED, so an attached role seen now was
             * reached by this join. Draining only keeps the queue from
             * staying full. */
            otDeviceRole role;
            while (thread_manager_poll_role_change(&role)) {
            }
            role = thread_manager_get_role();
            if (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER) {
                ESP_LOGI(TAG, "Thread join succeeded, otDeviceRole=%u(%s)", role, thread_manager_role_to_str(role));
                begin_join_result_retry(true, role);
                s_state = CS_STATE_JOINED;
                /* The new generation exists before anything may act on
                 * "upstream ready": whoever sees ready already sees the new
                 * number. */
                begin_new_upstream_generation();
                role_manager_set_upstream_ready(true);
                sensor_service_set_thread_joined(true); /* unblocks sensor_service's wireless sample hand-off */
            } else if (s_join_deadline != 0 && (int32_t)(xTaskGetTickCount() - s_join_deadline) >= 0) {
                otDeviceRole cur = thread_manager_get_role();
                ESP_LOGW(TAG, "Thread join timed out, otDeviceRole=%u(%s)", cur, thread_manager_role_to_str(cur));
                begin_join_result_retry(false, cur);
                s_state = CS_STATE_ERROR;
                s_join_deadline = 0;
                sensor_service_set_thread_joined(false);
            }
        }
        poll_join_result_retry(); /* independent of s_state -- a pending retry outlives CS_STATE_JOINED/ERROR */

        if (s_state == CS_STATE_JOINED && s_role_for_join != s_last_joined_role &&
                   (s_role_for_join == MODULE_ROLE_LEAF || s_role_for_join == MODULE_ROLE_INTERMEDIATE)) {
            /* role_manager flipped between LEAF/INTERMEDIATE while already
             * joined (e.g. a module docked/undocked downstream) -- re-apply
             * link mode with the same dataset, no re-provisioning needed. */
            thread_manager_role_t tm_role =
                (s_role_for_join == MODULE_ROLE_INTERMEDIATE) ? THREAD_MANAGER_ROLE_INTERMEDIATE
                                                               : THREAD_MANAGER_ROLE_LEAF;
            ESP_LOGI(TAG, "role changed to %s while joined -- re-applying Thread link mode",
                     role_manager_role_to_str(s_role_for_join));
            /* thread_data_poll_period_ms=0: same Kconfig-default policy as
             * the initial join above -- never s_polling_period_ms. */
            thread_manager_join_with_dataset(tm_role, s_dataset, s_dataset_len, 0);
            s_last_joined_role = s_role_for_join;
        }

        child_evt_t evt;
        if (xQueueReceive(s_evt_queue, &evt, pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;
        }

        switch (evt.event) {
        case POGO_PORT_EVENT_ATTACHED:
            reset_session();
            break;
        case POGO_PORT_EVENT_DETACHED:
            /* child_service only invalidates the pairing session/dedup
             * state here -- it does not call thread_manager itself.
             * Stopping Thread on an UPSTREAM detach is module main's
             * responsibility (role_manager transitions to UNASSIGNED, whose
             * on_role_change handler calls thread_manager_stop(); see
             * module/main/module_wiring.c and docs/state-machine.md). */
            reset_session();
            role_manager_set_upstream_ready(false);
            sensor_service_set_thread_joined(false); /* stops sensor_service's wireless hand-off immediately */
            break;
        case POGO_PORT_EVENT_PEER_READY:
            /* Always starts a fresh provisioning session, regardless of the
             * previous state -- not just from CS_STATE_WAIT_PEER. A
             * dev-mode handshake retry (pogo_port.c auto-retries the
             * handshake cycle after POGO_PORT_EVENT_ERROR in dev compat
             * mode) reaches PEER_READY again with s_state left at
             * CS_STATE_ERROR by the earlier POGO_PORT_EVENT_ERROR below; the
             * old `== CS_STATE_WAIT_PEER` check silently ignored that
             * PEER_READY, so s_state stayed ERROR and every subsequent
             * IDENTIFY_REQ/ASSIGN_UUID kept NACKing BAD_STATE forever. */
            begin_new_provisioning_session(CS_STATE_READY);
            break;
        case POGO_PORT_EVENT_FRAME_RECEIVED:
            handle_frame(&evt.data.frame);
            break;
        case POGO_PORT_EVENT_ERROR:
            ESP_LOGW(TAG, "UPSTREAM port error=%d -> ERROR", (int)evt.data.error_code);
            s_state = CS_STATE_ERROR;
            sensor_service_set_thread_joined(false);
            /* upstream ERROR: the collector endpoint (if any) this session
             * received is no longer trustworthy either -- see
             * clear_provisioning_temporaries()'s own call for the DETACHED/
             * new-session cases; POGO_PORT_EVENT_ERROR does not route through
             * reset_session()/begin_new_provisioning_session() so this is
             * called explicitly here too. */
            collector_endpoint_clear();
            break;
        }
    }
}

esp_err_t child_service_init(const module_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg; /* owned copy -- see s_cfg's doc comment above */
    /* The sensor driver itself is owned by sensor_service, initialized once
     * by module/main/app_main.c BEFORE child_service_init() is called --
     * child_service only ever calls sensor_service_measure_once() (see
     * handle_sensor_hello()) and never touches sensor_provider_ops_t
     * directly any more (section 3 of the follow-up spec). */

    s_evt_queue = xQueueCreate(16, sizeof(child_evt_t));
    if (s_evt_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    reset_session();

    BaseType_t ok = xTaskCreate(child_service_task, "child_service", 4096, NULL, 5, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

void child_service_on_port_event(pogo_port_id_t port_id, pogo_port_event_t event, const pogo_port_event_data_t *data)
{
    if (port_id != POGO_PORT_UPSTREAM || s_evt_queue == NULL) {
        return;
    }
    child_evt_t evt = {.event = event, .data = data ? *data : (pogo_port_event_data_t){0}};
    xQueueSend(s_evt_queue, &evt, 0);
}

void child_service_on_role_change(module_role_t new_role, module_role_t old_role)
{
    (void)old_role;
    s_role_for_join = new_role;
}

uint8_t child_service_get_own_topology_depth(void)
{
    return s_topology_depth;
}

uint32_t child_service_get_upstream_generation(void)
{
    return s_upstream_generation;
}

void child_service_get_parent_uuid(uint8_t out_uuid[POGO_UUID_LEN])
{
    memcpy(out_uuid, s_parent_uuid, POGO_UUID_LEN);
}

bool child_service_get_parent_is_power_base(void)
{
    return s_parent_is_power_base;
}
