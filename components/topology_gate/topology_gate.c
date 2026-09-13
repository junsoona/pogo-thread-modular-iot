#include "topology_gate/topology_gate.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define TOPOLOGY_GATE_BIT_REGISTERED            (1u << 0)
#define TOPOLOGY_GATE_BIT_REREGISTER_REQUESTED  (1u << 1)

static EventGroupHandle_t s_group;

esp_err_t topology_gate_init(void)
{
    s_group = xEventGroupCreate();
    return (s_group != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

void topology_gate_open(void)
{
    if (s_group == NULL) {
        return;
    }
    xEventGroupSetBits(s_group, TOPOLOGY_GATE_BIT_REGISTERED);
}

void topology_gate_close(void)
{
    if (s_group == NULL) {
        return;
    }
    xEventGroupClearBits(s_group, TOPOLOGY_GATE_BIT_REGISTERED);
}

bool topology_gate_is_open(void)
{
    if (s_group == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_group) & TOPOLOGY_GATE_BIT_REGISTERED) != 0;
}

void topology_gate_request_reregister(void)
{
    if (s_group == NULL) {
        return;
    }
    xEventGroupSetBits(s_group, TOPOLOGY_GATE_BIT_REREGISTER_REQUESTED);
}

bool topology_gate_take_reregister_request(void)
{
    if (s_group == NULL) {
        return false;
    }
    /* xEventGroupClearBits() returns the bits value as it was BEFORE this
     * call cleared them -- an atomic read-and-clear, so two overlapping
     * calls (there is only ever one caller, topology_reporter's own task,
     * but this is what makes that safe even under FreeRTOS preemption) can
     * never both observe the same pending request. */
    EventBits_t prev = xEventGroupClearBits(s_group, TOPOLOGY_GATE_BIT_REREGISTER_REQUESTED);
    return (prev & TOPOLOGY_GATE_BIT_REREGISTER_REQUESTED) != 0;
}
