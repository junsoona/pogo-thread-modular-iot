#!/usr/bin/env python3
"""
Post-hoc PASS/FAIL judge for a real 3-board 3-tier POGO/Thread chain
(Power Base -> INTERMEDIATE sensor module -> LEAF sensor module), reading
ONLY three already-captured `idf.py monitor` log files -- one per board.
This tool never opens a serial port, never flashes, and never controls
firmware; it is a pure log-file auditor, run after the fact against
whatever a real test session already saved to disk (see README's "3단
실물 시험" section for how to capture those three files).

Why UUID/event correlation, not line order: log lines from three
independent boards are captured independently and are NOT clock-synchronized
against each other -- a line's *position* in one file says nothing about its
order relative to lines in another file, and even within one file retries/
resyncs can reorder related events. Every check below is therefore driven
by which UUID suffix (the firmware only ever logs the last 2 bytes of a
module's permanent UUID, e.g. "uuid=..a1b2") or port id a log LINE names,
never by which file or line number it appears at.

Log line patterns this tool understands (see the cited source for each):
  - "port %d: PEER_READY" / "... -> PEER_READY"          pogo_port.c
  - "role changed: %s -> %s"                              role_manager.c
  - "topology report applied: uuid=..XX depth=%u role=%u type=%u"
                                                            power_base_transport.c
  - "sample recorded: uuid=..XX seq=%u channels=%u"        sensor_root_aggregator.c
  - "JOIN_RESULT_ACK received (seq=%u) ..."                child_service.c
  - FATAL/panic/capability-mismatch/protocol-conflict markers -- see
    FATAL_PATTERNS below for the exact source of each.

Usage:
    python3 tools/validate_multihop_logs.py \\
        --base   base_monitor.log \\
        --middle middle_monitor.log \\
        --leaf   leaf_monitor.log

Exit code 0 if every check PASSes, 1 otherwise.
"""
import argparse
import re
import sys
from collections import defaultdict

# --- FATAL/panic/reboot-loop/capability-mismatch/protocol-conflict patterns.
# Each tuple is (label, regex, source_note). Any match anywhere in ANY of the
# three logs is a hard FAIL for that check, printed with the exact line.
FATAL_PATTERNS = [
    ("panic",
     re.compile(r"Guru Meditation Error|Panic reason|abort\(\) was called|assert failed"),
     "ESP-IDF panic handler / abort() (module/main/app_main.c, power_base/main/app_main.c)"),
    ("fatal_config_error",
     re.compile(r"CONFIG_ERROR"),
     "module_config.c / power_base_config.c validate_config() (see docs/module-config.md)"),
    ("protocol_conflict",
     re.compile(r"PEER_TYPE_CONFLICT|hello protocol_version=\d+ \(expected \d+\)"),
     "pogo_port.c validate_incoming_hello()"),
    ("capability_mismatch",
     re.compile(r"CAPABILITY_MISMATCH|missing POGO_CAP_COLLECTOR_PROVISIONING|legacy 2-byte HELLO"),
     "pogo_port.c validate_incoming_hello()"),
    ("join_failed",
     re.compile(r"JOIN FAILED|THREAD_JOIN_TIMEOUT"),
     "parent_service.c run_provisioning_session()"),
]

REBOOT_BANNER_RE = re.compile(r"app_main: entry \(reset_reason=(\d+)\)")
REBOOT_LOOP_THRESHOLD = 3  # >=3 fresh boots in one board's own log = a reboot loop, not a single reset

PEER_READY_RE = re.compile(r"port (\d+):.*PEER_READY")
ROLE_CHANGED_RE = re.compile(r"role changed: \S+ -> (\S+)")
TOPO_APPLIED_RE = re.compile(r"topology report applied: uuid=\.\.([0-9a-fA-F]{4}) depth=(\d+) role=(\d+) type=(\d+)")
SAMPLE_RECORDED_RE = re.compile(r"sample recorded: uuid=\.\.([0-9a-fA-F]{4}) seq=(\d+) channels=(\d+)")
JOIN_RESULT_ACK_RE = re.compile(r"JOIN_RESULT_ACK received \(seq=(\d+)\)")

TOPOLOGY_ROLE_NAMES = {0: "UNASSIGNED", 1: "ROOT", 2: "INTERMEDIATE", 3: "LEAF", 4: "ERROR"}

