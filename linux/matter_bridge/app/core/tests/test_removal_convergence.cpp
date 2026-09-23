// Linux-side half of the "confirmed detach survives a Power Base reboot"
// regression (the firmware half is tools/test_linux_bridge_exporter_reboot_removal.c).
//
// Drives the REAL ingest::Registry (not hand-built NodeState vectors) into
// BridgeManager exactly the way chip/main.cpp's registry watcher does:
//   1. the module is online with an endpoint;
//   2. the Power Base loses the link and reboots before the tombstone is
//      delivered -- Linux hears nothing, so only its own stale timeout
//      applies: the module stays present, reachable=false, NO removal;
//   3. the rebooted Power Base re-sends the tombstone from flash:
//      apply_removal() -> remove_endpoint for the same endpoint id;
//   4. the SAME uuid is re-docked (also across a bridge process restart):
//      it comes back as a new endpoint with the SAME endpoint id and the
//      SAME uuid-derived identity, so controller-side names/rooms keyed on
//      it can be restored;
//   5. crossing: a late duplicate tombstone lands after the re-dock -- the
//      Power Base's next periodic online=true resync puts it back, so the
//      latest physical state wins.
#include <chrono>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

extern "C" {
#include "sensor_channel/sensor_channel.h"
}

#include "ingest/registry.hpp"
#include "matter_bridge/bridge_manager.hpp"
#include "matter_bridge/endpoint_store.hpp"
#include "test_check.hpp"

using matter_bridge::ApplyOutcome;
using matter_bridge::BridgeManager;
using matter_bridge::EndpointStore;
using matter_bridge::ModuleUpdate;
using matter_bridge::Uuid;

namespace {

Uuid tag_uuid(uint8_t tag)
{
    Uuid u{};
    u.fill(tag);
    return u;
}

void commit_all(BridgeManager &mgr, const std::vector<ModuleUpdate> &updates)
{
    for (const auto &u : updates) {
        ApplyOutcome o;
        o.endpoint_created = u.is_new_endpoint;
        o.endpoint_removed = u.remove_endpoint;
        o.reachable_applied = u.reachable.has_value();
        o.temperature_applied = u.temperature.has_value();
        o.humidity_applied = u.humidity.has_value();
        o.pm1_applied = u.pm1.has_value();
        o.pm25_applied = u.pm25.has_value();
        o.pm10_applied = u.pm10.has_value();
        o.illuminance_applied = u.illuminance.has_value();
        mgr.CommitApplied(u.module_uuid, u, o);
    }
}

const ModuleUpdate *find_update(const std::vector<ModuleUpdate> &updates, const Uuid &uuid)
{
    for (const auto &u : updates) {
        if (u.module_uuid == uuid) {
            return &u;
        }
    }
    return nullptr;
}

bool snapshot_has(const std::vector<ingest::NodeState> &snap, const Uuid &uuid, bool *online)
{
    for (const auto &n : snap) {
        if (n.module_uuid == uuid) {
            *online = n.online;
            return true;
        }
    }
    return false;
}

} // namespace

