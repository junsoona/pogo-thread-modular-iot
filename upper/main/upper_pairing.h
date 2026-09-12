/*
 * Upper module pairing state machine.
 *
 *   IDLE -> DOCKED -> IDENTIFYING -> ASSIGNING -> PROVISIONING
 *        -> WAITING_JOIN -> JOINED
 *        -> ERROR_RETRY (feeds back into the step being retried, or IDLE)
 *
 * Docking is detected by periodic PING/PONG over POGO UART (no dedicated
 * dock-sense GPIO in this MVP). Runs as its own FreeRTOS task.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the pairing task. Call after pogo_transport_init() and
 * upper_thread_init() have both succeeded. */
void upper_pairing_start(void);

#ifdef __cplusplus
}
#endif
