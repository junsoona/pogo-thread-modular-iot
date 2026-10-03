#!/usr/bin/env python3
"""
Prepares ONE spare Power Base for BLE enrollment (pb_thread:mode=2 with the
BLE transport -- docs/power-base.md "BLE 등록 전달 경로").

It writes one cfg image for exactly one board:

    power_base_cfg   board GPIO/timeouts from configs/power_base.json (unchanged)
    pb_thread        mode=2 (AWAIT_COMMISSIONING), NO Thread Dataset
    pb_bridge        the Linux bridge's IPv6 address and CoAP port you pass
    pb_ble_auth      protocomm Security 2 salt + verifier for this board's
                     EUI-64 (components/power_base_ble_auth)

and one setup-code file for the person who will enroll that board.

The setup code
  - is 128 random bits from the operating system's CSPRNG (`secrets`),
    written as 26 characters of RFC 4648 base32 (A-Z, 2-7), upper case, no
    padding or separators;
  - is the Security 2 password. The username is the board's EUI-64 as 16
    lower-case hex digits;
  - is never printed, never put on a command line or in the environment, and
    never stored in the image. The image holds only the salt and verifier
    that ESP-IDF's own SRP-6a code derives from it;
  - goes into the file named by --setup-code-file, created with mode 0600.
    That file must not exist yet and must be outside this repository. To
    rebuild the image for a code you already handed out, pass
    --existing-setup-code-file instead (it must be mode 0600); nothing is
    ever overwritten silently.

The salt/verifier are computed by ESP-IDF's official client-side SRP-6a
implementation (examples/protocols/esp_local_ctrl/scripts/security/srp6a.py:
RFC 5054 3072-bit group, g = 5, SHA-512), not by code written for this
project, and cross-checked here against the textbook formula.

Requires the ESP-IDF environment (nvs_partition_gen.py, nvs_parser, srp6a.py):
    source ~/.espressif/v6.1/esp-idf/export.sh

Usage (every value is yours; nothing is defaulted to a particular home):
    python tools/gen_power_base_ble_config.py \\
        --eui64 <16 hex digits of THIS board> \\
        --bridge-host <Linux OTBR mesh-local address> --bridge-port <ingest port> \\
        --setup-code-file "$HOME/pogo-setup-codes/<eui64>.setup-code" \\
        [--out-dir build/cfg_images]

Output: <out-dir>/power_base_ble_<eui64>_config.bin (mode 0600).
"""
import argparse
import base64
import hashlib
import importlib.util
import os
import re
import secrets
import stat
import subprocess
import sys
import tempfile
import zlib

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(TOOLS_DIR)
sys.path.insert(0, TOOLS_DIR)
import pack_power_base_config as pack  # noqa: E402

# Must match components/power_base_ble_auth/include/power_base_ble_auth/power_base_ble_auth.h.
RECORD_MAGIC = b"PBA1"
RECORD_FORMAT_VERSION = 1
RECORD_SRP_GROUP_SEC2_3072_SHA512 = 1
SALT_LEN = 16
VERIFIER_LEN = 384
RECORD_LEN = 420

SETUP_CODE_BYTES = 16  # 128 bits
SETUP_CODE_RE = re.compile(r"[A-Z2-7]{26}")
EUI64_RE = re.compile(r"[0-9a-fA-F]{16}")


def die(msg: str) -> None:
    sys.exit(f"refusing to prepare: {msg}")


def normalize_eui64(text: str) -> str:
    if not EUI64_RE.fullmatch(text):
        die("--eui64 must be exactly 16 hex digits (the board's EUI-64, e.g. from `esptool read-mac`)")
    eui = text.lower()
    if eui in ("0" * 16, "f" * 16):
        die("--eui64 is all zeros or all ones, not a real EUI-64")
    return eui


