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

    std::printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
