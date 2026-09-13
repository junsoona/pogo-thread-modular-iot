/*
 * Power base's UUID-keyed topology registry (section 9 of the power-base
 * follow-up spec). Pure, dependency-free C -- no ESP-IDF/FreeRTOS, no mutex
 * of its own (the caller, components/power_base_transport, serializes every
 * call behind one mutex, the same pattern sensor_service uses for its own
 * measurement mutex) -- so this is directly host-testable
 * (tools/test_topology_registry.c) against the REAL, unmodified logic.
 *
 * Connection ordering is reconstructed from each report's topology_depth +
 * parent_uuid, NEVER from the order reports physically arrive in (section 9:
 * "패킷 도착 순서를 물리적 연결 순서로 사용하면 안 된다").
 *
 * boot_session_generation is treated as an OPAQUE per-boot session token,
 * NEVER assumed to be numerically increasing (code review follow-up,
 * section 2: topology_reporter picks it however it likes -- a random nonce
 * or a persisted counter -- this component must not silently break either
 * way). Within one session (same generation as the stored one), sequence
 * must still strictly increase -- normal dedup/reorder rejection. ACROSS a
 * generation change (whether the new value is numerically larger OR
 * smaller than the stored one), this is treated as a legitimate new
 * session UNLESS the new generation exactly matches the one just
 * superseded (a delayed packet from the session immediately before the
 * current one) -- see apply_report()'s "superseded_generation" bookkeeping.
 * This bounded design recognizes and rejects a replay from the IMMEDIATELY
 * PRECEDING session only, not arbitrarily old ones -- a deliberate,
 * documented tradeoff (see the final report's "남은 단순화" section) rather
 * than unbounded per-node history.
 *
 * Bounded, RAM-only, array-based (TOPOLOGY_REGISTRY_MAX_NODES) -- same
 * reasoning as sensor_service's own aggregator: a permanently-paired,
 * bounded installation, never hot-swapped/auto-discovered at scale. Nothing
 * here is written to flash on every update (section 9: "레지스트리 전체를
 * 매번 flash에 쓰는 방식은 피한다") -- if power_base/main ever wants to
 * survive a reboot with a warm registry, it would persist only a small
 * summary to NVS itself and call topology_registry_apply_report() again once
 * modules re-report (which they always eventually do, via ATTACHED or
 * HEARTBEAT) -- see docs/power-base.md's "base 재부팅 후 registry 재구성"
 * section; this component itself has no NVS dependency at all.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "topology_proto/topology_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOPOLOGY_REGISTRY_MAX_NODES 32

typedef struct {
    bool     valid;   /* this slot has ever held a node (never reused for a different uuid) */
    bool     online;
    uint8_t  module_uuid[TOPOLOGY_UUID_LEN];
    uint8_t  parent_uuid[TOPOLOGY_UUID_LEN];
    uint8_t  depth;
    uint8_t  role; /* topology_role_t */
    uint16_t sensor_bitmap;
    bool     upstream_connected;
    bool     downstream_connected;
    uint32_t boot_session_generation;
    uint32_t last_seq;
    uint32_t last_seen_ms; /* caller-supplied monotonic clock, e.g. xTaskGetTickCount()*portTICK_PERIOD_MS */
    /* The generation just before boot_session_generation, so a delayed
     * packet from that exact old session is recognized and rejected even
     * though its numeric value may be larger OR smaller than the current
     * one -- see this header's top comment. has_superseded_generation is
     * false only for a node that has never crossed a session boundary yet
     * (its very first-ever sighting). */
    uint32_t superseded_generation;
    bool     has_superseded_generation;
    /* Sensor-data deliveries rejected for THIS uuid (offline at delivery
     * time, or a sensor_type_bitmap that was not a subset of this node's
     * own reported bitmap) -- see components/power_base_transport's "/s"
     * handler. Never reset by an ordinary topology update (carried forward
     * exactly like superseded_generation above). */
    uint32_t sensor_error_count;
} topology_registry_entry_t;

typedef struct {
    topology_registry_entry_t nodes[TOPOLOGY_REGISTRY_MAX_NODES];
    size_t count; /* number of slots ever used (valid==true), not number currently online */
} topology_registry_t;

typedef enum {
    TOPOLOGY_REGISTRY_OK = 0,
    /* Same generation, sequence == the stored last_seq exactly: an
     * idempotent retransmit of the most recently accepted report (the
     * sender never saw our CoAP ACK) -- already applied, nothing changed,
     * but this is NOT an error. Distinguished from TOPOLOGY_REGISTRY_OK so
     * a CoAP layer can reply with a "valid, no new representation" code
     * instead of manufacturing a second identical success. */
    TOPOLOGY_REGISTRY_OK_DUPLICATE,
    TOPOLOGY_REGISTRY_ERR_SELF_PARENT,        /* module_uuid == parent_uuid */
    TOPOLOGY_REGISTRY_ERR_INVALID_DEPTH,      /* depth == 0, or role/depth combination is impossible */
    TOPOLOGY_REGISTRY_ERR_DEPTH1_BAD_PARENT,  /* depth==1 but parent_uuid != power_base_uuid */
    TOPOLOGY_REGISTRY_ERR_UNKNOWN_PARENT,     /* depth>1 but parent_uuid names no known node yet */
    TOPOLOGY_REGISTRY_ERR_PARENT_DEPTH_MISMATCH, /* known parent's depth+1 != this report's depth */
    TOPOLOGY_REGISTRY_ERR_CYCLE,              /* module_uuid appears in its own claimed ancestor chain */
    /* A report naming the generation this node just moved away from (see
     * superseded_generation) -- a delayed packet from the PRECEDING
     * session, rejected regardless of how the numeric generation values
     * compare. NOT used for "older generation" in general any more (see
     * this header's top comment) -- an unrecognized-but-different
     * generation is instead accepted as a legitimate new session. */
    TOPOLOGY_REGISTRY_ERR_STALE_SESSION,
    TOPOLOGY_REGISTRY_ERR_DUPLICATE_SEQ,      /* same generation, sequence < the stored one (reordered) */
    TOPOLOGY_REGISTRY_ERR_FULL,               /* a brand-new uuid, but the table is already full */
} topology_registry_result_t;

