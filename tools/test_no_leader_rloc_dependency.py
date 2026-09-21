#!/usr/bin/env python3
"""
Confirms sensor_transport.c and topology_reporter.c do not use
thread_manager_get_leader_address() for their send destination. The API
stays in thread_manager for diagnostics, while both send paths obtain the
real destination from collector_endpoint.

sensor_transport has a separate thread_manager dependency for the
per-sample radio-parent diagnostic. That dependency is valid as long as it
does not feed the destination address. topology_reporter needs no such
diagnostic dependency.

A textual/structural check rather than a build-time one: neither file can be
host-compiled (real OpenThread/FreeRTOS/NVS calls), so this is the automated
regression check for "the Leader-RLOC dependency is gone" that a host test
suite can actually run without an ESP-IDF toolchain.

Usage:
    python3 tools/test_no_leader_rloc_dependency.py
"""
import os
import re
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TARGETS = [
    "components/sensor_transport/sensor_transport.c",
    "components/topology_reporter/topology_reporter.c",
]


def main():
    failures = 0

    def check(cond, msg):
        nonlocal failures
        print(("ok:   " if cond else "FAIL: ") + msg)
        if not cond:
            failures += 1

    for rel_path in TARGETS:
        path = os.path.join(ROOT_DIR, rel_path)
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()

        check("thread_manager_get_leader_address" not in text,
              f"{rel_path} no longer calls thread_manager_get_leader_address()")
        if rel_path.endswith("sensor_transport.c"):
            check(re.search(r'#include\s*"thread_manager/thread_manager\.h"', text) is not None,
                  f"{rel_path} includes thread_manager only for the radio-parent diagnostic")
            check("thread_manager_check_parent_for_sample(" in text,
                  f"{rel_path} calls the per-sample radio-parent diagnostic")
        else:
            check(not re.search(r'#include\s*"thread_manager/thread_manager\.h"', text),
                  f"{rel_path} does not include thread_manager")
        check('#include "collector_endpoint/collector_endpoint.h"' in text,
              f"{rel_path} includes collector_endpoint/collector_endpoint.h")
        check("collector_endpoint_get(" in text,
              f"{rel_path} reads its send destination from collector_endpoint_get()")

    # The diagnostic API remains available; only destination selection is
    # forbidden from using it.
    thread_manager_header = os.path.join(ROOT_DIR, "components/thread_manager/include/thread_manager/thread_manager.h")
    with open(thread_manager_header, "r", encoding="utf-8") as f:
        header_text = f.read()
    check("thread_manager_get_leader_address" in header_text,
          "thread_manager_get_leader_address() API itself is intentionally still present (for other diagnostic code)")

    for comp_dir in ("sensor_transport", "topology_reporter"):
        cmake_path = os.path.join(ROOT_DIR, "components", comp_dir, "CMakeLists.txt")
        with open(cmake_path, "r", encoding="utf-8") as f:
            cmake_text = f.read()
        if comp_dir == "sensor_transport":
            check("thread_manager" in cmake_text,
                  "components/sensor_transport/CMakeLists.txt REQUIRES thread_manager for parent diagnostics")
        else:
            check("thread_manager" not in cmake_text,
                  "components/topology_reporter/CMakeLists.txt does not require thread_manager")
        check("collector_endpoint" in cmake_text,
              f"components/{comp_dir}/CMakeLists.txt REQUIRES collector_endpoint")

    if failures == 0:
        print("ALL CHECKS PASSED")
        return 0
    print(f"{failures} CHECK(S) FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
