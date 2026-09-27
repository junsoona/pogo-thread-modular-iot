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

} // namespace pogohome_api
