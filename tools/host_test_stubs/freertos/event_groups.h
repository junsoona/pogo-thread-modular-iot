/*
 * Minimal host-only stand-in for FreeRTOS's event_groups.h -- just enough
 * of the real API surface (xEventGroupCreate/SetBits/ClearBits/GetBits) for
 * components/topology_gate/topology_gate.c to host-compile unmodified (see
 * that file's own comment and tools/host_test_stubs/esp_err.h's "put this
 * directory first on the include path" convention).
 *
 * tools/test_topology_gate.c's host tests are single-threaded, so this
 * reproduces each real FreeRTOS event-group call's OBSERVABLE behavior
 * (in particular: xEventGroupClearBits() returns the bits value as they
 * were BEFORE this call cleared them -- an atomic read-and-clear on real
 * FreeRTOS) without needing any actual thread-safety of its own. If
 * topology_gate.c ever starts using a different event-group call, add it
 * here rather than widening this stub speculatively.
 */
#pragma once

#include <stdint.h>
#include <stdlib.h>

typedef uint32_t EventBits_t;

typedef struct {
    EventBits_t bits;
} *EventGroupHandle_t;

static inline EventGroupHandle_t xEventGroupCreate(void)
{
    EventGroupHandle_t g = (EventGroupHandle_t)malloc(sizeof(*g));
    if (g != NULL) {
        g->bits = 0;
    }
    return g;
}

static inline EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t bits_to_set)
{
    if (g == NULL) {
        return 0;
    }
    g->bits |= bits_to_set;
    return g->bits;
}

/* Real FreeRTOS semantics: returns the value the event group had BEFORE
 * these bits were cleared. */
static inline EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t bits_to_clear)
{
    if (g == NULL) {
        return 0;
    }
    EventBits_t prev = g->bits;
    g->bits &= (EventBits_t)~bits_to_clear;
    return prev;
}

static inline EventBits_t xEventGroupGetBits(EventGroupHandle_t g)
{
    return (g != NULL) ? g->bits : 0;
}
