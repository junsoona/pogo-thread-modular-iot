# PogoHome Local API v1

A read-only HTTP/SSE API, served by the existing `pogo_matter_bridge` process
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

- **Read-only.** Every route is `GET`. There is no way to change Registry
  state, module/room metadata, or `pogo_matter_bridge`'s own configuration
  through this API -- `POST`/`PUT`/`PATCH`/`DELETE` are not implemented at
  all (an unsupported method on a known path is `405`).
- **No authentication today.** This API trusts whoever can reach it. It must
  only ever be bound to a trusted private LAN interface -- **never** expose
  it beyond your home network (no port-forwarding, no public reverse proxy).
  A future revision may add authentication (see "Future work" below); until
  then, treat reachability itself as the access control.
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
  "module_count": 2
}
```

`registry_generation` is `ingest::Registry::generation()` -- a counter bumped
once per genuinely-new topology/sample applied (never for a duplicate or
rejected report). `module_count` counts modules the Registry currently has
topology for (regardless of online/offline).

### `GET /api/v1/snapshot`

The full current state in one response:

```json
{
  "schema_version": 1,
  "generated_at_unix_ms": 1732000000000,
  "registry_generation": 42,
  "modules": [
    {
      "uuid": "744dbdfffe65d357",
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

This API never reads a request body (none of its routes need one) and never
adds CORS headers (a native iOS app making a direct request needs none).

## `curl` verification

```sh
curl -s http://<LAN-address>:8787/api/v1/health
curl -s http://<LAN-address>:8787/api/v1/snapshot | jq .
curl -N http://<LAN-address>:8787/api/v1/events   # streams; Ctrl-C to stop
curl -s -o /dev/null -w '%{http_code}\n' http://<LAN-address>:8787/no-such-path   # 404
curl -s -o /dev/null -w '%{http_code}\n' -X POST http://<LAN-address>:8787/api/v1/health  # 405
```

## Future work

- Bonjour/mDNS advertisement (`_pogohome._tcp`) so a client never needs to be
  told the LAN address by hand.
- Authentication (this API has none today -- see "Trust boundary" above).
- The PogoHome iOS app (`pogo-sensor-ios`, a separate repository) connecting
  to this API is its own, later milestone -- this follow-up only implements
  and documents the server side.
