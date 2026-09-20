#include "host_i2c_fake_device.h"

#include <string.h>

#include "driver/i2c_master.h"

#define MAX_RESPONSE_LEN   8u
#define MAX_QUEUED_RESPONSES 4u

typedef struct {
    uint8_t data[MAX_RESPONSE_LEN];
    size_t  len;
} fake_response_t;

static struct {
    fake_response_t responses[MAX_QUEUED_RESPONSES];
    size_t queued_count;
    size_t queue_head;

    uint8_t last_transmit[MAX_RESPONSE_LEN];
    size_t  last_transmit_len;

    int receive_call_count;
    bool fail_writes;
} s_fake;

/* Opaque handles this fake hands out -- never dereferenced, just non-NULL
 * placeholders so a driver's own NULL-checks (there are none in this
 * driver family, but a real bus/dev handle is still expected to be
 * non-NULL) behave the same as against real hardware. driver/i2c_master.h
 * only forward-declares these structs (real ESP-IDF's own handles are
 * opaque too) -- this is the one translation unit that actually completes
 * them, since it is the only one that ever instantiates or touches one. */
struct i2c_master_bus_t {
    int dummy;
};
struct i2c_master_dev_t {
    int dummy;
};
static struct i2c_master_bus_t s_fake_bus;
static struct i2c_master_dev_t s_fake_dev;

void host_i2c_fake_device_reset(void)
{
    memset(&s_fake, 0, sizeof(s_fake));
}

void host_i2c_fake_device_queue_response(const uint8_t *data, size_t len)
{
    if (len > MAX_RESPONSE_LEN || s_fake.queued_count >= MAX_QUEUED_RESPONSES) {
        return; /* test-only helper; a test that hits this has a bug in itself */
    }
    size_t slot = (s_fake.queue_head + s_fake.queued_count) % MAX_QUEUED_RESPONSES;
    memcpy(s_fake.responses[slot].data, data, len);
    s_fake.responses[slot].len = len;
    s_fake.queued_count++;
}

void host_i2c_fake_device_set_fail_writes(bool fail_writes)
{
    s_fake.fail_writes = fail_writes;
}

size_t host_i2c_fake_device_last_transmit(uint8_t *out, size_t out_cap)
{
    size_t n = s_fake.last_transmit_len < out_cap ? s_fake.last_transmit_len : out_cap;
    memcpy(out, s_fake.last_transmit, n);
    return s_fake.last_transmit_len;
}

int host_i2c_fake_device_receive_call_count(void)
{
    return s_fake.receive_call_count;
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *bus_config, i2c_master_bus_handle_t *ret_bus_handle)
{
    (void)bus_config;
    *ret_bus_handle = &s_fake_bus;
    return ESP_OK;
}

esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus_handle)
{
    (void)bus_handle;
    return ESP_OK;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus_handle, const i2c_device_config_t *dev_config,
                                     i2c_master_dev_handle_t *ret_handle)
{
    (void)bus_handle;
    (void)dev_config;
    *ret_handle = &s_fake_dev;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus_handle, uint16_t address, int xfer_timeout_ms)
{
    (void)bus_handle;
    (void)address;
    (void)xfer_timeout_ms;
    return ESP_OK; /* this fake device always acks its configured address */
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t i2c_dev, const uint8_t *write_buffer, size_t write_size,
                               int xfer_timeout_ms)
{
    (void)i2c_dev;
    (void)xfer_timeout_ms;
    if (s_fake.fail_writes) {
        return ESP_FAIL;
    }
    size_t n = write_size < MAX_RESPONSE_LEN ? write_size : MAX_RESPONSE_LEN;
    memcpy(s_fake.last_transmit, write_buffer, n);
    s_fake.last_transmit_len = n;
    return ESP_OK;
}

esp_err_t i2c_master_receive(i2c_master_dev_handle_t i2c_dev, uint8_t *read_buffer, size_t read_size,
                              int xfer_timeout_ms)
{
    (void)i2c_dev;
    (void)xfer_timeout_ms;
    s_fake.receive_call_count++;
    if (s_fake.queued_count == 0) {
        return ESP_FAIL; /* unresponsive/disconnected device -- nothing queued */
    }
    fake_response_t *next = &s_fake.responses[s_fake.queue_head];
    if (next->len != read_size) {
        return ESP_FAIL; /* a real device would never answer with the wrong length either */
    }
    memcpy(read_buffer, next->data, next->len);
    s_fake.queue_head = (s_fake.queue_head + 1) % MAX_QUEUED_RESPONSES;
    s_fake.queued_count--;
    return ESP_OK;
}
