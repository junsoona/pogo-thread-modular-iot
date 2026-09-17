#include "chip_endpoint.hpp"

#include <cstdio>
#include <cstring>

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/util/af-types.h>
#include <app/util/endpoint-config-api.h>
#include <app/util/util.h>
#include <clusters/Descriptor/Attributes.h>
#include <clusters/Descriptor/ClusterId.h>
#include <clusters/Pm10ConcentrationMeasurement/ClusterId.h>
#include <clusters/Pm1ConcentrationMeasurement/ClusterId.h>
#include <clusters/Pm25ConcentrationMeasurement/ClusterId.h>
#include <data-model-providers/codegen/CodegenDataModelProvider.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/DefaultTimerDelegate.h>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;

namespace pogo_matter_bridge {

namespace {

// Current ZCL implementation of a struct-typed attribute uses a max-size
// byte array -- see examples/bridge-app/linux/Device.h, which this constant
// and the descriptor attribute declaration below are copied from verbatim.
constexpr int kDescriptorAttributeArraySize = 254;

// Descriptor is this bridge's ONLY ember/DECLARE_DYNAMIC_ATTRIBUTE cluster
// (see chip_endpoint.hpp's own top comment for why) -- every OTHER cluster
// on a module endpoint (Bridged Device Basic Information, Temperature
// Measurement, Relative Humidity Measurement, each PM Concentration
// Measurement instance) is a modern code-driven ServerCluster, registered
// directly in C++ below, never declared here.
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(descriptorAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::DeviceTypeList::Id, ARRAY, kDescriptorAttributeArraySize, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::ServerList::Id, ARRAY, kDescriptorAttributeArraySize, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::ClientList::Id, ARRAY, kDescriptorAttributeArraySize, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::PartsList::Id, ARRAY, kDescriptorAttributeArraySize, 0),
#if CHIP_CONFIG_USE_ENDPOINT_UNIQUE_ID
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::EndpointUniqueID::Id, ARRAY, 32, 0),
#endif
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(moduleClusters)
DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(moduleEndpointType, moduleClusters);

// (taken from src/app/zap-templates/zcl/data-model/chip/matter-devices.xml,
// see docs/matter-bridge.md for the investigation these were picked from)
constexpr uint16_t kDeviceTypeBridgedNode = 0x0013;
constexpr uint16_t kDeviceTypeTempSensor = 0x0302;
constexpr uint16_t kDeviceTypeHumiditySensor = 0x0307;
constexpr uint16_t kDeviceTypeAirQualitySensor = 0x002C;
constexpr uint16_t kDeviceVersionDefault = 1;

constexpr EmberAfDeviceType kBridgedNodeOnlyDeviceTypes[] = {
    {kDeviceTypeBridgedNode, kDeviceVersionDefault},
};
constexpr EmberAfDeviceType kSht45DeviceTypes[] = {
    {kDeviceTypeTempSensor, kDeviceVersionDefault},
    {kDeviceTypeHumiditySensor, kDeviceVersionDefault},
    {kDeviceTypeBridgedNode, kDeviceVersionDefault},
};
constexpr EmberAfDeviceType kPms5003DeviceTypes[] = {
    {kDeviceTypeAirQualitySensor, kDeviceVersionDefault},
    {kDeviceTypeBridgedNode, kDeviceVersionDefault},
};

// The Bridged Device Basic Information cluster's delegate interface has
// default (no-op success) implementations for every method (see
// BridgedDeviceBasicInformationDelegate.h) -- this bridge does not need to
// customize node-label-change handling in this MVP, so one shared,
// stateless instance is reused for every module endpoint rather than a
// subclass per module.
BridgedDeviceBasicInformationDelegate gBasicInformationDelegate;

// Shared by every module's Bridged Device Basic Information cluster
// instance, exactly like examples/bridge-app/linux/main.cpp's own function-
// local `static chip::app::DefaultTimerDelegate timerDelegate;`.
DefaultTimerDelegate gTimerDelegate;

// module_uuid's own permanent 8 bytes, lowercase-hex-encoded -- this
// bridge's Matter EndpointUniqueID (chip_enable_endpoint_unique_id, see
// args.gni) AND the Bridged Device Basic Information UniqueId, so a
// controller's own notion of "this bridged device's stable identity"
// matches this bridge's own module_uuid-keyed identity exactly (see
// docs/matter-bridge.md's own UUID/endpoint persistence design).
void FormatUuidHex(const ingest::Uuid &uuid, char out[17])
{
    static const char kHexDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < uuid.size(); i++) {
        out[i * 2] = kHexDigits[(uuid[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHexDigits[uuid[i] & 0x0F];
    }
    out[uuid.size() * 2] = '\0';
}

DataModel::Nullable<int16_t> ToNullable(const matter_bridge::TemperatureCentiC &t)
{
    if (t.state != matter_bridge::Measurement::Valid) {
        return DataModel::Nullable<int16_t>(); // null
    }
    return DataModel::MakeNullable(t.value);
}

DataModel::Nullable<uint16_t> ToNullable(const matter_bridge::HumidityCentiPercent &h)
{
    if (h.state != matter_bridge::Measurement::Valid) {
        return DataModel::Nullable<uint16_t>();
    }
    return DataModel::MakeNullable(h.value);
}

DataModel::Nullable<float> ToNullable(const matter_bridge::PmConcentrationUgm3 &pm)
{
    if (pm.state != matter_bridge::Measurement::Valid) {
        return DataModel::Nullable<float>();
    }
    return DataModel::MakeNullable(pm.value);
}

// PMS5003's own rated maximum (datasheet: 0..1000 ug/m3 for PM1/PM2.5,
// 0..1000 ug/m3 for PM10 too) with generous headroom -- this is the
// cluster's OWN configured valid range (SetMeasuredValue() rejects
// anything outside it), separate from and in addition to
// matter_bridge::ConvertPm()'s own "physically impossible" rejection (see
// that function's own doc comment).
constexpr float kPmMaxUgm3 = 2000.0f;

ConcentrationMeasurement::ConcentrationMeasurementCluster::Config MakePmConfig(ClusterId cluster_id)
{
    ConcentrationMeasurement::ConcentrationMeasurementCluster::Config cfg;
    cfg.clusterId = cluster_id;
    cfg.features = BitFlags<ConcentrationMeasurement::Feature>(ConcentrationMeasurement::Feature::kNumericMeasurement);
    cfg.medium = ConcentrationMeasurement::MeasurementMediumEnum::kAir;
    cfg.unit = ConcentrationMeasurement::MeasurementUnitEnum::kUgm3;
    cfg.minMeasured = DataModel::MakeNullable(0.0f);
    cfg.maxMeasured = DataModel::MakeNullable(kPmMaxUgm3);
    return cfg;
}

} // namespace

// This bridge has no ember-external-storage attribute that ever actually
// needs serving here -- Descriptor (this bridge's only ember-declared
// cluster) is served by its own connectedhomeip-internal
// CodegenIntegration-registered modern cluster instance, never by this
// callback (see chip_endpoint.hpp's own top comment; this mirrors
// examples/bridge-app/linux's own main.cpp, which likewise never handles
// Descriptor here). Defined anyway (returning Failure) purely so the
// symbol exists for the linker -- connectedhomeip's ember core calls this
// unconditionally for any EXTERNAL_STORAGE attribute it can't otherwise
// resolve.
Protocols::InteractionModel::Status emberAfExternalAttributeReadCallback(EndpointId, ClusterId, const EmberAfAttributeMetadata *,
                                                                         uint8_t *, uint16_t)
{
    return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status emberAfExternalAttributeWriteCallback(EndpointId, ClusterId,
                                                                          const EmberAfAttributeMetadata *, uint8_t *)
{
    return Protocols::InteractionModel::Status::Failure;
}

void ChipEndpointManager::Init()
{
    // Endpoint 2 is bridge-common/pogo-matter-bridge.zap's own fixed
    // "dummy" endpoint -- it exists ONLY so ZAP generates code for every
    // cluster this bridge might need (see bridge-common/BUILD.gn's own top
    // comment); it must never be visible to a controller.
    constexpr EndpointId kDummyEndpointId = 2;
    emberAfEndpointEnableDisable(kDummyEndpointId, false);

    aggregator_endpoint_id_ = 1;
}

void ChipEndpointManager::CreateEndpoint(const matter_bridge::ModuleUpdate &update)
{
    auto ep = std::make_unique<ModuleEndpoint>();
    ep->endpoint_id = update.endpoint_id;
    ep->ember_index = next_ember_index_++;

    Span<const EmberAfDeviceType> device_types;
    if (update.has_temperature_channel || update.has_humidity_channel) {
        device_types = Span<const EmberAfDeviceType>(kSht45DeviceTypes);
    } else if (update.has_pm1_channel || update.has_pm25_channel || update.has_pm10_channel) {
        device_types = Span<const EmberAfDeviceType>(kPms5003DeviceTypes);
    } else {
        // Should not happen -- matter_bridge::BridgeManager never emits an
        // is_new_endpoint update for a module with none of these channels
        // (see bridge_manager.cpp). Falling back to a bare Bridged Node
        // rather than asserting keeps this bridge alive even if that
        // invariant is ever violated by a future change.
        device_types = Span<const EmberAfDeviceType>(kBridgedNodeOnlyDeviceTypes);
    }

    char unique_id[17];
    FormatUuidHex(update.module_uuid, unique_id);
    CharSpan unique_id_span(unique_id, strlen(unique_id));

    CHIP_ERROR err = emberAfSetDynamicEndpointWithEpUniqueId(ep->ember_index, ep->endpoint_id, &moduleEndpointType,
                                                             Span<DataVersion>(ep->ember_data_versions), device_types,
                                                             unique_id_span, aggregator_endpoint_id_);
    if (err != CHIP_NO_ERROR) {
        ChipLogError(NotSpecified, "pogo_matter_bridge: failed to create dynamic endpoint %u for module: %" CHIP_ERROR_FORMAT,
                    (unsigned) ep->endpoint_id, err.Format());
        return;
    }

    BridgedDeviceBasicInformationCluster::MutableData mutable_data;
    mutable_data.reachable = update.reachable.value_or(false);
    BridgedDeviceBasicInformationCluster::FixedData fixed_data;
    fixed_data.uniqueId = unique_id;

    ep->basic_info.Create(ep->endpoint_id, std::move(mutable_data), std::move(fixed_data),
                          BridgedDeviceBasicInformationCluster::Context{
                              .delegate = gBasicInformationDelegate,
                              .timerDelegate = gTimerDelegate,
                          });
    LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(ep->basic_info.Registration()));

    if (update.has_temperature_channel) {
        TemperatureMeasurementCluster::OptionalAttributeSet optional_attrs(0);
        ep->temperature.Create(ep->endpoint_id, optional_attrs,
                               TemperatureMeasurementCluster::StartupConfiguration{
                                   .minMeasuredValue = DataModel::MakeNullable((int16_t) -27315),
                                   .maxMeasuredValue = DataModel::MakeNullable((int16_t) 32767),
                                   .tolerance = 0,
                               });
        LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(ep->temperature.Registration()));
    }

    if (update.has_humidity_channel) {
        RelativeHumidityMeasurementCluster::Config cfg;
        cfg.minMeasuredValue = DataModel::MakeNullable((uint16_t) 0);
        cfg.maxMeasuredValue = DataModel::MakeNullable((uint16_t) 10000);
        ep->humidity.Create(ep->endpoint_id, cfg);
        LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(ep->humidity.Registration()));
    }