void RunRemovalConvergenceTests(int *failures)
{
    using namespace std::chrono;
    const uint16_t kSht45 = (uint16_t)((1u << SENSOR_CHANNEL_TEMPERATURE) | (1u << SENSOR_CHANNEL_HUMIDITY));
    const Uuid pb = tag_uuid(0xB0);
    const Uuid root = tag_uuid(0x00);
    const Uuid x = tag_uuid(0x51);

    char tmpl[] = "/tmp/mb_removal_convergence_XXXXXX";
    std::string dir = mkdtemp(tmpl);

    printf("\n-- scenario: confirmed detach not delivered before a Power Base reboot -> offline only while "
           "Linux hears nothing, removed once the reboot re-sends the tombstone, restored on re-dock --\n");

    ingest::Registry reg(milliseconds(120000), milliseconds(5000));
    auto t0 = steady_clock::now();
    uint16_t ep = 0;
    {
        EndpointStore store(dir);
        store.load();
        BridgeManager mgr(store);

        MB_CHECK(reg.apply_topology(pb, x, root, true, 1, 3, kSht45, false, 7, 1, t0) ==
                     ingest::ApplyResult::AppliedNew,
                 "X registers online");
        auto created = mgr.Reconcile(reg.snapshot(t0));
        const ModuleUpdate *c = find_update(created, x);
        MB_CHECK(c != nullptr && c->is_new_endpoint, "X gets a Matter endpoint");
        ep = c != nullptr ? c->endpoint_id : 0;
        commit_all(mgr, created);

        // Power Base link lost + reboot: no message at all for 10 minutes.
        auto later = t0 + minutes(10);
        auto stale_snap = reg.snapshot(later);
        bool online = true;
        MB_CHECK(snapshot_has(stale_snap, x, &online) && !online,
                 "without the tombstone, X is only offline (still in API/SSE snapshot)");
        auto stale_updates = mgr.Reconcile(stale_snap);
        const ModuleUpdate *s = find_update(stale_updates, x);
        MB_CHECK(s != nullptr && !s->remove_endpoint && s->reachable.has_value() && !*s->reachable,
                 "Matter only gets Reachable=false for the stale module -- never a removal");
        commit_all(mgr, stale_updates);

        // Rebooted Power Base re-sends the journaled tombstone.
        uint64_t gen = reg.generation();
        MB_CHECK(reg.apply_removal(pb, x, later) == ingest::ApplyResult::AppliedNew,
                 "the post-reboot tombstone applies");
        MB_CHECK(reg.generation() == gen + 1, "registry_generation advances (SSE clients get a new snapshot)");
        MB_CHECK(!snapshot_has(reg.snapshot(later), x, &online), "X is gone from the API/SSE snapshot");
        auto removal = mgr.Reconcile(reg.snapshot(later));
        const ModuleUpdate *r = find_update(removal, x);
        MB_CHECK(r != nullptr && r->remove_endpoint && r->endpoint_id == ep,
                 "Matter gets remove_endpoint for X's endpoint id");
        commit_all(mgr, removal);
        MB_CHECK(mgr.Reconcile(reg.snapshot(later)).empty(), "nothing left to do after the confirmed removal");
    }

    printf("\n-- the bridge process restarts, then the SAME uuid is physically re-docked --\n");
    {
        EndpointStore store(dir);
        MB_CHECK(store.load() == matter_bridge::LoadResult::Ok, "the endpoint map survives the restart");
        BridgeManager mgr(store);

        auto t1 = steady_clock::now();
        MB_CHECK(reg.apply_topology(pb, x, root, true, 1, 3, kSht45, false, 8, 1, t1) ==
                     ingest::ApplyResult::AppliedNew,
                 "the re-dock's online=true report re-approves X automatically");
        auto readd = mgr.Reconcile(reg.snapshot(t1));
        const ModuleUpdate *a = find_update(readd, x);
        MB_CHECK(a != nullptr && a->is_new_endpoint && a->endpoint_id == ep,
                 "X comes back as a new endpoint with the SAME endpoint id (same uuid-derived UniqueID)");
        commit_all(mgr, readd);

        printf("\n-- crossing: a late duplicate tombstone lands after the re-dock; the next periodic online=true "
               "resync wins --\n");
        MB_CHECK(reg.apply_removal(pb, x, t1) == ingest::ApplyResult::AppliedNew, "the late tombstone applies");
        auto gone = mgr.Reconcile(reg.snapshot(t1));
        const ModuleUpdate *g = find_update(gone, x);
        MB_CHECK(g != nullptr && g->remove_endpoint, "X is briefly removed");
        commit_all(mgr, gone);

        MB_CHECK(reg.apply_topology(pb, x, root, true, 1, 3, kSht45, false, 8, 2, t1) ==
                     ingest::ApplyResult::AppliedNew,
                 "the Power Base's next resync (X still docked, online=true) re-approves it");
        auto back = mgr.Reconcile(reg.snapshot(t1));
        const ModuleUpdate *b = find_update(back, x);
        MB_CHECK(b != nullptr && b->is_new_endpoint && b->endpoint_id == ep,
                 "X is back on the same endpoint id -- the latest physical state (docked) wins");
        commit_all(mgr, back);
        bool online = false;
        MB_CHECK(snapshot_has(reg.snapshot(t1), x, &online) && online, "X is online in the API/SSE snapshot");
    }

    std::string cmd = "rm -rf '" + dir + "'";
    (void)std::system(cmd.c_str());
}
