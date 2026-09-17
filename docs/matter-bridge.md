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
| `SENSOR_CHANNEL_ILLUMINANCE`  | BH1750   | *(not bridged in this MVP)*         | --         |
| `SENSOR_CHANNEL_VOC_*`/`NOX_*`| SGP40/41 | *(not bridged in this MVP)*         | --         |

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

### 3.1 VOC/NOx and illuminance: not bridged in this MVP

`SENSOR_CHANNEL_VOC_RAW`/`VOC_INDEX` (SGP40, SGP41) and
`SENSOR_CHANNEL_NOX_RAW`/`NOX_INDEX` (SGP41) are Sensirion's own **Gas
Index** values (0..500, unitless, algorithmically derived -- see
`components/sensirion_gas_index_algorithm`), not a calibrated gas
concentration in any real unit. `pogo_matter_bridge` never publishes a Gas
Index as a VOC Concentration Measurement `MeasuredValue`, and does not
define a manufacturer-specific cluster for it either -- `bridge_manager.cpp`
simply never creates an endpoint for a module whose `sensor_bitmap` has
none of the five bridged channel bits set (a BH1750-only or
SGP40/41-only module gets **no Matter endpoint at all**, not an empty
one), and never routes these channel types through `Apply()` even for a
module that also has a bridged channel. `SENSOR_CHANNEL_ILLUMINANCE`
(BH1750) is left for a future revision (Illuminance Measurement cluster,
0x0400) -- explicitly out of scope here.

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
  - **Temperature Measurement** / **Relative Humidity Measurement**
    (SHT45) or **Pm1/Pm25/Pm10ConcentrationMeasurement** (PMS5003) --
    whichever the module's `sensor_bitmap` declares, each the modern
    v1.6.0.0 code-driven `ServerCluster` class for that cluster
    (`TemperatureMeasurementCluster`, `RelativeHumidityMeasurementCluster`,
    `ConcentrationMeasurementCluster`), constructed and registered
    directly, exactly the same registration mechanism Bridged Device Basic
    Information uses.

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

## 9. Threading model

Three threads, each with exactly one job (see
`linux/matter_bridge/app/chip/main.cpp`'s own top comment for the full
detail):

1. **CoAP receive thread** -- `ingest::run_server()`, completely
   unmodified, calls only `ingest::Registry::apply_*()`. Never touches the
   Matter SDK.
2. **Registry watcher thread** -- blocks on
   `ingest::Registry::wait_for_change()` (a `condition_variable` wait,
   never a busy/polling loop), takes a `snapshot()`, and runs
   `matter_bridge::BridgeManager::Reconcile()` on it (pure, CHIP-SDK-free
   logic). Every resulting `ModuleUpdate` is hex-boxed onto the heap and
   handed to `chip::DeviceLayer::PlatformMgr().ScheduleWork()` -- this
   thread **never** calls into the Matter SDK directly, and never holds
   the registry's mutex while doing anything other than copying data out
   (see `ingest/registry.hpp`'s own top comment).
3. **Matter event-loop thread** (`ChipLinuxAppMainLoop()`) -- the only
   thread that ever touches a real Matter cluster object, via
   `ChipEndpointManager::Apply()` called from the `ScheduleWork()`
   callback.

Shutdown is cooperative: connectedhomeip's own `AppMain.cpp` owns
SIGINT/SIGTERM (it blocks those signals in every other thread, including
this application's own two) and calls `ApplicationShutdown()` during
teardown; that hook only flips two atomics, and `main()` joins both of
this program's threads *after* `ChipLinuxAppMainLoop()` returns -- no
signal handler of this application's own, and no blocking call inside the
shutdown hook itself.

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
```

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

## 15. Test coverage summary

Run `tools/test_matter_bridge.sh` for everything that doesn't need
`CHIP_ROOT` (regression coverage for `linux/matter_bridge/ingest` plus all
of `linux/matter_bridge/app/core`'s own tests -- unit conversion,
persistence, reconciliation/conflict detection, dedup). See
`tools/build_matter_bridge.sh` and section 13 above for what still needs a
real Linux host + `chip-tool`.
