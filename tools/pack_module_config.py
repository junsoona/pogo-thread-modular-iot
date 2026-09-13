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

# ESP32-H2 GPIO usability profile -- MUST match
# components/module_config/gpio_profile.c's gpio_profile_in_range()/
# gpio_profile_reserved_reason() exactly (checked mechanically by
# tools/test_gpio_profile_parity.py, not just by this comment). See that C
# file's own header comment for the full policy and why GPIO2/3 are ordinary
# GPIOs here (not strapping) while GPIO6/7 are rejected (not bonded out on
# the ESP32-H2 SuperMini/SiP package at all, regardless of strapping/flash/
# USB status).
GPIO_MAX = 27
STRAPPING_GPIOS = {8, 9, 25}
NOT_BONDED_OUT_GPIOS = {6, 7}  # ESP32-H2 SuperMini/SiP package
FLASH_GPIOS = set(range(15, 22))  # 15..21 inclusive
USB_GPIOS = {26, 27}


def gpio_reserved_reason(gpio: int):
    """Returns the same reason strings as gpio_profile_reserved_reason(), or
    None if `gpio` is a plain, assignable GPIO (or -1)."""
    if gpio < 0:
        return None
    if gpio in STRAPPING_GPIOS:
        return "boot strapping pin"
    if gpio in NOT_BONDED_OUT_GPIOS:
        return "not bonded out on ESP32-H2 SuperMini/SiP package"
    if gpio in FLASH_GPIOS:
        return "internal SPI flash pin"
    if gpio in USB_GPIOS:
        return "USB Serial/JTAG pin"
    return None


def check_gpio(field_name: str, gpio) -> None:
    """Mirrors module_config.c's check_gpio_field(): -1 or 0..GPIO_MAX only,
    and not one of the reserved sets above."""
    gpio = as_int(gpio)
    if not (gpio == -1 or 0 <= gpio <= GPIO_MAX):
        sys.exit(f"refusing to pack: {field_name}={gpio} is out of range for ESP32-H2 (valid: -1 or 0..{GPIO_MAX})")
    reason = gpio_reserved_reason(gpio)
    if reason is not None:
        sys.exit(f"refusing to pack: {field_name}={gpio} is reserved on ESP32-H2 ({reason}) -- "
                  f"see docs/pogo-interface.md")


DEV_MODES = {"AUTO": 0, "DEV_ROOT": 1, "DEV_LEAF": 2}
MOCK_DRIVER_ID = 0x4D4F434B  # ASCII "MOCK" -- must match sensor_provider.h's SENSOR_DRIVER_ID_MOCK

# Single common registry for driver_id (section 2 of the follow-up spec: "driver
# ID와 sensor bitmap은 명시적인 공통 헤더에서 관리한다") -- MUST match
# components/sensor_provider/include/sensor_provider/sensor_provider.h's
# SENSOR_DRIVER_ID_* #defines bit-for-bit. configs/*.json may write either the
# mnemonic string below or the raw numeric value; both end up packing the same
# 4 bytes, so no magic number needs to be retyped/reinvented per config file.
DRIVER_IDS = {
    "MOCK": 0x4D4F434B,
    "BH1750": 0x42483137,
    "SHT45": 0x53483435,
    "SGP40": 0x53473430,
    "SGP41": 0x53473431,
    "PMS5003": 0x504D3530,
}

# Bit position of each channel in sensor_type_bitmap (u16) -- MUST match
# components/sensor_channel/include/sensor_channel/sensor_channel.h's
# sensor_channel_type_t enum values exactly (same reasoning as DRIVER_IDS
# above: one numbering, never duplicated by hand per config file).
CHANNEL_BITS = {
    "ILLUMINANCE": 0,
    "TEMPERATURE": 1,
    "HUMIDITY": 2,
    "VOC_RAW": 3,
    "VOC_INDEX": 4,
    "NOX_RAW": 5,
    "NOX_INDEX": 6,
    "PM1_0": 7,
    "PM2_5": 8,
    "PM10": 9,
}


def resolve_driver_id(v):
    """Accepts a mnemonic string ("SHT45"), or falls back to as_int() for a
    raw JSON int / "0x.." string (kept for configs/dev_root.json,
    configs/dev_leaf.json, which predate this registry)."""
    if isinstance(v, str) and v in DRIVER_IDS:
        return DRIVER_IDS[v]
    return as_int(v)


def resolve_sensor_bitmap(v):
    """Accepts a list of channel-name strings (["TEMPERATURE", "HUMIDITY"]),
    or falls back to as_int() for a raw JSON int (kept for
    configs/dev_root.json, configs/dev_leaf.json)."""
    if isinstance(v, list):
        bitmap = 0
        for name in v:
            if name not in CHANNEL_BITS:
                raise ValueError(f"unknown channel name {name!r} in sensor_type_bitmap "
                                  f"(known: {sorted(CHANNEL_BITS)})")
            bitmap |= 1 << CHANNEL_BITS[name]
        return bitmap
    return as_int(v)


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
        resolve_driver_id(cfg["driver_id"]),
        resolve_sensor_bitmap(cfg["sensor_type_bitmap"]),
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


