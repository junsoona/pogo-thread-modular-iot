/*
 * Host-compiled verification of components/child_service/child_service.c's
 * pairing state machine and IDENTIFY_RESP MAC handling, for the real-hardware
 * integration-test failure fixed by the "unified module role manager"
 * follow-up (see the child_service.c comments on
 * begin_new_provisioning_session() / handle_identify_req()):
 *
 *   1. handshake timeout -> CS_STATE_ERROR -> dev-mode handshake retry ->
 *      POGO_PORT_EVENT_PEER_READY -> IDENTIFY -> ASSIGN_UUID ACK. Before the
 *      fix, PEER_READY only advanced CS_STATE_WAIT_PEER -> CS_STATE_READY,
 *      so a retry after ERROR left the state stuck in ERROR and every
 *      following request NACKed POGO_NACK_BAD_STATE forever.
 *   2. ROOT-only reboot: this side's own PEER_READY never re-fires (its
 *      pogo_port instance never saw a detach), so a new IDENTIFY_REQ from
 *      the restarted parent is the only signal available; it must discard
 *      the old session's uuid/slot/dataset and restart from IDENTIFIED
 *      instead of NACKing the parent's fresh ASSIGN_UUID as BAD_STATE.
 *   3. esp_efuse_mac_get_default() writes a full 8-byte EUI-64 on a chip
 *      with IEEE 802.15.4 support (ESP32-H2); IDENTIFY_RESP's wire mac field
 *      is 6 bytes. The fix reads into a properly-sized 8-byte scratch buffer
 *      and copies only 6 bytes out, so sensor_bitmap (the byte right after
 *      the old 6-byte mac[] in pogo_identify_resp_t) must always equal the
 *      configured value, never something that depends on what the MAC read
 *      happened to overrun into.
 *   4. A genuine retransmission of the same (type, seq) ASSIGN_UUID must
 *      still be answered from the dedup cache (condition 5 of the fix) --
 *      the new "discard and restart" behavior is scoped to a NEW
 *      PEER_READY / a NEW IDENTIFY_REQ after the state has moved past
 *      IDENTIFIED again, not to a retransmit within the same session.
 *   5. module_config_t is a LOCAL in module/main/app_main.c's app_main(),
 *      which returns after wiring everything up. child_service_init() must
 *      own a stable copy of it (not the caller's pointer, which is only
 *      valid for the duration of that call) -- SENSOR_HELLO's later reads
 *      of sensor_type_bitmap/measurement_timeout_ms must still be correct
 *      long after app_main() has returned.
 *
 * This links against the REAL, unmodified wire-format code (pogo_pack_...
 * and pogo_unpack_... from components/pogo_proto, which has no ESP-IDF/
 * FreeRTOS dependency) -- only the surrounding cs_state_t state machine of
 * child_service.c is reimplemented here, as a minimal model that mirrors
 * begin_new_provisioning_session()/handle_identify_req()/handle_assign_uuid()
 * and the PEER_READY/ERROR event switch step for step. If those functions
 * change in child_service.c, update the model below to match -- same caveat
 * as tools/test_polling_calc.c and tools/test_peer_hello_handshake.c.
 *
 * Build & run (from the repo root):
 *   cc -Wall -Wextra -I components/pogo_proto/include \
 *      -o /tmp/test_child_provisioning_session tools/test_child_provisioning_session.c \
 *      components/pogo_proto/pogo_frame.c components/pogo_proto/pogo_crc16.c \
 *      components/pogo_proto/pogo_protocol.c \
 *   && /tmp/test_child_provisioning_session
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "pogo_proto/pogo_protocol.h"

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char *msg)
{
    g_checks++;
    if (!cond) {
        printf("FAIL: %s\n", msg);
        g_failures++;
    } else {
        printf("ok:   %s\n", msg);
    }
}

/* ---- minimal mirror of child_service.c's cs_state_t / session model ---- */

typedef enum {
    CS_STATE_WAIT_PEER = 0,
    CS_STATE_READY,
    CS_STATE_IDENTIFIED,
    CS_STATE_UUID_ASSIGNED,
    CS_STATE_SLOT_ASSIGNED,
    CS_STATE_SENSOR_DONE,
    CS_STATE_POLL_SET,
    CS_STATE_JOINING,
    CS_STATE_JOINED,
    CS_STATE_ERROR,
} cs_state_t;

