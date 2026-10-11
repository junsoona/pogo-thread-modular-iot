#!/usr/bin/env bash
#
# Builds pogo_matter_bridge (linux/matter_bridge/app) against a pinned
# connectedhomeip v1.6.0.0 checkout -- see docs/matter-bridge.md.
#
# This bridge is only ever built/tested against connectedhomeip's EXACT
# v1.6.0.0 tag -- this script verifies that before touching anything, and
# refuses to proceed against any other revision (a floating branch, a
# different tag, an unpinned "main" checkout, ...).
#
# Usage:
#   CHIP_ROOT=/path/to/connectedhomeip tools/build_matter_bridge.sh [out_dir] [-- extra gn args...]
#
#   out_dir   Build output directory. Defaults to a directory OUTSIDE this
#             git working tree (${TMPDIR:-/tmp}/pogo_matter_bridge_build) --
#             this repo's own .gitignore does not need to (and should not
#             have to) know about Matter build artifacts.
#
# Requires:
#   - CHIP_ROOT pointing at a connectedhomeip checkout at exactly tag
#     v1.6.0.0.
#   - That checkout's own Matter build environment already bootstrapped
#     once (`cd "$CHIP_ROOT" && scripts/bootstrap.sh`, or
#     `scripts/checkout_submodules.py --shallow --platform linux` followed
#     by bootstrap.sh on a from-scratch Linux checkout) -- this script
#     sources "$CHIP_ROOT/.environment/activate.sh" to pick up the exact
#     gn/ninja/zap-cli versions that bootstrap installed, falling back to
#     whatever's already on PATH if that file doesn't exist.
#   - On the actual Linux target host: pkg-config + libssl-dev (OpenSSL,
#     this SDK's default crypto backend) and libglib2.0-dev (gdbus-codegen,
#     needed by connectedhomeip's own OpenThread D-Bus integration) --
#     e.g. `sudo apt install pkg-config libssl-dev libglib2.0-dev`.
#
# This bridge disables connectedhomeip's own WiFi/Ethernet network-
# commissioning drivers (chip_enable_wifi=false, chip_enable_ethernet=false)
# and Java/JNI compilation (matter_enable_java_compilation=false) -- this
# bridge does not manage the Linux host's own network provisioning (that is
# already handled by the OS and the existing Thread/OTBR setup this bridge
# only ever reads from, see docs/power-base.md) and never needs a JVM. Pass
# additional/overriding `gn` args after `--` if a deployment needs them.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
APP_DIR="$REPO_ROOT/linux/matter_bridge/app"

REQUIRED_TAG="v1.6.0.0"

if [ -z "${CHIP_ROOT:-}" ]; then
    echo "error: CHIP_ROOT is not set." >&2
    echo "       Point it at a connectedhomeip checkout at exactly tag $REQUIRED_TAG, e.g.:" >&2
    echo "         git clone --branch $REQUIRED_TAG --depth 1 https://github.com/project-chip/connectedhomeip.git" >&2
    exit 1
fi
if [ ! -d "$CHIP_ROOT" ]; then
    echo "error: CHIP_ROOT ($CHIP_ROOT) does not exist" >&2
    exit 1
fi
if [ ! -d "$CHIP_ROOT/.git" ]; then
    echo "error: CHIP_ROOT ($CHIP_ROOT) does not look like a git checkout (no .git)" >&2
    exit 1
fi

ACTUAL_REF="$(git -C "$CHIP_ROOT" describe --tags --exact-match 2>/dev/null || true)"
if [ "$ACTUAL_REF" != "$REQUIRED_TAG" ]; then
    echo "error: CHIP_ROOT is not checked out at exactly tag $REQUIRED_TAG." >&2
    if [ -n "$ACTUAL_REF" ]; then
        echo "       Currently at: $ACTUAL_REF" >&2
    else
        echo "       Currently at: $(git -C "$CHIP_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown) (not an exact tag match)" >&2
    fi
    echo "       This bridge is only ever built/tested against that exact SDK revision -- see docs/matter-bridge.md." >&2
    echo "       Fix with: git -C \"\$CHIP_ROOT\" checkout $REQUIRED_TAG" >&2
    exit 1
fi

OUT_DIR="${1:-${TMPDIR:-/tmp}/pogo_matter_bridge_build}"
mkdir -p "$OUT_DIR"

