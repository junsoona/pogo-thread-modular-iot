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
# also require editing this file.
idf_component_register(SRCS "esp_ot_rcp.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES openthread nvs_flash esp_event vfs console)
