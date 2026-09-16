# Power base

This document describes the dedicated power-base project (`power_base/`) added
by the power-base follow-up spec, and the sensor-module (`module/`) changes
that go with it. It supersedes the older "passive power-base test jig"
described in earlier revisions of [`pogo-interface.md`](pogo-interface.md):
that jig modeled a power base with no MCU at all (BASE_ID tied to GND,
nothing else). The real product has an ESP32-H2 SuperMini in the power base
too, running its own firmware, which is what this document covers.

## 1. What changed, in one paragraph

Before this follow-up, `role_manager` made a sensor module become
`MODULE_ROLE_ROOT` the moment its UPSTREAM peer read `POGO_PEER_TYPE_POWER_BASE`
(BASE_ID low). That is no longer true. **The power base is now the system's
fixed physical POGO root and downstream provisioning coordinator.** (A
LATER session -- see section 19 -- further decoupled this fixed
application role from Thread's own `otDeviceRole`: the power base is never
described as Thread's "ROOT" or "Leader" either, since it runs as an
always-on FTD and treats CHILD/ROUTER/LEADER alike as ready.) A sensor
module's UPSTREAM peer being the power base or another sensor module is
treated identically by `role_manager`: the module is `LEAF` (nothing docked
below it) or `INTERMEDIATE` (another module docked below it) -- never
`ROOT`. See [`role_manager.c`](../components/role_manager/role_manager.c)'s
`recompute()` and its own comment on this fix.

## 2. Final architecture

```
                       power_base/ (ESP32-H2, this project)
                       - Fixed physical POGO root, Active Dataset owner
                         (otDeviceRole is Thread's own decision -- CHILD,
                         ROUTER, and LEADER are all "ready"; see section 19)
                       - CoAP "/s" (sensor data) + "/t" (topology reports)
                       - topology_registry (who is attached, at what depth)
                       - parent_service (reused, unmodified) drives
                         provisioning of whatever docks below it
                              |
                              | POGO (one DOWNSTREAM-shaped port)
                              v
                       first sensor module (module/, depth=1, role=LEAF or
                       INTERMEDIATE -- NEVER ROOT)
                              |
                              v
                       (optional deeper chain: INTERMEDIATE -> ... -> LEAF)
```

`module/` and `power_base/` are two separate ESP-IDF projects/binaries for
two different boards -- not two dev-mode images of the same binary. Both
share the same `components/` tree via `EXTRA_COMPONENT_DIRS`, same as
`upper`/`lower` already did.

### Component reuse

| Component | Sensor module (`module/`) | Power base (`power_base/`) |
|---|---|---|
| `pogo_port` | both UPSTREAM and DOWNSTREAM instances | ONE instance, wired as `POGO_PORT_DOWNSTREAM`, own peer type `POGO_PEER_TYPE_POWER_BASE` |
| `role_manager` | used (derives LEAF/INTERMEDIATE/ERROR/UNASSIGNED) | **never used at all** -- the power base's role is a hardcoded fact, not derived |
| `child_service` | used (UPSTREAM) | **never used** -- no UPSTREAM port exists |
| `parent_service` | used (DOWNSTREAM), context wired to `role_manager`/`child_service`/`sensor_service` | used UNMODIFIED, context wired to the power base's own always-ready-once-attached (CHILD/ROUTER/LEADER, not LEADER-only -- see section 19) / fixed-depth-0 / efuse-derived identity (see `parent_service.h`'s `parent_service_context_t`) |
| `thread_manager` | `thread_manager_join_with_dataset()` (INTERMEDIATE/LEAF) | `thread_manager_start_as_always_on_ftd()` (forms or resumes the ONE Active Dataset -- the neutral, non-Leader-claiming name; see section 19) |
| `module_config` / sensor drivers / `sensor_service` | used | **never used** -- see `power_base_config` instead; no sensor is faked |
| `power_base_config` | n/a | fixed board config (this project's analogue of `module_config`, no sensor fields at all) |
| `sensor_root_aggregator` | used by `sensor_service` internally (thin wrapper, unchanged public API) | used directly by `power_base_transport` (no `sensor_service`/`sensor_provider` dependency) |
| `topology_proto` / `topology_registry` | `topology_proto` only (reporter side) | both (registry lives here) |
| `topology_reporter` | used -- sends this module's own state to the power base | n/a |
| `power_base_transport` | n/a | owns the receiving CoAP resources + the registry |

No component was duplicated to make this work: `parent_service` is the same
compiled unit in both projects, `sensor_channel`/`pogo_proto`/`node_calc`/
`thread_manager` are untouched and shared, and `module_config`'s
`gpio_profile.h` (the ESP32-H2 pin-usability table) is reused by
`power_base_config` instead of being retyped.

## 3. AUTO-mode sensor-module role determination table (updated)

| UPSTREAM peer | DOWNSTREAM peer | Role |
|---|---|---|
| none / not attached | (any) | `UNASSIGNED` |
| POWER_BASE or MODULE | none attached | `LEAF` |
| POWER_BASE or MODULE | MODULE | `INTERMEDIATE` |
| either port ERROR (incl. a BASE_ID/peer_type conflict) | (any) | `ROLE_ERROR` |

`MODULE_ROLE_ROOT` is never reached through this table. It still exists as an
enum value only because `MODULE_DEV_MODE_DEV_ROOT` forces it directly (see
`role_manager_init()`) -- a pre-power-base bench-test/regression fixture kept
for `upper`'s old behavior, never mixed with the AUTO path (see
[`pogo-interface.md`](pogo-interface.md)'s "development compat mode").

The first sensor module docked on the power base is `LEAF` (or
`INTERMEDIATE` once something docks below it) at `topology_depth=1` -- it is
never ROOT, and it receives the Active Dataset from the power base exactly
like every other module receives it from its own parent.

## 4. POGO wiring: power base <-> first sensor module

The power base's one physical port is electrically identical to a sensor
module's DOWNSTREAM port -- same GPIO numbers as `module/`'s confirmed
profile (UART1, GPIO10/11, DETECT GPIO3/23). It connects to the first
sensor module's UPSTREAM port (UART0, GPIO4/5, DETECT GPIO12/2, BASE_ID
GPIO14):

