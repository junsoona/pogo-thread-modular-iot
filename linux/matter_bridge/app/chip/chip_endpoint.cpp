#include "chip_endpoint.hpp"

#include <cstdio>
#include <cstring>

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/util/af-types.h>
#include <app/util/endpoint-config-api.h>
#include <app/util/util.h>
#include <clusters/AirQuality/Enums.h>
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
// on a module endpoint (Bridged Device Basic Information, Identify,
// Temperature Measurement, Relative Humidity Measurement, each PM
// Concentration Measurement instance, Air Quality, Illuminance Measurement)
// is a modern code-driven ServerCluster, registered directly in C++ below,
// never declared here.
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
// cross-checked against a real connectedhomeip v1.6.0.0 checkout at exactly
// that tag this pass -- see docs/matter-bridge.md for the investigation
// these were picked from)
constexpr uint16_t kDeviceTypeBridgedNode = 0x0013;
constexpr uint16_t kDeviceTypeTempSensor = 0x0302;
constexpr uint16_t kDeviceTypeHumiditySensor = 0x0307;
constexpr uint16_t kDeviceTypeAirQualitySensor = 0x002C;
constexpr uint16_t kDeviceTypeLightSensor = 0x0106;
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
// BH1750-only module: Light Sensor (matter-devices.xml requires Identify +
// Descriptor + Illuminance Measurement -- Identify is created unconditionally
// for every endpoint below, see CreateEndpoint(); the nominal "Binding"
// include on this device type has server="false"/is not required, and is
// not implemented, same reasoning as every other bridged device type here).
constexpr EmberAfDeviceType kBh1750DeviceTypes[] = {
    {kDeviceTypeLightSensor, kDeviceVersionDefault},
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

// This bridge's Identify cluster delegate: every module endpoint's
// underlying device is a battery/USB-powered Thread sensor module with no
// LED, speaker, or other actuator this bridge can drive to make it
// physically identify itself -- IdentifyType::kNone (set at construction,
// see CreateEndpoint()) already declares that honestly, so these callbacks
// are deliberately no-ops (logged, for operator visibility) rather than a
// fabricated "something happened" response. This is NOT the same kind of
// fabrication this bridge avoids elsewhere (e.g. never synthesizing an Air
// Quality grade from PM concentrations, see CreateEndpoint()'s own comment
// on AirQualityCluster): an Identify/TriggerEffect command's own effects
// are spec-defined as advisory hints a device MAY act on, and accepting-
// but-not-physically-acting is standard, spec-sanctioned behavior for a
// device with no matching actuator -- it does not misrepresent any
// measurement or capability. IsTriggerEffectEnabled()==true so
// TriggerEffect (mandatory per Temperature/Humidity/Light Sensor's own
// device type, see matter-devices.xml) is actually accepted rather than
// rejected as UnsupportedCommand. One shared, stateless instance, same
// reuse pattern as gBasicInformationDelegate above.
class NoopIdentifyDelegate : public IdentifyDelegate {
public:
    void OnIdentifyStart(IdentifyCluster &) override
    {
        ChipLogProgress(NotSpecified, "pogo_matter_bridge: Identify started (no physical presentation available "
                                     "on this bridged Thread sensor module)");
    }
    void OnIdentifyStop(IdentifyCluster &) override {}
    void OnTriggerEffect(IdentifyCluster &) override
    {
        ChipLogProgress(NotSpecified, "pogo_matter_bridge: TriggerEffect received (no-op -- no physical actuator "
                                     "on this bridged Thread sensor module)");
    }
    bool IsTriggerEffectEnabled() const override { return true; }
};
NoopIdentifyDelegate gIdentifyDelegate;

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

DataModel::Nullable<uint16_t> ToNullable(const matter_bridge::IlluminanceMeasuredValue &lux)
{
    if (lux.state != matter_bridge::Measurement::Valid) {
        return DataModel::Nullable<uint16_t>();
    }
    return DataModel::MakeNullable(lux.value);
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

// This cluster's own configured minMeasuredValue/maxMeasuredValue are
// matter_bridge::kIlluminanceMeasuredValueMin/Max (value_mapping.hpp) --
// the SAME shared constants matter_bridge::ConvertIlluminance() itself uses
// to decide Valid vs. Invalid -- deliberately NOT a narrower,
// device-tuned bound the way kPmMaxUgm3 above is for PM. An earlier
// version of this file used a narrower literal (50000, ~99,995 lux) here,
// which real direct-sunlight illuminance (~100,000 lux, encoding to
// ~50001) could exceed: ConvertIlluminance() would still report it Valid,
// but SetMeasuredValue() would reject it (ConstraintError) every single
// time, forever, since BridgeManager has no way to tell "will succeed on
// retry" apart from "structurally can never succeed" -- see
// value_mapping.hpp's own doc comment on kIlluminanceMeasuredValueMax for
// the full reasoning. Sharing the exact same constant instead of
// maintaining two independently-tuned bounds makes that class of bug
// structurally impossible to reintroduce.

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

bool ChipEndpointManager::CreateEndpoint(const matter_bridge::ModuleUpdate &update)
{
    auto ep = std::make_unique<ModuleEndpoint>();
    ep->endpoint_id = update.endpoint_id;
    ep->ember_index = next_ember_index_++;

    Span<const EmberAfDeviceType> device_types;
    if (update.has_temperature_channel || update.has_humidity_channel) {
        device_types = Span<const EmberAfDeviceType>(kSht45DeviceTypes);
    } else if (update.has_pm1_channel || update.has_pm25_channel || update.has_pm10_channel) {
        device_types = Span<const EmberAfDeviceType>(kPms5003DeviceTypes);
    } else if (update.has_illuminance_channel) {
        device_types = Span<const EmberAfDeviceType>(kBh1750DeviceTypes);
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
        return false;
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

    // Identify: MANDATORY on every device type this bridge ever declares
    // (Temperature Sensor, Humidity Sensor, Air Quality Sensor, Light
    // Sensor -- confirmed against matter-devices.xml in a real v1.6.0.0
    // checkout this pass, see chip_endpoint.hpp's own top comment), so
    // created unconditionally rather than gated on any has_*_channel flag.
    // IdentifyType::kNone is this bridge's own honest declaration -- these
    // bridged Thread sensor modules have no LED/speaker/actuator this
    // bridge can drive, so there is no real "visible indicator" or similar
    // to claim.
    ep->identify.Create(IdentifyCluster::Config(ep->endpoint_id, gTimerDelegate)
                            .WithIdentifyType(Identify::IdentifyTypeEnum::kNone)
                            .WithDelegate(&gIdentifyDelegate));
    LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(ep->identify.Registration()));

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

    if (update.has_pm1_channel || update.has_pm25_channel || update.has_pm10_channel) {
        // Air Quality Sensor's own MANDATORY cluster (0x005B, serverLocked
        // in matter-devices.xml) -- this endpoint declared that device type
        // (kPms5003DeviceTypes below) but, before this pass, never actually
        // implemented this cluster at all. BitFlags<Feature>() enables NONE
        // of the optional Fair/Moderate/VeryPoor/ExtremelyPoor gradations
        // (AirQualityCluster::SetAirQuality() would reject any of them with
        // ConstraintError otherwise), which restricts every legal value to
        // Unknown/Good/Poor -- and this bridge never calls SetAirQuality()
        // at all, leaving it at the class's own default, Unknown, forever.
        // This bridge has PM1/PM2.5/PM10 concentrations only, no CO/CO2/
        // NO2/O3/HCHO/TVOC/Radon inputs and no calibrated breakpoint table
        // validated for this deployment -- inventing a Good/Fair/Moderate/
        // Poor grade from PM alone would be exactly the kind of ungrounded
        // classification this bridge's existing SGP40/41 Gas Index policy
        // already refuses to do (never dress up a partial/uncalibrated
        // reading as a value it was never designed to produce; see
        // docs/matter-bridge.md section 3.1). Unknown is itself a fully
        // spec-legitimate, mandatory-conformance value -- not a placeholder
        // that then needs replacing.
        ep->air_quality.Create(ep->endpoint_id, BitFlags<AirQuality::Feature>());
        LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(ep->air_quality.Registration()));
    }

    if (update.has_illuminance_channel) {
        // LightSensorType is exposed (BH1750 is a photodiode-based ambient
        // light sensor per its own datasheet, not a CMOS imager) -- Tolerance
        // is left unset/unexposed, same as TemperatureMeasurementCluster's
        // own optional_attrs(0) above: this bridge has no real tolerance
        // figure from the sensor_channel wire format to report.
        IlluminanceMeasurementCluster::OptionalAttributeSet optional_attrs;
        optional_attrs.Set<IlluminanceMeasurement::Attributes::LightSensorType::Id>();
        ep->illuminance.Create(ep->endpoint_id, optional_attrs,
                               IlluminanceMeasurementCluster::StartupConfiguration{
                                   .minMeasuredValue = DataModel::MakeNullable(matter_bridge::kIlluminanceMeasuredValueMin),
                                   .maxMeasuredValue = DataModel::MakeNullable(matter_bridge::kIlluminanceMeasuredValueMax),
                                   .tolerance = 0,
                                   .lightSensorType = DataModel::MakeNullable(IlluminanceMeasurement::LightSensorTypeEnum::kPhotodiode),
                               });
        LogErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(ep->illuminance.Registration()));
    }

    modules_[ep->endpoint_id] = std::move(ep);
    return true;
}

