/*
 * Host-only stand-in for TF-PSA-Crypto's core/psa_crypto_its.h (the PSA
 * Internal Trusted Storage interface), for the host tests of
 * components/power_base_ot_store that do not build PSA itself. Types, flag and
 * status values are those of ESP-IDF v6.1's copy
 * (components/mbedtls/mbedtls/tf-psa-crypto/core/psa_crypto_its.h and
 * include/psa/crypto_values.h); tools/ot_host_repro/pb_volatile_store_psa.c
 * compiles the same component against the real header.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t psa_status_t;
typedef uint32_t psa_storage_create_flags_t;
typedef uint64_t psa_storage_uid_t;

#define PSA_SUCCESS ((psa_status_t)0)
#define PSA_ERROR_NOT_PERMITTED ((psa_status_t)-133)
#define PSA_ERROR_INVALID_ARGUMENT ((psa_status_t)-135)
#define PSA_ERROR_INVALID_HANDLE ((psa_status_t)-136)
#define PSA_ERROR_DOES_NOT_EXIST ((psa_status_t)-140)
#define PSA_ERROR_INSUFFICIENT_STORAGE ((psa_status_t)-142)

#define PSA_STORAGE_FLAG_NONE 0
#define PSA_STORAGE_FLAG_WRITE_ONCE (1 << 0)

struct psa_storage_info_t {
    uint32_t size;
    psa_storage_create_flags_t flags;
};

psa_status_t psa_its_set(psa_storage_uid_t uid, uint32_t data_length, const void *p_data,
                         psa_storage_create_flags_t create_flags);
psa_status_t psa_its_get(psa_storage_uid_t uid, uint32_t data_offset, uint32_t data_length, void *p_data,
                         size_t *p_data_length);
psa_status_t psa_its_get_info(psa_storage_uid_t uid, struct psa_storage_info_t *p_info);
psa_status_t psa_its_remove(psa_storage_uid_t uid);

#ifdef __cplusplus
}
#endif
