# PogoHome Local API v1

> 공개용 기록: 보드 식별값·계정·포트·주소는 예시로 바꿨습니다. 명령 실행 시 자신의 실제 값으로 바꾸세요. 원본 근거는 비공개로 보존했습니다.


An HTTP/SSE API (read-only for sensor data; see "Home settings" for the
TLS-only settings-sharing routes), served by the existing `pogo_matter_bridge` process
itself (see [`matter-bridge.md`](matter-bridge.md)), that lets a client on the
same LAN -- the future PogoHome iOS app, `curl`, or anything else -- read the
same sensor/topology state `pogo_matter_bridge` already bridges onto Matter,
without needing a Matter controller at all.

This is **not** a second data pipeline. It is a thin, read-only view directly
over the exact same `ingest::Registry` the ingest CoAP server and the Matter
bridge logic already share (see
[`linux/matter_bridge/ingest/include/ingest/registry.hpp`](../linux/matter_bridge/ingest/include/ingest/registry.hpp)).
Implementation: [`linux/matter_bridge/app/api/`](../linux/matter_bridge/app/api/).

## Trust boundary and bind policy

- **Sensor data is read-only.** `/api/v1/health`, `/api/v1/snapshot` and
  `/api/v1/events` are `GET` only. There is no way to change Registry state or
  `pogo_matter_bridge`'s own configuration through this API.
- **Sensor routes need no authentication.** Whoever can reach them can read
  them, over plain HTTP or TLS. Bind the API to a trusted private LAN
  interface only -- **never** expose it beyond your home network (no
  port-forwarding, no public reverse proxy).
- **Home settings need a paired device and TLS.** The shared home name, rooms
  and sensor names/rooms (see "Home settings") are only readable and writable
  over TLS, with a bearer token that a device receives by pairing with a
  one-time code the operator creates on the hub. Finding the hub over Bonjour
  grants nothing. Neither codes nor tokens are ever sent over plain HTTP,
  logged, or put in the Bonjour TXT record.
- **Power Base enrollment is off unless configured** (`--api-enroll-helper`)
  and then has exactly the settings routes' boundary: TLS only, a paired
  device's bearer token, and each session visible only to the device that
  started it. See "Power Base enrollment".
- **Bind policy: one explicit interface's own address, never
  `0.0.0.0`/`::`.** The server binds to the *actual LAN address* of the
  interface named by `--api-interface` -- resolved via `getifaddrs()`
  (IPv4 preferred; falls back to the interface's first non-link-local,
  non-loopback IPv6 address). It never binds to "all interfaces". If the
  named interface does not exist, has no usable address, or the port cannot
  be bound, `pogo_matter_bridge` refuses to start at all (a clear error on
  stderr, non-zero exit) -- it never silently disables the API or falls back
  to listening everywhere.
- **Disabled by default.** Omit `--api-interface` and this API does not
  exist: no listening socket, no extra threads, byte-for-byte the same
  process behavior as before this feature existed.
- This is deliberately a **different** interface than `--interface` (the
  Thread/OTBR interface the CoAP ingest server listens on) in the normal
  case -- e.g. `--interface wpan0 --api-interface wlp5s0` -- though nothing
  technically prevents pointing both at the same interface if that's ever
  the right topology for a given host.

## Enabling it

New `pogo_matter_bridge` options (both optional; the process behaves exactly
as it always has if `--api-interface` is omitted):

```text
--api-interface <ifname>   Linux network interface to bind the API server to (e.g. wlp5s0). Disabled if omitted.
--api-port <port>          TCP port, 1..65535 (default: 8787). Only meaningful with --api-interface.
--api-no-advertise         Do not announce the API over Bonjour (see "Bonjour" below). Advertised by default.
--api-no-settings          Do not offer home settings sharing (see "Home settings" below). Offered by default.
--api-enroll-helper <path> Enable Power Base enrollment through this root-run OTBR helper (see "Power Base enrollment"). Off by default.
--api-enroll-window <s>    Joiner window per enrollment, 30..300 seconds (default: 120).
```

`8787` was chosen because it collides with neither the ingest CoAP port
(`5684` default, `--port`) nor Matter's own UDP/TCP port (`5540`).

Example (substitute your host's real LAN interface name, e.g. from
`ip -4 addr`):

```text
pogo_matter_bridge --interface wpan0 --state-dir /var/lib/pogo-matter-bridge \
  --api-interface wlp5x0 --api-port 8787
```

### Adding this to the systemd unit

This follow-up does **not** modify `/etc/systemd/system/pogo-matter-bridge.service`
or restart the running service -- an operator adds the two flags to that
unit's `ExecStart=` line by hand (then `systemctl daemon-reload && systemctl
restart pogo-matter-bridge`) once ready:

```ini
ExecStart=/usr/local/bin/pogo_matter_bridge --interface wpan0 \
  --state-dir /var/lib/pogo-matter-bridge --KVS /var/lib/pogo-matter-bridge/chip-kvs \
  --api-interface wlp5x0 --api-port 8787
