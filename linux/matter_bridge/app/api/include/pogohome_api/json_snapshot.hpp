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

// GET /api/v1/health's body.
std::string BuildHealthJson(uint64_t registry_generation, size_t module_count);

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

} // namespace pogohome_api
