# Validation status (single source of truth)

This document is the one place that tracks, per feature, whether it has
**host-test coverage** and whether it has been **confirmed on real
ESP32-H2 SuperMini hardware**. README.md and the other `docs/*.md` files
each narrate their own feature's history in detail (including superseded
policies, kept as labeled historical record) -- when any of that narrative
prose disagrees with the table below on current status, **this document
wins**, because it is the only page updated as a single pass whenever a new
real-hardware result comes in, rather than being edited piecemeal across
many historical sections.

Legend: **Verified** = actually flashed and observed on real hardware.
**Host-test only** = a host-compiled test (see `tools/test_*.c`/`tools/test_*.py`)
links and runs the real, unmodified production `.c` file(s), or a build
completed with zero warnings, but no physical board has run this path.
**Not implemented** = explicitly out of scope for the current firmware; no
code exists for this yet.

## Core POGO / provisioning (module <-> module, or module <-> Power Base)

| Feature | Host test | Real hardware | Notes |
|---|---|---|---|
| POGO wire protocol (frame/CRC16/PEER_HELLO/capability negotiation) | Yes | **Verified** | Confirmed via Power Base + SHT45 and Power Base + PMS5003, both depth=1. |
| Permanent module UUID assignment | Yes | **Verified** | Confirmed via both single-hop combinations above. |
| `SET_COLLECTOR` (collector endpoint provisioning) | Yes (`tools/test_collector_provisioning_*.c`) | **Verified** | `collector endpoint accepted` observed before `dataset stored`, per the real provisioning order. |
| `topology_gate` data-plane registration gate | Yes (`tools/test_topology_gate.c`) | **Verified** | Power Base + PMS5003: first-sample/topology-registration race no longer reproduces; gate reopens correctly after a Power Base reboot and after a PMS5003 detach/reattach. |
| AUTO-mode POGO hotswap recovery (DOCK_TIMEOUT/LIVENESS_LOST auto-retry) | Yes (`tools/test_pogo_port_hotswap_recovery.c`) | **Verified** | PMS5003 terminal module: physical disconnect + reconnect recovers without a reset. |
| PMS5003 CONDITIONING provisioning (parent accepts `SENSOR_STATUS_CONDITIONING`) | Yes (`tools/test_conditioning_provisioning.c`) | **Verified** | Confirmed as part of the PMS5003 real run. |
| Backoff (UUID-hash-derived retry backoff) | Yes (`tools/test_node_calc_backoff.c`) | Host-test only | Timing under real radio contention not yet measured. |
| SED link mode (`router_eligible=false`, `rx-on-when-idle=false`) for a LEAF | Yes | **Verified** | Observed in real LEAF join logs for both SHT45 and PMS5003 single-hop runs. |
| Always-on FTD link mode for Power Base (CHILD/ROUTER/LEADER all "ready", decoupled from app role) | Yes (`tools/test_power_base_no_root_api.py`, `tools/test_power_base_boot_sequencing.c`) | **Verified** (reached LEADER in the observed runs) | CHILD/ROUTER-only attach under worse radio conditions not yet observed. |
| DEV_ROOT/DEV_LEAF regression fixtures (`upper`/`lower`, module-to-module, no Power Base) | Yes | Verified under an **older** GPIO profile only (see README "실물 배선" history) | Not re-verified under the current GPIO12/GPIO2 profile. |

## Sensors