```

## Endpoints

All responses: `Cache-Control: no-store`. JSON responses:
`Content-Type: application/json; charset=utf-8`. Every route below is `GET`
only -- any other method on a known path is `405 Method Not Allowed`; any
unknown path is `404 Not Found`.

### `GET /api/v1/health`

```json
{
  "schema_version": 1,
  "service": "pogohome",
  "registry_generation": 42,
  "module_count": 2,
  "power_base_count": 1,
  "hub_id": "3f0c9a4e8b1d4c2a9e7f6b5a4d3c2b1a"
}
```

`hub_id` (Bonjour revision, additive) is this hub's stable identity: 32
lowercase hex digits generated once from `/dev/urandom` and kept in
`<--state-dir>/hub_id` (mode 0600, next to `endpoint_map.bin`). It survives
restarts and never depends on the IP address, port, host name or Bonjour
service name. A client compares it with the `hub_id` of the Bonjour
announcement it followed (or the one it remembers) and refuses to connect
when they differ. A `hub_id` file that exists but is malformed stops the
bridge at startup instead of being replaced, like a damaged
`endpoint_map.bin`: a new id would make every client that remembers the hub
stop recognizing it. Deleting the file on purpose gives the hub a new
identity, which clients treat as a different hub.

`registry_generation` is `ingest::Registry::generation()` -- a counter bumped
once per genuinely-new topology/sample applied (never for a duplicate or
rejected report), and once when a Power Base is first heard from or changes
between online and offline (see "Power Bases" below). `module_count` counts
distinct modules the Registry currently has topology for (regardless of
online/offline) -- the same set `/api/v1/snapshot` lists. `power_base_count`
(multi-Power-Base revision) counts the Power Bases in the snapshot's
`power_bases` array.

`build` (version revision, 2026-10-06, additive and optional) says which
build of the bridge is answering:

```json
"build": {
  "component": "linux_bridge",
  "version": "0.1.0",
  "build_id": "590e2a11d2cb+dirty.cfbfdd18c5bd",
  "build_kind": "development"
}
```

It is always the last field of the health object. `version` is the
component's product version from `versions.json`; `build_id` is
`<commit, 12 digits>[+dirty].<source fingerprint, 12 digits>` (or
`unknown.<fingerprint>` for a build from a tree without Git); `build_kind` is
`development` or `release` (`docs/versioning.md`). The same values are printed
by `pogo_matter_bridge --version` and `pogohome-hubctl --version`, which
answer before the Matter stack, the state directory or any socket is touched.

`build` is **identification only**. Nothing may be decided from it: what a
bridge supports is said by `schema_version`, by the presence of the optional
fields, and by the capability values of the routes that have them. A bridge
that was built without the version stamp, and every bridge from before this
revision, serves no `build` at all; a client must work the same way without
it. PogoHome iOS decodes it leniently (a missing, `null` or malformed `build`
is "not known", never an error) and does not show or use it for any decision.
The field carries no board version: the bridge does not know the firmware
versions of Power Bases or modules, and no protocol reports them.

### `GET /api/v1/snapshot`

The full current state in one response:

```json
{
  "schema_version": 1,
  "generated_at_unix_ms": 1732000000000,
  "registry_generation": 42,
  "power_bases": [
    {"uuid": "020000fffe650a00", "online": true, "module_count": 1, "online_module_count": 1}
  ],
  "modules": [
    {
      "uuid": "020000fffe65d357",
      "power_base_uuid": "020000fffe650a00",
      "parent_uuid": "020000fffe650a00",
      "online": true,
      "depth": 1,
      "role": "intermediate",
      "sensor_bitmap": 6,
      "generation": 1,
      "last_seq": 123,
      "readings": [
        {
          "kind": "temperature",
          "raw_value": 23400,
          "scale": 1,
          "value": 23.400,
          "unit": "celsius",
          "status": "ok",
          "sample_seq": 123
        }
      ]
    }
  ]
}
```

`generated_at_unix_ms` is this *response's* generation time (wall clock,
`system_clock`) -- not a measurement timestamp; the Registry does not store
one, and this API never fabricates one for an individual reading.

**Where a snapshot stands (Linux bridge 0.5.0).** Every snapshot -- this
answer and every `snapshot` event of `GET /api/v1/events` -- ends with two
more fields, after every other field (also after the settings fields):

```json
"snapshot_stream": "9c1f…32 lowercase hex…", "snapshot_sequence": 41
```

- `snapshot_stream` is made when the bridge process starts: one value per
  run. Not a secret, and not the `hub_id`.
- `snapshot_sequence` counts the snapshots of that run in the order they
  were **made**: 1, 2, 3, …, `GET` answers and events in one line. The
  bridge makes its snapshots one at a time and reads everything a snapshot
  says inside that section (the Registry, management, the settings
  revision), so of two snapshots of one stream the one with the larger
  sequence says nothing older than the other -- whichever reaches a client
  first. The number is the order of making, not a version of the state: a
  `GET` when nothing changed still gets the next number, and so does a
  snapshot in which only the settings revision changed.

A client that follows the hub over a `GET` poll **and** the event stream
(separate connections: what arrives later is not always what was said
later) must not apply a snapshot whose sequence is not larger than the
largest one it applied of the same stream. Across two streams the numbers
say nothing: another stream is another run of the bridge, and which run is
the later one is not said here (a client knows it from the order in which
it started its own requests; see
[`power-base-management.md`](power-base-management.md), section 16). No
time is compared: `generated_at_unix_ms` is the hub's clock and can be set
back. A hub that predates the fields sends neither; its snapshots can only
be taken in the order they arrive.

#### Module fields

| field           | source (`ingest::NodeState`)         | notes |
|-----------------|---------------------------------------|-------|
| `uuid`          | `module_uuid`                         | full 8 bytes, lowercase hex, 16 characters -- never truncated |
| `power_base_uuid` | `power_base_uuid`                   | the Power Base this module is docked under (multi-Power-Base revision) -- see "Power Bases" |
| `parent_uuid`   | `parent_uuid`                         | the module's POGO parent: equal to `power_base_uuid` for a module docked directly on the Base (depth 1), otherwise the upstream module's `uuid` (multi-Power-Base revision) |
| `online`        | `online`                              | |
| `depth`         | `depth`                               | hops from the power base; the power base's direct child reports `1` |
| `role`          | `role` (`topology_role_t`)            | `unassigned` \| `root` \| `intermediate` \| `leaf` \| `error` |
| `sensor_bitmap` | `sensor_bitmap`                       | this module's fixed `sensor_type_bitmap`, as the raw integer |
| `generation`    | `boot_session_generation`             | this module's own topology session generation (an opaque per-boot token -- see `topology_registry.h`) -- **not** the same counter as the top-level `registry_generation` |
| `last_seq`      | `last_seq`                            | topology report sequence |
| `readings`      | `last_channels` (only if `has_sample`)| `[]` if no sample has ever been received for this module |

A module the Registry has never received a topology report for cannot exist
in `nodes_` at all (see `registry.hpp`'s own apply rules), so every module
object here always has a valid `uuid`/`online`/`depth`/`role`/`sensor_bitmap`/
`generation`/`last_seq`. **Module and room names are not yet tracked by the
Linux Registry at all** -- this API does not invent them; a future milestone
that adds user-assigned names will add those fields once there is a real
place to read them from.

#### Power Bases (multi-Power-Base revision)

One Linux bridge instance receives from any number of Power Bases: the ingest
CoAP server answers every datagram on its interface/port regardless of
sender, and every `bridge_export_proto` message (SAMPLE, TOPOLOGY_SNAPSHOT,
HEARTBEAT, TOPOLOGY_REMOVED) has always carried the sending Base's
`power_base_uuid` -- the Base's own eFuse EUI-64
(`power_base/main/app_main.c`). No firmware or wire-format change was needed.

**Where module membership comes from.** A module belongs to the Base whose
`topology_registry` accepted its `/t` report and exported it. The module only
sends to the collector it was given over the POGO wire (`SET_COLLECTOR`,
which carries that Base's UUID and Mesh-Local EID), and the Base's registry
rejects a depth-1 report whose `parent_uuid` is not the Base itself
(`TOPOLOGY_REGISTRY_ERR_DEPTH1_BAD_PARENT`). The API never infers membership
from `depth` or from the module UUID.

`power_bases` lists every Base the Registry has heard from since the bridge
started, in first-heard order (order carries no meaning):

| field                 | notes |
|-----------------------|-------|
| `uuid`                | the Base's EUI-64, 16 lowercase hex characters |
| `online`              | a HEARTBEAT (every 10 s) or any accepted message from this Base within the Registry's stale timeout (120 s, the same bound as modules) |
| `module_count`        | modules in this snapshot's `modules` whose `power_base_uuid` is this Base |
| `online_module_count` | how many of those are `online` |

A Base appears as soon as its first HEARTBEAT arrives, before any module is
docked. Bases are not removed while the bridge runs (there is no Base
tombstone); after a bridge restart a Base reappears on its next heartbeat.
The Registry tracks at most 16 Bases; a new Base evicts the oldest offline
Base that has no modules left. If none can be evicted, the Base is not tracked,
but its modules are still applied, and the API derives its `power_bases` entry
from them. That entry is `online` exactly when one of its modules is online, since
an online module means the bridge accepted that Base's message within the
timeout.

**When a Base goes silent.** The Base, and each of its modules, turn
`online:false` when nothing has been heard for the stale timeout, both through
the same background sweep, so SSE sends one new snapshot. Nothing is removed
and module names and rooms are untouched. A module is offline *because of its
Base* when its own `online` is false and its Base's `online` is false. The
bridge cannot know more than that (modules report to their Base, not to
Linux). When the Base's link comes back, its unchanged periodic resync
(same session and sequence, `online:true`) now restores those modules to
online immediately. Before this revision they stayed offline until the module
sent a report with a new sequence.

**One module, one entry.** The Registry keys nodes by (Base, module). A module
physically moved from Base A to Base B is briefly listed by both, because A
reports it offline only after its own sweep or detach tombstone. The API lists each
`uuid` exactly once: the online entry wins over an offline one, and between
two online (or two offline) entries the most recent physical attach wins.
The Registry stamps an increasing `attach_order` whenever a report brings a node online.
A detach tombstone from Base A now removes only A's entry. Before this
revision it removed the module under every Base, so a late tombstone from the
old Base briefly deleted a module already docked on the new one. With a single
Base both rules behave exactly as before.

The Matter bridge is unchanged: endpoint IDs stay keyed by module UUID only
(`endpoint_map.bin`), so a module keeps its endpoint across Bases, and
`BridgeManager` still withholds updates while two Bases both report it online.

#### Reading fields

| field        | source                                   | notes |
|--------------|-------------------------------------------|-------|
| `kind`       | `sensor_channel_reading_t.type`            | one of the 10 fixed identifiers below |
| `raw_value`  | `sensor_channel_reading_t.value`           | the exact wire int32, unmodified |
| `scale`      | `sensor_channel_reading_t.scale`           | the raw `sensor_scale_t` integer (`0`=NONE, `1`=MILLI, `2`=MICRO, `3`=UNIT) |
| `value`      | derived from `raw_value`/`scale`           | see "Value conversion" below |
| `unit`       | derived from `kind`                        | `null` for `voc_raw`/`voc_index`/`nox_raw`/`nox_index` (no physical unit -- see below) |
| `status`     | `sensor_channel_reading_t.status`          | one of the 8 identifiers below, or `null` if somehow unrecognized |
| `sample_seq` | `NodeState.last_sample_seq`                | shared by every reading in the same module (one wire sample carries all of that module's channels together) |

#### Sensor `kind` identifiers (fixed, stable API names)

| `kind`        | firmware `sensor_channel_type_t` | wire scale       | `unit`   |
|---------------|-----------------------------------|-------------------|----------|
| `illuminance` | `SENSOR_CHANNEL_ILLUMINANCE`      | MILLI (milli-lux)  | `"lux"` |
| `temperature` | `SENSOR_CHANNEL_TEMPERATURE`      | MILLI (millidegC)  | `"celsius"` |
| `humidity`    | `SENSOR_CHANNEL_HUMIDITY`         | MILLI (milli-%RH)  | `"percent"` |
| `voc_raw`     | `SENSOR_CHANNEL_VOC_RAW`          | NONE (raw driver ticks) | `null` |
| `voc_index`   | `SENSOR_CHANNEL_VOC_INDEX`        | UNIT (Sensirion Gas Index, 0..500) | `null` |
| `nox_raw`     | `SENSOR_CHANNEL_NOX_RAW`          | NONE (raw driver ticks) | `null` |
| `nox_index`   | `SENSOR_CHANNEL_NOX_INDEX`        | UNIT (Sensirion Gas Index, 0..500) | `null` |
| `pm1`         | `SENSOR_CHANNEL_PM1_0`            | UNIT (already ug/m3) | `"ug/m3"` |
| `pm2_5`       | `SENSOR_CHANNEL_PM2_5`            | UNIT (already ug/m3) | `"ug/m3"` |
| `pm10`        | `SENSOR_CHANNEL_PM10`             | UNIT (already ug/m3) | `"ug/m3"` |

`voc_raw`/`nox_raw` are **never** converted to ppm/ppb, and `voc_index`/
`nox_index` are never turned into an AQI or a Good/Fair/Poor grade -- this
API only ever exposes what the firmware actually measured, at the same
fidelity the Matter bridge itself uses (see
[`value_mapping.hpp`](../linux/matter_bridge/app/core/include/matter_bridge/value_mapping.hpp)
for the equivalent Matter-side conversions, which apply the exact same wire
scales).

#### `status` identifiers

Directly, losslessly renamed (snake_case) from `sensor_channel_status_t`:
`ok`, `not_initialized`, `timeout`, `bus_error`, `driver_error`, `crc_error`,
`range_error`, `conditioning` (the last is SGP4x's own warm-up state: a value
is present on the wire but not yet trustworthy).

#### Value conversion

`value` is computed from `raw_value` and `scale` using the *exact* meaning
`sensor_channel.h` defines for `sensor_scale_t`, in fixed-point integer
arithmetic only (never through a `double`), so the JSON text always
reconstructs the original integer exactly:

- `scale == 0` (NONE) or `scale == 3` (UNIT): `value == raw_value`, printed
  as a plain integer (these are already whole units on the wire).
- `scale == 1` (MILLI): `value = raw_value / 1000`, printed with exactly 3
  decimal digits (e.g. `23400` -> `23.400`).
- `scale == 2` (MICRO): `value = raw_value / 1000000`, printed with exactly
  6 decimal digits.

### Compatibility and versioning policy

- `schema_version` changes only for a **breaking** change (a field removed,
  renamed, or its meaning/type changed). Clients must reject a
  `schema_version` they don't know.
- Adding fields is **not** breaking and keeps `schema_version`. Clients must
  ignore unknown fields. The Bonjour revision's health `hub_id` is such an
  additive change; without a hub_id the served health JSON is byte-for-byte
  the previous shape (host test `test_hub_identity.cpp`). The multi-Power-Base revision is such an additive
  change: `power_bases`, module `power_base_uuid`/`parent_uuid`, and health
  `power_base_count`. Every pre-existing field keeps its name, type, value, and
  position. A host test (`app/api/tests/test_multi_power_base.cpp`) removes
  only the added fields from a single-Base snapshot and compares the rest
  byte for byte with the pre-revision output.
- The Power Base management revision (2026-10-05) is additive too: health
  `power_base_management`/`power_base_removal`, snapshot
  `power_base_removals`, a `management` word per `power_bases` entry, a
  `management` object in enrollment sessions that asked for one, and silent
  Power Bases the hub holds a management credential for listed as
  `"online": false` entries. None of them is served unless management was
  requested at startup (host test `test_removal_api.cpp` compares a snapshot
  without management byte for byte). See "Power Base management and removal".
- The version revision (2026-10-06) is additive as well: health gains the
  optional `build` object as its last field, and only in the served bridge
  (`ServerConfig::report_build`, set by `chip/main.cpp`). Without it the
  health body is byte for byte the previous one, and with it the previous
  body with only `,"build":{…}` appended (host test `test_build_info.cpp`).
  `schema_version` stays `1`.
- The removal request revision (2026-10-11, Linux bridge 0.5.0) is **not**
  only additive, and it is the one change of this kind since the routes
  exist: `POST /api/v1/power-base-removals` now takes four members instead
  of one, and the request of an earlier client (`power_base_uuid` alone) is
  refused with `422 removal_registration_required`. `schema_version` stays
  `1` -- it describes the snapshot and health bodies, which only gained
  fields (`power_base_removal_request` in health; `removal_serial` per
  managed `power_bases` entry, `request_id` per job, `snapshot_stream` and
  `snapshot_sequence` at the end of a snapshot). A client reads
  `power_base_removal_request` (or the presence of `removal_serial`) to know
  which request to send; see "Removal routes".
- A client detects the revision by the presence of `power_bases`. Absent means
  "this bridge does not report Power Bases", not "no Power Bases". PogoHome
  iOS decodes all three fields as optional and keeps working against an older
  bridge.
- Old clients keep working against a new bridge. The one observable
  difference is that a module moved between Bases is now always listed once. An older
  bridge could list the same `uuid` twice in that window, and
  PogoHome iOS rejects a snapshot with duplicate module UUIDs.

### Example: two Power Bases

Real output of this server (`BuildSnapshotJson` via `pogohome_api::Server`)
fed by two simulated Bases over the real UDP/CoAP ingest path on loopback,
only the first reading of each module kept (`...`). Base `..0a00` has an SHT45 module with a PMS5003 behind it.
Base `..0b00` has the same two sensor kinds:

```json
{
  "schema_version": 1,
  "generated_at_unix_ms": 1790154064727,
  "registry_generation": 72,
  "power_bases": [
    {"uuid": "020000fffe650a00", "online": true, "module_count": 2, "online_module_count": 2},
    {"uuid": "020000fffe650b00", "online": true, "module_count": 2, "online_module_count": 2}
  ],
  "modules": [
    {"uuid": "020000fffe650a01", "power_base_uuid": "020000fffe650a00", "parent_uuid": "020000fffe650a00", "online": true, "depth": 1, "role": "intermediate", "sensor_bitmap": 6, "generation": 1, "last_seq": 1,
     "readings": [{"kind": "temperature", "raw_value": 22270, "scale": 1, "value": 22.270, "unit": "celsius", "status": "ok", "sample_seq": 27}, ...]},
    {"uuid": "020000fffe650a02", "power_base_uuid": "020000fffe650a00", "parent_uuid": "020000fffe650a01", "online": true, "depth": 2, "role": "leaf", "sensor_bitmap": 896, "generation": 1, "last_seq": 1,
     "readings": [{"kind": "pm1", "raw_value": 4, "scale": 3, "value": 4, "unit": "ug/m3", "status": "ok", "sample_seq": 27}, ...]},
    {"uuid": "020000fffe650b01", "power_base_uuid": "020000fffe650b00", "parent_uuid": "020000fffe650b00", "online": true, "depth": 1, "role": "intermediate", "sensor_bitmap": 6, "generation": 1, "last_seq": 1,
     "readings": [{"kind": "temperature", "raw_value": 22060, "scale": 1, "value": 22.060, "unit": "celsius", "status": "ok", "sample_seq": 6}, ...]},
    {"uuid": "020000fffe650b02", "power_base_uuid": "020000fffe650b00", "parent_uuid": "020000fffe650b01", "online": true, "depth": 2, "role": "leaf", "sensor_bitmap": 896, "generation": 1, "last_seq": 1,
     "readings": [{"kind": "pm1", "raw_value": 4, "scale": 3, "value": 4, "unit": "ug/m3", "status": "ok", "sample_seq": 6}, ...]}
  ]
}
```

Once Base `..0a00` has been silent for the stale timeout (120 s in
production), the next SSE snapshot carries the following. This is the shape asserted by the
host test `test_multi_power_base.cpp`:

```json
"power_bases": [
  {"uuid": "020000fffe650a00", "online": false, "module_count": 2, "online_module_count": 0},
  {"uuid": "020000fffe650b00", "online": true, "module_count": 2, "online_module_count": 2}
]
```

and both `..0a0x` modules have `"online": false`, with their readings kept as
the last values received.

### `GET /api/v1/events` (Server-Sent Events)

`Content-Type: text/event-stream`, `Cache-Control: no-store, no-cache`.

1. Immediately on connect: one `event: snapshot` carrying the *current* full
   `/api/v1/snapshot`-shaped payload as its `data:` line (always one JSON
   object per line -- this API's JSON is always emitted compact/single-line).
2. Whenever `ingest::Registry::generation()` changes, the server re-reads a
   fresh full snapshot and sends another `event: snapshot`. There is no delta
   event in this v1 -- always the whole snapshot, which keeps client-side
   state convergence trivial given how small this Registry actually is
   (bounded to 64 nodes).
3. If nothing changes for 15 seconds, an SSE comment line (`: heartbeat`) is
   sent instead, so a client/proxy can tell the connection is still alive.
4. A reconnecting client always gets the current full snapshot first (same
   as point 1) -- there is no `Last-Event-ID`/resume-from-offset support in
   v1.
5. The *same* generation is never re-sent -- a snapshot event only ever
   follows a real `generation()` change.
6. A small, explicit cap limits concurrent SSE clients (production default:
   8); once at the cap, a new `/api/v1/events` connection gets `503` and is
   closed immediately. A slow or vanished client is detected on the next
   send attempt to it (the next change or the next heartbeat) and its slot
   is freed then -- there is no separate liveness ping between those.
7. Closing the TCP connection (client-side) is all that's needed to
   disconnect -- no explicit unsubscribe request.

Every event carries `snapshot_stream` and `snapshot_sequence` (see
`GET /api/v1/snapshot`): events and `GET` answers are numbered in one line,
in the order the bridge made them. The bridge makes one snapshot at a time
(a section of its own, which the ingest path never enters) and sends it
after leaving that section.

A slow/blocked SSE client can never stall ingest, Matter attribute pushes, or
any other client: this server only ever holds `ingest::Registry`'s internal
mutex long enough to copy a snapshot out (`Registry::snapshot()`), and only
ever serializes/sends *after* releasing it (see `http_server.cpp`'s own top
comment). Every client socket has a bounded send timeout, so a stalled
write eventually fails and that connection's own worker thread exits -- it
never blocks any other client or the registry watcher thread.

## Concurrency, resource bounds, and shutdown

- One acceptor thread; each connection gets its own worker thread, tracked
  so `Stop()` can join every one of them within a bounded time (no detached,
  unjoinable threads).
- `max_connections` (default 32) bounds total concurrent connections;
  `max_sse_clients` (default 8) is a smaller sub-limit specifically for
  `/api/v1/events`.
- Every client socket has bounded receive/send timeouts (a few seconds) --
  no read or write can block a worker thread forever.
- `pogo_matter_bridge`'s existing shutdown path (SIGINT/SIGTERM, handled by
  connectedhomeip's own `AppMain.cpp`) also stops this API server: its
  acceptor and every worker thread are signaled and joined, bounded in time,
  before the process exits.

## HTTP behavior summary

| condition                                   | response |
|----------------------------------------------|----------|
| known path, `GET`, success                    | `200`, JSON or `text/event-stream` |
| known path, method other than `GET`            | `405` |
| unknown path                                  | `404` |
| oversized request header block (> 8 KiB) or malformed request line | `400`, or the connection is simply closed |
| settings route over plain HTTP | `403 tls_required` |
| enrollment route, enrollment not configured | `404 enrollment_disabled` |
| enrollment route over plain HTTP (configured) | `403 tls_required` |

Only `PUT /api/v1/settings`, `POST /api/v1/pairing/claim` and
`POST /api/v1/power-base-enrollments` read a body (`Content-Length` only, at
most 256 KiB; 256 bytes for the enrollment body). The API never adds CORS headers (a
native iOS app making a direct request needs none).

## `curl` verification

```sh
curl -s http://<LAN-address>:8787/api/v1/health
curl -s http://<LAN-address>:8787/api/v1/snapshot | jq .
curl -N http://<LAN-address>:8787/api/v1/events   # streams; Ctrl-C to stop
curl -s -o /dev/null -w '%{http_code}\n' http://<LAN-address>:8787/no-such-path   # 404
curl -s -o /dev/null -w '%{http_code}\n' -X POST http://<LAN-address>:8787/api/v1/health  # 405
```

## Bonjour

While the API is enabled, `pogo_matter_bridge` announces it over DNS-SD/mDNS
so a client on the same LAN can find the hub without its address
(implementation: `app/api/include/pogohome_api/mdns_advertiser.hpp`).

| | |
|---|---|
| service type | `_jogakbo._tcp` (project-specific; this is the Linux sensor hub's HTTP API only -- never a Power Base or a sensor module) |
| instance name | `Jogakbo Hub (<short host name>)`; avahi's alternative name (`... #2`) after a name conflict. Display only. |
| port | the API's real bound port (`--api-port`) |
| interface | `--api-interface` only (its interface index), IPv4 or IPv6 to match the API's bound address |
| TXT | `txtvers=1`, `api=1` (the API major version = `schema_version`), `path=/api/v1`, `hub_id=<hub_id>` |