/* True for any result that means "the registry now reflects this report" --
 * i.e. not rejected. Distinguishes OK/OK_DUPLICATE from every ERR_* value in
 * one place, so callers (power_base_transport's CoAP-code mapping, log
 * lines) don't have to enumerate both success values by hand. */
static inline bool topology_registry_result_is_ok(topology_registry_result_t r)
{
    return r == TOPOLOGY_REGISTRY_OK || r == TOPOLOGY_REGISTRY_OK_DUPLICATE;
}

void topology_registry_init(topology_registry_t *reg);

/*
 * Validates and (if valid) applies one topology_report_t. power_base_uuid is
 * this power base's own identity (what a depth==1 report's parent_uuid must
 * equal). now_ms is a caller-supplied monotonic clock, stored as
 * last_seen_ms for stale-timeout sweeping.
 *
 * On TOPOLOGY_REGISTRY_OK: the node is inserted/updated and marked online.
 * A TOPOLOGY_REPORT_DETACHED report additionally marks the node itself (and,
 * via topology_registry_cascade_offline_children(), everything below it)
 * offline. A report whose downstream_connected transitions from a
 * previously-stored true to false also cascades an offline to that node's
 * direct children (and their subtrees) -- see this header's top comment:
 * "중간 모듈의 downstream이 끊기면 해당 하위 subtree를 offline". Any other
 * return value leaves the registry completely unchanged (no partial apply).
 */
topology_registry_result_t topology_registry_apply_report(topology_registry_t *reg, const topology_report_t *report,
                                                            const uint8_t power_base_uuid[TOPOLOGY_UUID_LEN],
                                                            uint32_t now_ms);

/*
 * Marks every node whose parent chain passes through `uuid` (its direct
 * children, grandchildren, ...) offline -- NOT `uuid` itself. Used both by
 * topology_registry_apply_report() (a reported downstream loss) and directly
 * by power_base/main when its OWN physical DOWNSTREAM POGO port detaches
 * (an immediate hardware signal, not waiting for a stale timeout -- section 9:
 * "전원 모듈의 직접 연결이 끊기면 첫 번째 모듈과 그 모든 하위 항목을 offline
 * 처리"). Safe to call for a uuid with no children (a no-op).
 */
void topology_registry_cascade_offline_children(topology_registry_t *reg, const uint8_t uuid[TOPOLOGY_UUID_LEN]);

/*
 * Marks `uuid` itself (if known) offline, plus its whole subtree via
 * topology_registry_cascade_offline_children(). Used by power_base/main when
 * its own DOWNSTREAM port detaches: the direct child (and everything below
 * it) is unreachable the instant that physical link drops.
 */
void topology_registry_mark_offline(topology_registry_t *reg, const uint8_t uuid[TOPOLOGY_UUID_LEN]);

/*
 * Marks any currently-online node whose last_seen_ms is more than
 * stale_timeout_ms behind now_ms as offline (does not cascade -- a lost
 * subtree's own members independently go stale within the same window, see
 * this header's top comment). Call periodically (e.g. once per heartbeat
 * interval), not on every single report.
 */
void topology_registry_sweep_stale(topology_registry_t *reg, uint32_t now_ms, uint32_t stale_timeout_ms);

/*
 * Increments a KNOWN node's sensor_error_count (a sensor-data delivery
 * rejected for being offline or bitmap-mismatched -- see
 * components/power_base_transport's "/s" handler). A no-op for a uuid the
 * registry has never seen at all -- there is no slot to attribute the count
 * to (that case is diagnosed via a log line at the call site instead, same
 * policy as sensor_root_aggregator's own record_error()).
 */
void topology_registry_record_sensor_error(topology_registry_t *reg, const uint8_t uuid[TOPOLOGY_UUID_LEN]);

size_t topology_registry_count(const topology_registry_t *reg);
/* Returns false if index is out of range or that slot was never used. */
bool topology_registry_get(const topology_registry_t *reg, size_t index, topology_registry_entry_t *out);
/* Returns NULL if uuid is not known. Pointer is only valid until the next
 * mutating call on `reg` -- copy out immediately if needed longer. */
const topology_registry_entry_t *topology_registry_find(const topology_registry_t *reg,
                                                          const uint8_t uuid[TOPOLOGY_UUID_LEN]);

#ifdef __cplusplus
}
#endif