| Sensor | Config | Host test | Real hardware | Notes |
|---|---|---|---|---|
| SHT45 | `configs/prototype_sht45.json` | Yes (`tools/test_sht45_convert.c`; plus `tools/test_sht45_driver.c` -- **new this pass**, links the real, unmodified `sht45.c` against a fake I2C device, see below) | **Verified** | Power Base + SHT45, depth=1. |
| PMS5003 | `configs/prototype_pms5003.json` | Yes (`tools/test_pms5003_driver.c`, `tools/test_pms5003_parser.c`, `tools/test_pms5003_terminal_config.py`) | **Verified** | Power Base + PMS5003, depth=1, including hotswap -- see above. |
| BH1750 | `configs/prototype_bh1750.json` | Yes (`tools/test_bh1750_convert.c`; plus `tools/test_bh1750_driver.c` -- **new this pass**, same fake-I2C-device approach; plus `matter_bridge_core`'s `ConvertIlluminance` coverage) | **Verified**, including the full Matter path end to end | Real BH1750 samples from module `..d357` reached Linux ingest under changing light. On the Linux host, the final bridge build linked successfully; `chip-tool illuminancemeasurement read measured-value 1 3` returned changing values from 0 to 22369 as the sensor was shaded and uncovered. A later build including the required Identify cluster also linked and ran; endpoint 3 returned `IdentifyTime: 0`, and `chip-tool identify identify 2 1 3` completed with Matter status `SUCCESS`. This does not test the sensor-bitmap-mismatch/restart procedure (section 8.2) or confirm how Apple Home/Google Home render illuminance. See `docs/matter-bridge.md` section 14. |
| SGP40 | `configs/prototype_sgp40.json` | Yes (`tools/test_sgp40_driver.c` -- **new this pass**, links the real, unmodified `sgp40.c` against a fake I2C device, see below; plus CRC and the vendored Gas Index Algorithm) | **Verified** (sensor + wire format + Linux ingest reception only) | Reported by the operator for this pass: a real SGP40 on module `..d357` produced real VOC RAW/INDEX samples that the same real ingest `Registry` received correctly. Gas Index is deliberately **not** bridged to any Matter cluster (unitless algorithmic index, not a calibrated concentration -- see `docs/matter-bridge.md` section 3.1) -- this reception result does not change that scope, and there is nothing further to verify on the Matter side for this sensor. |
| SGP41 | `configs/prototype_sgp41.json` | Yes (`tools/test_sgp41_driver.c` -- **new this pass**, same fake-I2C-device approach as SGP40 above, covers the conditioning-command response shape; plus CRC and the vendored Gas Index Algorithm) | **Verified** (sensor init + POGO/Thread join + Linux ingest reception only) | Reported by the operator for this pass; not reproduced inside this session, which had no physical board or real Linux host access. A real SGP41 on module `..d357` joined as INTERMEDIATE at depth=1 (the first `SENSOR_HELLO` attempt failed; the next attempt completed `JOIN OK`), and the existing PMS5003 module (`..d07a`) redocked downstream of it and re-registered at depth=2, continuing to send samples. On Linux ingest, the VOC channels initially reported `status=7` (`SENSOR_STATUS_CONDITIONING`) right after boot, then all four channels (VOC_RAW/VOC_INDEX/NOX_RAW/NOX_INDEX) transitioned to `status=0` roughly 10 seconds later -- consistent with `SGP41_CONDITIONING_WINDOW_MS` -- with the sample sequence number continuing to increase. See "SGP41 real-board result" below for the full scope note: this does **not** confirm the tick-budget timeout fix (below) actually fired on this run, and does **not** extend to SGP41's VOC/NOx reaching `pogo_matter_bridge` or any Matter cluster (out of scope by design, same as SGP40 above). |

**This pass**: fixed the same real bug in all four I2C sensor drivers --
`components/sht45/sht45.c`, `components/bh1750/bh1750.c`,
`components/sgp40/sgp40.c`, `components/sgp41/sgp41.c`'s
`read_measurement()` -- the polling loop's CALLER-tick-budget timeout
branch (`(xTaskGetTickCount() - wait_start) >= timeout_ticks`) returned
`ESP_ERR_TIMEOUT` without setting `measuring = false` or `status =
SENSOR_STATUS_TIMEOUT`, unlike the elapsed-real-time timeout branch
immediately above it in the same loop, which already did both (a
correction pass fixed an earlier version of this same entry that
overstated the impact -- see below for the corrected, non-exaggerated
description).

**Real production impact** (via `components/sensor_service/sensor_service.c`'s
`sensor_service_measure_once()`, the ONLY real caller of any of these
four drivers' `read_measurement()`): when this tick-budget branch wins the
narrow race against the elapsed-time branch (both are configured to the
same millisecond value by `sensor_service_measure_once()` today, but
`xTaskGetTickCount()`'s coarser tick quantization can cross its threshold
slightly first), `status` stays whatever it was (`SENSOR_STATUS_OK`)
instead of `SENSOR_STATUS_TIMEOUT` for that one polling cycle --
`sensor_service_get_status()` misreports a timed-out reading as healthy
until the NEXT periodic measurement's own `start_measurement()`/
`read_measurement()` pair naturally overwrites it. It does **not**
permanently wedge the driver: `sensor_service_measure_once()` always
calls `start_measurement()` again immediately before every
`read_measurement()`, which unconditionally re-arms `measuring`/
`start_time_us` regardless of the previous state, so the "measuring left
stuck true forever" scenario is never actually observed by this driver's
real caller. That scenario IS real and testable, though -- it's the
driver's own API contract (`read_measurement()` without a preceding
`start_measurement()` must fail cleanly with `ESP_ERR_INVALID_STATE`, not
silently resume polling against stale timing), which a HYPOTHETICAL future
caller could violate even though today's only real caller does not.

Fixed by applying the exact same two-line state transition already used
by the elapsed-time branch, in all four drivers. Host-tested by
`tools/test_sht45_driver.c`/`tools/test_bh1750_driver.c`/
`tools/test_sgp40_driver.c`/`tools/test_sgp41_driver.c` (all new this
pass, see their own rows above): each links and runs the REAL, unmodified
driver `.c` file against a new fake I2C device
(`tools/host_test_stubs/driver/i2c_master.h` +
`tools/host_test_stubs/host_i2c_fake_device.c`, also new this pass) that
drives `read_measurement()` into exactly the buggy branch and asserts both
the `status` transition (the real production-impact fix) and the API
contract (the synthetic-but-defensible fix); each test was also run
against the PRE-FIX driver source (via `git show HEAD:...`) to confirm it
actually fails there (3 checks fail per driver without the fix) rather
than passing vacuously. Not yet re-verified on real hardware -- this is a
host-test-only fix; no ESP-IDF board build of these four drivers'
production behavior was run this pass (a real `module` build was run this
pass to confirm compilation, see below, but not flashed/exercised on a
physical sensor), and the existing per-sensor real-hardware rows above are
otherwise unaffected by this fix.

### SGP41 real-board result (this pass)

**Reported by the operator for this pass; not reproduced inside this
session, which had no physical board or real Linux host access.** Scope
of this result is sensor initialization, POGO/Thread join, and Linux
ingest reception only -- the same boundary already used for the SGP40 row
above.

A real SGP41 on module `..d357` was flashed and docked as an INTERMEDIATE
at depth=1 (a different topology shape than the LEAF-swap test this
section previously outlined -- the operator instead docked it ahead of
the existing PMS5003 chain, extending it to depth=2). The first
`SENSOR_HELLO` attempt failed; the next attempt completed `JOIN OK`, and
the Power Base registered the module normally. The existing PMS5003
module (`..d07a`) redocked downstream of it and re-registered at
depth=2, continuing to send samples throughout.

On Linux ingest: immediately after boot, the VOC channels reported
`status=7` (`SENSOR_STATUS_CONDITIONING`, see
`components/sensor_provider/include/sensor_provider/sensor_provider.h`);
roughly 10 seconds later -- consistent with SGP41's own
`SGP41_CONDITIONING_WINDOW_MS` -- all four channels (VOC_RAW/VOC_INDEX/
NOX_RAW/NOX_INDEX) reported `status=0`, and the sample sequence number
kept incrementing. No `CONFIG_ERROR`, `THREAD_JOIN_TIMEOUT`, or
crash/reboot loop was reported.

**Not covered by this result:**

- Whether this pass's own tick-budget timeout fix (`read_measurement()`'s
  CALLER-tick-budget branch, described above) actually fired on this
  run. Nothing in this report indicates a timeout occurred either way --
  the fix remains verified only by the host test for SGP41, same as the
  other three drivers.
- Whether SGP41's VOC/NOx values reach `pogo_matter_bridge` or any Matter
  cluster. By design, Gas Index is never bridged to a Matter
  concentration cluster (see the SGP40 row above and
  `docs/matter-bridge.md` section 3.1) -- this result does not change
  that scope decision.
- The first failed `SENSOR_HELLO` attempt's own cause -- only that the
  existing retry recovered it on the next attempt.

The flash procedure used to produce this result, kept here for a future
re-test (SDA=GPIO0, SCL=GPIO1, I2C address 0x59, see
`configs/prototype_sgp41.json`; the board stays separately USB powered
until the POGO 5V circuit exists):

```sh
cd "/Users/junsoona/no-icloud/main project_1"
source ~/.espressif/v6.1/esp-idf/export.sh
bash tools/gen_config.sh build/cfg_images
SGP41_PORT=/dev/cu.usbmodemXXXX
idf.py -C module -B ~/esp-builds/pogo_module -p "$SGP41_PORT" flash
python -m esptool --chip esp32h2 -p "$SGP41_PORT" write-flash 0x187000 build/cfg_images/prototype_sgp41_config.bin
idf.py -C module -B ~/esp-builds/pogo_module -p "$SGP41_PORT" monitor
```

## POGO physical topology depth

**This section is about the POGO/application-layer topology** -- physical docking order,
`topology_depth` propagation, logical role assignment, and provisioning -- as tracked by
`components/topology_registry`/`topology_proto` and confirmed by
`tools/validate_three_tier_logs.py`. It is a distinct concept from actual Thread **radio**
multi-hop routing (which physical/radio path a CoAP packet took through the mesh); see
"Thread radio multi-hop routing" below for that separate, still-unverified item. Conflating
the two would overclaim what a passing 3-tier log judgement actually proves.

