#!/usr/bin/env python3
"""
Mechanically checks that tools/pack_module_config.py's GPIO reserved-pin
classification never drifts from components/module_config/gpio_profile.c's
-- the GPIO-correction follow-up's explicit ask ("C와 Python의 금지 GPIO
목록이 달라지지 않도록 host test를 추가한다"), verified by actually running
both instead of eyeballing two hand-maintained lists.

Requires tools/test_gpio_profile.c to already be built with its --dump mode
(see that file's own build comment). This script builds it itself if the
binary is missing.

Usage:
    python3 tools/test_gpio_profile_parity.py
"""
import os
import subprocess
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT_DIR)
sys.path.insert(0, os.path.join(ROOT_DIR, "tools"))

from tools.pack_module_config import gpio_reserved_reason, GPIO_MAX  # noqa: E402


def build_c_dump_binary() -> str:
    binary = "/tmp/test_gpio_profile_parity_bin"
    src = os.path.join(ROOT_DIR, "tools", "test_gpio_profile.c")
    impl = os.path.join(ROOT_DIR, "components", "module_config", "gpio_profile.c")
    include = os.path.join(ROOT_DIR, "components", "module_config", "include")
    subprocess.run(
        ["cc", "-Wall", "-Wextra", "-I", include, "-o", binary, src, impl],
        check=True,
    )
    return binary


def parse_c_dump(binary: str):
    out = subprocess.run([binary, "--dump"], check=True, capture_output=True, text=True).stdout
    result = {}
    for line in out.strip().splitlines():
        gpio_s, allowed_s, reason = line.split(",", 2)
        result[int(gpio_s)] = (allowed_s == "1", reason if reason else None)
    return result


def main():
    binary = build_c_dump_binary()
    c_results = parse_c_dump(binary)

    failures = 0
    checked = 0
    for gpio in range(-1, GPIO_MAX + 2):  # includes one past GPIO_MAX to check the boundary too
        c_allowed, c_reason = c_results.get(gpio, (False, "out of range"))
        py_reason = gpio_reserved_reason(gpio) if -1 <= gpio <= GPIO_MAX else "out of range"
        py_allowed = (-1 <= gpio <= GPIO_MAX) and py_reason is None

        checked += 1
        if c_allowed != py_allowed:
            print(f"FAIL: GPIO{gpio}: C allowed={c_allowed} but Python allowed={py_allowed}")
            failures += 1
        elif not c_allowed and c_reason != py_reason and gpio != GPIO_MAX + 1:
            # Out-of-range (gpio == GPIO_MAX+1) reasons are allowed to differ
            # in wording ("out of range" on both sides regardless) -- only
            # the *reserved-pin* reason strings must match exactly.
            print(f"FAIL: GPIO{gpio}: C reason={c_reason!r} but Python reason={py_reason!r}")
            failures += 1

    if failures == 0:
        print(f"ok:   C and Python GPIO classifications agree for all {checked} checked values "
              f"(GPIO-1..GPIO{GPIO_MAX + 1})")
        print("ALL CHECKS PASSED")
        return 0
    print(f"{failures} CHECK(S) FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
