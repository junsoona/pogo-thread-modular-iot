/*
 * Minimal host-only stand-in for FreeRTOS's own FreeRTOS.h, used ONLY to
 * host-compile components/topology_gate/topology_gate.c -- its logic has
 * zero real hardware dependency, but it uses ONE real FreeRTOS primitive
 * (an event group) for actual cross-task synchronization (see that
 * component's own header comment for why a plain bool is not acceptable
 * here). See tools/host_test_stubs/esp_err.h's own comment for the same
 * "put this directory first on the include path" convention.
 *
 * This file only needs to exist and be includable -- topology_gate.c
 * itself never references anything FreeRTOS.h would normally provide
 * directly (BaseType_t etc.), only freertos/event_groups.h's API (see that
 * stub).
 */
#pragma once
