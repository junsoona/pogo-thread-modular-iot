#!/usr/bin/env python3
"""
Runs the REAL tools/pack_power_base_config.py and tools/gen_power_base_await_config.sh
and re-parses every image they write (with ESP-IDF's own nvs_parser, via the
pack tool's verify_* helpers) to check the pb_thread:mode=2
(AWAIT_COMMISSIONING) rules:

  - mode 2 alone: no Dataset, pb_bridge disabled;
  - mode 2 + --bridge-host/--bridge-port: still no Dataset, pb_bridge enabled
    with exactly that address/port (the exporter itself waits for enrollment
    in firmware -- tools/test_power_base_enrollment.c covers that side);
  - mode 2 + --external-dataset-file: refused, no image written;
  - an incomplete / link-local bridge address: refused;
  - earlier modes unchanged: no flags -> STANDALONE(0), a Dataset file ->
    EXTERNAL_OTBR(1) with that exact Dataset.

Every address and Dataset byte below is made up (documentation/ULA ranges);
no installation's real address is used.

Requires the ESP-IDF environment (nvs_partition_gen.py / nvs_parser):
    source ~/.espressif/v6.1/esp-idf/export.sh
    python3 tools/test_pack_power_base_await_config.py
"""
import ipaddress
import os
import subprocess
import sys
import tempfile

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS_DIR = os.path.join(ROOT_DIR, "tools")
PACK = os.path.join(TOOLS_DIR, "pack_power_base_config.py")
GEN_AWAIT = os.path.join(TOOLS_DIR, "gen_power_base_await_config.sh")
CONFIG_JSON = os.path.join(ROOT_DIR, "configs", "power_base.json")

sys.path.insert(0, TOOLS_DIR)
import pack_power_base_config as pack  # noqa: E402

MADE_UP_BRIDGE = "fd00:db8:1:2::1234"
MADE_UP_PORT = 5684
MADE_UP_DATASET = bytes(range(1, 41))  # 40 bytes, not a real Dataset

failures = 0


def check(cond, msg):
    global failures
    print(("ok:   " if cond else "FAIL: ") + msg)
    if not cond:
        failures += 1


def verified(fn, *args, **kwargs):
    """The pack tool's verify_* helpers sys.exit() on mismatch."""
    try:
        fn(*args, **kwargs)
        return True
    except SystemExit as e:
        print(f"      verify failed: {e}")
        return False


def run_pack(out_path, *extra):
    return subprocess.run([sys.executable, PACK, CONFIG_JSON, out_path, *extra], capture_output=True, text=True)


