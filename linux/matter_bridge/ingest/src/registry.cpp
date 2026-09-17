#include "ingest/registry.hpp"

#include <algorithm>

namespace ingest {

NodeState *Registry::find_locked(const Uuid &power_base_uuid, const Uuid &module_uuid)
{
    for (auto &n : nodes_) {
        if (n.power_base_uuid == power_base_uuid && n.module_uuid == module_uuid) {
            return &n;
        }
    }
    return nullptr;
}

ApplyResult Registry::apply_topology(const Uuid &power_base_uuid, const Uuid &module_uuid, const Uuid &parent_uuid,
                                      bool online, uint8_t depth, uint8_t role, uint16_t sensor_bitmap,
                                      bool downstream_connected, uint32_t boot_session_generation, uint32_t last_seq)
{
    std::lock_guard<std::mutex> lock(mutex_);
    NodeState *n = find_locked(power_base_uuid, module_uuid);
    if (n == nullptr) {
        if (nodes_.size() >= kMaxNodes) {
            return ApplyResult::StorageFull;
        }
        nodes_.emplace_back();
        n = &nodes_.back();
        n->power_base_uuid = power_base_uuid;
        n->module_uuid = module_uuid;
    }

    bool exact_duplicate = n->has_topology && n->parent_uuid == parent_uuid && n->online == online &&
                            n->depth == depth && n->role == role && n->sensor_bitmap == sensor_bitmap &&
                            n->downstream_connected == downstream_connected &&
                            n->boot_session_generation == boot_session_generation && n->last_seq == last_seq;
    if (exact_duplicate) {
        return ApplyResult::DuplicateExact;
    }

    n->has_topology = true;
    n->parent_uuid = parent_uuid;
    n->online = online;
    n->depth = depth;
    n->role = role;
    n->sensor_bitmap = sensor_bitmap;
    n->downstream_connected = downstream_connected;
    n->boot_session_generation = boot_session_generation;
    n->last_seq = last_seq;
    return ApplyResult::AppliedNew;
}

ApplyResult Registry::apply_sample(const Uuid &power_base_uuid, const Uuid &module_uuid, uint32_t sample_seq,
                                    uint32_t uptime_ms, const std::vector<ChannelReading> &channels)
{
    std::lock_guard<std::mutex> lock(mutex_);
    NodeState *n = find_locked(power_base_uuid, module_uuid);
    if (n == nullptr) {
        if (nodes_.size() >= kMaxNodes) {
            return ApplyResult::StorageFull;
        }
        nodes_.emplace_back();
        n = &nodes_.back();
        n->power_base_uuid = power_base_uuid;
        n->module_uuid = module_uuid;
    }

    // Dedup key (power_base_uuid, module_uuid, sample_seq) -- an exact
    // repeat of the last-recorded seq for this (power_base_uuid, module_uuid)
    // is a retransmit, same policy as the firmware's own
    // sensor_root_aggregator_record_sample().
    if (n->has_sample && n->last_sample_seq == sample_seq) {
        return ApplyResult::DuplicateExact;
    }

    n->has_sample = true;
    n->last_sample_seq = sample_seq;
    n->last_sample_uptime_ms = uptime_ms;
    n->last_channels = channels;
    return ApplyResult::AppliedNew;
}

void Registry::apply_heartbeat(const Uuid &power_base_uuid, uint32_t /*uptime_ms*/, uint16_t /*node_count*/)
{
    // Deliberately a no-op on node state today (see this header's own doc
    // comment: never authoritative) -- kept as an explicit method rather
    // than silently dropping HEARTBEAT messages at the call site, so a
    // future revision that wants to track "Power Base last-heard-from" has
    // one obvious place to add it. power_base_uuid is intentionally unused
    // for now.
    (void)power_base_uuid;
}

size_t Registry::node_count() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return nodes_.size();
}

bool Registry::get_node(size_t index, NodeState &out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= nodes_.size()) {
        return false;
    }
    out = nodes_[index];
    return true;
}

} // namespace ingest
