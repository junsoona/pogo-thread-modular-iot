/*
 * Host-only stand-in for the project's generated sdkconfig.h, used ONLY by
 * host tests that link REAL production .c files depending on CONFIG_POGO_*
 * (components/pogo_port, components/pogo_proto, components/child_service,
 * components/parent_service, components/sensor_service, components/pms5003)
 * or, this follow-up, CONFIG_SENSOR_COAP_ACK_TIMEOUT_MS
 * (components/sensor_transport, components/topology_reporter, via
 * components/coap_response_timing/Kconfig) -- see
 * tools/test_pogo_port_hotswap_recovery.c, tools/test_conditioning_provisioning.c,
 * and tools/test_sensor_transport_gate_retry.c. The ONE sdkconfig.h stub in
 * this repository (see its own "put this directory first" convention
 * below) -- new CONFIG_* values a newly real-linked component needs are
 * added here rather than creating a second stub.
 *
 * Values are deliberately much SMALLER than components/pogo_proto/Kconfig's
 * real defaults (300ms ACK timeout, 3000ms hotswap cooldown, ...) so a host
 * test that exercises real retry/cooldown/timeout logic (real wall-clock
 * delays, via tools/host_test_stubs/freertos/task.h's pthread+nanosleep
 * vTaskDelay()) finishes in a reasonable time -- this changes ONLY how long
 * each state is held, never which state transitions happen or in what order,
 * so the logic under test is exercised exactly as it runs on real hardware,
 * just faster. Put `-I tools/host_test_stubs/pogo_fast` BEFORE
 * `-I tools/host_test_stubs` on the include path so this shadows nothing
 * else (there is only one sdkconfig.h stub) but IS found before any real
 * ESP-IDF build directory's generated one could ever leak in.
 *
 * NEVER used by the real module/power_base firmware builds -- those get
 * their real sdkconfig.h from the ESP-IDF build system, driven by
 * module/sdkconfig.defaults / power_base/sdkconfig.defaults and
 * components/pogo_proto/Kconfig's real defaults, completely independent of
 * this file.
 */
#pragma once

#define CONFIG_POGO_UART_PORT_NUM     1
#define CONFIG_POGO_UART_TX_GPIO      10
#define CONFIG_POGO_UART_RX_GPIO      11
#define CONFIG_POGO_UART_BAUD_RATE    115200
#define CONFIG_POGO_UART_RX_BUF_SIZE  512
#define CONFIG_POGO_UART_TX_BUF_SIZE  512

#define CONFIG_POGO_PING_INTERVAL_MS      60
#define CONFIG_POGO_ACK_TIMEOUT_MS         40
#define CONFIG_POGO_JOIN_TIMEOUT_MS       400
#define CONFIG_POGO_MAX_RETRY               3

#define CONFIG_POGO_ATTACH_SETTLE_MS       30
#define CONFIG_POGO_HOTSWAP_COOLDOWN_MS    150
#define CONFIG_POGO_DETECT_RECONCILE_MS    40

/*
 * This follow-up (SED CoAP response timing correction): deliberately much
 * SMALLER than components/coap_response_timing/Kconfig's real default
 * (2000ms) for the exact same "fast host test, identical logic/ordering"
 * reason as the CONFIG_POGO_* values above -- used by
 * tools/test_sensor_transport_gate_retry.c, which links the REAL
 * components/sensor_transport/sensor_transport.c.
 */
#define CONFIG_SENSOR_COAP_ACK_TIMEOUT_MS 40
