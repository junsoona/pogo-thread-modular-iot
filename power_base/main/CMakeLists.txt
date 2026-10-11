idf_component_register(
    SRCS "app_main.c" "enrollment_runtime.c" "enrollment_console.c" "enrollment_ble.c" "enrollment_mgmt.c" "enrollment_locate_hints.c"
         "enrollment_reenroll.c"
    INCLUDE_DIRS "."
    REQUIRES pogo_proto pogo_port parent_service power_base_config power_base_thread_config
             power_base_bridge_config power_base_transport linux_bridge_exporter
             power_base_enrollment thread_manager nvs_flash esp_event esp_netif esp_hw_support openthread
             console power_base_ble_auth power_base_ble_enroll protocomm bt
             power_base_mgmt coap_response_timing mbedtls
             power_base_reenroll esp_driver_gpio esp_timer
             pogo_build_info power_base_ot_store
)

# components/power_base_ot_store stands in front of OpenThread's two persistent
# stores (pb_volatile_store.h): every reference to one of these functions
# resolves to __wrap_<function> there, and __real_<function> is the platform's
# own. Only this project links them. A link without these options fails (the
# component refers to the __real_ names); tools/check_power_base_ot_store.py
# checks in the linked image that no other caller reaches the real functions.
foreach(pb_wrapped
        otPlatSettingsGet otPlatSettingsSet otPlatSettingsAdd otPlatSettingsDelete otPlatSettingsWipe
        psa_its_get_info psa_its_get psa_its_set psa_its_remove)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${pb_wrapped}")
endforeach()
