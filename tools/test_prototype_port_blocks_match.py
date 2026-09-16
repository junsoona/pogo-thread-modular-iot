#!/usr/bin/env python3
"""
Host checks for every AUTO-mode prototype board config in configs/*.json.

Originally this only compared prototype_sht45.json and prototype_bh1750.json
(the two boards validated on real hardware so far). Extended for the 3-tier
chain follow-up (Power Base -> SHT45/BH1750 INTERMEDIATE -> PMS5003 LEAF,
plus the still-unwired SGP40/SGP41 prototypes) to additionally cover:

  1. All FOUR ordinary I2C sensor boards (SHT45, BH1750, SGP40, SGP41) share
     an EXACTLY identical upstream_port/downstream_port block -- per section
     2/4 of the follow-up spec, only driver_id/i2c_address/
     sensor_type_bitmap/measurement_timeout_ms/calibration may ever differ
     between ordinary I2C boards; the POGO wiring must not.
  2. Each of those four configs' driver_id/sensor_type_bitmap/i2c_address,
     and the SGP40/SGP41 pair's shared GPIO0/1 I2C bus encoding, are
     confirmed in the ACTUAL PACKED NVS BLOB BYTES (not just the source
     JSON) -- a byte-offset regression in pack_module_config.py's
     pack_config() would otherwise pass source-level checks and still ship
     a wrong image.
  3. prototype_pms5003.json is confirmed to be the ONLY terminal/downstream-
     disabled exception among the five prototype configs -- every other
     config must keep its downstream POGO port fully enabled.
  4. tools/pack_module_config.py's DRIVER_IDS/CHANNEL_BITS dicts are
     mechanically diffed against the actual #define/enum values in
     components/sensor_provider/include/sensor_provider/sensor_provider.h
     and components/sensor_channel/include/sensor_channel/sensor_channel.h
     -- if the common header and the Python mapping ever drift apart, this
     test fails instead of silently packing a stale numeric ID.
  5. If IDF_PATH is set and build/cfg_images/*.bin already exist (source
     $IDF_PATH/export.sh; bash tools/gen_config.sh build/cfg_images), the
     ACTUAL GENERATED NVS IMAGES are re-parsed and their on-flash
     module_cfg:blob content is confirmed byte-for-byte, including
     SGP40/SGP41's driver_id/bitmap/0x59 address/GPIO0-1 encoding -- this is
     the only check that exercises the NVS CSV generation step itself, not
     just pack_config()'s in-memory bytes. Skipped (not failed) otherwise.
  6. Extended for the DOWNSTREAM DETECT_OUT GPIO22->GPIO3 relocation
     follow-up: every active final-prototype config (the four I2C boards
     plus configs/power_base.json) now uses GPIO3 for downstream_port.
     detect_out_gpio, GPIO22 is confirmed absent from every enabled port
     field of all five, and configs/power_base.json's own 44-byte
     power_base_config blob (a SEPARATE schema/packer from module_config's
     -- see tools/pack_power_base_config.py) is checked the same way as
     section 2/5 above: in-memory pack_config() bytes, and (same IDF_PATH/
     build/cfg_images gating, this time also requiring
     tools/gen_power_base_config.sh to have run) the actual generated
     power_base_config.bin re-parsed from disk.

Usage:
    python3 tools/test_prototype_port_blocks_match.py
    # for sections 5/6's NVS-image-content checks too:
    source ~/.espressif/v6.1/esp-idf/export.sh
    bash tools/gen_config.sh build/cfg_images
    bash tools/gen_power_base_config.sh build/cfg_images
    python3 tools/test_prototype_port_blocks_match.py
"""
import importlib.util
import json
import os
import re
import struct
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PACKER_PATH = os.path.join(ROOT_DIR, "tools", "pack_module_config.py")
SPEC = importlib.util.spec_from_file_location("pack_module_config", PACKER_PATH)
packer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(packer)

POWER_BASE_PACKER_PATH = os.path.join(ROOT_DIR, "tools", "pack_power_base_config.py")
POWER_BASE_SPEC = importlib.util.spec_from_file_location("pack_power_base_config", POWER_BASE_PACKER_PATH)
power_base_packer = importlib.util.module_from_spec(POWER_BASE_SPEC)
POWER_BASE_SPEC.loader.exec_module(power_base_packer)

