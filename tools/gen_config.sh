#!/usr/bin/env bash
# Generates dev_root_config.bin and dev_leaf_config.bin from configs/*.json
# (see docs/module-config.md). Requires the ESP-IDF environment:
#   source ~/.espressif/v6.1/esp-idf/export.sh
#
# Usage: tools/gen_config.sh [output_dir]   (default output_dir: build/cfg_images)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${1:-$ROOT_DIR/build/cfg_images}"
mkdir -p "$OUT_DIR"

python3 "$ROOT_DIR/tools/pack_module_config.py" "$ROOT_DIR/configs/dev_root.json" "$OUT_DIR/dev_root_config.bin"
python3 "$ROOT_DIR/tools/pack_module_config.py" "$ROOT_DIR/configs/dev_leaf.json" "$OUT_DIR/dev_leaf_config.bin"
python3 "$ROOT_DIR/tools/pack_module_config.py" "$ROOT_DIR/configs/prototype_sht45.json" "$OUT_DIR/prototype_sht45_config.bin"
python3 "$ROOT_DIR/tools/pack_module_config.py" "$ROOT_DIR/configs/prototype_bh1750.json" "$OUT_DIR/prototype_bh1750_config.bin"
python3 "$ROOT_DIR/tools/pack_module_config.py" "$ROOT_DIR/configs/prototype_pms5003.json" "$OUT_DIR/prototype_pms5003_config.bin"

echo "config images written to $OUT_DIR"
