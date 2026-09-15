#!/usr/bin/env python3
"""
Confirms power_base/main/app_main.c uses the neutral
thread_manager_start_as_always_on_ftd() API and never calls
thread_manager_start_as_root() or logs a "ROOT:" prefix -- required test
item 5 of this correction pass's minimum test list: "power_base가
thread_manager_start_as_root()를 호출하지 않음".

power_base/main/app_main.c is not host-compilable (it is nothing but
ESP-IDF/OpenThread calls), so this is a structural/string check rather than
a build-time one -- the same convention tools/test_no_leader_rloc_dependency.py
already uses for a similar "component X must not reference API Y" check.

Usage:
    python3 tools/test_power_base_no_root_api.py
"""
import os
import re
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP_MAIN = os.path.join(ROOT_DIR, "power_base", "main", "app_main.c")
THREAD_MANAGER_H = os.path.join(ROOT_DIR, "components", "thread_manager", "include", "thread_manager",
                                 "thread_manager.h")
THREAD_MANAGER_C = os.path.join(ROOT_DIR, "components", "thread_manager", "thread_manager.c")


def strip_c_comments(text):
    """Removes /* ... */ and // ... comments (not string-literal-aware, but
    good enough for this repository's own source -- no comment delimiter
    ever appears inside a string literal here). Used so a call-site check
    ("never CALLS X") does not false-positive on a comment that merely
    MENTIONS X's name in prose (e.g. explaining that it is deliberately not
    used)."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def main():
    failures = 0

    def check(cond, msg):
        nonlocal failures
        print(("ok:   " if cond else "FAIL: ") + msg)
        if not cond:
            failures += 1

    with open(APP_MAIN, "r", encoding="utf-8") as f:
        app_main = f.read()
    app_main_code_only = strip_c_comments(app_main)

    # thread_manager_start_as_root( must never appear as an actual CALL in
    # power_base's own boot path (comments are allowed to mention the name
    # in prose, e.g. explaining that it is deliberately not used -- checked
    # against the comment-stripped text so that prose doesn't false-positive
    # here) -- only thread_manager_start_as_always_on_ftd().
    check(not re.search(r"\bthread_manager_start_as_root\s*\(", app_main_code_only),
          "power_base/main/app_main.c never calls thread_manager_start_as_root() (outside of comments)")
    check("thread_manager_start_as_always_on_ftd(" in app_main_code_only,
          "power_base/main/app_main.c calls thread_manager_start_as_always_on_ftd()")

    # The "ROOT:" log prefix is reserved for module's DEV_ROOT fixture (via
    # thread_manager_start_as_root()'s own unchanged log text) -- power_base
    # must never actually EMIT a "ROOT" log prefix itself (checked against
    # the comment-stripped text: a comment is allowed to mention the string
    # "ROOT:" while explaining it is never logged), and must log
    # "always-on FTD:" (from thread_manager.c) plus its own
    # "power-base FTD:" prefix instead.
    check('"ROOT' not in app_main_code_only,
          "power_base/main/app_main.c never emits a \"ROOT\" log prefix itself (outside of comments)")
    check("power-base FTD:" in app_main,
          "power_base/main/app_main.c logs its own boot/attach progress as \"power-base FTD:\"")

    # No fixed 30s halt for "not attached yet" -- the correction pass's other
    # required behavior change. A halt_forever() call must still exist (for
    # genuine resource failures), but never gated on a bounded attach-wait
    # deadline any more.
    check("halt_forever" in app_main_code_only,
          "power_base/main/app_main.c still halts on genuine resource-creation failures")
    check(not re.search(r"never (attached|reached LEADER) within 30s", app_main_code_only),
          "power_base/main/app_main.c no longer halts specifically because attach took longer than 30s")
    check("attach_supervisor_task" in app_main_code_only,
          "power_base/main/app_main.c uses an attach supervisor task for the (unbounded) first-attach wait")

    # thread_manager itself: the neutral API must exist, and
    # thread_manager_start_as_root() must still exist too (module's DEV_ROOT
    # fixture depends on it) -- this correction pass adds the new API, it
    # does not remove the old one.
    with open(THREAD_MANAGER_H, "r", encoding="utf-8") as f:
        header = f.read()
    check("thread_manager_start_as_always_on_ftd(void)" in header,
          "thread_manager.h declares thread_manager_start_as_always_on_ftd()")
    check("thread_manager_start_as_root(void)" in header,
          "thread_manager.h still declares thread_manager_start_as_root() (kept for module's DEV_ROOT fixture)")

    with open(THREAD_MANAGER_C, "r", encoding="utf-8") as f:
        impl = strip_c_comments(f.read())
    check(re.search(r"esp_err_t\s+thread_manager_start_as_always_on_ftd\s*\(\s*void\s*\)", impl) is not None,
          "thread_manager.c defines thread_manager_start_as_always_on_ftd()")
    check(re.search(r"esp_err_t\s+thread_manager_start_as_root\s*\(\s*void\s*\)", impl) is not None,
          "thread_manager.c still defines thread_manager_start_as_root()")
    # Both public functions must apply the IDENTICAL link-mode config --
    # checked structurally by confirming they are both thin wrappers that
    # call the SAME shared static implementation function, rather than each
    # hand-rolling its own (independently-driftable) otThreadSetLinkMode()
    # call. thread_manager_join_with_dataset() (a different function
    # entirely, for INTERMEDIATE/LEAF joins) legitimately has its own
    # separate otThreadSetLinkMode() call site -- this check does not touch
    # that function at all, it only inspects the two functions named below.
    def function_body(name):
        m = re.search(r"esp_err_t\s+" + re.escape(name) + r"\s*\(\s*void\s*\)\s*\{(.*?)\n\}", impl, re.DOTALL)
        return m.group(1) if m else None

    always_on_body = function_body("thread_manager_start_as_always_on_ftd")
    root_body = function_body("thread_manager_start_as_root")
    check(always_on_body is not None and root_body is not None,
          "both public functions' bodies were found for inspection")
    if always_on_body is not None and root_body is not None:
        check("otThreadSetLinkMode" not in always_on_body,
              "thread_manager_start_as_always_on_ftd() does not hand-roll its own otThreadSetLinkMode() call")
        check("otThreadSetLinkMode" not in root_body,
              "thread_manager_start_as_root() does not hand-roll its own otThreadSetLinkMode() call")
        shared_callee = re.search(r"return\s+(\w+)\s*\(", always_on_body)
        check(shared_callee is not None and shared_callee.group(1) in root_body,
              "both public functions delegate to the same shared underlying implementation function")

    # module/main/app_main.c must be unaffected: it still uses
    # thread_manager_start_as_root() for the DEV_ROOT fixture path (this
    # correction pass explicitly must not change sensor-module DEV_ROOT
    # behavior/logs).
    module_app_main = os.path.join(ROOT_DIR, "module", "main", "app_main.c")
    with open(module_app_main, "r", encoding="utf-8") as f:
        module_text = f.read()
    check("thread_manager_start_as_root(" in module_text,
          "module/main/app_main.c is unaffected -- still calls thread_manager_start_as_root() for its DEV_ROOT fixture")

    if failures == 0:
        print("ALL CHECKS PASSED")
        return 0
    print(f"{failures} CHECK(S) FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
