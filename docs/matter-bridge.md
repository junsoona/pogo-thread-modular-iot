# Matter bridge mapping plan (not yet implemented)

This document is written by the follow-up that adds the **northbound
transport** carrying Power Base data to a Linux OTBR host --
`components/bridge_export_proto`, `components/linux_bridge_exporter`,
`components/power_base_bridge_config`, and `linux/matter_bridge/ingest` (see
`docs/power-base.md` and this follow-up's own final report for the full
design). It does **not** implement a Matter Bridge, any Matter endpoint, or
any Apple Home integration -- those are explicitly out of scope for this
session. This document exists so that work, when it happens, starts from a
recorded plan instead of guessing at conventions the transport layer below
it already established.

## 1. What already exists vs. what this document plans

Already implemented and tested (this follow-up):

- `linux/matter_bridge/ingest` receives and validates
  `bridge_export_proto`-encoded SAMPLE/TOPOLOGY_SNAPSHOT/HEARTBEAT messages
  over CoAP/UDP and maintains a bounded, UUID-keyed, mutex-protected
  `ingest::Registry` (see `linux/matter_bridge/ingest/include/ingest/registry.hpp`).

Planned, NOT implemented here:

- A Matter Bridge process (or a mode of the ingest program itself) that
  reads `ingest::Registry` and exposes each Power Base's known sensor
  modules as Matter bridged devices.
- Any `chip-tool`/Apple Home commissioning flow.

The plan below is scoped to the mapping decisions that the wire format and
registry design already lock in, so a future implementation does not have
to revisit them.

## 2. Identity: UUID is the permanent bridged-device key

Every `module_uuid` this system carries (POGO provisioning, `sensor_channel`,
`topology_proto`, and now `bridge_export_proto`) is the sensor module's own
permanent, stable identity -- never regenerated across reboots or
re-provisioning (see `docs/power-base.md` and
`components/child_service/child_service.c`'s `compute_permanent_uuid()`).
This is exactly the property a Matter bridged device's own persistent
identifier needs: **a future Matter Bridge maps one `module_uuid` to one
persistent bridged-device entry**, created once the first time that UUID is
seen and never re-created for the same UUID afterward, regardless of how
many times it goes offline/online or how its topology position (parent,
depth) changes.

`power_base_uuid` is a secondary key, not the bridged-device identity itself
-- see section 6.

## 3. Channel -> Matter cluster mapping

`sensor_channel_type_t` (`components/sensor_channel/include/sensor_channel/sensor_channel.h`)
enumerates every channel type this system currently knows about. The planned
mapping to standard Matter clusters:

| `sensor_channel_type_t`  | Sensor      | Matter cluster                              |
|---------------------------|-------------|----------------------------------------------|
| `SENSOR_CHANNEL_ILLUMINANCE` | BH1750   | Illuminance Measurement                       |
| `SENSOR_CHANNEL_TEMPERATURE` | SHT45    | Temperature Measurement                       |
| `SENSOR_CHANNEL_HUMIDITY`    | SHT45    | Relative Humidity Measurement                 |
| `SENSOR_CHANNEL_PM1_0`       | PMS5003  | PM1 Concentration Measurement                 |
| `SENSOR_CHANNEL_PM2_5`       | PMS5003  | PM2.5 Concentration Measurement               |
| `SENSOR_CHANNEL_PM10`        | PMS5003  | PM10 Concentration Measurement                |

Each of the three PM channels maps to its own standard Concentration
Measurement cluster instance (Matter defines PM1/PM2.5/PM10 as distinct
clusters, not one cluster with three attributes) -- a single permanently
paired PMS5003 module therefore becomes one bridged endpoint composed of
three such cluster instances (see section 7).

### 3.1 VOC/NOx: explicitly NOT a TVOC concentration

`SENSOR_CHANNEL_VOC_RAW`/`SENSOR_CHANNEL_VOC_INDEX` (SGP40, SGP41) and
`SENSOR_CHANNEL_NOX_RAW`/`SENSOR_CHANNEL_NOX_INDEX` (SGP41 only) are
Sensirion's own **Gas Index** values (0..500, unitless, algorithmically
derived -- see `components/sensirion_gas_index_algorithm`), not a calibrated
gas concentration in any real unit (ppb, mg/m3, ...). Matter's VOC
Concentration Measurement cluster expects an actual concentration value.
**A future Matter Bridge must NOT publish a Gas Index value as if it were a
VOC Concentration Measurement's `MeasuredValue`** -- doing so would present
a number that looks like a real concentration to any Matter client (Apple
Home included) but is not one, and is not on the same numeric scale as a
genuine sensor's concentration reading. If a future revision wants to expose
these channels via Matter at all, the honest options are:
  - a manufacturer-specific cluster/attribute clearly labeled as a Gas
    Index (0..500), never reusing the standard concentration cluster's
    semantics, or
  - omitting VOC/NOx channels from the Matter bridge entirely until a
    calibrated concentration is available.