How it is published: through the host's existing **avahi-daemon**, the same
daemon connectedhomeip's Linux platform mDNS uses, as one entry group on a
separate avahi client connection. That group only holds this one service: the
bridge does not register a host name and never touches the Matter
(`_matter._tcp`, `_matterc._udp`) or OTBR (`_meshcop._udp`, SRP) records.
`libavahi-client.so.3` is loaded with `dlopen()`, so there is no new build
dependency (no Avahi headers), and a host without Avahi still runs the bridge
and its API unadvertised (a `Bonjour: not advertising (...)` line on stderr).

When it is announced:

- only after the API server is listening, and only while it can take
  requests: the acceptor is running and the bound address is still on
  `--api-interface` (re-checked every 5 s). Otherwise the record is withdrawn
  and it comes back when both hold again;
- on shutdown (SIGINT/SIGTERM) the record is withdrawn before the API server
  stops. If the process dies without that, avahi-daemon drops the record when
  its D-Bus connection closes, so a dead bridge never stays announced;
- avahi-daemon restarting (or not running yet) is survived: the record is
  registered again when the daemon is back, without restarting the bridge;
- a name conflict renames the instance; clients identify the hub by `hub_id`,
  never by the name.

Announcing does not change the trust boundary above: the API was already
reachable by anyone on the LAN; Bonjour only tells them where it is. It is
still never announced beyond the one `--api-interface`.

There is no child process (`avahi-publish`), no systemd `.service` file under
`/etc/avahi/services` and no configuration change. `--api-no-advertise` turns
the announcement off.

Checking it on the Linux host:

```sh
avahi-browse -rt _jogakbo._tcp          # needs avahi-utils
journalctl -u pogo-matter-bridge -o cat | grep -E 'hub_id|Bonjour'
curl -s http://<LAN-address>:8787/api/v1/health   # hub_id equals the TXT hub_id
```

From a Mac on the same network: `dns-sd -B _jogakbo._tcp` and
`dns-sd -L "<instance name>" _jogakbo._tcp`.

## Home settings (settings-sharing revision)

PogoHome devices (iPhone, iPad, Mac) that follow the same hub can share one
copy of the **home name, the rooms (id, name, icon, order) and each sensor
module's custom name and room**. The hub stores it and decides every change;
there is no CloudKit or Apple Developer Program dependency.

