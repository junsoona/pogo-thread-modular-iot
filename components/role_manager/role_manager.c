#include "role_manager/role_manager.h"

#include "esp_log.h"

static const char *TAG = "role_manager";

typedef struct {
    bool attached;
    bool error;
    pogo_port_error_t error_code; /* which POGO_PORT_EVENT_ERROR set `error`; only meaningful while it is true */
    pogo_peer_type_t peer_type;
} port_state_t;

static port_state_t s_up;
static port_state_t s_down;
static module_role_t s_role = MODULE_ROLE_UNASSIGNED;
static bool s_forced = false;
static bool s_upstream_ready = false;
static role_manager_change_cb_t s_cb;
static void *s_ctx;

const char *role_manager_role_to_str(module_role_t role)
{
    switch (role) {
    case MODULE_ROLE_ROOT:
        return "ROOT";
    case MODULE_ROLE_INTERMEDIATE:
        return "INTERMEDIATE";
    case MODULE_ROLE_LEAF:
        return "LEAF";
    case MODULE_ROLE_ERROR:
        return "ROLE_ERROR";
    case MODULE_ROLE_UNASSIGNED:
    default:
        return "UNASSIGNED";
    }
}

static void set_role(module_role_t new_role)
{
    if (new_role == s_role) {
        return;
    }
    module_role_t old = s_role;
    s_role = new_role;
    ESP_LOGI(TAG, "role changed: %s -> %s", role_manager_role_to_str(old), role_manager_role_to_str(new_role));
    if (s_cb) {
        s_cb(new_role, old, s_ctx);
    }
}

/*
 * DOCK_TIMEOUT and LIVENESS_LOST are the two errors pogo_port itself treats
 * as transient: it keeps DETECT_IN's attach, waits out its cooldown and
 * starts a fresh handshake cycle on its own (see pogo_port.h). On the
 * DOWNSTREAM port they say nothing about this module's own UPSTREAM
 * relationship or Thread membership, so they are isolated to that link
 * instead of becoming MODULE_ROLE_ERROR -- see recompute(). Every other
 * error code (PEER_TYPE_CONFLICT, PROTOCOL_VERSION, CAPABILITY_MISMATCH, or
 * anything unclassified) is NOT isolated, on either port.
 */
static bool downstream_fault_is_isolated(void)
{
    return s_down.error && (s_down.error_code == POGO_PORT_ERR_DOCK_TIMEOUT ||
                            s_down.error_code == POGO_PORT_ERR_LIVENESS_LOST);
}

static void recompute(void)
{
    if (s_forced) {
        return; /* dev-mode forced role never changes based on port events */
    }

    /* Any UPSTREAM error, and any DOWNSTREAM error that is not an isolated
     * transient fault, is ROLE_ERROR. UPSTREAM is checked first and wins
     * when both ports are in error. */
    if (s_up.error || (s_down.error && !downstream_fault_is_isolated())) {
        set_role(MODULE_ROLE_ERROR);
        return;
    }
    if (!s_up.attached || s_up.peer_type == POGO_PEER_TYPE_UNKNOWN) {
        set_role(MODULE_ROLE_UNASSIGNED);
        return;
    }
    /*
     * Power-base follow-up spec, section 2: an AUTO-mode sensor module NEVER
     * becomes ROOT -- the dedicated power base is the system's one and only
     * ROOT/Leader (see docs/power-base.md). Whether the UPSTREAM peer is the
     * power base or another sensor module, this module's own role is always
     * LEAF/INTERMEDIATE, decided purely by its own DOWNSTREAM state -- see
     * the determination table in docs/pogo-interface.md. (An earlier
     * revision of this refactor -- before the power base existed -- treated
     * UPSTREAM peer_type==POWER_BASE as this module becoming ROOT itself;
     * that is exactly what this follow-up supersedes. MODULE_ROLE_ROOT is
     * kept only for the MODULE_DEV_MODE_DEV_ROOT forced-role regression
     * fixture, which bypasses this function entirely -- see s_forced above.)
     */
    if (!s_down.attached) {
        set_role(MODULE_ROLE_LEAF);
        return;
    }
    if (s_down.error) {
        /*
         * Isolated DOWNSTREAM fault: something is still physically docked
         * below (DETECT_IN attached) but its link is failed/retrying. That
         * is neither an empty port nor a ready MODULE, so the role this
         * module already settled into is held -- no ROLE_ERROR (which would
         * stop this module's own Thread membership), and no demotion of an
         * INTERMEDIATE to LEAF either (a sleepy-end-device link-mode
         * re-apply while a module is still docked, undone again on
         * recovery). parent_service and topology_reporter get the same
         * ERROR event and drop the DOWNSTREAM session / connected state
         * themselves; a later PEER_READY clears `error` and resumes the
         * normal derivation below. With no settled role to hold (UPSTREAM
         * resolved while DOWNSTREAM was already faulted), the unresolved
         * peer is not a ready MODULE: LEAF.
         */
        if (s_role != MODULE_ROLE_LEAF && s_role != MODULE_ROLE_INTERMEDIATE) {
            set_role(MODULE_ROLE_LEAF);
        }
        return;
    }
    if (s_down.peer_type == POGO_PEER_TYPE_MODULE) {
        set_role(MODULE_ROLE_INTERMEDIATE);
        return;
    }
    /* DOWNSTREAM attached but its handshake has not resolved peer_type yet
     * (still mid-PEER_HELLO) -- keep the previous role, no premature call. */
}

