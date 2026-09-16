#include "topology_registry/topology_registry.h"

#include <string.h>

static bool uuid_eq(const uint8_t a[TOPOLOGY_UUID_LEN], const uint8_t b[TOPOLOGY_UUID_LEN])
{
    return memcmp(a, b, TOPOLOGY_UUID_LEN) == 0;
}

void topology_registry_init(topology_registry_t *reg)
{
    memset(reg, 0, sizeof(*reg));
}

static int find_index(const topology_registry_t *reg, const uint8_t uuid[TOPOLOGY_UUID_LEN])
{
    for (size_t i = 0; i < TOPOLOGY_REGISTRY_MAX_NODES; i++) {
        if (reg->nodes[i].valid && uuid_eq(reg->nodes[i].module_uuid, uuid)) {
            return (int)i;
        }
    }
    return -1;
}

static int find_free_slot(const topology_registry_t *reg)
{
    for (size_t i = 0; i < TOPOLOGY_REGISTRY_MAX_NODES; i++) {
        if (!reg->nodes[i].valid) {
            return (int)i;
        }
    }
    return -1;
}

/*
 * Would `candidate` become its own ancestor if we accepted
 * parent_uuid=start_parent_uuid for it? Walks the claimed parent chain
 * starting at start_parent_uuid, bounded to MAX_NODES steps so even a
 * pre-existing corrupt chain (should never happen -- every insertion is
 * itself validated the same way) cannot loop forever.
 */
static bool creates_cycle(const topology_registry_t *reg, const uint8_t candidate[TOPOLOGY_UUID_LEN],
                           const uint8_t start_parent_uuid[TOPOLOGY_UUID_LEN])
{
    uint8_t cur[TOPOLOGY_UUID_LEN];
    memcpy(cur, start_parent_uuid, TOPOLOGY_UUID_LEN);
    for (size_t steps = 0; steps < TOPOLOGY_REGISTRY_MAX_NODES; steps++) {
        if (uuid_eq(cur, candidate)) {
            return true;
        }
        int idx = find_index(reg, cur);
        if (idx < 0) {
            return false; /* reached the power base (or another unknown ancestor) -- no cycle */
        }
        memcpy(cur, reg->nodes[idx].parent_uuid, TOPOLOGY_UUID_LEN);
    }
    return true; /* did not terminate within a sane bound -- treat as a cycle, refuse */
}

