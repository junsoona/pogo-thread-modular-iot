#!/usr/bin/env bash
#
# Generates power_base_otbr_config.bin -- the power base's cfg image for
# EXTERNAL_OTBR mode (joining a Linux OTBR + separate ESP32-H2 RCP's
# existing Thread network instead of forming its own -- see
# docs/power-base.md, components/power_base_thread_config,
# tools/pack_power_base_config.py).
#
# The Dataset file's content is a real secret (it contains the Network Key)
# -- this script never prints it, and the generated image is written 0600.
# It is loaded here ONLY as a file path, never as a command-line value.
#
# Usage:
#   tools/gen_power_base_otbr_config.sh <dataset_hex_file> [output_dir]
#
#   dataset_hex_file   Path to a file holding the OTBR's Active Dataset TLVs
#                       as hex text, e.g. the output of
#                       `ot-ctl dataset active -x` on the Linux OTBR host
#                       with the trailing "Done" line removed. NEVER commit
#                       this file (see .gitignore's /secrets/ and
#                       **/otbr-active-dataset.hex entries).
#   output_dir          Where to write power_base_otbr_config.bin
#                       (default: build/cfg_images)
#
# Requires the ESP-IDF environment (for nvs_partition_gen.py):
#   source ~/.espressif/v6.1/esp-idf/export.sh
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: tools/gen_power_base_otbr_config.sh <dataset_hex_file> [output_dir]" >&2
    exit 1
fi

DATASET_FILE="$1"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${2:-$ROOT_DIR/build/cfg_images}"
mkdir -p "$OUT_DIR"

python3 "$ROOT_DIR/tools/pack_power_base_config.py" \
    "$ROOT_DIR/configs/power_base.json" \
    "$OUT_DIR/power_base_otbr_config.bin" \
    --external-dataset-file "$DATASET_FILE"

echo "power_base EXTERNAL_OTBR config image written to $OUT_DIR/power_base_otbr_config.bin (mode 0600)"
