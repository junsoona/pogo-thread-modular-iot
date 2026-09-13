#include "sensor_provider/sensor_provider.h"

extern const sensor_provider_ops_t g_mock_sensor_provider_ops;

const sensor_provider_ops_t *sensor_provider_lookup(uint32_t driver_id)
{
    switch (driver_id) {
    case SENSOR_DRIVER_ID_MOCK:
        return &g_mock_sensor_provider_ops;
    default:
        return NULL; /* unsupported driver_id: CONFIG_ERROR upstream, no auto-detection */
    }
}