typedef struct {
    cs_state_t state;
    uint8_t uuid[POGO_UUID_LEN];
    uint8_t slot_id, topology_depth;
    uint32_t polling_period_ms;
    uint16_t dataset_len;
    bool dedup_uuid_valid;
    uint8_t dedup_uuid_seq;
    uint8_t dedup_uuid_resp_type;
    uint8_t dedup_uuid_resp_payload[2];
    bool upstream_ready;
    int upstream_ready_false_count; /* how many times it was (re)dropped to false */
} child_t;

static void child_init(child_t *c)
{
    memset(c, 0, sizeof(*c));
    c->state = CS_STATE_WAIT_PEER;
}

/* Mirrors clear_provisioning_temporaries() in child_service.c. */
static void clear_provisioning_temporaries(child_t *c)
{
    memset(c->uuid, 0, sizeof(c->uuid));
    c->slot_id = c->topology_depth = 0;
    c->polling_period_ms = 0;
    c->dataset_len = 0;
    c->dedup_uuid_valid = false;
}

/* Mirrors begin_new_provisioning_session() in child_service.c. */
static void begin_new_provisioning_session(child_t *c, cs_state_t next_state)
{
    clear_provisioning_temporaries(c);
    c->upstream_ready = false;
    c->upstream_ready_false_count++;
    c->state = next_state;
}

/* Mirrors the PEER_READY arm of child_service_task()'s event switch. */
static void on_peer_ready(child_t *c)
{
    begin_new_provisioning_session(c, CS_STATE_READY);
}

/* Mirrors the ERROR arm. */
static void on_port_error(child_t *c)
{
    c->state = CS_STATE_ERROR;
}

/*
 * Mirrors the FIXED handle_identify_req(): reads the (simulated)
 * esp_efuse_mac_get_default() result into an 8-byte scratch buffer, copies
 * only the wire field's 6 bytes out, and applies the (possibly
 * session-restarting) state transition. Returns false if a simulated efuse
 * failure means no IDENTIFY_RESP should be sent at all.
 */
static bool handle_identify_req(child_t *c, const uint8_t mac_efuse8[8], bool efuse_ok, uint8_t configured_bitmap,
                                 uint8_t fw_version, uint8_t *out_payload, size_t *out_len)
{
    if (!efuse_ok) {
        return false; /* no IDENTIFY_RESP sent; caller must not check out_payload/out_len */
    }

    pogo_identify_resp_t resp = {0};
    memcpy(resp.mac, mac_efuse8, sizeof(resp.mac)); /* bounded 6-of-8-byte copy -- the fix */
    resp.fw_version = fw_version;
    resp.sensor_bitmap = configured_bitmap;
    *out_len = pogo_pack_identify_resp(&resp, out_payload, 8);

    if (c->state == CS_STATE_READY) {
        c->state = CS_STATE_IDENTIFIED;
    } else if (c->state >= CS_STATE_UUID_ASSIGNED) {
        begin_new_provisioning_session(c, CS_STATE_IDENTIFIED);
    }
    return true;
}

/* Mirrors handle_assign_uuid()'s dedup + strict-state-order logic. */
static void handle_assign_uuid(child_t *c, uint8_t seq, const uint8_t uuid[POGO_UUID_LEN], uint8_t *resp_type,
                                uint8_t *resp_payload, size_t *resp_len)
{
    if (c->dedup_uuid_valid && c->dedup_uuid_seq == seq) {
        *resp_type = c->dedup_uuid_resp_type;
        memcpy(resp_payload, c->dedup_uuid_resp_payload, sizeof(c->dedup_uuid_resp_payload));
        *resp_len = 2;
        return; /* resent from cache, NOT reapplied */
    }

    if (c->state != CS_STATE_IDENTIFIED) {
        pogo_nack_t nack = {.acked_type = POGO_MSG_ASSIGN_UUID, .reason = POGO_NACK_BAD_STATE};
        *resp_type = POGO_MSG_NACK;
        *resp_len = pogo_pack_nack(&nack, resp_payload, 2);
    } else {
        memcpy(c->uuid, uuid, POGO_UUID_LEN);
        pogo_ack_t ack = {.acked_type = POGO_MSG_ASSIGN_UUID};
        *resp_type = POGO_MSG_ACK;
        *resp_len = pogo_pack_ack(&ack, resp_payload, 2);
        c->state = CS_STATE_UUID_ASSIGNED;
    }
    c->dedup_uuid_valid = true;
    c->dedup_uuid_seq = seq;
    c->dedup_uuid_resp_type = *resp_type;
    memcpy(c->dedup_uuid_resp_payload, resp_payload, sizeof(c->dedup_uuid_resp_payload));
}

