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

For a spare board that must wait safely for app-based registration, pass
--await-commissioning instead. This writes mode=2 with NO Dataset (so it is
never combined with --external-dataset-file). It may be combined with
--bridge-host/--bridge-port: the address is stored in "pb_bridge" as usual,
but the firmware starts the exporter only after the board has enrolled
(Joiner success, Thread attach, completion marker -- see docs/power-base.md).
The Linux host's Mesh-Local address is always passed explicitly by the
operator; nothing here defaults to a particular installation's address.

Optionally ALSO writes the "pb_bridge" namespace (components/
power_base_bridge_config -- this follow-up: Power Base -> Linux OTBR
northbound bridge transport, see docs/power-base.md) into the SAME
image/partition:

    python3 tools/pack_power_base_config.py configs/power_base.json out.bin \\
        --bridge-host fd11:22::1 --bridge-port 5684

Both --bridge-host and --bridge-port must be given together (or neither).
The address is validated the same way components/power_base_bridge_config's
own runtime loader does (rejects the unspecified address, multicast, and
link-local) -- see docs/power-base.md and
`sudo ot-ctl ipaddr mleid` on the Linux OTBR host for how to obtain a real
one. This is NOT a secret the way the Thread Dataset is, but it is still
never hardcoded here or in any test fixture -- always passed explicitly by
the operator. Omitting both flags (the default) writes "pb_bridge:enabled=0"
-- the bridge exporter stays disabled, same as a legacy image with no
"pb_bridge" namespace at all (see that component's own compatibility
policy).

Optionally ALSO writes the "pb_ble_auth" namespace (components/
power_base_ble_auth -- the per-board protocomm Security 2 salt/verifier
record for BLE enrollment, see docs/power-base.md "BLE 등록 전달 경로"):

    python3 tools/pack_power_base_config.py configs/power_base.json out.bin \
        --await-commissioning --bridge-host fd11:22::1 --bridge-port 5684 \
        --ble-auth-record-file /absolute/path/to/record.bin

Only together with --await-commissioning (BLE enrollment exists for mode 2
only). The record is a 420-byte file that tools/gen_power_base_ble_config.py
builds and passes here; it is never a command-line value, must not be
group/world-accessible, and its bytes are never printed. Use that tool
instead of calling this flag by hand: it also draws the board's setup code
and checks the result. The image is then written mode 0600.

Requires the ESP-IDF python environment (for nvs_partition_gen.py) --
source $IDF_PATH/export.sh first, same as for idf.py.
"""
import argparse
import ipaddress
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
PB_THREAD_MODE_AWAIT_COMMISSIONING = 2

# Must match components/power_base_bridge_config/include/power_base_bridge_config/
# power_base_bridge_config.h's own constants and wire layout.
PB_BRIDGE_NAMESPACE = "pb_bridge"
PB_BRIDGE_SCHEMA_VERSION = 1
PB_BRIDGE_BLOB_LEN = 24

# Must match components/power_base_ble_auth/include/power_base_ble_auth/
# power_base_ble_auth.h.
PB_BLE_AUTH_NAMESPACE = "pb_ble_auth"
PB_BLE_AUTH_KEY = "cred"
PB_BLE_AUTH_RECORD_LEN = 420

_HEX_ONLY_RE = re.compile(r"[0-9a-fA-F]+")


def read_ble_auth_record_file(path: str) -> bytes:
    """
    Reads the pb_ble_auth record (salt + verifier for one board) that
    tools/gen_power_base_ble_config.py prepared. Only its length is ever
    mentioned in a message. Refuses a missing file, a file that group or
    others can access, and any size other than PB_BLE_AUTH_RECORD_LEN --
    the record's own magic/CRC/EUI-64 are checked by the generator that made
    it and again by the firmware.
    """
    if not os.path.isfile(path):
        sys.exit(f"refusing to pack: BLE auth record file not found: {path}")
    if stat.S_IMODE(os.stat(path).st_mode) & 0o077:
        sys.exit("refusing to pack: BLE auth record file is accessible to group/others (expected mode 0600)")
    with open(path, "rb") as f:
        data = f.read()
    if len(data) != PB_BLE_AUTH_RECORD_LEN:
        sys.exit(f"refusing to pack: BLE auth record is {len(data)} bytes, expected {PB_BLE_AUTH_RECORD_LEN}")
    return data


def validate_bridge_host(addr_str: str) -> bytes:
    """
    Parses `addr_str` as an IPv6 address and returns its 16 raw bytes.
    Rejects a malformed address, the unspecified address (::), a multicast
    address (ff00::/8), and a link-local address (fe80::/10) -- the exact
    same policy components/power_base_bridge_config.c's own runtime loader
    enforces (see that component's own header comment for why link-local is
    rejected: no interface-index to disambiguate a link-local scope). Never
    accepts an IPv4 (or IPv4-mapped) literal -- this partition only stores a
    raw 16-byte value.
    """
    try:
        addr = ipaddress.IPv6Address(addr_str)
    except ValueError as e:
        sys.exit(f"refusing to pack: --bridge-host is not a valid IPv6 address: {e}")
    if addr.is_unspecified:
        sys.exit("refusing to pack: --bridge-host is the unspecified address (::)")
    if addr.is_multicast:
        sys.exit("refusing to pack: --bridge-host is a multicast address (ff00::/8)")
    if addr.is_link_local:
        sys.exit("refusing to pack: --bridge-host is a link-local address (fe80::/10) -- use the Linux OTBR "
                  "host's real Mesh-Local/backbone address (see `sudo ot-ctl ipaddr mleid`)")
    return addr.packed


def pack_pb_bridge_blob(enabled: bool, host_addr: bytes, port: int) -> bytes:
    """Matches power_base_bridge_config.c's CFG_BLOB_LEN=24 layout exactly:
    schema_version(4) + enabled(1) + reserved(1) + host_addr(16) + port(2)."""
    blob = struct.pack(">IBB", PB_BRIDGE_SCHEMA_VERSION, 1 if enabled else 0, 0) + host_addr + struct.pack(">H", port)
    if len(blob) != PB_BRIDGE_BLOB_LEN:
        raise AssertionError(f"internal error: packed pb_bridge blob is {len(blob)} bytes, expected "
                              f"{PB_BRIDGE_BLOB_LEN}")
    return blob


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


def verify_pb_thread_config(image_path: str, expected_mode: int, expected_tlvs_len: int,
                             expected_tlvs: bytes = None) -> None:
    """
    Re-parses the just-generated image and asserts the 'pb_thread' namespace
    matches what this run requested: mode always, and -- only when mode is
    EXTERNAL_OTBR -- that 'active_tlvs' is present and its
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

    if mode_val != expected_mode:
        sys.exit(f"verify_pb_thread_config: {PB_THREAD_NAMESPACE}:mode={mode_val} in {image_path}, "
                 f"expected {expected_mode}")

    if expected_mode != PB_THREAD_MODE_EXTERNAL_OTBR:
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


