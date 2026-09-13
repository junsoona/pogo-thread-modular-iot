#!/usr/bin/env python3
"""
Packs configs/power_base.json into a power_base_config NVS partition image
(power_base_config.bin -- see docs/power-base.md,
components/power_base_config).

Reuses tools/pack_module_config.py's GPIO reserved-pin classification and
CRC16 implementation directly (same ESP32-H2 chip, same electrical rules,
same integrity scheme -- see that module's own docstring for why those
tables must never drift from components/module_config/gpio_profile.c, kept
mechanically checked by tools/test_gpio_profile_parity.py) instead of
duplicating either.

Usage:
    python3 tools/pack_power_base_config.py configs/power_base.json build/cfg_images/power_base_config.bin

Requires the ESP-IDF python environment (for nvs_partition_gen.py) --
source $IDF_PATH/export.sh first, same as for idf.py.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pack_module_config import as_int, check_gpio, crc16_ccitt, find_nvs_partition_gen  # noqa: E402

CFG_PARTITION_SIZE = 0x3000  # must match power_base/partitions.csv's "cfg" partition size


def pack_port(p: dict) -> bytes:
    # Must match power_base_config.c's per-port layout (offsets 8..23) exactly.
    return struct.pack(
        ">hhhhhBBI",
        as_int(p["uart_num"]),
        as_int(p["tx_gpio"]),
        as_int(p["rx_gpio"]),
        as_int(p["detect_in_gpio"]),
        as_int(p["detect_out_gpio"]),
        1 if p.get("detect_out_active_low", True) else 0,
        0,  # reserved
        as_int(p.get("debounce_ms", 30)),
    )


def pack_config(cfg: dict) -> bytes:
    header = struct.pack(">II", as_int(cfg["schema_version"]), as_int(cfg["config_revision"]))
    port = pack_port(cfg["downstream_port"])
    tail = struct.pack(
        ">IIIhBBI",
        as_int(cfg["ack_timeout_ms"]),
        as_int(cfg["topology_stale_timeout_ms"]),
        as_int(cfg["heartbeat_timeout_ms"]),
        as_int(cfg["power_enable_gpio"]),
        1 if cfg.get("power_enable_active_high", True) else 0,
        0,  # reserved
        as_int(cfg["power_startup_delay_ms"]),
    )
    blob = header + port + tail
    if len(blob) != 44:
        raise AssertionError(f"internal error: packed blob is {len(blob)} bytes, expected 44")
    return blob


def verify_power_base_cfg_blob_len(image_path: str, expected_len: int) -> None:
    """Same regression guard as pack_module_config.py's
    verify_module_cfg_blob_len(), scoped to the 'power_base_cfg' namespace."""
    idf_path = os.environ.get("IDF_PATH", "")
    tool_dir = os.path.join(idf_path, "components", "nvs_flash", "nvs_partition_tool")
    if not os.path.isdir(tool_dir):
        sys.exit(f"cannot verify the generated image: nvs_partition_tool not found at {tool_dir}")
    if tool_dir not in sys.path:
        sys.path.insert(0, tool_dir)
    import nvs_parser  # noqa: E402 -- IDF_PATH-dependent, must be imported at runtime

    with open(image_path, "rb") as f:
        raw = bytearray(f.read())
    partition = nvs_parser.NVS_Partition(os.path.basename(image_path), raw)

    namespace_id = None
    blob_size = None
    for page in partition.pages:
        for entry in page.entries:
            if entry.key == "power_base_cfg" and entry.metadata["namespace"] == 0 and entry.data:
                namespace_id = entry.data.get("value")
            elif (
                entry.key == "blob"
                and entry.data
                and namespace_id is not None
                and entry.metadata["namespace"] == namespace_id
            ):
                blob_size = entry.data.get("size")

    if namespace_id is None:
        sys.exit(f"verify_power_base_cfg_blob_len: 'power_base_cfg' namespace not found in {image_path}")
    if blob_size is None:
        sys.exit(f"verify_power_base_cfg_blob_len: power_base_cfg:blob entry not found in {image_path}")
    if blob_size != expected_len:
        sys.exit(
            f"verify_power_base_cfg_blob_len: power_base_cfg:blob is {blob_size} bytes in {image_path}, "
            f"expected {expected_len} -- check the CSV row uses 'blob,file,binary,<path>', not "
            f"'blob,data,binary,<path>' (see tools/pack_module_config.py's own regression-guard comment)"
        )


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("config_json", help="e.g. configs/power_base.json")
    ap.add_argument("output_bin", help="e.g. power_base_config.bin")
    ap.add_argument("--size", type=lambda s: int(s, 0), default=CFG_PARTITION_SIZE,
                     help=f"cfg partition size in bytes (default 0x{CFG_PARTITION_SIZE:x}, "
                          f"must match power_base/partitions.csv)")
    args = ap.parse_args()

    with open(args.config_json, "r", encoding="utf-8") as f:
        cfg = json.load(f)

    # Same GPIO reserved-pin policy module_config.c/power_base_config.c
    # enforce at boot -- caught here too before ever producing a .bin.
    p = cfg["downstream_port"]
    for field in ("tx_gpio", "rx_gpio", "detect_in_gpio", "detect_out_gpio"):
        check_gpio(f"downstream_port.{field}", p[field])
        if as_int(p[field]) < 0:
            sys.exit(f"refusing to pack: downstream_port.{field} must be set -- the power base's one POGO port "
                      f"is always enabled (see docs/power-base.md)")
    if not p.get("detect_out_active_low", True):
        sys.exit("refusing to pack: downstream_port.detect_out_active_low=false is not supported yet "
                  "(DETECT_OUT is open-drain active-low only -- see docs/pogo-interface.md)")
    if as_int(cfg["downstream_port"]["uart_num"]) not in (0, 1):
        sys.exit("refusing to pack: downstream_port.uart_num must be 0 or 1 (ESP32-H2 has exactly two UART "
                  "controllers)")

    power_enable_gpio = as_int(cfg["power_enable_gpio"])
    check_gpio("power_enable_gpio", power_enable_gpio)
    used = [as_int(p["tx_gpio"]), as_int(p["rx_gpio"]), as_int(p["detect_in_gpio"]), as_int(p["detect_out_gpio"])]
    if power_enable_gpio >= 0 and power_enable_gpio in used:
        sys.exit(f"refusing to pack: power_enable_gpio={power_enable_gpio} is also used by the downstream port")
    if len(used) != len(set(used)):
        sys.exit("refusing to pack: downstream_port GPIOs are not all distinct")

    if as_int(cfg["topology_stale_timeout_ms"]) == 0:
        sys.exit("refusing to pack: topology_stale_timeout_ms must not be 0")
    if as_int(cfg["heartbeat_timeout_ms"]) == 0:
        sys.exit("refusing to pack: heartbeat_timeout_ms must not be 0")
    if as_int(cfg["heartbeat_timeout_ms"]) >= as_int(cfg["topology_stale_timeout_ms"]):
        sys.exit("refusing to pack: heartbeat_timeout_ms must be smaller than topology_stale_timeout_ms")

    blob = pack_config(cfg)
    crc = crc16_ccitt(blob)

    with tempfile.TemporaryDirectory() as tmp:
        blob_path = os.path.join(tmp, "blob.bin")
        with open(blob_path, "wb") as f:
            f.write(blob)

        csv_path = os.path.join(tmp, "power_base_cfg.csv")
        with open(csv_path, "w", encoding="utf-8") as f:
            f.write("key,type,encoding,value\n")
            f.write("power_base_cfg,namespace,,\n")
            f.write(f"blob,file,binary,{blob_path}\n")
            f.write(f"crc,data,u16,{crc}\n")

        tool = find_nvs_partition_gen()
        out_path = os.path.abspath(args.output_bin)
        os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
        subprocess.run(
            [sys.executable, tool, "generate", csv_path, out_path, str(args.size)],
            check=True,
        )

    verify_power_base_cfg_blob_len(out_path, len(blob))

    print(f"wrote {args.output_bin} (blob_crc=0x{crc:04x}, verified power_base_cfg:blob={len(blob)} bytes)")


if __name__ == "__main__":
    main()
