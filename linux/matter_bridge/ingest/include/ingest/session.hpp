// Glue between one already-parsed CoAP request (ingest::CoapMessage),
// components/bridge_export_proto's real unpack functions, and the
// in-memory Registry -- separated from minicoap.hpp/server.hpp so a future
// Matter Bridge (or a unit test) can drive this exact same logic without
// touching sockets at all (see this program's own README/docs/matter-bridge.md
// for why the receiving/validation logic and the stdout smoke
// representation stay decoupled).
#pragma once

#include "ingest/minicoap.hpp"
#include "ingest/registry.hpp"

namespace ingest {

// Applies one already-envelope-and-payload-validated bridge_export_proto
// message (matched against `req.uri_path`) to `registry`, prints one smoke
// stdout summary line (Power Base UUID suffix, module UUID suffix,
// online/depth/role, sample sequence, channel type/value/scale/status,
// duplicate-or-not -- never Dataset/key material, there is none to leak
// here in the first place), and returns the CoAP response code to ACK with:
//   2.04 Changed  -- a genuinely new state was applied
//   2.03 Valid    -- an exact-duplicate retransmit (already applied, no change)
//   4.00 Bad Request -- malformed envelope/payload, or a msg_type that does
//                       not match the resource it was posted to
//   4.04 Not Found -- a SAMPLE from a (power_base_uuid, module_uuid) this
//                       registry has no topology at all for yet (correction
//                       pass -- see registry.hpp's own top comment)
//   4.12 Precondition Failed -- a SAMPLE from a known-but-OFFLINE uuid, a
//                       sensor_bitmap that is not a subset of (or a channel
//                       that contradicts) the topology's own declared
//                       bitmap, OR a TOPOLOGY_SNAPSHOT whose
//                       generation/sequence is stale/reordered (correction
//                       pass -- mirrors components/power_base_transport's
//                       own CoAP code table exactly)
//   5.03 Service Unavailable -- a brand-new (power_base_uuid, module_uuid)
//                       pair, but the bounded registry is full
// The caller (server.cpp) is responsible for only calling this AFTER
// deciding the request is otherwise well-formed CoAP, and for only sending
// an ACK for a Confirmable request (matching
// components/power_base_transport's own send_coap_response() policy) --
// this function itself always finishes registry application before
// returning, so the caller's ACK is only ever sent after that (this
// follow-up's own explicit ordering requirement).
uint8_t handle_request(Registry &registry, const CoapMessage &req);

} // namespace ingest