/* ---- scenarios ---- */

static void scenario_timeout_error_retry_peer_ready_identify_assign_uuid_ack(void)
{
    printf("\n-- scenario: handshake timeout -> ERROR -> retry -> PEER_READY -> IDENTIFY -> ASSIGN_UUID ACK --\n");
    child_t c;
    child_init(&c);

    /* Handshake times out (POGO_PORT_ERR_DOCK_TIMEOUT) before ever reaching
     * PEER_READY -- pogo_port.c's dev-compat-mode auto-retry hasn't
     * happened yet on this port. */
    on_port_error(&c);
    check(c.state == CS_STATE_ERROR, "port error moves child_service to CS_STATE_ERROR");

    /* pogo_port.c's dev-mode auto-retry succeeds this cycle and fires a new
     * PEER_READY. */
    on_peer_ready(&c);
    check(c.state == CS_STATE_READY, "PEER_READY after ERROR reaches CS_STATE_READY (was stuck in ERROR pre-fix)");
    check(c.upstream_ready == false, "upstream_ready stays false across the retry");

    uint8_t mac8[8] = {0x60, 0x55, 0xf9, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t payload[8];
    size_t len = 0;
    bool sent = handle_identify_req(&c, mac8, true, 0x01, 1, payload, &len);
    check(sent, "IDENTIFY_RESP is sent after the retried PEER_READY");
    check(c.state == CS_STATE_IDENTIFIED, "IDENTIFY_REQ moves CS_STATE_READY -> CS_STATE_IDENTIFIED");

    uint8_t uuid[POGO_UUID_LEN] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t resp_type, resp_payload[2];
    size_t resp_len;
    handle_assign_uuid(&c, /*seq=*/7, uuid, &resp_type, resp_payload, &resp_len);
    check(resp_type == POGO_MSG_ACK, "ASSIGN_UUID is ACKed (not NACK BAD_STATE) once IDENTIFIED via the retried session");
    check(c.state == CS_STATE_UUID_ASSIGNED, "state advances to CS_STATE_UUID_ASSIGNED");
    check(memcmp(c.uuid, uuid, POGO_UUID_LEN) == 0, "the assigned uuid is stored");
}

static void scenario_root_only_reboot_restarts_provisioning(void)
{
    printf("\n-- scenario: ROOT-only reboot -> new IDENTIFY_REQ -> full provisioning restart --\n");

    /* Case A: LEAF was already fully JOINED with the old parent. */
    {
        child_t c;
        child_init(&c);
        c.state = CS_STATE_JOINED;
        uint8_t old_uuid[POGO_UUID_LEN] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
        memcpy(c.uuid, old_uuid, POGO_UUID_LEN);
        c.slot_id = 3;
        c.topology_depth = 2;
        c.polling_period_ms = 960;
        c.dataset_len = 90;
        c.upstream_ready = true;
        c.dedup_uuid_valid = true;
        c.dedup_uuid_seq = 5;
        c.dedup_uuid_resp_type = POGO_MSG_ACK;

        /* ROOT rebooted and, having no memory of the old session, sends a
         * brand new IDENTIFY_REQ. This LEAF's own pogo_port never detached,
         * so no PEER_READY event happens here -- IDENTIFY_REQ itself is the
         * only signal. */
        uint8_t mac8[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        uint8_t payload[8];
        size_t len = 0;
        bool sent = handle_identify_req(&c, mac8, true, 0x01, 1, payload, &len);
        check(sent, "IDENTIFY_RESP is still sent while restarting from JOINED");
        check(c.state == CS_STATE_IDENTIFIED, "JOINED + new IDENTIFY_REQ restarts the session to CS_STATE_IDENTIFIED");
        check(c.upstream_ready == false, "upstream_ready drops back to false for the new session (condition 3)");
        uint8_t zero_uuid[POGO_UUID_LEN] = {0};
        check(memcmp(c.uuid, zero_uuid, POGO_UUID_LEN) == 0, "the old uuid is discarded");
        check(c.dataset_len == 0, "the old staged/joined dataset length is discarded");
        check(c.dedup_uuid_valid == false, "the old ASSIGN_UUID dedup cache entry is discarded");

        /* The new parent assigns a DIFFERENT uuid, reusing the SAME wire
         * seq the old session's ASSIGN_UUID used (5) -- this must NOT be
         * treated as a dedup resend of the old (now-discarded) session. */
        uint8_t new_uuid[POGO_UUID_LEN] = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB};
        uint8_t resp_type, resp_payload[2];
        size_t resp_len;
        handle_assign_uuid(&c, /*seq=*/5, new_uuid, &resp_type, resp_payload, &resp_len);
        check(resp_type == POGO_MSG_ACK, "the new session's ASSIGN_UUID is freshly ACKed, not served from the stale dedup cache");
        check(memcmp(c.uuid, new_uuid, POGO_UUID_LEN) == 0, "the NEW uuid is stored, not the discarded old one");
    }

    /* Case B: LEAF was left in CS_STATE_ERROR by an earlier failed join
     * attempt with the old parent (e.g. SET_DATASET join timeout). */
    {
        child_t c;
        child_init(&c);
        c.state = CS_STATE_ERROR;
        c.dataset_len = 90;
        c.dedup_uuid_valid = true;
        c.dedup_uuid_seq = 9;

        uint8_t mac8[8] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01, 0x02};
        uint8_t payload[8];
        size_t len = 0;
        handle_identify_req(&c, mac8, true, 0x01, 1, payload, &len);
        check(c.state == CS_STATE_IDENTIFIED, "ERROR + new IDENTIFY_REQ also restarts the session (condition 4)");
        check(c.dataset_len == 0, "ERROR-state leftovers are discarded on restart too");
    }

    /* Case C: a new IDENTIFY_REQ mid-session (e.g. CS_STATE_SENSOR_DONE)
     * must also be treated as a restart, per "UUID_ASSIGNED 이후" -- not
     * just JOINED/ERROR specifically. */
    {
        child_t c;
        child_init(&c);
        c.state = CS_STATE_SENSOR_DONE;
        c.slot_id = 1;
        uint8_t mac8[8] = {1, 1, 1, 1, 1, 1, 1, 1};
        uint8_t payload[8];
        size_t len = 0;
        handle_identify_req(&c, mac8, true, 0x01, 1, payload, &len);
        check(c.state == CS_STATE_IDENTIFIED, "mid-session (SENSOR_DONE) IDENTIFY_REQ also restarts, not just JOINED/ERROR");
        check(c.slot_id == 0, "the discarded session's slot_id is cleared");
    }

    /* Control: a re-IDENTIFY while already exactly CS_STATE_IDENTIFIED (no
     * ASSIGN_UUID processed yet) must NOT restart anything -- matches the
     * pre-existing "harmless, no side effect to redo" comment. */
    {
        child_t c;
        child_init(&c);
        c.state = CS_STATE_IDENTIFIED;
        int before = c.upstream_ready_false_count;
        uint8_t mac8[8] = {2, 2, 2, 2, 2, 2, 2, 2};
        uint8_t payload[8];
        size_t len = 0;
        handle_identify_req(&c, mac8, true, 0x01, 1, payload, &len);
        check(c.state == CS_STATE_IDENTIFIED, "re-IDENTIFY while already IDENTIFIED does not regress or restart");
        check(c.upstream_ready_false_count == before, "no spurious session reset for an already-IDENTIFIED re-IDENTIFY");
    }
}

