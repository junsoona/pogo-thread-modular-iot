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
| SHT45 | `configs/prototype_sht45.json` | Yes | **Verified** | Power Base + SHT45, depth=1. |
| PMS5003 | `configs/prototype_pms5003.json` | Yes (`tools/test_pms5003_driver.c`, `tools/test_pms5003_parser.c`, `tools/test_pms5003_terminal_config.py`) | **Verified** | Power Base + PMS5003, depth=1, including hotswap -- see above. |
| BH1750 | `configs/prototype_bh1750.json` | Yes (`tools/test_bh1750_convert.c`) | Not yet | Never flashed/wired on real hardware. |
| SGP40 | `configs/prototype_sgp40.json` (this pass) | Yes (I2C commands, CRC, vendored Gas Index Algorithm) | Not yet | Config added this pass; real board still unwired. |
| SGP41 | `configs/prototype_sgp41.json` (this pass) | Yes (I2C commands, CRC, vendored Gas Index Algorithm, conditioning-vs-measure command switch) | Not yet | Config added this pass; real board still unwired. |

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

**Not verified, and not verifiable by the 3-tier POGO test or `tools/validate_three_tier_logs.py`.**
A passing `topology_depth=2` result (POGO docking order + application-layer provisioning
ancestry) does NOT by itself prove that a sensor sample's CoAP packet was actually radio-relayed
through the INTERMEDIATE module's Thread router role to reach the Power Base -- OpenThread's own
mesh routing could equally have delivered that packet over a direct radio link between the LEAF
and the Power Base (or via some other router already in range), independent of the POGO chain's
physical shape.

| Feature | Host test / code review | Real hardware | Notes |
|---|---|---|---|
| A sensor sample's CoAP packet is actually radio-relayed through the INTERMEDIATE module's router role (genuine Thread-mesh multi-hop, not just POGO depth=2) | None -- this is a real-RF/radio-path property, not something `idf.py monitor` application logs can establish | Not verified | Requires either (a) OpenThread router/neighbor-table and route diagnostics captured separately from the application log (e.g. `ot router table`/`ot neighbor table` CLI output, or `otThreadGetNextHop()`/`otThreadGetRouterInfo()`, showing the actual next-hop router used for the LEAF's destination), or (b) a dedicated RF test that physically prevents a direct LEAF<->Power-Base radio path (shielding/distance/attenuation) so any observed delivery can only have happened via the INTERMEDIATE module's relay. Neither has been done. |

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
| TOPOLOGY export (uuid-coalesced queue, current-state-at-drain-time, periodic full resync) | Yes (`tools/test_linux_bridge_exporter.c`, `tools/test_linux_bridge_exporter_timers.c`, `tools/test_linux_bridge_exporter_retry_policy.c`) | Not yet | Links the real, unmodified `linux_bridge_exporter.c`/`power_base_transport.c` against a simulated Linux CoAP host (`tools/host_test_stubs/host_pb_bridge_full_sim.c`); no real Linux OTBR host has actually received this component's own CoAP traffic yet. |
| SAMPLE export (uuid-coalesced pending table, latest-wins, periodic full resync) | Yes (same three files above) | Not yet | Same caveat as TOPOLOGY export above. |
| CoAP retry policy (2.xx success; 4.xx final, never retried; timeout/OT-error/5.xx bounded retry, shared by TOPOLOGY and SAMPLE) | Yes (`tools/test_linux_bridge_exporter_retry_policy.c`) | Not yet | |
| **This pass**: per-module_uuid SAMPLE export rate limit (`CONFIG_LINUX_BRIDGE_EXPORTER_SAMPLE_MIN_INTERVAL_MS`, default 1000ms) -- coalescing to the latest sample, wrap-safe timing, TOPOLOGY unaffected, periodic resync unaffected | Yes (`tools/test_linux_bridge_exporter_rate_limit.c`, links the real, unmodified `linux_bridge_exporter.c`) | Not yet | See "RCP/OTBR fault recovery and transport-load reduction (this pass)" below for what this pass changed and did not change. |

## Linux Matter Bridge (`pogo_matter_bridge`) MVP (see `docs/matter-bridge.md`)

Added in a prior pass (`feat: add Linux Matter Bridge MVP (pogo_matter_bridge) over
connectedhomeip v1.6.0.0`); this repository's own "explicitly out of scope" table
below previously still listed Matter Bridge as "Not implemented" -- a stale leftover
from before that commit, corrected in this pass.

| Feature | Host test | Real hardware | Notes |
|---|---|---|---|
| `linux/matter_bridge/ingest` (CoAP ingest server, `Registry::apply_topology()`/`apply_sample()`/`apply_heartbeat()`, generation/sequence ordering, conflict-safe (power_base_uuid, module_uuid) keying) | Yes (`ingest_unit_tests`, `ingest_registry_snapshot_test`, `ingest_loopback_test`, `ingest_ack_transport_static_test` -- all pass via `cmake`+`ctest`, zero warnings) | Not yet | CHIP-SDK-independent; builds and tests with nothing beyond a C++17 compiler + CMake. |
| `linux/matter_bridge/app/core` (`value_mapping`, `EndpointStore` persistence, `BridgeManager::Reconcile()`/conflict detection/dedup) | Yes (`matter_bridge_core_unit_tests` via `cmake`+`ctest`, zero warnings) | Not yet | Also CHIP-SDK-independent -- see `core/CMakeLists.txt`'s own top comment. |
| `linux/matter_bridge/app/chip` (`chip_endpoint.cpp`/`main.cpp`, the only files that touch the real Matter SDK) | Partial -- a real `connectedhomeip v1.6.0.0` checkout compiled 449/476 GN targets with `target_os="linux"` on a macOS development sandbox, including every one of this application's own source files; the remaining 5 failures were all genuinely Linux-only headers (`<linux/netlink.h>`, `<linux/ethtool.h>`, `<netpacket/packet.h>`, glibc `fdatasync`) inside connectedhomeip's own `src/platform/Linux/*`, not this application's code -- see `docs/matter-bridge.md` section 14 for the full detail | Not yet | The final Linux-only **link** step, actually running the binary, and `chip-tool` commissioning all still require a real Ubuntu x86_64 host (or container) and have never been attempted. |
| **This pass**: `BridgeManager::TryBeginSchedule()`/`AbortSchedule()`/`EndSchedule()` (per-uuid Matter-work-item coalescing + rate limit), `CommitApplied()` (commit-only-on-confirmed-success dedup state, replacing the old eager-commit-inside-`Reconcile()` design), `ReconcileOne()` (re-derive from a fresh snapshot at actual apply time), `EndpointStore` internal locking (newly required now that `get_or_assign()` is called from two threads) | Yes (`core/tests/test_bridge_manager.cpp` and `core/tests/test_endpoint_store.cpp`, both extended this pass -- the latter adds a real multi-threaded `get_or_assign()` contention scenario; links the real, unmodified `bridge_manager.cpp`/`endpoint_store.cpp`) | Not yet | This pass re-cloned and re-bootstrapped a real `connectedhomeip v1.6.0.0` checkout and `gn gen` again resolved the identical 6256-target graph, but no Linux cross-compiler was available in this pass's sandbox (unlike the prior pass above), so `ninja` could not compile anything at all this time -- a host-toolchain difference, not a new code finding. `chip_endpoint.cpp`'s new `ApplyOutcome`-returning `Apply()` and `main.cpp`'s rewritten `RegistryWatcherThreadMain()`/`ApplyUpdateOnMatterThread()` were instead verified by manual review against the exact same API shapes the already-compiled pre-existing code used -- see `docs/matter-bridge.md` section 14's own note on this pass. |

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

**This pass's own implementation (SAMPLE/Matter-publish transport-load reduction --
NOT a `RESET_FAULT` fix, tracked as a separate, unrelated change):**