def load_official_srp6a():
    """ESP-IDF's own SRP-6a client code, as shipped with esp_local_ctrl/esp_prov."""
    idf_path = os.environ.get("IDF_PATH", "")
    scripts = os.path.join(idf_path, "examples", "protocols", "esp_local_ctrl", "scripts")
    path = os.path.join(scripts, "security", "srp6a.py")
    if not os.path.isfile(path):
        sys.exit(f"cannot find ESP-IDF's srp6a.py at {path} -- source the ESP-IDF v6.1 export.sh first")
    if scripts not in sys.path:
        sys.path.insert(0, scripts)  # srp6a.py imports its sibling package `utils`
    spec = importlib.util.spec_from_file_location("pb_idf_srp6a", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def is_inside_repo(path: str) -> bool:
    real = os.path.realpath(path)
    root = os.path.realpath(ROOT_DIR)
    return real == root or real.startswith(root + os.sep)


def new_setup_code() -> str:
    code = base64.b32encode(secrets.token_bytes(SETUP_CODE_BYTES)).decode("ascii").rstrip("=")
    if not SETUP_CODE_RE.fullmatch(code):
        raise AssertionError("internal error: generated setup code has an unexpected shape")
    return code


def read_existing_setup_code(path: str) -> str:
    """Never echoes the file's content."""
    if not os.path.isfile(path):
        die(f"--existing-setup-code-file not found: {path}")
    st = os.stat(path)
    if stat.S_IMODE(st.st_mode) & 0o077:
        die("--existing-setup-code-file is accessible to group/others (expected mode 0600)")
    with open(path, "r", encoding="ascii", errors="strict") as f:
        code = f.read().strip()
    if not SETUP_CODE_RE.fullmatch(code):
        die("--existing-setup-code-file does not hold a setup code made by this tool "
            "(26 characters A-Z/2-7; its content is not shown)")
    padded = code + "=" * 6
    if base64.b32encode(base64.b32decode(padded)).decode("ascii") != padded:
        die("--existing-setup-code-file holds a non-canonical setup code (its content is not shown)")
    return code


def make_salt_verifier(srp6a, username: str, code: str):
    """Official generation, repeated until the salt is a full 16 bytes: the
    official generator drops leading zero bytes, and the firmware stores a
    fixed-size salt that clients hash exactly as received."""
    for _ in range(64):
        salt, verifier = srp6a.generate_salt_and_verifier(username, code, len_s=SALT_LEN)
        if len(salt) == SALT_LEN and len(verifier) <= VERIFIER_LEN:
            break
    else:
        raise AssertionError("internal error: could not draw a 16-byte salt")

    # Independent check against the formula in ESP-IDF's esp_srp.h:
    #   x = H(salt | H(username ":" password)),  v = g^x mod N
    n, g = srp6a.get_ng(srp6a.NG_3072)
    inner = hashlib.sha512(username.encode() + b":" + code.encode()).digest()
    x = int.from_bytes(hashlib.sha512(salt + inner).digest(), "big")
    v = pow(g, x, n)
    if g != 5 or n.bit_length() != 3072 or int.from_bytes(verifier, "big") != v or v in (0, 1):
        raise AssertionError("internal error: verifier does not match the Security 2 formula")
    return salt, verifier.rjust(VERIFIER_LEN, b"\x00")


def build_record(eui64_hex: str, salt: bytes, verifier: bytes) -> bytes:
    body = (RECORD_MAGIC + bytes([RECORD_FORMAT_VERSION, RECORD_SRP_GROUP_SEC2_3072_SHA512, SALT_LEN, 0]) +
            bytes.fromhex(eui64_hex) + salt + verifier)
    record = body + zlib.crc32(body).to_bytes(4, "little")
    if len(record) != RECORD_LEN:
        raise AssertionError(f"internal error: record is {len(record)} bytes, expected {RECORD_LEN}")
    return record


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--eui64", required=True, help="This board's EUI-64, 16 hex digits.")
    ap.add_argument("--bridge-host", required=True, metavar="IPV6_ADDR",
                    help="The Linux OTBR host's mesh-local address (`sudo ot-ctl ipaddr mleid` on that host).")
    ap.add_argument("--bridge-port", required=True, type=int, metavar="PORT",
                    help="The Linux bridge's CoAP ingest port.")
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--setup-code-file", metavar="PATH",
                       help="Where to write the NEW setup code (mode 0600). Must not exist; must be outside this "
                            "repository.")
    group.add_argument("--existing-setup-code-file", metavar="PATH",
                       help="Reuse the setup code in this file (mode 0600) instead of drawing a new one.")
    ap.add_argument("--out-dir", default=os.path.join(ROOT_DIR, "build", "cfg_images"),
                    help="Directory for the cfg image (default: build/cfg_images).")
    ap.add_argument("--config", default=os.path.join(ROOT_DIR, "configs", "power_base.json"),
                    help="Board config JSON (default: configs/power_base.json).")
    args = ap.parse_args()

    if not os.environ.get("IDF_PATH"):
        sys.exit("IDF_PATH is not set -- source the ESP-IDF v6.1 export.sh first")

    eui64 = normalize_eui64(args.eui64)
    pack.validate_bridge_host(args.bridge_host)
    if not (0 < args.bridge_port <= 0xFFFF):
        die(f"--bridge-port={args.bridge_port} is out of range (1..65535)")

    code_path = args.setup_code_file or args.existing_setup_code_file
    if is_inside_repo(code_path):
        die("the setup-code file must be outside this repository (e.g. a 0700 directory in your home)")
    out_dir = os.path.abspath(args.out_dir)
    image_path = os.path.join(out_dir, f"power_base_ble_{eui64}_config.bin")
    # nvs_partition_gen.py only writes names ending in ".bin".
    partial_path = os.path.join(out_dir, f"power_base_ble_{eui64}_config.partial.bin")
    if args.setup_code_file and os.path.exists(image_path):
        die(f"an image for this EUI-64 already exists: {image_path}. A new setup code would no longer match it. "
            f"Rebuild it for the code you already have with --existing-setup-code-file, or remove that image "
            f"yourself first -- nothing was changed")

    srp6a = load_official_srp6a()

    created_code_file = False
    if args.setup_code_file:
        code = new_setup_code()
        parent = os.path.dirname(os.path.abspath(code_path))
        if not os.path.isdir(parent):
            die(f"the directory for --setup-code-file does not exist: {parent}")
        try:
            # O_EXCL: an existing code file is never replaced.
            fd = os.open(code_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            die(f"--setup-code-file already exists: {code_path} (use --existing-setup-code-file to reuse that "
                f"code, or choose another path -- nothing was changed)")
        with os.fdopen(fd, "w", encoding="ascii") as f:
            f.write(code + "\n")
        os.chmod(code_path, 0o600)
        created_code_file = True
    else:
        code = read_existing_setup_code(code_path)

    ok = False
    try:
        salt, verifier = make_salt_verifier(srp6a, eui64, code)
        record = build_record(eui64, salt, verifier)
        os.makedirs(out_dir, exist_ok=True)
        with tempfile.TemporaryDirectory() as tmp:
            os.chmod(tmp, stat.S_IRWXU)
            record_path = os.path.join(tmp, "pb_ble_auth_cred.bin")
            fd = os.open(record_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "wb") as f:
                f.write(record)
            result = subprocess.run(
                [sys.executable, os.path.join(TOOLS_DIR, "pack_power_base_config.py"), args.config, partial_path,
                 "--await-commissioning", "--bridge-host", args.bridge_host, "--bridge-port", str(args.bridge_port),
                 "--ble-auth-record-file", record_path],
                capture_output=True, text=True)
        if result.returncode != 0:
            sys.stderr.write(result.stdout + result.stderr)
            sys.exit("image generation failed -- nothing usable was written")

        # Read the finished image back with ESP-IDF's own NVS parser.
        import ipaddress
        pack.verify_pb_thread_config(partial_path, expected_mode=pack.PB_THREAD_MODE_AWAIT_COMMISSIONING,
                                     expected_tlvs_len=0)
        pack.verify_pb_bridge_config(partial_path, expect_enabled=True,
                                     expected_host_addr=ipaddress.IPv6Address(args.bridge_host).packed,
                                     expected_port=args.bridge_port)
        pack.verify_pb_ble_auth(partial_path, expected_record=record)
        if os.path.getsize(partial_path) != pack.CFG_PARTITION_SIZE:
            sys.exit(f"generated image is {os.path.getsize(partial_path)} bytes, expected "
                     f"0x{pack.CFG_PARTITION_SIZE:x}")
        with open(partial_path, "rb") as f:
            if code.encode("ascii") in f.read():
                raise AssertionError("internal error: the setup code appears in the image")
        os.chmod(partial_path, 0o600)
        os.replace(partial_path, image_path)  # only a fully checked image gets the final name
        ok = True
    finally:
        if not ok:
            # Never leave a new code without its image, or a half-made image.
            if created_code_file and os.path.exists(code_path):
                os.remove(code_path)
            if os.path.exists(partial_path):
                os.remove(partial_path)

    print(f"wrote {image_path} ({os.path.getsize(image_path)} bytes, mode 0600): power_base_cfg + "
          f"pb_thread:mode=AWAIT_COMMISSIONING (no Dataset) + pb_bridge:enabled=1 port={args.bridge_port} + "
          f"pb_ble_auth for EUI-64 {eui64} (salt/verifier never printed)")
    print(f"setup code: {'written to' if created_code_file else 'reused from'} {code_path} (mode 0600) -- not "
          f"displayed. Security 2 username: {eui64}")
    print("this image is for that one board only; another board refuses it")


if __name__ == "__main__":
    main()
