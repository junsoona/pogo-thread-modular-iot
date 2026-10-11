/*
 * Power Base management runtime (docs/power-base-management.md): wires
 * components/power_base_mgmt's client to OpenThread CoAP, PSA crypto and the
 * enrollment record. Present in every Power Base build; it does something
 * only on a pb_thread:mode=2 board whose enrollment record is version 3,
 * i.e. one that was enrolled with a management credential.
 *
 * Nothing here is reachable from the unauthenticated data path: the only
 * thing that can start a release is the hub's answer to this board's own
 * authenticated poll, checked by pb_mgmt_client_poll().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "power_base_enrollment/power_base_enrollment.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stops every source of new work on this board, from the management task,
 * before the release is journaled: see app_main.c. */
typedef void (*pb_mgmt_quiesce_cb_t)(void);
/* The hub was found at another endpoint, and that endpoint is already stored
 * in the enrollment record and read back: everything else that sends to the
 * hub moves there now. From the management task: see app_main.c. */
typedef void (*pb_mgmt_endpoint_moved_cb_t)(const pb_enroll_bridge_endpoint_t *endpoint);
/* False while this board is doing something that ends its enrollment. Asked
 * from the management task right before the record would be rewritten. */
typedef bool (*pb_mgmt_locate_allowed_cb_t)(void);

/*
 * Boot, mode 2 only, BEFORE thread_manager_init() -- before OpenThread
 * exists. Needs NVS and nothing else.
 *
 * A board whose enrollment record is a FINAL release (its network was erased
 * and that was verified, by the boot that wrote the record) erases both
 * network stores themselves here and reads them back, at every boot. The
 * reason is one entry: OpenThread, when it starts, imports a random key under
 * the Network Key's id if none is stored, and leaves a stored one alone. A
 * later look at the stores therefore cannot tell OpenThread's own key from a
 * network's key that an erase failed to remove. Erasing first makes the
 * question unnecessary: whatever was there is removed and that is read back,
 * and the key this start then makes is seen being stored
 * (components/power_base_ot_store). Any other record -- enrolled, a pending
 * release, none -- is left alone: those need what is stored.
 *
 * Costs a released board two flash writes per boot (the old key out, the new
 * one in). Returns at once when the record is not a final release.
 */
void pb_mgmt_runtime_prepare_released_start(const uint8_t eui64[PB_ENROLL_EUI64_LEN]);

/*
 * Boot, mode 2 only, after thread_manager_init() + thread_manager_stop() and
 * BEFORE pb_enrollment_runtime_start(): if an earlier boot left a release
 * unfinished (record UNENROLLING, or RELEASED with the erase pending) this
 * finishes it -- stores the release, enables Thread on the stored Dataset
 * only to deliver the "released" notice, erases the stored network, checks
 * that, and tries to hand the receipt to the hub over a session that exists
 * in RAM only -- and then restarts the chip; it does not return in that case.
 * No service is started for this.
 *
 * A record that is already a FINAL release is checked instead: its "erased"
 * is compared with what the network stores hold at this boot
 * (pb_network_store_holds_no_network()), after
 * pb_mgmt_runtime_prepare_released_start() erased them before OpenThread
 * started. When that did not check out (the stores could not be erased or
 * read then) they are erased once more, through OpenThread, and read back,
 * and the chip restarts if OpenThread had loaded a Dataset; when that does
 * not check out either the boot goes on to the safe wait, and no release
 * receipt is handed out (enrollment_ble.c asks the same question before
 * every one). Returns at once when there is nothing to finish and nothing to
 * erase.
 */
void pb_mgmt_runtime_finish_pending_release(const uint8_t eui64[PB_ENROLL_EUI64_LEN]);

/*
 * Once the services run (start_services_once()): starts the management task
 * if this board's record is version 3 in state ENROLLED. It polls the hub at
 * the record's own bridge endpoint every PB_MGMT_POLL_INTERVAL_MS while
 * attached. ESP_OK also when there is nothing to start (versions 1 and 2:
 * such a board cannot be unenrolled remotely).
 *
 * The same task finds the hub again when that endpoint stops answering
 * (pb_mgmt_client.h, "locate"): the hints it asks are the routing locators
 * of the border routers, servers, the leader and the routers this board sees
 * in its own Thread Network Data and router table -- read locally, nothing
 * is sent to collect them (enrollment_locate_hints.h). `endpoint_moved` NULL: this board never looks for
 * its hub (and does not say it can). Versions 1 and 2 never do: without the
 * hub's key no answer can be told from anyone else's.
 */
esp_err_t pb_mgmt_runtime_start(const uint8_t eui64[PB_ENROLL_EUI64_LEN], pb_mgmt_quiesce_cb_t quiesce,
                                pb_mgmt_endpoint_moved_cb_t endpoint_moved, pb_mgmt_locate_allowed_cb_t locate_allowed);

/*
 * This board's stored Thread network, as both a remote unenroll and a local
 * reset (enrollment_reenroll.c) erase and check it.
 *
 * erase: disables Thread (OpenThread only erases its store while disabled)
 * and erases OpenThread's persistent store. ESP_OK = the calls returned no
 * error, which is not proof of anything. After a receipt session switched
 * OpenThread's stores to RAM, OpenThread's own erase no longer reaches the
 * flash: the two stores are then erased themselves as well
 * (pb_stored_network_erase()).
 * is_empty: reads the stores themselves back -- OpenThread's settings AND its
 * persistent PSA keys, where the Network Key is kept
 * (components/power_base_ot_store/pb_stored_network.h). True only when
 * nothing is left in either; false also when one could not be read. The
 * proof of an erase, and only in the boot of that erase: from the next start
 * on OpenThread keeps a key of its own under the Network Key's id.
 * holds_no_network: the question once OpenThread has started -- no settings
 * entry, no Dataset or PSKc key, both stores readable, and under the Network
 * Key's id nothing, or the one entry this boot saw being stored after it had
 * read the stores back empty. True on a released board whose stores were
 * erased before OpenThread started; false where a network was stored again,
 * and false where a key of unknown origin lies under the Network Key's id.
 */
esp_err_t pb_network_store_erase(void);
bool pb_network_store_is_empty(void);
bool pb_network_store_holds_no_network(void);

#ifdef __cplusplus
}
#endif