static void scenario_identify_resp_sensor_bitmap_matches_configured(void)
{
    printf("\n-- scenario: IDENTIFY_RESP.sensor_bitmap always equals the configured value --\n");

    const uint8_t configured_bitmap = 0x01; /* configs/dev_root.json / dev_leaf.json sensor_type_bitmap */
    const uint8_t fw_version = 1;

    /* Several different "efuse reads" simulating different boots/boards --
     * a fixed IDENTIFY_RESP layout must never let any of these leak into
     * sensor_bitmap or fw_version. */
    static const uint8_t mac_samples[][8] = {
        {0x60, 0x55, 0xf9, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0x60, 0x55, 0xf9, 0xff, 0xff, 0xff, 0x36, 0x3a}, /* mirrors the field-observed corrupted bytes */
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff},
        {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
    };

    for (size_t i = 0; i < sizeof(mac_samples) / sizeof(mac_samples[0]); i++) {
        child_t c;
        child_init(&c);
        c.state = CS_STATE_READY;

        uint8_t payload[8];
        size_t len = 0;
        bool sent = handle_identify_req(&c, mac_samples[i], true, configured_bitmap, fw_version, payload, &len);
        check(sent, "IDENTIFY_RESP is sent");
        check(len == 8, "IDENTIFY_RESP payload is 8 bytes");

        pogo_identify_resp_t decoded;
        bool ok = pogo_unpack_identify_resp(payload, (uint16_t)len, &decoded);
        check(ok, "IDENTIFY_RESP round-trips through pack/unpack");
        check(decoded.sensor_bitmap == configured_bitmap,
              "sensor_bitmap on the wire equals the configured value, regardless of the efuse MAC bytes");
        check(decoded.fw_version == fw_version, "fw_version on the wire is untouched by the MAC bytes");
        check(memcmp(decoded.mac, mac_samples[i], 6) == 0, "mac field holds exactly the first 6 EUI-64 bytes");
    }
}

