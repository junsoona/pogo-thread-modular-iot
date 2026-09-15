# POGO interface

**A dedicated power-base project now exists -- see
[power-base.md](power-base.md).** The power base's FIXED application role
(physical POGO root, downstream provisioning coordinator, UUID/slot/topology
registry owner, sensor-data collector) is decoupled from Thread's own
`otDeviceRole`: it always runs as an always-on FTD and accepts CHILD,
ROUTER, or LEADER alike as "attached and ready" (see power-base.md's
"module/Thread role decoupling and collector provisioning" section) -- it is
no longer described here or anywhere else as Thread's "one and only ROOT/
Leader". The ROOT/INTERMEDIATE/LEAF determination table below (an
AUTO-mode sensor module never becomes `MODULE_ROLE_ROOT` any more -- that is
a role_manager/POGO-layer concept, unrelated to Thread's own `otDeviceRole`,
and is unchanged by this update) still applies. The rest of this document
(POGO frame/port mechanics, BASE_ID/DETECT_IN/DETECT_OUT electrical
contract, the confirmed GPIO profile -- updated below for the Deep-sleep-
ready DETECT_IN/DETECT_OUT swap -- dev-compat mode) still applies
identically to the power base's own one POGO port.

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
High). **On the real power-base hardware (see [power-base.md](power-base.md)),
"a power base drives it Low" means a GND contact on the power base's
connector -- there is no BASE_ID GPIO on the power base's own ESP32-H2 at
all; its firmware never configures or reads one.** GPIO2 (the module's
DETECT_IN) and GPIO14 (the module's BASE_ID) are two different signals with
two different local pull-ups -- never tie them together, and never wire
either one directly to GND on the module side (only the power base's BASE_ID
contact is GND). `pogo_port` cross-checks this hardware read against the peer's
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
  `POGO_PORT_ERR_DOCK_TIMEOUT` after `CONFIG_POGO_MAX_RETRY` retries. This is
  a **transient** error (see "AUTO-mode hotswap recovery" below): in AUTO
  mode it auto-retries a fresh handshake cycle after a cooldown, as long as
  DETECT_IN still reads attached; in development compat mode it retries
  immediately (unchanged from before).

## AUTO-mode hotswap recovery (real DETECT_IN)

A real dock motion does not guarantee DETECT closes at exactly the same
instant the UART/GND pogo contacts fully seat -- DETECT can close first,
with UART/GND landing a few ms (or, in a bad connector, longer) later. The
first handshake cycle after that DETECT edge can then legitimately time out
with no HELLO_RESP ever received, even though the module IS physically
docked and about to be fully connected. `pogo_port.c` classifies its errors
into two kinds accordingly:

- **Transient** (`POGO_PORT_ERR_DOCK_TIMEOUT`, `POGO_PORT_ERR_LIVENESS_LOST`):
  the wire itself might still be fine, or the peer might simply not have
  finished booting/seating yet. In AUTO mode, the port enters
  `PORT_STATE_RECOVER_COOLDOWN` and, as long as DETECT_IN still reads
  attached, starts a **fresh handshake cycle** after
  `CONFIG_POGO_HOTSWAP_COOLDOWN_MS` -- no new DETECT_IN edge required. Each
  cycle still retries internally up to `CONFIG_POGO_MAX_RETRY` times, with
  the existing bounded `CONFIG_POGO_ACK_TIMEOUT_MS` delay between retries. A
  detach during the cooldown (`POGO_PORT_EVENT_DETACHED`) cancels the
  pending retry immediately -- the port falls back to `WAIT_ATTACH`, not a
  stale cooldown wait. Every fresh cycle flushes leftover UART RX bytes and
  reinitializes the frame parser first (`begin_handshake()`), so a previous
  failed session's stray bytes can never desync the next one.
- **Configuration/compatibility** (`POGO_PORT_ERR_PEER_TYPE_CONFLICT`,
  `POGO_PORT_ERR_PROTOCOL_VERSION`, `POGO_PORT_ERR_CAPABILITY_MISMATCH`):
  these mean the peer itself is wrong/incompatible, not merely slow -- they
  are unchanged from before this follow-up and still require a physical
  detach/reattach before retrying, in both AUTO and dev-compat modes.

Two more pieces close the loop for a real dock:

- **Attach settle** (`PORT_STATE_ATTACH_SETTLE`, `CONFIG_POGO_ATTACH_SETTLE_MS`):
  after DETECT_IN debounces attached, the port waits this short, bounded,
  explicit interval before sending the first `HELLO_REQ`, giving UART/GND a
  moment to finish seating instead of racing the handshake against
  still-settling contacts. A detach during this window is handled the same
  way as during the cooldown above.
- **DETECT level reconciliation** (`CONFIG_POGO_DETECT_RECONCILE_MS`): a
  missed GPIO ISR (or a full `detect_queue`, see `detect_isr_drops`) can
  leave the port's committed DETECT_IN state stale forever, with no future
  edge to correct it (the level is already where it "should" be, so no new
  edge ever fires). At this interval, the port task directly re-reads
  DETECT_IN's raw level and, if it disagrees with what the port currently
  believes, injects a synthetic edge -- which still has to survive the
  normal debounce window before being committed, exactly like a real one.

See `tools/test_pogo_port_hotswap_recovery.c` for the regression coverage
(boot-detached-then-attach, DETECT-before-UART timeout with no new edge,
peer booting late, detach-during-cooldown cancellation, config/compat errors
never auto-retrying, liveness loss recovering without a reset, missed-ISR
reconciliation, and bounded/non-flooding repeated recovery) -- it links the
real, unmodified `pogo_port.c` against an in-memory fake GPIO/UART bank
(`tools/host_test_stubs/`), not a hand-written model of its logic.

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

## ROOT / INTERMEDIATE / LEAF determination table (updated -- see docs/power-base.md)

**A dedicated power-base project (`power_base/`) now exists and is always
the fixed physical root of the POGO chain** (a POGO/role_manager-layer
concept -- unrelated to, and not requiring, Thread Leadership; see
[power-base.md](power-base.md)'s "module/Thread role decoupling" section).
A sensor module in AUTO mode never becomes `MODULE_ROLE_ROOT` any more,
regardless of what its UPSTREAM peer is -- see
[power-base.md](power-base.md) for the full rationale, wiring, and the fix
this required in `role_manager.c`. The table below is current:

| UPSTREAM peer | DOWNSTREAM peer     | Role          |
|---------------|----------------------|---------------|
| POWER_BASE or MODULE | none attached | LEAF          |
| POWER_BASE or MODULE | MODULE        | INTERMEDIATE  |
| not attached / unknown | (any)       | UNASSIGNED    |
| either port ERROR       | (any)      | ROLE_ERROR    |

`MODULE_ROLE_ROOT` still exists as an enum value but is reachable ONLY via
`MODULE_DEV_MODE_DEV_ROOT` (forced directly, bypassing this table entirely --
see "development compat mode" below), never through AUTO-mode port-event
derivation.

See `components/role_manager/role_manager.c`'s `recompute()` for the exact
(deterministic, order-sensitive) implementation, and
[state-machine.md](state-machine.md) for how a role change propagates into
`thread_manager`.

**The "ROOT용 passive power-base 시험 지그" bench rig described in earlier
revisions of this document and of `README.md` (BASE_ID tied directly to GND,
no MCU on the other end, making a MODULE become ROOT) is now obsolete.** A
module whose BASE_ID reads POWER_BASE is LEAF/INTERMEDIATE, never ROOT --
the same table row as any other UPSTREAM peer. To actually test AUTO-mode
BASE_ID/PEER_HELLO behavior end to end now requires either a real
`power_base/` board, or `power_base/`'s host-tested mirror logic (see
`tools/test_power_base_peer_hello.c`); a bare GND jig with no MCU no longer
demonstrates a meaningfully different code path than the one already
exercised by module-to-module docking.

## Confirmed GPIO profile (final prototype hardware)

The ESP32-H2 SuperMini development boards ARE the final graduation-project
prototype hardware -- no separate PCB is planned. Both boards run the exact
same `pogo_module.bin`, and both use the **exact same** UPSTREAM/DOWNSTREAM
POGO GPIO assignment (only the permanently-paired sensor's `driver_id`,
I2C address, and `calibration`/`config_revision` differ between boards --
see `configs/prototype_sht45.json`/`configs/prototype_bh1750.json` and
docs/module-config.md).

**Updated by the Deep-sleep-ready GPIO follow-up (this session):** UPSTREAM
`DETECT_IN` and `DETECT_OUT` are SWAPPED from the previously-confirmed
profile -- `DETECT_IN` is now GPIO12 and `DETECT_OUT` is now GPIO2 (the
previous profile had `DETECT_IN`=2/`DETECT_OUT`=12). The reason: GPIO12 is
an ESP32-H2 RTC GPIO channel (`RTCIO_GPIO12_CHANNEL`, see
`components/module_config/deep_sleep_gpio.h` and
`components/module_config/gpio_profile.c`'s compile-time cross-check against
ESP-IDF v6.1's own `soc/esp32h2/include/soc/rtc_io_channel.h`), usable as a
future terminal-leaf's `esp_sleep_enable_ext1_wakeup_io()` Deep-sleep wake
source on its UPSTREAM `DETECT_IN` edge; GPIO2 is not an RTC GPIO channel at
all. **Deep-sleep itself is NOT implemented in this session** -- this is
purely a GPIO-assignment change to keep that future option open without
having to move DETECT again later. DOWNSTREAM `DETECT_IN`/`DETECT_OUT` (23/22)
and every other pin are unchanged. **IMPORTANT: real boards already wired
to the previous profile must have their two UPSTREAM DETECT wires swapped
before flashing firmware built from this profile** -- see README.md's
rewiring notice.

| Function              | GPIO | Notes |
|-----------------------|-----:|-------|
| I2C SDA                |    0 | sensor bus |
| I2C SCL                |    1 | sensor bus |
| UPSTREAM UART0 TX      |    4 | |
| UPSTREAM UART0 RX      |    5 | |
| UPSTREAM DETECT_IN     |   12 | pull-up input -- RTC GPIO channel, future Deep-sleep wake candidate (swapped from GPIO2, see above) |
| UPSTREAM DETECT_OUT    |    2 | open-drain, active-low (swapped from GPIO12, see above) -- **not GPIO13, not GPIO6**, see below |
| UPSTREAM BASE_ID       |   14 | active-low input, local pull-up |
| DOWNSTREAM UART1 TX    |   10 | |
| DOWNSTREAM UART1 RX    |   11 | |
| DOWNSTREAM DETECT_OUT  |   22 | open-drain, active-low (unchanged) |
| DOWNSTREAM DETECT_IN   |   23 | pull-up input (unchanged) |
| DOWNSTREAM BASE_ID     |   -1 | BASE_ID is UPSTREAM-only, enforced by module_config validation |

### ESP32-H2 GPIO usability policy (as enforced by `module_config`/`pack_module_config.py`)

| Category | GPIOs | Reason |
|---|---|---|
| Boot strapping (reject) | 8, 9, 25 | would perturb boot mode if driven the wrong way at reset |
| Not bonded out on the SuperMini/SiP package (reject) | 6, 7 | the ESP32-H2 SiP flash model does not expose these pins at all, and no SuperMini header pin carries them either -- see the GPIO6 correction below |
| Internal SPI flash (reject) | 15-21 | wired internally to the SiP's flash die |
| USB Serial/JTAG (reject) | 26, 27 | native USB D-/D+, also flash/monitor's own transport (see the console change below) |
| Allowed, ordinary GPIOs | 2, 3, 4, 5 | **not** strapping pins -- freely usable as POGO/sensor GPIOs under this firmware's current configuration (USB-Serial/JTAG console, no external pad JTAG enabled). The one caveat: enabling the chip's separate *external pad* JTAG interface would need some of these pins back -- this firmware never enables pad JTAG and never touches any JTAG-related eFuse, so the caveat is documentation only, not a runtime restriction. |
| Not chip-restricted, but avoided by this board profile | 13 | documented ESP32-H2 SuperMini onboard LED pin (`components/pogo_proto/Kconfig`) -- a board-level fact, not a chip-level rule, so `module_config`'s validation does NOT reject GPIO13 outright (see the GPIO13/GPIO6 history below) |

`components/module_config/gpio_profile.c` (`gpio_profile_in_range()`/
`gpio_profile_reserved_reason()`) is the single source of truth for this
table; `tools/pack_module_config.py`'s `STRAPPING_GPIOS`/
`NOT_BONDED_OUT_GPIOS`/`FLASH_GPIOS`/`USB_GPIOS` mirror it exactly, checked
mechanically by `tools/test_gpio_profile_parity.py` rather than by two
hand-maintained lists silently drifting apart. `module_config.c`'s
`validate_config()` additionally rejects a GPIO number outside `[-1, 27]`,
reusing one GPIO for two functions, or giving an enabled port anything other
than `uart_num` 0 or 1 -- see docs/module-config.md.

### History: UPSTREAM DETECT_IN's GPIO -- 13, then 6, now 2

The follow-up spec's originally proposed profile put UPSTREAM DETECT_IN on
**GPIO13**. This repository already documented (see
`components/pogo_proto/Kconfig`'s `POGO_UART_TX_GPIO` help text, predating
this GPIO-profile work) that GPIO13 is the ESP32-H2 SuperMini's **onboard
LED** pin -- wiring an external DETECT_IN input there would conflict with
whatever drives that LED. Per the follow-up spec's own instruction ("남는
안전한 노출 GPIO로 설정만 변경하고 그 근거를 보고한다"), UPSTREAM DETECT_IN
was first moved to **GPIO6**.

That first correction was itself wrong: **GPIO6 and GPIO7 are not bonded out
at all** on the ESP32-H2 SiP flash package this project uses, and the
SuperMini board's own header does not expose them either -- there is no
physical pad to wire a DETECT_IN signal to, regardless of what the firmware
config claims. This was caught before any board was flashed and is now
corrected: UPSTREAM DETECT_IN is **GPIO2** (an ordinary, exposed,
non-strapping GPIO under this firmware's USB-Serial/JTAG console
configuration -- see the table above). `gpio_profile.c` now rejects GPIO6/7
outright (`"not bonded out on ESP32-H2 SuperMini/SiP package"`) so this
specific mistake cannot silently recur in a future config. Every
config/doc/wiring instruction in this repository used GPIO2, not GPIO6 or
GPIO13, for UPSTREAM DETECT_IN at that point; BASE_ID stayed on GPIO14
throughout (never flagged by either piece of evidence).

**Then, GPIO2, now GPIO12 -- the Deep-sleep-ready follow-up (this
session).** UPSTREAM DETECT_IN and DETECT_OUT are swapped: DETECT_IN moves
to GPIO12 (an ESP32-H2 RTC GPIO channel, a candidate wake source for a
future terminal-leaf's Deep-sleep ext1 wakeup) and DETECT_OUT takes GPIO2
(not an RTC GPIO channel, so it was never a wake candidate in the first
place -- fine as an ordinary open-drain output). GPIO2 remains a perfectly
valid, non-strapping, non-reserved GPIO under this profile either way -- it
did not stop being usable, it simply changed which DETECT signal it carries.
See docs/power-base.md's "Deep-sleep 범위" section for the full reasoning
and what is/is not implemented this session, and README.md for the required
rewiring of already-built boards.

### Console moved off UART0

ESP32-H2 has exactly two UART controllers, and both are now committed to the
POGO ports above (UPSTREAM=UART0, DOWNSTREAM=UART1) -- there is no UART left
for a log console. `module/sdkconfig.defaults` sets
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` (no secondary console): ESP_LOG/printf
now only ever goes out over the chip's native USB-Serial/JTAG peripheral
(GPIO26/27, already reserved and never touched by any POGO/sensor config),
which is also what `idf.py flash`/`monitor` already use. No UART0 byte is
ever emitted by the application or second-stage bootloader once this
sdkconfig applies. The one caveat: the ROM's very first-stage boot messages
(before the second-stage bootloader takes over) are baked into the chip's
mask ROM and are emitted over UART0 by hardware default at every power-on
reset, regardless of any `sdkconfig` -- this is a documented ESP32-family
characteristic, not something an application `sdkconfig` can suppress
without permanently burning an eFuse (not done here), and is a few bytes at
the very start of boot, not a sustained console.

## Development compat mode (dev_root.json / dev_leaf.json, preserved as a regression fixture)

`configs/dev_root.json`/`configs/dev_leaf.json` are kept exactly as they
were before this GPIO work, as the pre-existing two-board regression
fixture. They describe a board with exactly **one** physical POGO UART pair
(UART1, GPIO10=TX, GPIO11=RX, 115200 8N1) and **no** DETECT_IN/DETECT_OUT/
BASE_ID wiring at all -- this used to be the state of the actual dev boards;
now that the confirmed GPIO profile above is real, `dev_mode=AUTO` with the
profile above is what every new board should actually be flashed with (see
`configs/prototype_sht45.json`/`configs/prototype_bh1750.json`).
`module_config`'s validation only enforces "an enabled port needs
DETECT_IN/DETECT_OUT" and "AUTO needs a real UPSTREAM BASE_ID" when
`dev_mode == AUTO` specifically, so these two files keep validating and
packing exactly as before.

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

This was written as a **temporary testing feature** before real DETECT/
BASE_ID hardware existed; now that it does (the confirmed GPIO profile
above), `dev_root_config.bin`/`dev_leaf_config.bin` and the
`MODULE_DEV_MODE_DEV_ROOT`/`DEV_LEAF` code paths are kept only as a bench-test
fallback / regression fixture -- `configs/prototype_sht45.json`/
`configs/prototype_bh1750.json` (`dev_mode=AUTO`) are what every real board
is actually flashed with.

## Idle PING/PONG liveness (section 9 of the follow-up spec)

Once `PEER_READY`, a port that has heard nothing (no HELLO, no provisioning
frame, no PING/PONG) for `CONFIG_POGO_PING_INTERVAL_MS` sends one
`POGO_MSG_PING` and expects a same-seq `POGO_MSG_PONG` within
`CONFIG_POGO_ACK_TIMEOUT_MS`. This is entirely internal to `pogo_port` --
PING/PONG frames are answered/consumed inside `process_peer_ready()` and are
**never** forwarded up as `POGO_PORT_EVENT_FRAME_RECEIVED`; higher layers
(`child_service`/`parent_service`) never see them (an earlier revision had
`child_service` answer PING itself -- that responsibility has moved into
`pogo_port`, which is the only code that ever reads this port's UART, so
there is exactly one place deciding "is this byte PING/PONG or a
provisioning frame" -- see `pogo_port.c`'s `maybe_send_idle_ping()`/
`process_peer_ready()`).

A run of `CONFIG_POGO_MAX_RETRY` consecutive PING timeouts (same budget as
every other retry in this codebase) fires `POGO_PORT_EVENT_ERROR` with the
new `POGO_PORT_ERR_LIVENESS_LOST` code and moves the port to `ERROR` --
`AUTO` mode then waits for the next physical detach/attach (a liveness loss
while `DETECT_IN` still reads attached means something is genuinely wrong,
the same reasoning as `DOCK_TIMEOUT`); dev-compat mode (no `DETECT_IN`)
auto-retries a fresh handshake after one `CONFIG_POGO_PING_INTERVAL_MS`
cooldown, mirroring `handle_dock_timeout_or_silent_power_base()`'s recovery
policy. Any real traffic (a HELLO, a provisioning frame, or a successful
PING/PONG round trip) resets the idle countdown, so a busy link never sends
gratuitous PINGs.

For console diagnostics, `pogo_port` logs `WAIT_ATTACH` once on entry and
then prints one `alive` line every 15 seconds only while in `WAIT_ATTACH` or
`ERROR`. The line includes the live `DETECT_IN` level, internal port state,
own peer type, uptime, and cumulative DETECT ISR queue drops. This is a task
liveness indicator for monitors opened after boot, not a new attach/retry
mechanism; the handshake and AUTO-mode recovery policy are unchanged.

## PMS5003 (section 11): terminal-only UART transport

PMS5003 is the chain's terminal sensor, so its module never needs a
DOWNSTREAM POGO face. `configs/prototype_pms5003.json` assigns UART0 and
GPIO4/5 to UPSTREAM POGO as usual, assigns UART1 and GPIO10/11 to PMS5003 at
9600 8N1, and fully disables every downstream port field. Config validation
rejects any UART-sensor image that also enables downstream, or that collides
with the upstream UART/GPIOs; this module therefore remains a LEAF and can
never become INTERMEDIATE.

`components/pms5003` implements ACTIVE and PASSIVE command modes, wake/mode
selection, bounded UART reads, the datasheet 32-byte frame parser, checksum
and resynchronization, and PM1.0/PM2.5/PM10 atmospheric values in ug/m3.
During the configured 30-second fan/laser stabilization period, valid data
is carried with `SENSOR_CHANNEL_STATUS_CONDITIONING` per channel and
`SENSOR_STATUS_CONDITIONING` overall (`pms5003.c`'s `ctx->status`, unlike
SGP40/SGP41 which only mark individual channels and keep their own overall
status `OK`). `parent_service.c`'s `run_provisioning_session()` explicitly
allows `SENSOR_INFO_ACK.sensor_status == CONDITIONING` in addition to `OK`
(logging that the child is still warming up) and continues the full sequence
-- POLL_CONFIG, SET_COLLECTOR, SET_DATASET, and the Thread join -- exactly
as for `OK`; any OTHER non-zero status still abandons the session as before.
Once `uart_warmup_ms` has elapsed, `pms5003.c` reports `OK` on its own, with
no re-provisioning or reboot needed. The prototype config uses PASSIVE mode,
so each measurement request produces one frame.

See `tools/test_conditioning_provisioning.c` for the regression coverage: it
links the real, unmodified `child_service.c`, `parent_service.c`,
`sensor_service.c`, and `pms5003.c` (plus two real `pogo_port.c` instances
bridged over the fake UART bank) through an actual end-to-end provisioning
run, confirming the wire really carries `CONDITIONING`, that the parent
really continues all the way to `JOIN_RESULT_ACK`, that a genuine sensor
error status is still rejected, and that the ordinary `OK` path (SHT45,
BH1750, SGP40, SGP41) has no regression. `tools/test_pms5003_driver.c`
separately covers the UART transport layer itself (wake/mode sequencing,
PASSIVE mode's READ command, checksum/partial-frame/noise resync, timeout
and UART error handling, and init-failure cleanup) against the real
`pms5003.c` + `pms5003_parser.c`.
