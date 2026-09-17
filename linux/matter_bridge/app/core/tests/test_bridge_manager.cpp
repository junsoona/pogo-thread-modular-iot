// Covers this repo's test list items 4-6, 8-9 (online/offline -> Reachable,
// duplicate-sample dedup producing no redundant attribute update, endpoint
// identity surviving a detach/reattach, distinct uuids getting distinct
// endpoints, and the two-Power-Base-online-at-once conflict) against the
// production matter_bridge::BridgeManager -- see
// include/matter_bridge/bridge_manager.hpp. Builds ingest::NodeState values
// directly (no sockets, no wire pack/unpack) since BridgeManager only ever
// consumes an already-decoded ingest::Registry::snapshot().
//
// This pass (transport-load reduction, Linux Registry -> Matter):
// Reconcile() no longer commits what it computes into BridgeManager's own
// dedup state by itself -- that now only happens via an explicit
// CommitApplied() call once a caller has ACTUALLY applied the update to the
// real Matter SDK (see bridge_manager.hpp's own top comment for why). Since
// this test drives BridgeManager directly, with no real Matter SDK or
// Matter event-loop thread in the loop at all, every scenario below that
// depends on a PRIOR update having taken effect calls the commit_all()
// helper right after Reconcile() to simulate "every field of that update
// was applied successfully" -- i.e. it reproduces the exact same observable
// sequence the old eager-commit Reconcile() used to produce, but now via
// the two real, separate production calls a real ScheduleWork() callback
// would make. Scenarios that specifically exercise a FAILED/uncommitted
// apply are new this pass (see "a failed apply leaves the latest state
// retryable" below) and could not have been expressed against the old
// single-call design at all.
#include "matter_bridge/bridge_manager.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
#include "sensor_channel/sensor_channel.h"
}

#include "test_check.hpp"

using matter_bridge::ApplyOutcome;
using matter_bridge::BridgeManager;
using matter_bridge::EndpointStore;
using matter_bridge::Measurement;
using matter_bridge::ModuleUpdate;
using matter_bridge::Uuid;

namespace {

Uuid make_uuid(uint8_t tag)
{
    Uuid u{};
    u.fill(tag);
    return u;
}

// Simulates a fully-successful real-Matter-SDK application of `u` (every
// field it carries actually reached Matter) and commits it -- see this
// file's own top comment.
void commit_success(BridgeManager &mgr, const ModuleUpdate &u)
{
    ApplyOutcome outcome;
    outcome.endpoint_created = u.is_new_endpoint;
    outcome.reachable_applied = u.reachable.has_value();
    outcome.temperature_applied = u.temperature.has_value();
    outcome.humidity_applied = u.humidity.has_value();
    outcome.pm1_applied = u.pm1.has_value();
    outcome.pm25_applied = u.pm25.has_value();
    outcome.pm10_applied = u.pm10.has_value();
    mgr.CommitApplied(u.module_uuid, u, outcome);
}

// Same as commit_success(), applied to every update in `updates`.
void commit_all_success(BridgeManager &mgr, const std::vector<ModuleUpdate> &updates)
{
    for (const auto &u : updates) {
        commit_success(mgr, u);
    }
}

std::string make_temp_dir()
{
    char tmpl[] = "/tmp/mb_bridge_manager_test_XXXXXX";
    char *dir = mkdtemp(tmpl);
    return std::string(dir);
}

ingest::NodeState make_node(uint8_t pb_tag, uint8_t mod_tag, bool online, uint16_t sensor_bitmap, bool has_sample,
                             std::vector<ingest::ChannelReading> channels = {})
{
    ingest::NodeState n;
    n.power_base_uuid = make_uuid(pb_tag);
    n.module_uuid = make_uuid(mod_tag);
    n.has_topology = true;
    n.online = online;
    n.sensor_bitmap = sensor_bitmap;
    n.has_sample = has_sample;
    n.last_channels = std::move(channels);
    return n;
}

ingest::ChannelReading temp_channel(int32_t milli_value, uint8_t status = SENSOR_CHANNEL_STATUS_OK)
{
    ingest::ChannelReading ch;
    ch.type = SENSOR_CHANNEL_TEMPERATURE;
    ch.scale = SENSOR_SCALE_MILLI;
    ch.status = status;
    ch.value = milli_value;
    return ch;
}

uint16_t sht45_bitmap()
{
    return (uint16_t)((1u << SENSOR_CHANNEL_TEMPERATURE) | (1u << SENSOR_CHANNEL_HUMIDITY));
}

} // namespace