/* Demonstrates, deterministically and independent of the model above, the
 * actual overrun the fix removes: esp_efuse_mac_get_default() writing 8
 * bytes starting at a pogo_identify_resp_t's `mac` field overruns into
 * fw_version/sensor_bitmap because the struct is exactly 6+1+1 = 8 bytes
 * with no padding (all-uint8_t members). This is a representative
 * reproduction of the class of bug, not a byte-for-byte replay of any one
 * field log (the exact corrupted values also depend on compiler/stack
 * layout) -- see child_service.c's handle_identify_req() comment.
 */
static void scenario_old_code_reproduction_vs_fixed_copy(void)
{
    printf("\n-- scenario: reproduce the old 8-into-6-byte overrun vs. the fixed bounded copy --\n");

    uint8_t mac_efuse8[8] = {0x60, 0x55, 0xf9, 0x12, 0x34, 0x56, 0x78, 0x9a};

    pogo_identify_resp_t old_resp = {0};
    old_resp.fw_version = 1;
    old_resp.sensor_bitmap = 0x01;
    /* The old bug: esp_efuse_mac_get_default(resp.mac) with no length limit,
     * on a chip that returns 8 bytes -- modeled here as the equivalent
     * unbounded 8-byte copy starting at the struct's first field. */
    memcpy(&old_resp, mac_efuse8, sizeof(mac_efuse8));
    check(old_resp.sensor_bitmap != 0x01,
          "reproduction: the OLD unbounded 8-byte copy corrupts sensor_bitmap -- this is the bug being fixed");

    pogo_identify_resp_t new_resp = {0};
    new_resp.fw_version = 1;
    new_resp.sensor_bitmap = 0x01;
    memcpy(new_resp.mac, mac_efuse8, sizeof(new_resp.mac)); /* the fix: bounded to the 6-byte field */
    check(new_resp.sensor_bitmap == 0x01, "fix: the bounded 6-byte copy leaves sensor_bitmap untouched");
    check(new_resp.fw_version == 1, "fix: the bounded 6-byte copy leaves fw_version untouched");
}

/* ---- minimal mirror of child_service.c's owned module_config_t copy ---- */

/* Only the two fields child_service.c actually reads out of module_config_t
 * (sensor_type_bitmap in handle_identify_req(), measurement_timeout_ms in
 * handle_sensor_hello()) -- not a full mirror of module_config.h. */
typedef struct {
    uint16_t sensor_type_bitmap;
    uint32_t measurement_timeout_ms;
} module_config_t;

typedef struct {
    module_config_t cfg; /* OWNED copy -- mirrors child_service.c's static module_config_t s_cfg (the fix) */
    bool inited;
} child_cfg_owner_t;

/*
 * Mirrors the FIXED child_service_init(): copies *cfg BY VALUE into its own
 * storage and rejects a NULL cfg instead of storing/dereferencing it. The
 * pre-fix version stored `cfg` itself (a pointer into the caller's storage)
 * -- see the scenario below for why that is wrong.
 */