`SENSOR_CHANNEL_VOC_RAW`/`SENSOR_CHANNEL_NOX_RAW` (the pre-algorithm raw
ticks) are even less meaningful outside this codebase and should never be
bridged at all.

### 3.2 Non-OK channel status must not look like a valid measurement

`sensor_channel_status_t` (same header) distinguishes `OK` from
`NOT_INITIALIZED`/`TIMEOUT`/`BUS_ERROR`/`DRIVER_ERROR`/`CRC_ERROR`/
`RANGE_ERROR`/`CONDITIONING`. A future Matter Bridge must map a non-`OK`
status to that cluster attribute's own "no valid measurement" representation
(e.g. a null `MeasuredValue`, per the Matter spec's own nullable-attribute
convention) -- **never** publish the last-known numeric `value` field as
though it were current and valid while status is anything other than `OK`.
This matters especially for `SGP4x`'s `CONDITIONING` status (warm-up: a
`value` is present in the wire format, but Sensirion's own algorithm
explicitly does not consider it trustworthy yet -- see
`components/sensirion_gas_index_algorithm`'s own documentation) and for any
`*_ERROR` status, where the last-stored `value` could be stale by an
arbitrary amount of time.

## 4. Reachable: topology online/offline

`bridge_export_topology_node_t.online` (sourced from
`topology_registry_entry_t.online` via `power_base_transport`'s bridge
observer -- see `components/power_base_transport/power_base_transport.h`)
is planned to map directly onto Matter's Bridged Device Basic Information
cluster's `Reachable` attribute for that UUID's bridged endpoint: `online`
-> `Reachable=true`, `!online` -> `Reachable=false`. This already reflects
every real-hardware offline cause this system detects (a direct DOWNSTREAM
POGO detach, a cascaded subtree offline, or a stale-timeout sweep -- see
`docs/power-base.md`), so no additional liveness logic should be needed on
the Matter side beyond mirroring this one boolean.

## 5. Composed bridged endpoint per UUID

A single permanently-paired sensor can expose more than one channel (SHT45:
temperature + humidity; PMS5003: three PM channels) under the exact same
`module_uuid`. The planned mapping is Matter's own **composed bridged
device** pattern: one bridged endpoint per `module_uuid`, with one child
endpoint (or one cluster instance directly on the bridged endpoint, per
whatever the chosen Matter SDK's bridge example structures it as) per
channel that UUID's `sensor_bitmap` declares. This mirrors
`sensor_channel_reading_t`'s own array-of-channels-per-sample shape (see
`components/sensor_channel/include/sensor_channel/sensor_channel.h`) instead
of inventing a separate per-channel identity scheme.

## 6. `power_base_uuid`: a routing/provenance key, not a Matter identity

Every `bridge_export_proto` message type this follow-up defines
(`components/bridge_export_proto/include/bridge_export_proto/bridge_export_proto.h`)
carries `power_base_uuid`, and `ingest::Registry` keys every node by the pair
`(power_base_uuid, module_uuid)` (see
`linux/matter_bridge/ingest/include/ingest/registry.hpp`). This is
intentional: it keeps this system correct if a future deployment ever has
more than one Power Base feeding the same Linux ingest host. It is **not**
meant to become part of a Matter bridged device's own identity -- a sensor
module's `module_uuid` alone is its permanent identity (section 2); which
Power Base currently relays its data is provenance/routing information a
future Matter Bridge may want to log or expose as an optional attribute, but
should never fold into the bridged endpoint's own persistent key (a module
physically re-docked under a different Power Base must still resolve to the
SAME bridged Matter device).

## 7. Matter SDK pin (planned, not yet done)

When Matter Bridge implementation work starts, this codebase will pin the
Matter SDK to the official **`connectedhomeip` `v1.6.0.0`** tag, and build
`chip-tool` from that exact same revision (never a floating branch, and
never a mismatched chip-tool/SDK pair) -- so commissioning and cluster
behavior are tested against the exact bits the bridge itself links. This is
a forward-looking commitment recorded here per this follow-up's own scope,
not something this session builds or verifies.