def verify_module_cfg_blob_len(image_path: str, expected_len: int) -> None:
    """
    Re-parses the just-generated NVS image and asserts module_cfg's "blob"
    entry is exactly `expected_len` bytes on the wire -- a regression guard
    for the CSV-row encoding bug where "blob,data,binary,<path>" (wrong)
    stored the temp file *path string itself* as the blob's bytes instead of
    "blob,file,binary,<path>" (correct), which reads the file at that path.
    Both forms make nvs_partition_gen.py exit 0 -- pack_config()'s own
    len(blob) == 90 assert never sees this, since it only checks the bytes
    written to the temp file, not what actually ends up in the NVS image --
    so this is the only check that would have caught it.

    Reuses ESP-IDF's own nvs_partition_tool parser (same component that
    ships nvs_partition_gen.py, already required on PATH via IDF_PATH)
    rather than re-implementing the NVS binary format a second time.
    """
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
            if entry.key == "module_cfg" and entry.metadata["namespace"] == 0 and entry.data:
                namespace_id = entry.data.get("value")
            elif (
                entry.key == "blob"
                and entry.data
                and namespace_id is not None
                and entry.metadata["namespace"] == namespace_id
            ):
                blob_size = entry.data.get("size")

    if namespace_id is None:
        sys.exit(f"verify_module_cfg_blob_len: 'module_cfg' namespace not found in {image_path}")
    if blob_size is None:
        sys.exit(f"verify_module_cfg_blob_len: module_cfg:blob entry not found in {image_path}")
    if blob_size != expected_len:
        sys.exit(
            f"verify_module_cfg_blob_len: module_cfg:blob is {blob_size} bytes in {image_path}, "
            f"expected {expected_len} -- check the CSV row uses 'blob,file,binary,<path>', "
            f"not 'blob,data,binary,<path>' (see tools/pack_module_config.py)"
        )


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
    driver_id_val = resolve_driver_id(cfg["driver_id"])
    if driver_id_val == MOCK_DRIVER_ID and dev_mode_name == "AUTO":
        sys.exit("refusing to pack: driver_id is the MOCK sensor but dev_mode=AUTO -- "
                  "mock is development-only (see docs/module-config.md)")
    if as_int(cfg["measurement_timeout_ms"]) == 0:
        sys.exit("refusing to pack: measurement_timeout_ms must not be 0")
    bus_type_val = as_int(cfg["bus_type"])
    if bus_type_val not in (0, 1):
        sys.exit("refusing to pack: bus_type must be 0 (I2C) or 1 (SPI)")

    # ESP32-H2 GPIO profile -- same reserved-pin policy module_config.c
    # enforces at boot (see gpio_reserved_reason() above), caught here too
    # before ever producing a .bin.
    for port_name in ("upstream_port", "downstream_port"):
        p = cfg[port_name]
        for field in ("tx_gpio", "rx_gpio", "detect_in_gpio", "detect_out_gpio", "base_id_gpio"):
            check_gpio(f"{port_name}.{field}", p[field])
        if as_int(p["detect_out_gpio"]) >= 0 and not p.get("detect_out_active_low", True):
            sys.exit(f"refusing to pack: {port_name}.detect_out_active_low=false is not supported yet "
                      f"(DETECT_OUT is open-drain active-low only -- see docs/pogo-interface.md)")
    if bus_type_val == 0:
        check_gpio("i2c_sda_gpio", cfg["i2c_sda_gpio"])
        check_gpio("i2c_scl_gpio", cfg["i2c_scl_gpio"])
    else:
        for field in ("spi_mosi_gpio", "spi_miso_gpio", "spi_sclk_gpio", "spi_cs_gpio"):
            check_gpio(field, cfg[field])
    if as_int(cfg["downstream_port"]["base_id_gpio"]) != -1:
        sys.exit("refusing to pack: downstream_port.base_id_gpio must be -1 (BASE_ID is UPSTREAM-only, "
                  "see docs/pogo-interface.md)")
    if dev_mode_name == "AUTO" and as_int(cfg["upstream_port"]["base_id_gpio"]) < 0:
        sys.exit("refusing to pack: dev_mode=AUTO but upstream_port.base_id_gpio is not set "
                  "(see docs/module-config.md)")

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
            f.write(f"blob,file,binary,{blob_path}\n")
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

    verify_module_cfg_blob_len(out_path, len(blob))

    print(f"wrote {args.output_bin} (dev_mode={dev_mode_name}, driver_id=0x{driver_id_val:x}, "
          f"blob_crc=0x{crc:04x}, verified module_cfg:blob={len(blob)} bytes)")


if __name__ == "__main__":
    main()
