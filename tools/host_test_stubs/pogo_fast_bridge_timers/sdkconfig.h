/*
 * A SECOND sdkconfig.h stub, used ONLY by
 * tools/test_linux_bridge_exporter_timers.c -- deliberately identical to
 * tools/host_test_stubs/pogo_fast/sdkconfig.h EXCEPT for the three
 * CONFIG_LINUX_BRIDGE_EXPORTER_* values below, which here are SHORT (unlike
 * that other stub's own comment explaining why IT keeps them at the real
 * production 5000/10000ms defaults). This follow-up's correction pass
 * explicitly asks for a host test that waits for the real periodic
 * topology/sample resync and heartbeat timers to actually fire in real
 * wall-clock time -- that requires a short interval, which would otherwise
 * make tools/test_linux_bridge_exporter.c's own exact send-count-delta
 * assertions flaky (an extra, correctly-timed periodic send racing into the
 * middle of an unrelated scenario). Keeping this as a SEPARATE stub
 * directory (rather than changing the shared one) means the two test files
 * fully coexist: put `-I tools/host_test_stubs/pogo_fast_bridge_timers`
 * BEFORE `-I tools/host_test_stubs` on the include path for this one test's
 * own build command.
 *
 * NEVER used by the real module/power_base firmware builds -- see the other
 * stub's own header comment for the same disclaimer.
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

#define CONFIG_SENSOR_COAP_ACK_TIMEOUT_MS 40

#define CONFIG_LINUX_BRIDGE_EXPORTER_QUEUE_LEN 4

/*
 * SHORT on purpose -- see this file's own top comment. 150ms/250ms are
 * still comfortably longer than one EXPORTER_POLL_TICK_MS (200ms, see
 * linux_bridge_exporter.c) turn-around and one bounded CoAP attempt
 * (~100ms at this stub's CONFIG_SENSOR_COAP_ACK_TIMEOUT_MS=40), so a real
 * resync/heartbeat cycle can complete and be observed within a couple
 * hundred milliseconds of wall-clock test time.
 */
#define CONFIG_LINUX_BRIDGE_EXPORTER_FULL_RESYNC_INTERVAL_MS 150
#define CONFIG_LINUX_BRIDGE_EXPORTER_HEARTBEAT_INTERVAL_MS 250
/*
 * This pass (SAMPLE transport-load reduction): also SHORT on purpose, same
 * reasoning as the two values directly above -- a real per-uuid SAMPLE
 * rate-limit test (tools/test_linux_bridge_exporter_rate_limit.c) needs to
 * actually observe the window reopening in real wall-clock time without a
 * multi-second sleep. Deliberately still >= one EXPORTER_POLL_TICK_MS
 * (200ms) so a test can distinguish "still within the window" from "the
 * window reopened" with a plain sleep_ms() of a similar order.
 */
#define CONFIG_LINUX_BRIDGE_EXPORTER_SAMPLE_MIN_INTERVAL_MS 300
