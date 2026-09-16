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

Optionally also writes the "pb_thread" namespace (components/
power_base_thread_config -- see docs/power-base.md's "external OTBR Dataset"
section) into the SAME image/partition, alongside the unchanged
"power_base_cfg" namespace/blob format above:

    python3 tools/pack_power_base_config.py configs/power_base.json out.bin \\
        --external-dataset-file /absolute/path/to/otbr-active-dataset.hex

--external-dataset-file must be a FILE PATH, never a command-line value --
the file's content is the OTBR's real Active Dataset TLVs as hex text (e.g.
`ot-ctl dataset active -x` output with the trailing "Done" line removed).
This tool never logs, prints, or otherwise reveals that hex/byte content
anywhere (stdout, stderr, or exceptions) -- only its decoded LENGTH ever
appears in any message. When this flag is omitted, "pb_thread:mode" is
written as STANDALONE(0) and no "active_tlvs" key is written at all -- this
is the pre-existing, secret-free default and is what a plain
`tools/gen_power_base_config.sh` invocation still produces.

Requires the ESP-IDF python environment (for nvs_partition_gen.py) --
source $IDF_PATH/export.sh first, same as for idf.py.
"""
import argparse
import json
import os
import re
import stat
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pack_module_config import as_int, check_gpio, crc16_ccitt, find_nvs_partition_gen  # noqa: E402

CFG_PARTITION_SIZE = 0x3000  # must match power_base/partitions.csv's "cfg" partition size

# Must match components/power_base_thread_config/include/power_base_thread_config/
# power_base_thread_config.h's POWER_BASE_THREAD_CONFIG_MAX_TLV_LEN, which in turn
# must match OpenThread's OT_OPERATIONAL_DATASET_MAX_LENGTH (openthread/dataset.h).
OT_OPERATIONAL_DATASET_MAX_LENGTH = 254

PB_THREAD_NAMESPACE = "pb_thread"
PB_THREAD_MODE_STANDALONE = 0
PB_THREAD_MODE_EXTERNAL_OTBR = 1

_HEX_ONLY_RE = re.compile(r"[0-9a-fA-F]+")


def read_external_dataset_hex_file(path: str) -> bytes:
    """
    Reads an OTBR Active Dataset TLV hex file and returns the decoded raw
    TLV bytes. NEVER logs, prints, or includes the hex text or decoded bytes
    in any exception/exit message below -- only lengths. The caller (main())
    must likewise never print the return value.

    Expected content: `ot-ctl dataset active -x` output with the trailing
    "Done" line already removed by the operator (see docs/power-base.md).
    Multiple lines and surrounding whitespace are handled safely (all
    whitespace is stripped before validation), but this deliberately does
    NOT try to strip specific known-bad substrings like "Done"/"Error" --
    instead, ANY non-hex-digit character anywhere in the file (which such
    stray CLI text always contains) is rejected outright.

    Rejects: a missing file, an empty file, any non-hex-digit character, an
    odd-length hex string, 0 decoded bytes, and more than
    OT_OPERATIONAL_DATASET_MAX_LENGTH decoded bytes -- the same size policy
    components/power_base_thread_config/power_base_thread_config.c enforces
    again at load time, and thread_manager_start_as_always_on_ftd_with_dataset()
    enforces a third time via OpenThread's own otDatasetIsValid() (this tool
    never attempts to imitate that full semantic validation -- see
    docs/power-base.md).
    """
    if not os.path.isfile(path):
        sys.exit(f"refusing to pack: external dataset file not found: {path}")

    try:
        with open(path, "r", encoding="utf-8") as f:
            raw_text = f.read()
    except UnicodeDecodeError:
        sys.exit("refusing to pack: external dataset file is not valid UTF-8 text "
                  "(expected plain hex text, e.g. `ot-ctl dataset active -x` output)")

    compact = "".join(raw_text.split())  # strips all whitespace/newlines, handles multi-line input safely
    if not compact:
        sys.exit("refusing to pack: external dataset file is empty")
    if not _HEX_ONLY_RE.fullmatch(compact):
        sys.exit("refusing to pack: external dataset file contains non-hex-digit characters -- check for stray "
                  "text such as \"Done\"/\"Error\" left over from `ot-ctl dataset active -x` output (this message "
                  "never includes the file's actual content)")
    if len(compact) % 2 != 0:
        sys.exit(f"refusing to pack: external dataset hex string has odd length ({len(compact)} hex digits)")

    data = bytes.fromhex(compact)
    if len(data) == 0:
        sys.exit("refusing to pack: external dataset decodes to 0 bytes")
    if len(data) > OT_OPERATIONAL_DATASET_MAX_LENGTH:
        sys.exit(f"refusing to pack: external dataset is {len(data)} bytes, exceeds "
                 f"OT_OPERATIONAL_DATASET_MAX_LENGTH={OT_OPERATIONAL_DATASET_MAX_LENGTH}")
    return data


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


def verify_pb_thread_config(image_path: str, expect_external: bool, expected_tlvs_len: int,
                             expected_tlvs: bytes = None) -> None:
    """
    Re-parses the just-generated image and asserts the 'pb_thread' namespace
    matches what this run requested: mode (STANDALONE/EXTERNAL_OTBR) always,
    and -- only when expect_external -- that 'active_tlvs' is present and its
    on-the-wire byte content is EXACTLY expected_tlvs (length AND bytes).

    NEVER logs/prints expected_tlvs or the re-parsed active_tlvs bytes
    themselves -- only lengths and a boolean match/mismatch outcome, same
    secrecy policy as read_external_dataset_hex_file() above.
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
    mode_val = None
    tlv_entry = None
    for page in partition.pages:
        for entry in page.entries:
            if entry.key == PB_THREAD_NAMESPACE and entry.metadata["namespace"] == 0 and entry.data:
                namespace_id = entry.data.get("value")
            elif namespace_id is not None and entry.metadata["namespace"] == namespace_id and entry.data:
                if entry.key == "mode":
                    mode_val = entry.data.get("value")
                elif entry.key == "active_tlvs" and entry.metadata["type"] == "blob_data":
                    tlv_entry = entry

    if namespace_id is None:
        sys.exit(f"verify_pb_thread_config: '{PB_THREAD_NAMESPACE}' namespace not found in {image_path}")
    if mode_val is None:
        sys.exit(f"verify_pb_thread_config: '{PB_THREAD_NAMESPACE}:mode' not found in {image_path}")

    expected_mode = PB_THREAD_MODE_EXTERNAL_OTBR if expect_external else PB_THREAD_MODE_STANDALONE
    if mode_val != expected_mode:
        sys.exit(f"verify_pb_thread_config: {PB_THREAD_NAMESPACE}:mode={mode_val} in {image_path}, "
                 f"expected {expected_mode}")

    if not expect_external:
        if tlv_entry is not None:
            sys.exit(f"verify_pb_thread_config: a non-external image unexpectedly contains an "
                     f"'active_tlvs' blob in {image_path}")
        return

    if tlv_entry is None:
        sys.exit(f"verify_pb_thread_config: EXTERNAL_OTBR mode but no 'active_tlvs' blob found in {image_path}")
    actual_len = tlv_entry.data.get("size")
    if actual_len != expected_tlvs_len:
        sys.exit(f"verify_pb_thread_config: {PB_THREAD_NAMESPACE}:active_tlvs is {actual_len} bytes in "
                 f"{image_path}, expected {expected_tlvs_len} (content itself never printed)")
    if expected_tlvs is not None:
        raw_bytes = bytearray()
        for child in tlv_entry.children:
            raw_bytes += child.raw
        actual = bytes(raw_bytes[:actual_len])
        if actual != expected_tlvs:
            sys.exit(f"verify_pb_thread_config: {PB_THREAD_NAMESPACE}:active_tlvs content mismatch in "
                     f"{image_path} (same length ({actual_len} bytes) but different bytes -- content itself "
                     f"never printed)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("config_json", help="e.g. configs/power_base.json")
    ap.add_argument("output_bin", help="e.g. power_base_config.bin")
    ap.add_argument("--size", type=lambda s: int(s, 0), default=CFG_PARTITION_SIZE,
                     help=f"cfg partition size in bytes (default 0x{CFG_PARTITION_SIZE:x}, "
                          f"must match power_base/partitions.csv)")
    ap.add_argument("--external-dataset-file", default=None, metavar="PATH",
                     help="Path to a file (NEVER a literal value) containing the OTBR's Active Dataset TLVs as "
                          "hex text -- see this script's own module docstring. When given, writes "
                          "pb_thread:mode=EXTERNAL_OTBR(1) and pb_thread:active_tlvs=<these exact bytes>; when "
                          "omitted (default), writes pb_thread:mode=STANDALONE(0) and no active_tlvs key at all.")
    args = ap.parse_args()

    external_tlvs = None
    if args.external_dataset_file:
        external_tlvs = read_external_dataset_hex_file(args.external_dataset_file)

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
        # tempfile.mkdtemp()/TemporaryDirectory() already creates this directory
        # mode 0700 (owner-only) on POSIX -- restated here defensively since the
        # external Dataset blob (when present) is written inside it.
        os.chmod(tmp, stat.S_IRWXU)

        blob_path = os.path.join(tmp, "blob.bin")
        with open(blob_path, "wb") as f:
            f.write(blob)

        csv_path = os.path.join(tmp, "power_base_cfg.csv")
        with open(csv_path, "w", encoding="utf-8") as f:
            f.write("key,type,encoding,value\n")
            f.write("power_base_cfg,namespace,,\n")
            f.write(f"blob,file,binary,{blob_path}\n")
            f.write(f"crc,data,u16,{crc}\n")
            f.write(f"{PB_THREAD_NAMESPACE},namespace,,\n")
            if external_tlvs is not None:
                f.write(f"mode,data,u8,{PB_THREAD_MODE_EXTERNAL_OTBR}\n")
                tlv_path = os.path.join(tmp, "active_tlvs.bin")
                # Written with the umask-independent 0600 mode directly, and
                # never logged -- see read_external_dataset_hex_file()'s own
                # secrecy comment.
                fd = os.open(tlv_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
                with os.fdopen(fd, "wb") as tf:
                    tf.write(external_tlvs)
                f.write(f"active_tlvs,file,binary,{tlv_path}\n")
            else:
                f.write(f"mode,data,u8,{PB_THREAD_MODE_STANDALONE}\n")

        tool = find_nvs_partition_gen()
        out_path = os.path.abspath(args.output_bin)
        os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
        subprocess.run(
            [sys.executable, tool, "generate", csv_path, out_path, str(args.size)],
            check=True,
        )

    verify_power_base_cfg_blob_len(out_path, len(blob))
    verify_pb_thread_config(out_path, expect_external=(external_tlvs is not None),
                             expected_tlvs_len=(len(external_tlvs) if external_tlvs is not None else 0),
                             expected_tlvs=external_tlvs)

    if external_tlvs is not None:
        # A real secret (contains the Network Key) -- the generated image
        # itself must not be group/world-readable (see docs/power-base.md).
        os.chmod(out_path, stat.S_IRUSR | stat.S_IWUSR)
        print(f"wrote {args.output_bin} (blob_crc=0x{crc:04x}, verified power_base_cfg:blob={len(blob)} bytes, "
              f"pb_thread:mode=EXTERNAL_OTBR, pb_thread:active_tlvs={len(external_tlvs)} bytes [content never "
              f"printed], file mode=0600)")
    else:
        print(f"wrote {args.output_bin} (blob_crc=0x{crc:04x}, verified power_base_cfg:blob={len(blob)} bytes, "
              f"pb_thread:mode=STANDALONE)")


if __name__ == "__main__":
    main()
