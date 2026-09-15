/*
 * Minimal host-only stand-in for FreeRTOS's own FreeRTOS.h, used to
 * host-compile components whose logic has zero real hardware dependency but
 * which still use one or two real FreeRTOS primitives for actual cross-task
 * synchronization -- components/topology_gate/topology_gate.c (an event
 * group, see that component's own header comment for why a plain bool is
 * not acceptable there) and components/collector_endpoint/collector_endpoint.c
 * (a portMUX_TYPE critical section, see that component's own header comment
 * for the same reasoning). See tools/host_test_stubs/esp_err.h's own comment
 * for the same "put this directory first on the include path" convention.
 *
 * A host test process is single-threaded, so taskENTER_CRITICAL()/
 * taskEXIT_CRITICAL() below are no-ops -- they exist purely so the REAL
 * production source compiles and links unmodified; they provide no actual
 * mutual exclusion (none is needed here, and none would be meaningful to
 * fake for a portMUX spinlock on a host CPU anyway).
 */
#pragma once

/*
 * Real ESP-IDF's FreeRTOS.h is the "master" header: task.h/queue.h/semphr.h
 * all assume it was included first and rely on IT for TickType_t/BaseType_t/
 * pdMS_TO_TICKS/etc. A production .c file that includes only "freertos/
 * FreeRTOS.h" itself (e.g. components/sensor_provider/include/sensor_provider/
 * sensor_provider.h) still needs those -- so this stub pulls task.h in too,
 * matching that real transitive-include behavior instead of requiring every
 * such file to add its own extra include.
 */
#include "freertos/task.h"

typedef struct {
    int dummy;
} portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED {0}

#define taskENTER_CRITICAL(mux) ((void)(mux))
#define taskEXIT_CRITICAL(mux)  ((void)(mux))
