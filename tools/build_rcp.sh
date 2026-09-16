#!/usr/bin/env bash
#
# Convenience wrapper for building rcp/ (the Linux OTBR RCP firmware --
# see docs/otbr-rcp.md). Runs the same set-target/fullclean/build sequence
# documented there. Never hardcodes a serial port or a user-specific build
# path -- both are ordinary arguments/env vars, matching how every other
# project in this repository is built via plain `idf.py -C <project> -B
# <build dir>` invocations (see docs/otbr-rcp.md section 3).
#
# Usage:
#   tools/build_rcp.sh [BUILD_DIR]
#
#   BUILD_DIR   Where build artifacts go (default: ~/esp-builds/pogo_rcp,
#               matching this repository's own convention for module/
#               power_base builds -- see docs/otbr-rcp.md). Never the
#               in-tree rcp/build/ directory, to match how this repository's
#               other projects are normally built out-of-tree.
#
# This script only builds -- it never flashes (no `-p <PORT>`/`flash`
# argument is accepted or assumed). Flash rcp/ yourself once you are ready,
# per docs/otbr-rcp.md section 3 (and section 5's "stop OTBR first" note if
# a board is already running as an OTBR RCP).
#
# Fails loudly, before doing anything else, if the ESP-IDF environment is
# not already sourced in this shell (`source <esp-idf>/export.sh`) -- this
# script does not source it for you, since the exact esp-idf install path
# is a per-machine choice this repository does not hardcode either.
set -euo pipefail

# Checked first, before anything else (including reading $HOME below), so
# this fails with ONE clear, actionable message rather than an unrelated
# "unbound variable"/"command not found" further down.
if ! command -v idf.py > /dev/null 2>&1; then
    echo "error: idf.py not found on PATH -- source your ESP-IDF environment first, e.g.:" >&2
    echo "  source ~/.espressif/v6.1/esp-idf/export.sh" >&2
    exit 1
fi

if [ -z "${IDF_PATH:-}" ]; then
    echo "error: \$IDF_PATH is not set -- source your ESP-IDF environment first (see above)." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${1:-${HOME:-.}/esp-builds/pogo_rcp}"

echo "Building rcp/ (Linux OTBR RCP firmware) for esp32h2 into: $BUILD_DIR"
echo "(never flashes -- see docs/otbr-rcp.md section 3 for the flash command)"
echo

idf.py -C "$REPO_ROOT/rcp" -B "$BUILD_DIR" set-target esp32h2
idf.py -C "$REPO_ROOT/rcp" -B "$BUILD_DIR" fullclean
idf.py -C "$REPO_ROOT/rcp" -B "$BUILD_DIR" build

echo
echo "Build complete: $BUILD_DIR/pogo_rcp.bin"
