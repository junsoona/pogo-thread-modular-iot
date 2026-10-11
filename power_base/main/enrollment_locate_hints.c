#include "enrollment_locate_hints.h"

#include <stdbool.h>
#include <string.h>
#include "openthread/ip6.h"
#include "openthread/netdata.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"

#include "power_base_mgmt/pb_mgmt_client.h"

_Static_assert(PB_LOCATE_HINT_ADDR_LEN == PB_ENROLL_BRIDGE_ADDR_LEN, "a hint is an IPv6 address");

typedef struct {
    const uint8_t *prefix;
    uint16_t own_rloc16;
    uint8_t (*out)[PB_LOCATE_HINT_ADDR_LEN];
    size_t cap;
    size_t count;
} hint_list_t;

static void hint_add(hint_list_t *list, uint16_t rloc16)
{
    uint8_t addr[PB_LOCATE_HINT_ADDR_LEN];
    if (list->count >= list->cap || rloc16 == list->own_rloc16 ||
        !pb_mgmt_locator_address(list->prefix, rloc16, addr)) {
        return;
    }
    for (size_t i = 0; i < list->count; i++) {
        if (memcmp(list->out[i], addr, sizeof(addr)) == 0) {
            return;
        }
    }
    memcpy(list->out[list->count++], addr, sizeof(addr));
}

size_t pb_locate_collect_hints(otInstance *instance, uint8_t (*out)[PB_LOCATE_HINT_ADDR_LEN], size_t cap)
{
    hint_list_t list = {.out = out, .cap = cap, .count = 0};
    const otDeviceRole role = otThreadGetDeviceRole(instance);
    const otMeshLocalPrefix *prefix = otThreadGetMeshLocalPrefix(instance);
    if (prefix == NULL ||
        (role != OT_DEVICE_ROLE_CHILD && role != OT_DEVICE_ROLE_ROUTER && role != OT_DEVICE_ROLE_LEADER)) {
        return 0;
    }
    list.prefix = prefix->m8;
    list.own_rloc16 = otThreadGetRloc16(instance);

    otNetworkDataIterator it = OT_NETWORK_DATA_ITERATOR_INIT;
    otBorderRouterConfig border_router;
    while (otNetDataGetNextOnMeshPrefix(instance, &it, &border_router) == OT_ERROR_NONE) {
        hint_add(&list, border_router.mRloc16);
    }
    it = OT_NETWORK_DATA_ITERATOR_INIT;
    otExternalRouteConfig route;
    while (otNetDataGetNextRoute(instance, &it, &route) == OT_ERROR_NONE) {
        hint_add(&list, route.mRloc16);
    }
    it = OT_NETWORK_DATA_ITERATOR_INIT;
    otServiceConfig service;
    while (otNetDataGetNextService(instance, &it, &service) == OT_ERROR_NONE) {
        hint_add(&list, service.mServerConfig.mRloc16);
    }

    otIp6Address leader;
    if (otThreadGetLeaderRloc(instance, &leader) == OT_ERROR_NONE) {
        hint_add(&list, (uint16_t)(((uint16_t)leader.mFields.m8[14] << 8) | leader.mFields.m8[15]));
    }
    const uint8_t max_router_id = otThreadGetMaxRouterId(instance);
    for (uint16_t id = 0; id <= max_router_id && list.count < list.cap; id++) {
        otRouterInfo router;
        if (otThreadGetRouterInfo(instance, id, &router) == OT_ERROR_NONE && router.mAllocated) {
            hint_add(&list, router.mRloc16);
        }
    }
    return list.count;
}
