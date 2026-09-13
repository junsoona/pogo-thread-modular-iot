#include "sensor_provider/sensor_provider.h"

extern const sensor_provider_ops_t g_mock_sensor_provider_ops;

/* See sensor_provider_register()'s doc comment for why real drivers register
 * at runtime instead of sensor_provider.c REQUIRES-ing every driver
 * component directly. MOCK stays a compile-time switch case (it lives
 * inside this same component, so there is no circular-dependency reason to
 * route it through the registry too), and is always available regardless of
 * whether anything ever registers. */
#define SENSOR_PROVIDER_MAX_REGISTERED 8

typedef struct {
    uint32_t driver_id;
    const sensor_provider_ops_t *ops;
} registered_driver_t;

static registered_driver_t s_registered[SENSOR_PROVIDER_MAX_REGISTERED];
static int s_registered_count = 0;

esp_err_t sensor_provider_register(uint32_t driver_id, const sensor_provider_ops_t *ops)
{
    if (ops == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < s_registered_count; i++) {
        if (s_registered[i].driver_id == driver_id) {
            s_registered[i].ops = ops; /* re-registering the same id replaces, rather than duplicating */
            return ESP_OK;
        }
    }
    if (s_registered_count >= SENSOR_PROVIDER_MAX_REGISTERED) {
        return ESP_ERR_NO_MEM;
    }
    s_registered[s_registered_count].driver_id = driver_id;
    s_registered[s_registered_count].ops = ops;
    s_registered_count++;
    return ESP_OK;
}

const sensor_provider_ops_t *sensor_provider_lookup(uint32_t driver_id)
{
    if (driver_id == SENSOR_DRIVER_ID_MOCK) {
        return &g_mock_sensor_provider_ops;
    }
    for (int i = 0; i < s_registered_count; i++) {
        if (s_registered[i].driver_id == driver_id) {
            return s_registered[i].ops;
        }
    }
    return NULL; /* unsupported driver_id: CONFIG_ERROR upstream, no auto-detection */
}
