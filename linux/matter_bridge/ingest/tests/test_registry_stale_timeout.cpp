// Covers the field bug where the Linux ingest Registry's `online` value got
// stuck at true forever after a sensor module's power was cut with no
// detach message at all -- see registry.hpp's kDefaultStaleTimeout doc
// comment for the full rationale.
//
// Two parts:
//   1. Read-time correction (with_staleness_applied(), used by
//      get_node()/snapshot()): driven purely via an explicit `now`
//      (std::chrono::steady_clock::time_point) passed straight into
//      apply_topology()/apply_sample()/get_node()/snapshot(), so a "120
//      seconds elapse" scenario runs in microseconds with zero real
//      sleeping and exact millisecond-boundary precision.
//   2. ACTIVE sweep (sweep_stale_locked(), run by Registry's own
//      background sweep_thread_ on a real bounded timer): this is what
//      actually mutates stored state and drives generation()/
//      wait_for_change() the same way any other real registry change
//      does -- see registry.hpp's own top "Correction pass 2" comment.
//      This half necessarily uses a REAL (but tiny -- single/double-digit
//      millisecond) stale_timeout/sweep_interval and small real sleeps,
//      the same idiom test_registry_snapshot.cpp already uses for
//      wait_for_change()'s own thread tests ("a loose bound to avoid CI
//      flakiness").
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
    using namespace std::chrono;
    auto t0 = steady_clock::now();

    printf("\n-- scenario: online stays true while within the timeout, with no new report at all --\n");
    {
        Registry reg(milliseconds(120000));
        auto pb = make_uuid(0xB0);
        auto mod = make_uuid(0x01);
        reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1, t0);

        ingest::NodeState n;
        reg.get_node(0, n, t0 + milliseconds(119999));
        CHECK(n.online == true, "still online 1ms before the timeout, with no new message since the last report");
    }

    printf("\n-- scenario: exactly at/after the timeout, with no fresh report, online transitions to false exactly "
           "once (evaluated repeatedly) --\n");
    {
        Registry reg(milliseconds(120000));
        auto pb = make_uuid(0xB0);
        auto mod = make_uuid(0x01);
        reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1, t0);

        ingest::NodeState n;
        reg.get_node(0, n, t0 + milliseconds(120001));
        CHECK(n.online == false, "online demotes to false once the bounded timeout has elapsed since last_seen_at");

        // Re-reading well past the boundary keeps reporting false -- this is
        // a live-computed value, not a one-shot edge transition that could
        // get missed.
        reg.get_node(0, n, t0 + milliseconds(500000));
        CHECK(n.online == false, "online stays false on every later read too, not just the first one past the boundary");

        auto snap = reg.snapshot(t0 + milliseconds(500000));
        CHECK(snap.size() == 1 && snap[0].online == false, "snapshot() applies the same timeout-corrected value as get_node()");
    }

    printf("\n-- scenario: a sample received well within the timeout, but with no NEW topology report, keeps the "
           "node online (topology resync isn't the only thing that counts as liveness) --\n");
    {
        Registry reg(milliseconds(120000));
        auto pb = make_uuid(0xB0);
        auto mod = make_uuid(0x01);
        reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1, t0);

        std::vector<ingest::ChannelReading> channels = {ingest::ChannelReading{1, 0, 0, 100}};
        reg.apply_sample(pb, mod, 1, 1000, 0x0002, channels, t0 + milliseconds(100000));

        ingest::NodeState n;
        // 150s after the topology report, but only 50s after the sample --
        // still within the 120s bound of the sample's own last_seen_at.
        reg.get_node(0, n, t0 + milliseconds(150000));
        CHECK(n.online == true, "a sample received at t0+100s keeps the node online through t0+150s");
    }

    printf("\n-- scenario: an idempotent duplicate topology report (the exporter's own periodic full resync "
           "re-sending unchanged state) refreshes liveness --\n");
    {
        Registry reg(milliseconds(120000));
        auto pb = make_uuid(0xB0);
        auto mod = make_uuid(0x01);
        reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1, t0);

        // Same generation+seq -- DuplicateExact, but still a real message
        // received now, exactly what a 5s periodic full resync repeatedly
        // sends for a healthy, unchanged node.
        auto r = reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1, t0 + milliseconds(100000));
        CHECK(r == ingest::ApplyResult::DuplicateExact, "an unchanged resync is still DuplicateExact");

        ingest::NodeState n;
        reg.get_node(0, n, t0 + milliseconds(219999));
        CHECK(n.online == true, "the duplicate resync at t0+100s keeps the node online through t0+219.999s "
                                  "(within 120s of THAT report, not just the original one)");
    }

    printf("\n-- scenario: a fresh sample after the node went stale recovers online automatically, with no new "
           "topology report at all --\n");
    {
        Registry reg(milliseconds(120000));
        auto pb = make_uuid(0xB0);
        auto mod = make_uuid(0x01);
        reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1, t0);

        ingest::NodeState n;
        reg.get_node(0, n, t0 + milliseconds(200000));
        CHECK(n.online == false, "the node went stale with nothing else received");

        std::vector<ingest::ChannelReading> channels = {ingest::ChannelReading{1, 0, 0, 42}};
        auto r = reg.apply_sample(pb, mod, 1, 1000, 0x0002, channels, t0 + milliseconds(200500));
        CHECK(r == ingest::ApplyResult::AppliedNew,
              "a sample is still accepted for recovery -- apply_sample() gates on the STORED online flag (still "
              "true; only the computed/read-time value went stale), matching the firmware's own protocol contract");

        reg.get_node(0, n, t0 + milliseconds(200600));
        CHECK(n.online == true, "the fresh sample immediately recovers the computed online status");
    }

    printf("\n-- scenario: an explicit offline topology report is unaffected by the timeout machinery (unchanged "
           "pre-existing behavior) --\n");
    {
        Registry reg(milliseconds(120000));
        auto pb = make_uuid(0xB0);
        auto mod = make_uuid(0x01);
        reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1, t0);
        reg.apply_topology(pb, mod, make_uuid(0x00), false, 1, 3, 0x0002, false, 1, 2, t0 + milliseconds(1000));

        ingest::NodeState n;
        reg.get_node(0, n, t0 + milliseconds(1500));
        CHECK(n.online == false, "an explicit offline report takes effect immediately, well before any timeout");
    }

    printf("\n-- scenario: two different module_uuids under the same power_base_uuid have fully independent "
           "staleness timers --\n");
    {
        Registry reg(milliseconds(120000));
        auto pb = make_uuid(0xB0);
        auto mod_a = make_uuid(0x01);
        auto mod_b = make_uuid(0x02);
        reg.apply_topology(pb, mod_a, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1, t0);
        reg.apply_topology(pb, mod_b, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1, t0 + milliseconds(90000));

        auto snap = reg.snapshot(t0 + milliseconds(150000));
        CHECK(snap.size() == 2, "both nodes are tracked");
        bool a_online = false, b_online = false;
        for (const auto &node : snap) {
            if (node.module_uuid == mod_a) {
                a_online = node.online;
            }
            if (node.module_uuid == mod_b) {
                b_online = node.online;
            }
        }
        CHECK(a_online == false, "module A (reported at t0) is stale by t0+150s");
        CHECK(b_online == true, "module B (reported at t0+90s, only 60s of silence so far) is still online");
    }

    printf("\n-- scenario: a rejected message (StaleSession) never refreshes liveness --\n");
    {
        Registry reg(milliseconds(120000));
        auto pb = make_uuid(0xB0);
        auto mod = make_uuid(0x01);
        reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 5, t0);
        // A reordered/delayed packet in the SAME generation with a smaller
        // seq -- rejected (StaleSession) and must not count as evidence of
        // current liveness.
        auto r = reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 2, t0 + milliseconds(119000));
        CHECK(r == ingest::ApplyResult::StaleSession, "the reordered packet is rejected as before");

        ingest::NodeState n;
        reg.get_node(0, n, t0 + milliseconds(120001));
        CHECK(n.online == false, "the rejected packet did not refresh last_seen_at -- still stale by the "
                                   "ORIGINAL report's own 120s bound");
    }

    printf("\n-- scenario: a default-constructed Registry() keeps working exactly as before (existing callers never "
           "pass a timeout or a `now`) --\n");
    {
        Registry reg;
        auto pb = make_uuid(0xB0);
        auto mod = make_uuid(0x01);
        auto r = reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1);
        CHECK(r == ingest::ApplyResult::AppliedNew, "default construction and the real-clock default `now` still work");
        ingest::NodeState n;
        reg.get_node(0, n);
        CHECK(n.online == true, "immediately after being applied, a node is online under the real clock too");
    }

    // --- ACTIVE sweep scenarios (real background thread, real clock) ---
    // A tiny stale_timeout + sweep_interval keeps these fast (tens/low
    // hundreds of real milliseconds total) while still exercising the
    // genuine sweep_thread_ path end to end, not a mocked stand-in.
    const auto kTinyTimeout = milliseconds(50);
    const auto kTinySweep = milliseconds(10);
    // Generous margin over kTinyTimeout+kTinySweep to avoid CI flakiness --
    // same idiom test_registry_snapshot.cpp already uses for its own
    // thread-wakeup assertions.
    const auto kSettleWait = milliseconds(300);

    printf("\n-- scenario: the background sweep thread bumps generation() exactly once for a single node's "
           "offline transition, with NO new message at all --\n");
    {
        Registry reg(kTinyTimeout, kTinySweep);
        auto pb = make_uuid(0xB0);
        auto mod = make_uuid(0x01);
        reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1);
        CHECK(reg.generation() == 1, "the initial topology report is generation 1, as always");

        std::this_thread::sleep_for(kSettleWait);
        CHECK(reg.generation() == 2, "the background sweep thread's own offline transition bumped generation by "
                                       "exactly 1 (2 total), with no new apply_topology()/apply_sample() call at all");

        ingest::NodeState n;
        reg.get_node(0, n);
        CHECK(n.online == false, "the node is offline (stored, not just read-time-computed)");
    }

    printf("\n-- scenario: wait_for_change() wakes on an offline transition with no new registry traffic at all --\n");
    {
        Registry reg(kTinyTimeout, kTinySweep);
        auto pb = make_uuid(0xB0);
        auto mod = make_uuid(0x01);
        reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1);

        uint64_t last_seen = reg.generation();
        std::atomic<bool> woke{false};
        std::thread waiter([&] {
            bool changed = reg.wait_for_change(&last_seen, seconds(5));
            woke.store(changed);
        });

        // Deliberately NOTHING else happens here -- no apply_topology(), no
        // apply_sample() -- the only thing that can possibly wake the
        // waiter is the background sweep thread's own timeout-driven
        // offline transition.
        waiter.join();

        CHECK(woke.load(), "wait_for_change() woke purely from the sweep thread's offline transition");
        ingest::NodeState n;
        reg.get_node(0, n);
        CHECK(n.online == false, "by the time the waiter woke, the node was genuinely offline");
    }

    printf("\n-- scenario: repeated snapshot()/get_node() reads never advance generation() on their own -- only the "
           "one real offline transition does --\n");
    {
        Registry reg(kTinyTimeout, kTinySweep);
        auto pb = make_uuid(0xB0);
        auto mod = make_uuid(0x01);
        reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1);
        std::this_thread::sleep_for(kSettleWait);
        uint64_t settled_generation = reg.generation();
        CHECK(settled_generation == 2, "generation settled at 2 (1 report + 1 offline transition)");

        for (int i = 0; i < 20; i++) {
            auto snap = reg.snapshot();
            ingest::NodeState n;
            reg.get_node(0, n);
            CHECK(snap.size() == 1 && !snap[0].online && !n.online, "each repeated read still sees offline");
            std::this_thread::sleep_for(milliseconds(5));
        }
        CHECK(reg.generation() == settled_generation,
              "20 repeated snapshot()/get_node() reads across 100ms of real time never advanced generation() -- "
              "reads are side-effect-free; only sweep_stale_locked()'s own real transition bumps it, and only once");
    }

    printf("\n-- scenario: a fresh topology/sample reception after an offline transition both recovers online AND "
           "advances generation() again --\n");
    {
        Registry reg(kTinyTimeout, kTinySweep);
        auto pb = make_uuid(0xB0);
        auto mod = make_uuid(0x01);
        reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1);
        std::this_thread::sleep_for(kSettleWait);
        CHECK(reg.generation() == 2, "1 report + 1 sweep-driven offline transition");

        auto r = reg.apply_topology(pb, mod, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 2);
        CHECK(r == ingest::ApplyResult::AppliedNew, "a fresh topology report after going stale is accepted normally "
                                                       "(same generation, strictly larger seq)");
        CHECK(reg.generation() == 3, "recovery is itself a real change -- generation advances a 3rd time");

        ingest::NodeState n;
        reg.get_node(0, n);
        CHECK(n.online == true, "the node is online again immediately after the fresh report");
    }

    printf("\n-- scenario: two module_uuids' background sweep timers are fully independent in real time -- one can "
           "be swept offline while the other, reported later, is still comfortably online --\n");
    {
        // A longer timeout than kTinyTimeout, specifically so there is a
        // real window where A has crossed it but a LATER-reported B has not.
        Registry reg(milliseconds(80), kTinySweep);
        auto pb = make_uuid(0xB0);
        auto mod_a = make_uuid(0x01);
        auto mod_b = make_uuid(0x02);
        reg.apply_topology(pb, mod_a, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1);

        // Let A cross its own 80ms bound (plus sweep margin) with nothing
        // else happening, THEN report B fresh -- B's own last_seen_at
        // starts from here, not from A's.
        std::this_thread::sleep_for(milliseconds(120));
        reg.apply_topology(pb, mod_b, make_uuid(0x00), true, 1, 3, 0x0002, false, 1, 1);

        auto snap = reg.snapshot();
        bool a_online = true, b_online = false;
        for (const auto &node : snap) {
            if (node.module_uuid == mod_a) {
                a_online = node.online;
            }
            if (node.module_uuid == mod_b) {
                b_online = node.online;
            }
        }
        CHECK(a_online == false, "module A, reported once and never again, is stale after 120ms (bound=80ms)");
        CHECK(b_online == true, "module B, reported at t=120ms, is still fresh -- A going stale never affected "
                                  "B's own independent last_seen_at/timer");
    }

    std::printf("\n%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
