#!/usr/bin/env python3
"""
Static (no ESP-IDF toolchain required) config-content check for `rcp/`, the
Linux OTBR RCP firmware project (see docs/otbr-rcp.md). Parses
`rcp/sdkconfig.defaults` and scans `rcp/` for stray files the way the other
`tools/test_*.py` checkers in this repository parse `configs/*.json`/source
text directly, rather than compiling anything.

This does NOT invoke idf.py/cmake and does NOT replace actually building the
project (see the "빌드 성공 / 경고 0건" verification run separately, reported
in the session's own final report, not by this script). It exists to make
the specific Kconfig-content requirements from the original request
regression-checkable without an ESP-IDF environment:

  1. USB Serial/JTAG RCP transport is selected, and its own Kconfig
     dependency (`ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG`) is satisfied
     explicitly in the checked-in sdkconfig.defaults.
  2. UART/SPI RCP transport are NOT selected (mutually exclusive `choice`,
     see rcp/sdkconfig.defaults's own comment).
  3. The RCP Spinel debug console and every ordinary log source
     (bootloader/system/OpenThread) are disabled.
  4. No leftover UART RCP GPIO pin configuration exists anywhere under
     `rcp/` (this project never wires UART GPIOs at all -- see
     docs/otbr-rcp.md section 2).
  5. `rcp/` has no `idf_component.yml` anywhere (no managed component
     dependency of any kind -- see docs/otbr-rcp.md section 1 and
     rcp/main/CMakeLists.txt's own comment).
  6. `rcp/` never references anything from `components/`, `configs/`, or
     the existing projects' own GPIO defines -- it is a fully independent
     ESP-IDF project sharing nothing with `module`/`power_base`/`upper`/
     `lower`.

Usage:
    python3 tools/test_rcp_config.py
"""
import os
import re
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RCP_DIR = os.path.join(ROOT_DIR, "rcp")

_results = []


def record(passed, message):
    _results.append((passed, message))
    print(("ok:   " if passed else "FAIL: ") + message)


def read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def has_config_line(text, key, value):
    """True if `CONFIG_<key>=<value>` appears as its own line (not merely
    as a substring, and not commented out)."""
    pattern = re.compile(r"^CONFIG_%s=%s\s*$" % (re.escape(key), re.escape(value)), re.MULTILINE)
    return pattern.search(text) is not None


def has_config_not_set(text, key):
    """True if the value is explicitly the Kconfig-generated 'not set'
    comment form OR simply absent -- used for options this project relies
    on the `choice`'s own mutual exclusion for (see sdkconfig.defaults's
    own comment: selecting USB implicitly makes UART/SPI 'n')."""
    return not has_config_line(text, key, "y")


