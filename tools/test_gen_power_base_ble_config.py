#!/usr/bin/env python3
"""
Runs the REAL tools/gen_power_base_ble_config.py (and through it the real
tools/pack_power_base_config.py) and re-reads every image with ESP-IDF's own
NVS parser to check the BLE-enrollment prepare rules:

  - one image holds power_base_cfg, pb_thread:mode=2 with NO Dataset,
    pb_bridge with exactly the address/port given, and pb_ble_auth for
    exactly the EUI-64 given; it is the cfg partition size and mode 0600;
  - the pb_ble_auth record has the documented layout and its verifier is
    the Security 2 verifier of (EUI-64, setup code) -- recomputed here from
    the formula, not taken from the tool;
  - the setup code is 26 base32 characters (128 bits), differs per run, is
    written 0600, and appears in no output and not in the image;
  - an existing setup-code file or image is never replaced silently; reuse
    is explicit and needs a 0600 file;
  - bad EUI-64 / bridge values and a code file inside the repository are
    refused with nothing written;
  - --bridge-from-enrollment makes an image with NO bridge address (pb_bridge
    enabled=0, address and port all zero) AND the explicit policy "endpoint
    required at enrollment" (policy byte 1), otherwise the same image; an
    image with --bridge-host/--bridge-port carries no policy (byte 0);
    the bridge source must be stated, the two ways cannot be mixed, and a
    mistyped address is an error, never "no address";
  - the pack tool's new flag does not change images made without it, and is
    refused outside mode 2.

Every address, EUI-64 and code below is made up or generated for the test.

Requires the ESP-IDF environment:
    source ~/.espressif/v6.1/esp-idf/export.sh
    python tools/test_gen_power_base_ble_config.py
"""
import hashlib
import ipaddress
import os
import re
import stat
import subprocess
import sys
import tempfile
import zlib

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS_DIR = os.path.join(ROOT_DIR, "tools")
GEN = os.path.join(TOOLS_DIR, "gen_power_base_ble_config.py")
PACK = os.path.join(TOOLS_DIR, "pack_power_base_config.py")
CONFIG_JSON = os.path.join(ROOT_DIR, "configs", "power_base.json")

sys.path.insert(0, TOOLS_DIR)
import pack_power_base_config as pack  # noqa: E402

EUI = "6055F9FFFE001234"  # upper case on purpose: the tool must normalise it
EUI_NORM = EUI.lower()
EUI2 = "6055f9fffe005678"
BRIDGE = "fd00:db8:1:2::1234"
PORT = 5684

# RFC 3526 3072-bit MODP prime with g = 5: the only SRP group ESP-IDF's
# protocomm Security 2 uses (components/protocomm/src/crypto/srp6a/esp_srp.c).
N_3072 = int(
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DD"
    "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F"
    "83655D23DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183995497CEA956AE515D2261898FA0510"
    "15728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
    "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B18177B200C"
    "BBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF", 16)

failures = 0


def check(cond, msg):
    global failures
    print(("ok:   " if cond else "FAIL: ") + msg)
    if not cond:
        failures += 1


def verified(fn, *args, **kwargs):
    try:
        fn(*args, **kwargs)
        return True
    except SystemExit as e:
        print(f"      verify failed: {e}")
        return False


def run_gen(*extra):
    return subprocess.run([sys.executable, GEN, *extra], capture_output=True, text=True)


def gen_args(eui, code_flag, code_path, out_dir, bridge=BRIDGE, port=PORT):
    return ["--eui64", eui, "--bridge-host", bridge, "--bridge-port", str(port), code_flag, code_path, "--out-dir",
            out_dir]


def mode_of(path):
    return stat.S_IMODE(os.stat(path).st_mode)


def read_namespaces(image_path):
    """{namespace: {key: value-or-bytes}} via ESP-IDF's nvs_parser."""
    tool_dir = os.path.join(os.environ["IDF_PATH"], "components", "nvs_flash", "nvs_partition_tool")
    if tool_dir not in sys.path:
        sys.path.insert(0, tool_dir)
    import nvs_parser
    with open(image_path, "rb") as f:
        partition = nvs_parser.NVS_Partition("cfg", bytearray(f.read()))
    ids = {}
    out = {}
    for page in partition.pages:
        for entry in page.entries:
            if not entry.data:
                continue
            if entry.metadata["namespace"] == 0:
                ids[entry.data.get("value")] = entry.key
                out.setdefault(entry.key, {})
            elif entry.metadata["namespace"] in ids:
                ns = ids[entry.metadata["namespace"]]
                if entry.metadata["type"] == "blob_data":
                    raw = bytearray()
                    for child in entry.children:
                        raw += child.raw
                    out[ns][entry.key] = bytes(raw[:entry.data.get("size")])
                elif entry.metadata["type"] not in ("blob_index", "blob"):
                    out[ns][entry.key] = entry.data.get("value")
    return out


