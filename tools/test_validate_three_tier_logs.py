#!/usr/bin/env python3
"""
Host test for tools/validate_three_tier_logs.py.

Builds small synthetic 3-board monitor-log fixtures (in a temp dir) using
the EXACT log line formats the real firmware emits (pogo_port.c,
role_manager.c, power_base_transport.c, sensor_root_aggregator.c,
child_service.c, app_main.c -- see validate_three_tier_logs.py's own
docstring for the cited source of each), invokes the tool as a SUBPROCESS
for every scenario (never imports it -- the tool's module-level `_results`
list is never reset between calls, so importing and calling main()
repeatedly in one process would leak failures across scenarios; a fresh
subprocess per scenario sidesteps that entirely and also matches how a real
user actually runs this tool), and confirms:

  - a clean 3-tier log set PASSes (exit code 0)
  - the SAME clean 3-tier log set, with every line wrapped in ANSI color
    escape codes the way `idf.py monitor | tee some.log` actually saves them
    (INTERMEDIATE's and LEAF's own "role changed" lines specifically include
    a "\x1b[0m" reset, the exact byte sequence a real colorized capture
    would have right around those role names), STILL PASSes identically --
    read_lines() strips ANSI escapes before any regex runs, so a colorized
    log is judged by the exact same criteria as a plain one
  - each of these single defects independently FAILs (exit code 1), with
    the specific expected "FAIL: ..." line present in the tool's stdout:
      1. a missing PEER_READY on the middle module's own upstream link
      2. the middle module never settling to logical role INTERMEDIATE
      3. a missing depth=2 topology report at the Power Base
      4. the SAME uuid reported at both depth=1 and depth=2 (the exact
         UUID/depth-mixup class this tool exists to catch)
      5. a missing sensor sample from one of the two registered uuids
      6. a missing JOIN_RESULT_ACK on one hop
      7. a panic marker
      8. a protocol-conflict marker
      9. a capability-mismatch marker
     10. >=3 boot banners in one board's own log (reboot loop)

Usage:
    python3 tools/test_validate_three_tier_logs.py
"""
import os
import subprocess
import sys
import tempfile

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL_PATH = os.path.join(ROOT_DIR, "tools", "validate_three_tier_logs.py")

failures = 0


def check(cond, msg):
    global failures
    print(("ok:   " if cond else "FAIL: ") + msg)
    if not cond:
        failures += 1


# --- Baseline fixture: a clean, passing 3-tier chain. Every line's format
# matches the real production log line it stands in for (see this file's
# module docstring and validate_three_tier_logs.py's own docstring for the
# exact source of each). ---
GOOD_BASE = [
    "I (100) app_main: entry (reset_reason=1)",
    "I (200) pogo_port: port 1: PEER_READY (peer_type=2)",
    "I (300) power_base_transport: topology report applied: uuid=..a1b2 depth=1 role=2 type=0",
    "I (310) power_base_transport: topology report applied: uuid=..c3d4 depth=2 role=3 type=0",
    "I (400) sensor_root_aggregator: sample recorded: uuid=..a1b2 seq=1 channels=2",
    "I (410) sensor_root_aggregator: sample recorded: uuid=..c3d4 seq=1 channels=3",
]
GOOD_MIDDLE = [
    "I (50) app_main: entry (reset_reason=1)",
    "I (100) pogo_port: port 0: PEER_READY (peer_type=1)",
    "I (150) role_manager: role changed: UNASSIGNED -> INTERMEDIATE",
    "I (200) pogo_port: port 1: PEER_READY (peer_type=2)",
    "I (250) child_service: JOIN_RESULT_ACK received (seq=1) -- provisioning session fully confirmed",
]
GOOD_LEAF = [
    "I (50) app_main: entry (reset_reason=1)",
    "I (100) pogo_port: port 0: PEER_READY (peer_type=2)",
    "I (150) role_manager: role changed: UNASSIGNED -> LEAF",
    "I (200) child_service: JOIN_RESULT_ACK received (seq=1) -- provisioning session fully confirmed",
]


