/*
 * Runtime topology role -- ROOT/INTERMEDIATE/LEAF -- derived from the two
 * pogo_port instances' attach/peer-type state (section 7 of the
 * implementation spec). This is purely a function of *this boot's* docking
 * state: it holds no persistent identity of its own (no "upper"/"lower"
 * compile-time flag), and is expected to change over a module's lifetime as
 * it gets redocked in different positions.
 *
 * role_manager does not touch UART, NVS or OpenThread itself -- it is fed
 * port events by module main (see module/main/app_main.c) and in turn only
 * calls back with role transitions. child_service/parent_service/thread_manager
 * react to those transitions; role_manager never drives them directly.
 */
#pragma once

#include <stdbool.h>
#include "module_config/module_config.h"
#include "pogo_port/pogo_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MODULE_ROLE_UNASSIGNED = 0, /* no valid UPSTREAM relationship yet (undocked, or still mid-handshake) */
    MODULE_ROLE_ROOT,           /* UPSTREAM peer is POWER_BASE */
    MODULE_ROLE_INTERMEDIATE,   /* UPSTREAM peer is MODULE, and a MODULE is also attached DOWNSTREAM */
    MODULE_ROLE_LEAF,           /* UPSTREAM peer is MODULE, DOWNSTREAM is empty */
    MODULE_ROLE_ERROR,          /* contradictory port state (e.g. a PEER_TYPE_CONFLICT on either port) */
} module_role_t;

typedef void (*role_manager_change_cb_t)(module_role_t new_role, module_role_t old_role, void *ctx);

/*
 * dev_mode != MODULE_DEV_MODE_AUTO forces the role directly (DEV_ROOT ->
 * ROOT, DEV_LEAF -> LEAF) instead of deriving it from port state -- the
 * current two-UART development boards have no BASE_ID/DETECT hardware to
 * derive it from at all (see docs/pogo-interface.md). The forced role is
 * reported via cb exactly once, synchronously, from within this call.
 */
esp_err_t role_manager_init(module_dev_mode_t dev_mode, role_manager_change_cb_t cb, void *ctx);

/*
 * Feeds one pogo_port event into the role derivation. Call this for every
 * event on both ports (module main's dispatcher forwards the same event to
 * child_service/parent_service too; role_manager only cares about
 * ATTACHED/DETACHED/PEER_READY/ERROR, not FRAME_RECEIVED). A no-op in
 * forced (non-AUTO) dev-mode.
 */
void role_manager_on_port_event(pogo_port_id_t port_id, pogo_port_event_t event, const pogo_port_event_data_t *data);

module_role_t role_manager_get_role(void);
const char *role_manager_role_to_str(module_role_t role);

/*
 * Gates DOWNSTREAM provisioning (section 7): a DOWNSTREAM module may attach
 * before this module's own UPSTREAM role and Thread Dataset are settled.
 * parent_service must check this (in addition to the role itself) before
 * starting a child session, and should re-check it once this becomes true
 * if a DOWNSTREAM peer was already PEER_READY.
 */
bool role_manager_is_upstream_ready(void);

/* Called by module main / thread_manager once this module's own Thread role
 * (Leader/Router for ROOT, joined Router/Child for INTERMEDIATE) is settled,
 * or back to false on a role change / detach that invalidates it. */
void role_manager_set_upstream_ready(bool ready);

#ifdef __cplusplus
}
#endif
