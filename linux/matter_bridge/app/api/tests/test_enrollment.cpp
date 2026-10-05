// EnrollmentManager against a real ingest::Registry and a fake Commissioner:
// success only on a heartbeat that reached the Registry after the session
// started, cleanup of exactly one Joiner entry on every ending, failures of
// each Commissioner step, one session at a time, per-device access, and the
// races between heartbeat, cancel, expiry and shutdown.
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ingest/registry.hpp"
#include "pogohome_api/enrollment.hpp"
#include "test_check.hpp"
#include "test_fake_commissioner.hpp"

using namespace pogohome_api;
using test_support::FakeCommissioner;

namespace {

const char kEui[] = "744dbdfffe65d357";
const char kOtherEui[] = "744dbdfffe65d358";
const char kDeviceA[] = "0123456789abcdef";
const char kDeviceB[] = "fedcba9876543210";
const char kPskd[] = "J01NME7K2";

ingest::Uuid UuidOf(const std::string &eui)
{
    ingest::Uuid u{};
    std::string normalized;
    NormalizeEui64(eui, normalized, &u);
    return u;
}

EnrollmentConfig FastConfig(std::chrono::milliseconds window = std::chrono::milliseconds(120000))
{
    EnrollmentConfig c;
    c.window = std::chrono::duration_cast<std::chrono::seconds>(window);
    c.poll_interval = std::chrono::milliseconds(10);
    c.min_joiner_timeout = std::chrono::seconds(0);
    return c;
}

// Polls until `pred` or ~3 s.
template <typename Pred> bool WaitFor(Pred pred, std::chrono::milliseconds limit = std::chrono::milliseconds(3000))
{
    auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

EnrollmentSession GetOrEmpty(EnrollmentManager &m, const std::string &device, const std::string &id)
{
    EnrollmentSession s;
    m.Get(device, id, s);
    return s;
}

bool Finished(EnrollmentManager &m, const std::string &id)
{
    return GetOrEmpty(m, kDeviceA, id).finished;
}

EnrollmentStartOutcome StartA(EnrollmentManager &m, EnrollmentSession &s, const std::string &eui = kEui)
{
    std::string pskd = kPskd;
    return m.Start(kDeviceA, eui, pskd, s);
}

} // namespace

void RunEnrollmentTests(int *failures)
{
    printf("\n-- scenario: EUI-64 normalization for the API --\n");
    {
        std::string out;
        ingest::Uuid u{};
        API_CHECK(NormalizeEui64("744DBDFFFE65D357", out, &u) && out == kEui && u[0] == 0x74 && u[7] == 0x57,
                  "uppercase hex is accepted and lowercased, with the 8 bytes");
        API_CHECK(!NormalizeEui64("*", out) && !NormalizeEui64("744dbdfffe65d35", out) &&
                      !NormalizeEui64("0000000000000000", out) && !NormalizeEui64(" 744dbdfffe65d357", out),
                  "wildcard, short, all-zero and padded values are refused");
    }

    printf("\n-- scenario: success needs a NEW heartbeat from exactly that EUI-64 --\n");
    {
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        EnrollmentManager m(registry, fake, FastConfig());
        EnrollmentSession s;
        std::string pskd = kPskd;
        API_CHECK(m.Start(kDeviceA, kEui, pskd, s) == EnrollmentStartOutcome::Started, "a session starts");
        API_CHECK(pskd.empty(), "the caller's PSKd is wiped");
        API_CHECK(s.session_id.size() == 32 && s.device_id == kDeviceA && s.eui64 == kEui && s.window_seconds == 120,
                  "it has a 128-bit id, its starting device, the EUI-64 and the window");
        API_CHECK(s.state == EnrollmentState::Preparing && !s.finished, "it starts out preparing");
        API_CHECK(WaitFor([&] { return GetOrEmpty(m, kDeviceA, s.session_id).state == EnrollmentState::WaitingForHeartbeat; }),
                  "after the Commissioner and the Joiner entry it waits for the heartbeat");
        EnrollmentSession now = GetOrEmpty(m, kDeviceA, s.session_id);
        API_CHECK(now.joiner_entry == JoinerEntry::Allowed && now.expires_in_ms > 100000, "the entry is open");
        {
            std::lock_guard<std::mutex> lock(fake->mutex);
            API_CHECK(fake->added.size() == 1 && fake->added[0] == kEui, "exactly that EUI-64 was allowed");
            API_CHECK(fake->pskds_seen.size() == 1 && fake->pskds_seen[0] == kPskd, "with the PSKd from the request");
            API_CHECK(fake->add_timeouts[0] >= 119 && fake->add_timeouts[0] <= 120,
                      "for the rest of the 120 s window, so the OTBR closes it by itself");
        }

        registry.apply_heartbeat(UuidOf(kOtherEui), 1000, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        API_CHECK(GetOrEmpty(m, kDeviceA, s.session_id).state == EnrollmentState::WaitingForHeartbeat,
                  "another Power Base's heartbeat changes nothing");
        registry.apply_topology(UuidOf(kEui), UuidOf("1111111111111111"), UuidOf(kEui), true, 1, 2, 0x0006, false, 1,
                                1);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        API_CHECK(GetOrEmpty(m, kDeviceA, s.session_id).state == EnrollmentState::WaitingForHeartbeat,
                  "a topology report from that Base is not a heartbeat");

        registry.apply_heartbeat(UuidOf(kEui), 1000, 0);
        API_CHECK(WaitFor([&] { return Finished(m, s.session_id); }), "its own heartbeat ends the session");
        now = GetOrEmpty(m, kDeviceA, s.session_id);
        API_CHECK(now.state == EnrollmentState::HeartbeatReceived && now.heartbeat_received_at_unix_ms > 0,
                  "as heartbeat_received");
        API_CHECK(now.joiner_entry == JoinerEntry::Removed && now.expires_in_ms == 0, "and its Joiner entry is removed");
        std::lock_guard<std::mutex> lock(fake->mutex);
        API_CHECK(fake->removed.size() == 1 && fake->removed[0] == kEui, "only that EUI-64's entry, once");
    }

    printf("\n-- scenario: a Base cached before the start never counts --\n");
    {
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        EnrollmentManager m(registry, fake, FastConfig());
        // Heard from long ago: known to the Registry, now offline.
        const auto long_ago = std::chrono::steady_clock::now() - std::chrono::seconds(300);
        registry.apply_heartbeat(UuidOf(kEui), 1000, 0, long_ago);
        ingest::PowerBaseState base;
        API_CHECK(registry.get_power_base(UuidOf(kEui), base) && !base.online && base.last_heartbeat_serial > 0,
                  "the Base is cached with an old heartbeat and is offline");

        EnrollmentSession s;
        API_CHECK(StartA(m, s) == EnrollmentStartOutcome::Started, "an offline Base can be enrolled");
        API_CHECK(WaitFor([&] { return GetOrEmpty(m, kDeviceA, s.session_id).state == EnrollmentState::WaitingForHeartbeat; }),
                  "it waits");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        API_CHECK(GetOrEmpty(m, kDeviceA, s.session_id).state == EnrollmentState::WaitingForHeartbeat,
                  "the cached Base and its old heartbeat are not a success");

        // The very same timestamp as the old heartbeat: only arriving after
        // the start makes it count.
        registry.apply_heartbeat(UuidOf(kEui), 1000, 0, long_ago);
        API_CHECK(WaitFor([&] { return GetOrEmpty(m, kDeviceA, s.session_id).state == EnrollmentState::HeartbeatReceived; }),
                  "a new heartbeat counts even with an identical timestamp");
    }

    printf("\n-- scenario: a Base that is online right now is refused --\n");
    {
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        EnrollmentManager m(registry, fake, FastConfig());
        registry.apply_heartbeat(UuidOf(kEui), 1000, 0);
        EnrollmentSession s;
        API_CHECK(StartA(m, s) == EnrollmentStartOutcome::AlreadyConnected, "already connected");
        API_CHECK(fake->ensure_calls.load() == 0 && fake->add_calls.load() == 0, "and the OTBR was not touched");
    }

    printf("\n-- scenario: one session at a time; other devices see nothing --\n");
    {
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        EnrollmentManager m(registry, fake, FastConfig());
        EnrollmentSession s;
        API_CHECK(StartA(m, s) == EnrollmentStartOutcome::Started, "a session starts");
        EnrollmentSession again;
        API_CHECK(StartA(m, again, kOtherEui) == EnrollmentStartOutcome::Busy && again.session_id == s.session_id,
                  "a second start by the same device is refused and shows its running session");
        std::string pskd = kPskd;
        EnrollmentSession other;
        API_CHECK(m.Start(kDeviceB, kOtherEui, pskd, other) == EnrollmentStartOutcome::Busy &&
                      other.session_id.empty(),
                  "another device is refused without seeing the session");
        API_CHECK(!m.Get(kDeviceB, s.session_id, other), "another device cannot read it");
        API_CHECK(m.Cancel(kDeviceB, s.session_id, other) == EnrollmentCancelOutcome::NotFound,
                  "another device cannot cancel it");
        API_CHECK(!m.Get(kDeviceA, std::string(32, 'a'), other), "an unknown id is not found");
        API_CHECK(GetOrEmpty(m, kDeviceA, s.session_id).state != EnrollmentState::Cancelled,
                  "the session is untouched by those attempts");
        API_CHECK(fake->add_calls.load() <= 1, "no second Joiner entry was opened");
    }

    printf("\n-- scenario: cancel --\n");
    {
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        EnrollmentManager m(registry, fake, FastConfig());
        EnrollmentSession s;
        StartA(m, s);
        WaitFor([&] { return GetOrEmpty(m, kDeviceA, s.session_id).state == EnrollmentState::WaitingForHeartbeat; });
        EnrollmentSession c;
        API_CHECK(m.Cancel(kDeviceA, s.session_id, c) == EnrollmentCancelOutcome::Cancelled &&
                      c.state == EnrollmentState::Cancelled,
                  "the starting device cancels it");
        API_CHECK(WaitFor([&] { return Finished(m, s.session_id); }), "the session finishes");
        API_CHECK(GetOrEmpty(m, kDeviceA, s.session_id).joiner_entry == JoinerEntry::Removed, "its entry is removed");
        registry.apply_heartbeat(UuidOf(kEui), 1000, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        API_CHECK(GetOrEmpty(m, kDeviceA, s.session_id).state == EnrollmentState::Cancelled,
                  "a heartbeat after the cancel does not change a final state");
        API_CHECK(m.Cancel(kDeviceA, s.session_id, c) == EnrollmentCancelOutcome::AlreadyFinal,
                  "a second cancel says it is already final");
        EnrollmentSession next;
        API_CHECK(StartA(m, next, kOtherEui) == EnrollmentStartOutcome::Started && next.session_id != s.session_id,
                  "a new session can start once the old one has finished");
    }

    printf("\n-- scenario: a heartbeat that reached the Registry first wins over a cancel --\n");
    {
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        EnrollmentConfig config = FastConfig();
        config.poll_interval = std::chrono::milliseconds(5000); // the worker would notice only much later
        EnrollmentManager m(registry, fake, config);
        EnrollmentSession s;
        StartA(m, s);
        WaitFor([&] { return fake->add_calls.load() == 1; });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        registry.apply_heartbeat(UuidOf(kEui), 1000, 0);
        EnrollmentSession c;
        API_CHECK(m.Cancel(kDeviceA, s.session_id, c) == EnrollmentCancelOutcome::AlreadyFinal &&
                      c.state == EnrollmentState::HeartbeatReceived,
                  "the cancel answers heartbeat_received");
    }

    printf("\n-- scenario: expiry --\n");
    {
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        EnrollmentManager m(registry, fake, FastConfig(std::chrono::milliseconds(1000)));
        EnrollmentSession s;
        StartA(m, s);
        API_CHECK(WaitFor([&] { return Finished(m, s.session_id); }), "a 1 s window ends by itself");
        EnrollmentSession e = GetOrEmpty(m, kDeviceA, s.session_id);
        API_CHECK(e.state == EnrollmentState::Expired && e.joiner_entry == JoinerEntry::Removed,
                  "as expired, with its entry removed");
        std::lock_guard<std::mutex> lock(fake->mutex);
        API_CHECK(fake->add_timeouts.size() == 1 && fake->add_timeouts[0] == 1,
                  "the OTBR entry's own timeout matched the window");
    }

    printf("\n-- scenario: Commissioner failures --\n");
    {
        struct Case {
            const char *name;
            CommissionerStatus ensure;
            CommissionerStatus add;
            const char *error;
            bool remove_expected;
        };
        const Case cases[] = {
            {"the OTBR is unreachable", CommissionerStatus::Unavailable, CommissionerStatus::Ok, "otbr_unavailable",
             false},
            {"the Commissioner petition is refused", CommissionerStatus::Rejected, CommissionerStatus::Ok,
             "commissioner_rejected", false},
            {"the Commissioner start times out", CommissionerStatus::Timeout, CommissionerStatus::Ok, "otbr_timeout",
             false},
            {"the helper answers nonsense", CommissionerStatus::Failed, CommissionerStatus::Ok, "helper_failed", false},
            {"the Joiner add is refused", CommissionerStatus::Ok, CommissionerStatus::Rejected, "joiner_rejected",
             true},
            {"the Joiner add times out", CommissionerStatus::Ok, CommissionerStatus::Timeout, "otbr_timeout", true},
        };
        for (const Case &c : cases) {
            ingest::Registry registry;
            auto fake = std::make_shared<FakeCommissioner>();
            fake->ensure_result = c.ensure;
            fake->add_result = c.add;
            fake->remove_result = CommissionerStatus::NotFound;
            EnrollmentManager m(registry, fake, FastConfig());
            EnrollmentSession s;
            StartA(m, s);
            bool done = WaitFor([&] { return Finished(m, s.session_id); });
            EnrollmentSession f = GetOrEmpty(m, kDeviceA, s.session_id);
            API_CHECK(done && f.state == EnrollmentState::Failed && f.error == c.error,
                      (std::string(c.name) + ": failed with " + c.error).c_str());
            if (c.remove_expected) {
                API_CHECK(fake->remove_calls.load() == 1 && f.joiner_entry == JoinerEntry::Removed,
                          (std::string(c.name) + ": the possibly-added entry is still removed (NotFound is fine)").c_str());
            } else {
                API_CHECK(fake->add_calls.load() == 0 && fake->remove_calls.load() == 0 &&
                              f.joiner_entry == JoinerEntry::None,
                          (std::string(c.name) + ": no Joiner entry was ever touched").c_str());
            }
        }
    }

    printf("\n-- scenario: removal results --\n");
    {
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        fake->remove_result = CommissionerStatus::NotFound;
        EnrollmentManager m(registry, fake, FastConfig());
        EnrollmentSession s;
        StartA(m, s);
        WaitFor([&] { return fake->add_calls.load() == 1; });
        registry.apply_heartbeat(UuidOf(kEui), 1000, 0);
        WaitFor([&] { return Finished(m, s.session_id); });
        EnrollmentSession f = GetOrEmpty(m, kDeviceA, s.session_id);
        API_CHECK(f.state == EnrollmentState::HeartbeatReceived && f.joiner_entry == JoinerEntry::Removed,
                  "NotFound after OpenThread removed the entry itself is a successful cleanup");
    }
    {
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        fake->remove_result = CommissionerStatus::Unavailable;
        EnrollmentManager m(registry, fake, FastConfig());
        EnrollmentSession s;
        StartA(m, s);
        WaitFor([&] { return fake->add_calls.load() == 1; });
        EnrollmentSession c;
        m.Cancel(kDeviceA, s.session_id, c);
        WaitFor([&] { return Finished(m, s.session_id); });
        API_CHECK(GetOrEmpty(m, kDeviceA, s.session_id).joiner_entry == JoinerEntry::RemoveFailed,
                  "a failed removal is reported as remove_failed (the OTBR timeout still closes it)");
    }

    printf("\n-- scenario: a window too short to open is not opened --\n");
    {
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        EnrollmentConfig config = FastConfig(std::chrono::milliseconds(5000));
        config.min_joiner_timeout = std::chrono::seconds(10);
        EnrollmentManager m(registry, fake, config);
        EnrollmentSession s;
        StartA(m, s);
        WaitFor([&] { return Finished(m, s.session_id); });
        EnrollmentSession f = GetOrEmpty(m, kDeviceA, s.session_id);
        API_CHECK(f.state == EnrollmentState::Failed && f.error == "window_too_short" && fake->add_calls.load() == 0,
                  "window_too_short, no Joiner entry");
    }

    printf("\n-- scenario: cancel while the Joiner add is still running --\n");
    {
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        fake->add_delay = std::chrono::milliseconds(300);
        EnrollmentManager m(registry, fake, FastConfig());
        EnrollmentSession s;
        StartA(m, s);
        WaitFor([&] { return fake->add_calls.load() == 1; });
        auto t0 = std::chrono::steady_clock::now();
        EnrollmentSession c;
        API_CHECK(m.Cancel(kDeviceA, s.session_id, c) == EnrollmentCancelOutcome::Cancelled &&
                      c.joiner_entry == JoinerEntry::Adding,
                  "the cancel is accepted while the add runs");
        API_CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(100),
                  "without waiting for the Commissioner call (no lock is held during it)");
        API_CHECK(WaitFor([&] { return Finished(m, s.session_id); }), "the session then finishes");
        EnrollmentSession f = GetOrEmpty(m, kDeviceA, s.session_id);
        API_CHECK(f.state == EnrollmentState::Cancelled && f.joiner_entry == JoinerEntry::Removed &&
                      fake->remove_calls.load() == 1,
                  "stays cancelled (not reopened as waiting) and the late entry is removed");
    }

    printf("\n-- scenario: shutdown --\n");
    {
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        EnrollmentManager m(registry, fake, FastConfig());
        EnrollmentSession s;
        StartA(m, s);
        WaitFor([&] { return fake->add_calls.load() == 1; });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        m.Stop();
        EnrollmentSession f = GetOrEmpty(m, kDeviceA, s.session_id);
        API_CHECK(f.state == EnrollmentState::Failed && f.error == "bridge_stopping" && f.finished,
                  "Stop() ends the session as failed/bridge_stopping and waits for it");
        API_CHECK(f.joiner_entry == JoinerEntry::Removed && fake->remove_calls.load() == 1,
                  "the Joiner entry was removed before Stop() returned");
        EnrollmentSession after;
        API_CHECK(StartA(m, after) == EnrollmentStartOutcome::Stopping, "no new session after Stop()");
        m.Stop();
        API_CHECK(true, "a second Stop() is harmless");
    }

    printf("\n-- scenario: heartbeat vs cancel races --\n");
    {
        int consistent = 0;
        int removals_ok = 0;
        const int kRounds = 40;
        for (int i = 0; i < kRounds; i++) {
            ingest::Registry registry;
            auto fake = std::make_shared<FakeCommissioner>();
            EnrollmentConfig config = FastConfig();
            config.poll_interval = std::chrono::milliseconds(1);
            EnrollmentManager m(registry, fake, config);
            EnrollmentSession s;
            StartA(m, s);
            WaitFor([&] { return GetOrEmpty(m, kDeviceA, s.session_id).state == EnrollmentState::WaitingForHeartbeat; });
            std::atomic<bool> go{false};
            EnrollmentCancelOutcome outcome = EnrollmentCancelOutcome::NotFound;
            std::thread heartbeat([&] {
                while (!go.load()) {
                }
                registry.apply_heartbeat(UuidOf(kEui), 1000, 0);
            });
            std::thread cancel([&] {
                while (!go.load()) {
                }
                EnrollmentSession c;
                outcome = m.Cancel(kDeviceA, s.session_id, c);
            });
            go.store(true);
            heartbeat.join();
            cancel.join();
            WaitFor([&] { return Finished(m, s.session_id); });
            EnrollmentState final_state = GetOrEmpty(m, kDeviceA, s.session_id).state;
            if ((outcome == EnrollmentCancelOutcome::Cancelled && final_state == EnrollmentState::Cancelled) ||
                (outcome == EnrollmentCancelOutcome::AlreadyFinal && final_state == EnrollmentState::HeartbeatReceived)) {
                consistent++;
            }
            if (fake->remove_calls.load() == 1) {
                removals_ok++;
            }
        }
        API_CHECK(consistent == kRounds, "every round ends in exactly one outcome, the one the cancel reported");
        API_CHECK(removals_ok == kRounds, "every round removes the entry exactly once");
    }

    printf("\n-- scenario: expiry vs heartbeat at the deadline --\n");
    {
        int consistent = 0;
        const int kRounds = 10;
        for (int i = 0; i < kRounds; i++) {
            ingest::Registry registry;
            auto fake = std::make_shared<FakeCommissioner>();
            EnrollmentConfig config = FastConfig(std::chrono::milliseconds(1000));
            config.poll_interval = std::chrono::milliseconds(1);
            EnrollmentManager m(registry, fake, config);
            EnrollmentSession s;
            StartA(m, s);
            std::this_thread::sleep_for(std::chrono::milliseconds(995 + i));
            registry.apply_heartbeat(UuidOf(kEui), 1000, 0);
            WaitFor([&] { return Finished(m, s.session_id); });
            EnrollmentSession f = GetOrEmpty(m, kDeviceA, s.session_id);
            if ((f.state == EnrollmentState::Expired || f.state == EnrollmentState::HeartbeatReceived) &&
                f.joiner_entry == JoinerEntry::Removed && fake->remove_calls.load() == 1) {
                consistent++;
            }
        }
        API_CHECK(consistent == kRounds, "each round is either expired or heartbeat_received, cleaned up once");
    }

    printf("\n-- scenario: finished sessions are kept only for a while --\n");
    {
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        EnrollmentConfig config = FastConfig();
        config.retain_finished = std::chrono::milliseconds(100);
        EnrollmentManager m(registry, fake, config);
        EnrollmentSession s;
        StartA(m, s);
        EnrollmentSession c;
        WaitFor([&] { return fake->add_calls.load() == 1; });
        m.Cancel(kDeviceA, s.session_id, c);
        WaitFor([&] { return Finished(m, s.session_id); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        API_CHECK(!m.Get(kDeviceA, s.session_id, c), "a finished session is forgotten after retain_finished");
    }

    // ---- bridge endpoint delivery ----
    const std::string kMleid = "fd11:2233:4455:0:8c1f:2e3d:4c5b:6a79"; // the fake Commissioner's made-up EID
    const uint16_t kIngestPort = 5701;                                  // deliberately not the default 5684
    auto EndpointConfig = [&](std::chrono::milliseconds window = std::chrono::milliseconds(120000)) {
        EnrollmentConfig c = FastConfig(window);
        c.bridge_port = kIngestPort;
        return c;
    };
    auto StartWithEndpoint = [&](EnrollmentManager &m, EnrollmentSession &s, const char *device = kDeviceA) {
        std::string pskd = kPskd;
        EnrollmentStartOptions options;
        options.bridge_endpoint_version = kBridgeEndpointVersion;
        return m.Start(device, kEui, pskd, s, options);
    };
    auto Order = [](FakeCommissioner &fake) {
        std::lock_guard<std::mutex> lock(fake.mutex);
        std::string joined;
        for (const auto &step : fake.order) {
            joined += (joined.empty() ? "" : ",") + step;
        }
        return joined;
    };

    printf("\n-- scenario: a session that asks gets this hub's bridge endpoint, fixed once --\n");
    {
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        EnrollmentManager m(registry, fake, EndpointConfig());
        EnrollmentSession s;
        API_CHECK(StartWithEndpoint(m, s) == EnrollmentStartOutcome::Started && s.bridge_endpoint_requested &&
                      !s.has_bridge_endpoint && s.state == EnrollmentState::Preparing,
                  "it starts out preparing, with the endpoint not established yet");
        API_CHECK(WaitFor([&] { return GetOrEmpty(m, kDeviceA, s.session_id).state == EnrollmentState::WaitingForHeartbeat; }),
                  "it reaches waiting_for_heartbeat");
        EnrollmentSession now = GetOrEmpty(m, kDeviceA, s.session_id);
        API_CHECK(now.has_bridge_endpoint && now.bridge_endpoint.version == 1 && now.bridge_endpoint.address == kMleid,
                  "with the OTBR's Mesh-Local EID as its bridge endpoint (format version 1)");
        API_CHECK(now.bridge_endpoint.port == kIngestPort, "and the ingest listener's configured port, not a built-in number");
        API_CHECK(Order(*fake) == "mleid,ensure,add", "the address was read BEFORE the Commissioner start and the Joiner add");
        API_CHECK(fake->mleid_calls.load() == 1, "it was read once");

        // The same value for every later read, and for a repeated start.
        fake->mleid_address = "fd99:1:2:3:4:5:6:7"; // the OTBR's address "changes" mid-session
        EnrollmentSession again = GetOrEmpty(m, kDeviceA, s.session_id);
        API_CHECK(again.has_bridge_endpoint && again.bridge_endpoint.address == kMleid && again.bridge_endpoint.port == kIngestPort,
                  "a later read reports the same endpoint");
        EnrollmentSession retry;
        API_CHECK(StartWithEndpoint(m, retry) == EnrollmentStartOutcome::Busy && retry.session_id == s.session_id &&
                      retry.has_bridge_endpoint && retry.bridge_endpoint.address == kMleid &&
                      retry.bridge_endpoint.port == kIngestPort,
                  "a repeated start by the same device finds the same session with the same endpoint");
        API_CHECK(fake->mleid_calls.load() == 1 && fake->add_calls.load() == 1, "neither read the address nor added a Joiner again");

        EnrollmentSession other;
        API_CHECK(!m.Get(kDeviceB, s.session_id, other) && !other.has_bridge_endpoint && other.bridge_endpoint.address.empty(),
                  "another device cannot read the session, so not its endpoint either");
        EnrollmentSession other_start;
        std::string pskd = kPskd;
        EnrollmentStartOptions options;
        options.bridge_endpoint_version = kBridgeEndpointVersion;
        API_CHECK(m.Start(kDeviceB, kEui, pskd, other_start, options) == EnrollmentStartOutcome::Busy &&
                      other_start.session_id.empty() && !other_start.has_bridge_endpoint &&
                      other_start.bridge_endpoint.address.empty(),
                  "another device's start is Busy and learns nothing: no session, no endpoint");

        registry.apply_heartbeat(UuidOf(kEui), 1000, 0);
        API_CHECK(WaitFor([&] { return Finished(m, s.session_id); }), "the heartbeat ends it");
        now = GetOrEmpty(m, kDeviceA, s.session_id);
        API_CHECK(now.state == EnrollmentState::HeartbeatReceived && now.has_bridge_endpoint &&
                      now.bridge_endpoint.address == kMleid,
                  "success is still only heartbeat_received; the finished session keeps its endpoint");
    }

    printf("\n-- scenario: a session that does not ask is exactly the old session --\n");
    {
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        EnrollmentManager m(registry, fake, EndpointConfig());
        EnrollmentSession s;
        API_CHECK(StartA(m, s) == EnrollmentStartOutcome::Started && !s.bridge_endpoint_requested, "started without the option");
        API_CHECK(WaitFor([&] { return GetOrEmpty(m, kDeviceA, s.session_id).state == EnrollmentState::WaitingForHeartbeat; }),
                  "it waits for the heartbeat as before");
        EnrollmentSession now = GetOrEmpty(m, kDeviceA, s.session_id);
        API_CHECK(!now.has_bridge_endpoint && now.bridge_endpoint.address.empty() && fake->mleid_calls.load() == 0 &&
                      Order(*fake) == "ensure,add",
                  "no endpoint and no address read: the same two OTBR calls as before");
        m.Stop();
    }

    printf("\n-- scenario: no endpoint means no Joiner -- failure, timeout and bad answers --\n");
    {
        const struct {
            CommissionerStatus status;
            const char *address;
            const char *error;
            const char *what;
        } cases[] = {
            {CommissionerStatus::Unavailable, "", "bridge_endpoint_unavailable", "helper/OTBR not reachable"},
            {CommissionerStatus::InvalidState, "", "bridge_endpoint_unavailable", "the OTBR refused the command"},
            {CommissionerStatus::Timeout, "", "bridge_endpoint_timeout", "the helper timed out"},
            {CommissionerStatus::Failed, "", "bridge_endpoint_invalid", "the helper's answer was not one usable address"},
            {CommissionerStatus::Unsupported, "", "bridge_endpoint_unsupported", "an older helper without the operation"},
            {CommissionerStatus::Ok, "fe80::1", "bridge_endpoint_invalid", "Ok but a link-local address"},
            {CommissionerStatus::Ok, "fd11:2233:4455:0:0:ff:fe00:4c00", "bridge_endpoint_invalid", "Ok but an RLOC"},
            {CommissionerStatus::Ok, "::ffff:192.168.1.10", "bridge_endpoint_invalid", "Ok but an IPv4-mapped address"},
            {CommissionerStatus::Ok, "192.168.1.10", "bridge_endpoint_invalid", "Ok but an IPv4 address"},
            {CommissionerStatus::Ok, "hub.local", "bridge_endpoint_invalid", "Ok but a hostname"},
            {CommissionerStatus::Ok, "", "bridge_endpoint_invalid", "Ok but no address"},
            {CommissionerStatus::Ok, "fd11:2233:4455:0:8c1f:2e3d:4c5b:6a79%wpan0", "bridge_endpoint_invalid",
             "Ok but with a zone id"},
        };
        for (const auto &c : cases) {
            ingest::Registry registry;
            auto fake = std::make_shared<FakeCommissioner>();
            fake->mleid_result = c.status;
            fake->mleid_address = c.address;
            EnrollmentManager m(registry, fake, EndpointConfig());
            EnrollmentSession s;
            StartWithEndpoint(m, s);
            printf("      (%s)\n", c.what);
            API_CHECK(WaitFor([&] { return Finished(m, s.session_id); }), "the session ends");
            EnrollmentSession now = GetOrEmpty(m, kDeviceA, s.session_id);
            API_CHECK(now.state == EnrollmentState::Failed && now.error == c.error && !now.has_bridge_endpoint &&
                          now.bridge_endpoint.address.empty(),
                      "as failed with its fixed error code, without an endpoint");
            API_CHECK(now.joiner_entry == JoinerEntry::None && fake->add_calls.load() == 0 && fake->ensure_calls.load() == 0 &&
                          fake->remove_calls.load() == 0 && Order(*fake) == "mleid",
                      "nothing was added to the OTBR: no Commissioner start, no Joiner add, so nothing to remove");
            // The failure is final: a heartbeat afterwards changes nothing.
            registry.apply_heartbeat(UuidOf(kEui), 1000, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            API_CHECK(GetOrEmpty(m, kDeviceA, s.session_id).state == EnrollmentState::Failed, "and it stays failed");
        }

        // No ingest port known to the manager: refused before anything exists.
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        EnrollmentManager m(registry, fake, FastConfig());
        EnrollmentSession s;
        API_CHECK(StartWithEndpoint(m, s) == EnrollmentStartOutcome::BridgeEndpointUnavailable && s.session_id.empty() &&
                      fake->mleid_calls.load() == 0 && fake->add_calls.load() == 0,
                  "without a configured ingest port the start is refused: no session, no OTBR call");
        API_CHECK(StartA(m, s) == EnrollmentStartOutcome::Started, "a start without the option still works there");
        m.Stop();

        // A version this hub does not make.
        std::string pskd = kPskd;
        EnrollmentStartOptions bad;
        bad.bridge_endpoint_version = 2;
        ingest::Registry registry2;
        auto fake2 = std::make_shared<FakeCommissioner>();
        EnrollmentManager m2(registry2, fake2, EndpointConfig());
        API_CHECK(m2.Start(kDeviceA, kEui, pskd, s, bad) == EnrollmentStartOutcome::InternalError && pskd.empty() &&
                      fake2->mleid_calls.load() == 0,
                  "an endpoint format version the hub does not make starts nothing (the HTTP layer refuses it first)");
    }

    printf("\n-- scenario: the address read holds no lock --\n");
    {
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        fake->mleid_delay = std::chrono::milliseconds(600);
        EnrollmentManager m(registry, fake, EndpointConfig());
        EnrollmentSession s;
        StartWithEndpoint(m, s);
        API_CHECK(WaitFor([&] { return fake->mleid_in_progress.load(); }), "the helper call is under way");
        auto t0 = std::chrono::steady_clock::now();
        EnrollmentSession now = GetOrEmpty(m, kDeviceA, s.session_id);
        registry.apply_heartbeat(UuidOf(kOtherEui), 1000, 0);
        EnrollmentSession busy;
        const auto second = StartWithEndpoint(m, busy, kDeviceB);
        auto took = std::chrono::steady_clock::now() - t0;
        API_CHECK(fake->mleid_in_progress.load() && took < std::chrono::milliseconds(200),
                  "Get, another Start and a Registry heartbeat all return while it still runs");
        API_CHECK(now.state == EnrollmentState::Preparing && !now.has_bridge_endpoint && now.joiner_entry == JoinerEntry::None,
                  "the session is preparing, without an endpoint and without a Joiner entry");
        API_CHECK(second == EnrollmentStartOutcome::Busy && fake->add_calls.load() == 0, "the other start is Busy; no Joiner yet");
        API_CHECK(WaitFor([&] { return GetOrEmpty(m, kDeviceA, s.session_id).state == EnrollmentState::WaitingForHeartbeat; }),
                  "afterwards the session goes on");
        m.Stop();
    }

    printf("\n-- scenario: a session that ends while the address is read goes no further --\n");
    {
        // Cancelled.
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        fake->mleid_delay = std::chrono::milliseconds(300);
        EnrollmentManager m(registry, fake, EndpointConfig());
        EnrollmentSession s;
        StartWithEndpoint(m, s);
        WaitFor([&] { return fake->mleid_in_progress.load(); });
        EnrollmentSession c;
        API_CHECK(m.Cancel(kDeviceA, s.session_id, c) == EnrollmentCancelOutcome::Cancelled, "cancel during the address read is taken");
        API_CHECK(WaitFor([&] { return Finished(m, s.session_id); }), "the session finishes");
        EnrollmentSession now = GetOrEmpty(m, kDeviceA, s.session_id);
        API_CHECK(now.state == EnrollmentState::Cancelled && !now.has_bridge_endpoint && now.joiner_entry == JoinerEntry::None,
                  "it stays cancelled: the address that arrived afterwards is not attached and the state not revived");
        API_CHECK(fake->ensure_calls.load() == 0 && fake->add_calls.load() == 0 && fake->remove_calls.load() == 0 &&
                      Order(*fake) == "mleid",
                  "no Commissioner start and no Joiner add followed");
    }
    {
        // Cancelled, and the read then fails: still cancelled, not "failed".
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        fake->mleid_delay = std::chrono::milliseconds(300);
        fake->mleid_result = CommissionerStatus::Timeout;
        EnrollmentManager m(registry, fake, EndpointConfig());
        EnrollmentSession s;
        StartWithEndpoint(m, s);
        WaitFor([&] { return fake->mleid_in_progress.load(); });
        EnrollmentSession c;
        m.Cancel(kDeviceA, s.session_id, c);
        API_CHECK(WaitFor([&] { return Finished(m, s.session_id); }) &&
                      GetOrEmpty(m, kDeviceA, s.session_id).state == EnrollmentState::Cancelled &&
                      GetOrEmpty(m, kDeviceA, s.session_id).error.empty() && fake->add_calls.load() == 0,
                  "a cancel followed by a failed read stays cancelled (the first final state wins), no Joiner");
    }
    {
        // Expired.
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        fake->mleid_delay = std::chrono::milliseconds(1300);
        EnrollmentManager m(registry, fake, EndpointConfig(std::chrono::milliseconds(1000)));
        EnrollmentSession s;
        StartWithEndpoint(m, s);
        API_CHECK(WaitFor([&] { return Finished(m, s.session_id); }), "a window that ends during the address read");
        EnrollmentSession now = GetOrEmpty(m, kDeviceA, s.session_id);
        API_CHECK(now.state == EnrollmentState::Expired && !now.has_bridge_endpoint && fake->add_calls.load() == 0 &&
                      fake->ensure_calls.load() == 0,
                  "expires: no endpoint, no Commissioner start, no Joiner add");
    }
    {
        // Shutdown.
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        fake->mleid_delay = std::chrono::milliseconds(300);
        EnrollmentManager m(registry, fake, EndpointConfig());
        EnrollmentSession s;
        StartWithEndpoint(m, s);
        WaitFor([&] { return fake->mleid_in_progress.load(); });
        m.Stop();
        EnrollmentSession now = GetOrEmpty(m, kDeviceA, s.session_id);
        API_CHECK(now.state == EnrollmentState::Failed && now.error == "bridge_stopping" && !now.has_bridge_endpoint,
                  "shutdown during the address read: failed / bridge_stopping, no endpoint");
        API_CHECK(fake->ensure_calls.load() == 0 && fake->add_calls.load() == 0, "and nothing was added to the OTBR");
    }

    printf("\n-- scenario: later failures still happen after the endpoint, and clean up as before --\n");
    {
        ingest::Registry registry;
        auto fake = std::make_shared<FakeCommissioner>();
        fake->add_result = CommissionerStatus::Rejected;
        EnrollmentManager m(registry, fake, EndpointConfig());
        EnrollmentSession s;
        StartWithEndpoint(m, s);
        API_CHECK(WaitFor([&] { return Finished(m, s.session_id); }), "a rejected Joiner add ends the session");
        EnrollmentSession now = GetOrEmpty(m, kDeviceA, s.session_id);
        API_CHECK(now.state == EnrollmentState::Failed && now.error == "joiner_rejected" && now.has_bridge_endpoint,
                  "as joiner_rejected; the endpoint that had been fixed is still reported");
        API_CHECK(Order(*fake) == "mleid,ensure,add,remove", "the possible entry is removed, as for any attempted add");
    }
}
