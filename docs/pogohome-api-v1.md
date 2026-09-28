# PogoHome Local API v1

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

### `GET /api/v1/snapshot`

The full current state in one response:

```json
{
  "schema_version": 1,
  "generated_at_unix_ms": 1732000000000,
  "registry_generation": 42,
  "power_bases": [
    {"uuid": "744dbdfffe650a00", "online": true, "module_count": 1, "online_module_count": 1}
  ],
  "modules": [
    {
      "uuid": "744dbdfffe65d357",
      "power_base_uuid": "744dbdfffe650a00",
      "parent_uuid": "744dbdfffe650a00",
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
    {"uuid": "744dbdfffe650a00", "online": true, "module_count": 2, "online_module_count": 2},
    {"uuid": "744dbdfffe650b00", "online": true, "module_count": 2, "online_module_count": 2}
  ],
  "modules": [
    {"uuid": "744dbdfffe650a01", "power_base_uuid": "744dbdfffe650a00", "parent_uuid": "744dbdfffe650a00", "online": true, "depth": 1, "role": "intermediate", "sensor_bitmap": 6, "generation": 1, "last_seq": 1,
     "readings": [{"kind": "temperature", "raw_value": 22270, "scale": 1, "value": 22.270, "unit": "celsius", "status": "ok", "sample_seq": 27}, ...]},
    {"uuid": "744dbdfffe650a02", "power_base_uuid": "744dbdfffe650a00", "parent_uuid": "744dbdfffe650a01", "online": true, "depth": 2, "role": "leaf", "sensor_bitmap": 896, "generation": 1, "last_seq": 1,
     "readings": [{"kind": "pm1", "raw_value": 4, "scale": 3, "value": 4, "unit": "ug/m3", "status": "ok", "sample_seq": 27}, ...]},
    {"uuid": "744dbdfffe650b01", "power_base_uuid": "744dbdfffe650b00", "parent_uuid": "744dbdfffe650b00", "online": true, "depth": 1, "role": "intermediate", "sensor_bitmap": 6, "generation": 1, "last_seq": 1,
     "readings": [{"kind": "temperature", "raw_value": 22060, "scale": 1, "value": 22.060, "unit": "celsius", "status": "ok", "sample_seq": 6}, ...]},
    {"uuid": "744dbdfffe650b02", "power_base_uuid": "744dbdfffe650b00", "parent_uuid": "744dbdfffe650b01", "online": true, "depth": 2, "role": "leaf", "sensor_bitmap": 896, "generation": 1, "last_seq": 1,
     "readings": [{"kind": "pm1", "raw_value": 4, "scale": 3, "value": 4, "unit": "ug/m3", "status": "ok", "sample_seq": 6}, ...]}
  ]
}
```

Once Base `..0a00` has been silent for the stale timeout (120 s in
production), the next SSE snapshot carries the following. This is the shape asserted by the
host test `test_multi_power_base.cpp`:

```json
"power_bases": [
  {"uuid": "744dbdfffe650a00", "online": false, "module_count": 2, "online_module_count": 0},
  {"uuid": "744dbdfffe650b00", "online": true, "module_count": 2, "online_module_count": 2}
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

Only `PUT /api/v1/settings` and `POST /api/v1/pairing/claim` read a body
(`Content-Length` only, at most 256 KiB). The API never adds CORS headers (a
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
 "modules": [{"uuid": "744dbdfffe65d357", "name": "공기" | null, "room_id": "…" | null}]}
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

## Future work

- Registering a Power Base from a client. The API is read-only. A Base is
  registered by flashing its firmware and `cfg` image (see
  [`power-base.md`](power-base.md) "여러 Power Base 추가").
- The PogoHome iOS app (`pogo-sensor-ios`, a separate repository) connecting
  to this API is its own, later milestone -- this follow-up only implements
  and documents the server side.