| Feature | Host test / code review | Real hardware | Notes |
|---|---|---|---|
| depth=1 single hop (Power Base -> one LEAF) | Yes | **Verified** | SHT45 and PMS5003, each independently. |
| INTERMEDIATE role transition timing (only after own upstream provisioning completes) | Reviewed this pass -- OK, no bug | Not yet | `role_manager_is_upstream_ready()` only becomes true after this module's own Thread join succeeds (`child_service.c:889`), and `app_main.c:134` gates parent-eligibility on that flag. Depth/UUID handed to a downstream child come from this module's own already-settled state, not from the Power Base's CoAP topology-report acceptance -- no premature-parent path found. |
| INTERMEDIATE forwarding UUID/slot/depth+1/polling/collector/Dataset to its own downstream child | Reviewed this pass -- OK, no bug | Not yet | `parent_service.c` is generic (used by both Power Base and any INTERMEDIATE via `s_provider` callbacks): confirms the child's own permanent UUID, `ASSIGN_SLOT` with `depth=get_own_depth()+1`, `ASSIGN_PARENT_INFO`, measured-response-time polling period, `SET_COLLECTOR` before `SET_DATASET` (both sides enforce the ordering). |
| INTERMEDIATE always an always-on router-eligible FTD (never SED) | Reviewed this pass -- OK, no bug | Not yet | `thread_manager.c`'s non-LEAF branch sets `rx-on-when-idle=true`/FTD/`router_eligible=true`, keyed on `role_manager_get_role()==MODULE_ROLE_INTERMEDIATE` (independent of `otDeviceRole`), and is correctly re-applied if the role flips while already joined. |
| Terminal LEAF always an SED (never FTD/router-eligible) | Reviewed this pass -- OK, no bug | Not yet | Same `thread_manager.c` function's LEAF branch: `rx-on-when-idle=false`, MTD-like, `router_eligible=false`, plus `otLinkSetPollPeriod()`. |
| depth=2 POGO chain (Power Base -> SHT45/BH1750 INTERMEDIATE -> PMS5003 LEAF) -- POGO docking order, depth propagation, provisioning, CoAP reachability ONLY | Yes (`tools/validate_three_tier_logs.py`) | **Verified** | Real run with SHT45 as the INTERMEDIATE and PMS5003 as the LEAF (BH1750 in this role still not yet run). `tools/validate_three_tier_logs.py` judged the captured logs 16/16 PASS: both POGO links reached `PEER_READY` on both ends, SHT45 settled to logical role `INTERMEDIATE` and PMS5003 to `LEAF`, the Power Base's own `topology_registry` registered two distinct, disjoint UUIDs -- one at depth=1 (SHT45) and one at depth=2 (PMS5003), both modules' `JOIN_RESULT_ACK` was confirmed on their own hop, and sensor samples were recorded from both UUIDs. This item is still POGO/application-layer topology only -- it does NOT confirm Thread radio multi-hop (see next section). |
| Power Base<->INTERMEDIATE POGO detach cascades the whole subtree offline, and a child cannot resurrect itself while its parent stays offline (`TOPOLOGY_REGISTRY_ERR_PARENT_OFFLINE`) | Yes (`tools/test_topology_registry.c`, `tools/test_topology_gate.c`, `tools/test_sensor_transport_gate_retry.c`) | **Verified** | On the same depth=2 chain above, physically detaching the Power Base<->SHT45 POGO link took both SHT45 (depth=1) and PMS5003 (depth=2) offline in the Power Base's registry; PMS5003's own topology reports were rejected as `PARENT_OFFLINE` while SHT45 stayed detached, and the Power Base recorded no PMS5003 sensor sample for the entire detached window. Reattaching let SHT45 re-register at depth=1 first, then PMS5003 re-register at depth=2 once SHT45 was online again, and sensor sample delivery resumed automatically with neither board reset. |
| Power Base topology_registry: two modules' UUIDs/sensor bitmaps never cross-assigned | Reviewed this pass -- OK, no bug | Not yet | `topology_registry_apply_report()` always looks up a node's slot by UUID equality first; a free slot is only allocated for a genuinely new UUID, and every field (including `sensor_bitmap`) is written into a freshly-cleared entry -- no reuse of a stale slot's bitmap under a different UUID. |
| A reattach on one module never corrupts Power Base session/registry state for other modules | Reviewed this pass -- OK, no bug | Not yet | `topology_registry_cascade_offline_children()`/`mark_offline()` only touch the detaching UUID and its own descendants (no global reset). `power_base_transport_on_direct_downstream_detached()` marks only `depth==1` nodes offline, which is exactly the one physically-detached node given Power Base has exactly one physical DOWNSTREAM POGO port. Each module's own `child_service.c`/`parent_service.c` session state is scoped to that module's one upstream link and one downstream port -- a reattach only resets that module's own session. The depth=2 detach/reattach row above confirms the detaching module's own two-node subtree recovers correctly; a THIRD, unrelated sibling module's state during that same detach has not yet been exercised on real hardware. |

## Thread radio multi-hop routing (separate from POGO physical topology depth above)

**Still not verified on real hardware** -- this pass adds the diagnostic logging needed to judge
it, not a real-hardware result. A passing `topology_depth=2` result (POGO docking order +
application-layer provisioning ancestry) does NOT by itself prove that a sensor sample's CoAP
packet was actually radio-relayed through the INTERMEDIATE module's Thread router role to reach
the Power Base -- OpenThread's own mesh routing could equally have delivered that packet over a
direct radio link between the LEAF and the Power Base (or via some other router already in
range), independent of the POGO chain's physical shape.

### This pass: automatic radio-parent/role logging (code only, not yet run on real hardware)

Investigated the existing OpenThread state-change path first rather than adding a new one:
`components/thread_manager/thread_manager.c` already registers `ot_state_changed_cb()` via
`otSetStateChangedCallback()` on every role (Power Base, INTERMEDIATE, LEAF all call
`thread_manager_init()`), and already reacts to `OT_CHANGED_THREAD_ROLE` -- the correct,
already-existing hook for "this device's own role/parent just changed," fired only on a real
transition, never on a timer. This pass extends that SAME callback (new
`log_role_and_parent()`, called only from inside `ot_state_changed_cb()`, so it still only runs
on an actual role-change event -- no new polling task, no repeated logging while nothing
changes) to also log, using the real, unmodified `otLinkGetExtendedAddress()`:

- This device's own Extended Address and `otDeviceRole`, on every role change, for every role
  (Power Base, INTERMEDIATE, LEAF) -- so a LEAF's reported parent can be matched against an
  INTERMEDIATE's or Power Base's own log line from a separate real-hardware capture.
- While (and only while) `otDeviceRole == OT_DEVICE_ROLE_CHILD`, the radio parent's own
  Extended Address and RLOC16 via the real `otThreadGetParentInfo()` -- this is POGO-independent:
  it reads OpenThread's own MLE parent record, never `power_base_transport.c`'s
  application-layer topology report.