echo "==> pogo_matter_bridge build"
echo "    CHIP_ROOT = $CHIP_ROOT (tag $ACTUAL_REF, verified)"
echo "    out dir   = $OUT_DIR"

# --- Activate the Matter build environment (gn, ninja, zap-cli) -----------
if [ -f "$CHIP_ROOT/.environment/activate.sh" ]; then
    # shellcheck disable=SC1091
    source "$CHIP_ROOT/.environment/activate.sh"
fi
for tool in gn ninja zap-cli; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: '$tool' not found on PATH, and $CHIP_ROOT/.environment/activate.sh did not provide it." >&2
        echo "       Run '(cd \"\$CHIP_ROOT\" && scripts/bootstrap.sh)' once first." >&2
        exit 1
    fi
done

# --- Point this app's build-time-only SDK symlink at the real checkout ---
# //third_party/connectedhomeip is NEVER checked into this repo (CHIP_ROOT
# varies per machine, and this repo must never vendor the SDK -- see
# BUILD.gn's own top comment) -- this script (re)creates it fresh every
# run. //third_party/pogo_repo (this repo's own root) IS checked in, since
# it is a fixed relative path that never changes.
mkdir -p "$APP_DIR/third_party"
ln -sfn "$CHIP_ROOT" "$APP_DIR/third_party/connectedhomeip"

# --- Materialize the real .zap from its CHIP_ROOT-independent template ---
# See bridge-common/BUILD.gn's own top comment for why pogo-matter-bridge.zap
# itself is never checked in either.
sed "s#__CHIP_ROOT__#${CHIP_ROOT}#g" \
    "$APP_DIR/bridge-common/pogo-matter-bridge.zap.in" \
    > "$APP_DIR/bridge-common/pogo-matter-bridge.zap"

# --- The build identity (docs/versioning.md) --------------------------------
# Component, product version (versions.json), commit, clean/dirty and source
# fingerprint, as the header api/src/build_info.cpp includes -- what
# `pogo_matter_bridge --version` and the health `build` field then say.
# Generated here, before every build, into api/generated/ (never checked in).
# POGO_RELEASE=1 makes it a release build, which the tool refuses unless the
# tree is clean and its commit is known. Without python3 the build goes on
# and the bridge reports "unknown" rather than a made-up version.
POGO_REPO_ROOT="$(cd "$APP_DIR/../../.." && pwd)"
POGO_STAMP_ARGS=(stamp --component linux_bridge --out "$APP_DIR/api/generated/pogo_build_info_gen.h" --variant gn-linux)
if [ "${POGO_RELEASE:-0}" = "1" ]; then
    POGO_STAMP_ARGS+=(--release)
fi
if command -v python3 >/dev/null 2>&1; then
    python3 "$POGO_REPO_ROOT/tools/pogo_version.py" "${POGO_STAMP_ARGS[@]}"
else
    if [ "${POGO_RELEASE:-0}" = "1" ]; then
        echo "error: POGO_RELEASE=1 needs python3 for tools/pogo_version.py" >&2
        exit 1
    fi
    rm -f "$APP_DIR/api/generated/pogo_build_info_gen.h"
    echo "warning: python3 not found -- this build will report its version as \"unknown\"" >&2
fi

# --- gn gen + ninja --------------------------------------------------------
GN_ARGS=(
    'target_os="linux"'
    "is_debug=false"
    "chip_config_network_layer_ble=false"
    "chip_enable_wifi=false"
    "chip_enable_ethernet=false"
    "matter_enable_java_compilation=false"
)
shift 2>/dev/null || true
if [ "${1:-}" = "--" ]; then
    shift
    GN_ARGS+=("$@")
fi

(
    cd "$APP_DIR"
    gn gen "$OUT_DIR" --root="$APP_DIR" --args="${GN_ARGS[*]}"
    ninja -C "$OUT_DIR" pogo_matter_bridge pogohome-hubctl pogohome-otbr-helper
)

echo "==> Built: $OUT_DIR/pogo_matter_bridge"
echo "==> Built: $OUT_DIR/pogohome-hubctl"
echo "==> Built: $OUT_DIR/pogohome-otbr-helper (installed only by hand; see docs/pogohome-api-v1.md)"
