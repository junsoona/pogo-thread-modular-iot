/*
 * Host-only stand-in for components/role_manager/role_manager.c, used by
 * tools/test_conditioning_provisioning.c so it can link the REAL, unmodified
 * components/child_service/child_service.c, which calls exactly three
 * role_manager.h functions (role_manager_set_upstream_ready(),
 * role_manager_get_role(), role_manager_role_to_str()) -- never
 * role_manager_init()/on_port_event()/is_upstream_ready(), which only
 * module/main/app_main.c's own wiring (not linked by this test) would call.
 *
 * This is a stand-in for role derivation logic that is genuinely out of
 * scope for this test (role_manager's own DETECT-edge-driven ROOT/
 * INTERMEDIATE/LEAF derivation is unrelated to the SENSOR_STATUS_CONDITIONING
 * provisioning-gate fix under test here), not a reimplementation of anything
 * child_service.c/parent_service.c own. Defaults to MODULE_ROLE_LEAF, the
 * role a docked sensor module simulated by this test actually is.
 */
#include "role_manager/role_manager.h"

#include <pthread.h>

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static module_role_t s_role = MODULE_ROLE_LEAF;
static bool s_upstream_ready = false;

void host_role_manager_test_set_role(module_role_t role)
{
    pthread_mutex_lock(&s_mutex);
    s_role = role;
    pthread_mutex_unlock(&s_mutex);
}

bool host_role_manager_test_get_upstream_ready(void)
{
    pthread_mutex_lock(&s_mutex);
    bool ready = s_upstream_ready;
    pthread_mutex_unlock(&s_mutex);
    return ready;
}

void role_manager_set_upstream_ready(bool ready)
{
    pthread_mutex_lock(&s_mutex);
    s_upstream_ready = ready;
    pthread_mutex_unlock(&s_mutex);
}

module_role_t role_manager_get_role(void)
{
    pthread_mutex_lock(&s_mutex);
    module_role_t r = s_role;
    pthread_mutex_unlock(&s_mutex);
    return r;
}

const char *role_manager_role_to_str(module_role_t role)
{
    switch (role) {
    case MODULE_ROLE_UNASSIGNED: return "UNASSIGNED";
    case MODULE_ROLE_ROOT: return "ROOT";
    case MODULE_ROLE_INTERMEDIATE: return "INTERMEDIATE";
    case MODULE_ROLE_LEAF: return "LEAF";
    case MODULE_ROLE_ERROR: return "ERROR";
    default: return "UNKNOWN";
    }
}
