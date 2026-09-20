# Matter bridge: design, implementation, and verification status

This document originally recorded a design-only plan (see git history for
that earlier revision) written by the follow-up that added the
**northbound transport** carrying Power Base data to a Linux OTBR host --
`components/bridge_export_proto`, `components/linux_bridge_exporter`,
`components/power_base_bridge_config`, and `linux/matter_bridge/ingest`.
This revision documents the follow-up that actually **implements** the
Matter Bridge described there: `pogo_matter_bridge`
(`linux/matter_bridge/app`), a Linux application that runs the existing
CoAP ingest server and a real `connectedhomeip v1.6.0.0` Matter stack in
one process.

**This is an MVP, not an Apple-Home-certified product.** The completion
bar for this follow-up is: builds against `connectedhomeip v1.6.0.0`,
runs the existing CoAP ingest unchanged, exposes SHT45 and PMS5003 as
dynamic Matter endpoints, commissions with `chip-tool`, reads back sensor
values and `Reachable` with `chip-tool`, and survives a restart with the
same `module_uuid -> endpoint id` mapping. See "What is and isn't verified"
at the end of this document for exactly what has and hasn't been confirmed
against real hardware/`chip-tool`/Apple Home.

## 1. What already exists

- `linux/matter_bridge/ingest` receives and validates
  `bridge_export_proto`-encoded SAMPLE/TOPOLOGY_SNAPSHOT/HEARTBEAT messages
  over CoAP/UDP and maintains a bounded, UUID-keyed, mutex-protected
  `ingest::Registry` (see
  `linux/matter_bridge/ingest/include/ingest/registry.hpp`) -- unchanged by
  this follow-up except for three new read-model methods added for the
  Matter side to consume safely: `generation()`, `snapshot()`, and
  `wait_for_change()` (see that header's own doc comment). `ingest_server`
  (the CoAP-only smoke binary) and its existing tests still build and pass
  unmodified.
- `linux/matter_bridge/app` (this follow-up): `pogo_matter_bridge`, the
  application described below.

## 2. Identity: UUID is the permanent bridged-device key

Every `module_uuid` this system carries (POGO provisioning, `sensor_channel`,
`topology_proto`, and `bridge_export_proto`) is the sensor module's own
permanent, stable identity -- never regenerated across reboots or
re-provisioning (see `docs/power-base.md` and
`components/child_service/child_service.c`'s `compute_permanent_uuid()`).
`pogo_matter_bridge` maps one `module_uuid` to one persistent Matter
dynamic endpoint, created once the first time that UUID is seen (in a
topology report -- see section 8) and never re-created for the same UUID
afterward, regardless of how many times it goes offline/online.
`power_base_uuid` is a secondary key, not the bridged-device identity
itself -- see section 7.

## 3. Channel -> Matter cluster mapping (implemented)

| `sensor_channel_type_t`      | Sensor   | Matter cluster                    | Cluster ID |
|-------------------------------|----------|-------------------------------------|------------|
| `SENSOR_CHANNEL_TEMPERATURE`  | SHT45    | Temperature Measurement             | 0x0402     |
| `SENSOR_CHANNEL_HUMIDITY`     | SHT45    | Relative Humidity Measurement       | 0x0405     |
| `SENSOR_CHANNEL_PM1_0`        | PMS5003  | PM1 Concentration Measurement       | 0x042C     |
| `SENSOR_CHANNEL_PM2_5`        | PMS5003  | PM2.5 Concentration Measurement     | 0x042A     |
| `SENSOR_CHANNEL_PM10`         | PMS5003  | PM10 Concentration Measurement      | 0x042D     |
| `SENSOR_CHANNEL_ILLUMINANCE`  | BH1750   | Illuminance Measurement             | 0x0400     |
| `SENSOR_CHANNEL_VOC_*`/`NOX_*`| SGP40/41 | *(not bridged -- see 3.1)*          | --         |

Conversion is pure, CHIP-SDK-independent logic in
`linux/matter_bridge/app/core/include/matter_bridge/value_mapping.hpp` /
`src/value_mapping.cpp`, unit-tested directly in
`core/tests/test_value_mapping.cpp` (no sockets, no Matter SDK):

- **Temperature**: `sensor_channel` milli-degC (`SENSOR_SCALE_MILLI`) ->
  Matter's 0.01 degC `int16` (`value / 10`, rounded to nearest, not
  truncated). Rejected (never clamped) if the wire scale isn't
  `SENSOR_SCALE_MILLI` or the converted value falls outside the
  attribute's own spec range `[-27315, 32767]`.
- **Humidity**: milli-%RH -> Matter's 0.01-%RH `uint16` (`value / 10`,
  rounded). Rejected if negative or above `10000` (100.00%).
- **PM1/PM2.5/PM10**: `sensor_channel` already reports these as whole
  µg/m³ (`SENSOR_SCALE_UNIT`, see `components/pms5003/pms5003.c`) --
  passed straight through as the Matter Concentration Measurement
  cluster's `MeasuredValue` (`float`, `MeasurementUnitEnum::kUgm3`,
  `MeasurementMediumEnum::kAir`). Rejected if negative or the wire scale
  isn't `SENSOR_SCALE_UNIT`. The cluster itself is additionally configured
  with `minMeasured=0`, `maxMeasured=2000` (PMS5003's own rated range with
  headroom) -- `SetMeasuredValue()` enforces that range independently (see
  `chip/chip_endpoint.cpp`).
