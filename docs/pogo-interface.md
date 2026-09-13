# POGO interface

## Final hardware: two physical ports, six signal pins each

```
UPSTREAM port                          DOWNSTREAM port
(to power base or parent module)       (to a child module)

  1. UART_TX   ---------------------->   UART_RX   (2)
  2. UART_RX   <----------------------   UART_TX    (1)
  3. DETECT_OUT ---------------------->  DETECT_IN  (4)
  4. DETECT_IN <----------------------   DETECT_OUT  (3)
  5. BASE_ID   (read on UPSTREAM only; a peer only ever DRIVES this
                from ITS DOWNSTREAM-facing side if it is a power base)
  6. RESERVED  (future WAKE/INT)
```

Numbering above matches the implementation spec's default pin list. Final
GPIO numbers, which UART controller backs which logical port, and the exact
electrical design of BASE_ID are all deliberately **not** decided here --
they come from `module_config` (see [module-config.md](module-config.md)),
never from a compiled-in constant. `components/pogo_port`'s API only ever
talks about `POGO_PORT_UPSTREAM`/`POGO_PORT_DOWNSTREAM` logical ports, GPIO
numbers as plain `int`s, and an `active_low`/`debounce_ms` pair -- so a
change to the real electrical design (e.g. BASE_ID polarity, or moving to a
third GPIO for detect) never requires touching `pogo_port.c`'s role logic,
only the numbers in `module_config`.

### DETECT_OUT / DETECT_IN contract

- `DETECT_OUT`: open-drain output. Level 0 = actively asserted ("I am here
  and powered"), level 1 = released (external pull-up takes the line High).
  **High-Z until `pogo_port_enable()`** -- `pogo_port_init()` alone touches
  no GPIO state.
- `DETECT_IN`: pull-up input, `GPIO_INTR_ANYEDGE`. The ISR
  (`detect_isr_handler` in `pogo_port.c`) does exactly one thing: read the
  current level and push `{tick, level}` into a per-port FreeRTOS queue via
  `xQueueSendFromISR`. It never touches UART, NVS, or any OpenThread API.
  30ms debounce (configurable via `module_config`) is applied in that port's
  own task, which commits an ATTACHED/DETACHED transition only once the
  level has held steady for the whole debounce window, handling both attach
  and detach edges.

### BASE_ID

Read (not driven) on the UPSTREAM port only. Active-low: a power base drives
it Low; a normal module does not drive it at all (external pull-up keeps it
High). `pogo_port` cross-checks this hardware read against the peer's
declared `peer_type` (from `HELLO_REQ`/`HELLO_RESP`, see below):

- both agree -> `POGO_PORT_EVENT_PEER_READY`.
- they contradict, OR the peer declares a `peer_type` that is neither
  `MODULE` nor `POWER_BASE` -> `POGO_PORT_EVENT_ERROR` /
  `POGO_PORT_ERR_PEER_TYPE_CONFLICT` (a stray/garbage value is a protocol
  error, not "no opinion" -- see `pogo_peer_type_is_valid()`).
- the peer never answers at all (a passive power base has no UART/MCU to
  answer with) but BASE_ID reads POWER_BASE -> still `PEER_READY` with
  `peer_type=POWER_BASE`, since BASE_ID hardware alone is sufficient in
  that case.
- the peer never answers and BASE_ID does *not* say POWER_BASE ->
  `POGO_PORT_ERR_DOCK_TIMEOUT` after `CONFIG_POGO_MAX_RETRY` retries (in
  AUTO mode this stays an error until the next physical detach/attach; in
  development compat mode, see below, it is retried automatically).

## PEER_HELLO_REQ / PEER_HELLO_RESP handshake

This is an explicit request/response pair, **not** "both sides send the same
message and hope the timing lines up" -- an earlier revision worked that
way and could fail to complete if both sides weren't in lock step (e.g. one
board booting 5 seconds after the other). Whichever side attaches/boots
first is the *initiator*:

1. After a debounced ATTACHED (or immediately on enable, in development
   compat mode), the initiator sends `POGO_MSG_PEER_HELLO_REQ` (protocol
   version + this module's own `peer_type`, normally always
   `POGO_PEER_TYPE_MODULE`) and waits up to `CONFIG_POGO_ACK_TIMEOUT_MS` for
   a same-seq `POGO_MSG_PEER_HELLO_RESP`, retrying up to
   `CONFIG_POGO_MAX_RETRY` times.
