#!/usr/bin/env python3
"""
Packs a human-editable configs/*.json board description into a module_config
NVS partition image (module_config.bin / dev_root_config.bin / dev_leaf_config.bin
-- see docs/module-config.md).

This is the ONLY place (besides components/module_config/module_config.c's
doc comment) that knows the wire layout of the "blob" NVS value -- if you
change one, change the other and re-generate every image.

Usage:
    python3 tools/pack_module_config.py configs/dev_root.json build/cfg_images/dev_root_config.bin
    python3 tools/pack_module_config.py configs/dev_leaf.json build/cfg_images/dev_leaf_config.bin

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

CFG_PARTITION_SIZE = 0x3000  # must match module/partitions.csv's "cfg" partition size

DEV_MODES = {"AUTO": 0, "DEV_ROOT": 1, "DEV_LEAF": 2}
MOCK_DRIVER_ID = 0x4D4F434B  # ASCII "MOCK" -- must match sensor_provider.h's SENSOR_DRIVER_ID_MOCK


def crc16_ccitt(data: bytes) -> int:
    """CRC16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, xorout 0).
    Must match components/pogo_proto/pogo_crc16.c bit-for-bit."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def as_int(v):
    """Accepts a JSON int, or a "0x.." / decimal string."""
    if isinstance(v, str):
        return int(v, 0)
    return int(v)


def pack_port(p: dict) -> bytes:
    # Must match module_config.c's per-port 18-byte layout exactly.
    return struct.pack(
        ">hhhhhhBBI",
        as_int(p["uart_num"]),
        as_int(p["tx_gpio"]),
        as_int(p["rx_gpio"]),
        as_int(p["detect_in_gpio"]),
        as_int(p["detect_out_gpio"]),
        as_int(p["base_id_gpio"]),
        1 if p.get("detect_out_active_low", True) else 0,
        0,  # reserved
        as_int(p.get("debounce_ms", 30)),
    )


def pack_config(cfg: dict) -> bytes:
    calibration = cfg.get("calibration", [0, 0, 0, 0])
    if len(calibration) != 4:
        raise ValueError("calibration must have exactly 4 entries")

    # Must match module_config.c's 54-byte header layout exactly.
    header = struct.pack(
        ">IIIHBBhhHhhhhI4iI",
        as_int(cfg["schema_version"]),
        as_int(cfg["config_revision"]),
        as_int(cfg["driver_id"]),
        as_int(cfg["sensor_type_bitmap"]),
        as_int(cfg["bus_type"]),
        as_int(cfg["bus_number"]),
        as_int(cfg["i2c_sda_gpio"]),
        as_int(cfg["i2c_scl_gpio"]),
        as_int(cfg["i2c_address"]),
        as_int(cfg["spi_mosi_gpio"]),
        as_int(cfg["spi_miso_gpio"]),
        as_int(cfg["spi_sclk_gpio"]),
        as_int(cfg["spi_cs_gpio"]),
        as_int(cfg["spi_clock_hz"]),
        as_int(calibration[0]),
        as_int(calibration[1]),
        as_int(calibration[2]),
        as_int(calibration[3]),
        as_int(cfg["measurement_timeout_ms"]),
    )
    blob = header + pack_port(cfg["upstream_port"]) + pack_port(cfg["downstream_port"])
    if len(blob) != 90:
        raise AssertionError(f"internal error: packed blob is {len(blob)} bytes, expected 90")
    return blob


def find_nvs_partition_gen() -> str:
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        sys.exit("IDF_PATH is not set -- run `source $IDF_PATH/export.sh` (see docs/module-config.md) first")
    tool = os.path.join(idf_path, "components", "nvs_flash", "nvs_partition_generator", "nvs_partition_gen.py")
    if not os.path.isfile(tool):
        sys.exit(f"nvs_partition_gen.py not found at {tool}")
    return tool


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("config_json", help="e.g. configs/dev_root.json")
    ap.add_argument("output_bin", help="e.g. dev_root_config.bin")
    ap.add_argument("--size", type=lambda s: int(s, 0), default=CFG_PARTITION_SIZE,
                     help=f"cfg partition size in bytes (default 0x{CFG_PARTITION_SIZE:x}, must match partitions.csv)")
    args = ap.parse_args()

    with open(args.config_json, "r", encoding="utf-8") as f:
        cfg = json.load(f)

    dev_mode_name = cfg.get("dev_mode", "AUTO")
    if dev_mode_name not in DEV_MODES:
        sys.exit(f"refusing to pack: dev_mode must be one of {list(DEV_MODES)}, got {dev_mode_name!r}")
    dev_mode_val = DEV_MODES[dev_mode_name]

    # The same checks module_config.c / module/main/app_main.c enforce at
    # boot (see docs/module-config.md) -- catch them here too, before ever
    # producing a .bin someone might flash.
    driver_id_val = as_int(cfg["driver_id"])
    if driver_id_val == MOCK_DRIVER_ID and dev_mode_name == "AUTO":
        sys.exit("refusing to pack: driver_id is the MOCK sensor but dev_mode=AUTO -- "
                  "mock is development-only (see docs/module-config.md)")
    if as_int(cfg["measurement_timeout_ms"]) == 0:
        sys.exit("refusing to pack: measurement_timeout_ms must not be 0")
    if as_int(cfg["bus_type"]) not in (0, 1):
        sys.exit("refusing to pack: bus_type must be 0 (I2C) or 1 (SPI)")
    for port_name in ("upstream_port", "downstream_port"):
        p = cfg[port_name]
        if as_int(p["detect_out_gpio"]) >= 0 and not p.get("detect_out_active_low", True):
            sys.exit(f"refusing to pack: {port_name}.detect_out_active_low=false is not supported yet "
                      f"(DETECT_OUT is open-drain active-low only -- see docs/pogo-interface.md)")

    blob = pack_config(cfg)
    crc = crc16_ccitt(blob)

    with tempfile.TemporaryDirectory() as tmp:
        blob_path = os.path.join(tmp, "blob.bin")
        with open(blob_path, "wb") as f:
            f.write(blob)

        csv_path = os.path.join(tmp, "cfg.csv")
        with open(csv_path, "w", encoding="utf-8") as f:
            f.write("key,type,encoding,value\n")
            f.write("module_cfg,namespace,,\n")
            f.write(f"blob,data,binary,{blob_path}\n")
            f.write(f"crc,data,u16,{crc}\n")
            f.write("dev_cfg,namespace,,\n")
            f.write(f"dev_mode,data,u8,{dev_mode_val}\n")

        tool = find_nvs_partition_gen()
        out_path = os.path.abspath(args.output_bin)
        os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
        subprocess.run(
            [sys.executable, tool, "generate", csv_path, out_path, str(args.size)],
            check=True,
        )

    print(f"wrote {args.output_bin} (dev_mode={dev_mode_name}, driver_id=0x{as_int(cfg['driver_id']):x}, "
          f"blob_crc=0x{crc:04x})")


if __name__ == "__main__":
    main()
