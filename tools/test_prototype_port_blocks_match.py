#!/usr/bin/env python3
"""
Confirms configs/prototype_sht45.json and configs/prototype_bh1750.json
share an EXACTLY identical upstream_port/downstream_port block -- section 2
of the GPIO-correction follow-up's minimum test list: "두 prototype의 모든
POGO 포트 블록이 완전히 동일함을 확인". Per section 2 of the original GPIO
follow-up spec, only driver_id/i2c_address/sensor_type_bitmap/calibration
may ever differ between the two real boards' configs -- the POGO wiring must
not.

Usage:
    python3 tools/test_prototype_port_blocks_match.py
"""
import json
import os
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(name):
    with open(os.path.join(ROOT_DIR, "configs", name), "r", encoding="utf-8") as f:
        return json.load(f)


def main():
    sht45 = load("prototype_sht45.json")
    bh1750 = load("prototype_bh1750.json")

    failures = 0

    def check(cond, msg):
        nonlocal failures
        print(("ok:   " if cond else "FAIL: ") + msg)
        if not cond:
            failures += 1

    for port_name in ("upstream_port", "downstream_port"):
        check(sht45[port_name] == bh1750[port_name],
              f"{port_name} is byte-for-byte identical between prototype_sht45.json and prototype_bh1750.json")

    # Confirm they DO still differ on the fields that are allowed/expected to
    # differ -- a passing "identical ports" check would be meaningless if the
    # two files were accidentally identical copies of each other.
    check(sht45["driver_id"] != bh1750["driver_id"], "driver_id differs between the two boards (as expected)")
    check(sht45["i2c_address"] != bh1750["i2c_address"], "i2c_address differs between the two boards (as expected)")
    check(sht45["sensor_type_bitmap"] != bh1750["sensor_type_bitmap"],
          "sensor_type_bitmap differs between the two boards (as expected)")

    # And the shared fields that must match beyond just the port blocks.
    check(sht45["dev_mode"] == bh1750["dev_mode"] == "AUTO", "both configs use dev_mode=AUTO")
    check(sht45["bus_type"] == bh1750["bus_type"], "bus_type matches")
    check(sht45["i2c_sda_gpio"] == bh1750["i2c_sda_gpio"], "i2c_sda_gpio matches")
    check(sht45["i2c_scl_gpio"] == bh1750["i2c_scl_gpio"], "i2c_scl_gpio matches")

    # Explicitly confirm the deep-sleep-ready GPIO swap made it into both
    # files (UPSTREAM DETECT_IN=GPIO12/DETECT_OUT=GPIO2, swapped from the
    # earlier DETECT_IN=2/DETECT_OUT=12 profile -- see docs/power-base.md).
    check(sht45["upstream_port"]["detect_in_gpio"] == 12, "prototype_sht45.json's upstream_port.detect_in_gpio is 12 (deep-sleep-ready swap)")
    check(bh1750["upstream_port"]["detect_in_gpio"] == 12, "prototype_bh1750.json's upstream_port.detect_in_gpio is 12 (deep-sleep-ready swap)")
    check(sht45["upstream_port"]["detect_out_gpio"] == 2, "prototype_sht45.json's upstream_port.detect_out_gpio is 2 (deep-sleep-ready swap)")
    check(bh1750["upstream_port"]["detect_out_gpio"] == 2, "prototype_bh1750.json's upstream_port.detect_out_gpio is 2 (deep-sleep-ready swap)")

    if failures == 0:
        print("ALL CHECKS PASSED")
        return 0
    print(f"{failures} CHECK(S) FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
