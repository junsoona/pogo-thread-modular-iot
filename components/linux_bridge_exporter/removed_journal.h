/*
 * Durable copy of linux_bridge_exporter's unconfirmed TOPOLOGY_REMOVED set.
 *
 * A confirmed POGO detach erases the module from topology_registry at once,
 * so after a reboot nothing in RAM remembers that Linux still has to be
 * told. The exporter mirrors its pending-tombstone set here, in the default
 * "nvs" partition (namespace "pb_rm_journal", the same partition
 * OpenThread already writes), and reloads it at boot. The set only changes
 * on a confirmed detach, a re-attach of a still-pending uuid, or a
 * confirmed delivery, so flash writes stay rare.
 *
 * Private to components/linux_bridge_exporter. Blocking flash I/O: call
 * only from the exporter task or from linux_bridge_exporter_init(), never
 * from an OpenThread callback.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "topology_proto/topology_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads up to `capacity` uuids into `out` and returns how many were stored.
 * A missing journal, or one that fails validation, reads as empty (the
 * invalid record is logged and erased so it is not reread every boot). */
size_t removed_journal_load(uint8_t out[][TOPOLOGY_UUID_LEN], size_t capacity);

/* Replaces the whole journal with `uuids[0..count)` (count == 0 erases it)
 * and commits. Returns the first failing NVS error, if any. */
esp_err_t removed_journal_save(const uint8_t uuids[][TOPOLOGY_UUID_LEN], size_t count);

#ifdef __cplusplus
}
#endif
