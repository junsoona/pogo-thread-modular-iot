/*
 * Bidirectional POGO port: one physical UPSTREAM or DOWNSTREAM contact face,
 * each with its own UART, DETECT_IN/DETECT_OUT contact-sense GPIOs, an
 * optional BASE_ID GPIO, a persistent frame parser and an independent
 * attach/handshake state machine. Exactly two independent instances exist
 * (POGO_PORT_UPSTREAM, POGO_PORT_DOWNSTREAM) -- there is no global parser or
 * global pairing context (see section 5 of the implementation spec).
 *
 * Responsibilities owned by this component:
 *   - DETECT_IN edge -> 30ms-debounced attach/detach (ISR only queues raw
 *     edges; UART/NVS/OpenThread are never touched from ISR context).
 *   - DETECT_OUT: open-drain, High-Z until pogo_port_enable(), then actively
 *     driven Low to announce "I am here and powered" to the peer.
 *   - Post-attach PEER_HELLO_REQ/PEER_HELLO_RESP handshake (an explicit
 *     request/response, NOT "both sides send the same message" -- see
 *     pogo_port.c), cross-checked against the BASE_ID hardware read (see
 *     pogo_port_error_t / PEER_TYPE_CONFLICT). Whichever side attaches/boots
 *     first is the initiator and sends REQ; a REQ is answered with RESP in
 *     ANY state, including already-PEER_READY (covers the peer rebooting
 *     while still docked).
 *   - The persistent, per-instance pogo_frame_parser_t and UART RX/TX.
 *
 * Everything above the handshake (IDENTIFY, UUID/slot assignment, dataset
 * transfer, sensor info, polling config, ...) is out of scope here: once
 * POGO_PORT_EVENT_PEER_READY fires, higher layers (child_service on
 * UPSTREAM, parent_service on DOWNSTREAM) exchange further frames via
 * pogo_port_send() and receive them via POGO_PORT_EVENT_FRAME_RECEIVED.
 *
 * The event callback runs on this port's own FreeRTOS task, NOT from ISR
 * context -- but it should still be lightweight (e.g. post to the caller's
 * own queue) rather than block, so it does not stall this port's debounce/
 * handshake housekeeping or the other port's independent instance.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "pogo_proto/pogo_frame.h"
#include "pogo_proto/pogo_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POGO_PORT_UPSTREAM = 0,
    POGO_PORT_DOWNSTREAM = 1,
    POGO_PORT_ID_COUNT,
} pogo_port_id_t;

typedef enum {
    POGO_PORT_EVENT_ATTACHED = 0,   /* DETECT_IN debounced Low (or: DETECT hardware absent + port just enabled -- dev compat mode). Pre-handshake. */
    POGO_PORT_EVENT_DETACHED,       /* DETECT_IN debounced High. Any in-flight session/handshake is cancelled and invalid. */
    POGO_PORT_EVENT_PEER_READY,     /* HELLO_REQ/RESP (or a silent-POWER_BASE BASE_ID read) resolved; data->peer_type is authoritative from here on. */
    POGO_PORT_EVENT_FRAME_RECEIVED, /* one post-handshake protocol frame (HELLO_REQ/RESP frames are consumed internally and never reach here). */
    POGO_PORT_EVENT_ERROR,          /* data->error_code names which. In AUTO (real DETECT hardware) the port stays in ERROR until the next DETACHED; in dev compat mode (no DETECT_IN) it auto-retries the handshake cycle -- see pogo_port.c. */
} pogo_port_event_t;

typedef enum {
    POGO_PORT_ERR_NONE = 0,
    POGO_PORT_ERR_DOCK_TIMEOUT,        /* attached, but the peer never answered HELLO_REQ and BASE_ID doesn't explain why */
    POGO_PORT_ERR_PEER_TYPE_CONFLICT,  /* BASE_ID hardware contradicts the peer's declared peer_type, OR the peer declared a peer_type that is neither MODULE nor POWER_BASE */
    POGO_PORT_ERR_PROTOCOL_VERSION,    /* peer's HELLO_REQ/RESP protocol_version does not match ours */
} pogo_port_error_t;

typedef struct {
    pogo_peer_type_t   peer_type;  /* valid for POGO_PORT_EVENT_PEER_READY */
    pogo_frame_t        frame;      /* valid for POGO_PORT_EVENT_FRAME_RECEIVED */
    pogo_port_error_t  error_code; /* valid for POGO_PORT_EVENT_ERROR */
} pogo_port_event_data_t;

typedef void (*pogo_port_event_cb_t)(pogo_port_id_t port_id, pogo_port_event_t event,
                                      const pogo_port_event_data_t *data, void *user_ctx);

typedef struct {
    bool enabled;                /* false: this logical port does not exist on this board (dev compat mode) */
    int  uart_num;
    int  tx_gpio;
    int  rx_gpio;
    int  detect_in_gpio;         /* -1: no DETECT hardware -- port is treated as attached as soon as it is enabled (dev compat mode) */
    int  detect_out_gpio;        /* -1: not driven */
    int  base_id_gpio;           /* -1: not present (only ever meaningful on an UPSTREAM port) */
    bool detect_out_active_low;  /* must be true on real hardware; kept explicit for documentation/testability */
    uint32_t debounce_ms;
    int  baud_rate;
} pogo_port_config_t;

/*
 * Registers this port's static config and callback. Does not touch any
 * hardware yet (GPIOs stay High-Z, UART is not installed) -- call
 * pogo_port_enable() once module_config/role_manager decide this port
 * should actually run.
 */
esp_err_t pogo_port_init(pogo_port_id_t id, const pogo_port_config_t *cfg, pogo_port_event_cb_t cb, void *user_ctx);

/*
 * Brings up the UART + DETECT_OUT (driven Low) + DETECT_IN (interrupt) for
 * this port and starts its attach/handshake task. own_peer_type is what
 * THIS module answers with in PEER_HELLO (normally POGO_PEER_TYPE_MODULE --
 * only a dedicated power-base product would ever pass POGO_PEER_TYPE_POWER_BASE).
 */
esp_err_t pogo_port_enable(pogo_port_id_t id, pogo_peer_type_t own_peer_type);

/* Releases DETECT_OUT back to High-Z and stops the port's task. Any
 * in-flight session is invalidated first, same as a physical detach. */
esp_err_t pogo_port_disable(pogo_port_id_t id);

bool pogo_port_is_attached(pogo_port_id_t id);

/* Only meaningful once POGO_PORT_EVENT_PEER_READY has fired for this port
 * (returns POGO_PEER_TYPE_UNKNOWN otherwise). */
pogo_peer_type_t pogo_port_get_peer_type(pogo_port_id_t id);

/*
 * Sends one post-handshake protocol frame. seq is caller-supplied (the
 * request/response and dedup semantics belong to child_service/parent_service,
 * not to this transport layer). Returns ESP_ERR_INVALID_STATE if the port
 * is not currently PEER_READY.
 */
esp_err_t pogo_port_send(pogo_port_id_t id, uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len);

#ifdef __cplusplus
}
#endif
