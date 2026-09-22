#include "ingest/registry.hpp"

#include <algorithm>
#include <cstdio>

namespace ingest {

namespace {
// Last-2-bytes suffix, matching session.cpp's own "..%02x%02x" convention
// used throughout this program for a short, human-scannable identifier.
void print_uuid_suffix(const char *label, const Uuid &u)
{
    std::printf("%s=..%02x%02x", label, u[6], u[7]);
}
} // namespace

Registry::Registry(std::chrono::milliseconds stale_timeout, std::chrono::milliseconds sweep_interval)
    : stale_timeout_(stale_timeout), sweep_interval_(sweep_interval), sweep_thread_(&Registry::sweep_thread_main, this)
{
}

Registry::~Registry()
{
    stop_sweep_.store(true, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cv_.notify_all();
    }
    if (sweep_thread_.joinable()) {
        sweep_thread_.join();
    }
}

void Registry::sweep_thread_main()
{
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_sweep_.load(std::memory_order_relaxed)) {
        // Never a busy loop: blocks here for the whole interval unless
        // woken early by a real apply_topology()/apply_sample() change (see
        // note_applied_locked()) or the destructor's own notify_all().
        cv_.wait_for(lock, sweep_interval_);
        if (stop_sweep_.load(std::memory_order_relaxed)) {
            break;
        }
        sweep_stale_locked(std::chrono::steady_clock::now());
    }
}

void Registry::sweep_stale_locked(std::chrono::steady_clock::time_point now)
{
    for (auto &n : nodes_) {
        if (n.online && (now - n.last_seen_at > stale_timeout_)) {
            n.online = false;
            std::printf("[topology] ");
            print_uuid_suffix("pb", n.power_base_uuid);
            std::printf(" ");
            print_uuid_suffix("mod", n.module_uuid);
            std::printf(" online=0 -- stale: no accepted message in over %lldms (bound=%lldms)\n",
                        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now - n.last_seen_at).count(),
                        (long long)stale_timeout_.count());
            note_applied_locked();
        }
    }
}

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
                                      bool downstream_connected, uint32_t boot_session_generation, uint32_t last_seq,
                                      std::chrono::steady_clock::time_point now)
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

    if (!n->has_topology) {
        // First-ever sighting of this (power_base_uuid, module_uuid) --
        // accepted unconditionally, same as
        // components/topology_registry_apply_report()'s own first-insert
        // path (nothing to compare a generation/seq against yet).
        n->has_topology = true;
        n->parent_uuid = parent_uuid;
        n->online = online;
        n->depth = depth;
        n->role = role;
        n->sensor_bitmap = sensor_bitmap;
        n->downstream_connected = downstream_connected;
        n->boot_session_generation = boot_session_generation;
        n->last_seq = last_seq;
        n->has_superseded_generation = false;
        n->last_seen_at = now;
        note_applied_locked();
        return ApplyResult::AppliedNew;
    }

    if (boot_session_generation == n->boot_session_generation) {
        // Same session: sequence must be an exact match (idempotent
        // retransmit -> duplicate) or STRICTLY LARGER (a real, later
        // update) -- a smaller-or-equal-but-different seq is a
        // reordered/delayed packet and must never apply (this is exactly
        // what stops a stale "online" snapshot from resurrecting a node a
        // later report already marked offline).
        if (last_seq == n->last_seq) {
            // An idempotent retransmit still proves the node is alive
            // right now (e.g. the exporter's own periodic full-resync
            // re-sending unchanged state) -- refresh liveness even though
            // nothing else about the stored state changes.
            n->last_seen_at = now;
            return ApplyResult::DuplicateExact;
        }
        if (last_seq < n->last_seq) {
            return ApplyResult::StaleSession;
        }
        n->parent_uuid = parent_uuid;
        n->online = online;
        n->depth = depth;
        n->role = role;
        n->sensor_bitmap = sensor_bitmap;
        n->downstream_connected = downstream_connected;
        n->last_seq = last_seq;
        n->last_seen_at = now;
        note_applied_locked();
        return ApplyResult::AppliedNew;
    }

    if (n->has_superseded_generation && boot_session_generation == n->superseded_generation) {
        // A delayed packet from the session immediately before the current
        // one -- rejected regardless of how its numeric generation value
        // compares to the current one (components/topology_registry's own
        // documented, bounded tradeoff: only the IMMEDIATELY PRECEDING
        // session is recognized and rejected this way, not arbitrarily old
        // ones).
        return ApplyResult::StaleSession;
    }

    // A genuinely different, non-superseded generation -- a legitimate new
    // session (e.g. the Power Base rebooted and picked a fresh
    // boot_session_generation). Accepted unconditionally, same as the
    // firmware's own policy: generation is an OPAQUE per-boot token, never
    // assumed to be numerically increasing.
    n->superseded_generation = n->boot_session_generation;
    n->has_superseded_generation = true;
    n->boot_session_generation = boot_session_generation;
    n->parent_uuid = parent_uuid;
    n->online = online;
    n->depth = depth;
    n->role = role;
    n->sensor_bitmap = sensor_bitmap;
    n->downstream_connected = downstream_connected;
    n->last_seq = last_seq;
    n->last_seen_at = now;
    note_applied_locked();
    return ApplyResult::AppliedNew;
}

