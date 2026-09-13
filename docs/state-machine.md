# State machines

## pogo_port (per instance -- UPSTREAM and DOWNSTREAM are fully independent)

```
DISABLED --enable()--> WAIT_ATTACH
WAIT_ATTACH --[DETECT_IN debounced Low, or no DETECT hw + just enabled]--> HANDSHAKE
HANDSHAKE --[send HELLO_REQ; matching HELLO_RESP arrives, valid, no BASE_ID conflict]--> PEER_READY
HANDSHAKE --[retries exhausted, BASE_ID says POWER_BASE]--> PEER_READY (peer_type=POWER_BASE)
HANDSHAKE --[retries exhausted, BASE_ID doesn't explain it, AUTO mode]--> ERROR (DOCK_TIMEOUT)
HANDSHAKE --[retries exhausted, BASE_ID doesn't explain it, dev compat mode]--> ERROR (DOCK_TIMEOUT), then auto-retry after CONFIG_POGO_PING_INTERVAL_MS
HANDSHAKE --[HELLO_REQ/RESP peer_type invalid, or conflicts with BASE_ID]--> ERROR (PEER_TYPE_CONFLICT)
HANDSHAKE --[protocol_version mismatch]--> ERROR (PROTOCOL_VERSION_ERROR)
(HANDSHAKE or PEER_READY) --[HELLO_REQ received from peer]--> answer with HELLO_RESP (same seq), own state unchanged
PEER_READY --[FRAME_RECEIVED, not a hello]--> (event forwarded up, state unchanged)
(any of HANDSHAKE/PEER_READY/ERROR) --[DETECT_IN debounced High]--> WAIT_ATTACH
```

30ms debounce (configurable) is applied uniformly to both attach and detach
edges, in the port's own task -- never in the GPIO ISR, which only pushes
`{tick, level}` into a queue.

### PEER_HELLO_REQ / PEER_HELLO_RESP is an explicit request/response, not a symmetric exchange

Earlier revisions used one `PEER_HELLO` message both sides had to send with
a matching sequence, which only worked if both sides happened to be in lock
step. It is now two distinct message types (`POGO_MSG_PEER_HELLO_REQ`,
`POGO_MSG_PEER_HELLO_RESP`; `upper`/`lower` never used the old type, so this
is purely additive to the regression baseline):

- Whichever side attaches/boots first is the initiator: it sends
  `HELLO_REQ` (retrying up to `CONFIG_POGO_MAX_RETRY` times) and only
  reaches `PEER_READY` once a same-seq `HELLO_RESP` arrives.
- A `HELLO_REQ` is answered with `HELLO_RESP` **in any state** -- during our
  own `HANDSHAKE` (simultaneous or staggered boot: whichever side's `REQ`
  arrives first gets answered immediately, regardless of the receiver's own
  retry count) and even once already `PEER_READY` (the peer rebooted while
  still docked and is starting its handshake over from `HELLO_REQ`;
  answering does not reset our own `PEER_READY`).
- Both message types carry the same payload (`protocol_version`,
  `peer_type`) and go through the same validation
  (`validate_incoming_hello()` in `pogo_port.c`): `protocol_version` must
  equal ours (else `PROTOCOL_VERSION_ERROR`), `peer_type` must be `MODULE`
  or `POWER_BASE` (else `PEER_TYPE_CONFLICT` -- a stray/garbage value is not
  treated as "no opinion"), and if BASE_ID hardware is present its reading
  must agree with the declared `peer_type`.

### Dev compat mode: no permanent ERROR from a handshake timeout

