/*
 * Runtime wiring between role_manager, the two pogo_port instances and the
 * services that react to them -- everything module/main/app_main.c used to
 * keep as file-local statics next to its boot sequence. Split out so a host
 * test can link exactly this glue (the port-event fan-out, the role-change
 * reaction that stops Thread, and parent_service's provisioning gate)
 * against the REAL role_manager.c instead of mirroring it by hand -- see
 * tools/test_role_manager_downstream_isolation.c and
 * tools/test_module_downstream_fault_isolation.c. app_main.c still owns the
 * boot order; nothing here runs before app_main() registers it.
 */
#pragma once

#include "pogo_port/pogo_port.h"
#include "role_manager/role_manager.h"
#include "parent_service/parent_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* pogo_port_init()'s callback for BOTH ports: role_manager first, then
 * child_service (UPSTREAM) / parent_service (DOWNSTREAM), then
 * topology_reporter. */
void module_wiring_port_event_dispatcher(pogo_port_id_t port_id, pogo_port_event_t event,
                                          const pogo_port_event_data_t *data, void *user_ctx);

/* role_manager_init()'s role-change callback. */
void module_wiring_on_role_change(module_role_t new_role, module_role_t old_role, void *ctx);

/* parent_service_init()'s context for a sensor module. */
const parent_service_context_t *module_wiring_parent_context(void);

#ifdef __cplusplus
}
#endif
