# Power Base -> Linux OTBR bridge ingest

Receives `bridge_export_proto`-encoded SAMPLE/TOPOLOGY_SNAPSHOT/HEARTBEAT
messages from `components/linux_bridge_exporter` over CoAP/UDP and
maintains a bounded, UUID-keyed registry (`ingest::Registry`). Not the
Matter Bridge itself -- see `docs/matter-bridge.md` for that plan.

## Build

```
cd linux/matter_bridge/ingest
cmake -S . -B build
cmake --build build
```

## Run

```
./build/ingest_server --interface wpan0 [--port 5684]
```

`--interface` is **required**. See "Security model" below for why.

## Test

```
ctest --test-dir build --output-on-failure
```

Runs `ingest_unit_tests` (pure registry/session logic, no sockets) and
`ingest_loopback_test` (a real UDP/IPv6 socket test against the machine's
actual loopback interface, resolved via `if_nametoindex("lo")` /
`if_nametoindex("lo0")` -- never a hardcoded name, since it differs between
Linux and macOS).

## Security model

Two things this program deliberately does **not** provide, and does not
pretend to:

- **The CRC in `bridge_export_proto` is an error-detection check, not
  authentication.** It lets a receiver notice accidental corruption in
  transit; it proves nothing about who actually sent a datagram. Anyone who
  can reach this program's UDP port and knows the wire format can construct
  a message with a valid CRC.
- **This program has no cryptographic authentication or Power Base
  identity verification of its own.** It trusts every well-formed message
  it receives on its configured interface.

Because of this, **this program must only ever be reachable from inside the
Thread network's own security boundary** -- Thread's own link-layer/network-
layer encryption and the Linux OTBR's border-router role are what actually
restrict which devices can originate traffic that reaches this process at
all. This program is not a substitute for that boundary and must never be
exposed to an untrusted network (the public internet, an unrelated LAN
segment, ...).

`--interface` (`ServerConfig::interface_name`, enforced per-datagram via
`recvmsg()` + `IPV6_RECVPKTINFO`, not just at `bind()` time) is a
defense-in-depth measure on top of that boundary: it stops a process
running on the *same* Linux host, or reachable via some *other* local
interface, from impersonating a Power Base by sending traffic over a
different path. It does not replace the Thread-side boundary above.

A future revision may add a Power Base UUID allowlist once an operator has
a specific, known set of Power Base UUIDs to pin (see `ServerConfig`'s own
header comment) -- this is **deliberately not implemented** as
trust-on-first-use (TOFU) or any other automatic scheme in this pass; the
configuration surface is only shaped to make that addition possible later
without a redesign.

## Response codes

| Code | Meaning |
|------|---------|
| 2.04 Changed | a genuinely new state was applied |
| 2.03 Valid | an exact-duplicate retransmit (already applied, unchanged) |
| 4.00 Bad Request | malformed envelope/payload, or a msg_type/resource mismatch |
| 4.02 Bad Option | an unrecognized CoAP *critical* option (RFC 7252 5.4.1) |
| 4.04 Not Found | a SAMPLE for a `(power_base_uuid, module_uuid)` with no topology at all yet |
| 4.05 Method Not Allowed | anything other than a Confirmable POST |
| 4.12 Precondition Failed | a SAMPLE for a known-but-offline uuid or a bitmap mismatch, or a stale/reordered TOPOLOGY_SNAPSHOT (generation/sequence ordering, see `registry.hpp`) |
| 5.03 Service Unavailable | a brand-new uuid once the bounded registry is full |