Log line (new this pass, `TAG=thread_manager`):

```
thread radio: role=<n>(<ROLE>) own_ext_addr=xx:xx:xx:xx:xx:xx:xx:xx [parent_ext_addr=xx:xx:xx:xx:xx:xx:xx:xx parent_rloc16=0x----]
```

(the `parent_*` fields are only present while `role=CHILD`; ROUTER/LEADER/DETACHED/DISABLED log
only their own address).

Locking: every OpenThread API call added this pass runs from inside `ot_state_changed_cb()`
itself, which already runs on the OpenThread task -- per the existing rule already documented at
that callback's own top ("Called back from inside the OpenThread task itself: the API lock must
NOT be taken again here"), no `esp_openthread_lock_acquire()`/`release()` call was added or
needed. No POGO protocol byte, sensor measurement period, SED link-mode setting, OTBR Dataset,
Matter endpoint ID, or Matter pairing/fabric store was touched -- this is logging only.

**Known limitation of this mechanism** (documented, not fixed): it fires on `OT_CHANGED_THREAD_ROLE`,
which covers a LEAF attaching, detaching, or re-attaching through a `DETACHED` transition (the
normal Thread MLE behavior for losing and finding a parent) -- it would NOT separately log a
parent change that somehow never causes `otDeviceRole` to leave `CHILD` even momentarily (an
edge case OpenThread's own MLE implementation does not appear to produce for a simple parent
loss/switch, but has not been independently confirmed absent on this project's own real
hardware).

**Build verification only, no real-hardware run yet**: real ESP-IDF v6.1 `module` and
`power_base` fullclean-equivalent builds (a touched `thread_manager.c` forcing a real
recompile of just this file, then full link) both completed with zero warnings.
`thread_manager.c` remains not host-compilable (real OpenThread/FreeRTOS dependencies), same as
before this pass -- see other rows in this document for that same pre-existing limitation.

### Judgement criteria for the next real-hardware pass

Do **not** force a particular route, change routing preference, or modify the Dataset to try to
produce a desired result -- this diagnostic only observes whatever OpenThread itself already
decided.

Capture the `idf.py monitor` log from all three devices (Power Base, INTERMEDIATE, LEAF) across
a reboot/(re)join of the LEAF, and read each device's own `thread radio: ...` line(s):

1. **Genuine radio multi-hop through the INTERMEDIATE is confirmed** when ALL of the following
   hold from the same capture window: (a) the LEAF's `parent_ext_addr` equals the
   INTERMEDIATE's own logged `own_ext_addr`; (b) the INTERMEDIATE's own line shows
   `role=ROUTER` (or `LEADER`, though INTERMEDIATE is never expected to be Leader) -- i.e. it is
   genuinely acting as a Thread router, not merely attached as a child itself; and (c) a LEAF
   sensor sample is independently confirmed reaching Linux ingest (see "Linux northbound
   bridge" above) during that same window.
2. **Only POGO depth=2 is confirmed, radio multi-hop is NOT** when the LEAF's `parent_ext_addr`
   instead matches the Power Base's own logged `own_ext_addr` (or, under EXTERNAL_OTBR, some
   other router the OTBR itself exposes) -- the LEAF attached directly over the radio, and the
   POGO chain's physical docking order is not what carried that CoAP traffic. This is a valid,
   expected OpenThread outcome, not a bug -- mesh routing is free to pick the best available
   radio path independent of physical docking order.
3. Neither outcome should be recorded in the table below as **Verified** until an operator
   actually runs this capture on real hardware and states which of the two above was observed;
   this pass adds the instrumentation only.

| Feature | Host test / code review | Real hardware | Notes |
|---|---|---|---|
| A sensor sample's CoAP packet is actually radio-relayed through the INTERMEDIATE module's router role (genuine Thread-mesh multi-hop, not just POGO depth=2) | Code review this pass -- OK, no bug; real ESP-IDF `module`/`power_base` builds, zero warnings (see above) | Not verified | The `thread radio: ...` log line above (own Ext Addr + role always; parent Ext Addr + RLOC16 while CHILD) is now emitted automatically on every real role change, on all three roles -- no manual `ot parent`/`ot router table` CLI diagnostic needed. Judge with the three-point criteria immediately above. Not yet run on real hardware. |

## Thread join timing (open observation, this pass)

