#!/usr/bin/env python3
"""
Repository-wide audit for stale, current-tense documentation claims that
conflict with this correction pass's policy -- required test items 6/7/8 of
this follow-up's minimum test list: "power_base 실행 경로 로그와 현재형
문서에 고정 ROOT/Leader 표현이 없음", "현재형 GPIO 문서에 DETECT_IN=GPIO2가
남아 있지 않음", "과거 기록의 문구가 테스트에 걸리면 명시적인 superseded
문맥만 허용".

Scope: every `*.md` file in the repository (README.md + docs/*.md) plus
`power_base/sdkconfig.defaults` -- the actual "documentation and policy
description" surface this correction pass's task targets ("문서와 로그
정합성 전수 검사"). Source-code comments in components/thread_manager/
(which legitimately discuss "a fixed Thread ROOT/Leader" in the abstract,
to explain why thread_manager_start_as_always_on_ftd() exists as a NEUTRAL
alternative -- never asserting that power_base currently IS one) are
intentionally out of scope for the ROOT/Leader phrase checks below; actual
runtime behavior (what power_base's own code calls/logs) is checked
separately and more precisely by tools/test_power_base_no_root_api.py,
which greps only power_base/main/app_main.c's real call sites and log
strings (with comments stripped first).

Two independent ways a match is exempted as "clearly historical/superseded",
matching this task's "명확한 current-policy 구역을 우선 검증" instruction:

  1. HEADING INHERITANCE: markdown headings form a hierarchy (# > ## >
     ### > ...). If an ENCLOSING heading (this line's own section, or any
     ancestor section above it) has a historical marker in its heading text
     or the first ~400 characters of its body, every match inside that
     heading's whole subtree (including nested subheadings that don't
     themselves repeat the marker) is exempted -- e.g. a `####` block nested
     under a `###` heading titled "... (과거 정책, SUPERSEDED)" inherits
     that status.
  2. LOCAL WINDOW: independent of heading structure, if a historical marker
     appears within LOCAL_WINDOW_CHARS characters before or after the match
     itself (covers a long, un-subdivided section -- e.g. this repository's
     README.md top preamble, or a prose paragraph that says "이 표현은 ...
     대체되었다" right next to the quoted old phrase without a dedicated
     heading of its own).

A match exempted by NEITHER is reported as a stale, current-tense claim.

Usage:
    python3 tools/test_no_stale_root_leader_claims.py
"""
import glob
import os
import re
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LOCAL_WINDOW_CHARS = 600
HEADING_LEAD_CHARS = 400

HISTORICAL_MARKERS = [
    "superseded", "supersedes", "correction pass",
    "과거 정책", "과거 기록", "당시 firmware", "이전 세션",
    "사용 중단", "더 이상 유효하지 않다", "이제 유효하지 않다",
    "historical", "deprecated", "구버전", "재수정", "대체되었다", "바뀌었다", "수정되었다",
    "역사적 기록", "old (pre-", "was fixed to", "was bounded", "previously",
    "originally", "before this", "at the time", "no longer", "this pass found",
    "pre-this-session", "the old", "the original", "used to", "removed", "relaxed",
    "replaced", "이 정책", "고쳤다", "제거되었다", "완화", "이전", "재배선",
    "더 이상", "교환했다", "교환되었다",
]

FORBIDDEN_PATTERNS = [
    ("UPSTREAM DETECT_IN = GPIO2 (current-tense)",
     re.compile(r"UPSTREAM DETECT_IN\s*(?:은|=)\s*GPIO ?2\b")),
    ("'유일한 Leader'", re.compile(r"유일한\s*Leader")),
    ("'one and only ROOT/Leader'", re.compile(r"one and only ROOT/Leader", re.IGNORECASE)),
    ("'strictly LEADER' / '엄격히 LEADER'", re.compile(r"strictly LEADER|엄격히\s*LEADER", re.IGNORECASE)),
    ("'ROOT is always Leader'", re.compile(r"ROOT is always Leader", re.IGNORECASE)),
    ("'Thread ROOT/Leader'", re.compile(r"Thread ROOT/Leader")),
]

HEADING_RE = re.compile(r"^(#{1,6})\s")


def find_target_files():
    files = [os.path.join(ROOT_DIR, "README.md")]
    files += sorted(glob.glob(os.path.join(ROOT_DIR, "docs", "*.md")))
    files.append(os.path.join(ROOT_DIR, "power_base", "sdkconfig.defaults"))
    return [f for f in files if os.path.isfile(f)]


def has_marker(text):
    lower = text.lower()
    return any(marker.lower() in lower for marker in HISTORICAL_MARKERS)


def heading_level(line):
    m = HEADING_RE.match(line)
    return len(m.group(1)) if m else None