ApplyResult Registry::apply_sample(const Uuid &power_base_uuid, const Uuid &module_uuid, uint32_t sample_seq,
                                    uint32_t uptime_ms, uint16_t sample_sensor_bitmap,
                                    const std::vector<ChannelReading> &channels,
                                    std::chrono::steady_clock::time_point now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    NodeState *n = find_locked(power_base_uuid, module_uuid);
    if (n == nullptr || !n->has_topology) {
        // No topology at all for this (power_base_uuid, module_uuid) --
        // mirrors components/power_base_transport's own "/s" handler
        // rejecting a sample from a uuid it has never seen a topology
        // report for (CoAP 4.04 there; the same code here).
        return ApplyResult::TopologyUnknown;
    }
    if (!n->online) {
        return ApplyResult::PreconditionFailed;
    }
    if ((sample_sensor_bitmap & (uint16_t)~n->sensor_bitmap) != 0) {
        // The sample declares a bit the topology's own bitmap never
        // declared -- same subset check as the firmware's own handler.
        return ApplyResult::PreconditionFailed;
    }
    for (const auto &ch : channels) {
        if ((sample_sensor_bitmap & (uint16_t)(1u << ch.type)) == 0) {
            // A channel entry whose type bit is not even set in the
            // sample's OWN declared bitmap -- internally contradictory,
            // rejected the same way as a bitmap/topology mismatch.
            return ApplyResult::PreconditionFailed;
        }
    }

    // Dedup key (power_base_uuid, module_uuid, sample_seq) -- an exact
    // repeat of the last-recorded seq for this pair is a retransmit, same
    // policy as the firmware's own sensor_root_aggregator_record_sample().
    // (Unlike topology, sample sequence has no monotonicity requirement
    // here either -- mirrors sensor_root_aggregator's own "exact seq match
    // only" dedup rule, not a reordering guard; see this project's other
    // host tests for why that faithfully matches production firmware
    // behavior.)
    if (n->has_sample && n->last_sample_seq == sample_seq) {
        // Same rationale as apply_topology()'s own DuplicateExact branch:
        // still a real, just-received message proving liveness now.
        n->last_seen_at = now;
        return ApplyResult::DuplicateExact;
    }

    n->has_sample = true;
    n->last_sample_seq = sample_seq;
    n->last_sample_uptime_ms = uptime_ms;
    n->last_channels = channels;
    n->last_seen_at = now;
    note_applied_locked();
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

bool Registry::get_node(size_t index, NodeState &out, std::chrono::steady_clock::time_point now) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= nodes_.size()) {
        return false;
    }
    out = with_staleness_applied(nodes_[index], now);
    return true;
}

NodeState Registry::with_staleness_applied(NodeState n, std::chrono::steady_clock::time_point now) const
{
    if (n.online && (now - n.last_seen_at > stale_timeout_)) {
        n.online = false;
    }
    return n;
}

void Registry::note_applied_locked()
{
    generation_++;
    cv_.notify_all();
}

uint64_t Registry::generation() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

std::vector<NodeState> Registry::snapshot(std::chrono::steady_clock::time_point now) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NodeState> out;
    out.reserve(nodes_.size());
    for (const auto &n : nodes_) {
        out.push_back(with_staleness_applied(n, now));
    }
    return out;
}

bool Registry::wait_for_change(uint64_t *last_seen_generation, std::chrono::milliseconds timeout) const
{
    std::unique_lock<std::mutex> lock(mutex_);
    bool changed = cv_.wait_for(lock, timeout, [this, last_seen_generation] { return generation_ != *last_seen_generation; });
    if (changed) {
        *last_seen_generation = generation_;
    }
    return changed;
}

} // namespace ingest
