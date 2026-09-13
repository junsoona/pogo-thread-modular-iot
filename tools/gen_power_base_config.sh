#!/usr/bin/env bash
# Generates power_base_config.bin from configs/power_base.json (see
# docs/power-base.md). Requires the ESP-IDF environment:
#   source ~/.espressif/v6.1/esp-idf/export.sh
#
# Usage: tools/gen_power_base_config.sh [output_dir]   (default output_dir: build/cfg_images)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${1:-$ROOT_DIR/build/cfg_images}"
mkdir -p "$OUT_DIR"

python3 "$ROOT_DIR/tools/pack_power_base_config.py" "$ROOT_DIR/configs/power_base.json" "$OUT_DIR/power_base_config.bin"

echo "power_base config image written to $OUT_DIR"
