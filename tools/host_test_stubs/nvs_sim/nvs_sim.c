/* Implementation of the fake in-memory NVS store declared across nvs.h/
 * nvs_flash.h/nvs_sim.h in this directory. See nvs.h's top comment. */
#include <string.h>
#include <stdbool.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "nvs_sim.h"

#define SIM_MAX_NAMESPACES 8
#define SIM_MAX_ENTRIES_PER_NS 8
#define SIM_MAX_NAME_LEN 32
#define SIM_MAX_BLOB_LEN 512

typedef struct {
    char   key[SIM_MAX_NAME_LEN];
    bool   used;
    bool   is_blob;
    uint8_t u8_value;
    uint8_t blob[SIM_MAX_BLOB_LEN];
    size_t blob_len;
} sim_entry_t;

typedef struct {
    char   partition[SIM_MAX_NAME_LEN];
    char   ns_name[SIM_MAX_NAME_LEN];
    bool   used;
    sim_entry_t entries[SIM_MAX_ENTRIES_PER_NS];
} sim_namespace_t;

static sim_namespace_t s_namespaces[SIM_MAX_NAMESPACES];
static esp_err_t s_forced_init_error = ESP_OK;

void nvs_sim_reset(void)
{
    memset(s_namespaces, 0, sizeof(s_namespaces));
    s_forced_init_error = ESP_OK;
}

static sim_namespace_t *find_ns(const char *partition, const char *ns, bool create)
{
    for (int i = 0; i < SIM_MAX_NAMESPACES; i++) {
        if (s_namespaces[i].used && strcmp(s_namespaces[i].partition, partition) == 0 &&
            strcmp(s_namespaces[i].ns_name, ns) == 0) {
            return &s_namespaces[i];
        }
    }
    if (!create) {
        return NULL;
    }
    for (int i = 0; i < SIM_MAX_NAMESPACES; i++) {
        if (!s_namespaces[i].used) {
            memset(&s_namespaces[i], 0, sizeof(s_namespaces[i]));
            s_namespaces[i].used = true;
            strncpy(s_namespaces[i].partition, partition, SIM_MAX_NAME_LEN - 1);
            strncpy(s_namespaces[i].ns_name, ns, SIM_MAX_NAME_LEN - 1);
            return &s_namespaces[i];
        }
    }
    return NULL; /* fixed-capacity fake store exhausted -- a test bug, not a real-world case */
}

static sim_entry_t *find_or_create_entry(sim_namespace_t *n, const char *key)
{
    for (int i = 0; i < SIM_MAX_ENTRIES_PER_NS; i++) {
        if (n->entries[i].used && strcmp(n->entries[i].key, key) == 0) {
            return &n->entries[i];
        }
    }
    for (int i = 0; i < SIM_MAX_ENTRIES_PER_NS; i++) {
        if (!n->entries[i].used) {
            memset(&n->entries[i], 0, sizeof(n->entries[i]));
            n->entries[i].used = true;
            strncpy(n->entries[i].key, key, SIM_MAX_NAME_LEN - 1);
            return &n->entries[i];
        }
    }
    return NULL;
}

void nvs_sim_create_namespace(const char *partition, const char *ns)
{
    find_ns(partition, ns, true);
}

void nvs_sim_put_u8(const char *partition, const char *ns, const char *key, uint8_t value)
{
    sim_namespace_t *n = find_ns(partition, ns, true);
    sim_entry_t *e = find_or_create_entry(n, key);
    e->is_blob = false;
    e->u8_value = value;
}

void nvs_sim_put_blob(const char *partition, const char *ns, const char *key, const void *data, size_t len)
{
    sim_namespace_t *n = find_ns(partition, ns, true);
    sim_entry_t *e = find_or_create_entry(n, key);
    e->is_blob = true;
    e->blob_len = len;
    if (len > 0) {
        memcpy(e->blob, data, len);
    }
}

void nvs_sim_force_next_init_error(esp_err_t err)
{
    s_forced_init_error = err;
}

esp_err_t nvs_flash_init_partition(const char *partition_name)
{
    (void)partition_name;
    esp_err_t err = s_forced_init_error;
    s_forced_init_error = ESP_OK; /* one-shot */
    return err;
}

esp_err_t nvs_open_from_partition(const char *partition_name, const char *namespace_name, nvs_open_mode_t open_mode,
                                   nvs_handle_t *out_handle)
{
    (void)open_mode;
    sim_namespace_t *n = find_ns(partition_name, namespace_name, false);
    if (n == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out_handle = (nvs_handle_t)(n - s_namespaces) + 1; /* 0 reserved as "invalid" */
    return ESP_OK;
}

static sim_entry_t *lookup_entry(nvs_handle_t handle, const char *key)
{
    if (handle == 0 || handle > SIM_MAX_NAMESPACES) {
        return NULL;
    }
    sim_namespace_t *n = &s_namespaces[handle - 1];
    if (!n->used) {
        return NULL;
    }
    for (int i = 0; i < SIM_MAX_ENTRIES_PER_NS; i++) {
        if (n->entries[i].used && strcmp(n->entries[i].key, key) == 0) {
            return &n->entries[i];
        }
    }
    return NULL;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value)
{
    sim_entry_t *e = lookup_entry(handle, key);
    if (e == NULL || e->is_blob) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out_value = e->u8_value;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length)
{
    sim_entry_t *e = lookup_entry(handle, key);
    if (e == NULL || !e->is_blob) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (out_value == NULL) {
        /* Real ESP-IDF nvs_get_blob() size-query form. */
        *length = e->blob_len;
        return ESP_OK;
    }
    if (*length < e->blob_len) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, e->blob, e->blob_len);
    *length = e->blob_len;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}
