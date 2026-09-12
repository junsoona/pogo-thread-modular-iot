/*
 * Lower module pairing state machine.
 *
 *   UNPROVISIONED -> IDENTIFIED -> ASSIGNED -> DATASET_STORED
 *                 -> JOINING -> JOINED
 *                 -> ERROR
 *
 * Frame-driven: reacts to POGO UART messages from the upper module. Runs
 * as its own FreeRTOS task.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the pairing task. Call after pogo_transport_init(),
 * lower_storage_init() and lower_thread_init() have all succeeded. */
void lower_pairing_start(void);

#ifdef __cplusplus
}
#endif