def expected_verifier(username, code, salt):
    inner = hashlib.sha512(username.encode() + b":" + code.encode()).digest()
    x = int.from_bytes(hashlib.sha512(salt + inner).digest(), "big")
    return pow(5, x, N_3072).to_bytes(384, "big")


def main():
    if not os.environ.get("IDF_PATH"):
        print("SKIP: IDF_PATH not set -- source the ESP-IDF v6.1 export.sh first")
        return 2

    with tempfile.TemporaryDirectory() as tmp:
        os.chmod(tmp, 0o700)
        out_dir = os.path.join(tmp, "img")
        code_a = os.path.join(tmp, "a.setup-code")
        image_a = os.path.join(out_dir, f"power_base_ble_{EUI_NORM}_config.bin")

        print("\n-- a new board --")
        r = run_gen(*gen_args(EUI, "--setup-code-file", code_a, out_dir))
        check(r.returncode == 0 and os.path.isfile(image_a), "image written, named after the normalised EUI-64")
        check(os.path.getsize(image_a) == pack.CFG_PARTITION_SIZE == 0x3000, "image is exactly the cfg partition size")
        check(mode_of(image_a) == 0o600 and mode_of(code_a) == 0o600, "image and setup-code file are mode 0600")
        with open(code_a, "r", encoding="ascii") as f:
            raw_code = f.read()
        code = raw_code.strip()
        check(raw_code == code + "\n" and re.fullmatch(r"[A-Z2-7]{26}", code) is not None,
              "setup code is 26 base32 characters (16 random bytes = 128 bits)")
        check(code not in r.stdout + r.stderr, "the setup code is not in the tool's output")

        ns = read_namespaces(image_a)
        check(set(ns) == {"power_base_cfg", "pb_thread", "pb_bridge", "pb_ble_auth"},
              "the image holds exactly the four namespaces")
        check(verified(pack.verify_power_base_cfg_blob_len, image_a, 44), "power_base_cfg:blob is the usual 44 bytes")
        check(verified(pack.verify_pb_thread_config, image_a, expected_mode=pack.PB_THREAD_MODE_AWAIT_COMMISSIONING,
                       expected_tlvs_len=0) and "active_tlvs" not in ns["pb_thread"],
              "pb_thread:mode=2 and no Thread Dataset")
        check(verified(pack.verify_pb_bridge_config, image_a, expect_enabled=True,
                       expected_host_addr=ipaddress.IPv6Address(BRIDGE).packed, expected_port=PORT),
              "pb_bridge enabled with exactly the given address and port")

        rec = ns["pb_ble_auth"].get("cred", b"")
        check(list(ns["pb_ble_auth"]) == ["cred"] and len(rec) == 420, "pb_ble_auth holds one 420-byte record")
        check(rec[:4] == b"PBA1" and rec[4] == 1 and rec[5] == 1 and rec[6] == 16 and rec[7] == 0,
              "magic PBA1, format 1, SRP group 1, salt length 16, reserved 0")
        check(rec[8:16].hex() == EUI_NORM, "record names the given EUI-64")
        check(zlib.crc32(rec[:416]) == int.from_bytes(rec[416:420], "little"), "CRC-32 (little-endian) is correct")
        salt, verifier = rec[16:32], rec[32:416]
        check(salt[0] != 0 and verifier == expected_verifier(EUI_NORM, code, salt),
              "verifier = 5^SHA512(salt | SHA512(eui64 ':' code)) mod N3072, recomputed independently")
        check(verifier != expected_verifier(EUI.upper(), code, salt), "(the username is the lower-case EUI-64)")
        with open(image_a, "rb") as f:
            image_bytes = f.read()
        check(code.encode() not in image_bytes and code.lower().encode() not in image_bytes,
              "the setup code itself is not in the image")
        check(salt.hex() not in r.stdout + r.stderr and verifier[:16].hex() not in r.stdout + r.stderr,
              "salt and verifier are not in the tool's output")

        print("\n-- nothing is replaced silently --")
        before = (open(code_a, "rb").read(), image_bytes)
        r = run_gen(*gen_args(EUI, "--setup-code-file", code_a, out_dir))
        check(r.returncode != 0 and (open(code_a, "rb").read(), open(image_a, "rb").read()) == before,
              "same setup-code path again: refused, code and image unchanged")
        code_b = os.path.join(tmp, "b.setup-code")
        r = run_gen(*gen_args(EUI, "--setup-code-file", code_b, out_dir))
        check(r.returncode != 0 and not os.path.exists(code_b) and open(image_a, "rb").read() == image_bytes,
              "new code for a board that already has an image: refused, no code file left behind")
        os.remove(image_a)
        r = run_gen(*gen_args(EUI, "--setup-code-file", code_a, out_dir))
        check(r.returncode != 0 and not os.path.exists(image_a) and open(code_a, "rb").read() == before[0],
              "existing code file is not overwritten even when the image is gone")

        print("\n-- explicit reuse of a setup code --")
        r = run_gen(*gen_args(EUI, "--existing-setup-code-file", code_a, out_dir))
        ns2 = read_namespaces(image_a) if r.returncode == 0 else {}
        rec2 = ns2.get("pb_ble_auth", {}).get("cred", b"")
        check(r.returncode == 0 and len(rec2) == 420 and rec2[16:32] != salt,
              "image rebuilt for the same code, with a fresh salt")
        check(rec2[32:416] == expected_verifier(EUI_NORM, code, rec2[16:32]) and open(code_a, "rb").read() == before[0],
              "its verifier matches the same code; the code file is untouched")
        os.chmod(code_a, 0o644)
        r = run_gen(*gen_args(EUI, "--existing-setup-code-file", code_a, out_dir))
        check(r.returncode != 0, "a group/world-readable code file is refused")
        os.chmod(code_a, 0o600)
        junk = os.path.join(tmp, "junk.setup-code")
        with open(junk, "w") as f:
            f.write("password123\n")
        os.chmod(junk, 0o600)
        r = run_gen(*gen_args(EUI2, "--existing-setup-code-file", junk, out_dir))
        check(r.returncode != 0 and "password123" not in r.stdout + r.stderr and
              not os.path.exists(os.path.join(out_dir, f"power_base_ble_{EUI2}_config.bin")),
              "a hand-made weak code is refused, and not echoed")

        print("\n-- every board gets its own code --")
        code_c = os.path.join(tmp, "c.setup-code")
        r = run_gen(*gen_args(EUI2, "--setup-code-file", code_c, out_dir))
        image_c = os.path.join(out_dir, f"power_base_ble_{EUI2}_config.bin")
        rec3 = read_namespaces(image_c)["pb_ble_auth"]["cred"] if r.returncode == 0 else b""
        check(r.returncode == 0 and open(code_c).read().strip() != code, "second board: a different setup code")
        check(rec3[8:16].hex() == EUI2 and rec3[32:416] != verifier, "and a record for its own EUI-64")

        print("\n-- refused inputs: nothing written --")
        n = 0

        def refused(msg, *args):
            nonlocal n
            n += 1
            cf = os.path.join(tmp, f"refused{n}.setup-code")
            od = os.path.join(tmp, f"refused{n}")
            full = [a.replace("@CODE@", cf).replace("@OUT@", od) for a in args]
            rr = run_gen(*full)
            leftovers = os.path.exists(cf) or (os.path.isdir(od) and os.listdir(od))
            check(rr.returncode != 0 and not leftovers, msg)

        base = ["--bridge-host", BRIDGE, "--bridge-port", str(PORT), "--setup-code-file", "@CODE@", "--out-dir", "@OUT@"]
        refused("15-digit EUI-64", "--eui64", "6055f9fffe00123", *base)
        refused("EUI-64 with separators", "--eui64", "60:55:f9:ff:fe:00:12:34", *base)
        refused("all-zero EUI-64", "--eui64", "0" * 16, *base)
        refused("missing EUI-64", *base)
        eui = ["--eui64", EUI2, "--setup-code-file", "@CODE@", "--out-dir", "@OUT@"]
        refused("missing bridge address", *eui, "--bridge-port", str(PORT))
        refused("missing bridge port", *eui, "--bridge-host", BRIDGE)
        refused("link-local bridge address", *eui, "--bridge-host", "fe80::1", "--bridge-port", str(PORT))
        refused("IPv4 bridge address", *eui, "--bridge-host", "192.0.2.1", "--bridge-port", str(PORT))
        refused("bridge port 0", *eui, "--bridge-host", BRIDGE, "--bridge-port", "0")
        refused("bridge port 70000", *eui, "--bridge-host", BRIDGE, "--bridge-port", "70000")
        print("\n-- address-less image: the bridge endpoint comes with the enrollment --")
        code_n = os.path.join(tmp, "noaddr.setup-code")
        out_n = os.path.join(tmp, "noaddr")
        r = run_gen("--eui64", EUI, "--bridge-from-enrollment", "--setup-code-file", code_n, "--out-dir", out_n)
        image_n = os.path.join(out_n, f"power_base_ble_{EUI_NORM}_config.bin")
        check(r.returncode == 0 and os.path.isfile(image_n) and os.path.getsize(image_n) == pack.CFG_PARTITION_SIZE and
              mode_of(image_n) == 0o600 and mode_of(code_n) == 0o600,
              "--bridge-from-enrollment: image written, cfg partition size, image and code file mode 0600")
        ns_n = read_namespaces(image_n)
        check(set(ns_n) == {"power_base_cfg", "pb_thread", "pb_bridge", "pb_ble_auth"},
              "it holds the same four namespaces")
        blob_n = ns_n["pb_bridge"].get("blob")
        check(blob_n == (1).to_bytes(4, "big") + bytes([0, 1]) + bytes(18),
              "pb_bridge: schema 1, enabled=0, policy byte 1 (endpoint required), 16 address bytes and the port all zero")
        check(blob_n[pack.PB_BRIDGE_ENROLL_POLICY_OFFSET] == pack.PB_BRIDGE_ENROLL_POLICY_ENDPOINT_REQUIRED == 1 and
              pack.PB_BRIDGE_ENROLL_POLICY_OFFSET == 5,
              "the policy is the explicit byte the firmware reads (offset 5, value 1), not inferred from enabled=0")
        check(ns_n["pb_bridge"].get("crc") == pack.crc16_ccitt(blob_n),
              "the pb_bridge CRC covers the policy byte")
        check(verified(pack.verify_pb_bridge_config, image_n, expect_enabled=False,
                       expected_enroll_policy=pack.PB_BRIDGE_ENROLL_POLICY_ENDPOINT_REQUIRED),
              "the pack tool's own verifier reads it as disabled with the policy")
        check(not verified(pack.verify_pb_bridge_config, image_n, expect_enabled=False),
              "(and that verifier does notice a policy that was not expected)")
        check("REQUIRED" in r.stdout, "the output says the endpoint is required at enrollment")
        static_blob = ns["pb_bridge"].get("blob")
        check(static_blob[4] == 1 and static_blob[pack.PB_BRIDGE_ENROLL_POLICY_OFFSET] == 0 and
              verified(pack.verify_pb_bridge_config, image_a, expect_enabled=True,
                       expected_host_addr=ipaddress.IPv6Address(BRIDGE).packed, expected_port=PORT),
              "the image with --bridge-host/--bridge-port carries NO policy (byte 0): it enrolls the old way")
        check(ns_n["power_base_cfg"] == ns["power_base_cfg"] and ns_n["pb_thread"] == ns["pb_thread"],
              "the electrical config and pb_thread:mode=2 (no Dataset) are byte-identical to the image with an address")
        check(verified(pack.verify_pb_thread_config, image_n, expected_mode=pack.PB_THREAD_MODE_AWAIT_COMMISSIONING,
                       expected_tlvs_len=0),
              "pb_thread: mode 2 with an empty Dataset")
        rec_n = ns_n["pb_ble_auth"]["cred"]
        code_n_text = open(code_n).read().strip()
        check(len(rec_n) == 420 and rec_n[8:16].hex() == EUI_NORM and
              rec_n[32:416] == expected_verifier(EUI_NORM, code_n_text, rec_n[16:32]) and
              zlib.crc32(rec_n[:416]) == int.from_bytes(rec_n[416:420], "little"),
              "the board's own BLE credential is prepared exactly as with an address")
        image_n_bytes = open(image_n, "rb").read()
        static_addr = ipaddress.IPv6Address(BRIDGE).packed
        check(static_addr not in image_n_bytes and code_n_text.encode() not in image_n_bytes,
              "no bridge address and no setup code anywhere in the image")
        check(code_n_text not in r.stdout + r.stderr and rec_n[16:32].hex() not in r.stdout + r.stderr and
              "NO address" in r.stdout,
              "the output says the image has no address, and shows neither the setup code nor the salt")
        out_n2 = os.path.join(tmp, "noaddr2")
        r = run_gen("--eui64", EUI2, "--bridge-from-enrollment", "--setup-code-file", os.path.join(tmp, "noaddr2.code"),
                    "--out-dir", out_n2)
        ns_n2 = read_namespaces(os.path.join(out_n2, f"power_base_ble_{EUI2}_config.bin"))
        check(r.returncode == 0 and ns_n2["pb_bridge"] == ns_n["pb_bridge"] and
              ns_n2["power_base_cfg"] == ns_n["power_base_cfg"] and ns_n2["pb_thread"] == ns_n["pb_thread"] and
              ns_n2["pb_ble_auth"]["cred"][8:16].hex() == EUI2,
              "another board's address-less image differs only in its own BLE credential: nothing per-home is in it")
        r = run_gen("--eui64", EUI, "--bridge-from-enrollment", "--existing-setup-code-file", code_n, "--out-dir", out_n)
        check(r.returncode == 0 and read_namespaces(image_n)["pb_bridge"] == ns_n["pb_bridge"],
              "rebuilding with --existing-setup-code-file keeps it address-less")

        print("\n-- the bridge source must be stated; a typed address is never ignored --")
        eui_only = ["--eui64", EUI2, "--setup-code-file", "@CODE@", "--out-dir", "@OUT@"]
        refused("neither --bridge-from-enrollment nor an address (no default address is made up)", *eui_only)
        refused("--bridge-from-enrollment together with a valid address and port", *eui_only, "--bridge-from-enrollment",
                "--bridge-host", BRIDGE, "--bridge-port", str(PORT))
        refused("--bridge-from-enrollment together with --bridge-host only", *eui_only, "--bridge-from-enrollment",
                "--bridge-host", BRIDGE)
        refused("--bridge-from-enrollment together with --bridge-port only", *eui_only, "--bridge-from-enrollment",
                "--bridge-port", str(PORT))
        refused("--bridge-from-enrollment together with a MALFORMED address (not treated as \"no address\")", *eui_only,
                "--bridge-from-enrollment", "--bridge-host", "fd00:db8::zz", "--bridge-port", str(PORT))
        refused("an empty --bridge-host is an error, not \"no address\"", *eui_only, "--bridge-host", "",
                "--bridge-port", str(PORT))
        refused("a malformed --bridge-host", *eui_only, "--bridge-host", "fd00:db8:1:2::12345", "--bridge-port", str(PORT))
        refused("a hostname as --bridge-host", *eui_only, "--bridge-host", "hub.local", "--bridge-port", str(PORT))
        refused("a URL as --bridge-host", *eui_only, "--bridge-host", "coap://[fd00:db8::1]:5684", "--bridge-port",
                str(PORT))
        refused("the unspecified address as --bridge-host", *eui_only, "--bridge-host", "::", "--bridge-port", str(PORT))
        refused("a multicast --bridge-host", *eui_only, "--bridge-host", "ff03::1", "--bridge-port", str(PORT))
        refused("a non-numeric --bridge-port", *eui_only, "--bridge-host", BRIDGE, "--bridge-port", "abc")

        in_repo = os.path.join(ROOT_DIR, "build", "pb_test_should_not_exist.setup-code")
        r = run_gen("--eui64", EUI2, "--bridge-host", BRIDGE, "--bridge-port", str(PORT), "--setup-code-file", in_repo,
                    "--out-dir", os.path.join(tmp, "refused_repo"))
        check(r.returncode != 0 and not os.path.exists(in_repo), "a setup-code file inside the repository is refused")
        r = run_gen("--eui64", EUI2, "--bridge-host", BRIDGE, "--bridge-port", str(PORT), "--out-dir",
                    os.path.join(tmp, "refused_nocode"))
        check(r.returncode != 0, "no setup-code option at all is refused (a code is never passed as a value)")

        print("\n-- pack tool: the endpoint policy is explicit, mode-2 only, and never combined with an address --")
        pol = os.path.join(tmp, "pol.bin")
        r = subprocess.run([sys.executable, PACK, CONFIG_JSON, pol, "--await-commissioning",
                            "--require-enrollment-endpoint"], capture_output=True, text=True)
        check(r.returncode == 0 and read_namespaces(pol)["pb_bridge"]["blob"] == (1).to_bytes(4, "big") + bytes([0, 1]) + bytes(18)
              and "enrollment_endpoint=REQUIRED" in r.stdout,
              "--await-commissioning --require-enrollment-endpoint: enabled=0 with policy byte 1")
        nopol = os.path.join(tmp, "nopol.bin")
        r = subprocess.run([sys.executable, PACK, CONFIG_JSON, nopol, "--await-commissioning"], capture_output=True,
                           text=True)
        check(r.returncode == 0 and read_namespaces(nopol)["pb_bridge"]["blob"] == (1).to_bytes(4, "big") + bytes(20) and
              "REQUIRED" not in r.stdout,
              "--await-commissioning alone (exporter off on purpose): policy byte 0 -- 'no address' is not 'required'")
        legacy = os.path.join(tmp, "legacy.bin")
        r = subprocess.run([sys.executable, PACK, CONFIG_JSON, legacy], capture_output=True, text=True)
        check(r.returncode == 0 and read_namespaces(legacy)["pb_bridge"]["blob"][5] == 0,
              "a mode 0 image: policy byte 0, as before")
        for what, extra in (("without --await-commissioning", ["--require-enrollment-endpoint"]),
                            ("with a static address", ["--await-commissioning", "--require-enrollment-endpoint",
                                                       "--bridge-host", BRIDGE, "--bridge-port", str(PORT)])):
            bad_out = os.path.join(tmp, "pol_refused.bin")
            r = subprocess.run([sys.executable, PACK, CONFIG_JSON, bad_out, *extra], capture_output=True, text=True)
            check(r.returncode != 0 and not os.path.exists(bad_out), f"--require-enrollment-endpoint {what}: refused")

        print("\n-- pack tool: the new flag is optional and mode-2 only --")
        plain = os.path.join(tmp, "await_bridge.bin")
        r = subprocess.run([sys.executable, PACK, CONFIG_JSON, plain, "--await-commissioning", "--bridge-host", BRIDGE,
                            "--bridge-port", str(PORT)], capture_output=True, text=True)
        check(r.returncode == 0 and set(read_namespaces(plain)) == {"power_base_cfg", "pb_thread", "pb_bridge"},
              "a mode-2 image made without the flag has no pb_ble_auth namespace (as before)")
        check(verified(pack.verify_pb_ble_auth, plain, expected_record=None), "and verifies as such")
        ns_plain = read_namespaces(plain)
        check(ns_plain["power_base_cfg"] == ns["power_base_cfg"] and ns_plain["pb_thread"] == ns["pb_thread"] and
              ns_plain["pb_bridge"] == ns["pb_bridge"],
              "the other three namespaces are byte-identical with and without the BLE record")
        rec_file = os.path.join(tmp, "rec.bin")
        with open(rec_file, "wb") as f:
            f.write(rec)
        os.chmod(rec_file, 0o600)
        out = os.path.join(tmp, "p1.bin")
        r = subprocess.run([sys.executable, PACK, CONFIG_JSON, out, "--ble-auth-record-file", rec_file],
                           capture_output=True, text=True)
        check(r.returncode != 0 and not os.path.exists(out), "record without --await-commissioning: refused")
        os.chmod(rec_file, 0o644)
        r = subprocess.run([sys.executable, PACK, CONFIG_JSON, out, "--await-commissioning", "--ble-auth-record-file",
                            rec_file], capture_output=True, text=True)
        check(r.returncode != 0 and not os.path.exists(out), "group/world-readable record file: refused")
        os.chmod(rec_file, 0o600)
        with open(rec_file, "wb") as f:
            f.write(rec[:100])
        r = subprocess.run([sys.executable, PACK, CONFIG_JSON, out, "--await-commissioning", "--ble-auth-record-file",
                            rec_file], capture_output=True, text=True)
        check(r.returncode != 0 and not os.path.exists(out), "record of the wrong size: refused")

    print(f"\n{'PASS' if failures == 0 else 'FAIL'}: {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
