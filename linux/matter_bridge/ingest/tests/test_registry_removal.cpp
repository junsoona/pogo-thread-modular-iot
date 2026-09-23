// Covers ingest::Registry::apply_removal() and apply_topology()'s removed-
// list guard (this follow-up: POGO auto-registration/removal lifecycle --
// see registry.hpp's own top comment: "online=false와 removed를 혼용하지
// 마라"). Pure logic test (no sockets), linked against the REAL,
// unmodified registry.cpp -- see test_registry_and_session.cpp's own top
// comment for why nothing here is hand-mirrored.
//
// Covers: a removal erases the node from nodes_/snapshot() immediately,
// bumps generation() exactly once; a duplicate removal is idempotent (no
// second erase, no second generation() bump); a stray online=false
// topology report for a removed module_uuid is rejected outright and never
// resurrects it; an online=true report is treated as genuine re-approval
// (clears the removed marker, re-inserts as a fresh node, generation()
// advances again); apply_sample() for a removed module_uuid is rejected
// (TopologyUnknown, since it is not in nodes_ at all); wait_for_change()
// wakes on a removal exactly like any other real change.
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

using ingest::ApplyResult;
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
    printf("\n-- scenario: apply_removal() erases an active node immediately and excludes it from every reader --\n");
    {
        Registry reg;
        auto pb = make_uuid(0xB0);
        auto mod = make_uuid(0x01);
        reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1);
        CHECK(reg.node_count() == 1, "the node is active before removal");
        uint64_t gen_before = reg.generation();

        auto r = reg.apply_removal(pb, mod);
        CHECK(r == ApplyResult::AppliedNew, "the first removal for this module_uuid is AppliedNew");
        CHECK(reg.generation() == gen_before + 1, "generation() advances exactly once for the removal");
        CHECK(reg.node_count() == 0, "the node is gone from node_count()");
        auto snap = reg.snapshot();
        CHECK(snap.empty(), "snapshot() excludes the removed module -- no separate 'removed' flag for a consumer "
                              "(HTTP/SSE) to check");
    }

    printf("\n-- scenario: a duplicate removal is idempotent -- no second erase, no second generation() bump --\n");
    {
        Registry reg;
        auto pb = make_uuid(0xB0);
        auto mod = make_uuid(0x01);
        reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1);
        reg.apply_removal(pb, mod);
        uint64_t gen_after_first_removal = reg.generation();

        auto r2 = reg.apply_removal(pb, mod);
        CHECK(r2 == ApplyResult::DuplicateExact, "a second removal for the same module_uuid is DuplicateExact");
        CHECK(reg.generation() == gen_after_first_removal, "generation() does not advance on a duplicate removal");

        auto r3 = reg.apply_removal(pb, mod);
        CHECK(r3 == ApplyResult::DuplicateExact, "a THIRD removal is still idempotent");
        CHECK(reg.generation() == gen_after_first_removal, "still no further generation() advance");
    }

    printf("\n-- scenario: apply_removal() for a module_uuid Linux never even had a topology report for is still a "
           "real, generation-advancing fact (the removed list itself), not a no-op --\n");
    {
        Registry reg;
        auto pb = make_uuid(0xB0);
        auto mod = make_uuid(0x99);
        CHECK(reg.node_count() == 0, "nothing known about this module_uuid yet");
        uint64_t gen_before = reg.generation();

        auto r = reg.apply_removal(pb, mod);
        CHECK(r == ApplyResult::AppliedNew, "removing a never-seen module_uuid is still AppliedNew (recording the "
                                              "denylist fact)");
        CHECK(reg.generation() == gen_before + 1, "generation() still advances exactly once");
        CHECK(reg.node_count() == 0, "there was never a node to erase");
    }

    printf("\n-- scenario: a stray online=false topology report for a removed module_uuid is rejected outright, "
           "never resurrecting it (the firmware's own periodic resync of a long-offline entry looks exactly like "
           "this) --\n");
    {
        Registry reg;
        auto pb = make_uuid(0xB0);
        auto mod = make_uuid(0x01);
        reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1);
        reg.apply_removal(pb, mod);
        uint64_t gen_after_removal = reg.generation();

        auto r = reg.apply_topology(pb, mod, make_uuid(0x00), false, 1, 3, 0x0002, false, 1, 2);
        CHECK(r == ApplyResult::PreconditionFailed, "an online=false report for a removed module_uuid is rejected");
        CHECK(reg.node_count() == 0, "still not re-inserted into nodes_");
        CHECK(reg.generation() == gen_after_removal, "generation() is unaffected by the rejected report");
    }

    printf("\n-- scenario: an online=true topology report for a removed module_uuid IS treated as genuine "
           "re-approval -- re-registers as a fresh node and generation() advances again --\n");
    {
        Registry reg;
        auto pb = make_uuid(0xB0);
        auto mod = make_uuid(0x01);
        reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1);
        reg.apply_removal(pb, mod);
        uint64_t gen_after_removal = reg.generation();

        auto r = reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 2, 1);
        CHECK(r == ApplyResult::AppliedNew, "a fresh online=true report re-approves the module_uuid");
        CHECK(reg.generation() == gen_after_removal + 1, "generation() advances again for the re-approval");
        CHECK(reg.node_count() == 1, "the module is active again");

        auto snap = reg.snapshot();
        CHECK(snap.size() == 1 && snap[0].online == true, "the re-approved module appears online in snapshot()");

        // And it is no longer denylisted -- a later ordinary offline report
        // works exactly as it always would for any other active node.
        auto r2 = reg.apply_topology(pb, mod, make_uuid(0x00), false, 1, 3, 0x0002, false, 2, 2);
        CHECK(r2 == ApplyResult::AppliedNew, "after re-approval, this module_uuid is a completely ordinary node "
                                               "again -- an explicit offline report applies normally");
        CHECK(reg.node_count() == 1, "an ordinary offline transition never erases the node -- only apply_removal() "
                                       "does");
    }

    printf("\n-- scenario: apply_sample() for a removed module_uuid is rejected (TopologyUnknown -- it is not in "
           "nodes_ at all), even before any topology report ever arrives for it --\n");
    {
        Registry reg;
        auto pb = make_uuid(0xB0);
        auto mod = make_uuid(0x01);
        reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1);
        reg.apply_removal(pb, mod);

        std::vector<ingest::ChannelReading> channels = {ingest::ChannelReading{1, 0, 0, 100}};
        auto r = reg.apply_sample(pb, mod, 1, 1000, 0x0002, channels);
        CHECK(r == ApplyResult::TopologyUnknown, "a sample for a removed module_uuid is rejected -- it is not "
                                                   "'known but offline', it simply is not in nodes_ at all");
    }

    printf("\n-- scenario: removal only ever affects the targeted module_uuid -- an unrelated node is untouched --\n");
    {
        Registry reg;
        auto pb = make_uuid(0xB0);
        auto mod_a = make_uuid(0x01);
        auto mod_b = make_uuid(0x02);
        reg.apply_topology(pb, mod_a, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1);
        reg.apply_topology(pb, mod_b, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1);
        reg.apply_removal(pb, mod_a);

        CHECK(reg.node_count() == 1, "only module A was erased");
        auto snap = reg.snapshot();
        CHECK(snap.size() == 1 && snap[0].module_uuid == mod_b, "module B is completely unaffected");

        std::vector<ingest::ChannelReading> channels = {ingest::ChannelReading{1, 0, 0, 100}};
        auto r = reg.apply_sample(pb, mod_b, 1, 1000, 0x0002, channels);
        CHECK(r == ApplyResult::AppliedNew, "module B's own sample still applies normally");
    }

    printf("\n-- scenario: wait_for_change() wakes on a removal exactly like any other real registry change --\n");
    {
        using namespace std::chrono;
        Registry reg;
        auto pb = make_uuid(0xB0);
        auto mod = make_uuid(0x01);
        reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1);

        uint64_t last_seen = reg.generation();
        std::atomic<bool> woke{false};
        std::thread waiter([&] {
            bool changed = reg.wait_for_change(&last_seen, seconds(5));
            woke.store(changed);
        });

        reg.apply_removal(pb, mod);
        waiter.join();

        CHECK(woke.load(), "wait_for_change() woke from the removal");
    }

    printf("\n-- scenario: more than kMaxNodes distinct removals over the process lifetime -- every one still "
           "applies (the bounded removed list evicts its oldest marker instead of answering 5.03 forever, which "
           "would leave the module on Linux while the Power Base retries the tombstone indefinitely) --\n");
    {
        Registry reg;
        auto pb = make_uuid(0xB0);
        bool all_applied = true;
        for (size_t i = 0; i < Registry::kMaxNodes; i++) {
            Uuid old{};
            old[0] = 0xEE;
            old[7] = static_cast<uint8_t>(i);
            all_applied = all_applied && reg.apply_removal(pb, old) == ApplyResult::AppliedNew;
        }
        CHECK(all_applied, "the first kMaxNodes removals apply");

        auto mod = make_uuid(0x42);
        reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1);
        CHECK(reg.node_count() == 1, "a live module is present");
        CHECK(reg.apply_removal(pb, mod) == ApplyResult::AppliedNew,
              "its removal still applies with the removed list already full");
        CHECK(reg.node_count() == 0, "and it is gone from every reader");
        CHECK(reg.apply_removal(pb, mod) == ApplyResult::DuplicateExact, "a retry of it is still idempotent");
        CHECK(reg.apply_topology(pb, mod, make_uuid(0x00), false, 1, 3, 0x0002, false, 1, 2) ==
                  ApplyResult::PreconditionFailed,
              "a stray offline report for it is still rejected");
    }

    std::printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