def ansi_wrap(line, code="0;32"):
    """Wraps `line` the way `idf.py monitor`'s own colorizer does: an SGR
    color-select escape at the front and a reset ("\\x1b[0m") at the end.
    `idf.py monitor | tee some.log` preserves these bytes verbatim in the
    saved file -- see validate_three_tier_logs.py's own ANSI_ESCAPE_RE
    comment for why read_lines() must strip them before any regex runs."""
    return f"\x1b[{code}m{line}\x1b[0m"


def write_log(dir_path, name, lines):
    path = os.path.join(dir_path, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return path


def run_tool(tmp_dir, base_lines, middle_lines, leaf_lines):
    base_path = write_log(tmp_dir, "base.log", base_lines)
    middle_path = write_log(tmp_dir, "middle.log", middle_lines)
    leaf_path = write_log(tmp_dir, "leaf.log", leaf_lines)
    result = subprocess.run(
        [sys.executable, TOOL_PATH, "--base", base_path, "--middle", middle_path, "--leaf", leaf_path],
        capture_output=True, text=True, check=False,
    )
    return result.returncode, result.stdout


def scenario(tmp_dir, name, base_lines, middle_lines, leaf_lines, expect_pass, expect_fail_substring=None):
    returncode, stdout = run_tool(tmp_dir, base_lines, middle_lines, leaf_lines)
    if expect_pass:
        check(returncode == 0, f"{name}: exit code 0 (PASS)")
        check("ALL " in stdout and "CHECKS PASSED" in stdout, f"{name}: output reports ALL CHECKS PASSED")
    else:
        check(returncode == 1, f"{name}: exit code 1 (FAIL)")
        check(expect_fail_substring in stdout,
              f"{name}: output contains the expected failure line ({expect_fail_substring!r})")


def without(lines, substring):
    return [line for line in lines if substring not in line]


def main():
    with tempfile.TemporaryDirectory() as tmp_dir:
        # 1. A clean 3-tier chain PASSes.
        scenario(tmp_dir, "clean 3-tier chain", GOOD_BASE, GOOD_MIDDLE, GOOD_LEAF, expect_pass=True)

        # 1b. The same clean chain, captured the way a real
        # `idf.py monitor | tee some.log` session saves it -- every line
        # colorized, with a "\x1b[0m" reset specifically inside the
        # INTERMEDIATE and LEAF "role changed" lines (the two lines
        # ROLE_CHANGED_RE has to match against) -- must PASS exactly the
        # same as the plain, colorless fixture above.
        color_base = [ansi_wrap(line, "0;36") for line in GOOD_BASE]
        color_middle = [ansi_wrap(line, "0;36") for line in GOOD_MIDDLE]
        color_leaf = [ansi_wrap(line, "0;36") for line in GOOD_LEAF]
        assert any("-> INTERMEDIATE" in line and "\x1b[0m" in line for line in color_middle), \
            "test fixture bug: the colorized INTERMEDIATE role line must contain \\x1b[0m"
        assert any("-> LEAF" in line and "\x1b[0m" in line for line in color_leaf), \
            "test fixture bug: the colorized LEAF role line must contain \\x1b[0m"
        scenario(tmp_dir, "clean 3-tier chain with ANSI color codes (idf.py monitor | tee)",
                 color_base, color_middle, color_leaf, expect_pass=True)

        # 2. A missing PEER_READY (middle module's own upstream link) FAILs.
        bad_middle = without(GOOD_MIDDLE, "port 0: PEER_READY")
        scenario(tmp_dir, "missing PEER_READY (middle upstream)", GOOD_BASE, bad_middle, GOOD_LEAF,
                 expect_pass=False,
                 expect_fail_substring="FAIL: Base<->Middle link: Middle module's upstream POGO port logged PEER_READY")

        # 3. Middle module's role never settles to INTERMEDIATE FAILs.
        bad_middle_role = [line.replace("-> INTERMEDIATE", "-> ROLE_ERROR") for line in GOOD_MIDDLE]
        scenario(tmp_dir, "middle role never settles to INTERMEDIATE", GOOD_BASE, bad_middle_role, GOOD_LEAF,
                 expect_pass=False,
                 expect_fail_substring="FAIL: Middle module's own role_manager settled to INTERMEDIATE")

        # 4. A missing depth=2 topology report at the Power Base FAILs.
        bad_base_no_depth2 = without(GOOD_BASE, "depth=2")
        scenario(tmp_dir, "missing depth=2 topology report", bad_base_no_depth2, GOOD_MIDDLE, GOOD_LEAF,
                 expect_pass=False, expect_fail_substring="FAIL: Power Base registry saw depth=2")

        # 5. The SAME uuid reported at both depth=1 and depth=2 FAILs (the
        # exact UUID/depth-mixup class this tool exists to catch).
        mixed_uuid_base = [line.replace("uuid=..c3d4 depth=2", "uuid=..a1b2 depth=2") for line in GOOD_BASE]
        scenario(tmp_dir, "same uuid reported at depth=1 and depth=2", mixed_uuid_base, GOOD_MIDDLE, GOOD_LEAF,
                 expect_pass=False,
                 expect_fail_substring="possible UUID/depth mixup")

        # 6. A missing sensor sample from one of the two registered uuids FAILs.
        bad_base_no_sample = without(GOOD_BASE, "sample recorded: uuid=..c3d4")
        scenario(tmp_dir, "missing sensor sample for one uuid", bad_base_no_sample, GOOD_MIDDLE, GOOD_LEAF,
                 expect_pass=False, expect_fail_substring="FAIL: Power Base recorded sensor samples from")

        # 7. A missing JOIN_RESULT_ACK on one hop (the leaf's own) FAILs.
        bad_leaf_no_ack = without(GOOD_LEAF, "JOIN_RESULT_ACK")
        scenario(tmp_dir, "missing JOIN_RESULT_ACK (leaf hop)", GOOD_BASE, GOOD_MIDDLE, bad_leaf_no_ack,
                 expect_pass=False,
                 expect_fail_substring="FAIL: Leaf module logged JOIN_RESULT_ACK received")

        # 8. A panic marker anywhere FAILs.
        panic_base = GOOD_BASE + ["E (500) some_task: Guru Meditation Error: Core 0 panic'ed (LoadProhibited)"]
        scenario(tmp_dir, "panic marker present", panic_base, GOOD_MIDDLE, GOOD_LEAF,
                 expect_pass=False, expect_fail_substring="panic detected")

        # 9. A protocol-conflict marker anywhere FAILs.
        conflict_middle = GOOD_MIDDLE + ["E (260) pogo_port: port 0: PEER_TYPE_CONFLICT (BASE_ID=LOW, hello=MODULE)"]
        scenario(tmp_dir, "protocol conflict marker present", GOOD_BASE, conflict_middle, GOOD_LEAF,
                 expect_pass=False, expect_fail_substring="protocol_conflict detected")

        # 10. A capability-mismatch marker anywhere FAILs.
        mismatch_leaf = GOOD_LEAF + [
            "E (260) pogo_port: port 0: peer's HELLO capabilities=0x00 is missing "
            "POGO_CAP_COLLECTOR_PROVISIONING -- refusing"
        ]
        scenario(tmp_dir, "capability mismatch marker present", GOOD_BASE, GOOD_MIDDLE, mismatch_leaf,
                 expect_pass=False, expect_fail_substring="capability_mismatch detected")

        # 11. >=3 boot banners in one board's own log (reboot loop) FAILs.
        reboot_loop_base = GOOD_BASE + [
            "I (1000) app_main: entry (reset_reason=1)",
            "I (2000) app_main: entry (reset_reason=1)",
        ]
        scenario(tmp_dir, "reboot loop (>=3 boot banners)", reboot_loop_base, GOOD_MIDDLE, GOOD_LEAF,
                 expect_pass=False, expect_fail_substring="reboot loop, not a normal single boot")

    if failures == 0:
        print("ALL CHECKS PASSED")
        return 0
    print(f"{failures} CHECK(S) FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
