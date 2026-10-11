# ESP-IDF's protocomm component is not built for the linux target, so its
# sources are compiled here directly -- unmodified, from $IDF_PATH.
set(pc $ENV{IDF_PATH}/components/protocomm)
set(repo ${CMAKE_CURRENT_LIST_DIR}/../../..)

set(shim_dirs "")
if(CMAKE_HOST_APPLE)
    # macOS has no <endian.h>; security2.c needs htobe32()/be32toh().
    list(APPEND shim_dirs "host_shim")
endif()

idf_component_register(
    SRCS "harness_main.c"
         # ESP-IDF: protocomm core, Security 2, SRP-6a, session protobufs
         "${pc}/src/common/protocomm.c"
         "${pc}/src/security/security2.c"
         "${pc}/src/crypto/srp6a/esp_srp.c"
         "${pc}/src/crypto/srp6a/esp_srp_mpi.c"
         "${pc}/proto-c/constants.pb-c.c"
         "${pc}/proto-c/sec0.pb-c.c"
         "${pc}/proto-c/sec1.pb-c.c"
         "${pc}/proto-c/sec2.pb-c.c"
         "${pc}/proto-c/session.pb-c.c"
         # this repo: the code under test
         "${repo}/components/power_base_ble_enroll/pb_ble_service.c"
         "${repo}/components/power_base_ble_enroll/pb_ble_admission.c"
         "${repo}/components/power_base_ble_auth/power_base_ble_auth.c"
         "${repo}/components/power_base_enrollment/power_base_enrollment.c"
         "${repo}/components/power_base_mgmt/pb_mgmt_client.c"
         "${repo}/components/power_base_mgmt/pb_mgmt_proto.c"
         "${repo}/components/power_base_reenroll/pb_reenroll.c"
         "${repo}/components/power_base_reenroll/pb_reenroll_store.c"
         # ... and the board's network stores as the firmware checks them
         # before it hands out a release receipt: the real component over a
         # fake key storage (store_glue.c)
         "${repo}/components/power_base_ot_store/pb_stored_network.c"
         "${repo}/components/power_base_ot_store/pb_volatile_store.c"
         "store_glue.c"
         # functional NVS stand-in (the completion marker)
         "${repo}/tools/host_test_stubs/nvs_sim/nvs_sim.c"
    INCLUDE_DIRS "${repo}/tools/host_test_stubs/nvs_sim" ${shim_dirs}
                 "${repo}/components/power_base_ble_enroll/include"
                 "${repo}/components/power_base_ble_auth/include"
                 "${repo}/components/power_base_enrollment/include"
                 "${repo}/components/power_base_mgmt/include"
                 "${repo}/components/power_base_reenroll/include"
                 "${pc}/include/common" "${pc}/include/security" "${pc}/include/crypto/srp6a" "${pc}/proto-c"
    PRIV_INCLUDE_DIRS "${pc}/src/common" "${pc}/src/crypto/srp6a"
                      "${repo}/components/power_base_ot_store/include"
                      # stand-ins for OpenThread's settings header and for the PSA
                      # storage header, as in tools/test_power_base_ot_store.c
                      "${repo}/tools/host_test_stubs/power_base_ot_store/ot"
                      "${repo}/tools/host_test_stubs/power_base_ot_store/psa"
    REQUIRES mbedtls protobuf-c esp_event esp_hw_support)

# What the firmware's sdkconfig.ble_enrollment selects: Security 2 only.
target_compile_definitions(${COMPONENT_LIB} PUBLIC CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_2=1)
# The store component's "a chip reset" hooks (host builds only).
target_compile_definitions(${COMPONENT_LIB} PRIVATE PB_VOLATILE_STORE_TEST_HOOKS)
