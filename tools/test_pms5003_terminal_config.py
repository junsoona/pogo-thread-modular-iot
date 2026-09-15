#!/usr/bin/env python3
"""Host checks for the schema-v2 terminal PMS5003 configuration."""

import importlib.util
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACKER_PATH = os.path.join(ROOT, "tools", "pack_module_config.py")
SPEC = importlib.util.spec_from_file_location("pack_module_config", PACKER_PATH)
packer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(packer)


def main():
    with open(os.path.join(ROOT, "configs", "prototype_pms5003.json"), encoding="utf-8") as f:
        cfg = json.load(f)
    blob = packer.pack_config(cfg)

    checks = []
    checks.append((len(blob) == 104, "schema-v2 blob is 104 bytes"))
    checks.append((struct.unpack_from(">I", blob, 0)[0] == 2, "schema_version=2 is encoded"))
    checks.append((blob[14] == packer.BUS_TYPES["UART"], "bus_type=UART is encoded"))
    checks.append((struct.unpack_from(">hh", blob, 54) == (10, 11), "UART GPIO10/11 are encoded"))
    checks.append((struct.unpack_from(">I", blob, 58)[0] == 9600, "UART baud=9600 is encoded"))
    checks.append((blob[62] == packer.UART_MODES["PASSIVE"], "PASSIVE mode is encoded"))
    checks.append((struct.unpack_from(">I", blob, 64)[0] == 30000, "30-second warm-up is encoded"))
    checks.append((struct.unpack_from(">h", blob, 86)[0] == -1, "downstream UART is disabled"))
    checks.append((all(cfg["downstream_port"][key] == -1 for key in
                       ("uart_num", "tx_gpio", "rx_gpio", "detect_in_gpio", "detect_out_gpio", "base_id_gpio")),
                   "all downstream hardware fields are disabled"))

    failures = 0
    for passed, message in checks:
        print(("ok:   " if passed else "FAIL: ") + message)
        failures += 0 if passed else 1
    if failures:
        print(f"{failures} CHECK(S) FAILED")
        return 1
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