- Power Base -> Linux: `components/linux_bridge_exporter` now sends at most one
  SAMPLE export per module_uuid per `CONFIG_LINUX_BRIDGE_EXPORTER_SAMPLE_MIN_INTERVAL_MS`
  (default 1000ms), always coalescing to the single latest (seq, value) -- never a
  replayed backlog. TOPOLOGY events and the periodic full resync are unaffected.
- Linux Registry -> Matter: `matter_bridge::BridgeManager` now coalesces and
  rate-limits how often a module_uuid's changes actually reach
  `chip::DeviceLayer::PlatformMgr().ScheduleWork()`/the Matter SDK (at most one
  in-flight work item per uuid, and non-urgent SAMPLE-only changes wait at least
  `BridgeManager::kDefaultSampleUpdateInterval`, 1000ms, between schedule attempts) --
  a new-endpoint or Reachable/online-offline transition always bypasses that wait.
- **Explicitly unchanged by this pass**: sensor measurement period, POGO
  `POLL_CONFIG`, and the Power Base's own internal `sensor_root_aggregator` storage
  rate/period are all completely untouched -- every sensor sample is still measured
  and stored at exactly the same rate as before; only how often an
  ALREADY-CURRENT value is forwarded/published downstream was reduced.
- Not yet verified on real hardware -- see the per-component host-test rows above.
  ESP-IDF v6.1 `power_base` fullclean build (zero warnings) and this repository's
  full host test suite were run this pass; see this pass's own final report for the
  complete list.

## Explicitly out of scope (not implemented, not planned this pass)

| Feature | Status |
|---|---|
| Deep-sleep | Not implemented. GPIO12 was reserved as a future deep-sleep wake source (see `docs/pogo-interface.md`), but no deep-sleep code exists. |
| POGO 5V power supply over the POGO connector | Not implemented -- every board is powered independently over USB; `power_enable_gpio` is unconfigured (`power_base_main: power_enable_gpio not configured -- 5V rail assumed always-on`). |

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
