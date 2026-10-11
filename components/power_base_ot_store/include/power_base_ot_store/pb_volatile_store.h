/*
 * A one-way switch in front of the two places where OpenThread keeps what it
 * knows about its network across a reboot (docs/power-base-management.md,
 * section 4, "영수증 전달"):
 *
 *   - its settings: otPlatSettingsGet/Set/Add/Delete/Wipe -- on ESP-IDF the
 *     NVS namespace "openthread" (the Dataset without its keys, the network
 *     info, the parent, ...);
 *   - PSA Internal Trusted Storage: psa_its_get_info/get/set/remove -- on
 *     ESP-IDF v6.1 the NVS namespace "psa_its". OpenThread there is built with
 *     key references, so the Network Key and the PSKc are persistent PSA keys
 *     and live HERE, not in the settings.
 *
 * Before the switch every call goes to the platform's own function, unchanged.
 * After it, none does: reads and writes are served from two small tables in
 * RAM that start empty and die with the next reset. There is no way back but a
 * reset.
 *
 * What it is for: a Power Base that has erased its stored network and checked
 * that (pb_mgmt_client.h) can still tell its hub so over Thread -- with the
 * Dataset it kept in RAM -- without that attach storing anything again.
 * Attaching always stores: the parent's Child ID Response makes OpenThread save
 * the Active Dataset, the Network Key is imported as a persistent key, the
 * network info is written. With the switch on, all of that lands in RAM.
 *
 * How it gets in front: the firmware is linked with -Wl,--wrap=<function> for
 * the nine functions (power_base/main/CMakeLists.txt). Every reference to one
 * of them from another object file then resolves to __wrap_<function> in this
 * component, and __real_<function> is the platform's. A build without those
 * options would leave the switch without effect, which is why
 * pb_volatile_store_engage() checks, with two reads that change nothing, that
 * calls made under the public names do arrive here.
 * tools/check_power_base_ot_store.py checks the same in the linked image.
 *
 * Nothing here logs or returns a stored value.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Room for what one short attach writes: a Dataset is at most 254 bytes, the
 * rest is a few dozen bytes each. A write that does not fit is refused
 * (OT_ERROR_NO_BUFS / PSA_ERROR_INSUFFICIENT_STORAGE), never passed on. */
#define PB_VOLATILE_STORE_SETTINGS_BYTES 2048u
#define PB_VOLATILE_STORE_ITS_BYTES 1024u

typedef struct {
    /* Calls that arrived here under the public names, before or after the
     * switch. */
    uint32_t settings_calls;
    uint32_t its_calls;
    /* After the switch: writes kept in RAM, and writes refused for room. */
    uint32_t settings_kept;
    uint32_t settings_refused;
    uint32_t its_kept;
    uint32_t its_refused;
} pb_volatile_store_stats_t;

/*
 * Turns the switch and checks that it is in the path. True only when a read
 * of OpenThread's settings and a read of PSA ITS, both made under their public
 * names, were seen by this component. False = this image was not linked with
 * the wrap options: the switch is on but protects nothing, and the caller must
 * not enable Thread. `ot_instance` is passed through to otPlatSettingsGet().
 * Safe to call again.
 */
bool pb_volatile_store_engage(void *ot_instance);

bool pb_volatile_store_is_engaged(void);
void pb_volatile_store_get_stats(pb_volatile_store_stats_t *out);

/*
 * One PSA storage id can be watched: how often, since the watch was (re)started,
 * a psa_its_set() and a psa_its_remove() of that id were made under the public
 * names, passed on to the platform, and carried out there. (A call the
 * platform refused, and a call answered from RAM after the switch, changed
 * nothing in flash and is not counted.)
 *
 * Every write of key storage in the image comes through here (that is what
 * the wrap options are for), so this is where the origin of a stored entry can
 * be known: pb_stored_network.c starts the watch on the Network Key's id each
 * time it has read both stores back empty, and so knows whether the entry it
 * finds there later is the one it then saw being stored.
 *
 * While exactly one write and no remove has been seen, the bytes of that one
 * write are kept, so that "the entry in flash is that write" can be checked
 * and not only counted. A second write or a remove wipes them: what is kept
 * is only ever the first entry stored on stores that had just been read back
 * empty -- the key OpenThread makes for itself at a start, never a network's.
 * They do not leave this component (the comparison is made here), and nothing
 * logs them. RAM only; a reset forgets all of it.
 */
#define PB_VOLATILE_STORE_WATCH_BYTES 128u

typedef struct {
    bool watching;
    uint32_t stored;
    uint32_t removed;
} pb_volatile_store_its_watch_t;

void pb_volatile_store_its_watch_start(uint64_t uid);
void pb_volatile_store_its_watch_get(pb_volatile_store_its_watch_t *out);

/* True only when, since the watch started, exactly one write and no remove of
 * the watched id went through, that write was at most
 * PB_VOLATILE_STORE_WATCH_BYTES long, and `data` (`length` bytes) is byte for
 * byte what it stored. */
bool pb_volatile_store_its_watch_matches(const void *data, size_t length);

#ifdef __cplusplus
}
#endif
