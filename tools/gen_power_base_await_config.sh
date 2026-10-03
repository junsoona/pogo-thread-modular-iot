#!/usr/bin/env bash
# Prepare a SPARE Power Base for enrollment (pb_thread:mode=2,
# AWAIT_COMMISSIONING). The image contains no Thread Active Dataset: the
# board cannot resume or form a network until it enrolls through a Joiner
# (today only the bench "pb_join" console of a power_base build with
# sdkconfig.bench_joiner -- see docs/power-base.md).
#
# Optionally also stores the Linux bridge's Mesh-Local address and CoAP
# ingest port in "pb_bridge". The exporter still starts only after the board
# has enrolled. Get the address on the Linux OTBR host (`sudo ot-ctl ipaddr
# mleid`) and pass it explicitly -- it is never defaulted or hardcoded.
#
# Requires the ESP-IDF environment (for nvs_partition_gen.py):
#   source ~/.espressif/v6.1/esp-idf/export.sh
#
# Usage:
#   tools/gen_power_base_await_config.sh [output_dir] [bridge_host_ipv6 bridge_port]
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${1:-$ROOT_DIR/build/cfg_images}"
BRIDGE_HOST="${2:-}"
BRIDGE_PORT="${3:-}"

if [ $# -gt 3 ] || { [ -n "$BRIDGE_HOST" ] && [ -z "$BRIDGE_PORT" ]; }; then
    echo "usage: tools/gen_power_base_await_config.sh [output_dir] [bridge_host_ipv6 bridge_port]" >&2
    exit 1
fi
mkdir -p "$OUT_DIR"

OUT_NAME="power_base_await_config.bin"
EXTRA_ARGS=()
if [ -n "$BRIDGE_HOST" ]; then
    OUT_NAME="power_base_await_bridge_config.bin"
    EXTRA_ARGS+=(--bridge-host "$BRIDGE_HOST" --bridge-port "$BRIDGE_PORT")
fi

python3 "$ROOT_DIR/tools/pack_power_base_config.py" \
    "$ROOT_DIR/configs/power_base.json" \
    "$OUT_DIR/$OUT_NAME" \
    --await-commissioning \
    ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}