matter_bridge::ApplyOutcome ChipEndpointManager::Apply(const matter_bridge::ModuleUpdate &update)
{
    matter_bridge::ApplyOutcome outcome;

    if (update.is_new_endpoint) {
        outcome.endpoint_created = CreateEndpoint(update);
    }

    auto it = modules_.find(update.endpoint_id);
    if (it == modules_.end()) {
        // CreateEndpoint() above failed (logged there already), or this
        // update targets an endpoint that was never successfully created on
        // an earlier attempt either -- nothing more can be applied. Every
        // outcome field stays false, so the caller (main.cpp) never commits
        // any of this update as having succeeded -- see
        // matter_bridge::BridgeManager::CommitApplied()'s own doc comment.
        return outcome;
    }
    ModuleEndpoint &ep = *it->second;

    if (update.reachable.has_value() && ep.basic_info.IsConstructed()) {
        // SetReachable() returns void (no LogErrorOnFailure() call site
        // exists for it anywhere in this SDK's own examples either) --
        // dispatching it with a constructed cluster is considered
        // unconditionally successful, same as this function always treated
        // it before this pass.
        ep.basic_info.Cluster().SetReachable(*update.reachable);
        outcome.reachable_applied = true;
    }
    if (update.temperature.has_value() && ep.temperature.IsConstructed()) {
        CHIP_ERROR err = ep.temperature.Cluster().SetMeasuredValue(ToNullable(*update.temperature));
        LogErrorOnFailure(err);
        outcome.temperature_applied = (err == CHIP_NO_ERROR);
    }
    if (update.humidity.has_value() && ep.humidity.IsConstructed()) {
        CHIP_ERROR err = ep.humidity.Cluster().SetMeasuredValue(ToNullable(*update.humidity));
        LogErrorOnFailure(err);
        outcome.humidity_applied = (err == CHIP_NO_ERROR);
    }
    if (update.pm1.has_value() && ep.pm1.IsConstructed()) {
        CHIP_ERROR err = ep.pm1.Cluster().SetMeasuredValue(ToNullable(*update.pm1));
        LogErrorOnFailure(err);
        outcome.pm1_applied = (err == CHIP_NO_ERROR);
    }
    if (update.pm25.has_value() && ep.pm25.IsConstructed()) {
        CHIP_ERROR err = ep.pm25.Cluster().SetMeasuredValue(ToNullable(*update.pm25));
        LogErrorOnFailure(err);
        outcome.pm25_applied = (err == CHIP_NO_ERROR);
    }
    if (update.pm10.has_value() && ep.pm10.IsConstructed()) {
        CHIP_ERROR err = ep.pm10.Cluster().SetMeasuredValue(ToNullable(*update.pm10));
        LogErrorOnFailure(err);
        outcome.pm10_applied = (err == CHIP_NO_ERROR);
    }
    if (update.illuminance.has_value() && ep.illuminance.IsConstructed()) {
        CHIP_ERROR err = ep.illuminance.Cluster().SetMeasuredValue(ToNullable(*update.illuminance));
        LogErrorOnFailure(err);
        outcome.illuminance_applied = (err == CHIP_NO_ERROR);
    }
    return outcome;
}

} // namespace pogo_matter_bridge