The depth=2 real-hardware run above completed with no `THREAD_JOIN_TIMEOUT` on either
hop -- `CONFIG_POGO_JOIN_TIMEOUT_MS` (20s) was not hit on either the Power Base<->SHT45
or SHT45<->PMS5003 join. A diagnostic log added this pass
(`components/thread_manager/thread_manager.c`'s `esp_openthread_auto_start OK --
otDeviceRole immediately after enable: ...`) confirmed `otDeviceRole` transitioned to
`DETACHED` immediately every time `thread_manager_join_with_dataset()` was called on this
run -- so neither `otIp6SetEnabled(true)` nor `otThreadSetEnabled(true)` silently no-op'd.

| Item | Status | Notes |
|---|---|---|
| `THREAD_JOIN_TIMEOUT` on either hop | Not observed this run | Still possible under different real-world conditions; no code change was made on the strength of one clean run. |
| A roughly 16.3s gap between `otThreadSetEnabled(true)` succeeding (role already read back as `DETACHED`) and OpenThread's own first `Attach attempt` log, seen once on this run's FIRST join attempt (a later re-join on the same board attached ~730ms after the equivalent point) | **Unconfirmed root cause -- recorded as an open observation, not fixed** | This firmware's own API calls (`thread_manager.c`/`child_service.c`) all returned success and the role read back correctly immediately, so the delay is not in this firmware's own request path -- it appears to originate inside OpenThread's own MLE attach scheduling. `CONFIG_POGO_JOIN_TIMEOUT_MS` was NOT changed and no warning was suppressed to work around this. The raw capture this observation is drawn from is not checked into this repository. |

## Linux OTBR external Dataset join (see docs/otbr-rcp.md, docs/power-base.md)

**Verified on real hardware** (a later pass than the one that implemented this): a real
Power Base joined a real Linux OTBR's Thread network via `rcp/` -- the Power Base and the
OTBR's own Partition ID matched (`0x7e64ef94`), confirming a genuine shared-partition join,
not two coincidentally-similar independent networks. The same run also confirmed the
existing 3-tier POGO chain (Power Base -> SHT45 INTERMEDIATE -> PMS5003 LEAF) continues to
work end to end under EXTERNAL_OTBR mode: both depth=1 (SHT45) and depth=2 (PMS5003)
registered in the Power Base's topology registry, and sensor data from both nodes was
received. See "SED CoAP response timing correction" below for a timing issue this same run
surfaced (a false "delivery unconfirmed" symptom, not data loss) and its fix.

| Feature | Host test | Real hardware | Notes |
|---|---|---|---|
| `rcp/` (ESP32-H2 USB RCP firmware for Linux OTBR) sdkconfig/build shape | Yes (`tools/test_rcp_config.py`) | **Verified** | Flashed and run as the actual Radio Co-Processor for the real Linux OTBR host in the end-to-end run below. |
| `pb_thread` NVS namespace (mode STANDALONE/EXTERNAL_OTBR, active_tlvs) storage/compat policy | Yes (`tools/test_power_base_thread_config.c`, links the real `components/power_base_thread_config/power_base_thread_config.c` against an in-memory NVS fake) | **Verified** | A real EXTERNAL_OTBR `cfg` image (mode=1 + a real Dataset) was flashed to a real Power Base and successfully joined the real OTBR network below. |
| `tools/pack_power_base_config.py --external-dataset-file` / `tools/gen_power_base_otbr_config.sh` (dataset hex file validation, image generation, 0600 permission, NVS re-parse verification) | Yes (manual + scripted with a fake test-only TLV, never a real Dataset) | **Verified** | Used to generate the actual `cfg` image flashed in the end-to-end run below, from a real `ot-ctl dataset active -x` capture. |
| `power_base/main/app_main.c`'s STANDALONE vs. EXTERNAL_OTBR boot branch (exactly-once API call, no fallback) | Yes (`tools/test_power_base_boot_sequencing.c`, mirrors `app_main.c`'s control flow -- see that file's own "not host-compilable" note) | **Verified** | The EXTERNAL_OTBR branch ran and successfully joined on real hardware (see end-to-end row below). |
| `thread_manager_start_as_always_on_ftd_with_dataset()` (NULL/0/oversized-TLV rejection, `otDatasetIsValid()` gate, always-on-FTD link mode, no network-formation call on the EXTERNAL_OTBR path) | Code review only -- see "ESP-IDF real build + code review scope" below | **Verified** | Real ESP-IDF v6.1 `power_base` fullclean build with zero warnings; `nm power_base.elf` confirms it is linked into the final binary; and it actually ran successfully on real hardware (the join below). |
| End-to-end: a real Power Base joins a real Linux OTBR + `rcp/`'s actual Thread network, and a sensor module downstream of the Power Base receives that same Dataset via the pre-existing POGO `SET_DATASET` | None (real-hardware only) | **Verified** | Power Base and OTBR Partition IDs matched (`0x7e64ef94`); depth=1 (SHT45) and depth=2 (PMS5003) both registered in the topology registry and both delivered sensor data. |
| 3-tier POGO chain (Power Base -> SHT45 INTERMEDIATE -> PMS5003 LEAF) specifically under EXTERNAL_OTBR mode | None (real-hardware only) | **Verified** | Same run as above -- extends the pre-existing STANDALONE-mode depth=2 verification (see "POGO physical topology depth" above) to EXTERNAL_OTBR mode as well. |

## SED CoAP response timing correction (see docs/state-machine.md)

**Implemented and host-tested; NOT YET RE-VERIFIED on real hardware.** Surfaced by the same
real-hardware run above: the PMS5003 LEAF logged repeated `sample dropped after 4 attempts`/
topology-report timeouts even though the Power Base's own log showed the same `sample_seq`
already stored -- a false "unconfirmed" symptom caused by this LEAF's Thread SED data-poll
period being (at the time) accidentally coupled to its sensor measurement period (3528ms),
not by any actual data loss. See docs/state-machine.md's "SED CoAP response timing
correction" section for the full root-cause analysis and fix.

| Feature | Host test | Real hardware | Notes |
|---|---|---|---|
| `components/coap_response_timing` (shared ACK-wait derivation, overflow-safe) | Yes (`tools/test_coap_response_timing.c`) | Not yet | Pure function, exhaustively tested in isolation: correct derivation, den=0 defensive handling, 64-bit-safe overflow saturation. |
| Thread SED data-poll period decoupled from sensor measurement period (`thread_manager_join_with_dataset()`'s renamed `thread_data_poll_period_ms` parameter; `child_service.c` always passes 0) | Yes (`tools/test_conditioning_provisioning.c`, extended -- links the REAL `child_service.c`/`parent_service.c` end to end; snoops a REAL, dynamically-computed `polling_period_ms` off the wire and confirms it never reaches `thread_manager_join_with_dataset()`) | Not yet | Proves the general decoupling property using a real dynamically-measured sensor period, not a hardcoded literal. |
| `sensor_transport.c`/`topology_reporter.c` share ONE CoAP response-timing policy (`CONFIG_SENSOR_COAP_ACK_TIMEOUT_MS`, `coap_response_timing_wait_ms()`) instead of two independent copies | Yes (`tools/test_topology_reporter_session.c`, extended: links the real `coap_response_timing.c` and confirms it derives the identical wait value `tools/test_sensor_transport_gate_retry.c` derives for `sensor_transport.c`) | Not yet | `topology_reporter.c` itself remains not host-linkable (same pre-existing limitation as the rest of that file) -- cross-checked by code review (identical symbols) plus the real ESP-IDF `module` build. |
| A delayed-but-within-window CoAP 2.xx response still succeeds on the first attempt; a genuinely unanswered request becomes a bounded (never hung) failure; a late/stale callback for an abandoned attempt never corrupts a later attempt | Yes (`tools/test_sensor_transport_gate_retry.c`, extended -- links the real `sensor_transport.c` against a REAL wall-clock-delayed CoAP response simulator, `tools/host_test_stubs/host_ot_coap_sim.c`) | Not yet | First real host test in this repository to exercise this code's response-wait timing with genuine asynchronous (not synchronous) callback delivery. |
| `thread_manager_start_as_always_on_ftd_with_dataset()`'s LEAF Kconfig-default resolution (`CONFIG_THREAD_MANAGER_LEAF_DATA_POLL_PERIOD_MS`, 500ms) actually applied via `otLinkSetPollPeriod()` | Code review only (`thread_manager.c` not host-compilable) | Not yet | Verified: real ESP-IDF `module`/`power_base` fullclean builds, zero warnings; the ternary resolving `thread_data_poll_period_ms==0` to the Kconfig default is unchanged in shape from the pre-existing, already-reviewed logic it replaced. |
| Log wording change (`dropped` -> `delivery unconfirmed`) for both `sensor_transport.c` and `topology_reporter.c` | Yes (`tools/test_sensor_transport_gate_retry.c` asserts the exact new wording; `topology_reporter.c`'s own change verified by code review + build, same reasoning as its timing policy row above) | Not yet | |

## Linux northbound bridge (Power Base -> Linux OTBR host, `components/linux_bridge_exporter`)

| Feature | Host test | Real hardware | Notes |
|---|---|---|---|
| TOPOLOGY export (uuid-coalesced queue, current-state-at-drain-time, periodic full resync) | Yes (`tools/test_linux_bridge_exporter.c`, `tools/test_linux_bridge_exporter_timers.c`, `tools/test_linux_bridge_exporter_retry_policy.c`) | **Verified for delivery to the real Linux host** | The running Linux ingest received depth=1 and depth=2 topology reports from the real Power Base. Queue coalescing and every resync edge case remain host-test claims, not separate hardware measurements. |
| SAMPLE export (uuid-coalesced pending table, latest-wins, periodic full resync) | Yes (same three files above) | **Verified for delivery to the real Linux host** | The running Linux ingest logged changing BH1750, PMS5003, and earlier SGP40 samples from the Power Base. Exact per-UUID publish limits and retry-exhaustion behavior have not been measured separately on hardware. |
| CoAP retry policy (2.xx success; 4.xx final, never retried; timeout/OT-error/5.xx bounded retry, shared by TOPOLOGY and SAMPLE) | Yes (`tools/test_linux_bridge_exporter_retry_policy.c`) | Not yet | |
| Per-module_uuid SAMPLE export rate limit (`CONFIG_LINUX_BRIDGE_EXPORTER_SAMPLE_MIN_INTERVAL_MS`, default 1000ms) -- coalescing to the latest sample, wrap-safe timing, TOPOLOGY unaffected | Yes (`tools/test_linux_bridge_exporter_rate_limit.c`, `tools/test_linux_bridge_exporter_retry_exhaustion.c`, `tools/test_linux_bridge_exporter_timers.c`, all linking the real, unmodified `linux_bridge_exporter.c`) | Not yet | See "RCP/OTBR fault recovery and transport-load reduction" below. |
| **Correction pass**: unified SAMPLE scheduler -- the periodic full sample resync no longer bypasses the rate-limit gate (it now only marks a uuid pending, exactly like a reactive callback, via the shared `mark_sample_pending()`); `transmit_sample()` re-reads `sensor_root_aggregator`'s CURRENT value and re-packs the wire payload immediately before EVERY attempt (including retries), fixing a real bug where a stale `sample_seq` could be resent on attempt 2/3 if a newer sample arrived mid-retry; a fully-exhausted transient-failure retry sequence re-marks the uuid pending so the next allowed window republishes automatically | Yes (`tools/test_linux_bridge_exporter_rate_limit.c`'s ordered-attempt-inspection scenario, `tools/test_linux_bridge_exporter_retry_exhaustion.c`, `tools/test_linux_bridge_exporter_timers.c`'s new resync-vs-gate scenario -- all link the real, unmodified `linux_bridge_exporter.c` and inspect every captured CoAP attempt in order, not just the last one) | Not yet | Fixes two bugs found in code review of the rate-limit pass immediately above (stale-payload-on-retry, and periodic resync silently bypassing the per-uuid gate) -- see `linux_bridge_exporter.c`'s own top comment ("Unified SAMPLE scheduler"/"Per-attempt fresh payload" paragraphs) and `CONFIG_LINUX_BRIDGE_EXPORTER_SAMPLE_MIN_INTERVAL_MS`'s own Kconfig help text for the precise unit (one LOGICAL PUBLISH, i.e. a whole bounded attempt sequence, never one per individual CoAP attempt). |

## Linux Matter Bridge (`pogo_matter_bridge`) MVP (see `docs/matter-bridge.md`)

Added in a prior pass (`feat: add Linux Matter Bridge MVP (pogo_matter_bridge) over
connectedhomeip v1.6.0.0`); this repository's own "explicitly out of scope" table
below previously still listed Matter Bridge as "Not implemented" -- a stale leftover
from before that commit, corrected in this pass.

| Feature | Host test | Real hardware | Notes |
|---|---|---|---|
| `linux/matter_bridge/ingest` (CoAP ingest server, `Registry::apply_topology()`/`apply_sample()`/`apply_heartbeat()`, generation/sequence ordering, conflict-safe (power_base_uuid, module_uuid) keying) | Yes (`ingest_unit_tests`, `ingest_registry_snapshot_test`, `ingest_loopback_test`, `ingest_ack_transport_static_test` -- all pass via `cmake`+`ctest`, zero warnings) | **Verified** (this pass, reported by the operator, not reproduced inside this session) | Real BH1750 and SGP40 CoAP SAMPLE traffic (module `..d357`) was correctly received/decoded by this exact `Registry` code -- see the Sensors table above for the precise evidence and what this does/doesn't also confirm about `pogo_matter_bridge`'s own Matter-side (`app/chip`) code. |
| `linux/matter_bridge/app/core` (`value_mapping`, `EndpointStore` persistence, `BridgeManager::Reconcile()`/conflict detection/dedup) | Yes (`matter_bridge_core_unit_tests` via `cmake`+`ctest`, zero warnings) | **Verified for live BH1750 value mapping** | Real BH1750 changes reached `chip-tool` as changing Illuminance values at stable endpoint 3. Independent tests for endpoint-map corruption/recovery, every conflict path, and every restart edge case remain host-test-only. |
| `linux/matter_bridge/app/chip` (`chip_endpoint.cpp`/`main.cpp`, the files that touch the real Matter SDK) | Core host tests pass; the earlier macOS sandbox could only partially compile Linux targets | **Verified for the tested BH1750 and PMS5003 paths** | The operator built and linked `pogo_matter_bridge` against connectedhomeip v1.6.0.0 on the real Linux host, ran it, and read BH1750 Illuminance and PMS5003 Air Quality through an already commissioned `chip-tool`. This does not establish formal Matter certification or every cluster/device permutation. |
| **A prior pass (SAMPLE/Matter-publish transport-load reduction)**: `BridgeManager::TryBeginSchedule()`/`AbortSchedule()`/`EndSchedule()` (per-uuid Matter-work-item coalescing + rate limit), `CommitApplied()` (commit-only-on-confirmed-success dedup state, replacing the old eager-commit-inside-`Reconcile()` design), `ReconcileOne()` (re-derive from a fresh snapshot at actual apply time), `EndpointStore` internal locking (newly required now that `get_or_assign()` is called from two threads) | Yes (`core/tests/test_bridge_manager.cpp` and `core/tests/test_endpoint_store.cpp`, both extended that pass -- the latter adds a real multi-threaded `get_or_assign()` contention scenario; links the real, unmodified `bridge_manager.cpp`/`endpoint_store.cpp`) | Not yet | That pass re-cloned and re-bootstrapped a real `connectedhomeip v1.6.0.0` checkout and `gn gen` again resolved the identical 6256-target graph, but no Linux cross-compiler was available in that pass's sandbox (unlike the earlier pass above), so `ninja` could not compile anything at all that time -- a host-toolchain difference, not a new code finding. `chip_endpoint.cpp`'s new `ApplyOutcome`-returning `Apply()` and `main.cpp`'s rewritten `RegistryWatcherThreadMain()`/`ApplyUpdateOnMatterThread()` were instead verified by manual review against the exact same API shapes the already-compiled pre-existing code used -- see `docs/matter-bridge.md` section 14's own note on that pass. |
| **An earlier pass**: BH1750 -> Illuminance Measurement (0x0400) bridging (`ConvertIlluminance()`'s CSA log-encoding, `IlluminanceMeasurementCluster`/Light Sensor device type wiring), and `BridgeManager::SensorBitmapMismatches()` (detects + withholds channel updates for a module_uuid whose sensor hardware changed without a bridge restart), plus a follow-up correction fixing a real `minMeasuredValue`/`maxMeasuredValue` mismatch bug (see `docs/matter-bridge.md` section 3's own Illuminance entry and the shared `kIlluminanceMeasuredValueMin`/`Max` constants) | Yes -- `core/tests/test_value_mapping.cpp`/`test_bridge_manager.cpp`, extended across those passes; 137/137 checks pass, 0 warnings, **including under a full AddressSanitizer run** (`-fsanitize=address`, added after ASan caught a real out-of-bounds bug in the TEST code itself, not production code) | **Verified** (reported by the operator, a later pass than the code changes themselves -- see the Sensors table's BH1750 row above for the full result: real final build+link, `chip-tool illuminancemeasurement read measured-value 1 3` returning 0-22369 against live BH1750 samples) | Those passes took a narrower, source-verification-first approach rather than attempting a full SDK build themselves: cloned the real `connectedhomeip` repository at exactly tag `v1.6.0.0` (`git describe --tags --exact-match` confirmed) to read `IlluminanceMeasurementCluster.{h,cpp}` and the CSA data-model XML directly, so the new cluster's constructor/`SetMeasuredValue()` signature and the log-encoding formula/valid-range constants were matched against real source, not general knowledge or guesswork -- the operator's own later real-hardware run (Sensors table above) has since confirmed that source-level verification produced genuinely working code. |
| Device-type/mandatory-cluster conformance audit -- required `IdentifyCluster` on dynamic endpoints and `AirQualityCluster` on PMS5003, fixed at `Unknown` (see `docs/matter-bridge.md` section 5.1) | SDK-dependent code; no CHIP-independent host test | **Verified for BH1750 and PMS5003 endpoints** | The operator's updated Linux build linked successfully. Endpoint 3 (BH1750) and endpoint 4 (PMS5003) returned `IdentifyTime: 0`; `chip-tool identify identify 2 1 3` and `... 1 4` each returned `SUCCESS`. Endpoint 4 returned `AirQuality: 0` (`Unknown`), as designed. SHT45's Identify endpoint was not exercised in this run; `TriggerEffect` and certification were not tested. |

## RCP/OTBR fault recovery and transport-load reduction (this pass)

**Real-hardware verified** (reported by the operator for this pass; not reproduced
inside this session, which had no physical board access):

- Native systemd `Restart=` policy plus `PartOf=otbr-agent.service` applied to the
  RCP/OTBR service units. With this in place, two consecutive real RCP/OTBR restarts
  were observed to fully self-recover: `wpan0` came back up, the Matter Bridge
  (`pogo_matter_bridge`) process restarted and reattached automatically, and Apple
  Home and Google Home both recovered live sensor values afterward with no manual
  intervention.
- The existing connectedhomeip KVS and Matter pairing/fabric state survived both
  restarts intact -- no re-commissioning was needed.

**Root cause NOT confirmed:**

- The ESP32-H2 RCP's intermittent `RESET_FAULT` (see `docs/otbr-rcp.md`) still has no
  identified root cause. The systemd auto-recovery above means a `RESET_FAULT`
  episode is no longer a hands-on incident, but it does **not** mean the fault itself
  has been fixed or explained -- this pass makes no such claim.
- **Update (a later pass than this one)**: a real-hardware coredump separately
  confirmed a DIFFERENT, specific RCP crash mechanism (`ot_main` task,
  `assert(false)` after `esp_openthread_launch_mainloop()` returned -- see
  `docs/otbr-rcp.md` section 12 for the full evidence-tier breakdown). That
  coredump confirms the crash/assert path itself, not which error triggered it;
  a USB RX `EIO` trigger is source analysis, not something read out of the
  coredump. See the new section below for the real-hardware soak result on the
  fix for that mechanism, combined with the existing TX hardening above.

**Implementation (SAMPLE/Matter-publish transport-load reduction -- NOT a
`RESET_FAULT` fix, tracked as a separate, unrelated change):**

- Power Base -> Linux: `components/linux_bridge_exporter` sends at most one
  SAMPLE PUBLISH per module_uuid per `CONFIG_LINUX_BRIDGE_EXPORTER_SAMPLE_MIN_INTERVAL_MS`
  (default 1000ms), always publishing the single latest (seq, value) -- never a
  replayed backlog. TOPOLOGY events are a completely separate, unaffected path.
- **Correction pass** (code review found two real bugs in the above): (1) a
  stale `sample_seq` could be resent on a retry attempt if a newer sample
  arrived mid-flight -- fixed by re-reading `sensor_root_aggregator` fresh
  immediately before every individual CoAP attempt, never a cached payload;
  (2) the periodic full sample resync used to bypass the rate-limit gate
  entirely, so a reactive publish immediately followed by a resync tick
  could produce two publishes inside one window -- fixed by routing resync
  through the exact same `mark_sample_pending()`/gate a reactive publish
  uses, so it only ever marks a uuid pending, never dispatches directly.
  TOPOLOGY resync is unaffected (never rate-limited, unchanged both passes).
- Linux Registry -> Matter: `matter_bridge::BridgeManager` coalesces and
  rate-limits how often a module_uuid's changes actually reach
  `chip::DeviceLayer::PlatformMgr().ScheduleWork()`/the Matter SDK (at most one
  in-flight work item per uuid, and non-urgent SAMPLE-only changes wait at least
  `BridgeManager::kDefaultSampleUpdateInterval`, 1000ms, between schedule attempts) --
  a new-endpoint or Reachable/online-offline transition always bypasses that wait.
  Not touched by the correction pass (that pass's own code review only found
  issues on the Power Base -> Linux side).
- **Explicitly unchanged by both passes**: sensor measurement period, POGO
  `POLL_CONFIG`, and the Power Base's own internal `sensor_root_aggregator` storage
  rate/period are all completely untouched -- every sensor sample is still measured
  and stored at exactly the same rate as before; only how often an
  ALREADY-CURRENT value is forwarded/published downstream was reduced.
- Not yet verified on real hardware -- see the per-component host-test rows above.
  ESP-IDF v6.1 `power_base`/`module` fullclean builds (zero warnings) and this
  repository's full host test suite were run for both passes; see each pass's own
  final report for the complete list.

## RCP USB Spinel RX recovery + combined TX/RX real-hardware soak (a later pass than the one above)

See `docs/otbr-rcp.md` section 12 for the full write-up (evidence tiers,
call-chain analysis, host test, ELF verification). Summary here per this
document's own "single source of truth" role:

| Item | Host test / build | Real hardware | Notes |
|---|---|---|---|
| RX wrapper (`esp_openthread_platform_process_wrap.c`, `--wrap=esp_openthread_platform_process`, bounded `ESP_FAIL`+`EIO` retry, silent restart on exhaustion) | Yes (`tools/test_rcp_usb_rx_recovery.c`, links the real, unmodified wrapper; 6 scenarios / 24 checks pass, 0 warnings). Real ESP-IDF v6.1 `rcp/` fullclean build, both production and diagnostic sdkconfig variants, 0 warnings; `nm`/`objdump` confirm the call site and the wrap/real/recovery symbol chain in the linked ELF. | **Verified**, combined with the existing TX hardening (see below) | Mainloop-exit/`assert(false)` crash path is coredump-confirmed; the specific `EIO` trigger is source analysis, corroborated (not proven) by the real-hardware runs below -- see `docs/otbr-rcp.md` section 12's own evidence-tier breakdown. |
| Combined RCP USB Spinel hardening (this RX fix + the existing TX retry, section 9) -- diagnostic RCP build, 1 continuous hour, full 3-tier chain (Power Base -> SHT45 INTERMEDIATE -> PMS5003 LEAF) | N/A (real-hardware only) | **Verified** (reported by the operator for this pass; not reproduced inside this session, which had no physical board access) | `systemctl show otbr-agent -p NRestarts`: `425 -> 425`. Zero occurrences of `P-HdlcIntface`, `RESET_FAULT`, `RESET_SOFTWARE`, `Unexpected RCP reset`, `HandleRcpUnexpectedReset`, `Failed with result`. |
| Combined RCP USB Spinel hardening -- production RCP build, 1 continuous hour, same chain | N/A (real-hardware only) | **Verified** (reported by the operator for this pass; not reproduced inside this session) | `systemctl show otbr-agent -p NRestarts`: `0 -> 0`. Same six log patterns above, zero occurrences. |

**Not isolated by the above**: both real-hardware runs had the RX fix and
the TX fix applied TOGETHER. This confirms the combined hardening across
2 continuous hours (zero RCP/OTBR restarts total), not either fix's own
individual contribution. Also not covered: continuous operation beyond
2 hours, and behavior across different USB host controllers/hubs/cables.

## Explicitly out of scope (not implemented, not planned this pass)

| Feature | Status |
|---|---|
| Deep-sleep | Not implemented. GPIO12 was reserved as a future deep-sleep wake source (see `docs/pogo-interface.md`), but no deep-sleep code exists. |
| Light-sleep (automatic, for a terminal LEAF's short Thread SED polling period) | Not implemented -- **reviewed this pass, real code unchanged**. `docs/power-base.md` section 20 cross-checks the official ESP-IDF v6.1 `examples/openthread/ot_sleepy_device/light_sleep` example (cloned at exactly that tag) against this project's own POGO UART/CoAP/measurement timing and found it not yet safe to enable by default: `CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP` and UART wakeup are mutually exclusive on ESP32-H2 (confirmed against Espressif's own official sleep-modes documentation) -- max power savings means this device cannot be woken by incoming POGO traffic at all while asleep, and the alternative (keep the UART powered) still needs real-hardware proof that a full POGO frame, not just a byte or two, survives the wake -- no sensor or `pogo_port.c` driver in this codebase currently holds an `esp_pm_lock` around its I2C/UART transactions either. `CONFIG_FREERTOS_HZ=1000` is the example's OWN tuning choice for its own short (3s) poll period, not a requirement of Light-sleep itself -- a future revision could keep this project's current 100Hz tick rate with a different threshold tuning instead. See that section for the full remaining-work checklist. Sensors stay always-online in Matter as the user requires; the terminal LEAF's existing Thread SED (`mRxOnWhenIdle=false`) is unaffected. |
| POGO 5V power supply over the POGO connector | Not implemented -- every board is powered independently over USB; `power_enable_gpio` is unconfigured (`power_base_main: power_enable_gpio not configured -- 5V rail assumed always-on`). See `docs/power-base.md` section 8.1 for the circuit requirements (switch, current limit, reverse-power protection) and real-hardware checklist a future revision needs before this can be enabled -- not implemented this pass either, and `power_enable_gpio` remains deliberately unassigned. |

## Stale claims corrected by this pass

- README previously had one unlabeled bullet claiming the Power Base "엄격히
  LEADER만" (strictly LEADER only, no longer accepting ROUTER) as a still-
  current caveat -- this was leftover phrasing from a policy that
  `docs/power-base.md` sections 19.1/19.7 already documented as superseded
  (current policy: CHILD/ROUTER/LEADER are all "ready", decoupled from the
  module's fixed application role). Fixed in this pass.
- Multiple README sections described PMS5003 real-hardware measurement,
  `topology_gate`, and POGO hotswap recovery as "아직 실물 미검증" (not yet
  verified on real hardware). All three have since been verified -- see the
  tables above. Fixed in this pass.
- The depth=2 POGO chain (Power Base -> SHT45 INTERMEDIATE -> PMS5003 LEAF)
  row was "Not yet" pending the next real-hardware pass -- that pass has
  since happened (`tools/validate_three_tier_logs.py` 16/16 PASS) and the
  Power Base<->INTERMEDIATE detach/reattach recovery (`PARENT_OFFLINE`) was
  also confirmed on the same run. Both rows updated to **Verified** this
  pass. Thread radio multi-hop routing (a separate, still-unverified claim)
  is deliberately left unchanged -- see that section's own scope note.
- The Sensors table's BH1750/SGP40 rows said "Not yet"/"real board still
  unwired" -- corrected to **Verified**, but strictly scoped to what this
  pass's actual log evidence covers (real sensor + wire format + Linux
  ingest reception on module `..d357`, see that table's own notes). This
  correction does **not** extend to `pogo_matter_bridge` (the `app/chip`
  Matter-side binary) actually running against this hardware, or to any
  `chip-tool`/Apple Home/Google Home result -- those remain "Not yet",
  unchanged, and the Linux Matter Bridge MVP table's own new row for this
  pass's Illuminance Measurement bridging spells out exactly that
  distinction. SGP41's row has since been corrected to **Verified**, but
  strictly scoped to sensor init + POGO/Thread join + Linux ingest
  reception -- see the Sensors table and "SGP41 real-board result" above
  for the full evidence and the explicit "not covered by this result"
  list (the tick-budget timeout fix firing on real hardware, and any
  Matter-side reception, are both still unconfirmed). SGP40/41's Gas
  Index is, and remains, never represented as a Matter
  concentration-cluster value (`docs/matter-bridge.md` section 3.1) --
  this pass's real-hardware VOC RAW/INDEX reception does not change that
  scope decision.
