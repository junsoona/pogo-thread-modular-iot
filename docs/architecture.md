# Architecture

## Why this refactor exists

The original two-project MVP (`upper/`, `lower/`) proved the vertical slice
end-to-end on real hardware: POGO UART PING/PONG, identify, UUID/slot
assignment, Active Dataset TLV handoff, and a Thread join to `CHILD`. That
result is the regression baseline for everything below and is preserved
unmodified in `upper/`/`lower/`.

But `upper` and `lower` are not two kinds of firmware in the product this is
headed toward. A real installation is a *chain* of identical ESP32-H2
modules, each permanently paired with one sensor, stacked through physical
POGO docking. A module in the middle of a chain is simultaneously:

- a **child** to the module above it (or a power base), and
- a **parent** to the module below it.

There is no fixed "upper" or "lower" half of the chain -- there is one
firmware image, and a *runtime role* that can be different every time a
module gets docked somewhere else. That is what `module/` implements.

## One binary, two fixed things, everything else runtime

```
pogo_module.bin        <- identical bits on every ESP32-H2
      +
module_config.bin       <- DIFFERENT per physical module: the ESP32-H2<->sensor pairing
      =
one running module, whose ROOT/INTERMEDIATE/LEAF role, Thread dataset,
UUID, slot/depth, polling period, parent identity and session state are all
decided at runtime by docking, not by which .bin was flashed.
```

See [module-config.md](module-config.md) for exactly what's fixed vs.
dynamic and why they live in separate NVS partitions/namespaces.

## Component map

```
module/                     app_main.c: wires everything below together,
                             nothing else. See "Boot sequence" below.

components/
  pogo_proto/                Wire format shared by every layer above it:
                              frame codec+CRC16 (pogo_frame.*), payload
                              (de)serialization (pogo_protocol.*), and the
                              canonical pogo_peer_type_t enum. No FreeRTOS/
                              hardware dependency.

  pogo_port/                 Two independent instances (UPSTREAM,
                              DOWNSTREAM). Owns: UART, DETECT_IN/OUT GPIOs
                              (debounced attach/detach), the persistent
                              per-instance frame parser, and the post-attach
                              PEER_HELLO handshake (cross-checked against
                              BASE_ID). Emits ATTACHED/DETACHED/PEER_READY/
                              FRAME_RECEIVED/ERROR events. See
                              pogo-interface.md.

  role_manager/               Pure function of the two ports' current
                              attach/peer-type state -> ROOT / INTERMEDIATE /
                              LEAF / UNASSIGNED / ROLE_ERROR. Holds no
                              identity of its own; recomputed on every port
                              event (or forced by a dev-mode override). See
                              state-machine.md.

  thread_manager/             The one OpenThread adapter for every role
                              (module always builds CONFIG_OPENTHREAD_FTD=y).
                              Applies whichever link-mode/router-eligibility/
                              poll-period configuration the current role
                              needs, safely disabling Thread first.

  child_service/               Owns the UPSTREAM port only. Answers a
                              parent's IDENTIFY/ASSIGN_UUID/ASSIGN_SLOT/
                              SENSOR_HELLO/POLL_CONFIG/SET_DATASET, then
                              drives thread_manager to join as INTERMEDIATE
                              or LEAF (whichever role_manager has decided).

  parent_service/              Owns the DOWNSTREAM port only. Drives the
                              full provisioning sequence toward a docked
                              child: IDENTIFY -> UUID/slot(+depth) ->
                              SENSOR_HELLO -> computed POLL_CONFIG -> Active
                              Dataset TLV -> JOIN_RESULT. Holds the per-child
                              node record and the polling-period/backoff
                              calculations (pure, boundary-tested -- see
                              tools/test_polling_calc.c).

  module_config/               Loads+validates the fixed sensor/board pairing
                              from the "cfg" NVS partition. CONFIG_ERROR (no
                              sensor, no pairing, no Thread) on anything
                              wrong with it -- never a guess, never a
                              fallback driver.

  sensor_provider/             init/get_sensor_bitmap/start_measurement/
                              read_measurement/get_status/deinit interface +
                              a registry keyed by module_config.driver_id,
                              plus the development-only mock driver.

upper/, lower/                Preserved reference projects (regression
                              baseline). Unchanged except for the
                              pogo_proto wire-format additions both already
                              had to pick up (see below) -- they still build
                              and behave exactly as before.
```

## Boot sequence (`module/main/app_main.c`)

1. `nvs_flash_init()` -- the dynamic `"nvs"` partition (Thread's own dataset
   copy, plus whatever role_manager/child_service/parent_service persist).
2. `module_config_load()` -- the fixed `"cfg"` partition. Any failure halts
   here (see module-config.md) -- nothing below this line ever runs on a bad
   config.
3. `module_config_load_dev_mode()` -- AUTO / DEV_ROOT / DEV_LEAF. A non-AUTO
   value is loudly logged (development compat mode, see pogo-interface.md)
   and refuses to run if it would silently activate the mock sensor in a
   production (`AUTO`) image.
4. `thread_manager_init()` -- OpenThread FTD stack up, but Thread itself not
   yet enabled.
5. `role_manager_init()` -- starts deriving a role from port events (or is
   forced by the dev override).
6. `pogo_port_init()` x2, both routed through one dispatcher that always
   feeds `role_manager_on_port_event()`, and additionally feeds
   `child_service` (UPSTREAM events) or `parent_service` (DOWNSTREAM events).
7. `child_service_init()` / `parent_service_init()` -- always both; each only
   ever acts on its own port's events, so nothing special is needed for
   "this board doesn't have a DOWNSTREAM peer" -- parent_service simply never
   sees a DOWNSTREAM PEER_READY.
8. `pogo_port_enable()` x2 -- but only for a port `module_config` actually
   marks `enabled` (i.e. its `uart_num >= 0`). A dev-compat image disables
   one of the two entirely.

From here, everything is event-driven: `role_manager`'s role-change callback
starts Thread as ROOT (and later flips `role_manager_is_upstream_ready()`
once Leader/Router), while `child_service`/`parent_service` react to
`pogo_port` events on their respective ports.

## What changed in `pogo_proto` for this refactor

`upper`/`lower` and `module` all still share one `components/pogo_proto`.
Two changes were needed for the new provisioning steps (SENSOR_HELLO,
POLL_CONFIG, PEER_HELLO) and were made in a way that keeps `upper`/`lower`
building and behaving identically:

- New message types (`POGO_MSG_PEER_HELLO`, `POGO_MSG_SENSOR_HELLO`,
  `POGO_MSG_SENSOR_INFO_ACK`, `POGO_MSG_POLL_CONFIG`) and their payload
  structs/codecs. `upper`/`lower` never send or expect these, so this is
  purely additive for them.
- `pogo_assign_slot_t` grew a `topology_depth` field (1 byte) alongside the
  existing `slot_id`, because a chain of INTERMEDIATE modules needs to track
  *how deep* a child is, not just a fixed slot number. `upper_pairing.c` was
  updated (2-line change) to send both fields with the same fixed
  `topology_depth=1` it always implied; `lower_pairing.c` needed no change
  (it already only reads `slot_id`).

Both `upper` and `lower` were rebuilt from a clean `fullclean` after this
change and still link with zero warnings (see the build results in the
final report).
