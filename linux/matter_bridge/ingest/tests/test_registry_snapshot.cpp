// Covers the Matter-Bridge-facing read-model additions to ingest::Registry
// (generation()/snapshot()/wait_for_change() -- see registry.hpp's own doc
// comment): a real future Matter Bridge process links this exact same
// ingest_core library and depends on these three methods to safely observe
// registry changes from a thread other than the CoAP receive thread, so
// this test lives in ingest's own host-testable CMake project (not
// linux/matter_bridge/app) even though it exists for that consumer's sake.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "ingest/registry.hpp"

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                              \
            g_failures++;                                                                                            \
        } else {                                                                                                     \
            std::printf("ok:   %s\n", msg);                                                                          \
        }                                                                                                            \
    } while (0)

using ingest::Registry;
using ingest::Uuid;

static Uuid make_uuid(uint8_t tag)
{
    Uuid u{};
    u.fill(tag);
    return u;
}

int main()
{
    printf("\n-- scenario: generation() starts at 0 and only advances on AppliedNew --\n");
    {
        Registry reg;
        CHECK(reg.generation() == 0, "a fresh registry starts at generation 0");

        auto pb = make_uuid(0xB0);
        auto mod = make_uuid(0x01);
        auto parent = make_uuid(0x00);

        auto r1 = reg.apply_topology(pb, mod, parent, true, 1, 3, 0x0002, false, 1, 1);
        CHECK(r1 == ingest::ApplyResult::AppliedNew, "first topology report is AppliedNew");
        CHECK(reg.generation() == 1, "generation advances by exactly 1 on the first AppliedNew");

        auto r2 = reg.apply_topology(pb, mod, parent, true, 1, 3, 0x0002, false, 1, 1);
        CHECK(r2 == ingest::ApplyResult::DuplicateExact, "an exact repeat is a duplicate");
        CHECK(reg.generation() == 1, "a duplicate never advances generation");

        auto r3 = reg.apply_topology(pb, mod, parent, false, 1, 3, 0x0002, false, 1, 2);
        CHECK(r3 == ingest::ApplyResult::AppliedNew, "a real change (online flips, seq advances) is AppliedNew");
        CHECK(reg.generation() == 2, "generation advances again on a genuine change");
    }

    printf("\n-- scenario: snapshot() returns a full, independent copy --\n");
    {
        Registry reg;
        auto pb = make_uuid(0xB0);
        reg.apply_topology(pb, make_uuid(0x01), make_uuid(0x00), true, 1, 3, 0x0001, false, 1, 1);
        reg.apply_topology(pb, make_uuid(0x02), make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1);

        auto snap = reg.snapshot();
        CHECK(snap.size() == 2, "snapshot() returns every currently-tracked node");

        // Mutating the registry after taking the snapshot must never affect
        // the already-taken copy (it's a real copy, not a view/reference).
        reg.apply_topology(pb, make_uuid(0x03), make_uuid(0x00), true, 1, 3, 0x0004, false, 1, 1);
        CHECK(snap.size() == 2, "a snapshot already taken is unaffected by a later registry change");
        CHECK(reg.snapshot().size() == 3, "a NEW snapshot sees the later change");
    }

    printf("\n-- scenario: view() says which generation the copy was made at --\n");
    {
        Registry reg;
        auto pb = make_uuid(0xB0);
        CHECK(reg.view().generation == 0 && ingest::RegistryView().generation == 0,
              "a copy of a fresh registry, and a view nobody copied, are at generation 0");
        reg.apply_topology(pb, make_uuid(0x01), make_uuid(0x00), true, 1, 3, 0x0001, false, 1, 1);
        auto before = reg.view();
        CHECK(before.generation == reg.generation() && before.generation == 1,
              "view() carries generation() as it was under the same lock as the copy");
        // The Base and its node are erased (what management does when a Base
        // leaves the list); a reader is then told of a change outside.
        (void)reg.remove_power_base(pb);
        reg.notify_external_change();
        auto after = reg.view();
        CHECK(before.generation < after.generation && after.generation == reg.generation(),
              "a copy made before a change has a smaller generation than one made after it");
        CHECK(before.nodes.size() == 1 && after.nodes.empty(),
              "(the earlier copy still holds the node, the later one does not)");
        reg.apply_topology(pb, make_uuid(0x01), make_uuid(0x00), true, 1, 3, 0x0001, false, 1, 1);
        CHECK(reg.apply_topology(pb, make_uuid(0x01), make_uuid(0x00), true, 1, 3, 0x0001, false, 1, 1) ==
                      ingest::ApplyResult::DuplicateExact &&
                  reg.view().generation == reg.generation(),
              "a duplicate changes neither: two copies with nothing in between are at the same generation");
    }

    printf("\n-- scenario: wait_for_change() blocks until a real change, never busy-polls, and times out cleanly "
           "when nothing changes --\n");
    {
        Registry reg;
        uint64_t last_seen = reg.generation();
        auto start = std::chrono::steady_clock::now();
        bool changed = reg.wait_for_change(&last_seen, std::chrono::milliseconds(50));
        auto elapsed = std::chrono::steady_clock::now() - start;
        CHECK(!changed, "wait_for_change() returns false when the timeout elapses with no change");
        CHECK(elapsed >= std::chrono::milliseconds(40), "wait_for_change() actually waited near the full timeout "
                                                          "(a busy-poll implementation would return almost instantly "
                                                          "-- this is a loose bound to avoid CI flakiness)");
    }

    printf("\n-- scenario: wait_for_change() wakes promptly on a real change from another thread --\n");
    {
        Registry reg;
        uint64_t last_seen = reg.generation();
        std::atomic<bool> woke{false};

        std::thread waiter([&] {
            bool changed = reg.wait_for_change(&last_seen, std::chrono::seconds(5));
            woke.store(changed);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        reg.apply_topology(make_uuid(0xB0), make_uuid(0x01), make_uuid(0x00), true, 1, 3, 0x0001, false, 1, 1);
        waiter.join();

        CHECK(woke.load(), "a change on one thread wakes a wait_for_change() call blocked on another thread");
        CHECK(last_seen == reg.generation(), "the woken waiter's last_seen_generation is updated to the new value");
    }

    printf("\n-- scenario: the Registry remembers when each module was last newly listed under a Power Base --\n");
    {
        Registry reg;
        const Uuid pb_a = make_uuid(0xA0), pb_b = make_uuid(0xB0), m1 = make_uuid(0x01), m2 = make_uuid(0x02);
        auto listing_of = [&](const Uuid &module) -> uint64_t {
            for (const ingest::ModuleListing &l : reg.view().listings) {
                if (l.module_uuid == module) {
                    return l.generation;
                }
            }
            return 0;
        };
        CHECK(reg.view().listings.empty() && reg.view().listings_dropped_through == 0, "a fresh registry has no listings");
        reg.apply_topology(pb_a, m1, make_uuid(0x00), true, 1, 3, 0x0001, false, 1, 1);
        const uint64_t first = listing_of(m1);
        CHECK(first == reg.generation() && first == 1 && reg.view().listings.size() == 1,
              "the first report of a module under a Base lists it: at the generation that report produced");
        reg.apply_topology(pb_a, m1, make_uuid(0x00), true, 1, 3, 0x0001, false, 1, 1); // duplicate
        reg.apply_topology(pb_a, m1, make_uuid(0x00), false, 1, 3, 0x0001, false, 1, 2); // a change of the same node
        reg.apply_heartbeat(pb_a, 1000, 1);
        CHECK(listing_of(m1) == first && reg.generation() > first,
              "later reports of the same node -- a duplicate, a change, a heartbeat -- do not list it anew");
        reg.apply_topology(pb_b, m1, make_uuid(0x00), true, 1, 3, 0x0001, false, 1, 1);
        const uint64_t moved = listing_of(m1);
        CHECK(moved == reg.generation() && moved > first && reg.view().listings.size() == 1,
              "its first report under ANOTHER Base lists it anew (one entry per module: the latest)");
        // Confirmed detach from Base B: the node is gone, the listing is not.
        reg.apply_removal(pb_b, m1);
        CHECK(listing_of(m1) == moved, "a confirmed detach erases the node; that the module was listed is still known");
        reg.remove_power_base(pb_a);
        bool any_node = false;
        for (const ingest::NodeState &n : reg.view().nodes) {
            any_node = any_node || n.module_uuid == m1;
        }
        CHECK(!any_node && listing_of(m1) == moved, "nor does the removal of its other Base forget it");
        // Docked onto Base B again after the detach: a new node, a new listing.
        reg.apply_topology(pb_b, m1, make_uuid(0x00), true, 1, 3, 0x0001, false, 1, 9);
        CHECK(listing_of(m1) == reg.generation() && listing_of(m1) > moved, "docked again after the detach: listed anew");
        reg.apply_topology(pb_b, m2, make_uuid(0x00), true, 1, 3, 0x0001, false, 1, 1);
        CHECK(reg.view().listings.size() == 2 && listing_of(m2) == reg.generation(), "another module has its own entry");
        // A blocked Base lists nothing.
        const Uuid pb_c = make_uuid(0xC0), m3 = make_uuid(0x03);
        reg.set_power_base_blocked(pb_c, true);
        CHECK(reg.apply_topology(pb_c, m3, make_uuid(0x00), true, 1, 3, 0x0001, false, 1, 1) == ingest::ApplyResult::Blocked &&
                  listing_of(m3) == 0 && reg.view().listings.size() == 2,
              "a report of a blocked Base is not applied and lists nothing");
    }

    printf("\n-- scenario: the table of listings is bounded, and says what it dropped --\n");
    {
        Registry reg;
        const Uuid pb = make_uuid(0xA0);
        auto module = [](unsigned i) {
            Uuid u{};
            u[0] = 0x74;
            u[6] = static_cast<uint8_t>(i >> 8);
            u[7] = static_cast<uint8_t>(i & 0xff);
            return u;
        };
        // Fill the table: batches of nodes that are detached again.
        unsigned listed = 0;
        std::vector<uint64_t> generations;
        while (listed < Registry::kMaxListings) {
            for (unsigned i = 0; i < 32 && listed < Registry::kMaxListings; i++, listed++) {
                reg.apply_topology(pb, module(listed), make_uuid(0x00), true, 1, 3, 0x0001, false, 1, 1);
                generations.push_back(reg.generation());
            }
            reg.remove_power_base(pb);
        }
        ingest::RegistryView full = reg.view();
        CHECK(full.listings.size() == Registry::kMaxListings && full.listings_dropped_through == 0 && full.nodes.empty(),
              "as many listings as the table holds: nothing was dropped yet");
        // One more module: the oldest listing goes, and the table says so.
        reg.apply_topology(pb, module(1000), make_uuid(0x00), true, 1, 3, 0x0001, false, 1, 1);
        ingest::RegistryView over = reg.view();
        bool has_oldest = false, has_newest = false, has_second = false;
        for (const ingest::ModuleListing &l : over.listings) {
            has_oldest = has_oldest || l.module_uuid == module(0);
            has_second = has_second || l.module_uuid == module(1);
            has_newest = has_newest || l.module_uuid == module(1000);
        }
        CHECK(over.listings.size() == Registry::kMaxListings && !has_oldest && has_second && has_newest,
              "one more module: the oldest listing was dropped, every other one is kept");
        CHECK(over.listings_dropped_through == generations[0],
              "and the table says through which generation it dropped listings: a module without an entry may have been "
              "listed up to then");
        // A module that has a node is not the one that goes, however old.
        Registry busy;
        busy.apply_topology(pb, module(0), make_uuid(0x00), true, 1, 3, 0x0001, false, 1, 1); // stays docked
        const Uuid other = make_uuid(0xB0);
        for (unsigned i = 1; i <= Registry::kMaxListings; i++) {
            busy.apply_topology(other, module(i), make_uuid(0x00), true, 1, 3, 0x0001, false, 1, 1);
            busy.apply_removal(other, module(i));
        }
        bool docked_kept = false;
        for (const ingest::ModuleListing &l : busy.view().listings) {
            docked_kept = docked_kept || l.module_uuid == module(0);
        }
        CHECK(docked_kept && busy.view().listings.size() == Registry::kMaxListings && busy.view().listings_dropped_through > 0,
              "the listing of a module that is docked now is kept before that of one that is gone");
    }

    printf("\n-- scenario: hold() hands out a copy and keeps the Registry still until its reader is done --\n");
    {
        Registry reg;
        const Uuid pb = make_uuid(0xA0), m1 = make_uuid(0x01), m2 = make_uuid(0x02);
        reg.apply_topology(pb, m1, make_uuid(0x00), true, 1, 3, 0x0001, false, 1, 1);
        std::atomic<bool> inside{false}, started{false}, applied{false}, applied_while_held{false};
        std::thread reporter([&] {
            for (int i = 0; i < 10000 && !inside.load(); i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            started = true;
            reg.apply_topology(pb, m2, make_uuid(0x00), true, 1, 3, 0x0001, false, 1, 1);
            applied = true;
        });
        size_t nodes_seen = 0;
        uint64_t generation_seen = 0;
        reg.hold([&](const ingest::RegistryView &view) {
            nodes_seen = view.nodes.size();
            generation_seen = view.generation;
            inside = true;
            for (int i = 0; i < 2000 && !started.load(); i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            // The reporter is running and wants the Registry: give it time.
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            applied_while_held = applied.load();
        });
        reporter.join();
        CHECK(nodes_seen == 1 && generation_seen == 1, "the reader gets the copy view() would have made at that moment");
        CHECK(started.load() && !applied_while_held.load() && applied.load(),
              "a report that arrives while the reader runs is applied only after it returned");
        CHECK(reg.view().nodes.size() == 2 && reg.view().generation == 2, "and then it is applied as usual");
        bool ran = false;
        reg.hold(nullptr);
        reg.hold([&](const ingest::RegistryView &) { ran = true; });
        CHECK(ran, "hold() can be used again (the lock was given up), and an unset reader is no error");
    }

    std::printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