The current two-UART development boards have no `DETECT_IN` wiring
(`module_port_config_t.detect_in_gpio == -1`), so a port can never be told
"the peer physically left" -- it is simply always attached once enabled.
Without dedicated recovery, one failed handshake cycle (e.g. the peer board
hadn't finished booting yet) would leave the port in `ERROR` forever, since
`AUTO` mode's policy of "wait for the next DETACHED" has no DETACHED to wait
for. `handle_dock_timeout_or_silent_power_base()` in `pogo_port.c` handles
this: after logging the `DOCK_TIMEOUT` and firing `POGO_PORT_EVENT_ERROR`
(so the failure is still visible), if `detect_in_gpio < 0` it waits
`CONFIG_POGO_PING_INTERVAL_MS` (a bounded cooldown, not a hot loop) and
starts a fresh handshake cycle -- itself capped at `CONFIG_POGO_MAX_RETRY`
retries, same as any other cycle. `AUTO` mode (real `DETECT_IN`) is
unaffected: it keeps waiting for a physical detach/attach, since a
`DOCK_TIMEOUT` there means something is actually different (a non-answering
peer while the wire says something is connected).

## role_manager (AUTO dev_mode; forced modes just report ROOT/LEAF once)

Recomputed on every `ATTACHED`/`DETACHED`/`PEER_READY`/`ERROR` event from
either port (see the determination table in pogo-interface.md):

```
UNASSIGNED  <-> ROOT            (UPSTREAM peer becomes/stops being POWER_BASE)
UNASSIGNED  <-> LEAF            (UPSTREAM peer is MODULE, DOWNSTREAM empty)
LEAF        <-> INTERMEDIATE    (a MODULE docks/undocks DOWNSTREAM while
                                  UPSTREAM stays MODULE)
(any)       ->  ROLE_ERROR      (either port reports ERROR)
```

A DOWNSTREAM module docking before this module's own UPSTREAM role/dataset
are settled does **not** immediately start provisioning: `parent_service`
checks `role_manager_is_upstream_ready()` (set by `module/main/app_main.c`
once ROOT reaches Leader/Router, or by `child_service` once
INTERMEDIATE/LEAF successfully joins) before running a session, and keeps
re-checking every ~200ms until it's true.

## thread_manager role application

```
(role change) -> thread_manager_stop() [disable Thread+IP6, idempotent]
             -> set link mode + router eligibility for the NEW role
             -> ROOT:          resume commissioned dataset, or form new
                                (esp_openthread_auto_start(NULL))
                INTERMEDIATE/LEAF: esp_openthread_auto_start(&received_tlvs)
```

`INTERMEDIATE` and `LEAF` share the same "join with a dataset we didn't
generate" code path; only the link mode differs (full FTD + router-eligible
for INTERMEDIATE; `mRxOnWhenIdle=false`/`mDeviceType=false`/
`mNetworkData=false`/router-eligible=false + a poll period for LEAF).
`child_service` also re-invokes this (same cached dataset, no new
handshake) if `role_manager`'s role flips between LEAF and INTERMEDIATE
*after* a successful join -- e.g. a new module docks DOWNSTREAM later.

## child_service (drives UPSTREAM; strict forward sequence, each message type accepted in exactly one state)

```
WAIT_PEER -> READY (PEER_READY)
READY -> IDENTIFIED (IDENTIFY_REQ answered; a later re-IDENTIFY is answered
                      again without changing state -- harmless, no side effect)
IDENTIFIED -> UUID_ASSIGNED (ASSIGN_UUID)
UUID_ASSIGNED -> SLOT_ASSIGNED (ASSIGN_SLOT)
SLOT_ASSIGNED -> SENSOR_DONE (SENSOR_HELLO -> sensor_provider measurement -> SENSOR_INFO_ACK)
SENSOR_DONE -> POLL_SET (POLL_CONFIG range-checked, then acked)
POLL_SET -> JOINING (SET_DATASET validated via otDatasetIsValid(), durably
                      saved via save_dynamic_record(), acked, THEN
                      thread_manager_join_with_dataset() called)
JOINING -> JOINED (thread_manager_poll_role_change() reports CHILD/ROUTER/LEADER
                    before CONFIG_POGO_JOIN_TIMEOUT_MS elapses) -- sends JOIN_RESULT
JOINING -> ERROR (join timeout) -- sends JOIN_RESULT(success=false)
(any) -> WAIT_PEER on DETACHED (session/dedup state reset only -- see
          "UPSTREAM/DOWNSTREAM detach: what each layer does" below for what
          happens to Thread itself, which child_service does not decide)
```

