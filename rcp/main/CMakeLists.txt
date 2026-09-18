# Upstream's own main/CMakeLists.txt has no REQUIRES at all -- it relies on
# its main/idf_component.yml pulling in the (managed/local-path)
# `ot_examples_common` component, whose OWN CMakeLists.txt privately
# requires `openthread` (see $IDF_PATH/examples/openthread/ot_common_components/
# ot_examples_common/CMakeLists.txt), which is what actually pulls the whole
# openthread/esp_openthread component chain into the build. This project
# does not need `ot_examples_common`'s FTD/CLI/external-coexistence helper
# sources (see esp_ot_rcp.c's own comment) and has no idf_component.yml at
# all (item 7 of docs/otbr-rcp.md's requirements) -- so the same
# `openthread` (and the other components esp_ot_rcp.c directly calls into)
# is required here explicitly instead. `console` is listed even though the
# default build (CONFIG_OPENTHREAD_RCP_SPINEL_CONSOLE=n) never opens
# esp_console.h, so flipping that one Kconfig option back on later does not
# also require editing this file. `esp_timer` was added by the USB Spinel
# TX retry fix below -- rcp_uart_tx_retry.c calls esp_timer_get_time() for
# its bounded retry deadline. `esp_hal_usb` was added for
# rcp_uart_tx_io.c's rcp_uart_tx_flush(), which calls
# usb_serial_jtag_ll_txfifo_flush() (hal/usb_serial_jtag_ll.h) to preserve
# stock otPlatUartSend()'s own post-write flush semantics -- see
# rcp_uart_tx_retry.h's own comment. `espcoredump` is REQUIRED here
# (unconditionally, in both the production and diagnostic sdkconfig
# variants) because rcp/CMakeLists.txt sets MINIMAL_BUILD ON: a component
# that nothing REQUIRES is excluded from the build entirely, which also
# hides its Kconfig options ("unknown kconfig symbol
# 'ESP_COREDUMP_ENABLE_TO_FLASH'" from kconfgen) -- confirmed by an actual
# diagnostic-variant build before this line was added. Listing it here
# costs nothing in the production build: with the default
# CONFIG_ESP_COREDUMP_ENABLE_TO_NONE (rcp/sdkconfig.defaults never touches
# this), $IDF_PATH/components/esp_system/panic.c's own
# `#if CONFIG_ESP_COREDUMP_ENABLE` block (and everything else in
# `espcoredump`) compiles out to nothing -- see docs/otbr-rcp.md section 10.
idf_component_register(SRCS "esp_ot_rcp.c" "rcp_uart_tx_retry.c" "rcp_uart_tx_io.c" "esp_ot_uart_send_wrap.c"
                             "esp_openthread_platform_process_wrap.c" "rcp_usb_rx_recovery_restart.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES openthread nvs_flash esp_event vfs console esp_timer esp_hal_usb espcoredump)

# USB Spinel TX retry fix (RESET_FAULT under real hardware load -- see
# rcp_uart_tx_retry.c/esp_ot_uart_send_wrap.c and docs/otbr-rcp.md's "USB
# Spinel TX retry" section): overrides the `openthread` component's own
# otPlatUartSend() via ESP-IDF's own documented linker `--wrap` mechanism
# ($IDF_PATH/examples/build_system/wrappers) instead of touching
# $IDF_PATH itself. `-Wl,-u,__wrap_otPlatUartSend` additionally forces the
# linker to pull that symbol's object out of libmain.a: nothing calls it by
# that literal name at compile time (the --wrap substitution only happens
# during the final link's symbol resolution), so without this it could
# otherwise be left out of a plain archive scan.
#
# USB Spinel RX recovery fix -- see esp_openthread_platform_process_wrap.c's
# own header comment and docs/otbr-rcp.md's "USB Spinel RX recovery" section
# (section 12) for the full evidence-tier breakdown. Confirmed by a real-
# hardware coredump: the crash MECHANISM itself -- crashed task ot_main,
# assert(false) at esp_openthread.cpp:283 right after
# esp_openthread_launch_mainloop() returned at esp_openthread.cpp:279. The
# coredump does NOT preserve errno from the moment of the panic, so it does
# not by itself confirm that ESP_FAIL+EIO was the trigger -- that specific
# recovery target is identified by source analysis and backed by a combined
# diagnostic+production real-hardware soak (2 hours total, zero RCP/OTBR
# restarts) that had this RX fix and the existing TX fix (above) applied
# TOGETHER, which does not isolate either fix's own individual effect.
# Overrides the `openthread` component's own esp_openthread_platform_process()
# via the same documented `--wrap` mechanism, same reasoning as the TX fix
# above. This is a SEPARATE fix on a SEPARATE call path -- not a restatement
# of, and not dependent on, the TX fix.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=otPlatUartSend"
    "-Wl,-u,__wrap_otPlatUartSend"
    "-Wl,--wrap=esp_openthread_platform_process"
    "-Wl,-u,__wrap_esp_openthread_platform_process")
