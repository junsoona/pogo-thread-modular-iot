/* Implementation of the fake in-memory NVS store declared across nvs.h/
 * nvs_flash.h/nvs_sim.h in this directory. See nvs.h's top comment. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
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
    uint16_t u16_value;
    bool   is_u16;
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
static esp_err_t s_forced_commit_error = ESP_OK;
static char s_backing_file[512];

typedef struct {
    int nth; /* 0 = no fault pending */
    esp_err_t err;
    bool any_key;
    char key[SIM_MAX_NAME_LEN];
} sim_fault_t;

static sim_fault_t s_faults[NVS_SIM_OP_COUNT];
static int s_power_cut_commit;
static int s_commit_count;

void nvs_sim_inject(nvs_sim_op_t op, const char *key, int nth, esp_err_t err)
{
    sim_fault_t *f = &s_faults[op];
    memset(f, 0, sizeof(*f));
    f->nth = nth;
    f->err = err;
    f->any_key = (key == NULL);
    if (key != NULL) {
        strncpy(f->key, key, SIM_MAX_NAME_LEN - 1);
    }
}

void nvs_sim_power_cut_at_commit(int nth)
{
    s_power_cut_commit = nth;
}

int nvs_sim_commit_count(void)
{
    return s_commit_count;
}

/* True when this call is the one a pending fault was waiting for; *err then
 * says how it goes wrong. */
static bool fault_hits(nvs_sim_op_t op, const char *key, esp_err_t *err)
{
    sim_fault_t *f = &s_faults[op];
    if (f->nth == 0 || (!f->any_key && (key == NULL || strcmp(f->key, key) != 0))) {
        return false;
    }
    if (--f->nth != 0) {
        return false;
    }
    *err = f->err;
    return true;
}

void nvs_sim_reset(void)
{
    memset(s_namespaces, 0, sizeof(s_namespaces));
    s_forced_init_error = ESP_OK;
    s_forced_commit_error = ESP_OK;
    s_backing_file[0] = '\0';
    memset(s_faults, 0, sizeof(s_faults));
    s_power_cut_commit = 0;
    s_commit_count = 0;
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

void nvs_sim_put_u16(const char *partition, const char *ns, const char *key, uint16_t value)
{
    sim_namespace_t *n = find_ns(partition, ns, true);
    sim_entry_t *e = find_or_create_entry(n, key);
    e->is_blob = false;
    e->is_u16 = true;
    e->u16_value = value;
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

void nvs_sim_force_next_commit_error(esp_err_t err)
{
    s_forced_commit_error = err;
}

int nvs_sim_peek_blob(const char *partition, const char *ns, const char *key, void *out, size_t cap)
{
    for (int i = 0; i < SIM_MAX_NAMESPACES; i++) {
        sim_namespace_t *n = &s_namespaces[i];
        if (!n->used || strcmp(n->partition, partition) != 0 || strcmp(n->ns_name, ns) != 0) {
            continue;
        }
        for (int k = 0; k < SIM_MAX_ENTRIES_PER_NS; k++) {
            sim_entry_t *e = &n->entries[k];
            if (e->used && e->is_blob && strcmp(e->key, key) == 0) {
                if (out != NULL && cap >= e->blob_len) {
                    memcpy(out, e->blob, e->blob_len);
                }
                return (int)e->blob_len;
            }
        }
    }
    return -1;
}

bool nvs_sim_set_backing_file(const char *path)
{
    s_commit_count = 0;
    memset(s_namespaces, 0, sizeof(s_namespaces));
    strncpy(s_backing_file, path, sizeof(s_backing_file) - 1);
    s_backing_file[sizeof(s_backing_file) - 1] = '\0';
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return true; /* never committed yet -- blank flash */
    }
    bool ok = fread(s_namespaces, sizeof(s_namespaces), 1, f) == 1;
    fclose(f);
    if (!ok) {
        memset(s_namespaces, 0, sizeof(s_namespaces));
    }
    return ok;
}

/* Rewrites the backing file via temp + rename so a test process killed
 * mid-write can only ever leave the old or the new image behind. */
static bool persist_backing_file(void)
{
    if (s_backing_file[0] == '\0') {
        return true;
    }
    char tmp[sizeof(s_backing_file) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s_backing_file);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        return false;
    }
    bool ok = fwrite(s_namespaces, sizeof(s_namespaces), 1, f) == 1;
    ok = (fclose(f) == 0) && ok;
    return ok && rename(tmp, s_backing_file) == 0;
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
    if (e == NULL || e->is_blob || e->is_u16) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out_value = e->u8_value;
    return ESP_OK;
}

esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *out_value)
{
    sim_entry_t *e = lookup_entry(handle, key);
    if (e == NULL || e->is_blob || !e->is_u16) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out_value = e->u16_value;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length)
{
    esp_err_t fault = ESP_OK;
    if (fault_hits(NVS_SIM_OP_GET_BLOB, key, &fault) && fault != ESP_OK) {
        return fault;
    }
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

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle)
{
    sim_namespace_t *n = find_ns("nvs", namespace_name, open_mode == NVS_READWRITE);
    if (n == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out_handle = (nvs_handle_t)(n - s_namespaces) + 1;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    if (handle == 0 || handle > SIM_MAX_NAMESPACES || !s_namespaces[handle - 1].used) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (length > SIM_MAX_BLOB_LEN) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    esp_err_t fault = ESP_OK;
    if (fault_hits(NVS_SIM_OP_SET_BLOB, key, &fault)) {
        return fault; /* an error, or ESP_OK with nothing written */
    }
    sim_entry_t *e = find_or_create_entry(&s_namespaces[handle - 1], key);
    if (e == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    e->is_blob = true;
    e->is_u16 = false;
    e->blob_len = length;
    if (length > 0) {
        memcpy(e->blob, value, length);
    }
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    esp_err_t fault = ESP_OK;
    if (fault_hits(NVS_SIM_OP_ERASE_KEY, key, &fault)) {
        return fault; /* an error, or ESP_OK with nothing erased */
    }
    sim_entry_t *e = lookup_entry(handle, key);
    if (e == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    memset(e, 0, sizeof(*e));
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    if (handle == 0 || handle > SIM_MAX_NAMESPACES || !s_namespaces[handle - 1].used) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    esp_err_t fault = ESP_OK;
    if (fault_hits(NVS_SIM_OP_ERASE_ALL, NULL, &fault)) {
        return fault; /* an error, or ESP_OK with nothing erased */
    }
    memset(s_namespaces[handle - 1].entries, 0, sizeof(s_namespaces[handle - 1].entries));
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    esp_err_t err = s_forced_commit_error;
    s_forced_commit_error = ESP_OK; /* one-shot */
    if (err != ESP_OK) {
        return err;
    }
    esp_err_t fault = ESP_OK;
    if (fault_hits(NVS_SIM_OP_COMMIT, NULL, &fault) && fault != ESP_OK) {
        return fault;
    }
    if (s_power_cut_commit != 0 && --s_power_cut_commit == 0) {
        _exit(NVS_SIM_POWER_CUT_EXIT); /* the power went: this commit never reached the flash */
    }
    if (!persist_backing_file()) {
        return ESP_FAIL;
    }
    s_commit_count++;
    return ESP_OK;
}

/* See nvs.h: only "does this namespace hold anything". */
struct nvs_sim_iterator {
    int unused;
};

esp_err_t nvs_entry_find(const char *part_name, const char *namespace_name, nvs_type_t type,
                         nvs_iterator_t *output_iterator)
{
    static struct nvs_sim_iterator s_iterator;
    (void)type;
    *output_iterator = NULL;
    esp_err_t fault;
    if (fault_hits(NVS_SIM_OP_ENTRY_FIND, NULL, &fault) && fault != ESP_OK) {
        return fault;
    }
    sim_namespace_t *n = find_ns(part_name, namespace_name, false);
    if (n != NULL) {
        for (int i = 0; i < SIM_MAX_ENTRIES_PER_NS; i++) {
            if (n->entries[i].used) {
                *output_iterator = &s_iterator;
                return ESP_OK;
            }
        }
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

void nvs_release_iterator(nvs_iterator_t iterator)
{
    (void)iterator;
}