- **Illuminance** (BH1750, added this pass): `sensor_channel` reports
  milli-lux (`SENSOR_SCALE_MILLI`, see `components/bh1750/bh1750_convert.c`).
  Matter's Illuminance Measurement `MeasuredValue` is **not** lux or
  milli-lux -- it is the CSA/ZCL log-encoded `uint16` the cluster spec
  defines (confirmed against `src/app/clusters/illuminance-measurement-server/
  IlluminanceMeasurementCluster.{h,cpp}` and
  `data_model/1.6/clusters/IlluminanceMeasurement.xml` in a real
  `connectedhomeip v1.6.0.0` checkout at exactly that tag, cloned this pass
  specifically to confirm this API and formula rather than assume them):
  `MeasuredValue = round(10000 * log10(lux)) + 1` for `lux >= 1`, and the
  spec's own `MeasuredValue = 0` "too low to be measured" sentinel for any
  `lux < 1` (including exactly 0 -- the log formula is undefined/nonsensical
  below 1 lux and is never evaluated there). Rejected (never clamped) if the
  wire scale isn't `SENSOR_SCALE_MILLI`, the raw value is negative, or the
  encoded result somehow falls outside `[1, 65534]` (the attribute's own
  absolute representable ceiling -- mathematically unreachable for any real
  32-bit non-negative milli-lux value, since even `INT32_MAX` millilux
  encodes to ~63,320, but guarded rather than assumed, same as
  `ConvertTemperature()`/`ConvertHumidity()`'s own range checks). The
  cluster itself is additionally configured with `minMeasuredValue=1`,
  `maxMeasuredValue=50000` (~99,995 lux -- generous headroom over this
  driver's own realistic ~54,612 lux ceiling at `raw=65535`,
  `correction_permille=1000`; see `chip/chip_endpoint.cpp`'s own comment)
  and `lightSensorType=Photodiode` (BH1750's own sensing element per its
  datasheet) -- `SetMeasuredValue()` enforces the min/max range
  independently, same division of responsibility as the PM clusters above.
  See `value_mapping.hpp`'s own `IlluminanceMeasuredValue` doc comment and
  `core/tests/test_value_mapping.cpp`'s `ConvertIlluminance` scenarios
  (boundary values at exactly 1 lux, just under 1 lux, exactly 0, this
  pass's own real field-test range of 18-15,273 lux -- see section 14 --
  and the near-`INT32_MAX` non-overflow case).

### 3.1 VOC/NOx: not bridged in this MVP

`SENSOR_CHANNEL_VOC_RAW`/`VOC_INDEX` (SGP40, SGP41) and
`SENSOR_CHANNEL_NOX_RAW`/`NOX_INDEX` (SGP41) are Sensirion's own **Gas
Index** values (0..500, unitless, algorithmically derived -- see
`components/sensirion_gas_index_algorithm`), not a calibrated gas
concentration in any real unit. `pogo_matter_bridge` never publishes a Gas
Index as a VOC Concentration Measurement `MeasuredValue` (that cluster's
`MeasuredValue` is defined in a real concentration unit -- µg/m³, ppm, etc
-- and a unitless 0..500 index dressed up as one would misrepresent it to
every Matter controller/app that reads it), and does not define a
manufacturer-specific cluster for it either -- `bridge_manager.cpp` simply
never creates an endpoint for a module whose `sensor_bitmap` has none of
the six bridged channel bits set (an SGP40/41-only module gets **no
Matter endpoint at all**, not an empty one), and never routes these
channel types through `Apply()` even for a module that also has a bridged
channel. This is out of scope for any future revision of this MVP too,
absent an actual calibrated concentration reading from a different
sensor -- a Gas Index number simply has no correct Matter representation
today.

### 3.2 Non-OK channel status -> null, never the stale value

`sensor_channel_status_t` distinguishes `OK` from `NOT_INITIALIZED`/
`TIMEOUT`/`BUS_ERROR`/`DRIVER_ERROR`/`CRC_ERROR`/`RANGE_ERROR`/
`CONDITIONING`. Every `Convert*()` function in `value_mapping.cpp` maps
any non-`OK` status to that reading's `Measurement::Invalid`, which
`chip/chip_endpoint.cpp` maps onto that Matter attribute's own
`DataModel::Nullable<T>` null state (`SetNull()`) -- never the last-known
numeric value republished as if it were a fresh, valid reading. Covered by
`core/tests/test_value_mapping.cpp` and, end-to-end through
`BridgeManager`, `core/tests/test_bridge_manager.cpp`.

## 4. Reachable: topology online/offline (implemented)

`NodeState::online` (from the existing `ingest::Registry`, ultimately
`topology_registry_entry_t.online`) maps directly onto Bridged Device Basic
Information's `Reachable` attribute: `online -> Reachable=true`, `!online
-> Reachable=false` (`matter_bridge::BridgeManager::Reconcile()` ->
`ChipEndpointManager::Apply()` -> `BridgedDeviceBasicInformationCluster::
SetReachable()`). Going offline does **not** remove the endpoint and does
**not** re-push (or null out) the last-known channel values -- only
`Reachable` changes, matching how a real bridge should represent "this
reading may be stale" without discarding it (see
`core/tests/test_bridge_manager.cpp`'s detach/reattach scenario).

## 5. Endpoint structure (implemented; see section 6 for why no child endpoints)

- Endpoint 0: root (fixed, `bridge-common/pogo-matter-bridge.zap`).
- Endpoint 1: Aggregator (fixed, `MA-aggregator`) -- parent of every
  endpoint `pogo_matter_bridge` creates.
- Endpoint 2: a disabled-at-runtime placeholder endpoint that only exists
  so ZAP generates Descriptor-cluster support at all (every dynamic-
  endpoint Matter bridge example needs one; see
  `examples/bridge-app/linux/README.md`'s own "Limitations" section in the
  SDK). Never visible to a controller (`ChipEndpointManager::Init()`
  disables it).
- One dynamic endpoint **per `module_uuid`**, parented to the Aggregator,
  containing:
  - **Descriptor** -- the one connectedhomeip-provided cluster this bridge
    declares the classic ember `DECLARE_DYNAMIC_ATTRIBUTE` way (see
    `chip/chip_endpoint.cpp`'s own top comment for why: it's the one
    proven-working mechanism for Descriptor on a *dynamic* endpoint in this
    SDK version, even though a modern `DescriptorCluster` class also
    exists).
  - **Bridged Device Basic Information** -- the modern v1.6.0.0
    code-driven `BridgedDeviceBasicInformationCluster`, constructed and
    registered directly (never ember-declared), `UniqueId` = the
    `module_uuid`'s own lowercase-hex encoding.
  - **Identify** (every endpoint, added this pass -- see section 5.1) --
    the modern v1.6.0.0 code-driven `IdentifyCluster`, `IdentifyType::None`
    (this bridge has no LED/speaker/actuator on the bridged Thread sensor
    module it could drive), `TriggerEffect` accepted but a no-op.
  - **Temperature Measurement** / **Relative Humidity Measurement**
    (SHT45), **Pm1/Pm25/Pm10ConcentrationMeasurement** + **Air Quality**
    (PMS5003, `AirQuality` cluster added this pass -- see section 5.1), or
    **Illuminance Measurement** (BH1750) -- whichever the module's
    `sensor_bitmap` declares, each the modern v1.6.0.0 code-driven
    `ServerCluster` class for that cluster (`TemperatureMeasurementCluster`,
    `RelativeHumidityMeasurementCluster`, `ConcentrationMeasurementCluster`,
    `AirQualityCluster`, `IlluminanceMeasurementCluster`), constructed and
    registered directly, exactly the same registration mechanism Bridged
    Device Basic Information uses. A BH1750-only endpoint's `DeviceTypeList`
    is `{Light Sensor (0x0106), Bridged Node (0x0013)}`.

### 5.1 Device type / mandatory cluster conformance audit (this pass)

Every dynamic endpoint's `DeviceTypeList` was cross-checked against
`src/app/zap-templates/zcl/data-model/chip/matter-devices.xml` in a real
`connectedhomeip v1.6.0.0` checkout (cloned fresh this pass specifically
for this audit, not assumed from a prior pass's notes) for mandatory
clusters/attributes/commands. Findings and fixes:

- **Identify was missing on every endpoint.** Temperature Sensor (0x0302),
  Humidity Sensor (0x0307), Air Quality Sensor (0x002C), and Light Sensor
  (0x0106) all mark Identify `serverLocked="true"` (mandatory, not
  optional). A prior pass's own comment rationalized this gap by analogy
  to `examples/bridge-app/linux`'s own bridged temperature sensors (which
  also omit it) -- that analogy doesn't make it conformant, only common;
  fixed this pass by creating the real v1.6.0.0 `IdentifyCluster` (a
  code-driven `ServerCluster`, not an ember/ZAP-declared one) on every
  module endpoint unconditionally. `IdentifyType` is `None` (an honest
  declaration -- these bridged Thread sensor modules have no way to
  visibly/audibly identify themselves) and `TriggerEffect` is accepted
  (satisfies Temperature/Humidity/Light Sensor's own required-command
  list) but is a no-op beyond logging, since there is no actuator to
  drive -- accepting an effect command without a matching actuator is
  ordinary, spec-sanctioned behavior for `TriggerEffect` (its own
  identifiers are advisory hints a device MAY act on), not a fabricated
  capability.
- **Air Quality Sensor's own mandatory Air Quality cluster (0x005B) was
  never implemented at all** -- every PMS5003 endpoint declared the Air
  Quality Sensor device type while only implementing the (optional, per
  that device type's own cluster list) PM1/PM2.5/PM10 Concentration
  Measurement clusters. Fixed this pass by creating the real v1.6.0.0
  `AirQualityCluster` with **zero optional features enabled**
  (`BitFlags<AirQuality::Feature>()`), which restricts every legal
  `AirQuality` attribute value to `Unknown`/`Good`/`Poor` (the SDK's own
  `SetAirQuality()` rejects `Fair`/`Moderate`/`VeryPoor`/`ExtremelyPoor`
  with `ConstraintError` unless the matching feature bit is set) -- and
  this bridge never calls `SetAirQuality()` at all, leaving it at the
  class's own default, `Unknown`, permanently. This is a deliberate
  choice, not an oversight: PMS5003 provides PM1/PM2.5/PM10 only, with no
  CO/CO2/NO2/O3/HCHO/TVOC/Radon inputs and no calibrated breakpoint table
  validated for this deployment, so deriving a Good/Fair/Moderate/Poor
  grade from PM concentration alone would be exactly the kind of
  ungrounded classification this bridge's existing SGP40/41 Gas Index
  policy (section 3.1) already refuses to produce. `Unknown` is itself a
  fully spec-legitimate, mandatory-conformance attribute value, not a
  placeholder awaiting a real computation.
- **`requireCommand IdentifyQuery`**, listed by matter-devices.xml for
  Temperature Sensor/Humidity Sensor/Light Sensor's own Identify include,
  is stale ZCL-era XML with **no corresponding command anywhere in this
  SDK's actual Identify cluster** (`data_model/1.6/clusters/Identify.xml`
  defines exactly two commands: `Identify` and `TriggerEffect` -- no
  `IdentifyQuery`). Not implemented because it does not exist in this SDK
  version, not a remaining gap.
- **Light Sensor's own nominal `Binding` include** has `server="false"`
  and is not `requireCluster`-flagged -- genuinely optional, and not
  implemented, consistent with every other device type here (this
  bridge's automation logic lives in the controller, never the bridged
  device itself).
- **Device type selection itself was re-reviewed and found correct as-is**
  for all three sensor families: Air Quality Sensor's own cluster list
  explicitly includes PM1/PM2.5/PM10 Concentration Measurement as
  legitimate optional members (confirmed in the same real XML), so it
  remains the right (and only sensible) device type for PMS5003 -- the
  gap was the missing mandatory Air Quality cluster, never the device
  type choice. Light Sensor (0x0106) for BH1750 and the
  Temperature+Humidity Sensor combination for SHT45 were unchanged.
- No `linux/matter_bridge/app/core` files changed for this audit --
  Identify's `IdentifyTime`/`TriggerEffect` state and Air Quality's fixed
  `Unknown` value are both handled entirely within
  `chip/chip_endpoint.cpp` (created once, at `CreateEndpoint()`, never
  touched again), so `matter_bridge::BridgeManager`/`ModuleUpdate`/
  `ApplyOutcome` needed no changes at all.

## 6. Dynamic-endpoint constraint investigation: no child endpoints needed

The plan this document originally recorded asked whether SHT45 (2
channels) and PMS5003 (3 channels) would each need child endpoints under a
composed parent, or could live on one endpoint. Reading
`connectedhomeip v1.6.0.0`'s own real bridge example
(`examples/bridge-app/linux/main.cpp`) resolved this: its `ComposedDevice`
example needs child endpoints **only because it puts two INSTANCES OF THE
SAME cluster ID** (two independent `TemperatureMeasurement` sensors) under
one logical device -- a real, hard Matter constraint (an endpoint's server
cluster list cannot contain the same cluster ID twice). Every channel this
bridge supports maps to a **different** cluster ID (Temperature 0x0402,
Humidity 0x0405, Pm1/Pm25/Pm10 0x042C/0x042A/0x042D -- Matter deliberately
gives every concentration-measurement species its own cluster ID for
exactly this reason: to let them all coexist on one Air Quality Sensor
endpoint). SHT45's two channels and PMS5003's three channels therefore
each fit on **one** endpoint with zero conflicts -- no child endpoints are
used. If a future channel ever needed a second instance of a cluster ID
already used on that module's endpoint, the pattern to reach for is the
composed-device one described in `examples/bridge-app/linux/main.cpp`
(`AddDeviceEndpoint(..., parentEndpointId = <the module's own endpoint>)`),
not implemented here because nothing in this MVP's channel set needs it.

## 7. `power_base_uuid`: a routing/provenance key, not a Matter identity

`ingest::Registry` keys every node by `(power_base_uuid, module_uuid)` so
this system stays correct if more than one Power Base ever feeds the same
Linux host. `power_base_uuid` never becomes part of a Matter bridged
device's identity -- see section 8 for what happens when the SAME
`module_uuid` is reported online by two DIFFERENT `power_base_uuid`s at
once (a conflict, not an identity question).

## 8. UUID -> Matter endpoint ID persistence (implemented)

`matter_bridge::EndpointStore`
(`linux/matter_bridge/app/core/include/matter_bridge/endpoint_store.hpp`)
is the ONLY thing that makes "the same `module_uuid` gets the same Matter
endpoint after a restart" true (Matter's own fabric/ACL persistence, via
the SDK's standard `--KVS` option, is separate and unrelated to this
mapping).

- Stored at `<state-dir>/endpoint_map.bin`: a small versioned binary table
  (`module_uuid[8], endpoint_id(u16 BE)` per entry) with a `pogo_crc16_ccitt()`
  trailer (the same CRC16/CCITT-FALSE this codebase already uses for
  wire-format integrity, see `components/pogo_proto/pogo_crc16.h`) -- never
  a new checksum scheme.
- **Written atomically**: build the whole table in memory, write it to
  `endpoint_map.bin.tmp`, `write()` + `fsync()` the file, `rename()` it
  onto `endpoint_map.bin` (POSIX `rename()` onto an existing path is
  atomic -- a reader only ever observes a fully-old or fully-new file,
  never a torn write), then `fsync()` the containing directory too (so the
  rename itself survives a crash, not just the file's own contents).
- **Corruption handling**: a CRC mismatch, a truncated/malformed file, or
  an internally-inconsistent one (duplicate uuid/endpoint id, an id below
  the first dynamic id) makes `load()` return `Corrupt`. `main.cpp` treats
  that as **fatal** -- it refuses to start rather than silently
  reassigning `module_uuid -> endpoint id` mappings (a controller already
  bound to the old assignment could otherwise end up bound to the wrong
  device). The operator must inspect/move the file aside by hand.
- The first ID ever handed out is `3` (0=root, 1=aggregator, 2=the
  disabled placeholder endpoint), capacity is bounded at
  `ingest::Registry::kMaxNodes` (64) -- the same bound the ingest registry
  itself already uses, so neither side of this application can be
  surprised by the other's capacity.
- **Startup behavior**: `pogo_matter_bridge` does **not** eagerly recreate
  previously-known endpoints from `EndpointStore` alone at boot. A
  restarted process's `ingest::Registry` starts empty; endpoints reappear
  (with their SAME persisted IDs) once the Power Base's own periodic full
  resync (at most 5 seconds, see `docs/power-base.md`) repopulates it --
  deliberately reusing that existing resync instead of building a separate
  device-discovery protocol, per this follow-up's own scope. This means a
  previously-bridged device is briefly absent (not merely
  `Reachable=false`) for up to ~5s after a restart -- an accepted,
  documented tradeoff, not a bug.
- Restart persistence (`core/tests/test_endpoint_store.cpp`) and
  distinct-uuid/distinct-endpoint (`core/tests/test_bridge_manager.cpp`)
  are both covered by real, passing host tests against this exact code.

### 8.1 Same `module_uuid` online under two Power Bases at once: a conflict

`matter_bridge::BridgeManager::Reconcile()` groups `ingest::Registry`
snapshot entries by `module_uuid`. Since the registry keys nodes by
`(power_base_uuid, module_uuid)`, more than one entry for the same
`module_uuid` can only come from different Power Bases -- if **more than
one is online at the same time**, `Reconcile()` records the conflict
(`ConflictedModules()`) and emits **no update at all** for that module
this round: no arbitrary "pick one" resolution, and whatever Matter state
already existed (if any) is left exactly as-is until the conflict resolves
back down to at most one online source. Covered by
`core/tests/test_bridge_manager.cpp`.

### 8.2 A module_uuid's sensor hardware changing while the bridge runs: detected, never silently kept (this pass)

This bridge's whole endpoint-creation design assumes a `module_uuid`'s
`sensor_bitmap` is fixed for that module's entire lifetime, set once, the
first time it's ever seen (section 2, section 8) -- **this remains the
policy for a real deployed module** and is not changed by anything below.
It does not hold, however, for a single physical development/test board
whose sensor is deliberately swapped while `pogo_matter_bridge` keeps
running (the exact scenario this pass's own hardware validation used: the
same ESP32-H2 board `..d357` reconfigured SHT45 -> BH1750 -> SGP40, one at
a time, without restarting the bridge in between).

Without any handling, this would be silent and permanently broken: the
endpoint already exists (via `EndpointStore`, keyed by `module_uuid`, and
in `ChipEndpointManager::modules_`) with whichever clusters the ORIGINAL
sensor declared, and the newly-reported channel type has no cluster on
that endpoint to receive it -- `SetMeasuredValue()` would never even be
called (the cluster's `LazyRegisteredServerCluster` was never
`Create()`'d), so the new reading would simply vanish with no error
anywhere.

**What this bridge actually does instead:**

- `matter_bridge::BridgeManager` records the bridgeable `sensor_bitmap`
  bits (illuminance/temperature/humidity/pm1/pm25/pm10) a module's
  endpoint was ACTUALLY created with (`ModuleUpdate::declared_sensor_bitmap`,
  persisted into `CommitApplied()`'s `LastApplied` entry only once, at
  creation). Every later `Reconcile()` pass compares the CURRENT
  topology-reported `sensor_bitmap` against that recorded value.
- A mismatch is recorded and observable via
  `BridgeManager::SensorBitmapMismatches()` (same style as
  `ConflictedModules()`), and `chip/main.cpp`'s registry-watcher thread
  logs it once per `module_uuid` (`ChipLogError`, see
  `LogNewSensorBitmapMismatches()`) -- unlike a Power-Base conflict, this
  does **not** self-resolve; it persists until the process restarts.
- While mismatched, `Reconcile()`/`ReconcileOne()` **never** produce a
  value for a channel type outside the endpoint's ORIGINAL declared set --
  the new sensor's readings are computed by `value_mapping.cpp` exactly as
  normal, but withheld before ever reaching `ChipEndpointManager::Apply()`,
  so this bridge never repeatedly calls a doomed `SetMeasuredValue()`
  against a cluster that doesn't exist. Channels that WERE part of the
  original declaration (e.g. Reachable) keep working normally throughout.
  This is a detection-and-withhold policy, not an auto-migration one --
  see `core/tests/test_bridge_manager.cpp`'s sensor-bitmap-mismatch
  scenario for the exact before/after behavior, including the "nothing to
  say" case (no field in the original declaration changed, so no
  `ModuleUpdate` is emitted at all even though the mismatch is still
  recorded) and the "a real Reachable transition still happens, but the
  new channel is still withheld" case.

**Restart procedure for development-time sensor swap testing** (this
does NOT apply to, and must never be used to justify changing, the fixed
sensor-identification policy for a real deployed module):

1. Reconfigure/reflash the physical test board with the new sensor.
2. Restart `pogo_matter_bridge` itself (the whole process -- e.g.
   `systemctl restart <the unit>`, or re-run the binary). Do **not**
   delete or edit `<state-dir>/endpoint_map.bin` -- that is exactly what
   makes the next step preserve the same numeric endpoint id.
3. On restart, `BridgeManager`'s in-memory `last_applied_`/
   `declared_sensor_bitmap` state starts empty (this state was never
   persisted -- only `EndpointStore`'s `module_uuid -> endpoint id`
   mapping is). The first `TOPOLOGY_SNAPSHOT` this `module_uuid` sends
   after restart (the Power Base's own periodic full resync, at most 5s,
   same as section 8's own startup-behavior note) is therefore seen as
   `is_new` again, and `ChipEndpointManager::CreateEndpoint()` runs fresh,
   registering whichever clusters the CURRENT `sensor_bitmap` declares --
   on the SAME persisted endpoint id (`EndpointStore::get_or_assign()` is
   idempotent per `module_uuid`), so a controller's existing binding to
   that endpoint id is preserved even though its cluster set just changed
   underneath it.
4. Re-commission is **not** required for this -- the endpoint id and the
   device's overall Matter identity (`UniqueId` = the same `module_uuid`
   hex string) are unchanged; only re-read the specific attributes that
   are now relevant (a controller that already subscribed to the old
   cluster's attributes will simply stop getting updates for it, since
   that cluster is no longer registered on the endpoint after the
   restart).

This procedure is a development-only workaround for repeatedly
reconfiguring ONE physical test board; it is not a supported production
migration path, and no code in this bridge tries to make an in-place
cluster-set change happen without a restart (see the withholding behavior
above).

## 9. Threading model

Three threads, each with exactly one job (see
`linux/matter_bridge/app/chip/main.cpp`'s own top comment for the full
detail):

1. **CoAP receive thread** -- `ingest::run_server()`, completely
   unmodified, calls only `ingest::Registry::apply_*()`. Never touches the
   Matter SDK. Every valid incoming SAMPLE/TOPOLOGY_SNAPSHOT/HEARTBEAT is
   still applied to the registry immediately, at whatever rate it arrives
   -- see section 9.1 below for what changed and what deliberately did not.
2. **Registry watcher thread** -- polls
   `ingest::Registry::wait_for_change()` on a bounded 250ms timeout (never
   a busy loop, but no longer parked indefinitely either -- see 9.1), takes
   a `snapshot()`, and runs `matter_bridge::BridgeManager::Reconcile()` on
   it (pure, CHIP-SDK-free logic, and -- this pass -- a pure READ against
   `BridgeManager`'s own dedup state, never a mutation; see 9.1). For each
   resulting `ModuleUpdate`, `BridgeManager::TryBeginSchedule()` decides
   whether to actually schedule work this round; only a module_uuid it
   admits gets its uuid (not the `ModuleUpdate` itself) boxed onto the heap
   and handed to `chip::DeviceLayer::PlatformMgr().ScheduleWork()`. This
   thread **never** calls into the Matter SDK directly, and never holds
   the registry's mutex while doing anything other than copying data out
   (see `ingest/registry.hpp`'s own top comment).
3. **Matter event-loop thread** (`ChipLinuxAppMainLoop()`) -- the only
   thread that ever touches a real Matter cluster object. The
   `ScheduleWork()` callback (`ApplyUpdateOnMatterThread()`) re-reads
   `ingest::Registry::snapshot()` itself, right there, and calls
   `BridgeManager::ReconcileOne()` to re-derive the update fresh -- never
   reusing whatever `ModuleUpdate` the registry watcher thread originally
   saw -- before calling `ChipEndpointManager::Apply()` and reporting the
   real per-field outcome back via `BridgeManager::CommitApplied()`.

Shutdown is cooperative: connectedhomeip's own `AppMain.cpp` owns
SIGINT/SIGTERM (it blocks those signals in every other thread, including
this application's own two) and calls `ApplicationShutdown()` during
teardown; that hook only flips two atomics, and `main()` joins both of
this program's threads *after* `ChipLinuxAppMainLoop()` returns -- no
signal handler of this application's own, and no blocking call inside the
shutdown hook itself.

### 9.1 SAMPLE -> Matter publish rate limit (transport-load reduction pass)

This pass reduces how often this bridge actually calls into the Matter SDK
per module_uuid, without changing the ingest Registry's own semantics at
all:

- `ingest::Registry::apply_*()` is **unchanged** -- every valid incoming
  SAMPLE/TOPOLOGY_SNAPSHOT/HEARTBEAT is still applied immediately, at
  whatever rate the Power Base -> Linux exporter actually sends it (see
  `components/linux_bridge_exporter`'s own Kconfig/top-of-file comment for
  that side's own new per-uuid send-rate limit -- a separate mechanism one
  hop upstream of this one).
- What changed is purely how often the registry watcher thread turns a
  registry change into a `ScheduleWork()` call:
  `matter_bridge::BridgeManager::TryBeginSchedule(module_uuid, urgent)`
  allows **at most one in-flight Matter work item per module_uuid**, and,
  for a **non-urgent** update (a SAMPLE-value-only change -- no new
  endpoint, no Reachable/online-offline transition), additionally requires
  at least `BridgeManager::kDefaultSampleUpdateInterval` (1000ms) since
  that uuid's own last scheduled attempt. A brand-new endpoint or a
  Reachable transition is always `urgent` and bypasses that 1000ms gate
  entirely -- it can still see the (much smaller, unavoidable)
  single-in-flight delay, which exists only to stop two concurrent
  applications for the same endpoint from racing, never to throttle
  anything.
- A module_uuid `TryBeginSchedule()` declines this round is never a lost
  update: `Reconcile()` simply re-derives the same (or by-then-newer) diff
  against the registry on the watcher thread's next ~250ms pass, and once
  the gate opens, whatever is CURRENT at that moment is what gets scheduled
  -- never a queued backlog of older values (there is nothing to queue:
  `Reconcile()`/`ReconcileOne()` always re-read the registry fresh).
- `BridgeManager::CommitApplied()` -- new this pass -- is the only thing
  that ever advances `BridgeManager`'s own last-applied dedup state, and
  only for the specific fields `ChipEndpointManager::Apply()`'s returned
  `ApplyOutcome` reports as having actually succeeded against the real SDK.
  `Reconcile()` itself no longer mutates that state (it used to, eagerly,
  before this pass) -- a failed `ScheduleWork()` call (see
  `BridgeManager::AbortSchedule()`) or a failed `SetMeasuredValue()`/
  endpoint-creation call simply leaves the corresponding field's
  last-applied value untouched, so the very next `Reconcile()`/
  `ReconcileOne()` pass re-emits it for another attempt -- no separate
  retry bookkeeping was added; the existing diff-against-last-applied
  mechanism already does this once mutation only happens on confirmed
  success.
- A brand-new process's first valid snapshot for a module_uuid is always
  `is_new_endpoint` (hence `urgent`) and is therefore never delayed by this
  gate, matching the firmware-side exporter's own "first snapshot after
  restart applies immediately" policy.
- No deadband was added: any genuinely different Temperature/Humidity/
  Pm1/Pm25/Pm10 value is still pushed the next time the gate allows it,
  exactly as before -- this pass only changes *when* an already-decided
  change is allowed to reach the SDK, never *whether* it eventually does.
- Covered by `core/tests/test_bridge_manager.cpp` (`TryBeginSchedule()`/
  `EndSchedule()`/`AbortSchedule()` gating, `CommitApplied()`'s
  retry-on-failure behavior, and `ReconcileOne()` re-deriving from a
  fresher snapshot than whatever was current at schedule time). Not yet
  re-verified against the real Matter SDK/hardware -- see
  `docs/validation-status.md`.

## 10. Matter SDK pin

Pinned to `connectedhomeip` tag **`v1.6.0.0`** exactly.
`tools/build_matter_bridge.sh` verifies `git -C "$CHIP_ROOT" describe
--tags --exact-match` equals `v1.6.0.0` before doing anything else, and
refuses to build against any other revision (a floating branch, a
different tag, an unpinned checkout). `CHIP_ROOT` is always an external
environment variable pointing at a checkout the operator manages --
`connectedhomeip` is never vendored into this repository, and this
follow-up never modifies or commits anything inside that external
checkout.

## 11. Building

```bash
# One-time: obtain and bootstrap the pinned SDK (outside this repo).
git clone --branch v1.6.0.0 --depth 1 https://github.com/project-chip/connectedhomeip.git
cd connectedhomeip && scripts/bootstrap.sh   # installs gn/ninja/zap-cli into .environment/

# On the target Linux host, also: sudo apt install pkg-config libssl-dev libglib2.0-dev

export CHIP_ROOT=/path/to/connectedhomeip
tools/build_matter_bridge.sh                 # builds into $TMPDIR/pogo_matter_bridge_build
```

See `tools/build_matter_bridge.sh`'s own top comment for the exact `gn`
args it passes (network-commissioning drivers and Java/JNI compilation are
disabled -- this bridge doesn't manage the host's own network provisioning
and never needs a JVM) and how to add more via `-- <extra gn args>`.

## 12. Running

```bash
./pogo_matter_bridge \
  --interface wpan0 \
  --port 5684 \
  --state-dir /var/lib/pogo-matter-bridge \
  --KVS /var/lib/pogo-matter-bridge/chip-kvs
```

`--interface`, `--port`, and `--state-dir` are this application's own
flags (registered as a connectedhomeip `ArgParser::OptionSet`, see
`chip/main.cpp`); every other flag is a standard connectedhomeip Linux app
flag (`--KVS`, `--discriminator`, `--passcode`, ...) -- run `--help` for
the full list. `--interface`/`--port` behave exactly as
`ingest_server`'s own (see `linux/matter_bridge/ingest/README.md`).
`--state-dir` holds ONLY this application's own `endpoint_map.bin` (see
section 8) -- point `--KVS` at a path under the same directory so one
directory holds this program's entire persistent state.

A pairing code is printed to stdout by the SDK's own standard
commissioning-setup logging -- Dataset contents, network keys, and fabric
secrets are never logged by this application (it has none of its own to
log; see `linux/matter_bridge/ingest/README.md`'s own security model,
which this application inherits unchanged).

### Example systemd unit (not installed automatically)

```ini
[Unit]
Description=Pogo Matter Bridge
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/usr/local/bin/pogo_matter_bridge --interface wpan0 --state-dir /var/lib/pogo-matter-bridge --KVS /var/lib/pogo-matter-bridge/chip-kvs
StateDirectory=pogo-matter-bridge
Restart=on-failure
User=pogo-matter-bridge

[Install]
WantedBy=multi-user.target
```

Nothing in this follow-up installs this unit or writes to `/etc` -- copy
it by hand if useful.

## 13. Commissioning and verification with `chip-tool`

Build `chip-tool` from the SAME `v1.6.0.0` checkout (never a mismatched
revision):

```bash
(cd "$CHIP_ROOT" && gn gen out/chip-tool --root=examples/chip-tool --args='...' && ninja -C out/chip-tool)
```

Then, with `pogo_matter_bridge` running on the Linux OTBR host:

```bash
chip-tool pairing onnetwork 1 <setup-pin-code-printed-by-pogo_matter_bridge>

# Read Descriptor / Bridged Device Basic Information / measurement clusters
# on whichever endpoint corresponds to a bridged module (chip-tool
# descriptor reads on the Aggregator, endpoint 1, list its parts):
chip-tool descriptor read parts-list 1 1
chip-tool descriptor read server-list 1 <module endpoint id>
chip-tool bridgeddevicebasicinformation read reachable 1 <module endpoint id>
chip-tool temperaturemeasurement read measured-value 1 <module endpoint id>
chip-tool relativehumiditymeasurement read measured-value 1 <module endpoint id>
chip-tool pm25concentrationmeasurement read measured-value 1 <module endpoint id>
chip-tool illuminancemeasurement read measured-value 1 <module endpoint id>

# Identify (every endpoint, added this pass -- section 5.1):
chip-tool identify read identify-type 1 <module endpoint id>   # expect None (0)
chip-tool identify read identify-time 1 <module endpoint id>   # expect 0 outside an active Identify

# Air Quality (PMS5003 endpoints only, added this pass -- section 5.1):
chip-tool airquality read air-quality 1 <module endpoint id>   # expect Unknown (0), always
```

`illuminancemeasurement read measured-value` returns the CSA log-encoded
`uint16` (see section 3's own Illuminance entry), not lux -- convert with
`lux = 10 ^ ((MeasuredValue - 1) / 10000)` (or `MeasuredValue == 0` means
"too low to be measured") to sanity-check it against what the module is
actually seeing. `chip-tool illuminancemeasurement read light-sensor-type
1 <module endpoint id>` should read back `Photodiode` (0).
`chip-tool airquality read air-quality` should ALWAYS read back `Unknown`
(0) -- this bridge never computes any other value (see section 5.1); any
other reading would itself be evidence of a bug, not a "better" result.

Test Vendor ID/Product ID: this application uses connectedhomeip's own
standard `config/standalone` **test** VID/PID (`chip::VendorId::TestVendor1`
/ the standalone example's own test product id) -- these are **not**
production-registered identifiers and this application makes **no** claim
of Apple Home compatibility or certification.

## 14. What is and isn't verified

**Verified in this follow-up's own development sandbox** (no Linux host,
no Docker, no CHIP_ROOT/chip-tool pre-existing -- a real `connectedhomeip
v1.6.0.0` checkout was fetched and its own `scripts/bootstrap.sh` run to
get `gn`/`ninja`/`zap-cli`):

- `zap-cli` (via `scripts/tools/zap/generate.py`) successfully processed
  `bridge-common/pogo-matter-bridge.zap` (derived from `v1.6.0.0`'s own
  `examples/bridge-app/bridge-common/bridge-app.zap`) end-to-end, producing
  real `endpoint_config.h`/`gen_config.h`/`access.h` -- the root/aggregator/
  placeholder-endpoint structure this document describes is real,
  tool-processed output, not hand-authored guesswork.
- `gn gen` fully resolved this application's ENTIRE GN dependency graph
  against the real SDK (6256 targets, 0 errors) with `target_os="linux"` --
  every GN target this application depends on
  (`examples/platform/linux:app-main`, the four cluster-server targets,
  `bridge-common`'s `chip_data_model()`, `src/lib`) exists and links
  together as declared.
- `ninja -k` (keep going past failures, for maximum signal) compiled 449 of
  476 real object files from that graph on this macOS development machine
  with `target_os="linux"`, including
  `bridged-device-basic-information-server.BridgedDeviceBasicInformationCluster.cpp.o`
  and every other cluster-server source this application directly depends
  on, `examples/platform/linux/app-main.AppMain.cpp.o`, AND -- most
  directly relevant -- **every one of this follow-up's own new/reused
  source files compiled successfully as real `.o` files against the
  genuine `v1.6.0.0` headers**: `chip/chip_endpoint.cpp`, `chip/main.cpp`
  (the two files that actually call into the Matter SDK --
  `DECLARE_DYNAMIC_ATTRIBUTE`/`emberAfSetDynamicEndpointWithEpUniqueId`,
  `BridgedDeviceBasicInformationCluster`, `TemperatureMeasurementCluster`,
  `RelativeHumidityMeasurementCluster`, `ConcentrationMeasurementCluster`,
  `CodegenDataModelProvider::Instance().Registry().Register()`,
  `PlatformMgr().ScheduleWork()`, and this application's own
  `ArgParser::OptionSet`-based `--interface`/`--port`/`--state-dir` flags),
  `core/src/{value_mapping,bridge_manager,endpoint_store}.cpp`, and the
  reused `linux/matter_bridge/ingest/src/*.cpp` +
  `components/{bridge_export_proto,pogo_proto,sensor_channel}/*.c` sources.
  The exactly **5** compile failures, and every target `ninja` couldn't
  reach because of them, were ALL inside connectedhomeip's own
  `src/platform/Linux/*` -- code this follow-up does not own or modify --
  and are ALL genuinely Linux-only headers unavailable on macOS:
  `<linux/netlink.h>` (`PlatformManagerImpl.cpp`), `<linux/ethtool.h>`
  (`DiagnosticDataProviderImpl.cpp`, `ConnectivityUtils.cpp`),
  `<netpacket/packet.h>` (`ConfigurationManagerImpl.cpp`), and glibc's
  `fdatasync` (`CHIPLinuxStorageIni.cpp`, via `TemporaryFileStream.h`).
  This is the expected, documented boundary of building a Linux-targeted
  SDK component on a non-Linux host without a Linux sysroot/cross-toolchain
  or a Linux container, neither of which was available in that sandbox --
  not a defect in this application's own code, which is why compilation of
  every file this follow-up actually wrote succeeded.

**NOT verified** (needs an actual Ubuntu x86_64 host, or Linux
container/CI, per this application's own intended production target --
see `linux/matter_bridge/ingest/CMakeLists.txt`'s own top comment):

- The final **link** step producing the actual `pogo_matter_bridge`
  binary (every object file this follow-up itself wrote compiled
  successfully, per above -- linking needs the handful of Linux-platform
  object files that only build on real Linux).
- Running `pogo_matter_bridge` against a real Power Base/OTBR.
- Commissioning with `chip-tool`, or reading any cluster attribute back
  through it.
- Endpoint ID persistence surviving an actual process restart in
  production (the equivalent logic is verified at the `EndpointStore`
  unit-test level against the exact same code, see
  `core/tests/test_endpoint_store.cpp`, but not through a real Matter
  commissioning session).
- Apple Home compatibility of any kind -- not attempted, not claimed.

**This pass's own attempt (SAMPLE/Matter-publish transport-load reduction --
`chip_endpoint.cpp`/`main.cpp`/`core/*` changes described in section 9.1):** a
fresh `connectedhomeip v1.6.0.0` checkout was cloned and bootstrapped again in
this pass's own sandbox (same macOS development machine as the prior pass
above), and `gn gen` again resolved the identical 6256-target dependency graph
with `target_os="linux"` -- but this time, no Linux cross-compiler
(`aarch64-linux-gnu-g++`/`aarch64-linux-gnu-clang++`) was available in this
sandbox at all (not even with `is_clang=true`), so `ninja` could not compile
**any** object in the graph, including files this pass did not touch --
this is a pure host-toolchain-availability difference from the prior pass
(which apparently had such a cross-toolchain available), not a regression or
a new finding about this pass's own code. This pass's `chip_endpoint.cpp`/
`main.cpp` changes were therefore verified only by careful manual review
against the exact same connectedhomeip v1.6.0.0 API shapes the
already-compiled pre-existing code in this file used (same `Cluster()`
accessor pattern, same `SetMeasuredValue()`/`SetReachable()`/`ScheduleWork()`
call sites -- only their return values and calling convention changed, no
new API surface was introduced) plus the exhaustive `core/tests/test_bridge_manager.cpp`
coverage of the underlying `BridgeManager` logic those two files call into.
A real cross-toolchain-equipped (or native Linux) re-run of `ninja -k`
against these exact two files is the top recommended follow-up before
deploying this pass's Matter-side change.

**This pass's own attempt (BH1750 -> Illuminance Measurement bridging,
sensor-bitmap-mismatch detection):**

**Pre-deploy correction pass (before this feature shipped, same overall
effort):** code review found a real bug in the pass above before any real
hardware saw it: `linux/matter_bridge/app/BUILD.gn` never gained a
dependency on `${chip_root}/src/app/clusters/illuminance-measurement-server`
(now added -- every other cluster this bridge uses was already listed
there), and `chip_endpoint.cpp` configured this cluster's own
`maxMeasuredValue` as a narrower, device-tuned `50000` (~99,995 lux)
while `matter_bridge::ConvertIlluminance()` itself accepted encoded values
up to `65534` -- real direct-sunlight illuminance (~100,000 lux, encoding
to `50001`) would have converted successfully but then been permanently
rejected by `SetMeasuredValue()`'s own `ConstraintError`, retried forever,
never reaching Matter. Fixed by sharing ONE pair of constants
(`matter_bridge::kIlluminanceMeasuredValueMin`/`Max`, in
`value_mapping.hpp`) between `ConvertIlluminance()` and
`chip_endpoint.cpp`'s `StartupConfiguration`, so the two layers can no
longer drift apart -- see `value_mapping.hpp`'s own doc comment on
`kIlluminanceMeasuredValueMax` for the full reasoning. A new regression
test (`core/tests/test_value_mapping.cpp`, the "100,000 lux direct
sunlight" scenario) asserts this against the real production constant,
not a hardcoded literal, so this specific class of drift cannot silently
reappear. **Not attempted in this correction pass either** (explicitly
deferred to the operator, who has the actual Linux OTBR host and BH1750
hardware this needs): the real `tools/build_matter_bridge.sh` final
compile+link on a real Linux host, and `chip-tool` verification that
`MeasuredValue` actually updates from a real BH1750 sample -- see this
document's own "Building"/"Running"/"Commissioning and verification"
sections (11-13) and section 8.2's restart procedure for exactly how to
run that.

- **Real hardware, confirmed (reported by the operator for this pass, not
  reproduced inside this session, which had no physical board access)**:
  a real BH1750 on the same intermediate-module position previously used
  for SHT45 (module `..d357`) produced real CoAP `SAMPLE` traffic that
  `linux/matter_bridge/ingest`'s existing, unmodified `Registry` received
  and decoded correctly -- `type=0` (`SENSOR_CHANNEL_ILLUMINANCE`),
  `scale=1` (`SENSOR_SCALE_MILLI`), `status=0` (OK), values swinging from
  18 to 15,273 lux under real shading/lighting changes. The existing
  PMS5003-terminal 3-tier chain (Power Base -> intermediate -> PMS5003
  LEAF) continued working with BH1750 in the intermediate position, and a
  real SGP40 separately produced VOC RAW/INDEX samples the same ingest
  path also received correctly. **This confirms the real sensor hardware,
  its firmware driver, the wire format, and Linux ingest reception** --
  it does NOT confirm anything below this bullet, since no
  `pogo_matter_bridge` process (the `chip/` binary that actually creates
  Matter endpoints) was run against this hardware.
- **Source-verified, not build-verified, this pass**: this pass cloned the
  real `connectedhomeip` repository at exactly tag `v1.6.0.0` (sparse
  checkout, verified via `git describe --tags --exact-match` = `v1.6.0.0`)
  specifically to read `IlluminanceMeasurementCluster.{h,cpp}`,
  `illuminance-measurement-server/tests/TestIlluminanceMeasurementCluster.cpp`,
  `matter-devices.xml` (Light Sensor device type, 0x0106), and
  `data_model/1.6/clusters/IlluminanceMeasurement.xml` (the formal
  attribute constraints) directly, rather than assume the CSA log-encoding
  formula, valid ranges, or constructor/`SetMeasuredValue()` signatures
  from general knowledge or from the already-compiled sibling clusters'
  shapes (the prior pass's own stated method, see the paragraph above).
  `chip_endpoint.{hpp,cpp}`'s new `IlluminanceMeasurementCluster` wiring
  (constructor `StartupConfiguration`, `OptionalAttributeSet`,
  `SetMeasuredValue(DataModel::Nullable<uint16_t>)`) is a direct match to
  that real source, not a guess.
- **NOT attempted this pass**: cloning the FULL (non-sparse)
  `connectedhomeip v1.6.0.0` tree, `gn gen`, `ninja`, linking, running the
  actual `pogo_matter_bridge` binary, or `chip-tool` commissioning/reads --
  none of these were run, so none of them are claimed. This is a stricter
  scope cut than the prior two passes above (which did attempt `gn
  gen`/`ninja` against a full checkout); it was made deliberately given
  this pass's own time/resource budget, favoring exact-source API
  verification over a partial build attempt. **The single highest-value
  follow-up is exactly what section 11-13 already describe**: a real
  Ubuntu x86_64 (or Linux container) build + `chip-tool` run, now
  additionally exercising `illuminancemeasurement read measured-value`
  (section 13) against real BH1750 hardware.
- **Apple Home / Google Home**: Illuminance Measurement (0x0400) and the
  Light Sensor device type (0x0106) are both mandatory, standard Matter
  elements (not manufacturer-specific) -- so, as a matter of protocol
  conformance, a Matter controller that already renders an ambient-light
  sensor for a standard Light Sensor endpoint should render this one the
  same way. That is a **standard-cluster-conformance expectation**, not an
  observed result: this session did not pair this bridge with an actual
  Apple Home or Google Home hub, and this document makes no claim about
  either app's specific UI for this cluster. Do not conflate the two --
  the distinction docs/validation-status.md's Sensors table also
  preserves.
- Host-test coverage (CHIP-SDK-independent, see section 15): the CSA
  log-encoding boundary values (exactly 1 lux, just under 1 lux, exactly
  0, this pass's own real 18-15,273 lux field range, and the
  near-`INT32_MAX` non-overflow case), invalid type/scale/status/negative
  rejection, a brand-new BH1750-only endpoint's capability flags and
  initial value, and the sensor-bitmap-mismatch detection/withholding
  behavior (section 8.2) are all covered by
  `core/tests/test_value_mapping.cpp` and
  `core/tests/test_bridge_manager.cpp`, extended this pass -- 133/133
  checks pass, 0 warnings, including under AddressSanitizer.

**Real-hardware confirmation (reported by the operator, a later pass than
all of the above -- the operator ran the real final Linux build/link and
`chip-tool` themselves; not reproduced inside any session, none of which
had physical board or real Linux host access):**

- `tools/build_matter_bridge.sh` completed a real final compile **and
  link** against the pinned `connectedhomeip v1.6.0.0` SDK on a real Linux
  host, producing an actual runnable `pogo_matter_bridge` binary -- the
  first time this exact step (as opposed to `gn gen`/partial `ninja -k` on
  a non-Linux sandbox, see the passes above) has been confirmed for this
  application.
- With that binary running against real BH1750 hardware, `chip-tool
  illuminancemeasurement read measured-value 1 3` (module endpoint **3**
  -- the very first ID `EndpointStore::kFirstDynamicEndpointId` ever hands
  out, confirming that numbering in real use, not just in
  `core/tests/test_endpoint_store.cpp`) returned values ranging from **0
  to 22369** as real lighting conditions changed -- `0` correctly
  reflecting the spec's own "too low to be measured" sentinel (dark/heavily
  shaded) and `22369` decoding to `10^((22369-1)/10000) ≈ 172.5` lux (a
  plausible indoor-lighting reading). This confirms, for the first time on
  real hardware: the final build+link succeeds, `pogo_matter_bridge`
  actually commissions and serves a real Illuminance Measurement cluster,
  and `MeasuredValue` genuinely updates from live BH1750 samples through
  the full pipeline (BH1750 -> Power Base -> Linux ingest ->
  `ConvertIlluminance()` -> `IlluminanceMeasurementCluster::SetMeasuredValue()`
  -> `chip-tool` read).
- **Scope of what this does NOT yet confirm**: this observation predates
  this pass's own Identify/Air Quality cluster additions (section 5.1) --
  those are new code this exact pass, not yet exercised by any real build
  or `chip-tool` read (see immediately below). It also does not by itself
  confirm the sensor-bitmap-mismatch/restart procedure (section 8.2) --
  no sensor swap was reported alongside this observation. Apple Home /
  Google Home rendering remains a separate, still-unconfirmed question
  (see this document's own note above and `docs/validation-status.md`'s
  Sensors table) -- not reported either way this round.

**Later operator verification of the device-type/mandatory-cluster additions
(Identify on dynamic endpoints, Air Quality on PMS5003 -- section 5.1):**
On the real Linux host, `tools/build_matter_bridge.sh` completed its final
link against connectedhomeip v1.6.0.0 after these additions. The updated
bridge ran with its existing commissioned fabric and persistent endpoint
IDs. `chip-tool identify read identify-time 1 3` (BH1750) and `... 1 4`
(PMS5003) each returned `IdentifyTime: 0`; `chip-tool identify identify 2
1 3` and `... 1 4` both completed with Matter status `SUCCESS`. The PMS5003
endpoint returned `AirQuality: 0` (`Unknown`) from `chip-tool airquality
read air-quality 1 4`, exactly as designed. This verifies the tested
endpoints and commands, not SHT45's Identify endpoint, `TriggerEffect`,
formal Matter certification, or any particular Apple Home/Google Home UI.

## 15. Test coverage summary

Run `tools/test_matter_bridge.sh` for everything that doesn't need
`CHIP_ROOT` (regression coverage for `linux/matter_bridge/ingest` plus all
of `linux/matter_bridge/app/core`'s own tests -- unit conversion,
persistence, reconciliation/conflict detection, dedup). See
`tools/build_matter_bridge.sh` and section 13 above for what still needs a
real Linux host + `chip-tool`.