def verify_pb_bridge_config(image_path: str, expect_enabled: bool, expected_host_addr: bytes = None,
                             expected_port: int = None) -> None:
    """
    Re-parses the just-generated image and asserts the 'pb_bridge' namespace
    matches what this run requested. Not a secret the way Dataset TLVs are,
    but still never logs the raw address bytes -- only a match/mismatch
    outcome -- to keep this script's output policy uniform across every
    namespace it writes.
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
    blob_entry = None
    crc_val = None
    for page in partition.pages:
        for entry in page.entries:
            if entry.key == PB_BRIDGE_NAMESPACE and entry.metadata["namespace"] == 0 and entry.data:
                namespace_id = entry.data.get("value")
            elif namespace_id is not None and entry.metadata["namespace"] == namespace_id and entry.data:
                if entry.key == "blob" and entry.metadata["type"] == "blob_data":
                    blob_entry = entry
                elif entry.key == "crc":
                    crc_val = entry.data.get("value")

    if namespace_id is None:
        sys.exit(f"verify_pb_bridge_config: '{PB_BRIDGE_NAMESPACE}' namespace not found in {image_path}")
    if blob_entry is None or crc_val is None:
        sys.exit(f"verify_pb_bridge_config: '{PB_BRIDGE_NAMESPACE}:blob'/'crc' not found in {image_path}")

    raw_bytes = bytearray()
    for child in blob_entry.children:
        raw_bytes += child.raw
    actual_blob = bytes(raw_bytes[:PB_BRIDGE_BLOB_LEN])
    if len(actual_blob) != PB_BRIDGE_BLOB_LEN:
        sys.exit(f"verify_pb_bridge_config: {PB_BRIDGE_NAMESPACE}:blob is {len(actual_blob)} bytes in "
                 f"{image_path}, expected {PB_BRIDGE_BLOB_LEN}")
    if crc16_ccitt(actual_blob) != crc_val:
        sys.exit(f"verify_pb_bridge_config: {PB_BRIDGE_NAMESPACE}:crc does not match the stored blob's own CRC in "
                 f"{image_path}")

    actual_enabled = actual_blob[4] != 0
    if actual_enabled != expect_enabled:
        sys.exit(f"verify_pb_bridge_config: {PB_BRIDGE_NAMESPACE}:enabled={actual_enabled} in {image_path}, "
                 f"expected {expect_enabled}")
    if expect_enabled:
        actual_addr = actual_blob[6:22]
        actual_port = struct.unpack(">H", actual_blob[22:24])[0]
        if expected_host_addr is not None and actual_addr != expected_host_addr:
            sys.exit(f"verify_pb_bridge_config: {PB_BRIDGE_NAMESPACE}:host_addr content mismatch in {image_path} "
                     f"(content itself never printed)")
        if expected_port is not None and actual_port != expected_port:
            sys.exit(f"verify_pb_bridge_config: {PB_BRIDGE_NAMESPACE}:port={actual_port} in {image_path}, "
                     f"expected {expected_port}")


def verify_pb_ble_auth(image_path: str, expected_record: bytes = None) -> None:
    """
    Re-parses the image and asserts the 'pb_ble_auth' namespace matches this
    run: absent when expected_record is None, otherwise 'cred' present with
    exactly those bytes. Never prints record bytes.
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
    cred_entry = None
    for page in partition.pages:
        for entry in page.entries:
            if entry.key == PB_BLE_AUTH_NAMESPACE and entry.metadata["namespace"] == 0 and entry.data:
                namespace_id = entry.data.get("value")
            elif (namespace_id is not None and entry.metadata["namespace"] == namespace_id and entry.data
                  and entry.key == PB_BLE_AUTH_KEY and entry.metadata["type"] == "blob_data"):
                cred_entry = entry

    if expected_record is None:
        if namespace_id is not None:
            sys.exit(f"verify_pb_ble_auth: unexpected '{PB_BLE_AUTH_NAMESPACE}' namespace in {image_path}")
        return
    if namespace_id is None or cred_entry is None:
        sys.exit(f"verify_pb_ble_auth: '{PB_BLE_AUTH_NAMESPACE}:{PB_BLE_AUTH_KEY}' not found in {image_path}")
    actual_len = cred_entry.data.get("size")
    if actual_len != len(expected_record):
        sys.exit(f"verify_pb_ble_auth: {PB_BLE_AUTH_NAMESPACE}:{PB_BLE_AUTH_KEY} is {actual_len} bytes in "
                 f"{image_path}, expected {len(expected_record)} (content itself never printed)")
    raw_bytes = bytearray()
    for child in cred_entry.children:
        raw_bytes += child.raw
    if bytes(raw_bytes[:actual_len]) != expected_record:
        sys.exit(f"verify_pb_ble_auth: {PB_BLE_AUTH_NAMESPACE}:{PB_BLE_AUTH_KEY} content mismatch in {image_path} "
                 f"(content itself never printed)")


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
    ap.add_argument("--await-commissioning", action="store_true",
                    help="Prepare a spare Power Base without Thread credentials. Writes pb_thread:mode=2 and "
                         "no Dataset; Thread, the exporter and downstream services stay disabled until the board "
                         "enrolls. Mutually exclusive with --external-dataset-file; may be combined with "
                         "--bridge-host/--bridge-port.")
    ap.add_argument("--bridge-host", default=None, metavar="IPV6_ADDR",
                     help="The Linux OTBR host's real IPv6 address (e.g. from `sudo ot-ctl ipaddr mleid` on that "
                          "host) -- writes pb_bridge:enabled=1 with this address. Must be given together with "
                          "--bridge-port. See this script's own module docstring.")
    ap.add_argument("--bridge-port", type=int, default=None, metavar="PORT",
                     help="The Linux OTBR host's CoAP ingest port (see linux/matter_bridge/ingest, default 5684 "
                          "in that program). Must be given together with --bridge-host.")
    ap.add_argument("--ble-auth-record-file", default=None, metavar="PATH",
                    help="Path to a file (NEVER a literal value) holding the 420-byte pb_ble_auth record for ONE "
                         "board, as prepared by tools/gen_power_base_ble_config.py. Only with "
                         "--await-commissioning. Its bytes are never printed.")
    args = ap.parse_args()

    external_tlvs = None
    if args.await_commissioning and args.external_dataset_file:
        sys.exit("refusing to pack: --await-commissioning and --external-dataset-file are mutually exclusive")
    ble_auth_record = None
    if args.ble_auth_record_file:
        if not args.await_commissioning:
            sys.exit("refusing to pack: --ble-auth-record-file is only for --await-commissioning images")
        ble_auth_record = read_ble_auth_record_file(args.ble_auth_record_file)
    if args.external_dataset_file:
        external_tlvs = read_external_dataset_hex_file(args.external_dataset_file)

    if (args.bridge_host is None) != (args.bridge_port is None):
        sys.exit("refusing to pack: --bridge-host and --bridge-port must be given together (or neither)")
    bridge_host_bytes = None
    if args.bridge_host is not None:
        bridge_host_bytes = validate_bridge_host(args.bridge_host)
        if not (0 < args.bridge_port <= 0xFFFF):
            sys.exit(f"refusing to pack: --bridge-port={args.bridge_port} is out of range (1..65535)")

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
            elif args.await_commissioning:
                f.write(f"mode,data,u8,{PB_THREAD_MODE_AWAIT_COMMISSIONING}\n")
            else:
                f.write(f"mode,data,u8,{PB_THREAD_MODE_STANDALONE}\n")

            f.write(f"{PB_BRIDGE_NAMESPACE},namespace,,\n")
            pb_bridge_blob = pack_pb_bridge_blob(bridge_host_bytes is not None, bridge_host_bytes or bytes(16),
                                                  args.bridge_port or 0)
            pb_bridge_crc = crc16_ccitt(pb_bridge_blob)
            pb_bridge_blob_path = os.path.join(tmp, "pb_bridge_blob.bin")
            with open(pb_bridge_blob_path, "wb") as bf:
                bf.write(pb_bridge_blob)
            f.write(f"blob,file,binary,{pb_bridge_blob_path}\n")
            f.write(f"crc,data,u16,{pb_bridge_crc}\n")

            if ble_auth_record is not None:
                # A private copy inside the 0700 temp directory; never logged.
                ble_auth_path = os.path.join(tmp, "pb_ble_auth_cred.bin")
                fd = os.open(ble_auth_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
                with os.fdopen(fd, "wb") as af:
                    af.write(ble_auth_record)
                f.write(f"{PB_BLE_AUTH_NAMESPACE},namespace,,\n")
                f.write(f"{PB_BLE_AUTH_KEY},file,binary,{ble_auth_path}\n")

        tool = find_nvs_partition_gen()
        out_path = os.path.abspath(args.output_bin)
        os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
        subprocess.run(
            [sys.executable, tool, "generate", csv_path, out_path, str(args.size)],
            check=True,
        )

    verify_power_base_cfg_blob_len(out_path, len(blob))
    expected_mode = (PB_THREAD_MODE_EXTERNAL_OTBR if external_tlvs is not None else
                     PB_THREAD_MODE_AWAIT_COMMISSIONING if args.await_commissioning else
                     PB_THREAD_MODE_STANDALONE)
    verify_pb_thread_config(out_path, expected_mode=expected_mode,
                             expected_tlvs_len=(len(external_tlvs) if external_tlvs is not None else 0),
                             expected_tlvs=external_tlvs)
    verify_pb_bridge_config(out_path, expect_enabled=(bridge_host_bytes is not None),
                             expected_host_addr=bridge_host_bytes, expected_port=args.bridge_port)
    verify_pb_ble_auth(out_path, expected_record=ble_auth_record)
    if os.path.getsize(out_path) != args.size:
        sys.exit(f"generated image is {os.path.getsize(out_path)} bytes, expected the cfg partition size "
                 f"0x{args.size:x}")

    bridge_note = (f"pb_bridge:enabled=1 port={args.bridge_port}" if bridge_host_bytes is not None
                   else "pb_bridge:enabled=0")
    if external_tlvs is not None:
        # A real secret (contains the Network Key) -- the generated image
        # itself must not be group/world-readable (see docs/power-base.md).
        os.chmod(out_path, stat.S_IRUSR | stat.S_IWUSR)
        print(f"wrote {args.output_bin} (blob_crc=0x{crc:04x}, verified power_base_cfg:blob={len(blob)} bytes, "
              f"pb_thread:mode=EXTERNAL_OTBR, pb_thread:active_tlvs={len(external_tlvs)} bytes [content never "
              f"printed], {bridge_note}, file mode=0600)")
    elif args.await_commissioning:
        ble_note = ""
        if ble_auth_record is not None:
            # The verifier lets someone test setup-code guesses offline.
            os.chmod(out_path, stat.S_IRUSR | stat.S_IWUSR)
            ble_note = (f", pb_ble_auth:cred={len(ble_auth_record)} bytes [content never printed], "
                        f"file mode=0600")
        print(f"wrote {args.output_bin} (blob_crc=0x{crc:04x}, verified power_base_cfg:blob={len(blob)} bytes, "
              f"pb_thread:mode=AWAIT_COMMISSIONING, no Dataset, {bridge_note}"
              f"{' [exporter starts only after enrollment]' if bridge_host_bytes is not None else ''}{ble_note})")
    else:
        print(f"wrote {args.output_bin} (blob_crc=0x{crc:04x}, verified power_base_cfg:blob={len(blob)} bytes, "
              f"pb_thread:mode=STANDALONE, {bridge_note})")


if __name__ == "__main__":
    main()
