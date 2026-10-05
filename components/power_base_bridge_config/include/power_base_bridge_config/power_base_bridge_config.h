/*
 * Power-base -> Linux OTBR northbound bridge config (this follow-up: "Power
 * Base의 aggregator 데이터를 Linux OTBR 호스트로 전달하는 northbound bridge
 * transport"). A SMALL, separate component from components/power_base_config
 * (fixed board/electrical config) and components/power_base_thread_config
 * (Thread Dataset provenance) -- same reasoning as that component's own
 * header comment: this owns exactly one question, whether
 * components/linux_bridge_exporter should run at all this boot and, if so,
 * where it sends to.
 *
 * Loaded from the SAME "cfg" NVS partition (see power_base/partitions.csv),
 * its OWN namespace ("pb_bridge") -- does NOT change power_base_config's own
 * 44-byte blob/schema_version, nor power_base_thread_config's "pb_thread"
 * namespace, at all.
 *
 *   namespace: pb_bridge
 *     key "blob"  blob: POWER_BASE_BRIDGE_CONFIG_BLOB_LEN bytes, see the wire
 *                  layout in power_base_bridge_config.c
 *     key "crc"   u16:  CRC16/CCITT-FALSE of "blob" (components/pogo_proto's
 *                  pogo_crc16_ccitt(), same integrity scheme
 *                  power_base_config already uses for its own blob)
 *
 * Compatibility policy (mirrors power_base_thread_config_load()'s own
 * "legacy image" handling):
 *   - no "pb_bridge" namespace at all: *out = {enabled=false, ...},
 *     returns ESP_OK -- exporter disabled, every pre-existing
 *     power_base_config.bin keeps working completely unchanged.
 *   - namespace present, "blob" decodes with enabled=false: *out =
 *     {enabled=false, ...}, ESP_OK -- host_addr/port are NOT validated in
 *     this case (they may be garbage/unset; the exporter never reads them
 *     while enabled=false).
 *   - namespace present, enabled=true: host_addr/port ARE validated (see
 *     below) -- any violation FAILS CLOSED (a non-ESP_OK return, *out
 *     zeroed), same "no silent fallback" policy every other CONFIG_ERROR in
 *     this codebase uses. The caller (power_base/main/app_main.c) MUST NOT
 *     start linux_bridge_exporter in that case, and per this follow-up's own
 *     scope, must NOT treat it as a reason to halt the whole boot either --
 *     see this header's "boot policy" note below.
 *   - any other malformed/missing/CRC-mismatched/wrong-schema "blob" (with
 *     the namespace present): FAILS CLOSED the same way.
 *
 * Boot policy (this follow-up's own explicit requirement -- "Linux 호스트가
 * 런타임에 응답하지 않는 것은 Power Base 부팅 실패로 처리하지 않음", and by
 * extension a STORAGE-level pb_bridge problem must not be allowed to newly
 * brick an otherwise-healthy power base either): a non-ESP_OK return from
 * this loader means "the bridge exporter must not start this boot" -- it
 * does NOT mean "halt the whole power base". power_base/main/app_main.c logs
 * the CONFIG_ERROR this function already produced and simply continues
 * booting with the exporter disabled; every other subsystem (Thread,
 * power_base_transport, parent_service, the POGO port) is completely
 * unaffected either way. This is a deliberate difference from
 * power_base_config_load()/power_base_thread_config_load(), whose failures
 * DO halt the whole boot -- those loaders gate primary functions (POGO
 * pairing, Thread network membership) this optional, best-effort northbound
 * mirror never does.
 *
 * host_addr/port validation, only performed when enabled=true (see above):
 *   - host_addr must not be the unspecified address (::, all-zero)
 *   - host_addr must not be a multicast address (first byte 0xFF)
 *   - host_addr must not be a link-local unicast address (fe80::/10, i.e.
 *     first byte 0xFE and the top two bits of the second byte are 10) --
 *     the Linux OTBR host's real Mesh-Local/backbone address is what
 *     `sudo ot-ctl ipaddr mleid` on that host prints, never a link-local
 *     scope-ambiguous address (this component has no interface-index
 *     concept to disambiguate one)
 *   - port must not be 0
 * A violation of any of the above returns ESP_ERR_INVALID_ARG (CONFIG_ERROR
 * already logged with mode/reason, never the address bytes themselves --
 * same secrecy-adjacent policy as this codebase's Dataset handling, even
 * though a bridge host address is not itself a secret the way a Network Key
 * is; it is simply never useful to print bytes a human is expected to
 * recognize from `ot-ctl ipaddr mleid` output on their own screen).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POWER_BASE_BRIDGE_CONFIG_NVS_PARTITION  "cfg"
#define POWER_BASE_BRIDGE_CONFIG_NAMESPACE      "pb_bridge"
#define POWER_BASE_BRIDGE_CONFIG_SCHEMA_VERSION 1u
#define POWER_BASE_BRIDGE_CONFIG_ADDR_LEN 16u

/*
 * Enrollment endpoint policy: byte [5] of the "blob" (the byte that used to
 * be "reserved, 0"), so it is under the same CRC as `enabled`, the address
 * and the port, and cannot go missing on its own.
 *
 *   0  no policy. Every image made before this field existed, and every
 *      image with a static address: enrollment works as it always did -- a
 *      Joiner request needs no bridge endpoint, and the exporter uses what
 *      this blob says (possibly "disabled").
 *   1  ENDPOINT REQUIRED. The image deliberately carries no address
 *      (tools/gen_power_base_ble_config.py --bridge-from-enrollment): this
 *      board must be given the bridge endpoint WITH its enrollment. An
 *      enrollment request without one is refused before anything is started
 *      or erased, and a stored enrollment record without one is not resumed.
 *      Only valid together with enabled=0.
 *   anything else, or 1 together with enabled=1: not a policy this firmware
 *      knows -- see POWER_BASE_BRIDGE_ENROLL_POLICY_INVALID.
 *
 * "pb_bridge enabled=0" alone never means "endpoint required": an image may
 * switch the exporter off on purpose, and such a board keeps enrolling the
 * old way. The policy is only what this byte says.
 *
 * power_base_bridge_config_load() does not look at this byte (as before), so
 * nothing changes for pb_thread modes 0 and 1, which never enroll.
 */