Not shared: sensor readings and online state (the Registry's), simulated
(mock) data, Apple Home's homes/rooms/accessories, and anything per device
(language, appearance, selected tab, the Watch's sensor choice, permissions).

Implementation: `app/api/include/pogohome_api/{home_settings,settings_store,
device_auth,tls_identity}.hpp`; operator tool `app/api/tools/pogohome_hubctl.cpp`.

### Enabling and files

On by default whenever `--api-interface` is given; `--api-no-settings` turns
it off (the API is then exactly the read-only one, byte for byte). Its state
lives in `<--state-dir>/pogohome/` (mode 0700, files 0600):

| file | contents |
|---|---|
| `tls_key.pem`, `tls_cert.pem` | ECDSA P-256 key and self-signed certificate, created once. Devices pin the certificate's SHA-256 |
| `settings.json` | the shared settings, revision and a random `settings_id` |
| `devices.json` | paired devices: id, name, SHA-256 of the token (never the token), paired/last-used time |
| `pairing.json` | the one waiting pairing code, its expiry and wrong attempts (exists only while a code waits) |
| `auth.lock` | `flock` shared with `pogohome-hubctl` |

A damaged key/certificate, settings or device file is never replaced or reset:
the settings routes answer `503` (health says `settings_state: "unavailable"`),
the reason is logged, and sensors, Bonjour and Matter run as always. Move the
damaged file aside on purpose to start over (a new TLS identity means every
device pairs again).

### TLS on the API port

The same port serves plain HTTP and TLS; the server looks at each
connection's first byte (`0x16` = TLS handshake). TLS 1.2+ with ECDHE-ECDSA
AES-GCM/ChaCha20 only. There is no CA on a home LAN: a device checks the
certificate by pairing (below) and then pins its SHA-256 for every request.
The app never disables certificate checks globally -- only its settings
requests use the pin, per request.

Over plain HTTP `/api/v1/settings`, `/api/v1/pairing/claim` and
`/api/v1/devices/self` answer `403 {"error":"tls_required"}` before any
header value or body is used.

### Pairing

1. The operator runs `pogohome-hubctl --state-dir <dir> pair` on the hub,
   as the account the bridge runs as (see "Operating" below). It prints a code `XXXX-XXXX-XXXX` (12 Crockford base32 characters,
   60 bits), valid for 10 minutes by default (`--minutes 1..60`), for one
   device, destroyed after 5 wrong attempts. A new code replaces a waiting one.
2. The app connects with TLS, hashes the certificate it received (`C`), and
   sends `POST /api/v1/pairing/claim`
   `{"device_name", "client_nonce" (32 hex), "proof"}` where
   `proof = HMAC-SHA256(code, "pogohome-pair-v1\nclient\n" + hub_id + "\n" + C + "\n" + nonce + "\n" + SHA256hex(device_name))`.
   **The code itself is never sent.** The hub recomputes the proof with its
   own certificate hash, so a device talking to an impostor (another
   certificate) is refused and the impostor learns nothing it can use within
   the code's lifetime.
3. The hub answers `{"hub_id", "device_id", "token", "server_proof"}` with
   `server_proof = HMAC-SHA256(code, "pogohome-pair-v1\nserver\n" + hub_id + "\n" + C + "\n" + nonce + "\n" + device_id + "\n" + SHA256hex(token))`.
   The app keeps the token (Keychain, this device only) and pins `C` only if
   that proof checks out.

Errors: `403 {"error":"pairing_rejected","reason":"no_active_code"|"expired"|"bad_proof"}`,
`409 too_many_devices` (32), `400 invalid_request`, `503 auth_unavailable`.

Test vectors shared by `tests/test_device_auth.cpp` and the app's
`HubPairingProofTests`: code `7KQM3XWDPA9T`, hub_id
`3f0c9a4e8b1d4c2a9e7f6b5a4d3c2b1a`, C = SHA-256("test-cert"), nonce
`000102…0f`, name `Jun의 iPhone` → client proof `096c8f9c…3c99`; device id
`0123456789abcdef`, token `A`×43 → server proof `4d9e5bc4…94a0`.

### Routes (TLS, `Authorization: Bearer <token>`)

| route | result |
|---|---|
| `GET /api/v1/settings` | `{"schema_version":1,"hub_id","initialized","revision","updated_at_unix_ms","settings"}`; before the first upload `initialized:false`, `revision:null`, `settings:null` |
| `PUT /api/v1/settings` | body `{"base_revision": <revision> or null, "settings": {...}}` |
| `DELETE /api/v1/devices/self` | removes the calling device |

`settings`:

```json
{"home_name": "우리 집" | null,
 "rooms": [{"id": "8A3B1C10-…", "name_kind": "custom", "name": "거실", "symbol": "sofa.fill"},
           {"id": "…", "name_kind": "suggestion", "suggestion_symbol": "bed.double.fill", "suggestion_ordinal": 1, "symbol": "bed.double.fill"}],
 "modules": [{"uuid": "020000fffe65d357", "name": "공기" | null, "room_id": "…" | null}]}
```

- Rooms are in display order and keyed by UUID, never by name. A
  `suggestion` name is the app's own name for an icon, shown in each device's
  language; a `custom` name is the same everywhere. The app's fixed
  "unspecified" room (`9C6B8B2E-0000-4000-8000-000000000000`) is never listed;
  a module in it has `room_id: null`.
- Modules are keyed by the real module UUID the Registry reports, so a name
  and room survive detaching, re-docking and moving to another Power Base.
  Entries stay when a module is absent.
- Validation (`422 {"error":"invalid_settings","detail": <code>}`): at most
  64 rooms and 256 modules; names non-empty, no surrounding whitespace or
  control characters, at most 120 code points; unique room ids; no two custom
  names equal ignoring ASCII case, no two suggestions with the same icon and
  number; symbols `[a-z0-9.]{1,64}`; module uuids 16 lowercase hex, unique;
  `room_id` must name a listed room; unknown fields rejected. Bodies over
  256 KiB are `413`; chunked bodies `411`; malformed JSON `400`.

Concurrency: the hub owns the revision (`<settings_id>-<n>`, opaque to
clients). `base_revision: null` is only accepted while there are no settings
(the first upload); otherwise `base_revision` must equal the current revision.
Anything else is `409 {"error":"revision_conflict","current": <GET body>}` and
nothing is written, so an edit based on an older copy never overwrites or
undoes a newer change or deletion. Device clocks decide nothing. A deleted
room's modules move to `room_id: null` in the same document, so a deletion,
a reorder or several placements are always one atomic change.

Saving: temporary file, `fsync`, `rename`, directory `fsync`. A failed save is
`500 {"error":"settings_save_failed"}` and the previous settings stay in
memory and on disk -- never reported as saved.

### Health, snapshot and SSE

With settings sharing requested, `/api/v1/health` and every snapshot (GET and
SSE) end with `"settings_api":1,"settings_state":"ready"|"uninitialized"|"unavailable","settings_revision":<revision or null>`
(additive, after every earlier field). A saved change makes open SSE streams
send a new snapshot within one poll interval (1 s) even if the Registry did not
change, so apps notice another device's change without polling the settings
route. Without settings sharing none of this appears.

### Operating

Run `pogohome-hubctl` as the account `pogo_matter_bridge` runs as -- not
blindly with `sudo`. The files it writes under `<--state-dir>/pogohome/`
(`pairing.json`, `devices.json`, `auth.lock`) are created `0600` by whoever runs
it, so a copy written by root locks a bridge that runs as a normal user out of
pairing and settings. That account owns `<--state-dir>/pogohome/` (the bridge
creates it); before the bridge has ever created it, read the unit's `User=`
(`systemctl show -p User --value pogo-matter-bridge.service`, empty = root).

```sh
BRIDGE_USER="$(sudo stat -c %U /var/lib/pogo-matter-bridge/pogohome)"
hubctl() { sudo -u "$BRIDGE_USER" pogohome-hubctl --state-dir /var/lib/pogo-matter-bridge "$@"; }

hubctl pair                    # new code, 10 min
hubctl pairing-status
hubctl cancel-pairing
hubctl devices                 # id, paired, last used, name
hubctl revoke <device-id>
hubctl revoke-all
hubctl fingerprint             # what devices pin
hubctl settings                # check + summary
```

When the bridge runs as your own login account (as in
[`matter-bridge.md`](matter-bridge.md) "실행"), run `pogohome-hubctl` directly
without `sudo`.

Revocation applies to the device's next request; no restart. The app's
Settings shows each device's id ("이 기기 ID") to match `devices`.

## Power Base enrollment (enrollment revision)

A paired device can ask the hub to let **one** new Power Base, named by its
exact EUI-64, join the Thread network with a PSKd (Joiner passphrase) the
device chose for that board, for a short time. The hub reports success only
once **this Linux bridge has received a heartbeat from that Power Base**.

What exists: the hub side -- the routes below, the session logic
(`app/api/include/pogohome_api/enrollment.hpp`) and a small root-run helper
that is the bridge's only way to the OTBR Commissioner
(`app/api/tools/pogohome_otbr_helper.cpp`); the PogoHome app's DEBUG-only
"Power Base 등록 시험"; and, for a development/bench trial only, Power Base
firmware in `pb_thread:mode=2` that takes the PSKd on the spare board's
USB-Serial/JTAG console (`pb_join start`, bench build with
`power_base/sdkconfig.bench_joiner`), runs the Joiner, attaches, records its
enrollment and then starts the normal exporter, whose heartbeat is what ends
a session as `heartbeat_received` (see [`power-base.md`](power-base.md) "앱
등록 대기 상태와 Joiner 가입"). On that path the Linux bridge address comes
from the board's prepared `pb_bridge` image, not from this API. A session may
instead ask the hub for its **bridge endpoint** (see "Bridge endpoint
delivery" below, added 2026-10-03): the hub then reports its own OTBR
Mesh-Local EID and ingest port with the session, and the app's DEBUG BLE test
hands them to the board with the PSKd. When it was added, that path had been
run in host tests and builds only -- not on a board, over BLE, or against the
operating hub. Since then (2026-10-05) the hub side was built, installed and
checked on the operating host, and the path was run on one spare board; what
that confirmed and what it did not is listed under "Still to verify on the
operating host and a spare board" below.
What does not exist: a QR flow and a release app UI. Host tests use a fake
Commissioner and a fake `ot-ctl`.

### Enabling

`--api-enroll-helper <absolute path>` turns it on; it needs `--api-interface`
and settings sharing (no `--api-no-settings`), otherwise the bridge refuses
to start. `--api-enroll-window <seconds>` (30..300, default 120) sets the
Joiner window. The helper is not run at startup; nothing runs as root and
nothing touches the OTBR until a paired device starts a session. The bridge
runs the helper as `/usr/bin/sudo -n -- <path>`; the operator allows that one
command (see "Installing the OTBR helper" below).

Without `--api-enroll-helper` the routes answer `404 enrollment_disabled` and
nothing else changes: health, snapshot, SSE, settings, Bonjour, Matter and
sensor ingest behave exactly as before. With it, `GET /api/v1/health` adds
`"power_base_enrollment":"available"` (or `"unavailable"` when settings
sharing could not start) after every earlier field; snapshots and SSE are
unchanged. The Registry now numbers each Power Base heartbeat (an internal
counter; no wire, JSON or `generation()` change).

### Routes (TLS, `Authorization: Bearer <token>`)

Checked in this order, like the settings routes: not configured → `404
enrollment_disabled`; plain HTTP → `403 tls_required` (no header or body is
used); settings sharing unavailable → `503 enrollment_unavailable`; wrong
method → `405`; no/unknown token → `401` (`WWW-Authenticate: Bearer`);
credentials unreadable → `503 auth_unavailable`. Every answer has
`Cache-Control: no-store`.

| route | result |
|---|---|
| `POST /api/v1/power-base-enrollments` | starts a session: `202` + session, `Location: /api/v1/power-base-enrollments/<session_id>` |
| `GET /api/v1/power-base-enrollments/<session_id>` | `200` + session |
| `DELETE /api/v1/power-base-enrollments/<session_id>` | cancels: `200` + session (`state: "cancelled"`) |

Request (`Content-Length` only, at most 256 bytes, exactly these two fields,
or these two and the option described under "Bridge endpoint delivery"):

```json
{"eui64": "020000fffe65d357", "pskd": "J01NME7K2"}
```

- `eui64`: exactly 16 hex digits (either case; answered lowercase), not all
  zero. No `*` (wildcard Joiner), no separators, no discerner. It must be the
  EUI-64 the board's Joiner uses (`pb_join status` on the mode-2 bench
  firmware, `ot eui64` on an OpenThread CLI image), which is also the Power
  Base's `uuid` in snapshots and heartbeats.
- `pskd`: 6..32 characters of `0-9 A-Z` without `I O Q Z` (the Thread PSKd
  alphabet, uppercase only). Use a new random value per board and session.

Session:

```json
{"session_id": "5f0c…(32 hex)", "eui64": "020000fffe65d357",
 "state": "waiting_for_heartbeat", "finished": false, "joiner_entry": "allowed",
 "error": null, "window_seconds": 120,
 "created_at_unix_ms": 1790000000000, "expires_at_unix_ms": 1790000120000,
 "expires_in_ms": 118250, "heartbeat_received_at_unix_ms": null,
 "started_by_device_id": "0123456789abcdef"}
```

A session started without the bridge endpoint option has exactly these fields
-- no `bridge_endpoint` key -- so an existing client sees what it always saw.

`state`:

| state | meaning |
|---|---|
| `preparing` | (with the bridge endpoint option: reading the hub's bridge endpoint, then) making sure the Commissioner is active, then adding the Joiner entry |
| `waiting_for_heartbeat` | the OTBR allows this EUI-64 as a Joiner; the bridge has not yet received a new heartbeat from it |
| `heartbeat_received` | final. The bridge's Registry received a heartbeat from a Power Base with this EUI-64 **after** the session started. It means this Linux bridge heard the Base over Thread; it does **not** mean its modules or sensors work -- watch the snapshot for that |
| `cancelled` | final. The starting device cancelled it |
| `expired` | final. The window ended without a new heartbeat. The board may still have joined Thread (e.g. firmware that joins but does not report); the hub cannot tell |
| `failed` | final. `error` says why: `otbr_unavailable` (helper, sudo or OTBR not reachable), `otbr_timeout`, `commissioner_rejected` (e.g. another Commissioner holds the role), `joiner_rejected`, `helper_failed`, `window_too_short`, `bridge_stopping`; with the bridge endpoint option also `bridge_endpoint_unavailable`, `bridge_endpoint_timeout`, `bridge_endpoint_invalid`, `bridge_endpoint_unsupported` (see below) |

Joiner success, a Commissioner command succeeding or the board reaching
Thread `child`/`router` are never reported as success by themselves.
A Base that was already known before the start does not count either: a
cached online state or an old heartbeat -- even one with the same timestamp --
never ends a session; only a heartbeat applied after the start does (see
`Registry::heartbeat_serial()`).

`joiner_entry`: `none` (never added), `adding`, `allowed`, `removing`,
`removed` (removed, or already gone -- OpenThread removes an entry itself
after a successful join or at its timeout, and that `NotFound` counts as
removed), `remove_failed` (the OTBR still closes it at its own timeout, at
the latest `expires_at_unix_ms`). `finished` is `true` once the state is
final and the cleanup is done.

Errors (never echoing the EUI-64 or PSKd sent):

| status | `error` | when |
|---|---|---|
| `400` | `invalid_request` | not JSON, missing/extra/non-string fields |
| `400` | `malformed_headers`, `invalid_content_length` | |
| `411` | `length_required` | chunked or no `Content-Length` |
| `413` | `request_too_large` | body over 256 bytes |
| `422` | `invalid_eui64` | including `*` |
| `422` | `invalid_pskd` | |
| `422` | `unsupported_bridge_endpoint_version` | `bridge_endpoint_version` is an integer other than `1` |
| `503` | `enrollment_unavailable` (`reason`: `bridge_endpoint`) | the option was given but this bridge does not know its ingest port. Nothing started |
| `409` | `enrollment_in_progress` | a session is running or still cleaning up. To the device that started it, the answer also has `"session": {…}` (so a repeated POST finds its session); others get only the error |
| `409` | `power_base_already_connected` | a Base with this EUI-64 is online in the Registry right now |
| `409` | `session_already_final` + `"session"` | DELETE after the session ended |
| `404` | `session_not_found` | unknown or malformed id, forgotten, or **another device's** session (indistinguishable on purpose) |
| `503` | `enrollment_unavailable` (`reason`: `startup`/`stopping`) | |
| `500` | `internal_error` | no randomness for a session id |

### Bridge endpoint delivery (2026-10-03)

A Power Base sends its data to the Linux bridge's CoAP ingest listener over
Thread. Until now that destination was written into the board's prepared
image. A session can instead carry it, so that the app hands it to the board
during BLE enrollment:

```json
{"eui64": "020000fffe65d357", "pskd": "J01NME7K2", "bridge_endpoint_version": 1}
```

`bridge_endpoint_version` is the explicit request. It must be the integer `1`
(any other integer: `422 unsupported_bridge_endpoint_version`; any other
type: `400 invalid_request`). The client sends **no** address and no port --
a `bridge_endpoint` object, an address or a port in the request is `400
invalid_request`. The hub makes the endpoint:

- **address**: this hub's OTBR Mesh-Local EID, read at session time through
  the root helper's `mesh-local-eid` operation, i.e. exactly what `ot-ctl
  ipaddr mleid` prints. It is never the first of an address list, never an
  RLOC or ALOC, and never the API's own LAN address, URL or Bonjour name.
- **port**: the UDP port the ingest listener is bound to (the program's
  `--port`), not a constant in the API code.

Such a session's JSON has one more key, after `started_by_device_id`:

```json
"bridge_endpoint": {"version": 1, "address": "fd11:2233:4455:0:8c1f:2e3d:4c5b:6a79", "port": 5684}
```

- `version`: `1`. A client must refuse any other value.
- `address`: one IPv6 literal in canonical lower-case text. Always inside
  fd00::/8 and never with the interface id `0000:00ff:fe00:xxxx` (the hub
  refuses anything else from its own OTBR and fails the session instead).
- `port`: 1..65535.

Rules:

- **`preparing` may have `"bridge_endpoint": null`.** The endpoint is read on
  the session's own thread after the `202`; until then, and for a session
  that ended before it was read, the value is `null`.
- **`waiting_for_heartbeat` always has it.** The Joiner entry is added only
  after the endpoint is fixed. The order is: read the endpoint → make sure
  the Commissioner is active → add the Joiner. Nothing is added to the OTBR
  first and taken back.
- **Fixed once per session.** Every later `GET`, and the `409
  enrollment_in_progress` answer to a repeated `POST` by the same device,
  reports the same value, even if the OTBR's address changed meanwhile. A new
  session reads it again.
- **If it cannot be established the session fails and no Joiner is added**
  (`joiner_entry: "none"`, the Commissioner is not started either):

  | `error` | when |
  |---|---|
  | `bridge_endpoint_unavailable` | helper, sudo or OTBR not reachable, or the OTBR refused the command |
  | `bridge_endpoint_timeout` | the helper (or `ot-ctl`) did not answer in time (8 s) |
  | `bridge_endpoint_invalid` | the helper answered, but not with exactly one usable Mesh-Local EID (Thread not running, a list, an RLOC, a link-local address, …) |
  | `bridge_endpoint_unsupported` | the installed root helper predates the `mesh-local-eid` operation (it answers `err invalid_request`). Install the current helper |

- **A cancel, the deadline or a shutdown during the read wins.** The session
  stays `cancelled` / `expired` / `failed bridge_stopping`; the address that
  arrives afterwards is not attached and nothing further is done.
- **Nobody else sees it.** The endpoint is only in the session JSON, which
  only the paired device that started the session can read over TLS; another
  device gets `404 session_not_found` as before. `GET /api/v1/health`,
  snapshots, SSE and every unauthenticated or error answer contain neither
  the address nor the field.
- No lock of the API server, the Registry or the session is held while the
  helper reads the address.

A bridge older than this option accepts exactly two fields and answers `400
invalid_request` to a request that has the option; nothing is started. A
client that needs the endpoint must stop there -- not retry without the
option, and not substitute an address of its own.

What this does not do: tell a board that is already enrolled about a later
change of the hub's address or port.

### Session lifetime

1. `POST` checks the request, refuses if a session is running or the Base is
   online, records the Registry's heartbeat counter, and answers `202`
   (`preparing`) at once. Everything slow happens on the session's own thread.
   (With the bridge endpoint option, that thread first reads the endpoint;
   if that fails or the session ended meanwhile, steps 2, 3 and 5 do not
   happen.)
2. The helper makes sure the Commissioner is active. If it already was (for
   anything else), it is left as it is; the bridge **never stops** the
   Commissioner, also not one it started (stopping is not among the
   helper's operations; an operator can run `sudo ot-ctl commissioner stop`).
3. The helper runs `commissioner joiner add <eui64> <pskd> <timeout>` with
   `timeout` = the rest of the window, so the OTBR closes the entry by itself
   at `expires_at_unix_ms` even if the bridge crashes or is killed.
4. The session thread checks the Registry every 250 ms for a new heartbeat
   from that EUI-64, and ends at `heartbeat_received`, a `DELETE`, the
   deadline (monotonic clock; `expires_at_unix_ms` is informational), or the
   bridge stopping.
5. For every ending, if an add was attempted (even one that failed or timed
   out -- the entry may exist), the helper runs `commissioner joiner remove
   <eui64>` for that EUI-64 only.

Races: the first final state wins and is never changed. A heartbeat that has
reached the Registry before a `DELETE` or the deadline is evaluated wins
(`DELETE` then answers `409 session_already_final` with `heartbeat_received`);
a heartbeat after the session is final changes nothing. A `DELETE` while the
add is still running is accepted at once; the entry is removed right after
the add returns. On shutdown (`SIGTERM`) a running session becomes `failed`
/ `bridge_stopping` and its entry is removed before the process exits
(bounded by the helper timeouts below).

One session at a time, bridge-wide. A finished session stays readable by its
device for 10 minutes or until the next `POST` replaces it. Sessions are in
memory only; after a restart they are gone (and the OTBR entry has its own
timeout).

No lock of the API server or the Registry is held while the helper runs:
health, snapshot, SSE, settings and ingest keep working during an enrollment.
Helper time limits: 20 s for the Commissioner start (it waits up to 10 s for
the Leader to accept the petition), 8 s for each other call; inside the
helper, 5 s per `ot-ctl` command and a 30 s hard limit for the whole run.

### How the PSKd is handled

It travels app → hub only inside TLS, is checked and kept in memory for the
one Joiner add, and goes bridge → helper → `ot-ctl` only through **stdin**
pipes: never in a command line (`ps`, `/proc/<pid>/cmdline`), an environment
variable, a log line, a response, an error message or a file. The helper
runs `ot-ctl` with no command arguments and closes its stdin only after
`ot-ctl` printed `Done`/`Error`; it never prints `ot-ctl`'s output (which may
echo the command). Memory copies are overwritten after use as far as C++
strings allow (best effort). The OTBR itself keeps the PSKd only in its
Joiner table until the entry is removed or expires.

### Installing the OTBR helper (operator procedure -- review before applying)

The bridge runs as a normal user (the unit's `User=`; the examples below
call that account `$BRIDGE_USER` and step 4 shows how to read it), and the
OTBR's control socket `/run/openthread-wpan0.sock` is `root:root 0755`, so `ot-ctl` fails for
that user (`connect session failed: Permission denied`). Opening the socket
(chmod 777), running the bridge as root or allowing `sudo ot-ctl *` would each
give the bridge -- and anything that compromises it -- the whole OTBR CLI,
including the Network Key and Dataset. Instead the bridge may run exactly one
root-owned program with no arguments, which can only do five operations:
read the Commissioner state, start the Commissioner unless active, add one
exact Joiner, remove that one Joiner, and (added 2026-10-03) read the OTBR's
own Mesh-Local EID with the fixed command `ipaddr mleid`. It still takes no
free-form CLI command, reads no Dataset or Network Key, adds no wildcard
Joiner and never stops a Commissioner; the sudoers rule below ("that exact
path, no arguments") is unchanged. A helper installed before that date must
be rebuilt and reinstalled (steps 2 and 3) before a session can deliver a
bridge endpoint; until then such sessions fail with
`bridge_endpoint_unsupported` and the four earlier operations keep working. Nothing in this repository applies any of this; the running service is
not changed until an operator does so.

1. **Check the OTBR on the host** (as an admin):

   ```sh
   command -v ot-ctl; ls -l "$(command -v ot-ctl)"     # expect a root-owned file, e.g. /usr/sbin/ot-ctl
   sudo ot-ctl commissioner state                        # disabled | active ...  Done   (argument mode, for comparison)
   ```

   Then the helper's whole transport: the command on **stdin**, with stdin
   kept open until the answer has arrived. The helper does exactly that (it
   closes `ot-ctl`'s stdin only after `Done` or `Error ...`), so the test must
   do it too. It is bounded: stdin is held open for 3 s and the whole run is
   ended after 10 s at the latest.

   ```sh
   sudo -v                                   # authenticate first, so that no password prompt delays ot-ctl's start
   T="$(mktemp)"
   { printf 'commissioner state\n'; sleep 3; } | sudo timeout 10 ot-ctl > "$T" 2>&1; RC=$?
   ANSWER="$(tr -d '\r' < "$T" | sed 's/^\(> \)*//')"; rm -f "$T"
   printf '%s\n' "$ANSWER"
   if printf '%s\n' "$ANSWER" | grep -qxE 'disabled|petition|petitioning|active' &&
      printf '%s\n' "$ANSWER" | grep -qx 'Done'; then
     echo "ANSWERED on stdin: a state line and Done (exit status $RC)"
   elif printf '%s\n' "$ANSWER" | grep -q '^Error '; then
     echo "COMMAND REFUSED by the OTBR: stdin reached it, the command failed (exit status $RC)"
   else
     echo "NO ANSWER within the hold time (exit status $RC) -- not a verdict yet, see below"
   fi
   ```

   How to read it. The verdict is the output, never the exit status alone:

   - **A state line (`disabled`, `active`, ...) and `Done`**: stdin works.
     Exit status `0` then means `ot-ctl` ended by itself when stdin was
     closed; `124` means the answer came but `ot-ctl` was still running when
     `timeout` ended it. Both are "answered". The helper does not depend on
     `ot-ctl` exiting by itself: after the answer it closes stdin, waits a
     short grace time and ends the process.
   - **`Error <n>: ...`**: the command reached the OTBR and was refused
     there. That is a failed command, not a stdin problem. Read the error
     (and check that `commissioner` exists: `sudo ot-ctl help | grep commissioner`).
   - **Neither** (nothing, or only the echoed command): no answer arrived
     while stdin was open. Run it again with a longer hold
     (`sleep 8` and `timeout 15`) before concluding anything. An exit status
     of `0` here is not a success, and `connect session failed` in the output
     means the OTBR was not reachable at all.

   Do not use `printf 'commissioner state\n' | sudo ot-ctl` as the test (an
   earlier version of this procedure did). It closes stdin at once. On the
   operating host that form only echoed the command and printed no answer,
   while the same `ot-ctl` answered `active` and `Done` when stdin was kept
   open for a moment (operator's test). So a failure of the close-at-once
   form does not show that an `ot-ctl` build cannot take commands on stdin.

   Stop only if a run with stdin held open, also with the longer hold, still
   gives no answer, or if this `ot-ctl` works with a command-line argument
   only. The helper must not be used with such a build, and passing the PSKd
   as an argument instead is never an option (it would be visible in the
   process list).

   Status of this example: the bounded commands above were written for this
   document and have **not been run** on any host. What was observed on the
   operating host is only what the "Do not use ..." paragraph reports; the
   exact command line of the operator's held-open test is not recorded here.

2. **Build** the helper on the host (CMake, no Matter SDK needed; or take it
   from `tools/build_matter_bridge.sh`'s output, which assumes
   `/usr/sbin/ot-ctl`):

   ```sh
   cd linux/matter_bridge/app/api
   cmake -S . -B build -DPOGOHOME_OT_CTL_PATH="$(command -v ot-ctl)"
   cmake --build build --target pogohome-otbr-helper pogohome_api_unit_tests
   ctest --test-dir build --output-on-failure
   ```

3. **Install** it root-owned in a root-owned directory (nothing on the path
   may be writable by the bridge's user):

   ```sh
   sudo install -d -o root -g root -m 0755 /usr/local/libexec/pogohome
   sudo install -o root -g root -m 0755 build/pogohome-otbr-helper /usr/local/libexec/pogohome/pogohome-otbr-helper
   ```

4. **Allow exactly that command** to the bridge's account.

   First the account. It is the unit's `User=`, the same account
   "Operating" above calls `BRIDGE_USER`. Read it from the service; if you set
   it by hand instead, it is an example value that you must replace with your
   own account name.

   ```sh
   BRIDGE_USER="$(systemctl show -p User --value pogo-matter-bridge.service)"
   if [ -z "$BRIDGE_USER" ] || [ "$BRIDGE_USER" = root ]; then
     echo "STOP: no User= in the unit (the bridge runs as root), or the unit was not found" >&2
     unset BRIDGE_USER
   elif printf '%s\n' "$BRIDGE_USER" | grep -qxE '[a-z_][a-z0-9_-]*' &&
        [ "$(id -u -- "$BRIDGE_USER" 2>/dev/null)" -gt 0 ] 2>/dev/null; then
     echo "bridge account: $BRIDGE_USER"
   else
     echo "STOP: unexpected account name, unknown account, or uid 0 -- check the unit by hand" >&2
     unset BRIDGE_USER
   fi
   ```

   If this prints `STOP`, do not go on with the steps below. They are for a
   bridge that runs as a normal account. An empty `User=` means the bridge
   runs as root (or the unit name is different on this host): a sudoers rule
   for root restricts nothing, and a bridge running as root already has the
   whole OTBR CLI, which is what this procedure exists to avoid. Fix the unit
   first. The commands below use `${BRIDGE_USER:?...}`, which refuses to run
   while the variable is unset.

   Then the rule. One line, and it is the whole permission: that one account,
   as root, without a password, the one root-owned helper path, and `""`,
   which means "no arguments allowed". No wildcard, no second command and no
   `ot-ctl` entry belong in this file. Write it to a temporary file, let
   `visudo` check that file, and install it only if the check passed. A file
   with a syntax error must never be written into `/etc/sudoers.d` first: it
   can make `sudo` unusable for everyone.

   ```sh
   HELPER=/usr/local/libexec/pogohome/pogohome-otbr-helper
   RULE="${BRIDGE_USER:?set BRIDGE_USER as shown above} ALL=(root) NOPASSWD: $HELPER \"\"" &&
   SUDOERS_TMP="$(mktemp)" &&
   printf '%s\n' "$RULE" > "$SUDOERS_TMP" &&
   cat "$SUDOERS_TMP"      # read it: it must start with the account's NAME, not with the text $BRIDGE_USER
   ```

   ```sh
   [ "$(grep -Fxc -- "${RULE:?}" "${SUDOERS_TMP:?}")" -eq 1 ] &&
   sudo visudo -cf "$SUDOERS_TMP" &&
   sudo install -o root -g root -m 0440 "$SUDOERS_TMP" /etc/sudoers.d/pogohome-otbr-helper &&
   sudo visudo -cf /etc/sudoers.d/pogohome-otbr-helper
   rm -f "$SUDOERS_TMP"
   ```

   The line is written with `printf` on purpose. In a here-document whose
   delimiter is quoted (`<<'SUDOERS'`) the shell does not expand
   `$BRIDGE_USER`, and the file would then hold the literal text
   `$BRIDGE_USER` instead of the account. The `grep` line above refuses to
   install anything but the expected line.

   **sudo implementations.** Check which one the host has:
   `sudo --version | head -n 1`.

   - **sudo-rs.** The operating host had sudo-rs `0.2.13-0ubuntu1.2`. The
     rule that was installed and verified there is the single line above,
     in the form
     `<bridge-user> ALL=(root) NOPASSWD: /usr/local/libexec/pogohome/pogohome-otbr-helper ""`,
     and nothing else. An earlier version of this procedure also wrote
     `Defaults!/usr/local/libexec/pogohome/pogohome-otbr-helper !log_input, !log_output`;
     on that host `visudo` rejected that line as an unknown setting and the
     check failed. Do not add it there. What this sudo implementation can or
     cannot record of a command's input was not examined here; this document
     makes no statement about it.
   - **The original sudo (sudo.ws) with I/O logging.** I/O logging
     (`log_input`, `log_output`) is off unless the host's sudoers turns it
     on. If it is on, it would record the helper's stdin, where the PSKd
     travels. Only in that case write the temporary file with this command
     instead of the `printf` above, then continue with the same check and
     install block:

     ```sh
     printf 'Defaults!%s !log_input, !log_output\n%s\n' "$HELPER" "${RULE:?}" > "${SUDOERS_TMP:?}" &&
     cat "$SUDOERS_TMP"
     ```

     The file then has two lines, and the `grep` check still matches the
     rule line once. (The `!` characters are inside single quotes so that an
     interactive shell does not treat them as history expansion.) This variant was not installed on the operating host,
     so it is not verified there. It is not a required line for every host,
     and neither variant is a guarantee about what the rest of the system
     logs.

   The unit must allow `sudo` to raise privileges:
   `systemctl show -p NoNewPrivileges -p RestrictSUIDSGID pogo-matter-bridge.service`
   must print `no` for both (they are off unless the unit sets them). If the
   unit needs them on, use a root-run socket-activated service for the helper
   instead of sudo (not implemented here).

5. **Try it as the bridge's user** (nothing changes on the OTBR). Same shell
   as step 4, so `BRIDGE_USER` and `HELPER` are still set.

   `as_bridge` runs a command line as the bridge's account. If you are logged
   in as that account yourself, it must not go through `sudo -u`: that would
   authenticate you and cache sudo credentials for the very account under
   test.

   ```sh
   as_bridge() {
     if [ "$(id -un)" = "${BRIDGE_USER:?set BRIDGE_USER as shown above}" ]; then sh -c "$1"
     else sudo -u "$BRIDGE_USER" sh -c "$1"; fi
   }
   ```

   The two refusals first, each right after the bridge account's cached sudo
   credentials were removed. `sudo -n` never asks for a password, so with no
   cached credentials only a `NOPASSWD` rule can let a command through. With
   cached credentials, an account that also has password-protected sudo
   rights is let through, and the test then looks as if the rule were too
   wide although it is not. The administrator's own cached credentials say
   nothing about the bridge account either way.

   ```sh
   as_bridge 'sudo -K'; as_bridge "sudo -n -- $HELPER extra-arg"; echo "exit status $?"
   as_bridge 'sudo -K'; as_bridge 'sudo -n /usr/sbin/ot-ctl state'; echo "exit status $?"
   ```

   Both must be refused **by sudo**: a refusal message from sudo (its wording
   differs between sudo implementations), a non-zero exit status, and no
   output from the program. If the first prints `ok ...` or `err ...`, the
   helper ran, so sudo allowed the extra argument. If the second prints a
   state or `Done`, sudo allowed `ot-ctl`. Either means the sudoers
   configuration of this host gives that account more than the one rule;
   find out where before going on.

   Then the allowed call:

   ```sh
   as_bridge "printf 'pogohome-otbr/1 commissioner-state\n' | sudo -n -- $HELPER"
   # ok disabled | ok petitioning | ok active
   as_bridge "printf 'pogohome-otbr/1 mesh-local-eid\n' | sudo -n -- $HELPER"
   # ok mleid <the same address `sudo ot-ctl ipaddr mleid` prints>   (read-only; a helper built before 2026-10-03 answers: err invalid_request)
   journalctl -t pogohome-otbr-helper -n 5      # audit lines, no PSKd
   ```

   Recorded from the operating host (run by the operator, see
   [`validation-status.md`](validation-status.md)): with the sudo
   authentication cache cleared, the call with an extra argument was blocked;
   `commissioner-state` answered `ok disabled`; `mesh-local-eid` answered the
   address `ot-ctl ipaddr mleid` printed. The refusal of a direct `ot-ctl`
   call is not in that record. The `as_bridge` wrapper and the exact command
   lines above were written for this document and have not been run on any
   host.

6. **Enable it in the bridge** by adding
   `--api-enroll-helper /usr/local/libexec/pogohome/pogohome-otbr-helper` to
   the unit's `ExecStart=`, then `systemctl daemon-reload && systemctl restart
   pogo-matter-bridge` when ready.

To undo: remove the flag and restart the bridge, then
`sudo rm /etc/sudoers.d/pogohome-otbr-helper /usr/local/libexec/pogohome/pogohome-otbr-helper`.

Notes: the OTBR daemon serves one CLI session at a time, so each helper call
briefly replaces an interactive `ot-ctl` someone has open. The helper logs
each operation and EUI-64 (never the PSKd) to syslog as
`pogohome-otbr-helper`. `sudo` logs each run without arguments.

### Still to verify on the operating host and a spare board

Host tests prove the logic against fakes only. Before relying on it:

- On the Linux host: the full CMake test suite and the GN/Matter build
  (`tools/build_matter_bridge.sh`) with the host's GCC; the steps in
  "Installing the OTBR helper" (especially stdin mode of this `ot-ctl`).
- With the helper installed and the spare board running the Joiner probe:
  a session through the API reaches `waiting_for_heartbeat`;
  `sudo ot-ctl commissioner joiner table` shows exactly that EUI-64; after
  cancel or expiry the entry is gone; `journalctl -u otbr-agent` and the
  bridge's journal contain no PSKd. The probe never sends heartbeats, so such
  a session must end `expired`, never `heartbeat_received`.
- That the board's Joiner EUI-64 equals the `uuid` its Power Base firmware
  reports. Both come from the ESP32-H2 eFuse (ESP-IDF v6.1 builds them the
  same way); the mode-2 firmware compares them at boot and refuses the Joiner
  if they differ (`pb_join status` shows a warning), but this has not been
  observed on hardware yet.
- A real `heartbeat_received` with the mode-2 bench firmware on a spare board
  (procedure: [`validation-status.md`](validation-status.md) "Power Base 앱
  등록(mode 2)"), and that a wrong PSKd, a closed window or a missing OTBR
  ends the session `expired`.
- Bridge endpoint delivery (2026-10-03). **As recorded on 2026-10-03:** it
  had not been run on the Linux host at all: neither the GCC/Matter build of
  the changed sources, nor the rebuilt helper's `mesh-local-eid` against the
  real `ot-ctl` (including whether this `ot-ctl` build echoes the command,
  which the helper tolerates only as the exact line `ipaddr mleid`), nor a
  session with `bridge_endpoint_version`. That sentence describes that day
  only. **Follow-up (2026-10-05, run by the operator)**, recorded in
  [`validation-status.md`](validation-status.md) "허브 Thread 수신 주소 자동
  전달" under "후속 기록: Linux 배포 검증과 실물 시험 산출물 준비
  (2026-10-05)" and "실물 후속 검증 (2026-10-05)":
  - The hub API, the helper and their tests were built and passed with the
    host's GCC, and the GN/CHIP bridge build passed.
  - The new bridge and the new helper were installed on the operating hub.
    The hub's state (hub id, paired devices, settings, TLS files, endpoint
    map) was preserved, and the sudoers rule was left as it was.
  - The installed helper's `mesh-local-eid` returned one address from the
    real OTBR, the same one `ot-ctl ipaddr mleid` printed there.
  - A session started with `bridge_endpoint_version` fixed its endpoint (the
    bridge's journal: `bridge endpoint fixed for this session (port 5684)`).
    That session ended `cancelled` and is not counted as a success. In a
    second session the app showed `heartbeat_received` for the spare board's
    EUI-64.

  Not confirmed by that follow-up: the session's `bridge_endpoint` JSON was
  not captured, and the address delivered during the test was not compared
  with the OTBR's address directly. The second session's journal lines were
  not collected. The answers the helper must refuse (an RLOC, an address
  list, ...) were not produced against the real OTBR; that part is still
  host tests only. Whether this `ot-ctl` build echoes the command was not
  reported separately.
- Journal output: `pogohome_api: enrollment ...` and `[heartbeat] ...` go to
  stdout, which stdio block-buffers under systemd, so they could appear late
  or not at all in `journalctl` while stderr lines appeared at once. The
  source now makes stdout line-buffered (`app/chip/main.cpp`); a bridge built
  before that change still shows the old behavior until it is rebuilt and
  redeployed.

## Power Base management and removal (management revision, 2026-10-05)

The protocol, the board's record and the meaning of "removed" are specified
in [`power-base-management.md`](power-base-management.md); this section is
the HTTP side. **Verified by host tests and builds only** -- not run on the
operating hub, with a board, or built with GN/CHIP (see
[`validation-status.md`](validation-status.md)).

Management is on whenever enrollment is requested (`--api-enroll-helper`)
and the API port has TLS. There is no separate flag. Its state lives in
`<--state-dir>/pogohome/power_base_mgmt.json` (0600, written atomically):
management credentials per enrollment generation, removal jobs, the blocked
Power Bases and the pending settings clean-ups. A file that exists but cannot be read turns off only
this feature (`power_base_removal: "unavailable"`, the routes below answer
`503 removal_unavailable`, enrollment sessions that ask for a credential
answer `503 enrollment_unavailable` with `"reason":"management"`); the file
is never replaced. The key of a credential is written only to that file and
to the one `202` answer described below -- never to a log, a `GET`, the
snapshot, SSE or Bonjour.

### Health and snapshot

`GET /api/v1/health` adds `"power_base_management": 1` and
`"power_base_removal": "available"` or `"unavailable"`. Since Linux bridge
0.5.0 it also says `"power_base_removal_request": 2` (after
`power_base_forget`): what `POST /api/v1/power-base-removals` has to carry.
`2` is the request that names the registration it was confirmed for (below);
a hub without the field takes the Base's id alone, which names no
registration. A client that does not find the field, or finds a number it
does not know, must not send a removal request to that hub.

The snapshot (GET and SSE) adds:

- `"management": "supported"` or `"unsupported"` in each `power_bases` entry:
  whether the hub holds an **active** management credential for that Base's
  current enrollment. Only a `supported` Base can be removed.
- `"erase_confirmation": "automatic"` or `"manual"` in each `supported`
  `power_bases` entry, after `management` (added 2026-10-08, Linux bridge
  0.2.0; optional -- a hub that predates it sends none): how the erase behind
  a removal of that Base would be confirmed. `automatic`: its board announced
  (an authenticated management message) that it sends its release receipt to
  the hub itself, so a removal needs nothing but the request. `manual`: it
  announced nothing (older firmware); someone will have to collect the
  receipt over BLE. A hint for what a client tells its user before asking.
- `"removal_serial": <n>` in each `supported` `power_bases` entry, after
  `record` (added 2026-10-11, Linux bridge 0.5.0): how many removals were
  asked of the listed registration so far. A removal request names it,
  together with `record`; see "Removal routes".
- Entries for Power Bases the hub holds a credential for but has not heard
  since it started (`"online": false`, `module_count` 0), so that a Base
  waiting for its removal does not vanish from clients after a hub restart.
- `"power_base_removals": [ … ]`: every removal job the hub still lists
  (same JSON as below). The Registry generation is bumped whenever a job
  or its settings clean-up changes, so SSE clients see it without polling.
  A `released` job is listed until it is completed or superseded, however
  long that takes -- its Base is no longer in `power_bases`.

**One point in time (Linux bridge 0.4.1).** A snapshot (GET and SSE) never
lists a Power Base together with the record that took it out of the list --
a `released`/`completed` removal of that registration, or a list removal
(`power_base_forgets`). The server copies the Registry and then reads
management; everything management contributes is read in one critical
section, and a Registry copy that was made before management last took a
Base out of the list is replaced there by one made at that moment
([`power-base-management.md`](power-base-management.md), section 14).
Decided from the Registry's generation counter, never from a time. Hubs
before 0.4.1 could send such a mixed snapshot when another request was
carried out between the copy and the reading; a client must not read "the
Base is listed again" from a Base that is listed without a `record`, or
under the very `record` the removal names.

**Out of the list (Linux bridge 0.4.2): `power_base_departures`.** A
top-level array after `power_base_forgets`, with one entry per Power Base
that is **not in this hub's list now** because its registration here ended
and it has not been registered again -- exactly the blocked Bases (see
"Blocked Power Bases"):

```json
"power_base_departures": [
  {"power_base_uuid":"<16 hex>","record":"<32 hex>","reason":"removed","since_unix_ms":1790000100000}
]
```

- `record`: the registration that ended (what `record` was while the Base
  was listed; a removal job's `generation`). `reason`: `"removed"` -- the
  board released the registration (a removal's notice or receipt) -- or
  `"forgotten"` -- it was taken out of the list while offline; nothing says
  the board erased anything. Both keys are **left out** for a block a hub
  before 0.4.2 stored; the entry then says only that the Base is out of the
  list.
- This is the hub's present state, not history. It is in **every** snapshot
  for as long as the Base stays unregistered -- hours later, after a hub
  restart -- and it is gone with the snapshot that lists the Base again.
  How long a finished removal job or a list-removal record is *shown*
  (below) has nothing to do with it. A client that kept the modules of a
  Base on screen while the list was incomplete drops them when the Base is
  named here; it needs no job and no record for that, and must not remember
  an entry from an earlier snapshot.
- A Base named here is never in `power_bases` of the same snapshot. If a
  client ever sees both, the list stands.
- At most one entry per blocked Base, and the blocks are capped (64).
- Absent from a hub without management, and from hubs before 0.4.2: a client
  then falls back to the removal jobs and list-removal records it sees.

### Enrollment with a management credential

`POST /api/v1/power-base-enrollments` accepts one more optional field,
`"management_version": 1`, only together with `"bridge_endpoint_version": 1`.

- Without it the request and the answer are exactly as before (no
  `management` key anywhere).
- With it the hub creates a new enrollment generation (16 random bytes) and a
  management key (32 random bytes, `RAND_bytes`), stores them as a
  provisional credential **before** the session's worker starts, and answers
  `202` with `"management": {"version": 1, "generation": "<32 hex>",
  "hub_id": "<32 hex>", "key": "<64 hex>"}` inside the session. `key` and
  `hub_id` appear in that one answer only; `GET` and `DELETE` answers of the
  same session carry `{"version": 1, "generation": …}`.
- Errors: `400 invalid_request` (`management_version` without
  `bridge_endpoint_version`, or a wrong type), `422
  unsupported_management_version` (not 1), `503 enrollment_unavailable` with
  `"reason":"management"` (management is not usable right now; no session is
  made), and `409 management_required` (see "Blocked Power Bases").
- The credential becomes active when the board's first authenticated
  management poll arrives. Until then the Base is `"management":
  "unsupported"` in the snapshot and cannot be removed.

### Removal routes (TLS, `Authorization: Bearer <token>`)

Checked like the enrollment routes: not requested → `404 removal_disabled`;
plain HTTP → `403 tls_required`; management unusable → `503
removal_unavailable`; wrong method → `405`; no/unknown token → `401`. Every
answer has `Cache-Control: no-store`. These routes are separate from the
enrollment session's `DELETE`, which only cancels a session.

| route | result |
|---|---|
| `POST /api/v1/power-base-removals` `{"power_base_uuid":"<16 hex>","record":"<32 hex>","removal_serial":<n>,"request_id":"<32 hex>"}` (`Content-Length` only, at most 256 bytes, exactly these four members) | `202` + job (new), `Location: /api/v1/power-base-removals/<removal_id>`; `200` + job when this very request made a job before, or a removal of the registration it names is under way; see "The removal request" |
| `GET /api/v1/power-base-removals` | `200` `{"removals":[…]}` |
| `GET /api/v1/power-base-removals/<removal_id>` | `200` + job; `404 removal_not_found` |
| `DELETE /api/v1/power-base-removals/<removal_id>` | cancels a `pending` job: `200` + job (`cancelled`); otherwise `409 removal_not_cancellable` with the job as it is |
| `POST /api/v1/power-base-removals/receipt` `{"receipt":"<192 lowercase hex>"}` (at most 512 bytes, exactly this field) | the board's release receipt: `200` + job (`completed`); the same receipt again is the same answer |

Errors of the request: `400 invalid_request` (not exactly the four members,
or one of the wrong JSON type), `422 invalid_power_base_uuid`, `422
invalid_record`, `422 invalid_removal_serial` (not an integer from 0 to
2^32-1), `422 invalid_request_id`, `422 removal_registration_required` (the
body is `power_base_uuid` alone: the request of a client older than this
contract), `409 stale_record`, `409 request_id_conflict`, `404
unknown_power_base` (neither in the Registry nor a Base the hub holds a
credential for), `409 power_base_management_unsupported` (no active
credential -- the hub offers no fallback and removes nothing), `409
enrollment_in_progress` (a session for that EUI-64 is running), `503
too_many_removals` (32 jobs waiting for their board), `409
removal_capacity_full` with `"limit":"64"` (see "Blocked Power Bases"). No
error answer echoes a value of the request.

#### The removal request (Linux bridge 0.5.0)

A removal is asked of **one registration** of a Power Base, not of the
Base. The Base keeps its EUI-64 for as long as the board exists; a
registration ends (the Base is removed) and another begins (somebody
registers it again), under another `record`. A request that named the
EUI-64 alone and arrived -- or was sent once more after a lost answer --
after that would create a removal job for a registration nobody asked to
remove, and the hub would hand that registration's board an UNENROLL.
([`power-base-management.md`](power-base-management.md), section 16.)

| member | what it is |
|---|---|
| `power_base_uuid` | the Base |
| `record` | the registration the user was shown and confirmed: the `record` of that Base's `power_bases` entry |
| `removal_serial` | how many removals had been asked of that registration then: the entry's `removal_serial` |
| `request_id` | 32 lowercase hex the client chooses for **this confirmation**. The same request again -- after a lost answer -- carries the same id; a new confirmation by the user carries a new one |

The hub creates a job only when the Base is listed under `record` now, no
removal of it is under way, and `removal_serial` is that registration's
present value; creating the job moves the serial on, in the same stored
change. What follows from that:

| the request… | answer |
|---|---|
| names what is listed, with the present serial | `202`, the new job (it carries `request_id`) |
| has the `request_id` of a job the hub still has (under way, or finished and still listed) | `200`, that job as it is now -- `cancelled`, `completed` or `superseded` as it may be. Nothing new |
| names a registration whose removal is under way, whoever asked for it (also after `released`, when the Base has left the list) | `200`, that job |
| names another registration than the one listed (the Base was removed and registered again, or a registration was started) | `409 stale_record`: nothing is created, the board gets no command |
| carries a serial that has passed (a removal was asked of this registration since, and has ended) | `409 stale_record` |
| uses a `request_id` that belongs to a job of another Base | `409 request_id_conflict` |

So one request creates at most one job, and only for the registration it
names. That does not depend on the hub remembering the request: when the
finished job has left the hub's list (after ten minutes), or the hub was
started again, the same request is no longer answered with its job -- it is
refused, by the registration or by the serial, both of which are kept with
the registration itself. Nothing turns a forgotten request into a request
for what is listed now.

The request of an earlier client -- `{"power_base_uuid":"<16 hex>"}` and
nothing else -- names no registration and cannot be told from any of the
cases above. It is refused for every Base, listed or not (`422
removal_registration_required`), before anything is looked up. There is no
compatibility path that applies it to "the registration listed now": that
path is the defect. Such a client can still read the jobs, cancel one and
relay a receipt.

Errors of the receipt: `400 invalid_request` (not exactly that one field),
`422 invalid_receipt` (not 192 lowercase hex digits, or not an
`UNENROLL_DONE` management message with result `ERASED` -- the board's
"released" notice handed in here is refused this way), `409
receipt_rejected` (well formed, but not one this hub can verify for one of
its removals: another hub, an unknown or superseded generation, a cancelled
job, a wrong MAC; one answer for all of them).

```json
{"removal_id":"<32 hex>","power_base_uuid":"<16 hex>","generation":"<32 hex>",
 "state":"pending","result_unknown":false,"cancellable":true,
 "created_at_unix_ms":0,"updated_at_unix_ms":0,"completed_at_unix_ms":null,
 "requested_by_device_id":"<16 hex>","affected_module_count":2,
 "settings_cleanup_pending":false,"erase_confirmation":"automatic",
 "request_id":"<32 hex>"}
```

`request_id` (added 2026-10-11, Linux bridge 0.5.0; optional, last) is the id
of the request that created the job: how the client that asked recognises its
own request's job when the answer to it was lost. Absent on a job stored by a
hub that did not write it down. `generation` is the registration the job is
about (what `record` was for the Base when it was listed).

`erase_confirmation` (added 2026-10-08, Linux bridge 0.2.0; optional) is the
last field of a job that is **not final**, and absent from a final one --
whose JSON is exactly what it was. A hub that predates it sends none.

| value | when |
|---|---|
| `automatic` | the board of that enrollment announced that it sends its receipt itself, the job is not `result_unknown`, and -- once `released` -- no more than 4 minutes have passed since. Nothing for anyone to do |
| `manual` | the board announced nothing; or it did and no receipt arrived within 4 minutes of `released`; or the job is `result_unknown`. The receipt has to be read from the board over BLE and handed in (`POST …/receipt`), or the board enrolled again |

It is a hint for what a client shows and offers. It changes no state, and a
receipt that arrives after `manual` was reported completes the job like any
other. Clients must treat an absent or unknown value like `manual`.

`state`:

| state | meaning |
|---|---|
| `pending` | stored; the command has not been given to the board (where an offline Base waits). Cancellable |
| `sent` | the command went out in an answer to the board's authenticated poll |
| `accepted` | the board's authenticated `UNENROLL_ACCEPTED` arrived |
| `released` | the board's authenticated `UNENROLL_DONE`/`RELEASED` arrived: it stored that this enrollment is out of use for good, and the hub has removed it (below). **Not complete**: the erase of the board's stored join information is not confirmed |
| `completed` | final: the board's authenticated release receipt (`UNENROLL_DONE`/`ERASED`) was verified. The board makes it only after it erased its stored network, read the store back empty and stored its final record. The receipt arrives from the board itself (CoAP `pogo/v1/mgmt`, see below) or relayed by a client (`POST …/receipt`); it is the same message either way |
| `cancelled` | final: cancelled while `pending` |
| `superseded` | final: a new enrollment generation of the same board became active first |

`result_unknown` turns `true` when `sent`/`accepted` made no progress for 5
minutes; it is a hint, not a state. Nothing but the verified receipt makes a
job `completed`: not the released notice, not a timeout, not silence, not a
restart. A `released` job stays `released` for as long as no receipt arrives
and no new enrollment supersedes it. `completed_at_unix_ms` is `null` unless
`completed`. `settings_cleanup_pending` is a separate matter (below). Jobs
survive a hub restart, the app closing and the requesting device being
unpaired. A finished job is listed for 10 minutes (longer while its settings
clean-up is pending), and at most 32 finished ones are kept.

`generation` names the registration the job is about -- the same value as
the `record` of that Base in `power_bases` while that registration is the
listed one. A job whose `generation` differs from the listed Base's `record`
is about an earlier registration of the board.

`"list_removed": true` (added 2026-10-10, Linux bridge 0.4.1; present only
when true) on a `sent` or `accepted` job: the Base was taken out of this
hub's list (below, "Removing a Power Base from the hub's list") while the
command may be with the board. Two separate answers: the **list** is settled
-- the Base is not in `power_bases`, its data is refused -- and the **board**
is not: `state` is unchanged, `completed_at_unix_ms` stays `null`, and the
job, its key and the block are kept so that the board's receipt is still
verified should it ever arrive. The field is gone once the board's
`released` notice moves the job on; a new registration of the board ends the
job as `superseded`. It never stands for `completed`.

The hub never sends anything to a board on its own: the command travels in
the answer to the board's authenticated poll on the ingest port (CoAP
`pogo/v1/mgmt`), once per poll, always with the same command id.

**The two ways a receipt comes in.** A board erases its stored network only
after it told the hub "released" (OpenThread refuses to erase while Thread
runs), and afterwards nothing of the network is left in its flash.

- **From the board itself (Power Base 0.2.0).** After its final record is
  stored, the board brings Thread up once more from a copy it kept in RAM --
  with OpenThread's persistent stores switched to RAM, so that nothing is
  stored again -- and posts the receipt to `pogo/v1/mgmt` on the ingest port
  (`docs/power-base-management.md`, section 4). The hub verifies it, completes
  the job and answers `RESULT_ACK`. No client takes part. The same receipt
  again gets the same acknowledgement (the hub keeps its MAC with the
  completed job; the key is gone).
- **Relayed by a client.** The board keeps the receipt in its final enrollment
  record and hands it out over BLE, inside a Security 2 session; the app
  relays it with `POST …/receipt`. This is the way for a board that does not
  send it (older firmware) and for a receipt that did not get through (the
  board lost power in that last session, or the hub was not reachable).

Either way the device token, the Thread address or the route say nothing
about the receipt. What it says is believed only because its HMAC verifies
with the management key of that enrollment generation, which no client ever
has.

### What a released removal changes on the hub

Done once, when the released notice is verified (or, if that notice was
lost, when the receipt completes the job). The job state, the block and the
clean-up list are one file write; the Registry follows only after it
succeeded.

1. The EUI-64 is blocked (persistently). Its `heartbeat`, `topology`,
   `sample` and module-removal messages are answered `4.03` and change
   nothing in the Registry.
2. The Base and the nodes registered **under that Base** leave the Registry.
   A module that already moved to another Base is registered under that
   Base and stays.
3. Matter endpoints go through the existing reconcile path (a module that
   left the snapshot → `remove_endpoint`). `endpoint_map.bin` keeps the
   UUID → endpoint id record, so the same module gets the same id if it
   comes back. Other endpoints, the KVS, pairings, the TLS identity and
   `hub_id` are untouched.
4. The modules whose room assignment has to be cleared are written into the
   state file (see "Settings clean-up").

`pending`, `sent`, `accepted`, `cancelled` and `superseded` block and remove
nothing. `completed` changes nothing more on the data path: it records that
the erase was confirmed and drops the generation's key.

### Settings clean-up

The modules that left with a Base -- unenrolled, or taken out of the list --
and are not registered under another Base lose their room assignment in the
shared settings (name kept), saved through the normal revision rule. This is
tracked separately from the removal itself: `settings_cleanup_pending` in
the job (or in the list removal's record).

- The modules and the retry state are in the state file (`cleanups`), written
  by the same write that stored the release, so a process that ends right
  afterwards still knows what is owed.
- **Bound to one registration's assignment (0.4.1).** With each module the
  entry holds the room it was in at that moment (`rooms`; absent when the
  settings could not be read then, and in entries written by an earlier
  hub). The clean-up clears *that* assignment: a module that is in another
  room by the time it runs was given that room since, and keeps it. A module
  without a room at that moment is not in the entry at all.
- A worker thread does the settings write: never the ingest or an HTTP
  thread.
- Every attempt asks the Registry where each module is **now** -- when the
  pass starts, and then **decides and writes in one section** (0.4.2): the
  settings are read first, then the management and Registry locks are
  taken and kept, the Registry is copied under its lock, the modules of this
  attempt are decided from that copy, and the conditional settings write is
  made before either lock is given up. No report can be applied in between,
  so a module that is listed under a Base keeps the room it has -- also when
  nobody saved the settings since they were read (then the write is no
  revision conflict; 0.4.1 asked first and wrote afterwards, and cleared
  that room). A module registered under
  any Base at that moment (it moved, or its Base was registered again)
  leaves the clean-up for good. So does one that was listed under a Base at
  any time since the clean-up was noted, even if it is on none now and
  nobody looked while it was: the Registry remembers when each module was
  last newly listed. Whoever saves a room in between makes the clean-up's
  write a revision conflict, and the next attempt starts from their
  settings and goes through the same section. (Before 0.4.1 the question
  was asked once, before the write, and the write cleared by module UUID
  alone: a module that docked elsewhere and got a new room in between lost
  it.)
- While that section runs -- one settings file write -- reports and snapshot
  requests wait. Lock order: management, Registry, settings store; nothing
  takes them the other way round.
- After a restart (empty Registry) a clean-up read from the file -- or one
  that rests on `members` read from the file -- waits 120 seconds before its
  first attempt.
- **What is known of a Base that has not reported since the restart
  (0.4.1).** The state file lists, per listed Base, the modules the Registry
  last had under it (`members`). Unenrolling such a Base, or taking it out of
  the list, names those modules for the clean-up. A module another Base
  reported since belongs to that Base. What was never written down -- a file
  from an earlier hub, a process that ended first -- is not cleaned: no
  module is made up.
- A failure (settings file cannot be written or is damaged, or other writers
  win four attempts in a row) is retried with a backoff from 5 seconds up to
  10 minutes, without limit. Success removes the entry. If the clean-up was
  done but that could not be stored, it stays pending and is done again;
  clearing a room that is already cleared changes nothing and saves no
  revision.
- Repeated notices, the receipt and restarts never create a second clean-up.

### Blocked Power Bases

A blocked EUI-64 is unblocked in exactly one way: a **new enrollment
generation** becomes active (the board's first poll authenticated with the
new key). A heartbeat, an edit in an app, a hub restart, time, the job being
completed or dropped from the history, and an enrollment session without
`management_version` do not unblock it; the latter is refused with `409
management_required`.

The list holds at most 64 blocks and **none is ever dropped to make room**.
Room for a block is reserved when a removal is requested: the slots in use
are the blocked Bases plus the jobs that may still end in a block (`pending`,
`sent`, `accepted`). When all 64 are in use a request answers `409
removal_capacity_full`; no job is stored, so that board never gets a
command. A cancelled or superseded job gives its slot back. The count is
derived from the stored jobs and blocks, so it is the same after a restart;
a request that could not be stored reserves nothing; asking again for a Base
that already has a job takes no second slot. The only way to free a slot of
a blocked Base is to enroll that board again.

Credentials are bounded too (96): a new session makes room only by dropping
the oldest *provisional* one and is refused otherwise. Active credentials
and those of `released` jobs (needed to verify their receipts) are never
dropped for it; a credential is released when its receipt completed the job.

### Removing a Power Base from the hub's list (2026-10-10)

For a Power Base that does not come back (broken, lost, reset by its button
and registered at another hub). Specified in
[`power-base-management.md`](power-base-management.md), section 13. **This
is not a removal**: the hub drops its own record of one registration; the
board's stored join information is not erased and no field ever says it was;
no removal job becomes `completed` by it. Verified by host tests on a Mac
only.

Announced on its own, so that a client never assumes it:

- `GET /api/v1/health` adds `"power_base_forget": 1` after
  `power_base_removal`.
- Every `power_bases` entry of the snapshot (GET and SSE) the hub has a
  record for adds `"record": "<32 hex>"` after its management fields: the
  registration a request must name. Not a secret (for a Base with a
  credential it is the generation of the newest registration the hub issued
  for that board -- one that was only started counts; for a Base known from
  its data alone, a value this hub process made up, gone with a restart).
- A top-level `"power_base_forgets": [...]` follows `power_base_removals`.
- Since 0.4.2 a top-level `"power_base_departures": [...]` follows that (see
  "Out of the list" above). `power_base_forget` in health stays `1`: no
  route and no request changed.

A hub without the feature (0.3.0 and earlier) sends none of the three. A
client that does not know them ignores them (they are additive;
`schema_version` stays 1).

Routes -- the gates of the removal routes: TLS, `Authorization: Bearer
<token>` of a paired device, management available. An EUI-64 or a Thread
address is never an authorization.

| Request | Answer |
|---|---|
| `POST /api/v1/power-base-forgets` with exactly `{"power_base_uuid":"<16 hex>","record":"<32 hex>","request_id":"<32 hex>"}` | `201` + the record and `Location`: carried out. `200` + the same record: this `request_id` was carried out before (nothing is done twice) |
| `GET /api/v1/power-base-forgets` | `200 {"forgets":[...]}`, most recent first |
| `GET /api/v1/power-base-forgets/<forget_id>` | `200` + the record, or `404 forget_not_found` |
| any other method (in particular `DELETE`) | `405`: there is nothing to cancel or undo |

The record:

```json
{"forget_id":"<32 hex>","request_id":"<32 hex>","power_base_uuid":"<16 hex>",
 "record":"<32 hex>","state":"removed_from_hub","managed":true,
 "created_at_unix_ms":1790000000000,"requested_by_device_id":"<16 hex>",
 "affected_module_count":2,"cancelled_removal_id":null,"kept_removal_id":null,
 "settings_cleanup_pending":false}
```

`state` is always `removed_from_hub`. `cancelled_removal_id`: a removal of
that Base that was still `pending` ended as `cancelled` in the same stored
change. `kept_removal_id`: a removal whose command may be with the board
(`sent`/`accepted`) was left exactly as it is, with its key, and stays in
`power_base_removals` until the board's proof arrives or the board is
registered again. A record is listed as long as a finished removal (10
minutes), longer while its settings clean-up is pending, and (Linux bridge
0.4.1) **for as long as it is the reason its Base is not listed**: the last
record of a Base stays until that Base is registered again. A client that
was away when the Base was taken out -- in the background for an hour --
finds the record in the same snapshot that no longer lists the Base. At most
one such record per blocked Base.

Refusals of the `POST` (nothing changed in any of them):

| Status | `error` | Meaning |
|---|---|---|
| 400 | `invalid_request` | not exactly the three members |
| 422 | `invalid_power_base_uuid`, `invalid_record`, `invalid_request_id` | |
| 404 | `unknown_power_base` | the hub does not list that Base: never heard of, already taken out of the list, or released by a removal |
| 409 | `power_base_online` | the hub hears the Base (or a module under it) at this moment. Decided, stored and applied as one step under the Registry's lock -- what an app displayed plays no part |
| 409 | `stale_record` | `record` is not the registration the hub lists now: the board was registered again, or a registration was started, after the client read the list |
| 409 | `enrollment_in_progress` | an enrollment session for that EUI-64 is running |
| 409 | `request_id_conflict` | the `request_id` was used for another Base |
| 409 | `forget_capacity_full` | no block slot is free (64; none is ever dropped to make room) |
| 503 | `too_many_forgets` | 64 records, none of which can be dropped yet |
| 503 | `forget_unavailable` | `reason` `startup` (state file damaged) or `storage` (it could not be written) |
| 404 | `forget_disabled` | management is not configured |

What it does on the hub: the credentials of that registration are dropped,
the Base is blocked exactly like a released one ("Blocked Power Bases"
above -- only a new authenticated enrollment generation unblocks it), it and
the modules registered under it leave the Registry (and with that the
snapshot, SSE and their Matter endpoints), and the rooms of the modules that
left are cleared by the settings clean-up above. A lost answer is not a
"no": send the same `request_id` again.

### Still to verify

Everything here on real hardware and the operating hub: the GN/CHIP build of
`pogo_matter_bridge` with `power_base_management.cpp` (including the
clean-up worker started from `main.cpp`), the routes over the real TLS
listener, persistence across a real service restart, the ingest block
against a real board, a receipt read from a real board over BLE and relayed
by a real phone, Matter endpoint removal seen by a real controller, and the
settings revision seen by two real devices.

## Future work

- Registering and removing a Power Base as a product (removal in two steps:
  released, then completed by the board's receipt) is implemented in the
  hub, the firmware (BLE build) and the app's Release screens
  ("Power Base management and removal" above) but has only been run in host
  and simulator tests. The earlier paths still exist: the app's DEBUG tests
  and a bench-only firmware path (PSKd typed on the spare board's USB
  console, bridge address pre-set in its `cfg` image). A QR flow does not
  exist. Operating Bases added by flashing firmware and `cfg` (see
  [`power-base.md`](power-base.md) "여러 Power Base 추가") have no management
  credential and cannot be removed from the app.
- The PogoHome iOS app (`pogo-sensor-ios`, a separate repository) connecting
  to this API is its own, later milestone -- this follow-up only implements
  and documents the server side.