topology_registry_result_t topology_registry_apply_report(topology_registry_t *reg, const topology_report_t *report,
                                                            const uint8_t power_base_uuid[TOPOLOGY_UUID_LEN],
                                                            uint32_t now_ms)
{
    if (reg == NULL || report == NULL || power_base_uuid == NULL) {
        return TOPOLOGY_REGISTRY_ERR_INVALID_DEPTH;
    }

    if (uuid_eq(report->module_uuid, report->parent_uuid)) {
        return TOPOLOGY_REGISTRY_ERR_SELF_PARENT;
    }
    if (report->topology_depth == 0) {
        return TOPOLOGY_REGISTRY_ERR_INVALID_DEPTH;
    }
    /* An AUTO-mode sensor module is never ROOT and never reports while
     * UNASSIGNED (that state has no settled topology_depth at all) -- see
     * role_manager.c's determination table / docs/pogo-interface.md.
     * ROLE_ERROR IS reportable: a module can legitimately tell the registry
     * "I am in ROLE_ERROR" so it shows up as a known-but-degraded node
     * rather than silently going stale. */
    if (report->role == TOPOLOGY_ROLE_UNASSIGNED || report->role == TOPOLOGY_ROLE_ROOT) {
        return TOPOLOGY_REGISTRY_ERR_INVALID_DEPTH;
    }

    if (report->topology_depth == 1) {
        if (!uuid_eq(report->parent_uuid, power_base_uuid)) {
            return TOPOLOGY_REGISTRY_ERR_DEPTH1_BAD_PARENT;
        }
    } else {
        int parent_idx = find_index(reg, report->parent_uuid);
        if (parent_idx < 0) {
            return TOPOLOGY_REGISTRY_ERR_UNKNOWN_PARENT;
        }
        if ((uint8_t)(reg->nodes[parent_idx].depth + 1) != report->topology_depth) {
            return TOPOLOGY_REGISTRY_ERR_PARENT_DEPTH_MISMATCH;
        }
        /* Real-hardware finding: a parent that is itself offline (a direct
         * Power Base DOWNSTREAM detach, see
         * power_base_transport_on_direct_downstream_detached()) or whose own
         * downstream link just broke (that parent's own UPDATED report with
         * downstream_connected=false, see the cascade below) must not have
         * its already-offlined children resurrected by a stale
         * HEARTBEAT/UPDATED/ATTACHED that the child sends before IT notices
         * the break. Checked AFTER the depth-mismatch check above so a
         * structurally-impossible report is still reported as that specific
         * error, not conflated with liveness. */
        if (!reg->nodes[parent_idx].online || !reg->nodes[parent_idx].downstream_connected) {
            return TOPOLOGY_REGISTRY_ERR_PARENT_OFFLINE;
        }
    }

    if (creates_cycle(reg, report->module_uuid, report->parent_uuid)) {
        return TOPOLOGY_REGISTRY_ERR_CYCLE;
    }

    int idx = find_index(reg, report->module_uuid);
    bool is_new = (idx < 0);
    bool was_downstream_connected = false;
    uint32_t carry_superseded_generation = 0;
    bool carry_has_superseded_generation = false;
    uint32_t carry_sensor_error_count = 0;

    if (!is_new) {
        const topology_registry_entry_t *existing = &reg->nodes[idx];
        was_downstream_connected = existing->downstream_connected;
        carry_superseded_generation = existing->superseded_generation;
        carry_has_superseded_generation = existing->has_superseded_generation;
        carry_sensor_error_count = existing->sensor_error_count;

        if (report->boot_session_generation == existing->boot_session_generation) {
            /* Same session: normal monotonic sequence dedup. An exact
             * repeat of the most recently accepted sequence is an
             * idempotent retransmit (the sender never saw our ACK) -- OK,
             * not an error; anything older than that is a genuine
             * reorder/duplicate and IS rejected. */
            if (report->sequence == existing->last_seq) {
                return TOPOLOGY_REGISTRY_OK_DUPLICATE;
            }
            if (report->sequence < existing->last_seq) {
                return TOPOLOGY_REGISTRY_ERR_DUPLICATE_SEQ;
            }
            /* strictly greater: falls through to the normal apply below,
             * carry_* already captured above (no session transition). */
        } else if (existing->has_superseded_generation &&
                   report->boot_session_generation == existing->superseded_generation) {
            /* A delayed packet from the session we already moved away from
             * -- rejected regardless of how the numeric generation values
             * compare (boot_session_generation is an OPAQUE per-boot token,
             * never assumed to be strictly increasing -- see this header's
             * top comment). */
            return TOPOLOGY_REGISTRY_ERR_STALE_SESSION;
        } else {
            /* A generation we have never seen as this node's CURRENT or
             * IMMEDIATELY-PRECEDING session -- accepted as a legitimate new
             * session (a real reboot, possibly with a counter that did not
             * strictly increase, e.g. after an NVS reset -- code review
             * follow-up section 2: "이전 generation보다 숫자가 작은 새 부팅
             * 세션도 정상 수용"). The OLD current generation becomes the
             * new "superseded" one, so a delayed packet from IT is still
             * caught next time. */
            carry_superseded_generation = existing->boot_session_generation;
            carry_has_superseded_generation = true;
        }
    } else {
        idx = find_free_slot(reg);
        if (idx < 0) {
            return TOPOLOGY_REGISTRY_ERR_FULL;
        }
        /* brand-new node: no prior session at all, carry_* stay 0/false */
    }

    topology_registry_entry_t *e = &reg->nodes[idx];
    memset(e, 0, sizeof(*e));
    e->valid = true;
    e->online = (report->report_type != TOPOLOGY_REPORT_DETACHED);
    memcpy(e->module_uuid, report->module_uuid, TOPOLOGY_UUID_LEN);
    memcpy(e->parent_uuid, report->parent_uuid, TOPOLOGY_UUID_LEN);
    e->depth = report->topology_depth;
    e->role = report->role;
    e->sensor_bitmap = report->sensor_type_bitmap;
    e->upstream_connected = report->upstream_connected;
    e->downstream_connected = report->downstream_connected;
    e->boot_session_generation = report->boot_session_generation;
    e->last_seq = report->sequence;
    e->last_seen_ms = now_ms;
    e->superseded_generation = carry_superseded_generation;
    e->has_superseded_generation = carry_has_superseded_generation;
    e->sensor_error_count = carry_sensor_error_count;
    if (is_new) {
        reg->count++;
    }

    if (report->report_type == TOPOLOGY_REPORT_DETACHED) {
        topology_registry_cascade_offline_children(reg, report->module_uuid);
    } else if (was_downstream_connected && !report->downstream_connected) {
        /* This node itself is still online (it successfully reported!), but
         * whatever WAS docked below it just lost its physical link -- see
         * this header's top comment. */
        topology_registry_cascade_offline_children(reg, report->module_uuid);
    }

    return TOPOLOGY_REGISTRY_OK;
}

