// The ONLY file in this application that translates a
// matter_bridge::ModuleUpdate (see
// linux/matter_bridge/app/core/include/matter_bridge/bridge_manager.hpp)
// into real connectedhomeip v1.6.0.0 Matter dynamic endpoints and cluster
// attribute updates. Every call here MUST happen on the Matter event-loop
// thread (see main.cpp's own top comment) -- this class does no locking of
// its own because it is never touched from any other thread.
//
// Endpoint structure (see docs/matter-bridge.md for the full investigation
// this is based on):
//   - Endpoint 0: root (fixed, from bridge-common/pogo-matter-bridge.zap).
//   - Endpoint 1: Aggregator (fixed, MA-aggregator) -- the parent of every
//     endpoint this class creates.
//   - Endpoint 2: a disabled-at-runtime placeholder (see
//     bridge-common/BUILD.gn's own top comment) -- never touched here.
//   - One dynamic endpoint PER module_uuid, parented to the Aggregator.
//     NO child endpoints: v1.6.0.0's actual dynamic-endpoint constraint is
//     "at most one cluster INSTANCE per cluster ID per endpoint" (this is
//     why the SDK's own composed-device bridge example needs two child
//     endpoints for two independent TemperatureMeasurement instances), not
//     "one endpoint per measurement". Every channel this bridge supports
//     maps to a DIFFERENT cluster ID (TemperatureMeasurement 0x0402,
//     RelativeHumidityMeasurement 0x0405, Pm25/Pm1/Pm10ConcentrationMeasurement
//     0x042A/0x042C/0x042D, IlluminanceMeasurement 0x0400 -- Matter
//     deliberately gives every "concentration measurement" species its own
//     cluster ID for exactly this reason), so one module's entire channel
//     set fits on its own single endpoint with zero conflicts. See this
//     file's own README section in docs/matter-bridge.md for the
//     investigation this conclusion is based on.
//
// Cluster implementation choices per endpoint:
//   - Descriptor: the classic ember DECLARE_DYNAMIC_ATTRIBUTE path (see
//     .cpp) -- the one proven-working mechanism for a Descriptor cluster on
//     a DYNAMIC endpoint in this SDK version (examples/bridge-app/linux's
//     own Device.h/main.cpp use exactly this, even though a modern
//     DescriptorCluster class also exists -- that modern class's only
//     confirmed integration path in this SDK version is CodegenIntegration
//     for STATIC/zap-declared endpoints, not dynamic ones).
//   - Bridged Device Basic Information, Temperature Measurement, Relative
//     Humidity Measurement, each PM Concentration Measurement instance, and
//     Illuminance Measurement: the modern v1.6.0.0 code-driven ServerCluster
//     classes (BridgedDeviceBasicInformationCluster, TemperatureMeasurementCluster,
//     RelativeHumidityMeasurementCluster, ConcentrationMeasurementCluster,
//     IlluminanceMeasurementCluster), constructed directly and registered
//     into CodegenDataModelProvider::Instance().Registry() -- exactly the
//     same mechanism examples/bridge-app/linux's own
//     Device::CreateBridgedDeviceInfo() already uses for Bridged Device
//     Basic Information on a dynamic endpoint, extended here to the other
//     clusters since v1.6.0.0 ships the same kind of class for them too (see
//     each cluster's own src/app/clusters/*/README.md).
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include <app/clusters/bridged-device-basic-information-server/BridgedDeviceBasicInformationCluster.h>
#include <app/clusters/concentration-measurement-server/ConcentrationMeasurementCluster.h>
#include <app/clusters/illuminance-measurement-server/IlluminanceMeasurementCluster.h>
#include <app/clusters/relative-humidity-measurement-server/RelativeHumidityMeasurementCluster.h>
#include <app/clusters/temperature-measurement-server/TemperatureMeasurementCluster.h>
#include <app/server-cluster/ServerClusterInterfaceRegistry.h>
#include <app/util/attribute-storage.h>
#include <lib/core/DataModelTypes.h>

#include "matter_bridge/bridge_manager.hpp"

namespace pogo_matter_bridge {

class ChipEndpointManager {
public:
    // Must be called exactly once, on the Matter event-loop thread, after
    // the data model / interaction model engine is up (this application's
    // own ApplicationInit(), see main.cpp) -- establishes the ember "dummy"
    // endpoint disable + the first dynamic endpoint id, mirroring
    // examples/bridge-app/linux/main.cpp's own ApplicationInit() exactly.
    void Init();

    // Applies one BridgeManager-produced update to the real Matter data
    // model: creates the endpoint first if update.is_new_endpoint, then
    // pushes whichever fields are actually present. See
    // matter_bridge::ModuleUpdate's own doc comment -- every field here is
    // already pre-deduplicated by BridgeManager, so this function issues
    // exactly one SDK call per present field, never more.
    //
    // Returns which of those calls actually succeeded (this pass -- see
    // matter_bridge::ApplyOutcome's own doc comment): the caller (main.cpp)
    // feeds this straight into matter_bridge::BridgeManager::CommitApplied()
    // so a failed SDK call is never silently treated as if it had taken
    // effect -- the corresponding field simply stays retryable.
    matter_bridge::ApplyOutcome Apply(const matter_bridge::ModuleUpdate &update);

private:
    struct ModuleEndpoint {
        chip::EndpointId endpoint_id = chip::kInvalidEndpointId;
        uint16_t ember_index = 0;

        // Descriptor is the endpoint's only ember/DECLARE_DYNAMIC_ATTRIBUTE
        // cluster (see this header's own top comment) -- one DataVersion
        // slot for it.
        std::array<chip::DataVersion, 1> ember_data_versions{};

        chip::app::LazyRegisteredServerCluster<chip::app::Clusters::BridgedDeviceBasicInformationCluster> basic_info;
        chip::app::LazyRegisteredServerCluster<chip::app::Clusters::TemperatureMeasurementCluster> temperature;
        chip::app::LazyRegisteredServerCluster<chip::app::Clusters::RelativeHumidityMeasurementCluster> humidity;
        chip::app::LazyRegisteredServerCluster<chip::app::Clusters::ConcentrationMeasurement::ConcentrationMeasurementCluster>
            pm1;
        chip::app::LazyRegisteredServerCluster<chip::app::Clusters::ConcentrationMeasurement::ConcentrationMeasurementCluster>
            pm25;
        chip::app::LazyRegisteredServerCluster<chip::app::Clusters::ConcentrationMeasurement::ConcentrationMeasurementCluster>
            pm10;
        chip::app::LazyRegisteredServerCluster<chip::app::Clusters::IlluminanceMeasurementCluster> illuminance;
    };

    // Returns true iff the endpoint itself was actually created
    // (emberAfSetDynamicEndpointWithEpUniqueId() succeeded and the new
    // ModuleEndpoint was inserted into modules_) -- see Apply()'s own doc
    // comment for how this feeds matter_bridge::ApplyOutcome::endpoint_created.
    bool CreateEndpoint(const matter_bridge::ModuleUpdate &update);

    std::unordered_map<uint16_t, std::unique_ptr<ModuleEndpoint>> modules_; // keyed by chip::EndpointId
    uint16_t next_ember_index_ = 0;
    chip::EndpointId aggregator_endpoint_id_ = chip::kInvalidEndpointId;
};

} // namespace pogo_matter_bridge