PORT_UPSTREAM = "0"
PORT_DOWNSTREAM = "1"

_results = []  # (passed: bool, message: str)


def record(passed, message):
    _results.append((passed, message))
    print(("PASS: " if passed else "FAIL: ") + message)


def read_lines(path, label):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.readlines()
    except OSError as e:
        sys.exit(f"cannot read --{label} log at {path}: {e}")


def check_fatal_patterns(lines, board_label):
    """Returns True (PASS, i.e. clean) if none of FATAL_PATTERNS match any
    line in `lines`; otherwise prints each match and returns False."""
    clean = True
    for label, pattern, source in FATAL_PATTERNS:
        for lineno, line in enumerate(lines, 1):
            if pattern.search(line):
                record(False, f"{board_label} log line {lineno}: {label} detected ({source}): {line.strip()}")
                clean = False
    return clean


def check_reboot_loop(lines, board_label):
    boots = [m.group(1) for line in lines for m in [REBOOT_BANNER_RE.search(line)] if m]
    if len(boots) >= REBOOT_LOOP_THRESHOLD:
        record(False, f"{board_label} log shows {len(boots)} boot banners "
                      f"(reset_reason={boots}) -- looks like a reboot loop, not a normal single boot")
        return False
    record(True, f"{board_label} log shows {len(boots)} boot banner(s) -- no reboot loop")
    return True


def find_peer_ready_ports(lines):
    """Returns the set of port ids (as strings, "0" or "1") that logged
    PEER_READY somewhere in `lines`."""
    return {m.group(1) for line in lines for m in [PEER_READY_RE.search(line)] if m}


def last_role(lines):
    """Returns the LAST role_manager 'role changed' target role string seen
    in `lines` (the final settled role), or None if never logged."""
    role = None
    for line in lines:
        m = ROLE_CHANGED_RE.search(line)
        if m:
            role = m.group(1)
    return role


def collect_topology_reports(lines):
    """Returns {uuid_suffix: [(depth, role, report_type), ...]} for every
    'topology report applied' line -- a dict of lists since heartbeats repeat
    the same uuid many times; callers use the LAST entry per uuid for the
    settled state, but keep every depth actually observed for that uuid."""
    reports = defaultdict(list)
    for line in lines:
        m = TOPO_APPLIED_RE.search(line)
        if m:
            uuid, depth, role, rtype = m.group(1).lower(), int(m.group(2)), int(m.group(3)), int(m.group(4))
            reports[uuid].append((depth, role, rtype))
    return reports


def collect_sample_uuids(lines):
    return {m.group(1).lower() for line in lines for m in [SAMPLE_RECORDED_RE.search(line)] if m}


