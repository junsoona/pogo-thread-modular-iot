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
one and only Thread ROOT/Leader.** A sensor module's UPSTREAM peer being the
power base or another sensor module is treated identically by
`role_manager`: the module is `LEAF` (nothing docked below it) or
`INTERMEDIATE` (another module docked below it) -- never `ROOT`. See
[`role_manager.c`](../components/role_manager/role_manager.c)'s `recompute()`
and its own comment on this fix.

## 2. Final architecture

```
                       power_base/ (ESP32-H2, this project)
                       - Thread ROOT/Leader, Active Dataset owner
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
| `parent_service` | used (DOWNSTREAM), context wired to `role_manager`/`child_service`/`sensor_service` | used UNMODIFIED, context wired to the power base's own always-ready-once-Leader / fixed-depth-0 / efuse-derived identity (see `parent_service.h`'s `parent_service_context_t`) |
| `thread_manager` | `thread_manager_join_with_dataset()` (INTERMEDIATE/LEAF) | `thread_manager_start_as_root()` (forms or resumes the ONE Active Dataset) |
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
profile (UART1, GPIO10/11, DETECT GPIO22/23). It connects to the first
sensor module's UPSTREAM port (UART0, GPIO4/5, DETECT GPIO12/2, BASE_ID
GPIO14):

| Power base pin | Direction | First module pin |
|---|---|---|
| GPIO10 (UART TX) | ---> | GPIO5 (UART RX) |
| GPIO11 (UART RX) | <--- | GPIO4 (UART TX) |
| GPIO22 (DETECT_OUT) | ---> | GPIO2 (DETECT_IN) |
| GPIO23 (DETECT_IN) | <--- | GPIO12 (DETECT_OUT) |
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
- **GPIO2 (the first module's DETECT_IN) and GPIO14 (the first module's
  BASE_ID) are two different signals with two different local pull-ups.**
  Never tie them together, and never wire either one directly to GND on the
  module side (only the power base's BASE_ID contact is GND; DETECT_IN is a
  normal pull-up input driven Low by the peer's open-drain DETECT_OUT, never
  hard-wired to GND).

DETECT_OUT on both sides is open-drain, active-low, release-only-High (never
driven push-pull High) -- see [`pogo-interface.md`](pogo-interface.md)'s
DETECT_OUT/DETECT_IN contract, unchanged by this follow-up.
`downstream_port.detect_out_active_low` (both `module_config` and
`power_base_config`) must be `true`; `pogo_port`'s drive logic is hardcoded
to that policy regardless of what a config claims (see `module-config.md`).

Module-to-module (deeper chain) wiring, and the confirmed GPIO profile
table, are unchanged -- see [`pogo-interface.md`](pogo-interface.md).

## 5. Power base boot sequence

`power_base/main/app_main.c`'s `app_main()`, in order (each step below is
exactly one item of the follow-up spec's section 4):

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
   `ASSIGN_PARENT_INFO`) and its topology-registry key. Never regenerated,
   never logged in full.
5. `thread_manager_init()` -- OpenThread FTD stack up, no network yet.
6. `thread_manager_start_as_root()` -- **the only place in the whole system
   that ever forms or resumes an Active Dataset.** If NVS already holds a
   commissioned dataset, it is resumed; otherwise a new one is formed from
   Kconfig defaults. Neither the dataset bytes nor the Network Key are ever
   logged (same policy `thread_manager.c` already enforced).
7. Poll `thread_manager_get_role()` until strictly `Leader` (**not** "Leader
   or Router" -- code review follow-up section 5: this power base is the
   system's one and only Thread Leader, and in a network where it is the
   sole device that ever forms/resumes the dataset, a solo-forming FTD node
   attaches directly into the Leader role -- there is no partition to defer
   Leadership to, so "merely Router" is never the correct steady state
   here), bounded to 30s (same budget `module`'s `root_ready_watcher_task`
   uses). **Any failure at step 2, 6, or 7 halts here and NEVER calls
   `pogo_port_enable()`** -- see section 6.
8. `power_base_transport_init()` -- registers the CoAP `"/s"` (sensor data)
   and `"/t"` (topology report) resources.
9. `parent_service_init()` -- wired to the power base's own context (always
   `provisioning_allowed()` once strictly LEADER, `get_own_depth()==0`,
   `get_own_uuid()` = the efuse identity, `is_power_base=true`).
10. `pogo_port_init()` + `pogo_port_enable()` for the one DOWNSTREAM-shaped
    port -- see section 6 for why this single call is also what activates
    DETECT_OUT.
11. A periodic (5s) stale-sweep task calls `power_base_transport_sweep_stale()`.

## 6. DETECT_OUT gating

There is no separate "now assert DETECT_OUT" step to sequence: `pogo_port_enable()`
itself drives DETECT_OUT active (Low) as the very last part of bringing a
port up (see `pogo_port.c`'s `set_detect_out(p, true)` call inside it), and
`pogo_port_init()` alone never touches any GPIO. Simply never calling
`pogo_port_enable()` before Thread is confirmed strictly LEADER (step 7
above) IS the "DETECT_OUT only activates once Thread is ready" guarantee --
there is
no other place in this firmware image that can assert it. Because
`app_main()`'s boot sequence is linear and calls `pogo_port_enable()` exactly
once, DETECT_OUT is also activated exactly once per boot (see
`tools/test_power_base_boot_sequencing.c`).

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
- **Power base reboot**: the registry is RAM-only and starts empty. It is
  repopulated purely by modules' own ordinary `ATTACHED`/`HEARTBEAT` reports
  arriving into what looks like "an unknown uuid" -- no special "recovery
  mode" exists or is needed (see `tools/test_topology_registry.c`'s
  `scenario_base_reboot_registry_repopulated_by_heartbeat`).

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

## 16. What is still unverified on real hardware

Everything in this document is verified at the source/build/host-test level
only, in this session, on the CURRENT bench wiring (no power-base <-> module
harness has been built yet). In particular, still needing real hardware:

- The actual POGO wiring table in section 4 above, on a real power-base
  board + a real first sensor module.
- `pogo_port`'s BASE_ID/PEER_HELLO cross-check with a REAL power-base MCU on
  the other end (this session's tests mirror the logic; they do not exercise
  real UART/GPIO timing).
- `thread_manager_start_as_root()`'s dataset resume path across a REAL power
  cycle of the power-base board.
- CoAP delivery of both `"/s"` and `"/t"` over a real multi-hop Thread mesh,
  including the UUID-hash backoff schedule's real timing.
- The 30s strictly-LEADER boot-wait budget under real radio conditions.
- Any of section 11's detach/reconnect/stale behaviors under a real physical
  detach (as opposed to the host-tested pure registry logic).

See the final report's "실물 검증 절차" for the exact flash order and
pass/fail criteria to run once the new wiring exists.

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

5. **Power base gates strictly on LEADER.** `pb_provisioning_allowed()` and
   the boot-time role-wait in `power_base/main/app_main.c` used to accept
   `LEADER || ROUTER`. Fixed to strictly `LEADER` -- see that function's own
   comment for why ROUTER was never actually a correct steady state for a
   node that is the sole former/resumer of this network's dataset. Every
   log line naming an `otDeviceRole` (here, `child_service.c`,
   `parent_service.c`) now prints both the numeric value and a name via the
   new shared `thread_manager_role_to_str()`. See
   `tools/test_power_base_boot_sequencing.c`'s `scenario_router_only_never_activates`.

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