    if (update.has_pm1_channel) {
        ep->pm1.Create(ep->endpoint_id, MakePmConfig(Pm1ConcentrationMeasurement::Id));
        LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(ep->pm1.Registration()));
    }
    if (update.has_pm25_channel) {
        ep->pm25.Create(ep->endpoint_id, MakePmConfig(Pm25ConcentrationMeasurement::Id));
        LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(ep->pm25.Registration()));
    }
    if (update.has_pm10_channel) {
        ep->pm10.Create(ep->endpoint_id, MakePmConfig(Pm10ConcentrationMeasurement::Id));
        LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(ep->pm10.Registration()));
    }

    modules_[ep->endpoint_id] = std::move(ep);
}

void ChipEndpointManager::Apply(const matter_bridge::ModuleUpdate &update)
{
    if (update.is_new_endpoint) {
        CreateEndpoint(update);
    }

    auto it = modules_.find(update.endpoint_id);
    if (it == modules_.end()) {
        // CreateEndpoint() above failed (logged there already) -- nothing
        // more can be applied to an endpoint that doesn't exist.
        return;
    }
    ModuleEndpoint &ep = *it->second;

    if (update.reachable.has_value() && ep.basic_info.IsConstructed()) {
        ep.basic_info.Cluster().SetReachable(*update.reachable);
    }
    if (update.temperature.has_value() && ep.temperature.IsConstructed()) {
        LogErrorOnFailure(ep.temperature.Cluster().SetMeasuredValue(ToNullable(*update.temperature)));
    }
    if (update.humidity.has_value() && ep.humidity.IsConstructed()) {
        LogErrorOnFailure(ep.humidity.Cluster().SetMeasuredValue(ToNullable(*update.humidity)));
    }
    if (update.pm1.has_value() && ep.pm1.IsConstructed()) {
        LogErrorOnFailure(ep.pm1.Cluster().SetMeasuredValue(ToNullable(*update.pm1)));
    }
    if (update.pm25.has_value() && ep.pm25.IsConstructed()) {
        LogErrorOnFailure(ep.pm25.Cluster().SetMeasuredValue(ToNullable(*update.pm25)));
    }
    if (update.pm10.has_value() && ep.pm10.IsConstructed()) {
        LogErrorOnFailure(ep.pm10.Cluster().SetMeasuredValue(ToNullable(*update.pm10)));
    }
}

} // namespace pogo_matter_bridge
