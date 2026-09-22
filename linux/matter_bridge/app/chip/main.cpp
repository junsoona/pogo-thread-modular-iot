// pogo_matter_bridge: runs the existing Power Base -> Linux OTBR CoAP
// ingest server (ingest::run_server(), completely unmodified -- see
// linux/matter_bridge/ingest) and the real connectedhomeip v1.6.0.0 Matter
// stack in ONE process, bridging ingest::Registry's sensor state onto
// dynamic Matter endpoints (see chip_endpoint.hpp).
//
// Usage:
//   pogo_matter_bridge --interface wpan0 --state-dir /var/lib/pogo-matter-bridge [--port 5684] [connectedhomeip's
//   own standard options, e.g. --KVS <path>, --discriminator, --passcode, ...; see --help]
//
// Threading model (three threads, each with exactly one job):
//   1. The CoAP receive thread (ingest::run_server(), see that file's own
//      doc comment) -- receives SAMPLE/TOPOLOGY_SNAPSHOT/HEARTBEAT
//      messages and calls ONLY ingest::Registry::apply_*(). Never touches
//      the Matter SDK, never touches BridgeManager/ChipEndpointManager.
//   2. The "registry watcher" thread (RegistryWatcherThreadMain, this
//      file) -- polls ingest::Registry::wait_for_change() on a bounded
//      250ms timeout (never a busy loop, but also never SUSPENDED
//      indefinitely -- see this pass's own note below), takes a snapshot(),
//      and runs matter_bridge::BridgeManager::Reconcile() on it. For each
//      resulting matter_bridge::ModuleUpdate, matter_bridge::BridgeManager::
//      TryBeginSchedule() decides whether to actually call
//      chip::DeviceLayer::PlatformMgr().ScheduleWork() this round -- see
//      that method's own doc comment for the coalescing+rate-limit policy
//      (this pass: transport-load reduction, Linux Registry -> Matter).
//      This thread itself NEVER calls into the Matter SDK, per this
//      application's own explicit design requirement (see
//      chip_endpoint.hpp's own top comment) and per ingest/registry.hpp's
//      own top comment (never call the Matter SDK or do file I/O while
//      holding the registry's mutex -- wait_for_change()/snapshot() only
//      ever hold it briefly to copy data out). This thread also logs (once
//      per module_uuid) any matter_bridge::BridgeManager::SensorBitmapMismatches()
//      -- see LogNewSensorBitmapMismatches()'s own doc comment and
//      docs/matter-bridge.md's restart-for-sensor-swap section.
//
//      This pass: wait_for_change()'s return value is deliberately no
//      longer checked to skip re-processing on a timeout -- Reconcile() is
//      cheap (bounded by ingest::Registry::kMaxNodes) and re-running it
//      every ~250ms regardless is what lets a module_uuid that
//      TryBeginSchedule() rate-limited earlier get promptly retried once
//      its own window reopens, without needing a brand-new registry change
//      to wake this thread up again.
//   3. The Matter event-loop thread (connectedhomeip's own
//      ChipLinuxAppMainLoop()) -- the ONLY thread that ever touches a
//      Matter cluster object (via ChipEndpointManager::Apply(), called
//      from the ScheduleWork() callback below, which ALSO re-reads
//      ingest::Registry::snapshot() itself at that exact moment -- see
//      ApplyUpdateOnMatterThread()'s own doc comment -- and reports the
//      real outcome back via matter_bridge::BridgeManager::CommitApplied()).
//
// Shutdown: connectedhomeip's own AppMain.cpp owns SIGINT/SIGTERM handling
// (it blocks those signals in every OTHER thread, including the two this
// file creates, and handles them itself on the main thread -- see that
// file's own comments) and calls this application's ApplicationShutdown()
// hook once it starts tearing down. That hook only flips two atomics;
// main() joins both of this program's own threads AFTER
// ChipLinuxAppMainLoop() returns, never inside the shutdown hook itself.
#include <AppMain.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>

