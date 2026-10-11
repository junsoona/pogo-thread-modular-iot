/*
 * Hints for one "locate" round (docs/power-base-management.md, section 12;
 * components/power_base_mgmt/pb_mgmt_client.h): where in this board's own
 * Thread network the hub's host may be reachable now.
 *
 * This file calls OpenThread's public API and nothing else of the platform,
 * so that the very same code runs in the firmware (enrollment_mgmt.c, under
 * the OpenThread API lock) and against the real OpenThread core on a host
 * (tools/ot_host_repro/pb_locate_nexus.cpp).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "openthread/instance.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PB_LOCATE_HINT_ADDR_LEN 16

/*
 * Writes EVERY hint there is, most likely first, and returns how many -- up
 * to `cap`, which must be PB_MGMT_LOCATE_MAX_HINTS (128) for "every" to
 * hold: a Thread network has at most 32 routers, and its Network Data (254
 * bytes) can name at most 82 further nodes, so nothing is cut off there. The
 * management client walks through this list over several rounds
 * (pb_mgmt_client.h), so the order is the same from call to call while the
 * network is: Network Data order, the leader, then router ids upwards.
 * Reads the instance's own Network Data and router table; sends nothing and
 * changes nothing. 0 when the instance is not attached. The caller holds
 * whatever lock the platform needs for OpenThread API calls.
 *
 *   1. every node the Thread Network Data names as a border router (an
 *      on-mesh prefix or an external route) or as a server (a service). A
 *      Linux OTBR that does border routing is in there, and so is one that
 *      runs an SRP server;
 *   2. the leader, then every router in the router table. The hub's OTBR
 *      formed the network this board joined, so it is normally one of them
 *      even with nothing in the Network Data.
 *
 * What comes out is each node's routing locator
 * (<Mesh-Local Prefix>:0000:00ff:fe00:<RLOC16>), never this board's own.
 * A locator is somewhere to ASK: whoever sits behind it has to answer with
 * the hub's key, and the locator itself is never stored. An OTBR that is
 * neither in the Network Data nor a router (border routing off and attached
 * as a child) is not found this way.
 */
size_t pb_locate_collect_hints(otInstance *instance, uint8_t (*out)[PB_LOCATE_HINT_ADDR_LEN], size_t cap);

#ifdef __cplusplus
}
#endif