# Byte offsets into pack_config()'s schema-v1/v2 header (54 bytes, layout
# ">IIIHBBhhHhhhhI4iI" -- see pack_module_config.py's pack_config()).
OFF_DRIVER_ID = 8
OFF_SENSOR_BITMAP = 12
OFF_BUS_TYPE = 14
OFF_I2C_SDA_GPIO = 16
OFF_I2C_SCL_GPIO = 18
OFF_I2C_ADDRESS = 20
HEADER_LEN = 54
UPSTREAM_PORT_LEN = 18  # pack_port()'s ">hhhhhhBBI" -- see pack_module_config.py
# schema-v1 downstream_port.detect_out_gpio: HEADER_LEN + one full upstream_port
# block + detect_out_gpio's own offset within a port block (uart_num, tx_gpio,
# rx_gpio, detect_in_gpio, detect_out_gpio -- 4th field, 2 bytes each = offset 8).
OFF_MODULE_DOWNSTREAM_DETECT_OUT = HEADER_LEN + UPSTREAM_PORT_LEN + 8

# Byte offset into pack_power_base_config.py's pack_config()'s 44-byte blob
# (">II" 8-byte header + downstream_port's own ">hhhhhBBI" layout: uart_num,
# tx_gpio, rx_gpio, detect_in_gpio, detect_out_gpio -- 4th field, offset 8
# within the port block) -- see pack_power_base_config.py's pack_port().
OFF_POWER_BASE_DOWNSTREAM_DETECT_OUT = 8 + 8

GPIO22 = 22  # the header-inconvenient pin this follow-up removes from every active downstream_port
GPIO3 = 3    # its header-accessible replacement for DOWNSTREAM DETECT_OUT


def read_cfg_blob_from_nvs_image(image_path: str, namespace_key: str):
    """Re-parses a generated NVS image and reconstructs the blob bytes
    stored under `namespace_key` ("module_cfg" or "power_base_cfg") --
    same reconstruction approach as pack_module_config.py's
    read_module_cfg_blob(), generalized to power_base_config's separate
    namespace so this test can re-verify power_base_config.bin's actual
    on-flash content too, without duplicating pack_power_base_config.py's
    own packer. Returns None if the namespace/blob entry is not found."""
    nvs_parser = packer._import_nvs_parser()
    with open(image_path, "rb") as f:
        raw = bytearray(f.read())
    partition = nvs_parser.NVS_Partition(os.path.basename(image_path), raw)
    namespace_id = None
    blob_entry = None
    for page in partition.pages:
        for entry in page.entries:
            if entry.key == namespace_key and entry.metadata["namespace"] == 0 and entry.data:
                namespace_id = entry.data.get("value")
            elif (
                entry.key == "blob"
                and entry.data
                and namespace_id is not None
                and entry.metadata["namespace"] == namespace_id
                and entry.metadata["type"] == "blob_data"
            ):
                blob_entry = entry
    if blob_entry is None:
        return None
    raw_bytes = bytearray()
    for child in blob_entry.children:
        raw_bytes += child.raw
    return bytes(raw_bytes[: blob_entry.data["size"]])


failures = 0


def check(cond, msg):
    global failures
    print(("ok:   " if cond else "FAIL: ") + msg)
    if not cond:
        failures += 1


def load(name):
    with open(os.path.join(ROOT_DIR, "configs", name), "r", encoding="utf-8") as f:
        return json.load(f)