#include "chip_endpoint.hpp"
#include "ingest/registry.hpp"
#include "ingest/server.hpp"
#include "matter_bridge/bridge_manager.hpp"
#include "matter_bridge/endpoint_store.hpp"
#include "pogohome_api/http_server.hpp"
#include "pogohome_api/interface_address.hpp"

using chip::ArgParser::OptionDef;
using chip::ArgParser::OptionSet;
using chip::ArgParser::PrintArgError;

namespace {

struct AppConfig {
    std::string interface;
    uint16_t port = 5684;
    std::string state_dir;
    // PogoHome Local API v1 (see docs/pogohome-api-v1.md) -- disabled
    // (empty) by default, same "explicit required interface, never listen
    // everywhere" policy as `interface` above. api_port only matters if
    // api_interface is non-empty.
    std::string api_interface;
    uint16_t api_port = 8787;
};

AppConfig gAppConfig;

constexpr uint16_t kOptionInterface = 0xFF10;
constexpr uint16_t kOptionPort = 0xFF11;
constexpr uint16_t kOptionStateDir = 0xFF12;
constexpr uint16_t kOptionApiInterface = 0xFF13;
constexpr uint16_t kOptionApiPort = 0xFF14;

bool HandlePogoOption(const char *program, OptionSet *, int identifier, const char *name, const char *value)
{
    switch (identifier) {
    case kOptionInterface:
        gAppConfig.interface = value;
        return true;
    case kOptionPort: {
        int parsed = atoi(value);
        if (parsed <= 0 || parsed > 0xFFFF) {
            PrintArgError("%s: --port must be 1..65535\n", program);
            return false;
        }
        gAppConfig.port = static_cast<uint16_t>(parsed);
        return true;
    }
    case kOptionStateDir:
        gAppConfig.state_dir = value;
        return true;
    case kOptionApiInterface:
        gAppConfig.api_interface = value;
        return true;
    case kOptionApiPort: {
        int parsed = atoi(value);
        if (parsed <= 0 || parsed > 0xFFFF) {
            PrintArgError("%s: --api-port must be 1..65535\n", program);
            return false;
        }
        gAppConfig.api_port = static_cast<uint16_t>(parsed);
        return true;
    }
    default:
        PrintArgError("%s: INTERNAL ERROR: unhandled pogo_matter_bridge option: %s\n", program, name);
        return false;
    }
}

OptionDef gPogoOptionDefs[] = {
    {"interface", chip::ArgParser::kArgumentRequired, kOptionInterface},
    {"port", chip::ArgParser::kArgumentRequired, kOptionPort},
    {"state-dir", chip::ArgParser::kArgumentRequired, kOptionStateDir},
    {"api-interface", chip::ArgParser::kArgumentRequired, kOptionApiInterface},
    {"api-port", chip::ArgParser::kArgumentRequired, kOptionApiPort},
    {},
};

OptionSet gPogoOptions = {
    HandlePogoOption, gPogoOptionDefs, "POGO MATTER BRIDGE OPTIONS",
    "  --interface <ifname>\n"
    "       REQUIRED. The Thread network interface to receive this program's own CoAP ingest traffic on (e.g.\n"
    "       wpan0) -- see linux/matter_bridge/ingest/include/ingest/server.hpp's own SECURITY NOTE for why this\n"
    "       is required, not an optional \"listen on everything\" default.\n"
    "  --port <port>\n"
    "       UDP port for the CoAP ingest listener (default: 5684).\n"
    "  --state-dir <path>\n"
    "       REQUIRED. Directory for this bridge's OWN persistent state (the module_uuid -> Matter endpoint id\n"
    "       mapping, see matter_bridge::EndpointStore). Matter's own fabric/operational-credential storage is a\n"
    "       SEPARATE, standard connectedhomeip option (--KVS <path>, see --help below) -- point it at a path\n"
    "       under this same state-dir so a single directory holds all of this program's persistent state.\n"
    "  --api-interface <ifname>\n"
    "       OPTIONAL. Enables the read-only PogoHome Local API v1 HTTP server (see docs/pogohome-api-v1.md),\n"
    "       bound to THIS Linux network interface's own LAN address only (e.g. a Wi-Fi/Ethernet interface such\n"
    "       as wlp5x0 -- normally NOT the same interface as --interface's Thread interface). Omitted (the\n"
    "       default): the API server is disabled entirely, and this program behaves exactly as before this\n"
    "       option existed. If given, this program refuses to start at all if the named interface does not\n"
    "       exist, has no usable address, or the API server's port cannot be bound.\n"
    "  --api-port <port>\n"
    "       TCP port for the PogoHome Local API v1 HTTP server (default: 8787). Only meaningful with\n"
    "       --api-interface.\n",
};

ingest::Registry gRegistry;
std::atomic<bool> gStopIngest{false};
std::atomic<bool> gStopWatcher{false};

pogo_matter_bridge::ChipEndpointManager gEndpointManager;
matter_bridge::BridgeManager *gBridgeManager = nullptr;

std::thread gIngestThread;
std::thread gWatcherThread;
// Null (never constructed) unless --api-interface was given -- see this
// file's own top comment and docs/pogohome-api-v1.md. Its own acceptor and
// per-connection worker threads are entirely internal to
// pogohome_api::Server itself (see http_server.hpp), never tracked here.
std::unique_ptr<pogohome_api::Server> gApiServer;

// Runs on the Matter event-loop thread via
// chip::DeviceLayer::PlatformMgr().ScheduleWork() -- see this file's own
// top comment for the threading contract this depends on.
//
// This pass (transport-load reduction, Linux Registry -> Matter): the
// ScheduleWork() context now carries only a module_uuid, not a
// pre-computed matter_bridge::ModuleUpdate -- this function re-derives the
// update FRESH, from the registry's CURRENT snapshot at the exact moment
// this work item actually runs (matter_bridge::BridgeManager::ReconcileOne()),
// never whatever snapshot happened to be current back when
// RegistryWatcherThreadMain() originally called ScheduleWork(). This is
// what makes a detach/offline that lands in the registry WHILE this work
// item was queued (or a temperature that kept changing in the meantime)
// correctly win over a now-stale intended update -- see
// ReconcileOne()'s own doc comment.
void ApplyUpdateOnMatterThread(intptr_t context)
{
    auto *uuid_ctx = reinterpret_cast<matter_bridge::Uuid *>(context);
    matter_bridge::Uuid module_uuid = *uuid_ctx;
    delete uuid_ctx;

    std::vector<ingest::NodeState> snapshot = gRegistry.snapshot();
    std::optional<matter_bridge::ModuleUpdate> update = gBridgeManager->ReconcileOne(snapshot, module_uuid);
    if (update.has_value()) {
        matter_bridge::ApplyOutcome outcome = gEndpointManager.Apply(*update);
        // Only fields outcome actually reports as applied are committed --
        // see matter_bridge::BridgeManager::CommitApplied()'s own doc
        // comment: anything that failed here (a setter error, or the
        // endpoint itself failing to create) is simply left uncommitted, so
        // the NEXT Reconcile()/ReconcileOne() pass re-diffs and re-emits it
        // for another attempt -- no separate retry bookkeeping needed here.
        gBridgeManager->CommitApplied(module_uuid, *update, outcome);
    }
    // Whether or not there was anything left to apply (e.g. a uuid that
    // went offline again before this work item even ran), this work item
    // has now finished -- release the in-flight gate so a future update for
    // this uuid can be scheduled again.
    gBridgeManager->EndSchedule(module_uuid);
}

// Logs a module_uuid the moment matter_bridge::BridgeManager::Reconcile()
// FIRST reports it via SensorBitmapMismatches() (see that method's own doc
// comment) -- a real scenario only during development, when the SAME
// physical module_uuid's sensor hardware is reconfigured (e.g. SHT45 ->
// BH1750 -> SGP40 on one ESP32-H2 test board) while this bridge process
// keeps running. Deliberately logs only ONCE per module_uuid (tracked here,
// never cleared -- this condition does not self-resolve without a bridge
// restart, so re-logging it every ~250ms poll would just be noise) rather
// than on every Reconcile() pass it's still present in.
void LogNewSensorBitmapMismatches(const std::vector<matter_bridge::Uuid> &mismatched)
{
    static std::vector<matter_bridge::Uuid> already_logged;
    for (const auto &uuid : mismatched) {
        bool seen = false;
        for (const auto &logged : already_logged) {
            if (logged == uuid) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        already_logged.push_back(uuid);
        ChipLogError(NotSpecified,
                    "pogo_matter_bridge: module_uuid's reported sensor set no longer matches its ALREADY-CREATED "
                    "Matter endpoint's cluster set -- this only happens if the same physical module was "
                    "reconfigured with a different sensor while this bridge process kept running (see "
                    "docs/matter-bridge.md's restart-for-sensor-swap section). Channels outside the endpoint's "
                    "original declaration are never pushed to Matter; restart pogo_matter_bridge to recreate this "
                    "endpoint with its current sensor set.");
    }
}

void RegistryWatcherThreadMain()
{
    uint64_t last_seen_generation = 0;
    while (!gStopWatcher.load(std::memory_order_relaxed)) {
        // See this file's own top comment: the return value is
        // deliberately ignored -- this loop re-processes on every wakeup
        // AND on every bounded timeout, so a module_uuid TryBeginSchedule()
        // rate-limited earlier gets retried promptly once its own window
        // reopens, without needing a brand-new registry change.
        gRegistry.wait_for_change(&last_seen_generation, std::chrono::milliseconds(250));

        std::vector<ingest::NodeState> snapshot = gRegistry.snapshot();
        std::vector<matter_bridge::ModuleUpdate> updates = gBridgeManager->Reconcile(snapshot);
        LogNewSensorBitmapMismatches(gBridgeManager->SensorBitmapMismatches());

        for (auto &update : updates) {
            // Urgent: a brand-new endpoint or a Reachable (online<->offline)
            // transition -- never held back by the SAMPLE-only rate limit
            // (see matter_bridge::BridgeManager::TryBeginSchedule()'s own
            // doc comment).
            bool urgent = update.is_new_endpoint || update.reachable.has_value();
            if (!gBridgeManager->TryBeginSchedule(update.module_uuid, urgent)) {
                // Rate-limited (non-urgent, too soon since this uuid's last
                // schedule) or a work item for this uuid is already in
                // flight -- NOT a lost update: Reconcile() re-derives the
                // same (or by-then-newer) diff from the registry on this
                // loop's next pass, still well under 1s later.
                continue;
            }

            auto *uuid_ctx = new matter_bridge::Uuid(update.module_uuid);
            CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(ApplyUpdateOnMatterThread,
                                                                           reinterpret_cast<intptr_t>(uuid_ctx));
            if (err != CHIP_NO_ERROR) {
                ChipLogError(NotSpecified,
                            "pogo_matter_bridge: ScheduleWork failed (%" CHIP_ERROR_FORMAT ") -- releasing this "
                            "module_uuid's schedule gate so it is retried on this thread's own next pass (well "
                            "under 1s later), never left permanently blocked",
                            err.Format());
                delete uuid_ctx;
                gBridgeManager->AbortSchedule(update.module_uuid);
            }
        }
    }
}

} // namespace

void ApplicationInit()
{
    gEndpointManager.Init();
}

void ApplicationShutdown()
{
    gStopIngest.store(true, std::memory_order_relaxed);
    gStopWatcher.store(true, std::memory_order_relaxed);
    // gApiServer is deliberately NOT stopped here -- Stop() blocks (bounded,
    // but not instant) joining its own internal threads, and this hook's
    // own contract (see this file's top comment) is to only ever flip
    // atomics and return immediately; main() stops it, after
    // ChipLinuxAppMainLoop() returns, the same place it joins gIngestThread/
    // gWatcherThread.
}

int main(int argc, char *argv[])
{
    if (ChipLinuxAppInit(argc, argv, &gPogoOptions) != 0) {
        return -1;
    }

    if (gAppConfig.interface.empty()) {
        fprintf(stderr, "%s: --interface is required\n", argv[0]);
        return -1;
    }
    if (gAppConfig.state_dir.empty()) {
        fprintf(stderr, "%s: --state-dir is required\n", argv[0]);
        return -1;
    }

    // PogoHome Local API v1 (see docs/pogohome-api-v1.md) -- resolved and
    // bound here, BEFORE the ingest CoAP server or the registry watcher
    // thread ever starts, so a bad --api-interface/--api-port fails this
    // process's startup cleanly with nothing else running yet (this
    // follow-up spec's own explicit requirement: never silently disable the
    // API or fall back to "listen everywhere" -- see
    // pogohome_api::ResolveInterfaceAddress()'s and pogohome_api::Server::
    // Start()'s own doc comments for the exact failure conditions each
    // covers). Left entirely unconstructed (gApiServer stays null) when
    // --api-interface was not given -- this program's behavior is then
    // byte-for-byte what it was before this option existed.
    if (!gAppConfig.api_interface.empty()) {
        pogohome_api::ResolvedAddress resolved;
        std::string resolve_error;
        if (!pogohome_api::ResolveInterfaceAddress(gAppConfig.api_interface, resolved, resolve_error)) {
            fprintf(stderr, "%s: --api-interface: %s\n", argv[0], resolve_error.c_str());
            return -1;
        }

        pogohome_api::ServerConfig api_config;
        api_config.bind_address = resolved.numeric;
        api_config.address_family = resolved.family;
        api_config.port = gAppConfig.api_port;

        gApiServer = std::make_unique<pogohome_api::Server>(gRegistry, api_config);
        if (!gApiServer->Start()) {
            fprintf(stderr,
                    "%s: PogoHome Local API v1 server failed to start on interface '%s' -- see the error above\n",
                    argv[0], gAppConfig.api_interface.c_str());
            return -1;
        }
    }

    matter_bridge::EndpointStore endpoint_store(gAppConfig.state_dir);
    matter_bridge::LoadResult load_result = endpoint_store.load();
    if (load_result == matter_bridge::LoadResult::Corrupt) {
        fprintf(stderr,
                "%s: %s/endpoint_map.bin exists but failed its integrity check -- refusing to start rather than "
                "silently reassigning module_uuid -> Matter endpoint id mappings (a previously-bound controller "
                "could end up bound to the wrong device). Inspect or move this file aside by hand, then restart.\n",
                argv[0], gAppConfig.state_dir.c_str());
        return -1;
    }

    matter_bridge::BridgeManager bridge_manager(endpoint_store);
    gBridgeManager = &bridge_manager;

    ingest::ServerConfig ingest_config;
    ingest_config.interface_name = gAppConfig.interface;
    ingest_config.port = gAppConfig.port;

    gIngestThread = std::thread([&ingest_config]() {
        if (!ingest::run_server(ingest_config, gRegistry, &gStopIngest)) {
            fprintf(stderr, "pogo_matter_bridge: ingest CoAP server failed to start -- see the error above\n");
        }
    });

    gWatcherThread = std::thread(RegistryWatcherThreadMain);

    // Blocks until connectedhomeip's own shutdown handling (SIGINT/SIGTERM
    // on the main thread, see this file's own top comment) tears the
    // Matter stack down; ApplicationShutdown() above runs during that and
    // flips both stop flags before this call returns.
    ChipLinuxAppMainLoop();

    gStopIngest.store(true, std::memory_order_relaxed);
    gStopWatcher.store(true, std::memory_order_relaxed);
    // Stopped here (main thread, after the Matter event loop itself has
    // already torn down) rather than in ApplicationShutdown() -- see that
    // function's own comment. Bounded time (see pogohome_api::Server::
    // Stop()'s own doc comment), never a no-op if gApiServer is set: joins
    // every worker thread this API server ever spawned.
    if (gApiServer) {
        gApiServer->Stop();
    }
    if (gIngestThread.joinable()) {
        gIngestThread.join();
    }
    if (gWatcherThread.joinable()) {
        gWatcherThread.join();
    }

    return 0;
}
