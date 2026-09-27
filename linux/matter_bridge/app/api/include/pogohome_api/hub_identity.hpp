// The hub's own stable identifier (`hub_id`) -- what a PogoHome client uses
// to recognize THIS Linux sensor hub again after its IP address, API port,
// Bonjour service name or process lifetime changes (see
// docs/pogohome-api-v1.md "hub_id" and mdns_advertiser.hpp). It is never
// derived from an address, a hostname or the advertised service name: it is
// 128 random bits, created once and then kept in the bridge's existing
// --state-dir next to endpoint_map.bin.
//
// Same "never silently reassign an identity" policy as
// matter_bridge::EndpointStore: a hub_id file that exists but is unreadable
// or malformed is an error the operator must look at, never replaced by a
// fresh id (a client that remembered the old id would otherwise quietly stop
// recognizing this hub, or -- worse -- a copied state directory would give
// two hubs one id without anyone noticing a change).
#pragma once

#include <string>

namespace pogohome_api {

// File name inside --state-dir.
constexpr const char kHubIdFileName[] = "hub_id";
// 16 random bytes as lowercase hex.
constexpr size_t kHubIdHexLength = 32;

// Exactly kHubIdHexLength lowercase hex digits.
bool IsValidHubId(const std::string &hub_id);

enum class HubIdLoadResult {
    Loaded,  // an existing, valid hub_id file was read
    Created, // there was no file; a new id was generated and durably written
    Error,   // `error` says why; `hub_id` is left empty
};

// Reads <state_dir>/hub_id, or creates it when it does not exist yet. The new
// file is written to a temporary name, fsync'd, and linked into place without
// replacing anything (link(2) fails if another process created the file in
// the meantime, in which case that file is read instead), so an interrupted
// write never leaves a half-written id behind and two starts can never end up
// with two different ids.
HubIdLoadResult LoadOrCreateHubId(const std::string &state_dir, std::string &hub_id, std::string &error);

} // namespace pogohome_api