Any message received out of its exact required state (e.g. `ASSIGN_SLOT`
before `UUID_ASSIGNED`, or `SET_DATASET` before `POLL_SET`) is rejected with
`POGO_NACK_BAD_STATE` -- see `handle_assign_uuid`/`handle_assign_slot`/
`handle_sensor_hello`/`handle_poll_config`/`handle_set_dataset` in
`child_service.c`. This is independent of, and checked before, the
retransmit-dedup logic below.

Every write-type message (`ASSIGN_UUID`/`ASSIGN_SLOT`/`SENSOR_HELLO`/
`POLL_CONFIG`/`SET_DATASET`) is deduplicated by `(type, seq)`: a retransmit
of the exact same request gets the *cached* response resent, never
re-applied (no double UUID assignment, no double NVS write, no double
sensor measurement) -- checked first, ahead of the state-order check above,
so a legitimate retransmit is never rejected as "wrong state" just because
the state already advanced past it.

`SET_DATASET`'s ACK is gated on `save_dynamic_record()` actually succeeding
(`nvs_open`/every `nvs_set_*`/`nvs_commit` all checked): if the durable save
fails, child_service sends `NACK(POGO_NACK_STORAGE_FAILED)` instead of ACK,
discards the staged dataset (`s_dataset_len = 0`), stays in `POLL_SET`
(never advances to `JOINING`), and `thread_manager_join_with_dataset()` is
never called. The Dataset bytes and Network Key are never logged, only the
fact that storage failed.

## parent_service (drives DOWNSTREAM; linear sequence per docking)

```
1. IDENTIFY_REQ -> IDENTIFY_RESP
2. ASSIGN_UUID (random 8 bytes) -> ACK/NACK
3. ASSIGN_SLOT (slot_id=0, topology_depth=own_depth+1) -> ACK/NACK
4. SENSOR_HELLO -> SENSOR_INFO_ACK (response_time_ms measured by the CHILD's
                    own sensor_provider) or NACK(SENSOR_ERROR)
5. compute polling_period_ms = f(response_time_ms)  (see below)
   POLL_CONFIG -> ACK/NACK(POLL_CONFIG_RANGE)
6. SET_DATASET (this module's own Active Dataset TLVs) -> ACK/NACK
7. wait for (unsolicited) JOIN_RESULT, up to CONFIG_POGO_JOIN_TIMEOUT_MS
```

