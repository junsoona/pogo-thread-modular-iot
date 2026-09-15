#!/usr/bin/env python3
"""
Confirms sensor_transport.c and topology_reporter.c no longer depend on
thread_manager_get_leader_address() for their send destination -- required
test item 13 of this follow-up's minimum test list: "sensor_transport/
topology_reporter가 Leader RLOC를 사용하지 않음". thread_manager_get_leader_address()
itself is intentionally left in components/thread_manager (other diagnostic
code may still use it) -- this only checks the two data-transmission
components that were required to stop depending on it, and confirms they now
route through components/collector_endpoint instead.

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
        check(not re.search(r'#include\s*"thread_manager/thread_manager\.h"', text),
              f"{rel_path} no longer #includes thread_manager/thread_manager.h")
        check('#include "collector_endpoint/collector_endpoint.h"' in text,
              f"{rel_path} includes collector_endpoint/collector_endpoint.h")
        check("collector_endpoint_get(" in text,
              f"{rel_path} reads its send destination from collector_endpoint_get()")

    # And the reverse-direction check: components/thread_manager itself still
    # keeps the API (other diagnostic code may still use it) -- this
    # follow-up never said to delete it, only to stop using it for sensor/
    # topology destination decisions.
    thread_manager_header = os.path.join(ROOT_DIR, "components/thread_manager/include/thread_manager/thread_manager.h")
    with open(thread_manager_header, "r", encoding="utf-8") as f:
        header_text = f.read()
    check("thread_manager_get_leader_address" in header_text,
          "thread_manager_get_leader_address() API itself is intentionally still present (for other diagnostic code)")

    # CMakeLists.txt for both components must no longer REQUIRES thread_manager
    # (unless some other symbol in that file still needs it -- verified above
    # that neither .c file references thread_manager/ at all any more).
    for comp_dir in ("sensor_transport", "topology_reporter"):
        cmake_path = os.path.join(ROOT_DIR, "components", comp_dir, "CMakeLists.txt")
        with open(cmake_path, "r", encoding="utf-8") as f:
            cmake_text = f.read()
        check("thread_manager" not in cmake_text,
              f"components/{comp_dir}/CMakeLists.txt no longer REQUIRES thread_manager")
        check("collector_endpoint" in cmake_text,
              f"components/{comp_dir}/CMakeLists.txt REQUIRES collector_endpoint")

    if failures == 0:
        print("ALL CHECKS PASSED")
        return 0
    print(f"{failures} CHECK(S) FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
