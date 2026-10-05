#!/usr/bin/env python3
"""
Structural checks for the BLE enrollment glue that only builds under ESP-IDF
(power_base/main/enrollment_ble.c, enrollment_runtime.c,
components/power_base_ble_enroll/pb_ble_service.c) and for the build
configuration. The RULES are tested on real code in
tools/test_power_base_ble_admission.c and, with real Security 2, in
tools/test_power_base_ble_sec2_interop.py; this file guards the wiring those
rules rely on, like tools/test_power_base_await_commissioning_paths.py does
for the Joiner glue:

  1. Code that runs in BLE callbacks (the Security 2 wrappers and the three
     endpoint handlers, and every BLE-side function of the admission module)
     calls no OpenThread API, no NVS, no enrollment controller, and never
     stops BLE or frees protocomm.
  2. BLE is started, polled and stopped only by the enrollment task; the
     poll runs outside the controller mutex and before the one service start.
  3. A board that resumed at boot never creates the enrollment task, so it
     never opens BLE; ready/services still fire once.
  4. Only protocomm Security 2 is ever registered; the BLE build refuses to
     compile with Security 0/1, the pb_join console, the OpenThread CLI or
     DEBUG logs.
  5. Default and bench configurations are untouched by the BLE overlay; the
     overlay contains no console/CLI.
  6. No log statement in the new code passes a PSKd, challenge, request id,
     salt, verifier or credential.
  7. The partition table and the completion marker constants are unchanged.
  8. (When a BLE build's sdkconfig is found) its effective values.
  9. The delivered bridge endpoint: the capability and message constants,
     that the BLE side only copies it (no NVS, no OpenThread), that it reaches
     the controller through the worker only, that app_main.c puts it into the
     exporter's config before the services start and nowhere else, and that
     no log statement is handed an address.
 10. The enrollment endpoint policy: read from cfg only on the mode 2 path,
     given to the controller before the boot rule and to the BLE admission
     rules before the transport starts, and checked in both before anything
     is queued, erased or started.

Usage:
    python3 tools/test_power_base_ble_enrollment_paths.py
    PB_BLE_SDKCONFIG=/path/to/ble/build/sdkconfig python3 tools/test_power_base_ble_enrollment_paths.py
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
    m = re.search(signature_regex + r"[^;{]*\{", code)
    if not m:
        return None
    depth, i = 1, m.end()
    while depth and i < len(code):
        depth += {"{": 1, "}": -1}.get(code[i], 0)
        i += 1
    return code[m.end():i - 1]


failures = 0


def check(cond, msg):
    global failures
    print(("ok:   " if cond else "FAIL: ") + msg)
    if not cond:
        failures += 1


# Anything a BLE callback must not do itself.
FORBIDDEN_IN_CALLBACKS = re.compile(
    r"\bot[A-Z]\w*\s*\(|esp_openthread_|thread_manager_|\bnvs_\w+\s*\(|pb_enrollment_\w+\s*\(|pb_enroll_ctl_\w+\s*\(|"
    r"pb_enroll_marker_\w+\s*\(|pb_enroll_record_\w+\s*\(|power_base_bridge_config_\w+\s*\(|protocomm_ble_stop|protocomm_delete|nimble_port_|ble_gap_|vTaskDelay|"
    r"request_join\s*\(|\.cancel\s*\(|->cancel\s*\(|->request_join\s*\(|pb_ble_service_stop|pb_ble_adm_worker_poll")

SECRET_IDENTIFIERS = re.compile(
    r"\b(pskd|challenge|ch|request_id|asked|rec_request_id|rec_tag|tag|salt|verifier|cred|s_cred|raw|record|code_str)\b")


def log_statements(code):
    """Argument text of every ESP_LOGx()/printf() statement, string literals removed."""
    out = []
    for stmt in code.split(";"):
        m = re.search(r"\b(ESP_LOG[EWIDV]|ESP_LOG_BUFFER_HEX\w*|printf|fprintf|puts)\s*\((.*)", stmt, flags=re.DOTALL)
        if m:
            out.append(re.sub(r'"(?:\\.|[^"\\])*"', '""', m.group(2)))
    return out


def main():
    svc = strip_c_comments(read("components/power_base_ble_enroll/pb_ble_service.c"))
    adm = strip_c_comments(read("components/power_base_ble_enroll/pb_ble_admission.c"))
    ble = strip_c_comments(read("power_base/main/enrollment_ble.c"))
    rt = strip_c_comments(read("power_base/main/enrollment_runtime.c"))

    print("\n-- 1. BLE callbacks do not call OpenThread, NVS, the controller, or stop BLE --")
    for name in ("sec_new_session", "sec_close_session", "sec_req_handler", "ep_status", "ep_start", "ep_cancel"):
        body = function_body(svc, r"static\s+esp_err_t\s+" + name + r"\s*\(")
        check(body is not None and not FORBIDDEN_IN_CALLBACKS.search(body),
              f"pb_ble_service.c {name}(): only the leaf lock and the admission/SDK call")
        check(body is not None and "portMAX_DELAY" not in body and "xQueue" not in body,
              f"pb_ble_service.c {name}(): no queue wait")
    for name in ("pb_ble_adm_session_changed", "pb_ble_adm_sec_request_begin", "pb_ble_adm_sec_request_end",
                 "pb_ble_adm_handle_status", "start_admit", "pb_ble_adm_handle_start", "cancel_admit",
                 "pb_ble_adm_handle_cancel"):
        body = function_body(adm, r"\b" + name + r"\s*\(")
        check(body is not None and not FORBIDDEN_IN_CALLBACKS.search(body) and "w->" not in body,
              f"pb_ble_admission.c {name}(): pure state, no controller call")
    check(not re.search(r'#include\s+"(nvs|esp_openthread|openthread/|protocomm|freertos/)', adm),
          "pb_ble_admission.c includes no NVS, OpenThread, protocomm or FreeRTOS header")
    poll = function_body(adm, r"pb_ble_close_reason_t\s+pb_ble_adm_worker_poll\s*\(")
    calls = [(m.start(), m.group(0)) for m in
             re.finditer(r"w_lock\(w\)|w_unlock\(w\)|w->request_join\(|w->cancel\(|w->get_status\(", poll or "")]
    depth, bad = 0, 0
    for _, text in calls:
        if text.startswith("w_lock"):
            depth += 1
        elif text.startswith("w_unlock"):
            depth -= 1
        elif depth != 0:
            bad += 1
    check(poll is not None and calls and bad == 0 and depth == 0,
          "pb_ble_adm_worker_poll(): every controller call is made with the leaf lock released")

    print("\n-- 2. BLE is started, polled and stopped only by the enrollment task --")
    sources = {"enrollment_runtime.c": rt, "enrollment_ble.c": ble, "pb_ble_service.c": svc,
               "app_main.c": strip_c_comments(read("power_base/main/app_main.c")),
               "enrollment_console.c": strip_c_comments(read("power_base/main/enrollment_console.c"))}
    task = function_body(rt, r"static\s+void\s+enrollment_task\s*\(")
    check(task is not None, "enrollment_task() found")
    for fn in ("pb_enrollment_ble_start", "pb_enrollment_ble_poll"):
        users = [n for n, c in sources.items() if re.search(r"\b" + fn + r"\s*\(\s*\)\s*;", c)
                 or re.search(r"=\s*" + fn + r"\s*\(\s*\)", c)]
        check(users == ["enrollment_runtime.c"] and task is not None and fn in task and
              len(re.findall(r"\b" + fn + r"\s*\(", rt)) == 1,
              f"{fn}() is called from exactly one place: enrollment_task()")
    stop_callers = [n for n, c in sources.items() if "protocomm_ble_stop" in c]
    ble_poll = function_body(ble, r"void\s+pb_enrollment_ble_poll\s*\(")
    ble_start = function_body(ble, r"esp_err_t\s+pb_enrollment_ble_start\s*\(")
    check(stop_callers == ["enrollment_ble.c"] and ble_poll is not None and "protocomm_ble_stop(" in ble_poll and
          len(re.findall(r"protocomm_ble_stop\s*\(", ble)) == 1,
          "protocomm_ble_stop() is called only from pb_enrollment_ble_poll()")
    check(ble_poll is not None and ble_poll.find("protocomm_ble_stop(") < ble_poll.find("pb_ble_service_stop("),
          "the protocomm instance is freed only after the transport has stopped")
    check(ble_poll is not None and re.search(r"if\s*\(\s*err\s*!=\s*ESP_OK\s*\)\s*\{[^}]*return;", ble_poll) and
          "BLE_STOP_FAILED" in ble_poll,
          "a failed BLE stop frees nothing (objects stay alive for late callbacks)")
    check(ble_start is not None and "pb_ble_auth_load(" in ble_start and "joiner_eui64_matches" in ble_start and
          ble_start.find("pb_ble_auth_load(") < ble_start.find("protocomm_ble_start("),
          "BLE opens only after the credential loaded for this EUI-64 and the Joiner EUI-64 matched")
    give = task.find("xSemaphoreGive(s_lock)") if task else -1
    poll_at = task.find("pb_enrollment_ble_poll()") if task else -1
    ready_at = task.find("s_ready()") if task else -1
    check(-1 < give < poll_at < ready_at,
          "in the task: controller mutex released, then the BLE poll (which stops BLE once enrolled), then ready")
    check(task is not None and "xSemaphoreTake(s_lock" in task and
          task[task.find("xSemaphoreTake(s_lock"):give].count("pb_enrollment_ble") == 0,
          "no BLE call while the controller mutex is held")

    print("\n-- 3. resume path and the single service start --")
    start = function_body(rt, r"esp_err_t\s+pb_enrollment_runtime_start\s*\(")
    m = re.search(r"if\s*\(\s*resumed\s*\)\s*\{\s*s_ready\(\);\s*\}\s*else\s*\{", start or "")
    check(m is not None and "xTaskCreate(enrollment_task" in (start or "")[m.end():] and
          "xTaskCreate" not in (start or "")[:m.start()],
          "a board that resumed at boot never creates the enrollment task (so BLE is never opened)")
    check(len(re.findall(r"\bs_ready\(\)\s*;", rt)) == 2 and len(re.findall(r"s_ready_fired\s*=\s*true", rt)) == 2 and
          "!s_ready_fired" in (task or ""),
          "ready is fired from two places only, each guarded by s_ready_fired")
    check("PB_ENROLLMENT_HAS_TRANSPORT" in rt and
          re.search(r"#if\s+PB_ENROLLMENT_HAS_TRANSPORT\s+static\s+bool\s+role_is_attached", rt) is not None,
          "the enrollment task is built for either transport, not only for the bench console")
    check("enrollment_ble" not in sources["app_main.c"] and "protocomm" not in sources["app_main.c"],
          "app_main.c knows nothing about BLE (modes 0/1 untouched)")

    print("\n-- 4. Security 2 only --")
    repo_sources = []
    for base in ("power_base/main", "components"):
        for dirpath, _, files in os.walk(os.path.join(ROOT, base)):
            for fn in files:
                if fn.endswith((".c", ".h")):
                    repo_sources.append(os.path.join(dirpath, fn))
    weak = [p for p in repo_sources
            if re.search(r"protocomm_security[01]\b", strip_c_comments(open(p, encoding="utf-8").read()))]
    check(not weak, "no source references protocomm_security0 or protocomm_security1")
    check("s_sec = protocomm_security2" in svc and
          re.search(r"protocomm_set_security\(\s*pc\s*,\s*PB_BLE_EP_SESSION\s*,\s*&s_sec\s*,\s*&s_sec_params\s*\)", svc),
          "the registered security object is ESP-IDF's protocomm_security2")
    for fn in ("encrypt", "decrypt", "init", "cleanup"):
        check(not re.search(r"s_sec\." + fn + r"\s*=", svc), f"its {fn}() is ESP-IDF's own (not replaced)")
    check(len(re.findall(r"protocomm_security2\.(new_transport_session|close_transport_session|"
                         r"security_req_handler)\(", svc)) == 3,
          "the three wrapped entries each delegate to ESP-IDF's implementation")
    raw_ble = read("power_base/main/enrollment_ble.c")
    check(re.search(r"#if CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_0 \|\| "
                    r"CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1\s*\n#error", raw_ble) is not None,
          "enrollment_ble.c refuses to compile with Security 0/1")
    check(re.search(r"#if CONFIG_POWER_BASE_BENCH_JOINER_CONSOLE \|\| CONFIG_OPENTHREAD_CLI\s*\n#error", raw_ble)
          is not None, "enrollment_ble.c refuses to compile with the pb_join console or the OpenThread CLI")
    check(re.search(r"#if CONFIG_LOG_MAXIMUM_LEVEL > 3\s*\n#error", raw_ble) is not None,
          "enrollment_ble.c refuses to compile with DEBUG logging (ESP-IDF would dump session secrets)")
    for field in ("ble_bonding", "ble_sm_sc", "ble_link_encryption", "keep_ble_on"):
        check(re.search(r"ble\." + field + r"\s*=\s*0\s*;", ble) is not None, f"protocomm BLE config: {field} = 0")
    check("factory" not in ble and "erase" not in ble.lower() and "dataset" not in ble.lower() and
          "esp_console" not in ble and "pb_enrollment_forget" not in ble,
          "enrollment_ble.c offers no factory reset, Dataset access or console")

    print("\n-- 5. build configurations --")
    defaults = read("power_base/sdkconfig.defaults")
    bench = read("power_base/sdkconfig.bench_joiner")
    overlay = read("power_base/sdkconfig.ble_enrollment")
    module_defaults = read("module/sdkconfig.defaults")
    for name, text in (("power_base/sdkconfig.defaults", defaults), ("power_base/sdkconfig.bench_joiner", bench),
                       ("module/sdkconfig.defaults", module_defaults)):
        check(not re.search(r"^CONFIG_(BT_|POWER_BASE_BLE|ESP_PROTOCOMM|ESP_COEX)", text, re.MULTILINE),
              f"{name} sets nothing about BT, protocomm, coexistence or BLE enrollment")
    def on(opt):
        return re.search(r"^" + opt + r"=y$", overlay, re.MULTILINE) is not None

    def off(opt):
        return re.search(r"^" + opt + r"=n$", overlay, re.MULTILINE) is not None

    check(all(on(o) for o in ("CONFIG_OPENTHREAD_JOINER", "CONFIG_BT_ENABLED", "CONFIG_BT_NIMBLE_ENABLED",
                              "CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_2",
                              "CONFIG_ESP_COEX_SW_COEXIST_ENABLE", "CONFIG_POWER_BASE_BLE_ENROLLMENT")),
          "BLE overlay: Joiner, NimBLE, Security 2, software coexistence, BLE enrollment")
    check(off("CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_0") and off("CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1"),
          "BLE overlay: Security 0 and 1 explicitly off")
    check(off("CONFIG_BT_NIMBLE_ROLE_CENTRAL") and off("CONFIG_BT_NIMBLE_ROLE_OBSERVER") and
          re.search(r"^CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1$", overlay, re.MULTILINE) is not None,
          "BLE overlay: peripheral only, one connection")
    check(not re.search(r"^CONFIG_(POWER_BASE_BENCH_JOINER_CONSOLE|OPENTHREAD_CLI|OPENTHREAD_CONSOLE\w*|LOG_\w+)=",
                        overlay, re.MULTILINE),
          "BLE overlay: no pb_join console, no OpenThread CLI/console, no log level change")
    kconfig = read("power_base/main/Kconfig.projbuild")
    opt = re.search(r"config POWER_BASE_BLE_ENROLLMENT(.*?)(?=\n    config |\nendmenu)", kconfig, re.DOTALL)
    body = opt.group(1) if opt else ""
    check("default n" in body and "!POWER_BASE_BENCH_JOINER_CONSOLE" in body and "!OPENTHREAD_CLI" in body and
          "!ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_0" in body and "!ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1" in body and
          "ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_2" in body and "OPENTHREAD_JOINER" in body,
          "POWER_BASE_BLE_ENROLLMENT: default n; needs Joiner + Security 2; excludes Security 0/1, console, CLI")

    print("\n-- 6. no secret reaches a log statement --")
    for rel in ("components/power_base_ble_enroll/pb_ble_admission.c",
                "components/power_base_ble_enroll/pb_ble_service.c",
                "components/power_base_ble_auth/power_base_ble_auth.c", "power_base/main/enrollment_ble.c"):
        code = strip_c_comments(read(rel))
        stmts = log_statements(code)
        leaks = [s for s in stmts if SECRET_IDENTIFIERS.search(s)]
        check(not leaks, f"{rel}: {len(stmts)} log statement(s), none passes a PSKd/challenge/request id/credential")
        check("ESP_LOG_BUFFER" not in code and "esp_log_buffer" not in code, f"{rel}: no hex dump")

    print("\n-- 7. partition table and completion marker unchanged --")
    part = [re.sub(r"\s+", "", line) for line in read("power_base/partitions.csv").splitlines()
            if line.strip() and not line.startswith("#")]
    check(part == ["nvs,data,nvs,0x9000,0x6000,", "phy_init,data,phy,0xf000,0x1000,",
                   "factory,app,factory,0x10000,1500K,", "cfg,data,nvs,0x187000,0x3000,"],
          "power_base/partitions.csv: nvs, phy_init, factory (1500K), cfg at their existing offsets and sizes")
    enroll_h = read("components/power_base_enrollment/include/power_base_enrollment/power_base_enrollment.h")
    check('#define PB_ENROLL_NVS_NAMESPACE "pb_enroll"' in enroll_h and '#define PB_ENROLL_NVS_KEY "done"' in enroll_h and
          "#define PB_ENROLL_MARKER_LEN 36" in enroll_h, "completion marker: namespace, key and size unchanged")
    check("pb_enroll_marker" not in ble and "pb_enroll_marker" not in svc and "pb_enroll_marker" not in adm,
          "the BLE code never reads, writes or erases the marker itself")

    print("\n-- 8. effective configuration of a BLE build --")
    sdk = os.environ.get("PB_BLE_SDKCONFIG",
                         os.path.expanduser("~/esp-builds/pogo_power_base_ble_enrollment/sdkconfig"))
    if not os.path.isfile(sdk):
        print(f"      (not checked: no BLE build sdkconfig at {sdk})")
    else:
        with open(sdk, "r", encoding="utf-8") as f:
            cfg = f.read()

        def is_y(o):
            return re.search(r"^" + o + r"=y$", cfg, re.MULTILINE) is not None

        def unset(o):
            return re.search(r"^" + o + r"=", cfg, re.MULTILINE) is None

        check(all(is_y(o) for o in ("CONFIG_POWER_BASE_BLE_ENROLLMENT", "CONFIG_OPENTHREAD_JOINER",
                                    "CONFIG_BT_NIMBLE_ENABLED", "CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_2",
                                    "CONFIG_ESP_COEX_SW_COEXIST_ENABLE", "CONFIG_OPENTHREAD_FTD")),
              "BLE build: enrollment transport, Joiner, NimBLE, Security 2, SW coexistence, FTD are on")
        check(all(unset(o) for o in ("CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_0",
                                     "CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1",
                                     "CONFIG_POWER_BASE_BENCH_JOINER_CONSOLE", "CONFIG_OPENTHREAD_CLI",
                                     "CONFIG_BT_NIMBLE_ROLE_CENTRAL", "CONFIG_BT_NIMBLE_ROLE_OBSERVER")),
              "BLE build: Security 0/1, pb_join console, OpenThread CLI, central/observer roles are off")
        check(re.search(r"^CONFIG_LOG_MAXIMUM_LEVEL=3$", cfg, re.MULTILINE) is not None and
              re.search(r"^CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1$", cfg, re.MULTILINE) is not None,
              "BLE build: log level capped at INFO, one BLE connection")

    print("\n-- 9. the delivered bridge endpoint --")
    proto_h = read("components/power_base_ble_enroll/include/power_base_ble_enroll/pb_ble_proto.h")
    svc_h = read("components/power_base_ble_enroll/include/power_base_ble_enroll/pb_ble_service.h")

    def define(text, name):
        m = re.search(r"^#define\s+" + name + r"\s+(\S+)", text, re.MULTILINE)
        return m.group(1) if m else None

    check(define(proto_h, "PB_BLE_PROTO_VERSION") == "1u" and define(proto_h, "PB_BLE_MSG_START_REQ") == "0x02u" and
          define(proto_h, "PB_BLE_START_REQ_FIXED_LEN") == "44" and define(proto_h, "PB_BLE_START_REQ_MAX_LEN") == "76" and
          define(proto_h, "PB_BLE_STATUS_REQ_LEN") == "20" and define(proto_h, "PB_BLE_STATUS_RESP_LEN") == "40" and
          define(proto_h, "PB_BLE_CANCEL_REQ_LEN") == "28" and define(proto_h, "PB_BLE_SHORT_RESP_LEN") == "4",
          "the version 1 messages keep their type values and sizes")
    check(define(proto_h, "PB_BLE_MSG_START_EP_REQ") == "0x04u" and define(proto_h, "PB_BLE_MSG_START_EP_RESP") == "0x84u" and
          define(proto_h, "PB_BLE_START_EP_REQ_FIXED_LEN") == "64" and
          define(proto_h, "PB_BLE_START_EP_REQ_MAX_LEN") == "96" and
          define(proto_h, "PB_BLE_BRIDGE_ENDPOINT_VERSION") == "1u" and
          define(proto_h, "PB_BLE_CAP_BRIDGE_ENDPOINT_V1") == '"bridge_endpoint_v1"' and
          re.search(r"PB_BLE_CODE_INVALID_ENDPOINT\s*=\s*0x1d\b", proto_h) is not None,
          "the endpoint start: type 0x04 / 0x84, 64 + PSKd bytes, endpoint format 1, code 0x1d, capability name")
    version_json = re.search(r"#define PB_BLE_VERSION_JSON(.*?)\n\n", svc_h, re.DOTALL)
    version_plain = version_json.group(1).replace("\\", "") if version_json else ""
    check(re.search(r'"pb_enroll":\{"proto":1,"\s*"?"cap":\["" PB_BLE_CAP_BRIDGE_ENDPOINT_V1 ""\]\}\}', version_plain)
          is not None,
          "proto-ver: pb_enroll.proto stays 1 and pb_enroll.cap lists the capability")
    check('sec_ver":2' in version_json.group(1).replace("\\", "") and
          'sec_patch_ver":1' in version_json.group(1).replace("\\", ""),
          "proto-ver still announces Security 2, patch 1")

    start_admit = function_body(adm, r"static\s+uint8_t\s+start_admit\s*\(")
    check(start_admit is not None and "pb_enroll_bridge_endpoint_check(" in start_admit and
          not re.search(r"\bnvs_\w+\s*\(|pb_enroll_record_\w+\s*\(|pb_enroll_marker_\w+\s*\(|\bot[A-Z]\w*\s*\(|"
                        r"request_join", start_admit),
          "admission (BLE callback): checks the endpoint and copies it; no NVS, no OpenThread, no controller call")
    tag_fn = function_body(adm, r"static\s+bool\s+request_tag\s*\(")
    check(tag_fn is not None and re.search(r"buf\[n\+\+\]\s*=\s*req\[1\]\s*;", tag_fn) is not None and
          re.search(r"memcpy\(&buf\[n\],\s*&req\[START_EP_VERSION_OFFSET\],\s*START_EP_BLOCK_LEN\)", tag_fn) is not None,
          "the request digest covers the message type and the whole endpoint block (format, port, address)")
    worker = function_body(adm, r"pb_ble_close_reason_t\s+pb_ble_adm_worker_poll\s*\(")
    check(worker is not None and re.search(r"w->request_join\(\s*pskd\s*,\s*have_endpoint\s*\?\s*&endpoint\s*:\s*NULL", worker)
          is not None and len(re.findall(r"request_join\s*\(", adm)) == 1,
          "the endpoint reaches the controller in one place only: the worker pass")
    ble_join = function_body(ble, r"static\s+esp_err_t\s+ctl_request_join\s*\(")
    check(ble_join is not None and "pb_enrollment_request_join_with_endpoint(pskd, endpoint)" in ble_join and
          "pb_enrollment_request_join(pskd)" in ble_join,
          "enrollment_ble.c: a start with an endpoint uses the endpoint request, one without uses the old one")
    check("nvs_" not in ble and "power_base_bridge_config" not in ble and "linux_bridge_exporter" not in ble,
          "enrollment_ble.c stores nothing and does not touch the exporter")
    console = strip_c_comments(read("power_base/main/enrollment_console.c"))
    check("with_endpoint" not in console and "pb_enrollment_request_join(pskd)" in console,
          "the bench console path is unchanged: it never delivers an endpoint")

    app = sources["app_main.c"]
    ready = function_body(app, r"static\s+void\s+on_enrollment_ready\s*\(")
    apply_at = ready.find("power_base_bridge_config_apply_delivered(") if ready else -1
    supervisor_at = ready.find("start_attach_supervisor()") if ready else -1
    check(ready is not None and "pb_enrollment_get_status(" in ready and "has_bridge_endpoint" in ready and
          -1 < apply_at < supervisor_at,
          "app_main.c: on_enrollment_ready() applies the delivered endpoint BEFORE it starts the attach supervisor")
    check(len(re.findall(r"power_base_bridge_config_apply_delivered\s*\(", app)) == 1,
          "the delivered endpoint is applied in that one place only")
    services = function_body(app, r"static\s+void\s+start_services_once\s*\(")
    check(services is not None and "linux_bridge_exporter_init(s_own_uuid, &s_bridge_cfg)" in services and
          len(re.findall(r"linux_bridge_exporter_init\s*\(", app)) == 1 and
          len(re.findall(r"\bstart_services_once\s*\(", app)) == 2,
          "the exporter is started once, from start_services_once(), with s_bridge_cfg")
    check(len(re.findall(r"\bs_bridge_cfg\b", app)) ==
          len(re.findall(r"power_base_bridge_config_load\(&s_bridge_cfg\)|s_bridge_cfg\.enabled|"
                         r"power_base_bridge_config_apply_delivered\(&s_bridge_cfg|"
                         r"linux_bridge_exporter_init\(s_own_uuid, &s_bridge_cfg\)|"
                         r"static\s+power_base_bridge_config_t\s+s_bridge_cfg", app)),
          "s_bridge_cfg is only loaded, replaced by the delivered endpoint, read by the exporter start, or disabled")
    rt_status = function_body(rt, r"void\s+pb_enrollment_get_status\s*\(")
    check(rt_status is not None and "PB_ENROLL_STATE_ENROLLED" in rt_status and "has_bridge_endpoint" in rt_status,
          "enrollment_runtime.c reports the endpoint only for an enrolled board")
    bridge_c = strip_c_comments(read("components/power_base_bridge_config/power_base_bridge_config.c"))
    check("NVS_READWRITE" not in bridge_c and "nvs_set_" not in bridge_c and "nvs_commit" not in bridge_c and
          "nvs_erase" not in bridge_c,
          "the cfg partition stays read-only: power_base_bridge_config.c opens it read-only and never writes")

    address_identifiers = re.compile(
        r"\b(endpoint|pending_ep|bridge_endpoint|host_addr|addr|s_host_addr|delivered|ep|raw|dest_addr|mleid)\b")
    for rel in ("components/power_base_enrollment/power_base_enrollment.c",
                "components/power_base_bridge_config/power_base_bridge_config.c",
                "components/power_base_ble_enroll/pb_ble_admission.c",
                "components/power_base_ble_enroll/pb_ble_service.c", "power_base/main/enrollment_ble.c",
                "power_base/main/enrollment_runtime.c", "power_base/main/app_main.c"):
        stmts = log_statements(strip_c_comments(read(rel)))
        leaks = [st for st in stmts if address_identifiers.search(re.sub(r"\bendpoint\s*!=\s*NULL\b", "", st))]
        check(not leaks, f"{rel}: {len(stmts)} log statement(s), none is handed a bridge address")

    print("\n-- 10. the enrollment endpoint policy --")
    app_main_fn = function_body(app, r"void\s+app_main\s*\(")
    await_at = app_main_fn.find("if (s_thread_cfg.mode == POWER_BASE_THREAD_MODE_AWAIT_COMMISSIONING) {\n        /* Unregistered")
    if await_at < 0:
        await_at = app_main_fn.rfind("POWER_BASE_THREAD_MODE_AWAIT_COMMISSIONING", 0,
                                     app_main_fn.find("power_base_bridge_config_load_enroll_policy("))
    policy_at = app_main_fn.find("power_base_bridge_config_load_enroll_policy(")
    start_at = app_main_fn.find("pb_enrollment_runtime_start(s_own_uuid, endpoint_policy, on_enrollment_ready)")
    return_at = app_main_fn.find("return;", start_at)
    other_modes_at = app_main_fn.find("thread_manager_start_as_always_on_ftd")
    check(len(re.findall(r"power_base_bridge_config_load_enroll_policy\s*\(", app)) == 1 and
          -1 < await_at < policy_at < start_at < return_at < other_modes_at,
          "app_main.c reads the policy once, inside the mode 2 branch (which returns before the mode 0/1 starts), and "
          "hands it to the enrollment runtime")
    check("POWER_BASE_BRIDGE_ENROLL_POLICY_ENDPOINT_REQUIRED" in app_main_fn and
          re.search(r"case POWER_BASE_BRIDGE_ENROLL_POLICY_INVALID:\s*default:\s*endpoint_policy = "
                    r"PB_ENROLL_ENDPOINT_POLICY_INVALID", app_main_fn) is not None,
          "anything that is not 'none' or 'endpoint required' becomes INVALID (no new enrollment), never OPTIONAL")
    loader = function_body(bridge_c, r"esp_err_t\s+power_base_bridge_config_load\s*\(")
    unpack = function_body(bridge_c, r"static\s+void\s+unpack_blob\s*\(")
    check(loader is not None and unpack is not None and "ENROLL_POLICY" not in loader and "ENROLL_POLICY" not in unpack and
          "blob[5]" not in unpack and "blob + 5" not in unpack,
          "power_base_bridge_config_load() (used by modes 0/1 too) does not look at the policy byte")
    rt_start = function_body(rt, r"esp_err_t\s+pb_enrollment_runtime_start\s*\(")
    init_at = rt_start.find("pb_enroll_ctl_init(&s_ctl")
    set_at = rt_start.find("pb_enroll_ctl_set_endpoint_policy(&s_ctl, endpoint_policy)")
    boot_at = rt_start.find("pb_enroll_ctl_boot(&s_ctl)")
    check(-1 < init_at < set_at < boot_at, "enrollment_runtime.c gives the policy to the controller before the boot rule")
    ctl_c = strip_c_comments(read("components/power_base_enrollment/power_base_enrollment.c"))
    req = function_body(ctl_c, r"static\s+esp_err_t\s+request_join\s*\(")
    pol_at = req.find("PB_ENROLL_ENDPOINT_POLICY_REQUIRED && endpoint == NULL")
    check(req is not None and -1 < pol_at < req.find("pb_enroll_marker_clear()") < req.find("c->ops.joiner_start(") and
          pol_at < req.find("c->attempt++") and "ESP_ERR_NOT_ALLOWED" in req,
          "the controller refuses before it erases the old record, counts an attempt or starts the Joiner")
    check(len(re.findall(r"\brequest_join\(c, pskd, (NULL|endpoint), now_ms\)", ctl_c)) == 2,
          "both public request functions go through that one check")
    boot_fn = function_body(ctl_c, r"pb_enroll_state_t\s+pb_enroll_ctl_boot\s*\(")
    guard_at = boot_fn.find("c->endpoint_policy == PB_ENROLL_ENDPOINT_POLICY_REQUIRED")
    check(-1 < guard_at < boot_fn.find("c->ops.thread_start_stored(") and
          "pb_enroll_marker_clear" not in boot_fn and "erase" not in boot_fn,
          "the boot rule decides on the policy before Thread could be enabled, and erases nothing")
    adm_start = function_body(adm, r"static\s+uint8_t\s+start_admit\s*\(")
    adm_pol = adm_start.find("PB_ENROLL_ENDPOINT_POLICY_REQUIRED && !with_endpoint")
    check(-1 < adm_start.find("a->closed != PB_BLE_OPEN") < adm_pol < adm_start.find("request_tag(") and
          adm_pol < adm_start.find("drop_challenge(a)") and adm_pol < adm_start.find("q_push(") and
          "PB_BLE_CODE_UNAVAILABLE" in adm_start[adm_pol:adm_pol + 200],
          "admission answers UNAVAILABLE before the digest, the challenge and the queue are touched")
    svc_start = function_body(svc, r"esp_err_t\s+pb_ble_service_start\s*\(")
    check(-1 < svc_start.find("pb_ble_adm_init(&s_adm") < svc_start.find("pb_ble_adm_set_endpoint_policy(&s_adm, cfg->endpoint_policy)") <
          svc_start.find("protocomm_new()"),
          "pb_ble_service.c sets the policy right after the admission state is initialised, before protocomm exists")
    ble_start = function_body(ble, r"esp_err_t\s+pb_enrollment_ble_start\s*\(")
    check(".endpoint_policy = st.endpoint_policy" in ble_start and
          -1 < ble_start.find("PB_ENROLL_ENDPOINT_POLICY_INVALID") < ble_start.find("pb_ble_auth_load(") <
          ble_start.find("protocomm_ble_start("),
          "enrollment_ble.c passes the runtime's policy on, and does not open BLE at all when it is unusable")
    check("ESP_ERR_NOT_ALLOWED" in console and len(re.findall(r"pb_enrollment_request_join\w*\(", console)) == 1,
          "the bench console has one way to start a Joiner -- the controller's -- and reports the policy refusal")
    check(define(proto_h, "PB_BLE_MSG_START_REQ") == "0x02u" and re.search(r"PB_BLE_CODE_UNAVAILABLE\s*=\s*0x1b\b", proto_h),
          "UNAVAILABLE is still 0x1b: no response code was added for the refusal")

    print(f"\n{'PASS' if failures == 0 else 'FAIL'}: {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