2. **A `HELLO_REQ` is answered with `HELLO_RESP` in any state** -- whether
   this port is still running its own handshake (simultaneous boot: each
   side is simultaneously an initiator and a responder; staggered boot: the
   side that attached first keeps answering the late side's `REQ`s once
   they start arriving) or already `PEER_READY` (the peer rebooted while
   still docked and is starting its own handshake fresh -- answering does
   not reset this side's own `PEER_READY`).
3. Both message types validate the same way (`validate_incoming_hello()` in
   `pogo_port.c`): `protocol_version` must match (else
   `PROTOCOL_VERSION_ERROR`), `peer_type` must be `MODULE` or `POWER_BASE`
   (else `PEER_TYPE_CONFLICT`), and BASE_ID (if present) must agree.

This is fully internal to `pogo_port` -- higher layers
(`child_service`/`parent_service`) never see a `HELLO_REQ`/`HELLO_RESP`
frame; they only ever see `POGO_PORT_EVENT_PEER_READY` and, from then on,
`POGO_PORT_EVENT_FRAME_RECEIVED` for the actual provisioning messages.
`upper`/`lower` never used the old single `PEER_HELLO` message type either,
so replacing it with this REQ/RESP pair is purely additive to the
regression baseline.

## ROOT / INTERMEDIATE / LEAF determination table

| UPSTREAM peer | DOWNSTREAM peer     | Role          |
|---------------|----------------------|---------------|
| POWER_BASE    | (irrelevant)         | ROOT          |
| MODULE        | MODULE               | INTERMEDIATE  |
| MODULE        | none attached        | LEAF          |
| not attached / unknown | (any)       | UNASSIGNED    |
| either port ERROR       | (any)      | ROLE_ERROR    |

See `components/role_manager/role_manager.c`'s `recompute()` for the exact
(deterministic, order-sensitive) implementation, and
[state-machine.md](state-machine.md) for how a role change propagates into
`thread_manager`.

## Development compat mode (current two boards)

The current development boards (ESP32-H2 SuperMini x2) have exactly **one**
physical POGO UART pair each (UART1, GPIO10=TX, GPIO11=RX, 115200 8N1) and
**no** DETECT_IN/DETECT_OUT/BASE_ID wiring at all. This is a temporary
hardware limitation, not a design choice -- the code above is written
against the final two-port design; the dev boards just can't exercise all
of it yet.

To still validate the *same* `pogo_module.bin` on these boards, `module_config`
carries a small, separate NVS namespace (`dev_cfg`, distinct from the fixed
sensor pairing's `module_cfg` namespace -- see module-config.md) holding one
override value:

| dev_mode   | Effect                                                          |
|------------|------------------------------------------------------------------|
| `AUTO`     | Final behavior: role derived from BASE_ID + PEER_HELLO as above.  |
| `DEV_ROOT` | The single UART is mapped to DOWNSTREAM only (UPSTREAM port disabled entirely in `module_config`); `role_manager` is forced directly to ROOT instead of deriving it. Reproduces the old `upper` project's behavior. |
| `DEV_LEAF` | The single UART is mapped to UPSTREAM only (DOWNSTREAM disabled); `role_manager` is forced directly to LEAF. Reproduces the old `lower` project's behavior. |

`dev_mode` is **not** a compile-time firmware choice -- it is one NVS value
in the same `cfg` partition image, generated per-board by
`tools/pack_module_config.py` from `configs/dev_root.json` /
`configs/dev_leaf.json` (see module-config.md for the generation commands).
A non-`AUTO` value is always logged loudly at boot
(`module_main: DEVELOPMENT OVERRIDE ACTIVE: dev_mode=...`) so a real-hardware
log is never mistaken for a dev-compat one.

Because dev-compat ports have no `DETECT_IN` (`detect_in_gpio == -1`), a
port is always attached the instant it is enabled and can never learn "the
peer physically left" the way `AUTO` mode can via a debounced detach. A
`DOCK_TIMEOUT` (e.g. the peer board hasn't finished booting yet) would
therefore leave the port wedged in `ERROR` forever under `AUTO`'s policy of
"wait for the next detach/attach". `pogo_port.c` handles this specifically
for `detect_in_gpio < 0`: it still logs the failure and fires
`POGO_PORT_EVENT_ERROR`, then waits `CONFIG_POGO_PING_INTERVAL_MS` (a
bounded cooldown, not a hot loop) and starts a fresh handshake cycle --
itself still capped at `CONFIG_POGO_MAX_RETRY` retries. `PEER_TYPE_CONFLICT`
and `PROTOCOL_VERSION_ERROR` are NOT auto-retried even in dev-compat mode --
those mean something is actually wrong (config or version mismatch), not
just "the peer isn't listening yet".

This is explicitly a **temporary testing feature**: once real DETECT/BASE_ID
hardware exists, `dev_root_config.bin`/`dev_leaf_config.bin` and the
`MODULE_DEV_MODE_DEV_ROOT`/`DEV_LEAF` code paths stop being needed for
day-to-day validation (they can stay as a bench-test fallback, but AUTO
becomes the only image anyone actually flashes to a finished module).