Any step's `send_and_wait()` retries up to `CONFIG_POGO_MAX_RETRY` times
*with the same seq* (so the child's dedup table recognizes a retransmit).
A `DETECT_IN`-debounced `DETACHED` or a `pogo_port` `ERROR` aborts the
in-flight session immediately (`s_session_aborted`), regardless of which
step it's in. If the whole sequence fails while the peer is still
physically attached, it is retried after `CONFIG_POGO_PING_INTERVAL_MS`.

### Polling period formula (integer-only, boundary-tested)

| response_time_ms          | multiplier | note                          |
|----------------------------|-----------:|--------------------------------|
| `== 0`                     | (error)    | treated as a measurement error |
| `< 50`                     | 15         |                                 |
| `50 <= RT < 100`           | 10         |                                 |
| `100 <= RT < 500`          | 8          |                                 |
| `500 <= RT <= 1000`        | 3          |                                 |
| `1000 < RT <= 10000`       | 1.5        | computed as `RT * 3 / 2`        |
| `RT > 10000`               | 1          |                                 |

Implemented in `parent_service_calc_polling_period_ms()`
(`components/parent_service/parent_service.c`), with a 64-bit intermediate
to guard against `uint32_t` overflow before the final cast. Boundary values
`0, 49, 50, 99, 100, 499, 500, 1000, 1001, 10000, 10001` and the mock
sensor's documented `120ms -> 960ms` are all checked in
`tools/test_polling_calc.c` (a host-compiled copy of the algorithm -- see
that file's header comment for why, and the final report for the run
output).

### Node record & backoff

Per attached child, `parent_service` keeps `uuid`, `mac`, `sensor_type_bitmap`,
`response_time_ms`, `polling_period_ms`, `backoff_delay_ms`, `retry_count`,
`topology_depth`, `child_role`, and `last_seen`. `backoff_delay_ms` is the
low 8 bits of an FNV-1a hash of the UUID (always in `[0, 255]`) --
`parent_service_calc_backoff_ms()`. Actual use of this value for sensor-data
retransmission scheduling is future work (explicitly out of scope for this
refactor); only the field and its calculation are implemented now.

## Error handling (section 16)

| Error                    | Where it's raised                              | Recovery path |
|---------------------------|------------------------------------------------|----------------|
| `CONFIG_ERROR`             | `module_config_load()`/`validate_config()` (unsupported `driver_id`/`bus_type`, `measurement_timeout_ms==0`, bad/duplicate/missing GPIOs, `detect_out_active_low=false`, mock driver with `dev_mode=AUTO`, ...) | halt at boot (no sensor/pairing/Thread) |
| `DOCK_TIMEOUT`             | `pogo_port` (no HELLO_RESP reply, BASE_ID doesn't explain it) | AUTO: port -> ERROR until next DETACHED. Dev compat mode (no DETECT_IN): port -> ERROR, logged, then auto-retries a fresh handshake cycle after `CONFIG_POGO_PING_INTERVAL_MS` |
| `PEER_TYPE_CONFLICT`       | `pogo_port` (BASE_ID vs. declared peer_type disagree, or peer_type is neither MODULE nor POWER_BASE) | port -> ERROR until next DETACHED (both AUTO and dev compat mode -- this is a real config/compatibility problem, not a timing issue, so it is not auto-retried) |
| `PROTOCOL_VERSION_ERROR`   | `pogo_port` (HELLO_REQ/RESP version mismatch)   | port -> ERROR until next DETACHED (not auto-retried, same reasoning as above) |
| `UART_TIMEOUT` / `FRAME_CRC_ERROR` | `pogo_frame_parser_feed()` (silent resync, see pogo_frame.c) | parser resyncs on the next valid SOF; the request/response layer above times out and retries |
| `SENSOR_ERROR`             | `child_service` (sensor_provider start/read failed), or `parent_service` (child reports `sensor_status != 0`) | NACK to parent (child side) / abandon session (parent side); no driver fallback |
| `POLL_CONFIG_ERROR`        | `child_service` (polling_period_ms out of range) | NACK; parent abandons this session |
| `DATASET_ERROR`            | `child_service`/`parent_service` (`otDatasetIsValid()` false, own dataset unreadable, or malformed IDENTIFY_RESP/SENSOR_INFO_ACK) | NACK / abandon session |
| storage failure (`POGO_NACK_STORAGE_FAILED`) | `child_service` (`save_dynamic_record()`: `nvs_open`/any `nvs_set_*`/`nvs_commit` failed) | NACK instead of ACK for `SET_DATASET`; stays in `POLL_SET`; Thread join never started |
| `THREAD_JOIN_TIMEOUT`      | `child_service` (no role change before `CONFIG_POGO_JOIN_TIMEOUT_MS`) | JOIN_RESULT(false); child_service -> ERROR |
| `ROLE_TRANSITION_ERROR`    | `module/main` (`thread_manager_start_as_root()`/`thread_manager_stop()` fails, or ROOT never reaches Leader/Router within 30s) | logged; role_manager stays whatever it was, no Thread activity assumed |

Retries: `CONFIG_POGO_MAX_RETRY` (default 3) per request; ACK timeout
`CONFIG_POGO_ACK_TIMEOUT_MS` (300ms dev profile; 100ms is the *target*
production value called out in section 16 but not yet changed here, since
it hasn't been validated against real UART timing on final hardware);
`CONFIG_POGO_JOIN_TIMEOUT_MS` (20s) is separate and only applies to the
Thread join wait.

## UPSTREAM/DOWNSTREAM detach: what each layer does

- **UPSTREAM detach stops this module's own Thread membership.**
  `child_service` itself only invalidates the pairing session (dedup state,
  uuid/slot/dataset staged in RAM) -- it does not call `thread_manager`.
  But the detach also drives `role_manager` to `UNASSIGNED`, and
  `module/main/app_main.c`'s `on_role_change()` handler calls
  `thread_manager_stop()` on every transition into `UNASSIGNED`/`ROLE_ERROR`,
  which covers this case (an UPSTREAM detach is the only way a previously
  valid role reaches `UNASSIGNED`). This keeps the physical (POGO) and
  Thread layers in the same state -- module_config and the sensor_provider
  selection are untouched, and rejoining always requires a fresh POGO
  handshake through a new `SET_DATASET`, never an automatic resume of a
  stale membership. (An earlier revision of this refactor left Thread
  running after an UPSTREAM undock, reasoning that the wireless mesh link
  is independent of the wired provisioning link; that turned out to leave
  the physical and logical layers out of sync and has been corrected.)
- **DOWNSTREAM detach does not touch this module's own Thread membership.**
  `parent_service` clears its child node record and cancels any in-flight
  provisioning session (`s_node` reset, `s_peer_currently_ready`/
  `s_peer_ready_pending` cleared -- see `parent_service_task()`'s
  `POGO_PORT_EVENT_DETACHED` case), but nothing calls `thread_manager` as a
  result: this module's own Thread role does not depend on whether anything
  is docked below it. A `role_manager` recompute after a DOWNSTREAM detach
  can still change INTERMEDIATE -> LEAF, which `child_service_on_role_change()`
  handles by re-applying link mode with the *same* dataset (no new
  handshake, no Thread stop/restart cycle -- see "thread_manager role
  application" above).
- **A stale ROOT-readiness watcher cannot mark upstream_ready=true after
  this module has left ROOT.** `root_ready_watcher_task()` is launched with
  a generation number that `on_role_change()` bumps every time ROOT is left
  (or a new watcher is started); the watcher checks its generation before
  acting and self-deletes within one 200ms poll if superseded. At most one
  watcher's result ever takes effect.

## Known simplifications (be explicit about these; do not read them as bugs)

- **No auto-rejoin from stored dynamic NVS on boot.** Section 14 requires
  that a changed upstream parent not be trusted silently; rather than add an
  explicit "parent identity" field to the wire protocol just for this
  refactor, `child_service` always waits for a fresh, live handshake
  (through `SET_DATASET`) before ever calling
  `thread_manager_join_with_dataset()`. The dynamic NVS record it writes is
  a diagnostic/forward-compatibility artifact for now, not read back at
  boot. (The old `lower` reference project's boot-time auto-restore is
  unaffected -- it is a different, frozen codebase.)
- **SENSOR_HELLO handling blocks child_service's task for up to
  `measurement_timeout_ms`.** A DETACH during that window is only noticed
  once the measurement call returns. Acceptable for this MVP's timeout
  scale; a future revision could poll the sensor in smaller increments
  interleaved with port-event checks if that matters for a much longer
  `measurement_timeout_ms`.
- **Periodic sensor-data reporting is not implemented.** `polling_period_ms`
  is computed, transmitted, validated, and stored (and, for a LEAF, actually
  applied to `otLinkSetPollPeriod()`), but no code yet wakes up every
  `polling_period_ms` to read the sensor and report a value upstream --
  explicitly future work per section 12/13 of the implementation spec.
