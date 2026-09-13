#include "role_manager/role_manager.h"

#include "esp_log.h"

static const char *TAG = "role_manager";

typedef struct {
    bool attached;
    bool error;
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

static void recompute(void)
{
    if (s_forced) {
        return; /* dev-mode forced role never changes based on port events */
    }

    if (s_up.error || s_down.error) {
        set_role(MODULE_ROLE_ERROR);
        return;
    }
    if (!s_up.attached || s_up.peer_type == POGO_PEER_TYPE_UNKNOWN) {
        set_role(MODULE_ROLE_UNASSIGNED);
        return;
    }
    if (s_up.peer_type == POGO_PEER_TYPE_POWER_BASE) {
        set_role(MODULE_ROLE_ROOT);
        return;
    }
    /* s_up.peer_type == POGO_PEER_TYPE_MODULE from here on. */
    if (!s_down.attached) {
        set_role(MODULE_ROLE_LEAF);
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

    switch (event) {
    case POGO_PORT_EVENT_ATTACHED:
        st->attached = true;
        st->error = false;
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
        st->peer_type = data->peer_type;
        break;
    case POGO_PORT_EVENT_ERROR:
        st->error = true;
        break;
    case POGO_PORT_EVENT_FRAME_RECEIVED:
    default:
        return; /* not a topology-relevant event */
    }

    recompute();
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
