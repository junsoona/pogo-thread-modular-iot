#!/usr/bin/env bash
#
# Runs every automated test for the Power Base -> Linux OTBR Matter bridge
# that does NOT require a connectedhomeip checkout -- see
# docs/matter-bridge.md for what this does and does not cover.
#
# Covers (see each test file's own top comment for exactly which items):
#   - linux/matter_bridge/ingest: the existing CoAP ingest unit/loopback
#     tests PLUS the new Registry snapshot/generation/wait_for_change tests
#     this follow-up added -- regression coverage for the read-model the
#     Matter side depends on.
#   - linux/matter_bridge/app/core: SHT45/PMS5003 unit conversion, non-OK
#     status -> null, online/offline -> Reachable, duplicate-sample
#     attribute-update dedup, detach/reattach endpoint persistence,
#     distinct-uuid distinct-endpoint, restart persistence, and the
#     two-Power-Base-online-at-once conflict -- all against the exact
#     production mapping/persistence/reconciliation code, see
#     linux/matter_bridge/app/core/tests/*.
#
# Does NOT cover (needs a real connectedhomeip v1.6.0.0 build + chip-tool --
# see tools/build_matter_bridge.sh and docs/matter-bridge.md's own
# "chip-tool verification" section for that manual procedure):
#   - Actually creating/reading a Matter dynamic endpoint through the real
#     SDK.
#   - Commissioning, Descriptor/Bridged-Device-Basic-Information/measurement
#     cluster reads via chip-tool.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

run_cmake_project() {
    local project_dir="$1"
    local build_dir="$project_dir/build"
    echo "==> Building and testing: $project_dir"
    cmake -S "$project_dir" -B "$build_dir" >/dev/null
    cmake --build "$build_dir"
    ctest --test-dir "$build_dir" --output-on-failure
}

run_cmake_project "$REPO_ROOT/linux/matter_bridge/ingest"
run_cmake_project "$REPO_ROOT/linux/matter_bridge/app/core"

echo
echo "==> All CHIP_ROOT-independent tests passed."
echo "    Remaining verification (needs CHIP_ROOT + chip-tool, see docs/matter-bridge.md):"
echo "      1. tools/build_matter_bridge.sh"
echo "      2. Run pogo_matter_bridge on the actual Linux OTBR host"
echo "      3. Commission and read attributes with chip-tool"
