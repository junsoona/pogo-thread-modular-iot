/*
 * Host-only stand-in for the generated sdkconfig.h carrying the REAL
 * components/pogo_proto/Kconfig defaults -- the values module/ and
 * power_base/ actually build with (neither sdkconfig.defaults overrides
 * them). Used ONLY by tools/test_pogo_port_cooldown_hello.c.
 *
 * tools/host_test_stubs/pogo_fast/sdkconfig.h shrinks every timing to keep
 * the other host tests fast, and in doing so changes the one ratio that
 * test is about: with pogo_fast's numbers a handshake cycle (4 x 40ms) is
 * LONGER than the cooldown (150ms), so two retrying ports always overlap
 * and a retry-phase lockout cannot happen at all. With the real numbers
 * (4 x 300ms handshake vs a 3000ms cooldown) it can. The test re-reads
 * components/pogo_proto/Kconfig at run time and fails if these drift.
 */
#pragma once

#define CONFIG_POGO_UART_PORT_NUM     1
#define CONFIG_POGO_UART_TX_GPIO      10
#define CONFIG_POGO_UART_RX_GPIO      11
#define CONFIG_POGO_UART_BAUD_RATE    115200
#define CONFIG_POGO_UART_RX_BUF_SIZE  512
#define CONFIG_POGO_UART_TX_BUF_SIZE  512

#define CONFIG_POGO_PING_INTERVAL_MS     1000
#define CONFIG_POGO_ACK_TIMEOUT_MS        300
#define CONFIG_POGO_JOIN_TIMEOUT_MS     20000
#define CONFIG_POGO_MAX_RETRY               3

#define CONFIG_POGO_ATTACH_SETTLE_MS      150
#define CONFIG_POGO_HOTSWAP_COOLDOWN_MS  3000
#define CONFIG_POGO_DETECT_RECONCILE_MS   500
