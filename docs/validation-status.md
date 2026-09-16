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
| depth=2 POGO chain (Power Base -> SHT45/BH1750 INTERMEDIATE -> PMS5003 LEAF) -- POGO docking order, depth propagation, provisioning, CoAP reachability ONLY | Code review this pass: no confirmed bugs in any of the 6 reviewed areas (existing parent/child code is already generic, not hardcoded to depth=1) | Not yet | Target of the next real-hardware pass -- see README "3단 센서 체인 실물 배선" and `tools/validate_three_tier_logs.py` for post-hoc log judging. This item is POGO/application-layer topology only -- it does NOT confirm Thread radio multi-hop (see next row). |
| Power Base topology_registry: two modules' UUIDs/sensor bitmaps never cross-assigned | Reviewed this pass -- OK, no bug | Not yet | `topology_registry_apply_report()` always looks up a node's slot by UUID equality first; a free slot is only allocated for a genuinely new UUID, and every field (including `sensor_bitmap`) is written into a freshly-cleared entry -- no reuse of a stale slot's bitmap under a different UUID. |
| A reattach on one module never corrupts Power Base session/registry state for other modules | Reviewed this pass -- OK, no bug | Not yet | `topology_registry_cascade_offline_children()`/`mark_offline()` only touch the detaching UUID and its own descendants (no global reset). `power_base_transport_on_direct_downstream_detached()` marks only `depth==1` nodes offline, which is exactly the one physically-detached node given Power Base has exactly one physical DOWNSTREAM POGO port. Each module's own `child_service.c`/`parent_service.c` session state is scoped to that module's one upstream link and one downstream port -- a reattach only resets that module's own session. |

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

## Explicitly out of scope (not implemented, not planned this pass)

| Feature | Status |
|---|---|
| Deep-sleep | Not implemented. GPIO12 was reserved as a future deep-sleep wake source (see `docs/pogo-interface.md`), but no deep-sleep code exists. |
| Linux OTBR (OpenThread Border Router) | Not implemented. |
| Matter Bridge | Not implemented. |
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
