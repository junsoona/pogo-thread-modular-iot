# Architecture

**A dedicated power-base project (`power_base/`) now exists and is the
system's one and only Thread ROOT/Leader -- see [power-base.md](power-base.md)
for its full architecture, boot sequence, and component reuse table.** This
document's component map/boot sequence below describe `module/` (the sensor
module firmware) specifically; `power_base/` is a separate ESP-IDF
project/binary that reuses `pogo_port`/`pogo_proto`/`thread_manager`/
`parent_service`/`sensor_channel` unmodified and never runs
`role_manager`/`child_service`/`sensor_provider`/`sensor_service` at all.

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
                              ASSIGN_PARENT_INFO -> SENSOR_HELLO -> computed
                              POLL_CONFIG -> Active Dataset TLV -> JOIN_RESULT.
                              Holds the per-child node record and the
                              polling-period/backoff calculations (pure,
                              boundary-tested -- see tools/test_polling_calc.c).
                              Reused UNMODIFIED by power_base/ (see
                              power-base.md): it takes a small
                              parent_service_context_t at init instead of
                              reaching into role_manager/child_service
                              directly, so the same component serves both "a
                              sensor module that is ROOT/INTERMEDIATE" and
                              "the dedicated power base".

  topology_proto/              Fixed-length, versioned binary wire format for
                              topology reports (a module's role/parent/depth/
                              connection state), sent over Thread CoAP --
                              separate from pogo_proto (POGO) and
                              sensor_channel (sensor samples). No ESP-IDF
                              dependency. See power-base.md section 9.

  topology_registry/           Power base's pure, dependency-free, RAM-only,
                              UUID-keyed topology table: validates and
                              applies topology reports (self-parent/cycle/
                              depth-jump/stale-session/duplicate-sequence
                              rejection), and handles direct- and
                              intermediate-downstream-detach subtree
                              offlining plus stale-timeout sweeping. See
                              power-base.md section 10 and
                              tools/test_topology_registry.c.

  topology_reporter/            Module-side sender: reports this module's own
                              role/parent/depth/connection state to the power
                              base's "/t" CoAP resource (ATTACHED/UPDATED/
                              DETACHED/HEARTBEAT), with the same bounded
                              UUID-hash backoff sensor_transport already
                              uses. Polls role_manager rather than hooking
                              its role-change callback -- see that
                              component's own header comment for why.

  sensor_root_aggregator/       The bounded, mutex-protected,
                              (module_uuid,sample_seq)-deduplicating sample
                              store, extracted out of sensor_service so
                              power_base_transport can use the exact same
                              logic with no sensor driver dependency at all
                              (the power base has no sensor of its own).
                              sensor_service's own public
                              sensor_service_root_*() functions are now thin
                              wrappers around this component, unchanged in
                              behavior/signature.

  power_base_config/            power_base/'s analogue of module_config --
                              its own small, independent fixed-config NVS
                              blob (no sensor fields at all), reusing
                              module_config/gpio_profile.h's ESP32-H2 GPIO
                              policy and pogo_proto/pogo_crc16.h's CRC16
                              integrity scheme rather than duplicating either.

  power_base_transport/         power_base/'s Thread CoAP receiving side:
                              registers "/s" (sensor data, reusing
                              sensor_channel unmodified) and "/t" (topology
                              reports), owns the topology_registry instance
                              and its mutex, cross-checks a sensor sample's
                              UUID against the registry before storing it.

  module_config/               Loads+validates the fixed sensor/board pairing
                              from the "cfg" NVS partition, including the
                              ESP32-H2 GPIO profile (strapping/flash/USB
                              exclusion, UART0/1-only, AUTO-only DETECT/
                              BASE_ID requirements -- see
                              docs/module-config.md). CONFIG_ERROR (no
                              sensor, no pairing, no Thread) on anything
                              wrong with it -- never a guess, never a
                              fallback driver.

  sensor_provider/             init/get_sensor_bitmap/get_min_polling_period_ms/
                              start_measurement/read_measurement/get_status/
                              deinit interface + a registry keyed by
                              module_config.driver_id (mock built in;
                              bh1750/sht45/sgp40/sgp41/pms5003 register
                              themselves at boot via sensor_provider_register()
                              -- see that function's doc comment for why real
                              drivers live in their own components instead of
                              inside sensor_provider itself).

  sensor_channel/               The common per-sample wire format (section 6
                              of the follow-up spec): pack/unpack a
                              sensor_sample_t (module UUID, sample_seq,
                              uptime, sensor_bitmap, and N typed/scaled
                              fixed-point channel readings) to/from bytes.
                              No ESP-IDF dependency; also defines
                              sensor_channel_type_t, the one shared channel
                              numbering module_config.sensor_type_bitmap and
                              this wire format both use.

  node_calc/                    Pure integer math shared by parent_service
                              and sensor_service/sensor_transport: the
                              response-time -> polling-period formula and the
                              UUID-hash backoff (both originally
                              parent_service-only; moved here once
                              sensor_service also needed them, to avoid a
                              parent_service <-> child_service <-> sensor_service
                              dependency cycle).

  sensor_service/               Owns the ONE permanently-paired sensor driver
                              (section 3), independent of role: initializes
                              it exactly once, serializes every measurement
                              (provisioning AND periodic) through one mutex,
                              runs the periodic sampling task at whatever
                              effective_polling_period_ms is current, records
                              ROOT's own samples into a local aggregator, and
                              hands non-ROOT samples to a registered
                              "sample ready" callback (wired to
                              sensor_transport by module/main/app_main.c).
                              child_service now only ever calls
                              sensor_service_measure_once() for SENSOR_HELLO,
                              never sensor_provider_ops_t directly.

  sensor_transport/              The Thread data-plane (CoAP) transport for
                              periodic samples (section 8) -- deliberately
                              separate from pogo_proto (left alone). ROOT
                              runs a CoAP resource that unpacks a
                              sensor_channel-encoded POST into
                              sensor_service's aggregator; INTERMEDIATE/LEAF
                              send via Confirmable POST to
                              thread_manager_get_leader_address() (never a
                              hardcoded address), retrying with the
                              node_calc UUID-hash backoff schedule.

  bh1750/, sht45/, sgp40/, sgp41/, pms5003/
                                Real sensor_provider drivers (sections 4/5/10/11).
                              bh1750 and sht45 use the real ESP-IDF
                              i2c_master API against the address module_config
                              specifies (never auto-detected). sgp40/sgp41
                              compute VOC/NOx Index via the vendored,
                              unmodified Sensirion Gas Index Algorithm (see
                              components/sensirion_gas_index_algorithm/THIRD_PARTY_NOTICE.md)
                              and share components/sensirion_crc8's word CRC.
                              pms5003 is a complete, host-tested 32-byte
                              frame parser plus a registrable
                              sensor_provider_ops_t whose init() always fails
                              honestly -- ESP32-H2's two UART controllers are
                              both committed to the POGO ports, so no
                              hardware transport exists yet (see
                              docs/pogo-interface.md's PMS5003 section).

upper/, lower/                Preserved reference projects (regression
                              baseline). Unchanged except for the
                              pogo_proto wire-format additions both already
                              had to pick up (see below) -- they still build
                              and behave exactly as before.
```

## Boot sequence (`module/main/app_main.c`)

1. `nvs_flash_init()` -- the dynamic `"nvs"` partition (Thread's own dataset
   copy, plus whatever role_manager/child_service/parent_service persist).
2. `sensor_provider_register()` x N -- registers every real driver this
   image is compiled with (bh1750, sht45, sgp40, sgp41, pms5003) into
   sensor_provider's runtime registry, before it is ever looked up.
3. `module_config_load()` -- the fixed `"cfg"` partition. Any failure halts
   here (see module-config.md) -- nothing below this line ever runs on a bad
   config.
4. `module_config_load_dev_mode()` -- AUTO / DEV_ROOT / DEV_LEAF. A non-AUTO
   value is loudly logged (development compat mode, see pogo-interface.md)
   and refuses to run if it would silently activate the mock sensor in a
   production (`AUTO`) image.
5. `thread_manager_init()` -- OpenThread FTD stack up, but Thread itself not
   yet enabled.
6. `sensor_service_init()` -- initializes the one paired sensor driver
   exactly once (section 3); `sensor_transport_init()` starts the CoAP
   sample-receiving resource (needs the OT instance from step 5); the
   sample-ready callback then wires sensor_service's periodic samples into
   sensor_transport.
7. `role_manager_init()` -- starts deriving a role from port events (or is
   forced by the dev override).
8. `pogo_port_init()` x2, both routed through one dispatcher that always
   feeds `role_manager_on_port_event()`, and additionally feeds
   `child_service` (UPSTREAM events) or `parent_service` (DOWNSTREAM events).
9. `child_service_init()` / `parent_service_init()` -- always both; each only
   ever acts on its own port's events, so nothing special is needed for
   "this board doesn't have a DOWNSTREAM peer" -- parent_service simply never
   sees a DOWNSTREAM PEER_READY. `parent_service_init()` here passes a
   context wired to role_manager/child_service/sensor_service (see
   power-base.md section 7 -- power_base/'s own app_main.c wires a
   completely different context to the same component).
10. `topology_reporter_init()` -- starts reporting this module's state to the
   power base over Thread CoAP ("/t"); see power-base.md section 9.
11. `pogo_port_enable()` x2 -- but only for a port `module_config` actually
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

A second, later addition (the follow-up hardware bring-up spec's section 7,
JOIN_RESULT reliability) is the same kind of change: one new message type,
`POGO_MSG_JOIN_RESULT_ACK` (empty payload, seq matches the JOIN_RESULT it
acknowledges). `upper`/`lower` never send or expect it -- `lower_pairing.c`
still sends `JOIN_RESULT` with a hardcoded seq of 0 and never waits for an
ACK; `upper_pairing.c` still matches `JOIN_RESULT` by type only, ignoring
seq -- so this, too, is purely additive to the regression baseline. See
docs/state-machine.md's `child_service`/`parent_service` sections for the
actual retry/ACK behavior this enables in `module`.