void RunBridgeManagerTests(int *failures)
{
    printf("\n-- scenario: a brand-new online module with a sample produces exactly one ModuleUpdate, marked as a "
           "new endpoint with the right capability flags, reachable=true, and the converted temperature --\n");
    {
        std::string dir = make_temp_dir();
        EndpointStore store(dir);
        store.load();
        BridgeManager mgr(store);

        auto node = make_node(0xB0, 0x01, true, sht45_bitmap(), true, {temp_channel(21456)});
        auto updates = mgr.Reconcile({node});

        MB_CHECK(updates.size() == 1, "a single new online module produces exactly one ModuleUpdate");
        const ModuleUpdate &u = updates[0];
        MB_CHECK(u.is_new_endpoint, "the first-ever update for a module_uuid is marked as a new endpoint");
        MB_CHECK(u.has_temperature_channel && u.has_humidity_channel, "capability flags reflect the topology's own "
                                                                        "sensor_bitmap (SHT45: temp+humidity)");
        MB_CHECK(!u.has_pm1_channel && !u.has_pm25_channel && !u.has_pm10_channel, "capability flags never claim a "
                                                                                    "channel this module doesn't have");
        MB_CHECK(u.reachable.has_value() && *u.reachable == true, "a new online module's initial Reachable is true");
        MB_CHECK(u.temperature.has_value() && u.temperature->state == Measurement::Valid && u.temperature->value == 2146,
                 "the new module's initial temperature is the converted MeasuredValue");
        MB_CHECK(!u.humidity.has_value(), "a channel with no SAMPLE data yet (no humidity reading in this sample) "
                                           "produces no value push for it -- only its capability flag is set");
    }

    printf("\n-- scenario: reconciling the SAME unchanged snapshot again produces NO update at all (this repo's "
           "test list item 5: duplicate sample data must never cause a redundant attribute push) --\n");
    {
        std::string dir = make_temp_dir();
        EndpointStore store(dir);
        store.load();
        BridgeManager mgr(store);

        auto node = make_node(0xB0, 0x01, true, sht45_bitmap(), true, {temp_channel(21456)});
        auto first = mgr.Reconcile({node});
        MB_CHECK(first.size() == 1, "the first reconcile produces one update");
        commit_all_success(mgr, first);

        auto second = mgr.Reconcile({node});
        MB_CHECK(second.empty(), "reconciling the identical snapshot again produces zero updates -- nothing "
                                  "actually changed since the last time Matter was told anything");
    }

    printf("\n-- scenario: only the channel that actually changed is included in the next update; endpoint id "
           "stays the same --\n");
    {
        std::string dir = make_temp_dir();
        EndpointStore store(dir);
        store.load();
        BridgeManager mgr(store);

        auto node1 = make_node(0xB0, 0x01, true, sht45_bitmap(), true, {temp_channel(21456)});
        auto first = mgr.Reconcile({node1});
        uint16_t ep = first[0].endpoint_id;
        commit_all_success(mgr, first);

        auto node2 = make_node(0xB0, 0x01, true, sht45_bitmap(), true, {temp_channel(22000)});
        auto second = mgr.Reconcile({node2});
        MB_CHECK(second.size() == 1, "a genuine temperature change produces exactly one update");
        MB_CHECK(!second[0].is_new_endpoint, "this is NOT treated as a new endpoint the second time");
        MB_CHECK(second[0].endpoint_id == ep, "the endpoint id is unchanged across updates for the same module");
        MB_CHECK(!second[0].reachable.has_value(), "Reachable did not change, so it is not re-pushed");
        MB_CHECK(second[0].temperature.has_value() && second[0].temperature->value == 2200,
                 "the new temperature value is pushed");
    }

    printf("\n-- scenario: a non-OK channel status pushes an Invalid (null) measurement, not the stale last value "
           "--\n");
    {
        std::string dir = make_temp_dir();
        EndpointStore store(dir);
        store.load();
        BridgeManager mgr(store);

        auto good = make_node(0xB0, 0x01, true, sht45_bitmap(), true, {temp_channel(21456)});
        commit_all_success(mgr, mgr.Reconcile({good}));

        auto bad = make_node(0xB0, 0x01, true, sht45_bitmap(), true, {temp_channel(21456, SENSOR_CHANNEL_STATUS_CRC_ERROR)});
        auto updates = mgr.Reconcile({bad});
        MB_CHECK(updates.size() == 1, "a status transition alone (even with an unchanged raw value) is a real change");
        MB_CHECK(updates[0].temperature.has_value() && updates[0].temperature->state == Measurement::Invalid,
                 "the pushed temperature is Invalid (Matter null), never the last-known 21.456 degC value");
    }

    printf("\n-- scenario: offline -> Reachable=false, no channel values are re-pushed; reattach with unchanged "
           "data restores Reachable=true without resending unchanged values, keeping the SAME endpoint id --\n");
    {
        std::string dir = make_temp_dir();
        EndpointStore store(dir);
        store.load();
        BridgeManager mgr(store);

        auto online = make_node(0xB0, 0x01, true, sht45_bitmap(), true, {temp_channel(21456)});
        auto first = mgr.Reconcile({online});
        uint16_t ep = first[0].endpoint_id;
        commit_all_success(mgr, first);

        auto offline = make_node(0xB0, 0x01, false, sht45_bitmap(), true, {temp_channel(21456)});
        auto detach_update = mgr.Reconcile({offline});
        MB_CHECK(detach_update.size() == 1, "going offline produces exactly one update");
        MB_CHECK(detach_update[0].reachable.has_value() && *detach_update[0].reachable == false,
                 "Reachable flips to false on detach");
        MB_CHECK(!detach_update[0].temperature.has_value(), "no channel value is pushed while offline (the "
                                                              "endpoint's last-known reading is simply left alone, "
                                                              "per docs/matter-bridge.md)");
        MB_CHECK(detach_update[0].endpoint_id == ep, "the endpoint id survives a detach");
        commit_all_success(mgr, detach_update);

        auto reattach = make_node(0xB0, 0x01, true, sht45_bitmap(), true, {temp_channel(21456)});
        auto reattach_update = mgr.Reconcile({reattach});
        MB_CHECK(reattach_update.size() == 1, "reattaching (coming back online) produces exactly one update");
        MB_CHECK(reattach_update[0].reachable.has_value() && *reattach_update[0].reachable == true,
                 "Reachable flips back to true on reattach");
        MB_CHECK(!reattach_update[0].temperature.has_value(), "the SAME temperature value seen before detach is "
                                                                "not redundantly re-pushed on reattach");
        MB_CHECK(reattach_update[0].endpoint_id == ep, "the endpoint id survives a full detach/reattach cycle "
                                                        "(this repo's test list item 6)");
    }

    printf("\n-- scenario: a module with only an unbridged channel (BH1750 illuminance) never gets a Matter "
           "endpoint at all in this MVP (docs/matter-bridge.md section 3.1) --\n");
    {
        std::string dir = make_temp_dir();
        EndpointStore store(dir);
        store.load();
        BridgeManager mgr(store);

        uint16_t illuminance_bitmap = (uint16_t)(1u << SENSOR_CHANNEL_ILLUMINANCE);
        auto node = make_node(0xB0, 0x01, true, illuminance_bitmap, false);
        auto updates = mgr.Reconcile({node});
        MB_CHECK(updates.empty(), "a BH1750-only module produces no ModuleUpdate -- no Matter endpoint is ever "
                                   "created for it in this MVP");
        MB_CHECK(store.size() == 0, "no endpoint id is consumed for a module this bridge will never expose");
    }

    printf("\n-- scenario: two different module_uuids always get two different endpoint ids (this repo's test "
           "list item 8) --\n");
    {
        std::string dir = make_temp_dir();
        EndpointStore store(dir);
        store.load();
        BridgeManager mgr(store);

        auto node_a = make_node(0xB0, 0xAA, true, sht45_bitmap(), true, {temp_channel(1000)});
        auto node_b = make_node(0xB0, 0xBB, true, sht45_bitmap(), true, {temp_channel(2000)});
        auto updates = mgr.Reconcile({node_a, node_b});
        MB_CHECK(updates.size() == 2, "two distinct new modules produce two updates");
        MB_CHECK(updates[0].endpoint_id != updates[1].endpoint_id, "they never share an endpoint id");
    }

    printf("\n-- scenario: the SAME module_uuid online under TWO different power_base_uuids at once is a "
           "conflict -- withheld, not arbitrarily resolved (this repo's test list item 9) --\n");
    {
        std::string dir = make_temp_dir();
        EndpointStore store(dir);
        store.load();
        BridgeManager mgr(store);

        auto via_pb1 = make_node(0x01, 0x01, true, sht45_bitmap(), true, {temp_channel(1000)});
        auto via_pb2 = make_node(0x02, 0x01, true, sht45_bitmap(), true, {temp_channel(2000)});
        auto updates = mgr.Reconcile({via_pb1, via_pb2});

        MB_CHECK(updates.empty(), "a conflicted module_uuid produces NO ModuleUpdate at all this round -- its "
                                   "update is withheld, not resolved by picking either Power Base's data");
        auto conflicted = mgr.ConflictedModules();
        MB_CHECK(conflicted.size() == 1 && conflicted[0] == make_uuid(0x01),
                 "the conflicted module_uuid is recorded and observable");

        // Resolve the conflict: pb2's copy goes offline, leaving pb1 as the
        // single online source.
        auto pb1_still_online = make_node(0x01, 0x01, true, sht45_bitmap(), true, {temp_channel(1000)});
        auto pb2_now_offline = make_node(0x02, 0x01, false, sht45_bitmap(), true, {temp_channel(2000)});
        auto resolved = mgr.Reconcile({pb1_still_online, pb2_now_offline});

        MB_CHECK(mgr.ConflictedModules().empty(), "once only one source is online, the conflict clears");
        MB_CHECK(resolved.size() == 1, "resolving the conflict produces exactly one update (treated as this "
                                        "module's first trustworthy state, since nothing was ever committed to "
                                        "Matter while it was conflicted)");
        MB_CHECK(resolved[0].reachable.has_value() && *resolved[0].reachable == true,
                 "the resolved update reflects the surviving online source (pb1)");
    }

    printf("\n-- scenario (this pass): a failed apply leaves the latest state retryable -- CommitApplied() with "
           "nothing marked as applied commits NOTHING, so the next Reconcile() still sees (and re-emits) the "
           "exact same diff --\n");
    {
        std::string dir = make_temp_dir();
        EndpointStore store(dir);
        store.load();
        BridgeManager mgr(store);

        auto node = make_node(0xB0, 0x01, true, sht45_bitmap(), true, {temp_channel(21456)});
        auto first = mgr.Reconcile({node});
        MB_CHECK(first.size() == 1 && first[0].is_new_endpoint, "setup: the first update is a new endpoint");

        ApplyOutcome all_failed; // every field defaults to false -- nothing applied
        mgr.CommitApplied(first[0].module_uuid, first[0], all_failed);

        auto retry = mgr.Reconcile({node});
        MB_CHECK(retry.size() == 1 && retry[0].is_new_endpoint,
                 "since endpoint creation was never confirmed to have succeeded, the retry is STILL treated as a "
                 "new endpoint, with the same full ModuleUpdate as before -- nothing was silently dropped");
        MB_CHECK(retry[0].endpoint_id == first[0].endpoint_id,
                 "the endpoint id itself is stable across the retry (EndpointStore's own persistence is "
                 "independent of whether the Matter apply succeeded)");

        // Now simulate: the endpoint creation + Reachable succeed, but the
        // temperature setter itself fails.
        ApplyOutcome partial;
        partial.endpoint_created = true;
        partial.reachable_applied = true;
        partial.temperature_applied = false;
        mgr.CommitApplied(retry[0].module_uuid, retry[0], partial);

        auto after_partial = mgr.Reconcile({node});
        MB_CHECK(after_partial.size() == 1, "with temperature still not committed, a further reconcile still "
                                              "reports a pending change");
        MB_CHECK(!after_partial[0].is_new_endpoint, "endpoint creation itself WAS committed, so this is correctly "
                                                      "no longer treated as new");
        MB_CHECK(!after_partial[0].reachable.has_value(), "Reachable WAS committed -- not re-emitted");
        MB_CHECK(after_partial[0].temperature.has_value() && after_partial[0].temperature->value == 2146,
                 "temperature was NOT committed (the setter failed) -- it is re-emitted for retry, still carrying "
                 "the correct value");

        ApplyOutcome temp_now_ok;
        temp_now_ok.temperature_applied = true;
        mgr.CommitApplied(after_partial[0].module_uuid, after_partial[0], temp_now_ok);
        auto settled = mgr.Reconcile({node});
        MB_CHECK(settled.empty(), "once temperature is finally committed too, the module has nothing left to "
                                    "retry");
    }

    printf("\n-- scenario (this pass): TryBeginSchedule()/EndSchedule() enforce at most one in-flight Matter work "
           "item per uuid, and gate non-urgent (SAMPLE-only) scheduling to the given min_interval, while an "
           "urgent (new-endpoint/Reachable) update always bypasses the time gate --\n");
    {
        std::string dir = make_temp_dir();
        EndpointStore store(dir);
        store.load();
        BridgeManager mgr(store);

        Uuid u = make_uuid(0x77);
        auto fast_interval = std::chrono::milliseconds(60);

        MB_CHECK(mgr.TryBeginSchedule(u, /*urgent=*/false, fast_interval),
                 "a uuid never scheduled before is allowed immediately even when not urgent (this repo's own "
                 "'first valid snapshot applies without waiting' requirement)");
        MB_CHECK(!mgr.TryBeginSchedule(u, /*urgent=*/false, fast_interval),
                 "while that work item is still in flight, a second non-urgent attempt for the SAME uuid is "
                 "refused -- at most one pending Matter work item per uuid");
        MB_CHECK(!mgr.TryBeginSchedule(u, /*urgent=*/true, fast_interval),
                 "even an URGENT attempt is refused while a work item for this uuid is already in flight -- the "
                 "single-in-flight-per-uuid cap is a concurrency-safety rule, not a rate limit, and applies "
                 "regardless of urgency");

        mgr.EndSchedule(u); // the in-flight work item "finishes"
        MB_CHECK(!mgr.TryBeginSchedule(u, /*urgent=*/false, fast_interval),
                 "immediately after finishing, a NON-URGENT attempt is still refused -- less than min_interval "
                 "has elapsed since the last successful schedule");
        MB_CHECK(mgr.TryBeginSchedule(u, /*urgent=*/true, fast_interval),
                 "but an URGENT attempt (e.g. this uuid just went offline) is allowed right away -- urgent "
                 "updates are never held back by the SAMPLE rate limit");
        mgr.EndSchedule(u);

        std::this_thread::sleep_for(fast_interval + std::chrono::milliseconds(20));
        MB_CHECK(mgr.TryBeginSchedule(u, /*urgent=*/false, fast_interval),
                 "once min_interval has genuinely elapsed, a non-urgent attempt is allowed again -- a rolling "
                 "window, never a permanent stall");
        mgr.EndSchedule(u);

        Uuid other = make_uuid(0x78);
        MB_CHECK(mgr.TryBeginSchedule(other, /*urgent=*/false, fast_interval),
                 "a DIFFERENT uuid's own gate is completely independent -- never affected by uuid 0x77's own "
                 "in-flight/rate-limit state");
        mgr.EndSchedule(other);
    }

    printf("\n-- scenario (this pass): AbortSchedule() releases the in-flight gate immediately after a "
           "synchronous ScheduleWork()-equivalent failure, so the uuid is not left permanently blocked --\n");
    {
        std::string dir = make_temp_dir();
        EndpointStore store(dir);
        store.load();
        BridgeManager mgr(store);

        Uuid u = make_uuid(0x79);
        MB_CHECK(mgr.TryBeginSchedule(u, /*urgent=*/true, std::chrono::milliseconds(1)),
                 "setup: the first attempt begins scheduling");
        mgr.AbortSchedule(u); // simulates ScheduleWork() itself returning an error synchronously
        MB_CHECK(mgr.TryBeginSchedule(u, /*urgent=*/true, std::chrono::milliseconds(1)),
                 "after AbortSchedule(), the gate is open again immediately -- a failed enqueue never permanently "
                 "blocks this uuid");
        mgr.EndSchedule(u);
    }

    printf("\n-- scenario (this pass): ReconcileOne() re-derives from whatever snapshot it is given at call time "
           "-- a work item that executes against a FRESHER (now-offline) snapshot than whatever was current when "
           "it was scheduled correctly reflects the offline state, never a stale 'online' from an earlier sample "
           "--\n");
    {
        std::string dir = make_temp_dir();
        EndpointStore store(dir);
        store.load();
        BridgeManager mgr(store);

        Uuid u = make_uuid(0x7A);
        auto online_node = make_node(0xB0, 0x7A, true, sht45_bitmap(), true, {temp_channel(21000)});
        commit_all_success(mgr, mgr.Reconcile({online_node}));

        // A sample-triggered ModuleUpdate would have been computed against
        // this (now-stale) online snapshot if the caller had reused it --
        // but production code (chip/main.cpp's own ApplyUpdateOnMatterThread())
        // always re-takes registry.snapshot() at execution time instead, so
        // this test exercises exactly that: ReconcileOne() is given a
        // DIFFERENT, fresher snapshot (module went offline in the meantime)
        // at "execution time".
        auto offline_node = make_node(0xB0, 0x7A, false, sht45_bitmap(), true, {temp_channel(21000)});
        auto update = mgr.ReconcileOne({offline_node}, u);
        MB_CHECK(update.has_value(), "ReconcileOne() produces an update against the FRESH (offline) snapshot");
        MB_CHECK(update->reachable.has_value() && *update->reachable == false,
                 "the update reflects Reachable=false from the fresh snapshot -- never a stale 'online' state "
                 "from whenever the work item was originally scheduled");
        MB_CHECK(!update->temperature.has_value(), "no stale temperature value is pushed while offline either");

        // A uuid no longer present in the given snapshot at all (e.g. it
        // aged out) simply yields no update -- never a crash or a stale
        // reapplication.
        Uuid gone = make_uuid(0x7B);
        auto none = mgr.ReconcileOne({offline_node}, gone);
        MB_CHECK(!none.has_value(), "ReconcileOne() for a uuid absent from the given snapshot returns no update");
    }
}