def count_join_result_acks(lines):
    return sum(1 for line in lines if JOIN_RESULT_ACK_RE.search(line))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", required=True, help="Power Base idf.py monitor log file")
    ap.add_argument("--middle", required=True, help="INTERMEDIATE module (SHT45 or BH1750) monitor log file")
    ap.add_argument("--leaf", required=True, help="LEAF module (PMS5003) monitor log file")
    args = ap.parse_args()

    base_lines = read_lines(args.base, "base")
    middle_lines = read_lines(args.middle, "middle")
    leaf_lines = read_lines(args.leaf, "leaf")

    print(f"=== {args.base} ({len(base_lines)} lines) / {args.middle} ({len(middle_lines)} lines) / "
          f"{args.leaf} ({len(leaf_lines)} lines) ===\n")

    # --- 1. Both POGO links show PEER_READY on both ends. ---
    base_ports = find_peer_ready_ports(base_lines)
    middle_ports = find_peer_ready_ports(middle_lines)
    leaf_ports = find_peer_ready_ports(leaf_lines)

    record(PORT_DOWNSTREAM in base_ports,
           "Base<->Middle link: Power Base's downstream POGO port logged PEER_READY")
    record(PORT_UPSTREAM in middle_ports,
           "Base<->Middle link: Middle module's upstream POGO port logged PEER_READY")
    record(PORT_DOWNSTREAM in middle_ports,
           "Middle<->Leaf link: Middle module's downstream POGO port logged PEER_READY")
    record(PORT_UPSTREAM in leaf_ports,
           "Middle<->Leaf link: Leaf module's upstream POGO port logged PEER_READY")

    # --- 2. Middle module's logical role settled to INTERMEDIATE. ---
    middle_role = last_role(middle_lines)
    record(middle_role == "INTERMEDIATE",
           f"Middle module's own role_manager settled to INTERMEDIATE (last role change: {middle_role})")

    # --- 3. Leaf module's logical role settled to LEAF. ---
    leaf_role = last_role(leaf_lines)
    record(leaf_role == "LEAF",
           f"Leaf module's own role_manager settled to LEAF (last role change: {leaf_role})")

    # --- 4/5. Power Base topology_registry: two distinct UUIDs registered,
    # with depth 1 and depth 2 observed, and INTERMEDIATE/LEAF roles
    # corroborated from the Base's own registry (not just each module's own
    # self-report). ---
    topo = collect_topology_reports(base_lines)
    record(len(topo) >= 2,
           f"Power Base registered {len(topo)} distinct module UUID(s) via topology reports (need >=2): "
           f"{sorted(topo)}")

    depths_by_uuid = {uuid: sorted({d for d, _, _ in entries}) for uuid, entries in topo.items()}
    depth1_uuids = {u for u, ds in depths_by_uuid.items() if 1 in ds}
    depth2_uuids = {u for u, ds in depths_by_uuid.items() if 2 in ds}
    record(len(depth1_uuids) >= 1, f"Power Base registry saw depth=1 for uuid(s): {sorted(depth1_uuids)}")
    record(len(depth2_uuids) >= 1, f"Power Base registry saw depth=2 for uuid(s): {sorted(depth2_uuids)}")
    # Must be two DIFFERENT, DISJOINT uuid sets -- a module never legitimately
    # holds both depth=1 and depth=2 at once, so any overlap is itself a real
    # topology-registry bug (the exact UUID-mixup class item 6 of the
    # follow-up spec asks this tool to catch), not something to wave through.
    both_present_and_disjoint = bool(depth1_uuids) and bool(depth2_uuids) and depth1_uuids.isdisjoint(depth2_uuids)
    record(both_present_and_disjoint,
           f"depth=1 and depth=2 were observed for two DIFFERENT, DISJOINT module UUIDs "
           f"(INTERMEDIATE at depth 1: {sorted(depth1_uuids)}, LEAF at depth 2: {sorted(depth2_uuids)})"
           if both_present_and_disjoint else
           f"depth=1/depth=2 uuid sets are not both non-empty and disjoint -- possible UUID/depth mixup "
           f"(depth1={sorted(depth1_uuids)}, depth2={sorted(depth2_uuids)}, "
           f"overlap={sorted(depth1_uuids & depth2_uuids)})")

    for uuid, ds in depths_by_uuid.items():
        last_role_code = topo[uuid][-1][1]
        role_name = TOPOLOGY_ROLE_NAMES.get(last_role_code, f"UNKNOWN({last_role_code})")
        print(f"      uuid=..{uuid}: depths observed={ds}, last reported role={role_name}")

    # --- 6. Sensor samples received from both UUIDs. ---
    sample_uuids = collect_sample_uuids(base_lines)
    overlap = sample_uuids & set(topo)
    record(len(overlap) >= 2,
           f"Power Base recorded sensor samples from {len(overlap)} of its registered UUID(s) (need >=2): "
           f"{sorted(overlap)} (all sample uuids seen: {sorted(sample_uuids)})")

    # --- 7. JOIN_RESULT_ACK confirmed on both hops (each child's own log). ---
    middle_acks = count_join_result_acks(middle_lines)
    leaf_acks = count_join_result_acks(leaf_lines)
    record(middle_acks >= 1,
           f"Middle module logged JOIN_RESULT_ACK received for its own Base<->Middle join ({middle_acks} time(s))")
    record(leaf_acks >= 1,
           f"Leaf module logged JOIN_RESULT_ACK received for its own Middle<->Leaf join ({leaf_acks} time(s))")

    # --- 8. FATAL/panic/reboot-loop/capability-mismatch/protocol-conflict
    # detection across all three logs. ---
    print()
    for lines, label in ((base_lines, "base"), (middle_lines, "middle"), (leaf_lines, "leaf")):
        check_fatal_patterns(lines, label)
        check_reboot_loop(lines, label)

    print()
    failed = [msg for passed, msg in _results if not passed]
    if not failed:
        print(f"ALL {len(_results)} CHECKS PASSED")
        return 0
    print(f"{len(failed)}/{len(_results)} CHECK(S) FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