void topology_registry_cascade_offline_children(topology_registry_t *reg, const uint8_t uuid[TOPOLOGY_UUID_LEN])
{
    if (reg == NULL || uuid == NULL) {
        return;
    }
    for (size_t i = 0; i < TOPOLOGY_REGISTRY_MAX_NODES; i++) {
        if (reg->nodes[i].valid && uuid_eq(reg->nodes[i].parent_uuid, uuid)) {
            reg->nodes[i].online = false;
            topology_registry_cascade_offline_children(reg, reg->nodes[i].module_uuid);
        }
    }
}

void topology_registry_mark_offline(topology_registry_t *reg, const uint8_t uuid[TOPOLOGY_UUID_LEN])
{
    if (reg == NULL || uuid == NULL) {
        return;
    }
    int idx = find_index(reg, uuid);
    if (idx >= 0) {
        reg->nodes[idx].online = false;
    }
    topology_registry_cascade_offline_children(reg, uuid);
}

void topology_registry_sweep_stale(topology_registry_t *reg, uint32_t now_ms, uint32_t stale_timeout_ms)
{
    if (reg == NULL) {
        return;
    }
    for (size_t i = 0; i < TOPOLOGY_REGISTRY_MAX_NODES; i++) {
        topology_registry_entry_t *e = &reg->nodes[i];
        if (!e->valid || !e->online) {
            continue;
        }
        /* Unsigned subtraction wraps correctly even if now_ms has wrapped
         * around past last_seen_ms, same idiom as pogo_port's TickType_t
         * deadline comparisons. */
        uint32_t elapsed = now_ms - e->last_seen_ms;
        if (elapsed > stale_timeout_ms) {
            e->online = false;
        }
    }
}

void topology_registry_record_sensor_error(topology_registry_t *reg, const uint8_t uuid[TOPOLOGY_UUID_LEN])
{
    if (reg == NULL || uuid == NULL) {
        return;
    }
    int idx = find_index(reg, uuid);
    if (idx >= 0) {
        reg->nodes[idx].sensor_error_count++;
    }
    /* uuid unknown entirely: no slot to attribute this to -- diagnosed via
     * a log line at the call site instead (see this function's header
     * comment). */
}

size_t topology_registry_count(const topology_registry_t *reg)
{
    return (reg != NULL) ? reg->count : 0;
}

bool topology_registry_get(const topology_registry_t *reg, size_t index, topology_registry_entry_t *out)
{
    if (reg == NULL || out == NULL || index >= TOPOLOGY_REGISTRY_MAX_NODES || !reg->nodes[index].valid) {
        return false;
    }
    *out = reg->nodes[index];
    return true;
}

const topology_registry_entry_t *topology_registry_find(const topology_registry_t *reg,
                                                          const uint8_t uuid[TOPOLOGY_UUID_LEN])
{
    if (reg == NULL || uuid == NULL) {
        return NULL;
    }
    int idx = find_index(reg, uuid);
    return (idx >= 0) ? &reg->nodes[idx] : NULL;
}
