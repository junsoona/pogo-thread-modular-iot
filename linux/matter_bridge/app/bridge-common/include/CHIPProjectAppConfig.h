/*
 * Project-specific overrides to connectedhomeip's default configuration --
 * see config/standalone/CHIPProjectConfig.h (included below) for every
 * value this doesn't touch. Mirrors examples/bridge-app/bridge-common's own
 * CHIPProjectAppConfig.h in structure; the one value actually overridden
 * here is sized for THIS application, not copied blindly.
 */
#pragma once

// Overrides CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT (default: 0, see
// <platform/CHIPDeviceConfig.h>). One dynamic Matter endpoint is created per
// module_uuid (see linux/matter_bridge/app/chip/chip_endpoint.hpp -- this
// bridge does not use child endpoints, see that file's own top comment for
// why), so this must be at least ingest::Registry::kMaxNodes (64, see
// linux/matter_bridge/ingest/include/ingest/registry.hpp) -- matches
// matter_bridge::EndpointStore::kMaxEndpoints exactly, so neither side of
// this application can ever be surprised by the other's capacity.
#define CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT 64

#include <CHIPProjectConfig.h>