#define POWER_BASE_BRIDGE_CONFIG_ENROLL_POLICY_OFFSET 5u
#define POWER_BASE_BRIDGE_CONFIG_ENROLL_POLICY_NONE 0u
#define POWER_BASE_BRIDGE_CONFIG_ENROLL_POLICY_ENDPOINT_REQUIRED 1u

typedef enum {
    /* No "pb_bridge" namespace at all, or an intact blob whose policy byte
     * is 0: the pre-existing behaviour. */
    POWER_BASE_BRIDGE_ENROLL_POLICY_NONE = 0,
    /* Intact blob, enabled=0, policy byte 1. */
    POWER_BASE_BRIDGE_ENROLL_POLICY_ENDPOINT_REQUIRED,
    /* The policy cannot be trusted: the cfg partition or the namespace
     * cannot be read, "blob"/"crc" are missing, the blob has another size or
     * schema version, its CRC does not match, the policy byte is neither 0
     * nor 1, or it is 1 while enabled=1. The caller must not start any NEW
     * enrollment on such a board. */
    POWER_BASE_BRIDGE_ENROLL_POLICY_INVALID,
} power_base_bridge_enroll_policy_t;

typedef struct {
    bool     enabled;
    uint8_t  host_addr[POWER_BASE_BRIDGE_CONFIG_ADDR_LEN]; /* IPv6, only meaningful when enabled */
    uint16_t port;                                          /* only meaningful when enabled */
} power_base_bridge_config_t;

/*
 * Loads and validates the bridge-export config from the "cfg" NVS partition.
 * *out is memset to 0 (enabled=false) on every path before anything else, so
 * a caller that ignores the return code still starts from an unambiguous
 * "disabled" struct rather than stale/partial data. See this header's top
 * comment for the exact compatibility/validation/boot-policy rules.
 */
esp_err_t power_base_bridge_config_load(power_base_bridge_config_t *out);

/*
 * pb_thread:mode=2 only (power_base/main/app_main.c calls it nowhere else):
 * what the prepared image says about enrolling this board without a bridge
 * endpoint. Reads the same "pb_bridge" blob with the same integrity checks
 * as power_base_bridge_config_load(); never writes; never logs the address.
 */
power_base_bridge_enroll_policy_t power_base_bridge_config_load_enroll_policy(void);
const char *power_base_bridge_enroll_policy_name(power_base_bridge_enroll_policy_t policy);

/*
 * pb_thread:mode=2 only: replaces *cfg -- whatever the "cfg" partition's
 * pb_bridge said, including "disabled" and "absent" -- with the Linux bridge
 * endpoint that was delivered with this board's enrollment and stored in its
 * version 2 enrollment record (components/power_base_enrollment). The caller
 * (power_base/main/app_main.c's on_enrollment_ready()) calls this BEFORE the
 * services start, so linux_bridge_exporter_init() -- which copies the address
 * once -- is handed the delivered endpoint, this boot and after every
 * reboot that restores the record.
 *
 * The cfg partition is not written: it stays a read-only prepared image.
 *
 * Failure policy, deliberately different from a bad cfg/pb_bridge (which
 * only ever means "exporter off, everything else as usual"): if the
 * delivered endpoint does not pass the same checks the loader applies
 * (unspecified, multicast, link-local, port 0), *cfg becomes DISABLED and
 * this returns ESP_ERR_INVALID_ARG. It never keeps or restores the address
 * the prepared image had: a board enrolled with a delivered endpoint is
 * never silently pointed at another host. The board itself keeps running;
 * only the optional exporter stays off.
 *
 * A board whose enrollment delivered no endpoint (version 1 record, bench
 * console, version 1 BLE start) and every mode 0/1 board never calls this;
 * for them *cfg is exactly what power_base_bridge_config_load() returned.
 */
esp_err_t power_base_bridge_config_apply_delivered(power_base_bridge_config_t *cfg,
                                                   const uint8_t host_addr[POWER_BASE_BRIDGE_CONFIG_ADDR_LEN],
                                                   uint16_t port);

#ifdef __cplusplus
}
#endif
