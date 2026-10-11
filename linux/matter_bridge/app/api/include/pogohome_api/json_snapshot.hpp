// Pure ingest::NodeState -> PogoHome Local API v1 JSON serialization --
// zero sockets, zero connectedhomeip dependency, so this is directly
// host-testable and reusable from both GET /api/v1/snapshot and the SSE
// `event: snapshot` payload (see http_server.hpp), which are byte-for-byte
// the same JSON shape (see docs/pogohome-api-v1.md).
//
// Every function here takes an already-copied std::vector<ingest::NodeState>
// (the caller's own ingest::Registry::snapshot() result) -- NEVER a Registry
// reference -- so it is impossible for this file to accidentally serialize
// while holding Registry::mutex_ (see registry.hpp's own top comment: never
// do JSON generation while holding that lock).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ingest/registry.hpp"

namespace pogohome_api {

constexpr int kSchemaVersion = 1;

// GET /api/v1/health's body. The three-argument form (what http_server.cpp
// serves) adds `power_base_count`; the two-argument form is the original
// shape, kept for existing callers.
std::string BuildHealthJson(uint64_t registry_generation, size_t module_count);
std::string BuildHealthJson(uint64_t registry_generation, size_t module_count, size_t power_base_count);
// The Bonjour revision appends `hub_id` (see hub_identity.hpp) after every
// earlier field, so a client can check that the hub answering at an address
// is the one it discovered or remembered. An empty `hub_id` leaves the
// three-argument shape unchanged.
std::string BuildHealthJson(uint64_t registry_generation, size_t module_count, size_t power_base_count,
                            const std::string &hub_id);

// Exactly one entry per module_uuid, in first-appearance order. The Registry
// keys nodes by (power_base_uuid, module_uuid), so a module moved between
// Power Bases is briefly present under both until the old Base reports it
// offline. The owner is the online entry with the largest attach_order (the
// most recent physical attach); with no online entry, the entry with the
// largest attach_order (ties: the first seen). With a single Power Base
// every module has one entry and this returns `nodes` unchanged (minus
// has_topology == false entries).
std::vector<ingest::NodeState> ResolveModuleOwners(const std::vector<ingest::NodeState> &nodes);

// GET /api/v1/snapshot's body, and the SSE `event: snapshot` `data:` payload
// (see http_server.cpp) -- identical shape either way. `nodes` is rendered
// in its given order (the caller's snapshot() order); a NodeState with
// has_topology == false is skipped entirely (see registry.hpp: every node
// apply_topology() ever creates has has_topology == true immediately, so
// this should never actually happen for a real Registry -- defensive only,
// never fabricates topology fields for a node this API cannot honestly
// describe).
std::string BuildSnapshotJson(const std::vector<ingest::NodeState> &nodes, uint64_t registry_generation,
                               int64_t generated_at_unix_ms);

// The full v1 snapshot (what http_server.cpp serves): modules pass through
// ResolveModuleOwners() and carry power_base_uuid/parent_uuid, and a
// top-level `power_bases` array lists every Power Base in `view` plus any
// Base a module names that the Registry is not tracking (see
// ingest::Registry::kMaxPowerBases) -- that derived entry is online exactly
// when one of its modules is, since an online module means this host
// accepted a message from that Base within the stale timeout. The nodes-only
// overload above is this with no tracked Power Bases.
std::string BuildSnapshotJson(const ingest::RegistryView &view, uint64_t registry_generation,
                               int64_t generated_at_unix_ms);

// What Power Base management adds to the snapshot
// (docs/power-base-management.md), when the hub offers it.
struct SnapshotManagement {
    // EUI-64s (16 lowercase hex) this hub holds an ACTIVE management
    // credential for: the Bases that can be unenrolled remotely.
    std::vector<std::string> managed_power_bases;
    // Those of them whose board announced that it sends its release receipt
    // itself: a removal of such a Base needs no confirmation by hand.
    std::vector<std::string> self_confirming_power_bases;
    // The removal jobs, as a serialized JSON array ("[...]").
    std::string removals_json = "[]";
    // The hub can take an offline Power Base out of its list
    // (docs/power-base-management.md, section 13). False: the snapshot is
    // byte for byte the one of a hub without that.
    bool forget_supported = false;
    // With it: (EUI-64, record) of the listed Bases -- the registration a
    // request to take one out of the list must name. A Base without an entry
    // here gets no "record".
    std::vector<std::pair<std::string, std::string>> records;
    // (EUI-64, removal serial) of the managed Bases: how many removals were
    // asked of the listed registration so far. A request for a removal names
    // the value it was confirmed against (docs/power-base-management.md,
    // section 16); a managed Base with an entry here gets "removal_serial".
    std::vector<std::pair<std::string, uint32_t>> removal_serials;
    // And the list removals, as a serialized JSON array ("[...]").
    std::string forgets_json = "[]";
    // The Power Bases that are out of the list now (their registration ended
    // and they were not registered again), as a serialized JSON array. Given
    // with `forget_supported`; none of them is among the listed Bases.
    std::string departures_json = "[]";
};

// The snapshot above with management: every `power_bases` entry gets
// `"management": "supported" | "unsupported"`, and a supported one also
// `"erase_confirmation": "automatic" | "manual"` (how the erase of a removal
// of that Base would be confirmed); a managed Base the Registry
// does not list (it has been silent since the bridge started) is added as an
// offline entry without modules, so that it can still be selected and
// unenrolled; and a top-level `power_base_removals` array follows `modules`.
// Additive only: with `management == nullptr` the output is byte for byte
// the overload above. With `forget_supported` a listed Base also carries
// `"record"` (when the hub has one for it) and a top-level
// `power_base_forgets` array follows `power_base_removals`, and a top-level
// `power_base_departures` array follows that. A managed Base named in
// `removal_serials` carries `"removal_serial"` after `"record"`.
std::string BuildSnapshotJson(const ingest::RegistryView &view, uint64_t registry_generation,
                               int64_t generated_at_unix_ms, const SnapshotManagement *management);

} // namespace pogohome_api
