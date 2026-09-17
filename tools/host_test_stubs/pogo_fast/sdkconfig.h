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

/*
 * This follow-up (Power Base -> Linux OTBR northbound bridge transport):
 * same "much smaller, identical logic/ordering, real wall-clock" convention
 * as every value above -- used by tools/test_linux_bridge_exporter.c, which
 * links the REAL components/linux_bridge_exporter/linux_bridge_exporter.c.
 * QUEUE_LEN is deliberately tiny (4) so a test can actually fill it to
 * exercise the overflow/drop/rate-limited-warning path without sending
 * dozens of events.
 */
#define CONFIG_LINUX_BRIDGE_EXPORTER_QUEUE_LEN 4
/*
 * UNLIKE every other "fast" value in this file, these two match the REAL
 * production Kconfig defaults (5000/10000ms) exactly, on purpose:
 * tools/test_linux_bridge_exporter.c asserts exact send-count deltas across
 * its scenarios, which a short periodic resync/heartbeat interval would
 * intermittently perturb (an extra, correctly-timed-but-untracked send
 * racing in mid-scenario). Keeping the test's own total runtime well under
 * 5s avoids that interference entirely while still exercising the exact
 * production timing values.
 */
#define CONFIG_LINUX_BRIDGE_EXPORTER_FULL_RESYNC_INTERVAL_MS 5000
#define CONFIG_LINUX_BRIDGE_EXPORTER_HEARTBEAT_INTERVAL_MS 10000
/*
 * This pass (SAMPLE transport-load reduction): matches the real production
 * Kconfig default (1000ms) exactly, same reasoning as the resync/heartbeat
 * values directly above -- tools/test_linux_bridge_exporter.c's exact
 * send-count-delta assertions run well under 1s per scenario, so this never
 * perturbs them. tools/test_linux_bridge_exporter_rate_limit.c (this pass)
 * uses the SHORT pogo_fast_bridge_timers stub instead, same convention as
 * the resync/heartbeat timer test.
 */
#define CONFIG_LINUX_BRIDGE_EXPORTER_SAMPLE_MIN_INTERVAL_MS 1000
