// Power Base management milestone (docs/power-base-management.md, section 7):
// what the Matter side does when a Power Base is unenrolled.
//
// Drives the REAL ingest::Registry into BridgeManager the way chip/main.cpp's
// registry watcher does. The hub's management code, on a verified completion
// proof, blocks the Base and calls Registry::remove_power_base(); from there
// on it is the existing Reconcile() path:
//   1. the modules registered under the removed Base get remove_endpoint;
//   2. a module that had moved to another Base, and that Base's own modules,
//      keep their endpoints -- nothing is sent for them;
//   3. late data from the removed Base creates no endpoint again;
//   4. the endpoint id map is not edited: when the same module is seen again
//      (the Base was enrolled again) it gets the SAME endpoint id.
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

Uuid tagged(uint8_t tag)
{
    Uuid u{};
    u.fill(tag);
    return u;
}

void commit(BridgeManager &mgr, const std::vector<ModuleUpdate> &updates)
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

const ModuleUpdate *find(const std::vector<ModuleUpdate> &updates, const Uuid &uuid)
{
    for (const auto &u : updates) {
        if (u.module_uuid == uuid) {
            return &u;
        }
    }
    return nullptr;
}

} // namespace

void RunPowerBaseRemovalTests(int *failures)
{
    using namespace std::chrono;
    const uint16_t kSht45 = (uint16_t)((1u << SENSOR_CHANNEL_TEMPERATURE) | (1u << SENSOR_CHANNEL_HUMIDITY));
    const Uuid base_a = tagged(0xA0), base_b = tagged(0xB0);
    const Uuid x = tagged(0x51), y = tagged(0x52), z = tagged(0x53);

    char tmpl[] = "/tmp/mb_power_base_removal_XXXXXX";
    std::string dir = mkdtemp(tmpl);

    printf("\n-- scenario: a Power Base is unenrolled -> exactly its modules lose their Matter endpoints --\n");
    ingest::Registry reg(milliseconds(120000), milliseconds(5000));
    auto t0 = steady_clock::now();
    uint16_t ep_x = 0, ep_y = 0, ep_z = 0;
    {
        EndpointStore store(dir);
        store.load();
        BridgeManager mgr(store);

        reg.apply_heartbeat(base_a, 1, 2, t0);
        reg.apply_heartbeat(base_b, 1, 1, t0);
        reg.apply_topology(base_a, x, base_a, true, 1, 3, kSht45, false, 7, 1, t0);
        // Y was docked on A and has since moved to B: A holds a stale offline
        // entry for it, B the live one.
        reg.apply_topology(base_a, y, base_a, false, 1, 3, kSht45, false, 7, 2, t0);
        reg.apply_topology(base_b, y, base_b, true, 1, 3, kSht45, false, 9, 1, t0);
        reg.apply_topology(base_b, z, base_b, true, 1, 3, kSht45, false, 9, 1, t0);
        auto created = mgr.Reconcile(reg.snapshot(t0));
        const ModuleUpdate *cx = find(created, x), *cy = find(created, y), *cz = find(created, z);
        MB_CHECK(cx != nullptr && cy != nullptr && cz != nullptr && cx->is_new_endpoint && cy->is_new_endpoint &&
                     cz->is_new_endpoint,
                 "X (on A), Y (moved to B) and Z (on B) have Matter endpoints");
        ep_x = cx != nullptr ? cx->endpoint_id : 0;
        ep_y = cy != nullptr ? cy->endpoint_id : 0;
        ep_z = cz != nullptr ? cz->endpoint_id : 0;
        commit(mgr, created);
        MB_CHECK(mgr.Reconcile(reg.snapshot(t0)).empty(), "(settled)");

        // What the hub does when A's completion proof verifies.
        reg.set_power_base_blocked(base_a, true);
        std::vector<Uuid> erased = reg.remove_power_base(base_a);
        MB_CHECK(erased.size() == 2, "A's two registry nodes (X, and its stale entry for Y) are erased");
        auto removal = mgr.Reconcile(reg.snapshot(t0));
        const ModuleUpdate *rx = find(removal, x);
        MB_CHECK(rx != nullptr && rx->remove_endpoint && rx->endpoint_id == ep_x,
                 "Matter gets remove_endpoint for X's endpoint id");
        const ModuleUpdate *ry = find(removal, y), *rz = find(removal, z);
        MB_CHECK((ry == nullptr || !ry->remove_endpoint) && (rz == nullptr || !rz->remove_endpoint),
                 "Y (now on B) and Z are not removed");
        commit(mgr, removal);
        MB_CHECK(mgr.Reconcile(reg.snapshot(t0)).empty(), "nothing left to do after the removal");

        // Late data from the removed Base.
        MB_CHECK(reg.apply_topology(base_a, x, base_a, true, 1, 3, kSht45, false, 7, 5, t0) ==
                         ingest::ApplyResult::Blocked &&
                     reg.apply_heartbeat(base_a, 2, 1, t0) == ingest::ApplyResult::Blocked,
                 "a late topology report and heartbeat from A are refused");
        MB_CHECK(mgr.Reconcile(reg.snapshot(t0)).empty(), "so no endpoint is created again for X");

        // The other Base goes on as before.
        reg.apply_topology(base_b, z, base_b, true, 1, 3, kSht45, false, 9, 2, t0);
        auto other = mgr.Reconcile(reg.snapshot(t0));
        MB_CHECK(find(other, x) == nullptr && (find(other, z) == nullptr || !find(other, z)->remove_endpoint),
                 "B's modules keep being bridged");
        commit(mgr, other);
    }

    printf("\n-- the Base is enrolled again (also across a bridge restart): the same module, the same endpoint id --\n");
    {
        EndpointStore store(dir);
        MB_CHECK(store.load() != matter_bridge::LoadResult::Corrupt, "endpoint_map.bin loads");
        MB_CHECK(store.lookup(x).has_value() && *store.lookup(x) == ep_x && store.lookup(y).has_value() &&
                     *store.lookup(y) == ep_y && store.lookup(z).has_value() && *store.lookup(z) == ep_z,
                 "the removal edited nothing in the endpoint id map: X, Y and Z keep their ids");
        BridgeManager mgr(store);
        ingest::Registry fresh(milliseconds(120000), milliseconds(5000));
        fresh.set_power_base_blocked(base_a, true); // what the hub restores from its state file
        MB_CHECK(fresh.apply_topology(base_a, x, base_a, true, 1, 3, kSht45, false, 8, 1, t0) ==
                     ingest::ApplyResult::Blocked,
                 "after the restart A is still blocked");
        fresh.set_power_base_blocked(base_a, false); // a new authenticated enrollment generation became active
        MB_CHECK(fresh.apply_topology(base_a, x, base_a, true, 1, 3, kSht45, false, 8, 1, t0) ==
                     ingest::ApplyResult::AppliedNew,
                 "once a new enrollment is active, X registers again");
        auto back = mgr.Reconcile(fresh.snapshot(t0));
        const ModuleUpdate *bx = find(back, x);
        MB_CHECK(bx != nullptr && bx->is_new_endpoint && bx->endpoint_id == ep_x,
                 "X comes back as a new endpoint with the SAME endpoint id");
    }

    printf("\n-- an offline Power Base is taken out of the hub's list (section 13): the same Matter path --\n");
    {
        // What the hub's ForgetPowerBase() does to the Registry, in one step: only for a Base that
        // is not heard from, and only after its commit (the state file write) succeeded.
        using Outcome = ingest::Registry::OfflineRemoval;
        const Uuid base_c = tagged(0xC0);
        const Uuid w = tagged(0x54);
        EndpointStore store(dir);
        store.load();
        BridgeManager mgr(store);
        ingest::Registry live(milliseconds(120000), milliseconds(5000));
        live.apply_heartbeat(base_c, 1, 1, t0);
        live.apply_heartbeat(base_b, 1, 1, t0);
        live.apply_topology(base_c, w, base_c, true, 1, 3, kSht45, false, 3, 1, t0);
        live.apply_topology(base_b, z, base_b, true, 1, 3, kSht45, false, 9, 1, t0);
        auto created = mgr.Reconcile(live.snapshot(t0));
        const ModuleUpdate *cw = find(created, w);
        const uint16_t ep_w = cw != nullptr ? cw->endpoint_id : 0;
        MB_CHECK(cw != nullptr && cw->is_new_endpoint && find(created, z) != nullptr, "(W on C and Z on B have Matter endpoints)");
        commit(mgr, created);

        int commits = 0;
        MB_CHECK(live.block_and_remove_power_base_if_offline(base_c, [&]() { commits++; return true; }, nullptr, t0) ==
                         Outcome::Online &&
                     commits == 0 && mgr.Reconcile(live.snapshot(t0)).empty(),
                 "while C is heard from nothing is removed: W keeps its endpoint");
        // Ten minutes of silence. A refused commit (the state file could not be written) removes nothing.
        const auto later = t0 + minutes(10);
        MB_CHECK(live.block_and_remove_power_base_if_offline(base_c, [&]() { commits++; return false; }, nullptr, later) ==
                         Outcome::CommitFailed &&
                     commits == 1 && !live.is_power_base_blocked(base_c),
                 "offline, but the removal could not be stored: C is not blocked and its node is still there");
        auto unreachable = mgr.Reconcile(live.snapshot(later));
        MB_CHECK(find(unreachable, w) != nullptr && !find(unreachable, w)->remove_endpoint,
                 "Matter only learns that W is unreachable -- its endpoint is not removed");
        commit(mgr, unreachable);

        std::vector<Uuid> erased;
        MB_CHECK(live.block_and_remove_power_base_if_offline(base_c, [&]() { commits++; return true; }, &erased, later) ==
                         Outcome::Removed &&
                     erased == std::vector<Uuid>{w},
                 "offline and stored: C is taken out with its module");
        auto removal = mgr.Reconcile(live.snapshot(later));
        const ModuleUpdate *rw = find(removal, w);
        MB_CHECK(rw != nullptr && rw->remove_endpoint && rw->endpoint_id == ep_w, "Matter gets remove_endpoint for W's endpoint id");
        MB_CHECK(find(removal, z) == nullptr || !find(removal, z)->remove_endpoint, "Z on the other Base is not removed");
        commit(mgr, removal);
        auto settled = mgr.Reconcile(live.snapshot(later));
        MB_CHECK(find(settled, w) == nullptr, "after that nothing is left to do for W: its endpoint slot is free");
        MB_CHECK(live.apply_topology(base_c, w, base_c, true, 1, 3, kSht45, false, 3, 9, later) == ingest::ApplyResult::Blocked &&
                     find(mgr.Reconcile(live.snapshot(later)), w) == nullptr,
                 "C's late report is refused and creates no endpoint again");

        // Registered again: the block ends, and the same module gets the same endpoint id.
        live.set_power_base_blocked(base_c, false);
        MB_CHECK(live.apply_topology(base_c, w, base_c, true, 1, 3, kSht45, false, 4, 1, later) == ingest::ApplyResult::AppliedNew,
                 "(a new authenticated enrollment of C became active; W reports again)");
        auto back = mgr.Reconcile(live.snapshot(later));
        const ModuleUpdate *bw = find(back, w);
        MB_CHECK(bw != nullptr && bw->is_new_endpoint && bw->endpoint_id == ep_w && store.lookup(w).has_value() &&
                     *store.lookup(w) == ep_w,
                 "W comes back as a new endpoint with the SAME endpoint id: the id map was not edited by the list removal");
    }

    std::string cmd = "rm -rf '" + dir + "'";
    (void)std::system(cmd.c_str());
}