esp_err_t role_manager_init(module_dev_mode_t dev_mode, role_manager_change_cb_t cb, void *ctx)
{
    s_up = (port_state_t){0};
    s_down = (port_state_t){0};
    s_upstream_ready = false;
    s_cb = cb;
    s_ctx = ctx;
    s_forced = (dev_mode != MODULE_DEV_MODE_AUTO);
    s_role = MODULE_ROLE_UNASSIGNED;

    if (dev_mode == MODULE_DEV_MODE_DEV_ROOT) {
        ESP_LOGW(TAG, "development override: DEV_ROOT -- forcing role=ROOT (see docs/pogo-interface.md)");
        set_role(MODULE_ROLE_ROOT);
    } else if (dev_mode == MODULE_DEV_MODE_DEV_LEAF) {
        ESP_LOGW(TAG, "development override: DEV_LEAF -- forcing role=LEAF (see docs/pogo-interface.md)");
        set_role(MODULE_ROLE_LEAF);
    }
    return ESP_OK;
}

void role_manager_on_port_event(pogo_port_id_t port_id, pogo_port_event_t event, const pogo_port_event_data_t *data)
{
    if (s_forced) {
        return;
    }
    port_state_t *st = (port_id == POGO_PORT_UPSTREAM) ? &s_up : &s_down;
    bool new_downstream_fault = false;

    switch (event) {
    case POGO_PORT_EVENT_ATTACHED:
        st->attached = true;
        st->error = false;
        st->error_code = POGO_PORT_ERR_NONE;
        st->peer_type = POGO_PEER_TYPE_UNKNOWN;
        break;
    case POGO_PORT_EVENT_DETACHED:
        *st = (port_state_t){0};
        if (port_id == POGO_PORT_UPSTREAM) {
            s_upstream_ready = false;
        }
        break;
    case POGO_PORT_EVENT_PEER_READY:
        st->attached = true;
        st->error = false;
        st->error_code = POGO_PORT_ERR_NONE;
        st->peer_type = (data != NULL) ? data->peer_type : POGO_PEER_TYPE_UNKNOWN;
        break;
    case POGO_PORT_EVENT_ERROR: {
        /* pogo_port always passes the code; without one the error is
         * unclassified and therefore never isolated (see recompute()). */
        pogo_port_error_t code = (data != NULL) ? data->error_code : POGO_PORT_ERR_NONE;
        /* Once per fault kind, not once per retry cycle -- pogo_port already
         * logs every cycle's own DOCK_TIMEOUT. */
        new_downstream_fault = (port_id == POGO_PORT_DOWNSTREAM && code != s_down.error_code);
        st->error = true;
        st->error_code = code;
        /* The peer_type learned from an earlier PEER_READY describes a link
         * that no longer exists; only a fresh PEER_READY sets it again. */
        st->peer_type = POGO_PEER_TYPE_UNKNOWN;
        break;
    }
    case POGO_PORT_EVENT_FRAME_RECEIVED:
    default:
        return; /* not a topology-relevant event */
    }

    recompute();

    if (new_downstream_fault && downstream_fault_is_isolated() &&
        (s_role == MODULE_ROLE_LEAF || s_role == MODULE_ROLE_INTERMEDIATE)) {
        ESP_LOGW(TAG, "DOWNSTREAM transient error=%d isolated to the downstream link -- keeping role %s",
                 (int)s_down.error_code, role_manager_role_to_str(s_role));
    }
}

module_role_t role_manager_get_role(void)
{
    return s_role;
}

bool role_manager_is_upstream_ready(void)
{
    return s_upstream_ready;
}

void role_manager_set_upstream_ready(bool ready)
{
    s_upstream_ready = ready;
}