def main():
    if not os.environ.get("IDF_PATH"):
        print("SKIP: IDF_PATH not set -- source the ESP-IDF v6.1 export.sh first")
        return 2

    with tempfile.TemporaryDirectory() as tmp:
        print("\n-- mode 2 alone --")
        out = os.path.join(tmp, "await.bin")
        r = run_pack(out, "--await-commissioning")
        check(r.returncode == 0 and os.path.isfile(out), "packs")
        check(verified(pack.verify_pb_thread_config, out, expected_mode=pack.PB_THREAD_MODE_AWAIT_COMMISSIONING,
                       expected_tlvs_len=0), "pb_thread:mode=2 and no active_tlvs")
        check(verified(pack.verify_pb_bridge_config, out, expect_enabled=False), "pb_bridge disabled")

        print("\n-- mode 2 + bridge address/port --")
        out = os.path.join(tmp, "await_bridge.bin")
        r = run_pack(out, "--await-commissioning", "--bridge-host", MADE_UP_BRIDGE, "--bridge-port", str(MADE_UP_PORT))
        check(r.returncode == 0 and os.path.isfile(out), "packs")
        check(verified(pack.verify_pb_thread_config, out, expected_mode=pack.PB_THREAD_MODE_AWAIT_COMMISSIONING,
                       expected_tlvs_len=0), "still mode 2 with no active_tlvs")
        check(verified(pack.verify_pb_bridge_config, out, expect_enabled=True,
                       expected_host_addr=ipaddress.IPv6Address(MADE_UP_BRIDGE).packed, expected_port=MADE_UP_PORT),
              "pb_bridge enabled with exactly that address and port")
        check("exporter starts only after enrollment" in r.stdout, "output says the exporter waits for enrollment")

        print("\n-- refused combinations --")
        ds_file = os.path.join(tmp, "made_up_dataset.hex")
        with open(ds_file, "w") as f:
            f.write(MADE_UP_DATASET.hex())
        out = os.path.join(tmp, "refused1.bin")
        r = run_pack(out, "--await-commissioning", "--external-dataset-file", ds_file)
        check(r.returncode != 0 and not os.path.exists(out), "mode 2 + Dataset file refused, nothing written")
        check(MADE_UP_DATASET.hex() not in (r.stdout + r.stderr), "refusal does not echo the Dataset")
        out = os.path.join(tmp, "refused2.bin")
        r = run_pack(out, "--await-commissioning", "--bridge-host", MADE_UP_BRIDGE)
        check(r.returncode != 0 and not os.path.exists(out), "bridge host without port refused")
        out = os.path.join(tmp, "refused3.bin")
        r = run_pack(out, "--await-commissioning", "--bridge-host", "fe80::1", "--bridge-port", "5684")
        check(r.returncode != 0 and not os.path.exists(out), "link-local bridge address refused")
        out = os.path.join(tmp, "refused4.bin")
        r = run_pack(out, "--await-commissioning", "--bridge-host", MADE_UP_BRIDGE, "--bridge-port", "0")
        check(r.returncode != 0 and not os.path.exists(out), "port 0 refused")

        print("\n-- earlier modes unchanged --")
        out = os.path.join(tmp, "standalone.bin")
        r = run_pack(out)
        check(r.returncode == 0 and verified(pack.verify_pb_thread_config, out,
                                             expected_mode=pack.PB_THREAD_MODE_STANDALONE, expected_tlvs_len=0),
              "no flags: STANDALONE(0), no Dataset")
        out = os.path.join(tmp, "external.bin")
        r = run_pack(out, "--external-dataset-file", ds_file)
        check(r.returncode == 0 and verified(pack.verify_pb_thread_config, out,
                                             expected_mode=pack.PB_THREAD_MODE_EXTERNAL_OTBR,
                                             expected_tlvs_len=len(MADE_UP_DATASET), expected_tlvs=MADE_UP_DATASET),
              "Dataset file: EXTERNAL_OTBR(1) with exactly that Dataset")
        check((os.stat(out).st_mode & 0o077) == 0, "EXTERNAL_OTBR image is still 0600")

        print("\n-- gen_power_base_await_config.sh --")
        gen_dir = os.path.join(tmp, "gen")
        r = subprocess.run(["bash", GEN_AWAIT, gen_dir, MADE_UP_BRIDGE, str(MADE_UP_PORT)], capture_output=True,
                           text=True)
        out = os.path.join(gen_dir, "power_base_await_bridge_config.bin")
        check(r.returncode == 0 and verified(pack.verify_pb_bridge_config, out, expect_enabled=True,
                                             expected_host_addr=ipaddress.IPv6Address(MADE_UP_BRIDGE).packed,
                                             expected_port=MADE_UP_PORT),
              "wrapper with address + port writes power_base_await_bridge_config.bin")
        r = subprocess.run(["bash", GEN_AWAIT, gen_dir, MADE_UP_BRIDGE], capture_output=True, text=True)
        check(r.returncode != 0, "wrapper with an address but no port refuses")
        r = subprocess.run(["bash", GEN_AWAIT, gen_dir], capture_output=True, text=True)
        out = os.path.join(gen_dir, "power_base_await_config.bin")
        check(r.returncode == 0 and verified(pack.verify_pb_bridge_config, out, expect_enabled=False),
              "wrapper without an address keeps pb_bridge disabled")

    print(f"\n{'PASS' if failures == 0 else 'FAIL'}: {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
