// Glue between one already-parsed CoAP request (ingest::CoapMessage),
// components/bridge_export_proto's real unpack functions, and the
// in-memory Registry -- separated from minicoap.hpp/server.hpp so a future
// Matter Bridge (or a unit test) can drive this exact same logic without
// touching sockets at all (see this program's own README/docs/matter-bridge.md
// for why the receiving/validation logic and the stdout smoke
// representation stay decoupled).
#pragma once

#include <functional>
#include <vector>

#include "ingest/minicoap.hpp"
#include "ingest/registry.hpp"

namespace ingest {

// The Power Base management resource "pogo/v1/mgmt"
// (docs/power-base-management.md, section 6). This library only routes it:
// the request payload goes to the handler as it arrived and the handler's
// code and payload go back in the Acknowledgement. Authentication, state and
// persistence are all the handler's (pogohome_api::PowerBaseManagement) --
// nothing about a management message is decided here, and the handler is the
// only thing that can make this server answer with a payload.
struct ManagementReply {
    uint8_t code = kCoapCodeNotFound;
    std::vector<uint8_t> payload;
    // The locate port only (server.hpp): true = send nothing at all. Never
    // set for a request on the ingest port, where every Confirmable request
    // is answered.
    bool silent = false;
};
using ManagementHandler = std::function<ManagementReply(const std::vector<uint8_t> &request)>;
constexpr char kManagementUriPath[] = "pogo/v1/mgmt";

struct Response {
    uint8_t code = kCoapCodeBadRequest;
    std::vector<uint8_t> payload; // empty for every data resource
};

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
// The caller (server.cpp) is responsible for calling this ONLY for a
// Confirmable request whose method is POST -- a Non-confirmable (or
// Acknowledgement/Reset) datagram, or a Confirmable non-POST method, must
// never reach this function at all (never merely "call it but skip the
// ACK"), so the registry is provably untouched by anything but a
// Confirmable POST (correction pass; matches
// components/power_base_transport's own send_coap_response() policy of
// only ever applying state for a request it will also ACK). This function
// itself always finishes registry application before returning, so the
// caller's ACK is only ever sent after that (this follow-up's own explicit
// ordering requirement).
//   4.03 Forbidden -- the sending Power Base was unenrolled from this hub
//                       (Registry::set_power_base_blocked()); nothing applied
uint8_t handle_request(Registry &registry, const CoapMessage &req);

// The same, with the management resource: a request to kManagementUriPath is
// handed to `management` (4.04 when there is none) and never reaches the
// Registry through this function; every other request is handled exactly as
// by handle_request(registry, req) above, with an empty payload.
Response handle_request(Registry &registry, const CoapMessage &req, const ManagementHandler &management);

} // namespace ingest