def main():
    global failures

    i2c_boards = {
        "sht45": load("prototype_sht45.json"),
        "bh1750": load("prototype_bh1750.json"),
        "sgp40": load("prototype_sgp40.json"),
        "sgp41": load("prototype_sgp41.json"),
    }
    pms5003 = load("prototype_pms5003.json")
    power_base = load("power_base.json")

    # --- 1. All four I2C boards' POGO port blocks are byte-for-byte
    # identical (checked against sht45 as the reference). ---
    reference = i2c_boards["sht45"]
    for name, cfg in i2c_boards.items():
        if name == "sht45":
            continue
        for port_name in ("upstream_port", "downstream_port"):
            check(cfg[port_name] == reference[port_name],
                  f"{port_name} is byte-for-byte identical between prototype_sht45.json and prototype_{name}.json")

    # Confirm they DO still differ on driver identity -- a passing "ports
    # identical" check would be meaningless if all four files were
    # accidentally identical copies of each other.
    names = list(i2c_boards)
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            a, b = i2c_boards[names[i]], i2c_boards[names[j]]
            check(a["driver_id"] != b["driver_id"],
                  f"driver_id differs between {names[i]} and {names[j]} (as expected)")
            check(a["sensor_type_bitmap"] != b["sensor_type_bitmap"],
                  f"sensor_type_bitmap differs between {names[i]} and {names[j]} (as expected)")

    # SGP40 and SGP41 share the SAME fixed 0x59 datasheet address (no ADDR
    # strap on either part) -- that must stay EQUAL, unlike SHT45/BH1750's
    # distinct addresses. Assert this explicitly so the mismatch above never
    # gets "fixed" into a wrong, board-specific address by mistake.
    check(i2c_boards["sgp40"]["i2c_address"] == i2c_boards["sgp41"]["i2c_address"] == "0x59",
          "sgp40 and sgp41 share the same fixed 0x59 I2C address (as expected -- no ADDR strap on either part)")

    # And the shared fields that must match beyond just the port blocks.
    for name, cfg in i2c_boards.items():
        check(cfg["dev_mode"] == "AUTO", f"{name}: dev_mode=AUTO")
        check(cfg["bus_type"] in (0, "I2C"), f"{name}: bus_type=I2C")
        check(cfg["i2c_sda_gpio"] == reference["i2c_sda_gpio"] == 0, f"{name}: i2c_sda_gpio matches (GPIO0)")
        check(cfg["i2c_scl_gpio"] == reference["i2c_scl_gpio"] == 1, f"{name}: i2c_scl_gpio matches (GPIO1)")

    # Explicitly confirm the deep-sleep-ready GPIO swap made it into all
    # four files (UPSTREAM DETECT_IN=GPIO12/DETECT_OUT=GPIO2).
    for name, cfg in i2c_boards.items():
        check(cfg["upstream_port"]["detect_in_gpio"] == 12,
              f"{name}: upstream_port.detect_in_gpio is 12 (deep-sleep-ready swap)")
        check(cfg["upstream_port"]["detect_out_gpio"] == 2,
              f"{name}: upstream_port.detect_out_gpio is 2 (deep-sleep-ready swap)")

    # --- 2. Re-pack each config and confirm driver_id/bitmap/address/GPIO
    # encoding in the ACTUAL BLOB BYTES, not just the source JSON. ---
    for name, cfg in i2c_boards.items():
        blob = packer.pack_config(cfg)
        check(len(blob) >= HEADER_LEN, f"{name}: packed blob is at least the {HEADER_LEN}-byte header")

        expected_driver_id = packer.resolve_driver_id(cfg["driver_id"])
        got_driver_id = struct.unpack_from(">I", blob, OFF_DRIVER_ID)[0]
        check(got_driver_id == expected_driver_id,
              f"{name}: packed blob driver_id=0x{got_driver_id:08x} matches configs/prototype_{name}.json (0x{expected_driver_id:08x})")

        expected_bitmap = packer.resolve_sensor_bitmap(cfg["sensor_type_bitmap"])
        got_bitmap = struct.unpack_from(">H", blob, OFF_SENSOR_BITMAP)[0]
        check(got_bitmap == expected_bitmap,
              f"{name}: packed blob sensor_type_bitmap=0x{got_bitmap:04x} matches configs/prototype_{name}.json (0x{expected_bitmap:04x})")

        got_bus_type = blob[OFF_BUS_TYPE]
        check(got_bus_type == packer.BUS_TYPES["I2C"], f"{name}: packed blob bus_type is I2C(0)")

        got_sda, got_scl = struct.unpack_from(">hh", blob, OFF_I2C_SDA_GPIO)
        check((got_sda, got_scl) == (0, 1), f"{name}: packed blob encodes I2C SDA/SCL as GPIO0/GPIO1")

        expected_addr = packer.as_int(cfg["i2c_address"])
        got_addr = struct.unpack_from(">H", blob, OFF_I2C_ADDRESS)[0]
        check(got_addr == expected_addr,
              f"{name}: packed blob i2c_address=0x{got_addr:02x} matches configs/prototype_{name}.json (0x{expected_addr:02x})")

    # SGP40/SGP41 specifically: both must encode the fixed 7-bit 0x59
    # address in the blob (the follow-up spec's explicit ask).
    for name in ("sgp40", "sgp41"):
        blob = packer.pack_config(i2c_boards[name])
        got_addr = struct.unpack_from(">H", blob, OFF_I2C_ADDRESS)[0]
        check(got_addr == 0x59, f"{name}: packed blob i2c_address is exactly 0x59")

    # --- 3. PMS5003 is the ONLY terminal/downstream-disabled exception. ---
    down_fields = ("uart_num", "tx_gpio", "rx_gpio", "detect_in_gpio", "detect_out_gpio", "base_id_gpio")
    check(all(packer.as_int(pms5003["downstream_port"][f]) == -1 for f in down_fields),
          "prototype_pms5003.json: downstream_port is fully disabled (terminal-only)")
    check(pms5003["bus_type"] == "UART", "prototype_pms5003.json: bus_type=UART (sensor bus, not POGO)")
    check(pms5003["schema_version"] == 2, "prototype_pms5003.json: schema_version=2 (UART sensor extension)")

    for name, cfg in i2c_boards.items():
        check(all(packer.as_int(cfg["downstream_port"][f]) != -1 for f in
                   ("uart_num", "tx_gpio", "rx_gpio", "detect_in_gpio", "detect_out_gpio")),
              f"{name}: downstream_port stays fully enabled (NOT a terminal-only exception like PMS5003)")

    # PMS5003's upstream POGO port still matches the common block (it is a
    # normal POGO child, only its DOWNSTREAM face and sensor bus differ).
    check(pms5003["upstream_port"] == reference["upstream_port"],
          "prototype_pms5003.json: upstream_port matches the common I2C-board upstream_port block")

    # --- 4. pack_module_config.py's DRIVER_IDS/CHANNEL_BITS must not drift
    # from the C headers they claim to mirror. ---
    provider_h = os.path.join(ROOT_DIR, "components", "sensor_provider", "include", "sensor_provider",
                               "sensor_provider.h")
    channel_h = os.path.join(ROOT_DIR, "components", "sensor_channel", "include", "sensor_channel",
                              "sensor_channel.h")

    with open(provider_h, "r", encoding="utf-8") as f:
        provider_src = f.read()
    c_driver_ids = {
        m.group(1): int(m.group(2), 16)
        for m in re.finditer(r"#define\s+SENSOR_DRIVER_ID_(\w+)\s+(0x[0-9A-Fa-f]+)u", provider_src)
    }
    check(bool(c_driver_ids), "sensor_provider.h: at least one SENSOR_DRIVER_ID_* #define was found (parser sanity)")
    check(c_driver_ids == packer.DRIVER_IDS,
          f"pack_module_config.py's DRIVER_IDS matches sensor_provider.h's SENSOR_DRIVER_ID_* defines exactly "
          f"(C={c_driver_ids}, py={packer.DRIVER_IDS})")

    with open(channel_h, "r", encoding="utf-8") as f:
        channel_src = f.read()
    # Scope to just the sensor_channel_type_t enum body -- sensor_channel.h
    # also defines an unrelated SENSOR_CHANNEL_STATUS_* status enum whose
    # members would otherwise collide with this regex.
    type_enum_match = re.search(r"\}\s*sensor_channel_type_t\s*;", channel_src)
    check(type_enum_match is not None, "sensor_channel.h: sensor_channel_type_t enum found (parser sanity)")
    enum_body = channel_src[:type_enum_match.start()]
    enum_body = enum_body[enum_body.rfind("typedef enum"):]
    c_channel_bits = {
        m.group(1): int(m.group(2))
        for m in re.finditer(r"SENSOR_CHANNEL_(\w+)\s*=\s*(\d+)", enum_body)
    }
    check(bool(c_channel_bits), "sensor_channel.h: at least one SENSOR_CHANNEL_* enum value was found (parser sanity)")
    check(c_channel_bits == packer.CHANNEL_BITS,
          f"pack_module_config.py's CHANNEL_BITS matches sensor_channel.h's sensor_channel_type_t exactly "
          f"(C={c_channel_bits}, py={packer.CHANNEL_BITS})")

    # --- 5. Re-parse the ACTUAL GENERATED NVS IMAGES (not just pack_config()'s
    # in-memory bytes) and confirm the on-flash module_cfg:blob content
    # matches -- only runnable once IDF_PATH is set and tools/gen_config.sh
    # has produced build/cfg_images/*.bin (skipped otherwise, since a plain
    # host-test run has neither). This is the only check that would catch a
    # bug in the NVS CSV encoding step itself (see
    # pack_module_config.py's verify_module_cfg_blob_len() docstring for the
    # exact historical bug this class of check caught before). ---
    idf_path = os.environ.get("IDF_PATH", "")
    cfg_images_dir = os.path.join(ROOT_DIR, "build", "cfg_images")
    image_map = {
        "sht45": ("prototype_sht45.json", "prototype_sht45_config.bin"),
        "bh1750": ("prototype_bh1750.json", "prototype_bh1750_config.bin"),
        "sgp40": ("prototype_sgp40.json", "prototype_sgp40_config.bin"),
        "sgp41": ("prototype_sgp41.json", "prototype_sgp41_config.bin"),
        "pms5003": ("prototype_pms5003.json", "prototype_pms5003_config.bin"),
    }
    if idf_path and os.path.isdir(cfg_images_dir):
        for name, (cfg_name, image_name) in image_map.items():
            image_path = os.path.join(cfg_images_dir, image_name)
            if not os.path.isfile(image_path):
                check(False, f"{image_name} exists in build/cfg_images (run tools/gen_config.sh first)")
                continue
            cfg = load(cfg_name)
            expected_blob = packer.pack_config(cfg)
            actual_blob = packer.read_module_cfg_blob(image_path)
            check(actual_blob == expected_blob,
                  f"{image_name}: on-flash module_cfg:blob content matches pack_config({cfg_name}) exactly "
                  f"({len(actual_blob)} bytes re-parsed from the generated NVS image)")
            if name != "pms5003":
                got_downstream_detect_out = struct.unpack_from(">h", actual_blob, OFF_MODULE_DOWNSTREAM_DETECT_OUT)[0]
                check(got_downstream_detect_out == GPIO3,
                      f"{image_name}: downstream_port.detect_out_gpio in the generated NVS image is "
                      f"GPIO{GPIO3} (got GPIO{got_downstream_detect_out})")
            if name in ("sgp40", "sgp41"):
                got_driver_id = struct.unpack_from(">I", actual_blob, OFF_DRIVER_ID)[0]
                got_bitmap = struct.unpack_from(">H", actual_blob, OFF_SENSOR_BITMAP)[0]
                got_addr = struct.unpack_from(">H", actual_blob, OFF_I2C_ADDRESS)[0]
                got_sda, got_scl = struct.unpack_from(">hh", actual_blob, OFF_I2C_SDA_GPIO)
                check(got_driver_id == packer.resolve_driver_id(cfg["driver_id"]),
                      f"{image_name}: driver_id in the generated NVS image matches {cfg_name}")
                check(got_bitmap == packer.resolve_sensor_bitmap(cfg["sensor_type_bitmap"]),
                      f"{image_name}: sensor_type_bitmap in the generated NVS image matches {cfg_name}")
                check(got_addr == 0x59, f"{image_name}: i2c_address in the generated NVS image is 0x59")
                check((got_sda, got_scl) == (0, 1),
                      f"{image_name}: I2C SDA/SCL GPIO0/1 encoding in the generated NVS image is correct")
    else:
        print("skip: NVS-image-content re-parse checks (set IDF_PATH and run "
              "`bash tools/gen_config.sh build/cfg_images` first -- see this file's module docstring)")

    # --- 6. DOWNSTREAM DETECT_OUT GPIO22 -> GPIO3 migration (this follow-up):
    # every ACTIVE final-prototype config (the four I2C boards plus
    # configs/power_base.json) must now use GPIO3, GPIO22 must not appear
    # anywhere in any of their enabled port fields, UPSTREAM stays untouched
    # (DETECT_IN=12/DETECT_OUT=2, already checked in section 1), and
    # PMS5003's downstream stays fully disabled (-1, already checked in
    # section 3 -- unaffected by this migration since it was never GPIO22
    # or GPIO3 in the first place). ---
    check(power_base["downstream_port"]["detect_out_gpio"] == GPIO3,
          f"power_base.json: downstream_port.detect_out_gpio is GPIO{GPIO3}")
    for name, cfg in i2c_boards.items():
        check(cfg["downstream_port"]["detect_out_gpio"] == GPIO3,
              f"{name}: downstream_port.detect_out_gpio is GPIO{GPIO3}")

    # GPIO22 must not appear in any enabled port field of any of the five
    # active final-prototype configs (four I2C boards' upstream+downstream,
    # power_base's one downstream port). PMS5003's downstream is excluded
    # (all -1, an intentionally-disabled port, not a live GPIO22/GPIO3
    # candidate either way -- section 3 already confirms it stays disabled).
    port_gpio_fields = ("tx_gpio", "rx_gpio", "detect_in_gpio", "detect_out_gpio", "base_id_gpio")
    for name, cfg in i2c_boards.items():
        for port_name in ("upstream_port", "downstream_port"):
            port = cfg[port_name]
            used_gpio22 = [f for f in port_gpio_fields if f in port and packer.as_int(port[f]) == GPIO22]
            check(not used_gpio22,
                  f"{name}.{port_name}: GPIO{GPIO22} is not used by any field (found in: {used_gpio22})")
    used_gpio22_power_base = [f for f in port_gpio_fields if f in power_base["downstream_port"]
                               and packer.as_int(power_base["downstream_port"][f]) == GPIO22]
    check(not used_gpio22_power_base,
          f"power_base.json.downstream_port: GPIO{GPIO22} is not used by any field "
          f"(found in: {used_gpio22_power_base})")

    # In-memory packed-blob check (power_base_config uses a separate 44-byte
    # schema/packer -- components/power_base_config, tools/pack_power_base_config.py
    # -- not module_config's, so it needs its own pack_config() call and byte
    # offset rather than reusing `packer`'s).
    power_base_blob = power_base_packer.pack_config(power_base)
    check(len(power_base_blob) == 44, "power_base.json: packed blob is the expected 44 bytes")
    got_power_base_detect_out = struct.unpack_from(">h", power_base_blob, OFF_POWER_BASE_DOWNSTREAM_DETECT_OUT)[0]
    check(got_power_base_detect_out == GPIO3,
          f"power_base.json: packed blob downstream_port.detect_out_gpio is GPIO{GPIO3} "
          f"(got GPIO{got_power_base_detect_out})")

    # NVS-image-content re-parse for power_base_config.bin too (same
    # IDF_PATH/build/cfg_images gating as section 5 above).
    if idf_path and os.path.isdir(cfg_images_dir):
        power_base_image_path = os.path.join(cfg_images_dir, "power_base_config.bin")
        if os.path.isfile(power_base_image_path):
            actual_power_base_blob = read_cfg_blob_from_nvs_image(power_base_image_path, "power_base_cfg")
            check(actual_power_base_blob == power_base_blob,
                  f"power_base_config.bin: on-flash power_base_cfg:blob content matches "
                  f"pack_config(power_base.json) exactly ({len(actual_power_base_blob)} bytes re-parsed "
                  f"from the generated NVS image)")
            got = struct.unpack_from(">h", actual_power_base_blob, OFF_POWER_BASE_DOWNSTREAM_DETECT_OUT)[0]
            check(got == GPIO3,
                  f"power_base_config.bin: downstream_port.detect_out_gpio in the generated NVS image is "
                  f"GPIO{GPIO3} (got GPIO{got})")
        else:
            check(False, "power_base_config.bin exists in build/cfg_images (run tools/gen_power_base_config.sh first)")
    else:
        print("skip: power_base_config.bin NVS-image-content re-parse check (same IDF_PATH/build gating as above)")

    if failures == 0:
        print("ALL CHECKS PASSED")
        return 0
    print(f"{failures} CHECK(S) FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
