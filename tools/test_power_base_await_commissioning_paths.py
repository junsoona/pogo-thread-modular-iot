#!/usr/bin/env python3
"""
Structural checks for the pb_thread:mode=2 glue that only builds under
ESP-IDF (power_base/main/app_main.c, enrollment_runtime.c,
enrollment_console.c, components/thread_manager). The enrollment RULES are
tested on real code in tools/test_power_base_enrollment.c; this file guards
the wiring those rules rely on, the same way tools/test_power_base_no_root_api.py
guards app_main.c:

  1. thread_manager_start_as_always_on_ftd_stored() never calls
     esp_openthread_auto_start() (the only path that forms a network).
  2. app_main.c's AWAIT_COMMISSIONING branch starts nothing itself: no
     Thread start, supervisor, power rail, exporter or POGO port; the only
     mode-2 route to start_attach_supervisor() is on_enrollment_ready().
  3. The Joiner callback (OpenThread task, API lock held) takes no lock and
     makes no OpenThread call.
  4. No ESP_LOGx/printf statement anywhere in the enrollment code passes the
     `pskd` variable; the console never echoes its arguments.
  5. Default builds keep the Joiner, the bench console and the OpenThread CLI
     off; the bench overlay never turns the OpenThread CLI on.

Usage:
    python3 tools/test_power_base_await_commissioning_paths.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(rel):
    with open(os.path.join(ROOT, rel), "r", encoding="utf-8") as f:
        return f.read()


def strip_c_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def function_body(code, signature_regex):
    """Body (between the outermost braces) of the first function whose
    definition matches signature_regex, or None."""
    m = re.search(signature_regex + r"[^;{]*\{", code)
    if not m:
        return None
    depth, i = 1, m.end()
    while depth and i < len(code):
        depth += {"{": 1, "}": -1}.get(code[i], 0)
        i += 1
    return code[m.end():i - 1]


def block_after(code, opener_regex):
    """Text of the first `if (...) {` block matching opener_regex."""
    return function_body(code, opener_regex)


failures = 0


def check(cond, msg):
    global failures
    print(("ok:   " if cond else "FAIL: ") + msg)
    if not cond:
        failures += 1


def main():
    tm = strip_c_comments(read("components/thread_manager/thread_manager.c"))
    body = function_body(tm, r"esp_err_t\s+thread_manager_start_as_always_on_ftd_stored\s*\(")
    check(body is not None, "thread_manager_start_as_always_on_ftd_stored() found")
    check(body is not None and "esp_openthread_auto_start" not in body,
          "stored-dataset start never calls esp_openthread_auto_start()")
    check(body is not None and "otThreadSetEnabled(instance, true)" in body and "has_network_key" in body,
          "stored-dataset start enables Thread only after checking a stored Network Key")

    app = strip_c_comments(read("power_base/main/app_main.c"))
    main_body = function_body(app, r"void\s+app_main\s*\(\s*void\s*\)")
    check(main_body is not None, "app_main() found")
    init_pos = main_body.find("thread_manager_init()")
    await_block = block_after(main_body[init_pos:], r"if\s*\(\s*s_thread_cfg\.mode\s*==\s*"
                                                   r"POWER_BASE_THREAD_MODE_AWAIT_COMMISSIONING\s*\)")
    check(await_block is not None and "return;" in await_block, "AWAIT branch after thread_manager_init() returns")
    forbidden = ["thread_manager_start_as_always_on_ftd", "start_attach_supervisor", "attach_supervisor_task",
                 "apply_power_rail_sequencing", "start_services_once", "linux_bridge_exporter_init",
                 "pogo_port_enable"]
    for name in forbidden:
        check(await_block is not None and name not in await_block, f"AWAIT branch does not call {name}")
    check(await_block is not None and "thread_manager_stop()" in await_block and
          "pb_enrollment_runtime_start(" in await_block,
          "AWAIT branch stops Thread and hands over to the enrollment runtime")
    rail_guard = re.search(r"if\s*\(\s*s_thread_cfg\.mode\s*!=\s*POWER_BASE_THREAD_MODE_AWAIT_COMMISSIONING\s*\)\s*\{"
                           r"\s*apply_power_rail_sequencing\(\);", main_body)
    check(rail_guard is not None, "boot-time power rail sequencing is skipped in mode 2")
    ready = function_body(app, r"static\s+void\s+on_enrollment_ready\s*\(")
    check(ready is not None and "start_attach_supervisor()" in ready and "apply_power_rail_sequencing()" in ready,
          "on_enrollment_ready() enables the rail and the normal supervisor")
    callers = [m.start() for m in re.finditer(r"\bstart_attach_supervisor\s*\(\s*\)\s*;", app)]
    check(len(callers) == 2, "start_attach_supervisor() has exactly two callers (modes 0/1 and on_enrollment_ready)")

    rt = strip_c_comments(read("power_base/main/enrollment_runtime.c"))
    cb = function_body(rt, r"static\s+void\s+joiner_callback\s*\(")
    check(cb is not None, "joiner_callback() found")
    check(cb is not None and not re.search(r"xSemaphoreTake|esp_openthread_lock_acquire|\bot[A-Z]\w*\s*\(", cb),
          "Joiner callback takes no lock and calls no OpenThread API")
    check(cb is not None and re.search(r"xQueueSend\s*\([^;]*,\s*0\s*\)", cb) is not None,
          "Joiner callback only posts with a zero timeout")

    sources = ["components/power_base_enrollment/power_base_enrollment.c", "power_base/main/enrollment_runtime.c",
               "power_base/main/enrollment_console.c", "components/thread_manager/thread_manager.c"]
    for rel in sources:
        code = strip_c_comments(read(rel))
        leaks = [s for s in code.split(";") if re.search(r"\b(ESP_LOG[EWIDV]|printf|fprintf|puts)\s*\(", s)
                 and re.search(r"\bpskd\b", s)]
        check(not leaks, f"{rel}: no log/print statement uses the pskd variable")

    console = strip_c_comments(read("power_base/main/enrollment_console.c"))
    prints = [s for s in console.split(";") if re.search(r"\bprintf\s*\(", s) and re.search(r"\bargv\b", s)]
    check(not prints, "pb_join never prints its arguments")
    check(re.search(r'strcmp\(argv\[1\],\s*"start"\)\s*==\s*0\s*&&\s*argc\s*==\s*2', console) is not None,
          "'pb_join start' takes no argument (the PSKd is prompted, never in the command line/history)")

    defaults = read("power_base/sdkconfig.defaults")
    for opt in ("CONFIG_POWER_BASE_BENCH_JOINER_CONSOLE=y", "CONFIG_OPENTHREAD_JOINER=y", "CONFIG_OPENTHREAD_CLI=y"):
        check(opt not in defaults, f"sdkconfig.defaults does not set {opt}")
    check("CONFIG_OPENTHREAD_CLI=n" in defaults, "sdkconfig.defaults keeps the OpenThread CLI off")
    bench = read("power_base/sdkconfig.bench_joiner")
    check("CONFIG_POWER_BASE_BENCH_JOINER_CONSOLE=y" in bench and "CONFIG_OPENTHREAD_JOINER=y" in bench,
          "bench overlay enables the Joiner and pb_join")
    check("OPENTHREAD_CLI" not in bench and "OPENTHREAD_CONSOLE" not in bench,
          "bench overlay never enables the OpenThread CLI/console")
    kconfig = read("power_base/main/Kconfig.projbuild")
    check(re.search(r"config POWER_BASE_BENCH_JOINER_CONSOLE.*?default n", kconfig, re.DOTALL) is not None,
          "POWER_BASE_BENCH_JOINER_CONSOLE defaults to n")

    print(f"\n{'PASS' if failures == 0 else 'FAIL'}: {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
