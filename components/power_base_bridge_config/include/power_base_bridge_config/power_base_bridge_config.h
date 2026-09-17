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

#ifdef __cplusplus
}
#endif