static bool child_service_init_cfg(child_cfg_owner_t *c, const module_config_t *cfg)
{
    if (cfg == NULL) {
        return false; /* mirrors returning ESP_ERR_INVALID_ARG */
    }
    c->cfg = *cfg;
    c->inited = true;
    return true;
}

static void scenario_module_config_outlives_caller_stack_frame(void)
{
    printf("\n-- scenario: child_service's module_config copy survives the caller's local going out of scope --\n");

    /* Mirrors module/main/app_main.c: `cfg` there is a LOCAL inside
     * app_main(), and &cfg is passed to child_service_init(). */
    module_config_t caller_cfg = {.sensor_type_bitmap = 0x0001, .measurement_timeout_ms = 1000};

    child_cfg_owner_t c = {0};
    bool ok = child_service_init_cfg(&c, &caller_cfg);
    check(ok, "child_service_init succeeds with a valid cfg pointer");

    /* Mimics app_main() returning and its stack frame being reused by
     * whatever runs next -- a host process cannot literally free/reuse one
     * C local's storage on demand, so the closest simulation is overwriting
     * it in place. If child_service_init_cfg() had stored the POINTER (the
     * original bug) instead of copying the VALUE, every check below would
     * observe this stomped memory instead of the original 0x0001 / 1000. */
    memset(&caller_cfg, 0xEE, sizeof(caller_cfg));

    check(c.cfg.sensor_type_bitmap == 0x0001,
          "child_service's owned copy still reads sensor_bitmap=0x0001 after the caller's original is stomped");
    check(c.cfg.measurement_timeout_ms == 1000,
          "child_service's owned copy still reads measurement_timeout_ms=1000 after the caller's original is stomped");

    /* condition 3 of this follow-up: child_service_init(NULL) must be
     * rejected, not dereferenced. */
    child_cfg_owner_t c2 = {0};
    bool ok2 = child_service_init_cfg(&c2, NULL);
    check(!ok2, "child_service_init(NULL) is rejected instead of dereferencing NULL (mirrors ESP_ERR_INVALID_ARG)");
}

static void scenario_dedup_retransmit_still_cached(void)
{
    printf("\n-- scenario: a genuine (type, seq) retransmit is still answered from the dedup cache --\n");
    child_t c;
    child_init(&c);
    c.state = CS_STATE_IDENTIFIED;

    uint8_t uuid[POGO_UUID_LEN] = {9, 9, 9, 9, 9, 9, 9, 9};
    uint8_t resp_type1, resp_payload1[2];
    size_t resp_len1;
    handle_assign_uuid(&c, /*seq=*/3, uuid, &resp_type1, resp_payload1, &resp_len1);
    check(resp_type1 == POGO_MSG_ACK, "first ASSIGN_UUID(seq=3) is ACKed");
    check(c.state == CS_STATE_UUID_ASSIGNED, "state advances once");

    /* A different uuid arriving with the SAME seq is a retransmission of the
     * same request (the peer never saw the ACK) -- it must be answered from
     * the cache, and must NOT re-run the assignment with this new uuid. */
    uint8_t different_uuid[POGO_UUID_LEN] = {1, 1, 1, 1, 1, 1, 1, 1};
    uint8_t resp_type2, resp_payload2[2];
    size_t resp_len2;
    handle_assign_uuid(&c, /*seq=*/3, different_uuid, &resp_type2, resp_payload2, &resp_len2);
    check(resp_type2 == POGO_MSG_ACK, "retransmit of seq=3 is answered (cached ACK), not reapplied or NACKed");
    check(memcmp(c.uuid, uuid, POGO_UUID_LEN) == 0, "the ORIGINAL uuid is still stored -- the retransmit did not overwrite it");
    check(memcmp(resp_payload1, resp_payload2, sizeof(resp_payload1)) == 0, "the cached response bytes are identical");
}

int main(void)
{
    scenario_timeout_error_retry_peer_ready_identify_assign_uuid_ack();
    scenario_root_only_reboot_restarts_provisioning();
    scenario_identify_resp_sensor_bitmap_matches_configured();
    scenario_old_code_reproduction_vs_fixed_copy();
    scenario_module_config_outlives_caller_stack_frame();
    scenario_dedup_retransmit_still_cached();

    printf("\n%d/%d checks passed\n", g_checks - g_failures, g_checks);
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
