#include "ingest/session.hpp"

#include <cstdio>
#include <cstring>

extern "C" {
#include "bridge_export_proto/bridge_export_proto.h"
#include "sensor_channel/sensor_channel.h"
}

namespace ingest {

namespace {

Uuid to_uuid(const uint8_t *bytes)
{
    Uuid u{};
    std::memcpy(u.data(), bytes, u.size());
    return u;
}

// Last-2-bytes suffix, matching the firmware's own "..%02x%02x" convention
// used throughout for a short, human-scannable identifier.
void print_uuid_suffix(const char *label, const Uuid &u)
{
    std::printf("%s=..%02x%02x", label, u[6], u[7]);
}

const char *result_str(ApplyResult r)
{
    switch (r) {
    case ApplyResult::AppliedNew:
        return "new";
    case ApplyResult::DuplicateExact:
        return "duplicate";
    case ApplyResult::StorageFull:
        return "storage_full";
    }
    return "?";
}

uint8_t result_to_coap_code(ApplyResult r)
{
    switch (r) {
    case ApplyResult::AppliedNew:
        return kCoapCodeChanged;
    case ApplyResult::DuplicateExact:
        return kCoapCodeValid;
    case ApplyResult::StorageFull:
        return kCoapCodeServiceUnavailable;
    }
    return kCoapCodeBadRequest;
}

uint8_t handle_sample(Registry &registry, const uint8_t *payload, uint16_t payload_len)
{
    bridge_export_sample_t exp;
    if (!bridge_export_unpack_sample(payload, payload_len, &exp)) {
        std::printf("[sample] malformed payload -- rejected (4.00)\n");
        return kCoapCodeBadRequest;
    }
    sensor_sample_t decoded;
    if (!sensor_channel_unpack(exp.sensor_channel_payload, exp.sensor_channel_len, &decoded)) {
        // bridge_export_unpack_sample() already validates this internally
        // (see that function's own doc comment) -- reaching here would mean
        // an internal inconsistency, not a wire-format issue a sender could
        // trigger; still handled defensively rather than assumed.
        std::printf("[sample] embedded sensor_channel frame failed to re-decode -- rejected (4.00)\n");
        return kCoapCodeBadRequest;
    }

    std::vector<ChannelReading> channels;
    channels.reserve(decoded.channel_count);
    for (uint8_t i = 0; i < decoded.channel_count; i++) {
        channels.push_back(ChannelReading{decoded.channels[i].type, decoded.channels[i].scale,
                                            decoded.channels[i].status, decoded.channels[i].value});
    }

    Uuid pb_uuid = to_uuid(exp.power_base_uuid);
    Uuid mod_uuid = to_uuid(decoded.module_uuid);
    ApplyResult result = registry.apply_sample(pb_uuid, mod_uuid, decoded.sample_seq, decoded.uptime_ms, channels);

    std::printf("[sample] ");
    print_uuid_suffix("pb", pb_uuid);
    std::printf(" ");
    print_uuid_suffix("mod", mod_uuid);
    std::printf(" seq=%u", (unsigned)decoded.sample_seq);
    for (const auto &ch : channels) {
        std::printf(" ch(type=%u,scale=%u,status=%u,value=%d)", (unsigned)ch.type, (unsigned)ch.scale,
                     (unsigned)ch.status, (int)ch.value);
    }
    std::printf(" %s\n", result_str(result));
    return result_to_coap_code(result);
}

uint8_t handle_topology(Registry &registry, const uint8_t *payload, uint16_t payload_len)
{
    bridge_export_topology_node_t node;
    if (!bridge_export_unpack_topology_node(payload, payload_len, &node)) {
        std::printf("[topology] malformed payload -- rejected (4.00)\n");
        return kCoapCodeBadRequest;
    }

    Uuid pb_uuid = to_uuid(node.power_base_uuid);
    Uuid mod_uuid = to_uuid(node.module_uuid);
    Uuid parent_uuid = to_uuid(node.parent_uuid);
    ApplyResult result = registry.apply_topology(pb_uuid, mod_uuid, parent_uuid, node.online, node.depth, node.role,
                                                  node.sensor_bitmap, node.downstream_connected,
                                                  node.boot_session_generation, node.last_seq);

    std::printf("[topology] ");
    print_uuid_suffix("pb", pb_uuid);
    std::printf(" ");
    print_uuid_suffix("mod", mod_uuid);
    std::printf(" online=%d depth=%u role=%u %s\n", node.online ? 1 : 0, (unsigned)node.depth, (unsigned)node.role,
                result_str(result));
    return result_to_coap_code(result);
}

uint8_t handle_heartbeat(Registry &registry, const uint8_t *payload, uint16_t payload_len)
{
    bridge_export_heartbeat_t hb;
    if (!bridge_export_unpack_heartbeat(payload, payload_len, &hb)) {
        std::printf("[heartbeat] malformed payload -- rejected (4.00)\n");
        return kCoapCodeBadRequest;
    }
    Uuid pb_uuid = to_uuid(hb.power_base_uuid);
    registry.apply_heartbeat(pb_uuid, hb.uptime_ms, hb.node_count);

    std::printf("[heartbeat] ");
    print_uuid_suffix("pb", pb_uuid);
    std::printf(" uptime_ms=%u node_count=%u\n", (unsigned)hb.uptime_ms, (unsigned)hb.node_count);
    return kCoapCodeChanged; // heartbeats have no duplicate concept -- see registry.hpp's own doc comment
}

} // namespace

uint8_t handle_request(Registry &registry, const CoapMessage &req)
{
    bridge_export_msg_type_t type;
    const uint8_t *payload;
    uint16_t payload_len;
    if (!bridge_export_unpack_envelope(req.payload.data(), req.payload.size(), &type, &payload, &payload_len)) {
        std::printf("[malformed] uri=%s payload_len=%zu -- rejected (4.00)\n", req.uri_path.c_str(),
                     req.payload.size());
        return kCoapCodeBadRequest;
    }

    if (req.uri_path == "pogo/v1/sample") {
        if (type != BRIDGE_EXPORT_MSG_SAMPLE) {
            std::printf("[sample] wrong msg_type=%d for this resource -- rejected (4.00)\n", (int)type);
            return kCoapCodeBadRequest;
        }
        return handle_sample(registry, payload, payload_len);
    }
    if (req.uri_path == "pogo/v1/topology") {
        if (type != BRIDGE_EXPORT_MSG_TOPOLOGY_SNAPSHOT) {
            std::printf("[topology] wrong msg_type=%d for this resource -- rejected (4.00)\n", (int)type);
            return kCoapCodeBadRequest;
        }
        return handle_topology(registry, payload, payload_len);
    }
    if (req.uri_path == "pogo/v1/heartbeat") {
        if (type != BRIDGE_EXPORT_MSG_HEARTBEAT) {
            std::printf("[heartbeat] wrong msg_type=%d for this resource -- rejected (4.00)\n", (int)type);
            return kCoapCodeBadRequest;
        }
        return handle_heartbeat(registry, payload, payload_len);
    }

    std::printf("[unknown-resource] uri=%s -- rejected (4.00)\n", req.uri_path.c_str());
    return kCoapCodeBadRequest;
}

} // namespace ingest