**Updated by the Deep-sleep-ready GPIO follow-up (an earlier session): the
first module's UPSTREAM DETECT_IN/DETECT_OUT are SWAPPED** from the
previously documented GPIO2/GPIO12 assignment -- DETECT_IN is now GPIO12
(an ESP32-H2 RTC GPIO channel, a candidate wake source for a future
terminal-leaf's Deep-sleep ext1 wakeup -- see section 20 below) and
DETECT_OUT is now GPIO2. **A real power base + first-module pair already
wired to that old profile must have these two DETECT wires swapped before
flashing firmware built from it** -- see README.md's rewiring notice and
section 20 below; POGO 5V supply is still not implemented (see section 8),
so 5V OUT/5V IN in the table below remain aspirational, not yet wired on the
bench.

**Updated by the header-accessibility follow-up (this session): the power
base's own DETECT_OUT moved from GPIO22 to GPIO3** -- GPIO22 is not a
convenient header pin on the real ESP32-H2 SuperMini board and required
direct soldering with board-damage risk; GPIO3 is header-accessible, an
ordinary non-strapping GPIO, and used only as an open-drain DETECT_OUT
output (never a Deep-sleep wake source -- see docs/pogo-interface.md's
"History: DOWNSTREAM DETECT_OUT's GPIO -- 22, now 3" for the full
reasoning). The power base's GPIO23 (DETECT_IN) is UNCHANGED. **A real
power base already wired to the GPIO22 profile must have that one DETECT
wire moved to GPIO3 before flashing firmware built from this profile** --
`cfg`-partition-only reflashing is sufficient (no app rebuild needed), see
README.md's exact commands.

| Power base pin | Direction | First module pin |
|---|---|---|
| GPIO10 (UART TX) | ---> | GPIO5 (UART RX) |
| GPIO11 (UART RX) | <--- | GPIO4 (UART TX) |
| GPIO3 (DETECT_OUT) | ---> | GPIO12 (DETECT_IN) |
| GPIO23 (DETECT_IN) | <--- | GPIO2 (DETECT_OUT) |
| BASE_ID contact (GND, **not a GPIO** -- see below) | ---> | GPIO14 (BASE_ID) |
| GND | --- | GND |
| 5V OUT | ---> | 5V IN |

Two facts that are easy to get wrong and must not be:

- **BASE_ID is a GND contact on the power base's connector, not a GPIO the
  power base's own ESP32-H2 drives or reads.** The power base firmware never
  configures, reads, or writes any "BASE_ID GPIO" at all -- `power_base_config`
  has no such field, and `pogo_port_config_t.base_id_gpio` is always passed
  as `-1` for the power base's own port (see `power_base/main/app_main.c`).
  The first sensor module's own GPIO14 has a local 3.3V pull-up and reads
  BASE_ID as Low ONLY when it is physically docked into the power base's
  connector, where that pin meets the power base's GND contact.
- **GPIO12 (the first module's DETECT_IN, since this session's swap -- was
  GPIO2 before it) and GPIO14 (the first module's BASE_ID) are two different
  signals with two different local pull-ups.** Never tie them together, and
  never wire either one directly to GND on the module side (only the power
  base's BASE_ID contact is GND; DETECT_IN is a normal pull-up input driven
  Low by the peer's open-drain DETECT_OUT, never hard-wired to GND).

DETECT_OUT on both sides is open-drain, active-low, release-only-High (never
driven push-pull High) -- see [`pogo-interface.md`](pogo-interface.md)'s
DETECT_OUT/DETECT_IN contract, unchanged by this follow-up.
`downstream_port.detect_out_active_low` (both `module_config` and
`power_base_config`) must be `true`; `pogo_port`'s drive logic is hardcoded
to that policy regardless of what a config claims (see `module-config.md`).

Module-to-module (deeper chain) wiring, and the confirmed GPIO profile
table, are unchanged -- see [`pogo-interface.md`](pogo-interface.md).

## 5. Power base boot sequence

**Updated TWICE now -- see section 19 (module/Thread role decoupling) and
section 19.8 (attach supervisor, this correction pass) for the full
history.** Current policy: `power_base` never waits for strictly `Leader`,
and (as of section 19.8) a first attach that takes longer than any fixed
budget is NOT a fatal error -- there is no `halt_forever()` for "not
attached yet" anywhere in this firmware any more. The numbered list below
reflects the CURRENT control flow.

`power_base/main/app_main.c`'s `app_main()`:

1. `nvs_flash_init()` -- the dynamic `"nvs"` partition (OpenThread's own
   Active Dataset copy only; the power base has no role_manager/child_service
   dynamic state to persist).
2. `power_base_config_load()` -- the fixed `"cfg"` partition. `CONFIG_ERROR`
   halts here (loops forever, logs once), same policy as `module_config_load()`.
3. Power rail sequencing (`power_enable_gpio`/`power_enable_active_high`/
   `power_startup_delay_ms`) -- see section 8 below. A no-op today (no
   switching hardware decided yet).
4. `esp_efuse_mac_get_default()` -- this power base's own stable 8-byte
   identity (its EUI-64), used both as its POGO UUID (for
   `ASSIGN_PARENT_INFO`/`SET_COLLECTOR`) and its topology-registry key.
   Never regenerated, never logged in full.
5. `thread_manager_init()` -- OpenThread FTD stack up, no network yet.
6. `thread_manager_start_as_always_on_ftd()` (**this correction pass**;
   previously `thread_manager_start_as_root()` -- see section 19.8) -- **the
   only place in the whole system that ever forms or resumes an Active
   Dataset for this power base.** If NVS already holds a commissioned
   dataset, it is resumed; otherwise a new one is formed from Kconfig
   defaults. Neither the dataset bytes nor the Network Key are ever logged.
   Logs as `always-on FTD:`, never `ROOT:` (that log prefix is reserved for
   `module`'s DEV_ROOT regression fixture, which still calls
   `thread_manager_start_as_root()` -- a thin wrapper around the exact same
   underlying logic, kept only for that fixture's own unchanged logs/tests --
   see `thread_manager.h`). A failure HERE (a genuine OpenThread API error)
   halts immediately, `pogo_port_enable()` NEVER having been called.
7. **(This correction pass -- no fixed timeout, no halt)**
   `attach_supervisor_task()` is started and `app_main()` returns. This task
   polls `thread_manager_get_role()` (200ms interval, non-busy) for this
   device's FIRST attach -- `otDeviceRole` becoming `Child`, `Router`, OR
   `Leader` -- with NO timeout and NO `halt_forever()` for simply "not
   attached yet": an OTBR or another Thread device that only becomes
   reachable much later must still work. Once 30s have passed without
   attaching, ONE rate-limited `ESP_LOGW` appears (`power-base FTD: still
   not attached after ...ms ... -- not halting`), repeating at most once
   every 60s thereafter -- never a flood, never a busy loop. The instant this
   device IS observed attached, `start_services_once()` (steps 8-12 below)
   runs, and this task deletes itself -- it never runs those steps a second
   time, regardless of any LATER role change (including dropping back to
   DISABLED/DETACHED and reattaching) -- see section 19.1/19.8.
8. `power_base_transport_init()` -- registers the CoAP `"/s"` (sensor data)
   and `"/t"` (topology report) resources.
9. `parent_service_init()` -- wired to the power base's own context:
   `provisioning_allowed()` re-checks LIVE, on every call, whether
   `otDeviceRole` is currently `Child`/`Router`/`Leader` -- no caching, no
   separate watcher task, since `parent_service`'s own idle loop already
   re-checks this every iteration. `get_own_depth()==0`, `get_own_uuid()` =
   the efuse identity, `is_power_base=true`, `get_collector()` fills the
   collector endpoint (this power base's own permanent UUID + Thread
   Mesh-Local EID + CoAP port) whenever attached, and returns false
   (collector not currently valid) while DISABLED/DETACHED.
10. `pogo_port_init()` + `pogo_port_enable()` for the one DOWNSTREAM-shaped
    port -- see section 6 for why this single call is also what activates
    DETECT_OUT. A failure in this step or step 11 (a genuine resource-
    creation failure, e.g. `xTaskCreate()` failing) still halts -- this is
    NOT the same thing as "not attached yet".
11. A periodic (5s) stale-sweep task calls `power_base_transport_sweep_stale()`,
    created and CONFIRMED running before step 10's `pogo_port_enable()`.

## 6. DETECT_OUT gating

**Updated by this session's attach-supervisor follow-up (section 19.8):**
`pogo_port_enable()` (and therefore DETECT_OUT) is called from
`start_services_once()`, invoked exactly once by `attach_supervisor_task()`
the first time this device is ever observed attached -- CHILD, ROUTER, OR
LEADER (not "strictly LEADER" -- that policy was already relaxed in section
19.1, and this section previously described the OLDER strictly-LEADER wording
verbatim; both are superseded here). There is no separate "now assert
DETECT_OUT" step to sequence: `pogo_port_enable()` itself drives DETECT_OUT
active (Low) as the very last part of bringing a port up (see `pogo_port.c`'s
`set_detect_out(p, true)` call inside it), and `pogo_port_init()` alone never
touches any GPIO. Simply never calling `pogo_port_enable()` before this
device's first confirmed attach IS the "DETECT_OUT only activates once
attached" guarantee -- there is no other place in this firmware image that
can assert it. `start_services_once()` is itself only ever invoked once per
boot (guarded by `attach_supervisor_task()` deleting itself immediately
after calling it), so DETECT_OUT is also activated exactly once per boot,
regardless of how long the first attach took or how many times
`otDeviceRole` fluctuates afterward (see `tools/test_power_base_boot_sequencing.c`).

## 7. Parent UUID propagation (`POGO_MSG_ASSIGN_PARENT_INFO`)

A new, purely additive POGO message (`0x11`, see `pogo_proto/pogo_protocol.h`)
lets a parent (power base or another sensor module) tell a docking child its
own identity, so the child can name a real `parent_uuid` in its own topology
reports:

```
IDENTIFY_REQ/RESP -> ASSIGN_UUID -> ASSIGN_SLOT -> ASSIGN_PARENT_INFO
    -> SENSOR_HELLO -> POLL_CONFIG -> SET_DATASET -> JOIN_RESULT(_ACK)
```

`ASSIGN_PARENT_INFO`'s payload is 9 bytes (8-byte `parent_uuid` + 1-byte
`parent_is_power_base` flag, no multi-byte integers so no endian conversion
is needed) -- see `pogo_pack_assign_parent_info()`/`pogo_unpack_assign_parent_info()`.
`child_service` gates it exactly like every other provisioning step (its own
`CS_STATE_PARENT_INFO_SET`, strict-order NACK, `(type, seq)` dedup) and
exposes it via `child_service_get_parent_uuid()`/`_get_parent_is_power_base()`.
Existing message numbers/wire formats are unchanged; `upper`/`lower` never
send or expect this type, so it is purely additive to that regression
baseline, the same pattern `JOIN_RESULT_ACK` used before it.

`parent_service` itself no longer reaches into `role_manager`/`child_service`
directly to compute a child's depth or its own identity -- it takes a small
`parent_service_context_t` at init instead (`provisioning_allowed`,
`get_own_depth`, `get_own_uuid`, `role_name`, `is_power_base`,
`ack_timeout_ms`). `module/main/app_main.c` wires this to
`role_manager`/`child_service`/`sensor_service`; `power_base/main/app_main.c`
wires it to the power base's own hardcoded identity. Neither side pretends to
be the other.

## 8. Power rail

No power-switching GPIO has been decided on the current bench hardware.
`power_base_config.power_enable_gpio == -1` means the 5V rail is assumed
**always on** -- this firmware never claims to be able to cut real 5V power
in that case, and does not simulate doing so. If a future revision adds
switching hardware, `power_enable_gpio`/`power_enable_active_high`/
`power_startup_delay_ms` are already wired to drive it (see
`power_base/main/app_main.c`'s `apply_power_rail_sequencing()`) -- set
`power_enable_gpio >= 0` in `configs/power_base.json` and repack.

**Safety note for bench testing:** never connect the power base's own 5V OUT
and a USB 5V source to the same rail without protection. Use ONE of:
USB data-only cable while the power base's 5V is disconnected; a 5V
selector jumper/switch; or a proper power-ORing/load-switch circuit. Modules
receive 5V+GND from the power base over POGO and never have their 3.3V rails
tied together across modules.

## 9. Topology reporting

Every sensor module (never the power base itself) sends a fixed-length,
versioned binary report to the power base's CoAP `"/t"` resource, over
Thread, via `components/topology_reporter` (module side) and
`components/topology_proto`+`components/power_base_transport` (wire format +
power-base receiving side):

```
payload_version | module_uuid | parent_uuid | topology_depth | role
| sensor_type_bitmap | boot_session_generation | sequence
| upstream_connected | downstream_connected | report_type
```
(32 bytes total -- see `topology_proto.h` for the exact byte offsets.)

`report_type` is one of `ATTACHED` (first report once settled),
`UPDATED` (role, or downstream connection state, changed), `DETACHED`
(best-effort, sent from a cached snapshot since `child_service` may already
have cleared its own session state for the same detach -- see
`topology_reporter.c`'s header comment), or `HEARTBEAT` (periodic,
`CONFIG_TOPOLOGY_REPORTER_HEARTBEAT_INTERVAL_MS`, default 30s). Sent as one
Confirmable CoAP POST with the same bounded UUID-hash backoff schedule
`sensor_transport` already uses for sensor samples (1 initial + 3 retries) --
no infinite retry loop, no busy polling while not Thread-joined.

**Data-plane readiness gate (real-hardware finding, see section 18):** on real
hardware, a sensor module's first `"/s"` sample can reach the power base
before its OWN `ATTACHED` report has been accepted into `topology_registry` --
`sensor_service`'s periodic task and `topology_reporter`'s task run on
independent schedules. `components/topology_gate` closes this window; see
section 18 for the full design and section 15 for its host test.

`role` and `topology_depth` reuse `module_role_t`'s exact numeric values
(`topology_proto.h`'s `topology_role_t` mirrors them by convention, the same
pattern `sensor_channel_type_t` already uses for `module_config.sensor_type_bitmap`).

## 10. Topology registry (power base)

`components/topology_registry` is a pure, dependency-free, RAM-only,
UUID-keyed table (`TOPOLOGY_REGISTRY_MAX_NODES = 32`), owned and
mutex-protected by `components/power_base_transport`. It reconstructs
connection ORDER from each report's `topology_depth`+`parent_uuid`, never
from arrival order. `topology_registry_apply_report()` rejects, without
mutating anything:

- a module naming itself as its own parent (`ERR_SELF_PARENT`);
- `topology_depth == 0`, or `role` = `UNASSIGNED`/`ROOT` (`ERR_INVALID_DEPTH`
  -- an AUTO-mode module never has either);
- `topology_depth == 1` with a `parent_uuid` that isn't the power base's own
  (`ERR_DEPTH1_BAD_PARENT`);
- `topology_depth > 1` naming an unknown parent (`ERR_UNKNOWN_PARENT`), or
  one whose own depth+1 doesn't match (`ERR_PARENT_DEPTH_MISMATCH`);
- `topology_depth > 1` naming a KNOWN parent that is currently
  `online == false` and/or `downstream_connected == false`
  (`ERR_PARENT_OFFLINE` -- real-hardware finding: without this check, a
  child's own stale HEARTBEAT/UPDATED/ATTACHED -- sent before it notices
  its parent's link broke -- would flip its entry back online ahead of the
  parent actually reconnecting; see section 11's detach handling below and
  `tools/test_topology_registry.c`'s
  `scenario_parent_offline_rejects_children_then_recovers`);
- a topology cycle -- the claimed parent chain already contains this module
  (`ERR_CYCLE`);
- a report from an older `boot_session_generation` (`ERR_STALE_SESSION`), or
  a duplicate/reordered `sequence` within the same generation
  (`ERR_DUPLICATE_SEQ`);
- a brand-new uuid once the table is full (`ERR_FULL`).

See `tools/test_topology_registry.c` for all of the above, executed against
the real, unmodified component.

## 11. Detach / reconnect / stale handling

- **Power base's own DOWNSTREAM POGO detaches** (a direct hardware
  `DETECTED` edge, no Thread involved at all): `power_base/main/app_main.c`'s
  port dispatcher calls `power_base_transport_on_direct_downstream_detached()`,
  which marks every currently-online `depth==1` node (and its whole subtree,
  via `topology_registry_cascade_offline_children()`) offline immediately --
  no waiting for a stale timeout.
- **An intermediate module's own DOWNSTREAM detaches**: that module is still
  reachable (it can still report), so it sends an `UPDATED` report with
  `downstream_connected=false`. The registry then offlines everything BELOW
  that node (not the node itself) the moment that report is accepted.
- **A module's own UPSTREAM is lost** (it is about to go dark itself):
  `topology_reporter` attempts one best-effort `DETACHED` report from its
  last cached snapshot before Thread actually goes down. This is inherently
  unreliable (that is the nature of losing connectivity) -- it is a bonus
  signal, not the primary detach-detection mechanism.
- **Ordinary radio loss with nothing physically wrong**: never treated as an
  immediate detach. `topology_registry_sweep_stale()` (power base's own
  periodic 5s task, using `power_base_config.topology_stale_timeout_ms`)
  marks a node offline only once its last accepted report is older than that
  timeout.
- **Reconnect**: a later valid report for the SAME uuid revives the SAME
  entry (`online=true` again) -- it is never deleted, only marked offline, so
  its history (depth, parent, sensor bitmap) survives a stale/detach window.
  For a `topology_depth > 1` node, "valid" now also requires its OWN
  reported parent to currently be `online == true` AND
  `downstream_connected == true` (`ERR_PARENT_OFFLINE`, section 10) -- a
  child cannot revive itself ahead of its parent; only once the parent's own
  report is accepted first can the child's next report succeed.
- **A rejected child, while its parent stays offline**: `power_base_transport`'s
  `"/t"` handler maps `ERR_PARENT_OFFLINE` to the same CoAP `4.12
  Precondition Failed` every other rejection uses (section 14 below), and
  its `"/s"` sensor-sample handler ALSO returns `4.12` for that same
  known-but-offline uuid (unchanged, pre-existing "known+offline" response
  -- section 13). `sensor_transport` reacts to a `4.12` on a sensor sample
  exactly like the `4.04` power-base-reboot case just above: it closes
  `topology_gate` and asks `topology_reporter` to re-attach on its very next
  poll tick. While the parent stays offline that re-ATTACH itself keeps
  getting rejected as `ERR_PARENT_OFFLINE` too, so the gate simply stays
  closed and `topology_reporter` retries at its normal bounded poll-tick +
  backoff pace (never a busy loop) -- see section 18 and
  `tools/test_topology_gate.c`'s `scenario_parent_offline_recovery`. Once the
  parent's own report is accepted again, the very next child re-ATTACH
  succeeds and the gate reopens automatically, resuming sensor sending with
  no manual intervention.
- **Power base reboot**: the registry is RAM-only and starts empty. It is
  repopulated purely by modules' own ordinary `ATTACHED`/`HEARTBEAT` reports
  arriving into what looks like "an unknown uuid" -- no special "recovery
  mode" exists or is needed (see `tools/test_topology_registry.c`'s
  `scenario_base_reboot_registry_repopulated_by_heartbeat`). A module that was
  already sending sensor samples before the reboot does not just wait for its
  next scheduled `HEARTBEAT` to notice, though: `power_base_transport`'s `"/s"`
  handler still returns CoAP 4.04 Not Found for that now-forgotten uuid (an
  unchanged, pre-existing response), and `sensor_transport` reacts to that
  specific code by closing `topology_gate` and asking `topology_reporter` to
  re-attach immediately -- see section 18.

## 12. Dataset ownership

Only the power base ever calls `thread_manager_start_as_root()` (form-or-resume
an Active Dataset, become Leader). Every sensor module only ever calls
`thread_manager_join_with_dataset()` with a dataset it received via
`SET_DATASET` from its own parent -- it never forms one itself, and a failed
join never falls back to forming a new network (see
[`state-machine.md`](state-machine.md)'s error table, unchanged). A power
base reboot resumes its existing dataset from NVS rather than forming a new
one, so previously-joined modules can simply re-attach once their own POGO
chain re-establishes (still requiring a fresh POGO handshake per module, per
the existing "no auto-rejoin from stored dynamic NVS" simplification --
unchanged by this follow-up).

## 13. Sensor data reception

The power base registers the SAME `"/s"` CoAP resource `sensor_transport`
already defines for `module/`'s own DEV_ROOT/passive-power-base fixtures,
via `power_base_transport` (which unpacks with the unmodified
`sensor_channel_unpack()`, cross-checks the sample's `module_uuid` against
the topology registry, and forwards a known uuid to
`sensor_root_aggregator_record_sample()`). An unknown uuid's sample is
logged and dropped -- never stored, never a crash (see
`tools/test_topology_registry.c`'s
`scenario_sensor_data_uuid_linked_to_topology_entry`, which exercises the
exact lookup this handler performs). The power base never measures anything
itself -- it has no sensor driver at all.

## 14. Configuration

`components/power_base_config` (namespace `power_base_cfg`, partition `"cfg"`
in `power_base/partitions.csv`, distinct from and NOT shared with `module`'s
own `"cfg"` image -- different boards, different binaries): schema version,
the one downstream port's UART/GPIO/detect-polarity/debounce, an optional
`ack_timeout_ms` override for `parent_service`'s own provisioning-step
timeout (0 = the shared `CONFIG_POGO_ACK_TIMEOUT_MS` Kconfig default),
`topology_stale_timeout_ms`, `heartbeat_timeout_ms`, and the optional
`power_enable_gpio`/`power_enable_active_high`/`power_startup_delay_ms`
triple. Same CRC16 integrity scheme as `module_config` (`pogo_crc16`), same
"any failure is one `CONFIG_ERROR:` line and a halt" policy. Generate with:

```bash
source ~/.espressif/v6.1/esp-idf/export.sh
bash tools/gen_power_base_config.sh build/cfg_images
# -> build/cfg_images/power_base_config.bin (44-byte blob, verified against
#    the actual NVS image content, not just the bytes handed to the generator)
```

`tools/pack_power_base_config.py` reuses `tools/pack_module_config.py`'s GPIO
reserved-pin classification and CRC16 implementation directly (no duplicated
table) -- see that script's own docstring.

## 15. Host tests added by this follow-up

All link against real, unmodified production sources except where noted
(pogo_port/child_service/parent_service/app_main sequencing logic has no
ESP-IDF-independent form to link against, so those are mirror models of the
real control flow, the same convention every pre-existing pogo_port/
child_service/parent_service test in this repository already uses):

| File | Links against | Covers |
|---|---|---|
| `tools/test_topology_proto_pack.c` | real `topology_proto.c` | pack/unpack round trip, short/long packets, bad version, bad role/report_type |
| `tools/test_topology_registry.c` | real `topology_registry.c` + `topology_proto.c` | depth1/self-parent/cycle/depth-jump rejection, invalid role/depth, duplicate/reordered sequence + generation reset, direct-child-detach subtree offline, intermediate-downstream-detach subtree offline, stale timeout + reconnect, base-reboot repopulation, sensor-uuid<->topology linkage |
| `tools/test_power_base_peer_hello.c` | real `pogo_proto` pack/unpack (mirrors `pogo_port.c`'s validate/retry logic) | POWER_BASE<->MODULE HELLO, BASE_ID/peer_type match and conflict, silent power base |
| `tools/test_role_manager_auto_chain.c` | mirrors `role_manager.c`'s fixed `recompute()` | first module never ROOT, 1/2/3-stage chains |
| `tools/test_power_base_boot_sequencing.c` | mirrors `power_base/main/app_main.c`'s control flow | DETECT_OUT never active before Leader, activated exactly once after |
| `tools/test_direct_child_depth_and_parent_info.c` | real `pogo_proto` pack/unpack | depth=1 + power-base parent uuid assignment, `ASSIGN_PARENT_INFO` round trip |
| `tools/test_pogo_port_disable_idempotent.c` | mirrors `pogo_port.c`'s `enabled` guard | `pogo_port_disable()` repeated-call safety, DETECT_OUT release |

**Added by the subsequent code review follow-up** (see section 17):

| File | Links against | Covers |
|---|---|---|
| `tools/test_permanent_module_uuid.c` | real `pogo_proto` pack/unpack (`PROPOSE_UUID`/`ASSIGN_UUID`) | permanent efuse-derived uuid stable across 10 reprovisions/base reboot/parent change, different efuse -> different uuid, all-zero/wrong-length rejection |
| `tools/test_topology_reporter_session.c` | mirrors `topology_reporter.c`'s NVS boot-counter logic | monotonic generation across reboots, NVS-persist failure disables reporting (never "looks successful"), sequence rollover before wraparound |
| `tools/test_coap_response_semantics.c` | mirrors `power_base_transport.c`/client response handling | registry-result -> CoAP code mapping, sensor online/bitmap validation, stale-callback-after-timeout is ignored (not a crash), a rejected response is never logged "sent OK" |
| `tools/test_pogo_port_enable_rollback.c` | mirrors `pogo_port.c`'s hardened `pogo_port_enable()` | a failure at every single step fully unwinds (DETECT_OUT released, ISR removed, UART removed, `enabled=false`), a subsequent retry succeeds cleanly, double-enable rejected |
| `tools/test_power_base_boot_sequencing.c` (extended) | mirrors `power_base/main/app_main.c`'s control flow | added: reaching ROUTER (never LEADER) still halts -- ROUTER alone is no longer sufficient |
| `tools/test_topology_registry.c` (extended) | real `topology_registry.c` | session transitions no longer assume increasing generation numbers; idempotent same-seq retransmit is `OK_DUPLICATE`, not an error |

All pre-existing 17 host tests (15 C + 2 Python) plus the first round's 7
power-base tests were re-run and still pass -- see the final report for the
exact run.

**Added by the real-hardware race fix (this session, section 18):**

| File | Links against | Covers |
|---|---|---|
| `tools/test_topology_gate.c` | real `components/topology_gate/topology_gate.c` (FreeRTOS event group shadowed by `tools/host_test_stubs/freertos/`), mirrors `topology_reporter.c`'s ATTACHED/gate state machine and `sensor_transport.c`'s gate check + 4.04 reaction | no send before ATTACHED ack, send allowed after 2.xx, a failing ATTACHED keeps the gate closed and retries (bounded, no busy loop), the gate closes on detach/error/new session, power-base-reboot 4.04 -> immediate re-registration -> 2.xx -> resumed sending, DEV_ROOT/DEV_LEAF (`require_topology_registration=false`) unaffected, a duplicate/late callback never opens the gate early, sample_seq policy unaffected |

All pre-existing 27 host tests (25 C + 2 Python) were re-run and still pass
alongside this new one -- 30 total (28 C + 2 Python). See the final report for
the exact run.

## 16. What is still unverified on real hardware

**Update (this session, see section 18):** a real power base + one real SHT45
sensor module (depth=1, single hop) has since been wired and exercised.
Confirmed on that real hardware: the power base reaching `otDeviceRole=4
(LEADER)`; POGO `PEER_READY` between the two boards; the module's permanent
efuse-derived uuid being proposed/confirmed; `polling_period_ms=195` delivered
via `POLL_CONFIG`; a 110-byte Active Dataset delivered and durably stored;
the module joining Thread as `otDeviceRole=2 (CHILD)`; `JOIN_RESULT_ACK`;
a topology report applied at the power base; and an SHT45 2-channel sample
recorded in `sensor_root_aggregator`. Everything below that is not covered by
that specific run is still unverified exactly as before.

That same run also surfaced a real startup race (a sensor sample reaching
`"/s"` before this module's own topology `ATTACHED` report had been applied,
rejected once as CoAP 4.04 -- see section 18) -- fixed in this session via
`components/topology_gate`, but **the fix itself has not yet been reflashed
and re-verified on real hardware**; only host tests + clean builds cover it
so far. This is the single most important thing to reflash and re-check next.

In particular, still needing real hardware:

- **The `topology_gate` fix from section 18 itself** -- reflash both boards
  and confirm: no sensor sample before the ATTACHED ack; the CoAP-4.04
  power-base-reboot recovery path re-registers within one poll tick, not a
  30s heartbeat wait; DEV_ROOT/DEV_LEAF regression images are unaffected.
- The actual POGO wiring table in section 4 above, on a BH1750 sensor module
  (only SHT45 has been tried) and on chains of more than one sensor module.
- `pogo_port`'s BASE_ID/PEER_HELLO cross-check with more than one sensor
  module attached (only a single depth=1 module has been tried against the
  real power-base MCU so far).
- `thread_manager_start_as_always_on_ftd()`'s dataset resume path (this
  correction pass's name for what was `thread_manager_start_as_root()` when
  this bullet was first written) across a REAL power cycle of the power-base
  board.
- CoAP delivery of both `"/s"` and `"/t"` over a real depth>=2 POGO chain
  (only a single depth=1 hop has been tried), including the UUID-hash backoff
  schedule's real timing. Note: even a passing depth=2 POGO chain test only
  confirms POGO physical/application-layer topology (docking order, depth
  propagation, provisioning, CoAP reachability) -- it does NOT by itself
  prove the CoAP packet actually radio-hopped through the INTERMEDIATE
  module's Thread router role (genuine Thread-mesh multi-hop is a distinct,
  separately-unverified property; see docs/validation-status.md's "POGO
  physical topology depth" vs. "Thread radio multi-hop routing" sections).
- The unbounded, no-halt first-attach wait (`attach_supervisor_task()`,
  section 19.8) under adverse real radio conditions or a genuinely absent
  Thread network for an extended period (the one run so far reached LEADER
  quickly, before this correction pass even existed).
- Any of section 11's detach/reconnect/stale behaviors under a real physical
  detach (as opposed to the host-tested pure registry logic) -- the power
  base reboot / registry-loss path specifically is section 18's concern.

See the final report's "실물 검증 절차" for the exact flash order and
pass/fail criteria to run next.

## 17. Code review follow-up (this session)

A subsequent code review pass found and fixed seven issues in the
implementation above. None of these change the architecture described in
sections 1-16; they correct implementation defects within it. Real hardware
is still NOT wired -- everything below is verified at the source/build/
host-test level only, same as the rest of this document.

1. **Permanent module UUID.** `parent_service.c` used to assign each child a
   fresh `esp_fill_random()` uuid every provisioning session -- a
   permanently-paired sensor would register as a "new" device every time it
   redocked. Fixed with two new, purely additive POGO messages:
   `POGO_MSG_REQUEST_UUID` (parent -> child, empty payload) and
   `POGO_MSG_PROPOSE_UUID` (child -> parent, its own efuse EUI-64, 8 bytes --
   see `child_service.c`'s `compute_permanent_uuid()`). `POGO_MSG_ASSIGN_UUID`
   keeps its exact existing wire format but is now a CONFIRMATION: the
   parent echoes back exactly what the child proposed, and the child (via
   `handle_assign_uuid()`) rejects (`POGO_NACK_UUID_MISMATCH`) anything else
   instead of accepting it. An all-zero efuse read is never treated as a
   valid identifier, on either side. See `tools/test_permanent_module_uuid.c`.

2. **Session generation, not a random number the registry can't reason
   about.** `topology_reporter_init()` now derives `boot_session_generation`
   from an NVS-persisted, monotonically-incrementing-per-boot counter
   (`load_next_boot_generation()`) instead of `esp_random()`, and rolls the
   local sequence counter over to a fresh persisted generation before it
   could ever wrap (`maybe_roll_over_sequence()`). If persisting a
   generation ever fails, reporting is disabled for the rest of that boot --
   it never sends with an un-persisted number. Independently,
   `topology_registry_apply_report()` no longer assumes generation numbers
   increase: it now tracks each node's `superseded_generation` (the session
   just left) and rejects only a delayed packet naming THAT exact one,
   accepting any other different generation as a legitimate new session --
   see `topology_registry.h`'s top comment for the documented, bounded limit
   this implies (only the immediately-preceding session is guarded, not
   arbitrarily old ones). See `tools/test_topology_reporter_session.c` and
   `tools/test_topology_registry.c`'s session-transition scenarios.

3. **CoAP response codes actually mean something, and clients check them.**
   `power_base_transport.c` used to ACK every `"/s"`/`"/t"` request with
   `2.04 Changed` regardless of outcome. It now maps
   `topology_registry_result_t` (and the sensor-sample validation added by
   item 4) to a real CoAP code -- see `power_base_transport.h`'s top comment
   for the exact table. `topology_reporter.c` and `sensor_transport.c`'s
   send paths now check the ACTUAL response code (not just the OpenThread
   transport result) before logging "sent OK". Both components' CoAP client
   callback context is no longer a per-call stack-allocated
   `SemaphoreHandle_t` (which a late callback -- one that fires after the
   waiter already timed out and returned -- could touch after it was
   deleted); it is now one persistent semaphore plus a monotonically
   incrementing token, so a stale callback recognizes it is stale and does
   nothing. See `tools/test_coap_response_semantics.c`.

4. **Sensor data checked against the topology registry, not just "is the
   uuid known".** `power_base_transport.c`'s `"/s"` handler now also
   requires the reporting node to be `online`, and its `sensor_type_bitmap`
   to be a subset of (or equal to) that node's own last-reported bitmap --
   an offline node's data, or a bitmap claiming an unconfigured channel, is
   rejected (and counted via the registry's new
   `topology_registry_record_sensor_error()`), not silently stored. See
   `tools/test_coap_response_semantics.c`.

5. **Power base gates strictly on LEADER (this pass) -- SUPERSEDED by
   section 19.** At the time of this code-review pass, `pb_provisioning_allowed()`
   and the boot-time role-wait in `power_base/main/app_main.c` were changed
   from accepting `LEADER || ROUTER` to strictly `LEADER`, reasoning that
   ROUTER was never a correct steady state for a node that is the sole
   former/resumer of this network's dataset. **A later session (section 19)
   revisited this and relaxed it again -- to `CHILD || ROUTER || LEADER`,
   the power base's fixed application role never actually depending on
   Thread Leadership in the first place.** Every log line naming an
   `otDeviceRole` (here, `child_service.c`, `parent_service.c`) still prints
   both the numeric value and a name via the shared
   `thread_manager_role_to_str()`, unaffected by that later change. See
   `tools/test_power_base_boot_sequencing.c` (rewritten in section 19's
   session for the new CHILD/ROUTER/LEADER-all-sufficient policy).

6. **Initialization failure/rollback hardening.** `pogo_port_enable()` now
   checks every `gpio_config`/`gpio_set_level`/`gpio_isr_handler_add`/
   `xTaskCreate` return value and fully unwinds (DETECT_OUT released, any
   installed ISR removed, the UART driver removed, `enabled` left false) on
   any failure, so a retry always starts clean (see
   `tools/test_pogo_port_enable_rollback.c`); calling it twice without an
   intervening `pogo_port_disable()` is now rejected outright.
   `topology_reporter_init()` cleans up its queue/semaphore if task creation
   fails, instead of leaving them allocated with nothing consuming them.
   `topology_reporter_on_port_event()`'s `xQueueSend()` failure is now
   rate-limited-logged AND self-healing -- `topology_reporter_task()` polls
   `pogo_port_is_attached()`/`pogo_port_get_peer_type()` as ground truth every
   tick regardless of the event queue, so a dropped event desyncs
   `downstream_connected` for at most one poll tick, never permanently.
   `power_base/main/app_main.c`'s stale-sweep task creation failure is now
   logged loudly and reflected in the final boot-ready log line instead of
   being silently absorbed into an otherwise-normal boot.

7. **Logs an operator can actually see.** `topology_reporter.c` and
   `power_base_transport.c` now log ATTACHED/UPDATED/DETACHED at `ESP_LOGI`
   (was `ESP_LOGD`, invisible on a default INFO-level monitor); routine
   HEARTBEAT successes stay at `ESP_LOGD` so a healthy, unchanging chain
   does not flood the log.

All four projects (`module`, `power_base`, `upper`, `lower`) were rebuilt
from a clean `fullclean` after these fixes and still link with zero
warnings -- see the final report for the exact build/test results.

## 18. Real-hardware validation and the first-sample/topology race fix (this session)

**Real-hardware result.** A power base + one SHT45 sensor module (depth=1,
single hop) has been wired and exercised on real hardware. Confirmed:
`power_base` reaching `otDeviceRole=4 (LEADER)`; POGO `PEER_READY` between the
two boards; the module's permanent efuse-derived uuid proposed and confirmed;
`polling_period_ms=195` delivered via `POLL_CONFIG`; a 110-byte Active Dataset
delivered and durably stored; the module joining Thread as `otDeviceRole=2
(CHILD)`; `JOIN_RESULT_ACK`; a topology report applied at the power base
(`role=3`, LEAF); and an SHT45 2-channel sample recorded in
`sensor_root_aggregator`. This is the first real-hardware confirmation of the
whole chain described in sections 1-17.

**The race.** The same run also showed this sequence, right after boot:

```text
power_base_transport: sensor sample from unknown module uuid=..d357 -- dropped, not stored
sensor_transport: sensor sample rejected by receiver: CoAP 4.04
power_base_transport: topology report applied: uuid=..d357 ...
sensor_root_aggregator: sample recorded: uuid=..d357 ...
```

`sensor_service`'s periodic sampling task starts measuring the instant it has
a polling period, and its sample-ready callback (`sensor_transport_send_sample()`)
was previously gated ONLY on Thread-joined state (`sensor_service_set_thread_joined()`,
flipped by `child_service` the moment `otDeviceRole` settles). `topology_reporter`
runs its own, independent `TOPOLOGY_REPORTER_POLL_TICK_MS` (500ms) poll loop and
only notices `role_manager_is_upstream_ready()` becoming true on its next tick.
On real hardware, this module's first `"/s"` sample can therefore reach the
power base before its own `ATTACHED` report has been applied to
`topology_registry` -- an unknown-uuid rejection (CoAP 4.04, section 13's
existing, unchanged semantics), recovered a moment later once the `ATTACHED`
report itself succeeds. One sample lost, not data corruption, but a policy
violation of "no `"/s"` before this module is actually registered".

**The fix: `components/topology_gate`.** A small, dependency-free (one
FreeRTOS event group) component holding two bits: whether this module is
CURRENTLY registered (data-plane gate), and whether a re-registration has been
requested. It is the only thing `topology_reporter` (the sole writer of the
first bit) and `sensor_transport` (the reader of the first bit, and the writer
of the second) share -- neither gains a direct dependency on the other, so no
component-dependency cycle is introduced (`topology_reporter` already depends
on `sensor_service`; `sensor_transport` already depends on `sensor_service`
too; both simply gain a one-way dependency on this one leaf component).

- **Where the gate opens:** `topology_reporter_task()`'s `settled &&
  !has_attached` branch now only sets `has_attached = true` (and calls
  `topology_gate_open()`) when `send_live_report(TOPOLOGY_REPORT_ATTACHED)`
  itself returns true -- i.e. a real CoAP 2.xx response, unchanged from the
  existing "2.xx only" success check `send_one_attempt()` already enforced
  (code review follow-up section 3, section 17 item 3 above). Previously
  `has_attached` was set unconditionally, before the send even happened; a
  failed ATTACHED attempt is now retried on the next poll tick instead of
  silently giving up on ever registering until the next heartbeat.
- **Where it closes:** the same task's `!settled && has_attached` branch
  (upstream DETACHED/ERROR, role settling to UNASSIGNED/ROLE_ERROR, this
  module's own Thread membership lost, or a fresh provisioning session
  starting -- `role_manager_is_upstream_ready()`/`role_manager_get_role()`
  fold all of these into the one `settled` boolean this branch already
  checked), within at most one `TOPOLOGY_REPORTER_POLL_TICK_MS` -- the same
  bound `resync_downstream_connected()` already documents for a dropped port
  event, not a new synchronous cross-task guarantee.
- **`sensor_transport_send_sample()`** now takes a `require_topology_registration`
  bool (from `sensor_transport_init()`, true only for the AUTO path against an
  active power base) and, when true, refuses to even attempt a `"/s"` send
  while `topology_gate_is_open()` is false -- not a warning every sample
  period, an `ESP_LOGD` (section 4 of this session's task: "매 주기마다
  경고를 출력하지 않는다"). `sensor_service`'s own measurement, `sample_seq`
  increment, and local state updates are all untouched -- only the wireless
  hand-off is skipped, exactly like the existing Thread-joined gate.
- **Power base reboot recovery:** its `topology_registry` is RAM-only (section
  10), so a reboot can make it reject an already-registered module's sample as
  CoAP 4.04 Not Found -- the ONLY meaning that code has for `"/s"` (section
  13, unchanged). `sensor_transport`'s `send_one_attempt()` reacts to that
  specific code (when `require_topology_registration` is true) by calling
  `topology_gate_close()` and `topology_gate_request_reregister()` right away.
  `topology_reporter_task()` checks (and atomically clears)
  `topology_gate_take_reregister_request()` every poll tick; when set, it
  forces `has_attached = false`, so the very same tick's `settled &&
  !has_attached` branch retries `ATTACHED` immediately -- bounded by one poll
  tick, not the up to `CONFIG_TOPOLOGY_REPORTER_HEARTBEAT_INTERVAL_MS` (30s
  default) a plain heartbeat schedule would otherwise impose.
- **DEV_ROOT/DEV_LEAF regression is unaffected.** `require_topology_registration`
  is false for that module-to-module path (`module/main/app_main.c` passes
  `dev_mode == MODULE_DEV_MODE_AUTO`): DEV_ROOT's peer never runs
  `power_base_transport` and so has no `"/t"` resource at all, meaning its
  ATTACHED reports could never succeed -- gating `"/s"` sending on that would
  have permanently blocked DEV_LEAF's sensor data. `topology_reporter` still
  runs its ATTACHED-retry loop uniformly for every mode (harmless -- a bounded,
  rate-limited-logged retry against a peer with no `"/t"` resource), but
  `sensor_transport` simply never consults the gate in that mode. `upper`/
  `lower` never call `sensor_transport_init()` at all and are untouched.
- **No shared semaphores/callback state.** This gate never touches
  `topology_reporter.c`'s or `sensor_transport.c`'s own token-guarded async
  CoAP response-handler state (section 17 item 3) -- it only ever reads
  `send_live_report()`'s/`send_one_attempt()`'s already-finished boolean
  result or a CoAP response code, after those functions' own stale-callback
  protection has already resolved.

See `components/topology_gate/include/topology_gate/topology_gate.h` for the
full design comment and `tools/test_topology_gate.c` for the host test
(section 15). **This fix itself has not yet been reflashed and re-verified on
real hardware** -- see section 16.

## 19. Module/Thread role decoupling and collector provisioning (this session)

This session's task had five parts: decouple the power base's fixed
application role from Thread's own `otDeviceRole`; remove the sensor/
topology data plane's dependency on "the current Thread Leader"; introduce
a POGO-delivered collector endpoint (in place of that Leader-RLOC
dependency); swap the UPSTREAM DETECT_IN/DETECT_OUT GPIO assignment for a
future Deep-sleep wake path; and lay only the groundwork for a future Linux
OTBR. **None of Linux OTBR, RCP firmware, a Matter Bridge, MCU Deep-sleep, or
POGO 5V supply were implemented this session** -- see subsection 19.6 below
for exactly what groundwork exists and what does not.

### 19.1 Fixed application role vs. dynamic Thread role

The power base's fixed, permanent application role -- the physical root of
the POGO chain, the downstream provisioning coordinator, the UUID/slot/
topology registry owner, the sensor-data aggregator, and (in a future
revision) the POGO 5V supplier and the gateway in front of a Linux Matter
Bridge -- no longer requires or claims Thread Leadership. `power_base`
always runs as an always-on FTD (`rx-on-when-idle=true`,
`router-eligible=true` -- unchanged, `thread_manager_start_as_root()` always
applied this link mode). `otDeviceRole` is entirely Thread's own decision;
`CHILD`, `ROUTER`, and `LEADER` are all treated as "attached and ready"
identically -- only `DISABLED`/`DETACHED` are "not ready". Every log line and
doc passage claiming "the power base is Thread's one and only Leader" or
"ROOT is always Leader" describes the OLD (pre-this-session) policy and has
been superseded (see sections 5 and 17 item 5 above, and README.md).

Sensor modules are completely unaffected by this: `role_manager`'s
`MODULE_ROLE_ROOT`/`INTERMEDIATE`/`LEAF`/`UNASSIGNED`/`ROLE_ERROR` state
machine (section 1-3 above) is a POGO/role_manager-layer concept, unrelated
to `otDeviceRole`, and nothing about it changed this session.

`power_base/main/app_main.c`'s boot sequence (section 5) now waits, once,
for this device's FIRST attach as ANY of CHILD/ROUTER/LEADER, then runs its
fixed one-time init (CoAP resources, `parent_service_init()`,
`pogo_port_init()`+`enable()`, the stale-sweep task) EXACTLY ONCE. None of
that is ever re-created for a LATER role change: `pb_provisioning_allowed()`
and `pb_get_collector()` are both pure, stateless functions of the CURRENT
`otDeviceRole`, re-evaluated fresh on every call (`parent_service`'s own
idle loop already polls `provisioning_allowed()` every iteration -- no new
watcher task was needed). Concretely:

- A drop to DISABLED/DETACHED makes `pb_provisioning_allowed()` return
  false (no NEW downstream provisioning session starts) and
  `pb_get_collector()` return false (collector not currently valid) on
  their very next call -- no explicit "pause" step.
- An already-JOINED child's Thread membership is untouched by the power
  base's own role fluctuation; only a session that happens to be mid-flight
  at that moment (e.g. about to request SET_COLLECTOR) is abandoned by
  `parent_service`, the same as any other failed provisioning step, and
  naturally retried once the peer redocks or the power base reattaches.
- A reattach (role becomes CHILD/ROUTER/LEADER again) makes both callbacks
  return true/valid again on their very next call -- no explicit "resume"
  step, and no busy-retry loop (the existing bounded, non-busy polling
  `parent_service_task()` already used is unchanged).

See `tools/test_power_base_boot_sequencing.c` (rewritten this session) for
the host-tested model of all of the above.

### 19.2 Collector provisioning replaces the Leader-RLOC dependency

`sensor_transport.c` and `topology_reporter.c` (module side) used to send
every `"/s"`/`"/t"` request to `thread_manager_get_leader_address()` --
"the current Leader's RLOC", which happened to always be the power base in
the old strictly-Leader topology. Both now send to
`components/collector_endpoint`'s current snapshot instead: a small,
FreeRTOS-task-safe, per-module cache of the power base's permanent UUID +
Thread Mesh-Local EID + CoAP port, delivered via a new POGO message
(`POGO_MSG_SET_COLLECTOR`, `0x14`) rather than derived from Thread role at
all. Neither component depends on `thread_manager_get_leader_address()` any
more (that API itself is untouched, left in `components/thread_manager` for
any other diagnostic use); see `tools/test_no_leader_rloc_dependency.py`.

This means a sensor module's data destination is now completely independent
of the power base's OWN Thread role: it stays fixed at whatever Mesh-Local
EID SET_COLLECTOR delivered, and Thread's own mesh routing (via RLOCs)
handles multi-hop delivery to that address transparently regardless of
which node currently holds Leadership.

`components/collector_endpoint` (new leaf component, module-side only --
the power base does not need one, since it computes its own identity
directly): `init`/`set`/`clear`/`get` (a full-struct snapshot by value)/
`is_valid`. `set()` independently validates its inputs (rejects the
unspecified/multicast/link-local address, port 0, or an all-zero
`power_base_uuid`) even though its only real caller (`child_service.c`)
already validated the same fields via `pogo_unpack_set_collector()`/
`pogo_collector_endpoint_is_valid()` -- defense in depth, not redundant
trust. See `tools/test_collector_endpoint.c`.

`thread_manager_get_mesh_local_eid()` (new) wraps
`otThreadGetMeshLocalEid()` -- confirmed against the real
`components/openthread/openthread/include/openthread/thread.h` shipped with
ESP-IDF v6.1 (`const otIp6Address *otThreadGetMeshLocalEid(otInstance
*aInstance)`), not assumed from memory. Its returned pointer is into
OpenThread's own internal state and is only valid while the OpenThread API
lock is held, so the wrapper copies the 16 bytes out before releasing the
lock, exactly like `thread_manager_get_active_dataset_tlvs()` already does
for a different OpenThread getter.

### 19.3 Wire format: `POGO_MSG_SET_COLLECTOR`

Purely additive (existing message types/wire formats unchanged) -- 30 bytes:

```
power_base_uuid(8) | addr(16, IPv6, network byte order) | port(2, big-endian) | generation(4, big-endian)
```

`addr` is the power base's own Mesh-Local EID; `port` is the CoAP port
`power_base_transport_init()` bound (`OT_DEFAULT_COAP_PORT`); `generation`
is an informational provisioning epoch (a random-per-boot value on the
power base side today -- not read back or relied on for any wire-level
decision, carried only so a future revision could use it). `pogo_pack_set_collector()`/
`pogo_unpack_set_collector()` live in `components/pogo_proto` alongside
every other message codec; `pogo_collector_endpoint_is_valid()` (and the
lower-level `pogo_ipv6_addr_is_valid_collector()`) are the shared
length/byte-order/address/port validators both the wire layer and
`components/collector_endpoint`'s own independent `set()`-time check use.
See `tools/test_collector_provisioning_pack.c`.

### 19.4 Provisioning sequence and NVS storage

`child_service.c`'s (module UPSTREAM) state sequence gained one state,
`CS_STATE_COLLECTOR_SET`, between `POLL_SET` and `JOINING`:

```
... -> SENSOR_DONE -> POLL_SET -> COLLECTOR_SET -> JOINING (SET_DATASET) -> JOINED
```

`SET_DATASET` now requires `CS_STATE_COLLECTOR_SET` (was `CS_STATE_POLL_SET`)
-- a `SET_DATASET` arriving before `SET_COLLECTOR` has succeeded is NACKed
`POGO_NACK_BAD_STATE`, so the Thread join (and this module's own periodic
measurement/reporting once joined) can never start before it knows where
its data should go. A structurally invalid (wrong length) `SET_COLLECTOR`
payload is also `POGO_NACK_BAD_STATE`; a well-formed but semantically bad
endpoint (bad address/port/uuid) is the more specific
`POGO_NACK_INVALID_COLLECTOR`; an NVS write failure is
`POGO_NACK_STORAGE_FAILED` and leaves the state machine at `POLL_SET` (the
Thread join never starts) -- the same three-way split `SET_DATASET` itself
already used for invalid-dataset/storage-failure.

The collector endpoint is durably written into the SAME runtime `"nvs"` NVS
namespace (`child_dyn`, alongside the existing dataset TLV record) the
dynamic Active Dataset already uses -- never the fixed `"cfg"` partition.
Like the dataset record, it is diagnostic/forward-compatibility only and is
never read back at boot (the same "no auto-rejoin from stored dynamic NVS"
simplification `docs/state-machine.md` already documents for the dataset
applies identically here: a changed upstream parent must not be trusted
silently, so a fresh, live `SET_COLLECTOR` is always required before
`components/collector_endpoint`'s in-RAM copy is ever populated).

`clear_provisioning_temporaries()` (shared by `reset_session()` on
ATTACHED/DETACHED and `begin_new_provisioning_session()` on PEER_READY/a
fresh IDENTIFY_REQ) now also calls `collector_endpoint_clear()`, and the
`POGO_PORT_EVENT_ERROR` case in `child_service_task()`'s switch (which does
not route through either of those) gained its own explicit
`collector_endpoint_clear()` call. Net effect: a new provisioning session,
a physical detach, or an UPSTREAM port error all immediately invalidate
whatever collector endpoint the OLD session had. See
`tools/test_collector_provisioning_sequence.c`.

`power_base`'s own `parent_service_context_t.get_collector` (`pb_get_collector()`)
fills this power base's own Mesh-Local EID + CoAP port + permanent UUID
directly on every call -- it has no `components/collector_endpoint` of its
own (it IS the collector, never a forwarder). An INTERMEDIATE module's
`module_parent_get_collector()` forwards the EXACT endpoint it itself
received from its own upstream (`components/collector_endpoint`'s
snapshot), completely unmodified -- it never substitutes its own address.
The `MODULE_DEV_MODE_DEV_ROOT` module-to-module regression fixture (which
has no power base at all) is the one exception: when `role_manager_get_role()
== MODULE_ROLE_ROOT`, `module_parent_get_collector()` fills the endpoint
from THIS module's own Mesh-Local EID instead, mirroring
`pb_get_collector()` -- this is what lets `DEV_LEAF`'s `sensor_transport`
keep sending its `"/s"` samples to `DEV_ROOT`, the same destination as
before this session (previously "the current Leader", always `DEV_ROOT`
itself in that topology).

### 19.5 Capability negotiation

Since the new provisioning step is mandatory on both sides of every POGO
link once both `module`/`power_base` are rebuilt from this revision,
`pogo_peer_hello_t` (the PEER_HELLO_REQ/RESP payload) gained a third byte,
`capabilities` (a bitmask; `POGO_CAP_COLLECTOR_PROVISIONING` is the only bit
defined so far). `pogo_port.c`'s `validate_incoming_hello()` now also
requires the peer's declared `capabilities` include
`POGO_CAP_COLLECTOR_PROVISIONING`; a peer that predates this field entirely
(a legacy, still-2-byte HELLO payload -- `pogo_unpack_peer_hello()` now
requires exactly 3 bytes and fails cleanly on 2) or that declares
`capabilities=0` is rejected loudly and explicitly at the handshake with a
new error, `POGO_PORT_ERR_CAPABILITY_MISMATCH` -- never auto-retried (same
policy as `PEER_TYPE_CONFLICT`/`PROTOCOL_VERSION_ERROR`: this means
something is actually wrong, not a timing issue). This is deliberately a
capability bit inside the HELLO PAYLOAD, not a bump of `POGO_FRAME_VERSION`
(the raw frame-level sync byte): bumping the frame version would make
`pogo_frame_parser_feed()` silently drop EVERY frame from an old peer at the
byte level (no diagnosable rejection at all, not even at the handshake),
whereas this design gives a clear, logged, handshake-stage rejection while
leaving frame-level backward parseability alone. `upper`/`lower` never use
`pogo_port`'s PEER_HELLO handshake at all (they hand-roll their own
provisioning directly over `pogo_transport`), so they are completely
unaffected either way -- see `tools/test_peer_hello_handshake.c` and
`tools/test_power_base_peer_hello.c`'s new capability-mismatch scenarios.

### 19.6 GPIO swap and Deep-sleep groundwork (implemented vs. not)

UPSTREAM `DETECT_IN`/`DETECT_OUT` are swapped (GPIO12/GPIO2, was
GPIO2/GPIO12) -- see section 4 above and README.md's rewiring notice.
GPIO12 is an ESP32-H2 RTC GPIO channel (`RTCIO_GPIO12_CHANNEL`), a candidate
`esp_sleep_enable_ext1_wakeup_io()` Deep-sleep wake source for a future
terminal-leaf module waking on its upstream parent's DETECT_IN edge; GPIO2
is not. This is verified two ways: a host-testable restatement
(`components/module_config/deep_sleep_gpio.h`,
`deep_sleep_wake_gpio_is_capable()`, `tools/test_deep_sleep_gpio.c`) of
ESP-IDF v6.1's own RTC GPIO channel range (GPIO7..GPIO14, per
`soc/esp32h2/include/soc/rtc_io_channel.h`), and a compile-time
`_Static_assert` in `components/module_config/gpio_profile.c` (built for
every real ESP32-H2 target) against the REAL `RTCIO_GPIO12_CHANNEL` macro,
so a future ESP-IDF revision that removed/renumbered it would fail the
build loudly rather than silently invalidate this assumption.

**Explicitly NOT implemented this session** (all deferred to a future
revision, per this session's own scope limits):

- Deep-sleep entry/exit itself -- no `esp_deep_sleep_start()`/wake-source
  configuration/RTC-memory retention anywhere in this firmware.
- Linux OTBR installation, RCP firmware, or any code targeting it.
- A Matter Bridge.
- MCU Deep-sleep for the terminal leaf (or Light-sleep for short polling
  periods via Thread SED).
- POGO 5V supply -- both dev boards still use independent USB power; only
  signal lines + GND are wired.

The FUTURE conditions this GPIO choice is meant to support (documented
here, not implemented): only a terminal LEAF (nothing docked below it) may
ever Deep-sleep -- an INTERMEDIATE module is always an always-on FTD (it
must keep routing for whatever is docked below it) and must never
Deep-sleep. A short polling period should use Thread SED (`mRxOnWhenIdle=false`,
already what `thread_manager_join_with_dataset(THREAD_MANAGER_ROLE_LEAF, ...)`
applies today) plus Light-sleep, not Deep-sleep, since Deep-sleep's RAM loss
would require a full re-join on every wake. Before any future Deep-sleep
entry: no unsent sensor sample, no in-flight CoAP callback, no pending UART
transaction, and no sensor measurement may be outstanding. Wake sources:
a timer PLUS the upstream parent's DETECT_IN edge (GPIO12) via
`esp_sleep_enable_ext1_wakeup_io()`; a failed/unavailable Deep-sleep
condition should fall back to Light-sleep rather than staying fully awake.

### 19.7 What is still unverified on real hardware (this session's changes)

Everything in sections 1-18's "still unverified" lists remains exactly as
unverified as before. Additionally, none of this session's changes have
been flashed to real hardware yet:

- The relaxed CHILD/ROUTER/LEADER attach policy (section 19.1) -- only
  host-tested (`tools/test_power_base_boot_sequencing.c`) and clean-built so
  far.
- `POGO_MSG_SET_COLLECTOR` end-to-end over a real POGO link (power base ->
  first module, and first module -> a second, INTERMEDIATE-forwarded hop) --
  only host-tested (wire format + sequencing) and clean-built.
- Capability-negotiation rejection of a genuinely mismatched real peer (both
  real boards in this project are always reflashed together in practice, so
  this scenario has not been exercised with two ACTUALLY different firmware
  revisions on real hardware).
- **The swapped UPSTREAM DETECT_IN/DETECT_OUT GPIO assignment requires
  physically re-wiring the two DETECT wires on any board previously wired to
  the old profile before it is flashed with firmware built from this
  session** -- see README.md for the exact rewiring table and flash/monitor
  commands to use afterward.
- `thread_manager_get_mesh_local_eid()` returning a real, on-mesh-reachable
  address on actual hardware (confirmed only against the ESP-IDF v6.1 header
  declaration and OpenThread's documented semantics, not yet exercised on a
  real radio).

## 19.8 Correction pass: neutral Thread API and no permanent halt on a slow first attach (this session)

A follow-up correction pass to section 19 found two remaining problems with
that session's own work, plus several stale docs/log strings it left behind
(sections 4-6, 16, 17 above were all corrected in place by this pass -- see
each for exactly what changed). Neither problem was a regression in
already-verified real-hardware behavior; both were within section 19's own,
still-real-hardware-unverified changes.

**Problem 1: `power_base` still called `thread_manager_start_as_root()` and
logged `ROOT:`.** Despite section 19.1's stated intent ("no longer described
... as Thread's ROOT or Leader"), the actual boot code still called the
ROOT-named API and inherited its `ROOT:` log prefix. Fixed with a genuinely
neutral API, `thread_manager_start_as_always_on_ftd()` (identical underlying
behavior -- same link mode, same resume-or-form logic -- see
`thread_manager.h`'s comment on both names) that `power_base` now calls
instead, logging `always-on FTD:`. `thread_manager_start_as_root()` itself
is UNCHANGED (same name, same log text, same behavior) and remains in place
purely for `module`'s `MODULE_DEV_MODE_DEV_ROOT` regression fixture, which
this correction pass deliberately left untouched (see
`docs/pogo-interface.md`'s "development compat mode").

**Problem 2: a first attach that legitimately takes longer than 30s
permanently halted this power base.** The original section 19 boot-sequence
wait was bounded to 30s and called `halt_forever()` on expiry -- worded as
"only DISABLED/DETACHED for the whole window is a genuine failure now", but
in practice this makes a REAL, expected scenario (an OTBR not yet up, or a
Thread network this power base needs to join not yet reachable) fatal. This
power base's fixed application role does not actually require attaching
within any particular time budget -- it should simply keep trying and start
service the moment it succeeds, however long that takes. Fixed with
`attach_supervisor_task()` (`power_base/main/app_main.c`): a dedicated task,
started once from `app_main()`, that polls `thread_manager_get_role()` (200ms
interval, non-busy) with NO timeout and NO halt for "not attached yet". Once
30s pass without attaching, ONE rate-limited `ESP_LOGW` appears
(`ATTACH_WARN_FIRST_MS`), repeating at most once every 60s thereafter
(`ATTACH_WARN_REPEAT_MS`) -- visible to an operator without flooding the
log. The moment this device IS observed attached (CHILD, ROUTER, or LEADER),
`start_services_once()` runs (the exact same steps 8-12 from section 5,
unchanged) and the supervisor task deletes itself -- it never runs those
steps again, including across a LATER drop to DISABLED/DETACHED and
reattach (which needs no supervisor involvement at all: `pb_provisioning_allowed()`/
`pb_get_collector()` already read `otDeviceRole` live on every call, see
section 19.1). `start_services_once()` only touches file-scope `static`
state (`s_cfg`/`s_own_uuid`/`s_collector_generation`/`s_parent_ctx`), never a
pointer to one of `app_main()`'s own stack locals, so it remains valid for as
long as the supervisor task runs, independent of `app_main()` itself
returning (which it now does, immediately after starting the supervisor
task). A genuine resource-creation failure INSIDE `start_services_once()`
(CoAP resource registration, `parent_service_init()`, `pogo_port_init()`/
`enable()`, the stale-sweep task's own `xTaskCreate()`) still halts -- that
is a real, distinct failure class from "not attached yet", and this
correction pass did not relax it. Likewise, `thread_manager_start_as_always_on_ftd()`
itself failing (a genuine OpenThread API error, checked in `app_main()`
before the supervisor task is even started) still halts immediately.

**Docs/log string audit.** This pass searched the whole repository (not
just `power_base/`) for now-stale current-tense claims left over from
section 19's own work and an earlier GPIO-swap follow-up, and corrected
every one found outside of clearly-marked historical/superseded passages:
`README.md`'s GPIO tables and pass/fail criteria (which still said
`UPSTREAM DETECT_IN=GPIO2` and "전원 모듈은 엄격히 LEADER만 허용" in
current-tense sections), this document's own architecture diagram and
section 6 (DETECT_OUT gating, which still said "before Thread is confirmed
strictly LEADER"), `power_base/sdkconfig.defaults`'s top comment, and
`docs/architecture.md`/`docs/state-machine.md`/`docs/pogo-interface.md`'s
remaining "one and only Thread ROOT/Leader" phrasing. README's real-hardware
log block from BEFORE this session's GPIO/role-decoupling/collector changes
is now explicitly labeled "과거 정책, SUPERSEDED -- 당시 firmware 기준" and
kept only as a historical record of what that OLDER firmware did; a new,
clearly-separate "현재 firmware 기대 로그 (이번 세션, 아직 실물 미검증)"
section describes what THIS firmware is expected to log, not yet verified.
See `tools/test_no_stale_root_leader_claims.py` (new, this pass) for the
automated regression check.

**Host tests added this pass:** `tools/test_power_base_boot_sequencing.c`
gained scenarios for "never attaches within 30s -- does not halt, keeps
retrying, warns once (rate-limited)", "attaches later (CHILD) -- services
start exactly once", "further ROUTER/LEADER changes -- no duplicate
creation", and "`thread_manager_start_as_root()` is never called by
power_base's own boot path" (a structural/string check, since the host model
cannot link the real ESP-IDF-dependent `app_main.c`). See the final report
for the exact scenario list and pass/fail counts.

**Still unverified on real hardware (in addition to everything in section
19.7):** the unbounded attach-retry behavior itself under a genuinely
extended DETACHED period (no real OTBR has been tested against this power
base yet, at any point in this project); the rate-limited warning's actual
cadence on a real console; confirmation that a real attach arriving well
after 30s still triggers `start_services_once()` correctly on real hardware
(only host-tested so far).