def compute_line_historical_flags(text):
    """Returns a list, one bool per line, saying whether that line falls
    within a heading subtree whose own heading (or that heading's immediate
    ancestors) was marked historical -- see this file's top comment,
    exemption #1."""
    lines = text.splitlines(keepends=True)
    flags = [False] * len(lines)
    stack = []  # list of (level, is_historical)

    # Pre-scan: for each heading line index, find its own "lead text"
    # (heading line + body up to the NEXT heading of any level, truncated
    # to HEADING_LEAD_CHARS) to check for a marker in that heading's own
    # introduction.
    heading_indices = [i for i, line in enumerate(lines) if heading_level(line) is not None]

    def own_lead_text(idx):
        end = heading_indices[heading_indices.index(idx) + 1] if idx in heading_indices and \
            heading_indices.index(idx) + 1 < len(heading_indices) else len(lines)
        body = "".join(lines[idx:end])
        return body[:HEADING_LEAD_CHARS + len(lines[idx])]

    for i, line in enumerate(lines):
        lvl = heading_level(line)
        if lvl is not None:
            while stack and stack[-1][0] >= lvl:
                stack.pop()
            own_historical = has_marker(own_lead_text(i))
            parent_historical = stack[-1][1] if stack else False
            stack.append((lvl, own_historical or parent_historical))
        flags[i] = stack[-1][1] if stack else False
    return flags


def audit_file(path, failures_ref):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    rel = os.path.relpath(path, ROOT_DIR)

    line_historical = compute_line_historical_flags(text)
    # Map each character offset to a line index for fast lookup.
    line_starts = [0]
    for line in text.splitlines(keepends=True):
        line_starts.append(line_starts[-1] + len(line))

    def line_index_for_offset(offset):
        # binary-search-free linear scan is fine here (files are small)
        lo, hi = 0, len(line_starts) - 2
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if line_starts[mid] <= offset:
                lo = mid
            else:
                hi = mid - 1
        return lo

    file_had_violation = False
    for label, pattern in FORBIDDEN_PATTERNS:
        for m in pattern.finditer(text):
            li = line_index_for_offset(m.start())
            exempt_by_heading = line_historical[li] if li < len(line_historical) else False
            window_start = max(0, m.start() - LOCAL_WINDOW_CHARS)
            window_end = min(len(text), m.end() + LOCAL_WINDOW_CHARS)
            exempt_by_window = has_marker(text[window_start:window_end])
            if exempt_by_heading or exempt_by_window:
                continue
            line_no = li + 1
            print(f"FAIL: {rel}:{line_no}: current-tense stale claim {label} found with no historical/superseded "
                  f"marker nearby (neither an enclosing heading nor within {LOCAL_WINDOW_CHARS} chars)")
            failures_ref[0] += 1
            file_had_violation = True
    if not file_had_violation:
        print(f"ok:   {rel} has no current-tense stale ROOT/Leader/GPIO2 claims outside marked-historical context")


def self_test(failures_ref):
    print()
    violation = "## Some current section\n\n" + ("x" * 50) + "\n\nThe power base is the system's one and only ROOT/Leader.\n"
    historical_heading = ("## Old policy (SUPERSEDED)\n\nThe power base is the system's one and only "
                           "ROOT/Leader. This has since changed.\n")
    historical_nested = ("## Old policy (SUPERSEDED, 과거 정책)\n\n" + ("y" * 500) +
                          "\n\n### A nested detail\n\nThe power base is the system's one and only ROOT/Leader.\n")
    historical_local = "## Some section\n\nThis phrase was later replaced: the power base is the system's one and only ROOT/Leader.\n"

    def violations_in(sample_text):
        flags = compute_line_historical_flags(sample_text)
        n = 0
        line_starts = [0]
        for line in sample_text.splitlines(keepends=True):
            line_starts.append(line_starts[-1] + len(line))

        def li_for(offset):
            lo, hi = 0, len(line_starts) - 2
            while lo < hi:
                mid = (lo + hi + 1) // 2
                if line_starts[mid] <= offset:
                    lo = mid
                else:
                    hi = mid - 1
            return lo

        for _label, pattern in FORBIDDEN_PATTERNS:
            for m in pattern.finditer(sample_text):
                li = li_for(m.start())
                exempt_h = flags[li] if li < len(flags) else False
                ws, we = max(0, m.start() - LOCAL_WINDOW_CHARS), min(len(sample_text), m.end() + LOCAL_WINDOW_CHARS)
                exempt_w = has_marker(sample_text[ws:we])
                if not (exempt_h or exempt_w):
                    n += 1
        return n

    checks = [
        (violations_in(violation) >= 1, "self-test: a violation in a plain non-historical section (far from any marker) is detected"),
        (violations_in(historical_heading) == 0, "self-test: the same text under a heading literally saying SUPERSEDED is exempted"),
        (violations_in(historical_nested) == 0, "self-test: a NESTED subheading under a SUPERSEDED parent heading is exempted (heading inheritance)"),
        (violations_in(historical_local) == 0, "self-test: the same text with a local 'was later replaced' phrase nearby is exempted (local window)"),
    ]
    for ok, msg in checks:
        print(("ok:   " if ok else "FAIL: ") + msg)
        if not ok:
            failures_ref[0] += 1


def main():
    failures = [0]
    targets = find_target_files()
    for path in targets:
        audit_file(path, failures)

    self_test(failures)

    print()
    print(f"audited {len(targets)} file(s)")
    if failures[0] == 0:
        print("ALL CHECKS PASSED")
        return 0
    print(f"{failures[0]} CHECK(S) FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
