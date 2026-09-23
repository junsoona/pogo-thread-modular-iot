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
//   - Bridged Device Basic Information, Identify (every endpoint), Temperature
//     Measurement, Relative Humidity Measurement, each PM Concentration
//     Measurement instance, Air Quality (PM-bearing endpoints only), and
//     Illuminance Measurement: the modern v1.6.0.0 code-driven ServerCluster
//     classes (BridgedDeviceBasicInformationCluster, IdentifyCluster,
//     TemperatureMeasurementCluster, RelativeHumidityMeasurementCluster,
//     ConcentrationMeasurementCluster, AirQualityCluster,
//     IlluminanceMeasurementCluster), constructed directly and registered
//     into CodegenDataModelProvider::Instance().Registry() -- exactly the
//     same mechanism examples/bridge-app/linux's own
//     Device::CreateBridgedDeviceInfo() already uses for Bridged Device
//     Basic Information on a dynamic endpoint, extended here to the other
//     clusters since v1.6.0.0 ships the same kind of class for them too (see
//     each cluster's own src/app/clusters/*/README.md).
//
// Device-type conformance (this pass): every module_uuid's dynamic
// endpoint's DeviceTypeList is cross-checked against
// src/app/zap-templates/zcl/data-model/chip/matter-devices.xml in a real
// v1.6.0.0 checkout (not assumed) for its MANDATORY clusters/attributes/
// commands. Findings: Temperature Sensor (0x0302), Humidity Sensor
// (0x0307), Air Quality Sensor (0x002C), and Light Sensor (0x0106) all
// mandate Identify (hence the unconditional IdentifyCluster above); Air
// Quality Sensor additionally mandates the Air Quality cluster itself
// (0x005B), which this bridge's PM-bearing endpoints previously omitted
// entirely (fixed this pass -- see .cpp's own comment on why its
// AirQuality attribute is never computed from a measurement). Every
// device type's own nominal `requireCommand IdentifyQuery` is stale ZCL-era
// XML with no corresponding command in this SDK's actual Identify cluster
// (data_model/1.6/clusters/Identify.xml defines only Identify and
// TriggerEffect) -- not implementable because it does not exist, not a gap
// in this bridge. Light Sensor's own nominal `include cluster="Binding"`
// has `server="false"`/not `requireCluster`, i.e. genuinely optional, and
// is not implemented (irrelevant to a bridge whose automation lives in the
// controller, not the bridged device).
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include <app/clusters/air-quality-server/AirQualityCluster.h>
#include <app/clusters/bridged-device-basic-information-server/BridgedDeviceBasicInformationCluster.h>
#include <app/clusters/concentration-measurement-server/ConcentrationMeasurementCluster.h>
#include <app/clusters/identify-server/IdentifyCluster.h>
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

        // Every module endpoint's device type (Temperature+Humidity Sensor,
        // Air Quality Sensor, or Light Sensor -- see .cpp) requires Identify
        // as a MANDATORY cluster (matter-devices.xml, cross-checked against
        // a real v1.6.0.0 checkout this pass) -- unconditionally created for
        // every endpoint, unlike the sensor-specific clusters above.
        chip::app::LazyRegisteredServerCluster<chip::app::Clusters::IdentifyCluster> identify;

        // Air Quality Sensor's own MANDATORY cluster (0x005B) -- only
        // created for a module with a PM channel (see .cpp's own top
        // comment on why this is never computed from a measurement, only
        // ever left at its class default of Unknown).
        chip::app::LazyRegisteredServerCluster<chip::app::Clusters::AirQualityCluster> air_quality;
    };

    // Returns true iff the endpoint itself was actually created
    // (emberAfSetDynamicEndpointWithEpUniqueId() succeeded and the new
    // ModuleEndpoint was inserted into modules_) -- see Apply()'s own doc
    // comment for how this feeds matter_bridge::ApplyOutcome::endpoint_created.
    bool CreateEndpoint(const matter_bridge::ModuleUpdate &update);

    // Mirror image of CreateEndpoint(): unregisters and destroys every
    // cluster it created (reverse order), then clears the ember dynamic
    // endpoint slot. Checked against connectedhomeip v1.6.0.0's
    // ServerClusterInterfaceRegistry::Unregister(ServerClusterInterface *)
    // -> CHIP_ERROR (CHIP_ERROR_NOT_FOUND if not registered) and
    // emberAfClearDynamicEndpoint(uint16_t, MatterClusterShutdownType)
    // -> EndpointId. Returns true once the endpoint is fully gone (including
    // when endpoint_id is no longer tracked at all -- an already-finished
    // removal); false leaves whatever is still registered in place for the
    // next attempt (see the .cpp).
    bool RemoveEndpoint(uint16_t endpoint_id);

    // Lowest dynamic ember index (0..CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT-1)
    // not held by a live ModuleEndpoint, or kEmberInvalidEndpointIndex. A
    // removed module's slot is reused, so repeated detach/re-dock cycles
    // never exhaust the dynamic endpoint table.
    uint16_t AllocateEmberIndex() const;

    std::unordered_map<uint16_t, std::unique_ptr<ModuleEndpoint>> modules_; // keyed by chip::EndpointId
    chip::EndpointId aggregator_endpoint_id_ = chip::kInvalidEndpointId;
};

} // namespace pogo_matter_bridge
