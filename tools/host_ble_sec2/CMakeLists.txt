# Host (ESP-IDF "linux" target) harness that runs ESP-IDF's REAL protocomm
# Security 2 server code and this repo's REAL BLE enrollment service code
# without a board, so that ESP-IDF's official Python client can talk to them.
# Driven by tools/test_power_base_ble_sec2_interop.py -- see that file.
#
#   source ~/.espressif/v6.1/esp-idf/export.sh
#   export IDF_COMPONENT_MANAGER=0
#   H="$HOME/esp-builds/pogo_host_ble_sec2"
#   idf.py --preview -C tools/host_ble_sec2 -B "$H" -D SDKCONFIG="$H/sdkconfig" set-target linux
#   idf.py -C tools/host_ble_sec2 -B "$H" -D SDKCONFIG="$H/sdkconfig" build
cmake_minimum_required(VERSION 3.16)
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pb_ble_sec2_harness)