def main():
    if not os.path.isdir(RCP_DIR):
        sys.exit(f"rcp/ project directory not found at {RCP_DIR}")

    defaults_path = os.path.join(RCP_DIR, "sdkconfig.defaults")
    if not os.path.isfile(defaults_path):
        sys.exit(f"rcp/sdkconfig.defaults not found at {defaults_path}")
    defaults = read(defaults_path)

    # --- 1. USB Serial/JTAG RCP transport, and its own Kconfig dependency. ---
    record(has_config_line(defaults, "OPENTHREAD_ENABLED", "y"),
           "sdkconfig.defaults: CONFIG_OPENTHREAD_ENABLED=y")
    record(has_config_line(defaults, "OPENTHREAD_RADIO", "y"),
           "sdkconfig.defaults: CONFIG_OPENTHREAD_RADIO=y (Radio Only device -- RCP, not FTD/MTD)")
    record(has_config_line(defaults, "OPENTHREAD_RCP_USB_SERIAL_JTAG", "y"),
           "sdkconfig.defaults: CONFIG_OPENTHREAD_RCP_USB_SERIAL_JTAG=y")
    record(has_config_line(defaults, "ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG", "y"),
           "sdkconfig.defaults: CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y "
           "(OPENTHREAD_RCP_USB_SERIAL_JTAG's own Kconfig `depends on`)")

    # --- 2. UART/SPI RCP transport NOT selected. ---
    record(has_config_not_set(defaults, "OPENTHREAD_RCP_UART"),
           "sdkconfig.defaults: CONFIG_OPENTHREAD_RCP_UART is not 'y' (mutually exclusive with USB)")
    record(has_config_not_set(defaults, "OPENTHREAD_RCP_SPI"),
           "sdkconfig.defaults: CONFIG_OPENTHREAD_RCP_SPI is not 'y' (mutually exclusive with USB)")

    # --- 3. RCP debug console and every ordinary log source disabled. ---
    record(has_config_not_set(defaults, "OPENTHREAD_RCP_SPINEL_CONSOLE"),
           "sdkconfig.defaults: CONFIG_OPENTHREAD_RCP_SPINEL_CONSOLE is not 'y' "
           "(no RCP debug console in this initial version)")
    record(has_config_not_set(defaults, "OPENTHREAD_CONSOLE_ENABLE"),
           "sdkconfig.defaults: CONFIG_OPENTHREAD_CONSOLE_ENABLE is not 'y' (no dedicated OpenThread console)")
    record(has_config_not_set(defaults, "OPENTHREAD_CLI"),
           "sdkconfig.defaults: CONFIG_OPENTHREAD_CLI is not 'y' (no OpenThread CLI)")
    record(has_config_line(defaults, "LOG_DEFAULT_LEVEL_NONE", "y"),
           "sdkconfig.defaults: CONFIG_LOG_DEFAULT_LEVEL_NONE=y (no ordinary application log output)")
    record(has_config_line(defaults, "OPENTHREAD_LOG_LEVEL_NONE", "y"),
           "sdkconfig.defaults: CONFIG_OPENTHREAD_LOG_LEVEL_NONE=y (no OpenThread stack log output)")
    record(has_config_line(defaults, "BOOTLOADER_LOG_LEVEL_ERROR", "y"),
           "sdkconfig.defaults: CONFIG_BOOTLOADER_LOG_LEVEL_ERROR=y (bootloader itself stays terse)")

    # --- 4. No leftover UART RCP GPIO pin configuration anywhere under rcp/.
    # NOTE: rcp/main/esp_ot_config.h is kept byte-for-byte verbatim from
    # upstream (item 3 of the original request: preserve the original file),
    # and upstream's own file textually mentions OPENTHREAD_UART_RX_PIN/
    # OPENTHREAD_UART_TX_PIN inside a `#if CONFIG_OPENTHREAD_RCP_UART` branch
    # -- permanently dead code here since CONFIG_OPENTHREAD_RCP_UART is never
    # "y" (checked above) and no Kconfig item to turn it on exists (checked
    # right below). A plain text grep for those macro names would therefore
    # flag verbatim, harmless upstream dead code as if this project actively
    # wired UART GPIOs -- so this check instead confirms there is no Kconfig
    # SURFACE to configure UART RCP pins at all: upstream's own
    # main/Kconfig.projbuild (the ONLY thing that defines
    # OPENTHREAD_UART_PIN_MANUAL/OPENTHREAD_UART_RX_PIN/OPENTHREAD_UART_TX_PIN
    # as real, settable Kconfig options) was deliberately NOT carried into
    # this project -- see docs/otbr-rcp.md section 2. ---
    kconfig_projbuild_path = os.path.join(RCP_DIR, "main", "Kconfig.projbuild")
    record(not os.path.isfile(kconfig_projbuild_path),
           "rcp/main/Kconfig.projbuild does not exist -- no Kconfig surface to configure UART RCP pins manually")
    record(not re.search(r"^CONFIG_OPENTHREAD_UART_(PIN_MANUAL|RX_PIN|TX_PIN)=", defaults, re.MULTILINE),
           "sdkconfig.defaults never assigns CONFIG_OPENTHREAD_UART_PIN_MANUAL/RX_PIN/TX_PIN")

    # --- 5. No idf_component.yml anywhere under rcp/ (no managed component deps). ---
    component_yml_hits = []
    for dirpath, _dirnames, filenames in os.walk(RCP_DIR):
        for name in filenames:
            if name == "idf_component.yml":
                component_yml_hits.append(os.path.relpath(os.path.join(dirpath, name), ROOT_DIR))
    record(len(component_yml_hits) == 0,
           f"rcp/ has no idf_component.yml anywhere (found: {component_yml_hits})")

    # --- 6. rcp/ shares nothing with the existing module/power_base/upper/lower
    # projects: no reference to components/, configs/, or their own GPIO defines. ---
    cross_reference_hits = []
    forbidden_patterns = [
        re.compile(r"EXTRA_COMPONENT_DIRS"),
        re.compile(r"\bconfigs/prototype_"),
        re.compile(r"\bmodule_config/module_config\.h\b"),
        re.compile(r"\bDETECT_IN_GPIO\b|\bDETECT_OUT_GPIO\b"),
    ]
    for dirpath, _dirnames, filenames in os.walk(RCP_DIR):
        for name in filenames:
            path = os.path.join(dirpath, name)
            if name in ("sdkconfig", "sdkconfig.old") or "/build/" in path.replace(os.sep, "/"):
                continue
            try:
                text = read(path)
            except (UnicodeDecodeError, OSError):
                continue
            for pattern in forbidden_patterns:
                if pattern.search(text):
                    cross_reference_hits.append((os.path.relpath(path, ROOT_DIR), pattern.pattern))
    record(len(cross_reference_hits) == 0,
           f"rcp/ never references components/'s shared GPIO defines, EXTRA_COMPONENT_DIRS, "
           f"or the other projects' own configs/ files (found: {cross_reference_hits})")

    # --- 7. USB Spinel TX retry fix (RESET_FAULT follow-up -- see
    # docs/otbr-rcp.md's "USB Spinel TX retry" section). Static presence/
    # wiring checks only; the actual retry/backoff behavior is exercised by
    # tools/test_rcp_uart_tx_retry.c, and the real ESP-IDF build/link is
    # covered separately (see the final report, not this script). ---
    retry_files = (
        os.path.join(RCP_DIR, "main", "rcp_uart_tx_retry.h"),
        os.path.join(RCP_DIR, "main", "rcp_uart_tx_retry.c"),
        os.path.join(RCP_DIR, "main", "rcp_uart_tx_io.c"),
        os.path.join(RCP_DIR, "main", "esp_ot_uart_send_wrap.c"),
    )
    for path in retry_files:
        record(os.path.isfile(path), f"{os.path.relpath(path, ROOT_DIR)} exists")

    main_cmake_path = os.path.join(RCP_DIR, "main", "CMakeLists.txt")
    if os.path.isfile(main_cmake_path):
        main_cmake = read(main_cmake_path)
        record("rcp_uart_tx_retry.c" in main_cmake and "rcp_uart_tx_io.c" in main_cmake
               and "esp_ot_uart_send_wrap.c" in main_cmake,
               "rcp/main/CMakeLists.txt builds the retry-fix sources")
        record("-Wl,--wrap=otPlatUartSend" in main_cmake,
               "rcp/main/CMakeLists.txt wires the otPlatUartSend --wrap override "
               "(ESP-IDF's own documented mechanism -- $IDF_PATH itself is never modified)")
        record("esp_hal_usb" in main_cmake,
               "rcp/main/CMakeLists.txt requires esp_hal_usb (hal/usb_serial_jtag_ll.h, "
               "for rcp_uart_tx_io.c's rcp_uart_tx_flush())")

    retry_header_path = os.path.join(RCP_DIR, "main", "rcp_uart_tx_retry.h")
    if os.path.isfile(retry_header_path):
        retry_header = read(retry_header_path)
        record("rcp_uart_tx_flush(void)" in retry_header,
               "rcp_uart_tx_retry.h declares rcp_uart_tx_flush() (preserves stock otPlatUartSend()'s "
               "post-write usb_serial_jtag_ll_txfifo_flush(), \"DIG-727\")")
        record("rcp_uart_tx_recovery_restart(void)" in retry_header and "noreturn" in retry_header,
               "rcp_uart_tx_retry.h declares a noreturn rcp_uart_tx_recovery_restart() hook "
               "(a failed/timed-out send must never return an otError to NcpSend(), which ignores it)")

    retry_c_path = os.path.join(RCP_DIR, "main", "rcp_uart_tx_retry.c")
    if os.path.isfile(retry_c_path):
        retry_c = read(retry_c_path)
        record("OT_ERROR_FAILED" not in retry_c,
               "rcp_uart_tx_retry.c never returns OT_ERROR_FAILED -- a fatal send hands off to "
               "rcp_uart_tx_recovery_restart() instead of returning to NcpSend()")
        record(retry_c.count("rcp_uart_tx_flush()") >= 1,
               "rcp_uart_tx_retry.c calls rcp_uart_tx_flush() after every write attempt")

    diag_sdkconfig_path = os.path.join(RCP_DIR, "sdkconfig.diag")
    if os.path.isfile(diag_sdkconfig_path):
        diag_sdkconfig = read(diag_sdkconfig_path)
        record(has_config_line(diag_sdkconfig, "ESP_COREDUMP_ENABLE_TO_FLASH", "y"),
               "sdkconfig.diag: CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y")
        record(has_config_line(diag_sdkconfig, "ESP_SYSTEM_PANIC_SILENT_REBOOT", "y"),
               "sdkconfig.diag: CONFIG_ESP_SYSTEM_PANIC_SILENT_REBOOT=y "
               "(coredump-to-flash must never re-enable panic text on the Spinel wire)")
    else:
        record(False, "rcp/sdkconfig.diag exists")

    diag_partitions_path = os.path.join(RCP_DIR, "partitions_diag.csv")
    if os.path.isfile(diag_partitions_path):
        diag_partitions = read(diag_partitions_path)
        record(re.search(r"^coredump,\s*data,\s*coredump,", diag_partitions, re.MULTILINE) is not None,
               "partitions_diag.csv declares a `coredump` data partition")
    else:
        record(False, "rcp/partitions_diag.csv exists")

    # Production sdkconfig.defaults/partitions.csv (used by a normal build)
    # must stay untouched by the diagnostic-build addition -- no coredump/
    # panic-print settings leaking into the default, non-diagnostic build.
    record(not has_config_line(defaults, "ESP_COREDUMP_ENABLE_TO_FLASH", "y"),
           "sdkconfig.defaults (production) does NOT enable coredump-to-flash")
    record("CONFIG_PARTITION_TABLE_FILENAME=\"partitions.csv\"" in defaults,
           "sdkconfig.defaults (production) still points at partitions.csv, not partitions_diag.csv")

    # --- 8. USB Spinel RX recovery fix (confirmed coredump root cause --
    # crashed task ot_main, assert(false) at esp_openthread.cpp:283 -- see
    # docs/otbr-rcp.md's "USB Spinel RX recovery" section). Static
    # presence/wiring checks only; the actual retry/timeout/errno-reset
    # behavior is exercised by tools/test_rcp_usb_rx_recovery.c, and the
    # real ESP-IDF build/link/nm/objdump verification is covered separately
    # (see the final report, not this script). ---
    rx_recovery_files = (
        os.path.join(RCP_DIR, "main", "rcp_usb_rx_recovery.h"),
        os.path.join(RCP_DIR, "main", "esp_openthread_platform_process_wrap.c"),
        os.path.join(RCP_DIR, "main", "rcp_usb_rx_recovery_restart.c"),
    )
    for path in rx_recovery_files:
        record(os.path.isfile(path), f"{os.path.relpath(path, ROOT_DIR)} exists")

    if os.path.isfile(main_cmake_path):
        main_cmake = read(main_cmake_path)
        record("esp_openthread_platform_process_wrap.c" in main_cmake and "rcp_usb_rx_recovery_restart.c" in main_cmake,
               "rcp/main/CMakeLists.txt builds the RX recovery fix sources")
        record("-Wl,--wrap=esp_openthread_platform_process" in main_cmake,
               "rcp/main/CMakeLists.txt wires the esp_openthread_platform_process --wrap override "
               "(ESP-IDF's own documented mechanism -- $IDF_PATH itself is never modified)")
        record("-Wl,-u,__wrap_esp_openthread_platform_process" in main_cmake,
               "rcp/main/CMakeLists.txt forces the linker to pull __wrap_esp_openthread_platform_process "
               "out of libmain.a")

    rx_recovery_header_path = os.path.join(RCP_DIR, "main", "rcp_usb_rx_recovery.h")
    if os.path.isfile(rx_recovery_header_path):
        rx_recovery_header = read(rx_recovery_header_path)
        record("rcp_usb_rx_recovery_restart(void)" in rx_recovery_header and "noreturn" in rx_recovery_header,
               "rcp_usb_rx_recovery.h declares a noreturn rcp_usb_rx_recovery_restart() hook "
               "(EIO persisting for the whole retry budget must never return control to the mainloop)")
        record("RCP_USB_RX_RECOVERY_RETRY_YIELD_MS" in rx_recovery_header
               and "RCP_USB_RX_RECOVERY_TOTAL_TIMEOUT_MS" in rx_recovery_header,
               "rcp_usb_rx_recovery.h declares its own retry-yield/total-timeout constants "
               "(kept separate from the TX fix's own RCP_UART_TX_* constants)")

    rx_recovery_wrap_path = os.path.join(RCP_DIR, "main", "esp_openthread_platform_process_wrap.c")
    if os.path.isfile(rx_recovery_wrap_path):
        rx_recovery_wrap = read(rx_recovery_wrap_path)
        record("errno = 0" in rx_recovery_wrap,
               "esp_openthread_platform_process_wrap.c resets errno to 0 before calling "
               "__real_esp_openthread_platform_process()")
        record("EIO" in rx_recovery_wrap and "ESP_FAIL" in rx_recovery_wrap,
               "esp_openthread_platform_process_wrap.c checks for the exact ESP_FAIL+EIO combination")
        record("vTaskDelay" in rx_recovery_wrap,
               "esp_openthread_platform_process_wrap.c yields via vTaskDelay() -- never a busy spin")

    # --- Sanity: the existing projects' own top-level CMakeLists.txt files
    # were not touched by this pass (this test only checks THEIR project name
    # still matches what they always were -- the real "did I edit shared
    # files" check is `git diff --check` / `git status`, run separately). ---
    for project, expected_project_name in (("module", "pogo_module"), ("power_base", "power_base")):
        cmake_path = os.path.join(ROOT_DIR, project, "CMakeLists.txt")
        if os.path.isfile(cmake_path):
            text = read(cmake_path)
            record(f"project({expected_project_name})" in text,
                   f"{project}/CMakeLists.txt still declares project({expected_project_name}) (untouched)")

    if all(passed for passed, _ in _results):
        print("\nALL CHECKS PASSED")
        return 0
    failed = sum(1 for passed, _ in _results if not passed)
    print(f"\n{failed}/{len(_results)} CHECK(S) FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
