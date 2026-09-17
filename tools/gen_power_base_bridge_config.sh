#!/usr/bin/env bash
#
# Generates a power_base cfg image that ALSO carries the "pb_bridge"
# namespace (components/power_base_bridge_config -- this follow-up:
# northbound Power Base -> Linux OTBR host bridge transport,
# components/linux_bridge_exporter). A separate, explicit wrapper from
# tools/gen_power_base_config.sh / tools/gen_power_base_otbr_config.sh so
# "does this image also configure a Linux bridge host" is never an implicit
# side effect of generating an ordinary STANDALONE or EXTERNAL_OTBR image.
#
# The Linux host's real IPv6 address is obtained on the Linux OTBR host
# itself, once it's up and joined:
#   sudo ot-ctl ipaddr mleid
# This is NOT a secret the way the Thread Dataset is (no Network Key
# involved), but it is still never hardcoded here or in any committed
# fixture -- always passed explicitly by the operator.
#
# Requires the ESP-IDF environment (for nvs_partition_gen.py):
#   source ~/.espressif/v6.1/esp-idf/export.sh
#
# Usage:
#   tools/gen_power_base_bridge_config.sh <bridge_host_ipv6> <bridge_port> [output_dir] [dataset_hex_file]
#
#   bridge_host_ipv6    The Linux OTBR host's real IPv6 address (see above).
#   bridge_port         The Linux OTBR host's CoAP ingest port (linux/matter_bridge/ingest
#                        listens on 5684 by default).
#   output_dir          Where to write the image (default: build/cfg_images).
#   dataset_hex_file    Optional: also join that EXISTING Thread network in
#                        EXTERNAL_OTBR mode (same file format as
#                        tools/gen_power_base_otbr_config.sh's own argument).
#                        Omit this to produce a STANDALONE image that ALSO
#                        has the bridge configured.
set -euo pipefail

if [ $# -lt 2 ]; then
    echo "usage: tools/gen_power_base_bridge_config.sh <bridge_host_ipv6> <bridge_port> [output_dir] [dataset_hex_file]" >&2
    exit 1
fi

BRIDGE_HOST="$1"
BRIDGE_PORT="$2"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${3:-$ROOT_DIR/build/cfg_images}"
DATASET_FILE="${4:-}"
mkdir -p "$OUT_DIR"

OUT_NAME="power_base_bridge_config.bin"
EXTRA_ARGS=()
if [ -n "$DATASET_FILE" ]; then
    OUT_NAME="power_base_otbr_bridge_config.bin"
    EXTRA_ARGS+=(--external-dataset-file "$DATASET_FILE")
fi

python3 "$ROOT_DIR/tools/pack_power_base_config.py" \
    "$ROOT_DIR/configs/power_base.json" \
    "$OUT_DIR/$OUT_NAME" \
    --bridge-host "$BRIDGE_HOST" --bridge-port "$BRIDGE_PORT" \
    "${EXTRA_ARGS[@]}"

if [ -n "$DATASET_FILE" ]; then
    echo "power_base EXTERNAL_OTBR + bridge config image written to $OUT_DIR/$OUT_NAME (mode 0600, contains the Dataset secret)"
else
    echo "power_base STANDALONE + bridge config image written to $OUT_DIR/$OUT_NAME"
fi
