/*
 * child_service owns the UPSTREAM pogo_port only (section 7): it answers a
 * parent's IDENTIFY/ASSIGN_UUID/ASSIGN_SLOT/SENSOR_HELLO/POLL_CONFIG/
 * SET_DATASET requests, then drives thread_manager to join using the
 * received dataset as either INTERMEDIATE or LEAF (whichever role_manager
 * has settled on). It runs regardless of whether role_manager's role is
 * INTERMEDIATE or LEAF -- both have an UPSTREAM parent; only ROOT does not
 * run child_service at all.
 */
#pragma once

#include "esp_err.h"
#include "module_config/module_config.h"
#include "pogo_port/pogo_port.h"
#include "role_manager/role_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t child_service_init(const module_config_t *cfg);

/* Feed every UPSTREAM pogo_port event here (events for DOWNSTREAM are
 * ignored). Safe to call from the pogo_port callback context directly --
 * this only enqueues. */
void child_service_on_port_event(pogo_port_id_t port_id, pogo_port_event_t event, const pogo_port_event_data_t *data);

/*
 * Lets an already-JOINED child re-apply its Thread link mode (LEAF <->
 * INTERMEDIATE), without re-provisioning, if role_manager's role changes
 * while the previously-received dataset is still valid (section 2: the
 * sensor pairing and an already-received dataset both survive a role
 * change; only the DOWNSTREAM-facing behavior differs).
 */
void child_service_on_role_change(module_role_t new_role, module_role_t old_role);

/*
 * This module's own topology_depth as assigned by ITS upstream parent (0 if
 * this module is ROOT, or if no ASSIGN_SLOT has been processed yet).
 * parent_service uses this to assign own_depth+1 to whatever it attaches
 * downstream.
 */
uint8_t child_service_get_own_topology_depth(void);

#ifdef __cplusplus
}
#endif
