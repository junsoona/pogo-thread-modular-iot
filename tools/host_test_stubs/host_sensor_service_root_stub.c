/*
 * Host-only stand-in for the ONE sensor_service.h symbol
 * components/sensor_transport/sensor_transport.c references
 * (sensor_service_root_record_sample(), called only from that file's
 * ROOT-side coap_sample_handler()). No host test in this repository
 * exercises coap_sample_handler() itself (it needs a real inbound CoAP
 * request, which is out of scope for tools/test_sensor_transport_gate_retry.c's
 * client-side retry/gate scenarios) -- this stub exists purely so the
 * symbol resolves at link time; it is never called by anything under test.
 * Linking the real components/sensor_service/sensor_service.c instead would
 * pull in sensor_provider/module_config/role_manager's own real driver
 * dependencies for no purpose here.
 */
#include "sensor_service/sensor_service.h"

void sensor_service_root_record_sample(const sensor_sample_t *sample)
{
    (void)sample;
}
