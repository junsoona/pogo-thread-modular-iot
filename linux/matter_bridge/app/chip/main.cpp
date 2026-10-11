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

#include <net/if.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>

#include "chip_endpoint.hpp"
#include "ingest/registry.hpp"
#include "ingest/server.hpp"
#include "matter_bridge/bridge_manager.hpp"
#include "matter_bridge/endpoint_store.hpp"
#include "pogohome_api/build_info.hpp"
#include "pogohome_api/atomic_file.hpp"
#include "pogohome_api/commissioner.hpp"
#include "pogohome_api/device_auth.hpp"
#include "pogohome_api/enrollment.hpp"
#include "pogohome_api/http_server.hpp"
#include "pogohome_api/hub_identity.hpp"
#include "pogohome_api/interface_address.hpp"
#include "pogohome_api/mdns_advertiser.hpp"
#include "pogohome_api/power_base_management.hpp"
#include "pogohome_api/settings_store.hpp"
#include "pogohome_api/tls_identity.hpp"

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
    // Bonjour `_jogakbo._tcp` advertisement of the API (see
    // pogohome_api/mdns_advertiser.hpp) -- on whenever the API is, unless
    // --api-no-advertise.
    bool api_advertise = true;
    // Home settings sharing over the API's TLS (see
    // docs/pogohome-api-v1.md "Home settings") -- on whenever the API is,
    // unless --api-no-settings.
    bool api_settings = true;
    // Power Base enrollment (docs/pogohome-api-v1.md "Power Base
    // enrollment") -- off unless --api-enroll-helper names the root-run
    // OTBR helper, which the bridge runs as `sudo -n -- <path>`.
    std::string api_enroll_helper;
    int api_enroll_window_s = 120;
};

AppConfig gAppConfig;

constexpr uint16_t kOptionInterface = 0xFF10;
constexpr uint16_t kOptionPort = 0xFF11;
constexpr uint16_t kOptionStateDir = 0xFF12;
constexpr uint16_t kOptionApiInterface = 0xFF13;
constexpr uint16_t kOptionApiPort = 0xFF14;
constexpr uint16_t kOptionApiNoAdvertise = 0xFF15;
constexpr uint16_t kOptionApiNoSettings = 0xFF16;
constexpr uint16_t kOptionApiEnrollHelper = 0xFF17;
constexpr uint16_t kOptionApiEnrollWindow = 0xFF18;

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
    case kOptionApiNoAdvertise:
        gAppConfig.api_advertise = false;
        return true;
    case kOptionApiNoSettings:
        gAppConfig.api_settings = false;
        return true;
    case kOptionApiEnrollHelper:
        if (value[0] != '/') {
            PrintArgError("%s: --api-enroll-helper must be an absolute path\n", program);
            return false;
        }
        gAppConfig.api_enroll_helper = value;
        return true;
    case kOptionApiEnrollWindow: {
        int parsed = atoi(value);
        if (parsed < 30 || parsed > 300) {
            PrintArgError("%s: --api-enroll-window must be 30..300 seconds\n", program);
            return false;
        }
        gAppConfig.api_enroll_window_s = parsed;
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
    {"api-no-advertise", chip::ArgParser::kNoArgument, kOptionApiNoAdvertise},
    {"api-no-settings", chip::ArgParser::kNoArgument, kOptionApiNoSettings},
    {"api-enroll-helper", chip::ArgParser::kArgumentRequired, kOptionApiEnrollHelper},
    {"api-enroll-window", chip::ArgParser::kArgumentRequired, kOptionApiEnrollWindow},
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
    "       --api-interface.\n"
    "  --api-no-advertise\n"
    "       Do not announce the API over Bonjour/mDNS. By default an enabled API is advertised as _jogakbo._tcp\n"
    "       on --api-interface through the host's avahi-daemon while it can take requests (see\n"
    "       docs/pogohome-api-v1.md). Without avahi the API still runs, unadvertised.\n"
    "  --api-no-settings\n"
    "       Do not offer home settings sharing. By default an enabled API also serves the shared home settings\n"
    "       (home name, rooms, sensor names and rooms) to devices paired with `pogohome-hubctl pair`, over TLS on\n"
    "       the same port, with its key, certificate, settings and device list under <--state-dir>/pogohome/.\n"
    "  --api-enroll-helper <absolute-path>\n"
    "       OPTIONAL. Enables Power Base enrollment for paired devices (TLS, bearer token) and names the\n"
    "       root-run OTBR helper (pogohome-otbr-helper) this bridge runs as `sudo -n -- <path>`. Needs\n"
    "       --api-interface and settings sharing, and an operator-installed sudoers rule for exactly that path\n"
    "       (see docs/pogohome-api-v1.md \"Power Base enrollment\"). Omitted (the default): no enrollment.\n"
    "  --api-enroll-window <seconds>\n"
    "       How long one enrollment allows the Power Base's Joiner, 30..300 (default: 120).\n",
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
// Null unless the API is enabled, advertising was not turned off and
// libavahi-client could be loaded. Declared after gApiServer so that, on any
// exit path, it is destroyed (and withdraws the record) before the server.
std::unique_ptr<pogohome_api::MdnsAdvertiser> gApiAdvertiser;
// Null unless --api-enroll-helper was given. Stopped after gApiServer, so
// its Joiner entry is removed once no request can start another session.
std::shared_ptr<pogohome_api::EnrollmentManager> gEnrollment;
// Power Base management (docs/power-base-management.md): created with
// enrollment, read by the ingest thread through ingest::ServerConfig.
std::shared_ptr<pogohome_api::PowerBaseManagement> gPowerBaseManagement;

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
            // Urgent: a brand-new endpoint, a Reachable (online<->offline)
            // transition, or a confirmed removal (this follow-up: POGO
            // auto-registration/removal lifecycle -- a removed module_uuid
            // must have its Matter endpoint torn down promptly, never held
            // back behind the SAMPLE-only rate limit) -- see
            // matter_bridge::BridgeManager::TryBeginSchedule()'s own doc
            // comment.
            bool urgent = update.is_new_endpoint || update.reachable.has_value() || update.remove_endpoint;
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
    // Under systemd stdout is a pipe, so stdio block-buffers it: the
    // "pogohome_api: enrollment ..." and "[heartbeat] ..." lines (stdout)
    // could reach the journal late -- only when something else flushed --
    // while stderr lines appeared at once. Line buffering writes each line
    // as it is printed. No line printed to stdout contains a PSKd.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    // `pogo_matter_bridge --version`: which build this is (docs/versioning.md).
    // Answered before the Matter stack, the state directory or any socket is
    // touched, so it is safe to run next to a running bridge.
    if (pogohome_api::WantsVersion(argc, argv)) {
        printf("%s\n", pogohome_api::VersionLine("pogo_matter_bridge").c_str());
        return 0;
    }

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
    if (!gAppConfig.api_enroll_helper.empty()) {
        // Enrollment rides on settings sharing's TLS and paired devices.
        if (gAppConfig.api_interface.empty() || !gAppConfig.api_settings) {
            fprintf(stderr, "%s: --api-enroll-helper needs --api-interface and settings sharing (no --api-no-settings)\n",
                    argv[0]);
            return -1;
        }
        struct stat st{};
        if (stat(gAppConfig.api_enroll_helper.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
            access(gAppConfig.api_enroll_helper.c_str(), X_OK) != 0) {
            fprintf(stderr, "%s: --api-enroll-helper %s is not an executable file\n", argv[0],
                    gAppConfig.api_enroll_helper.c_str());
            return -1;
        }
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

        // The hub's stable identity, served by /api/v1/health and in the
        // Bonjour TXT record (see pogohome_api/hub_identity.hpp). A damaged
        // file stops startup, like a damaged endpoint_map.bin below: a new id
        // would make every client that remembers this hub stop recognizing it.
        std::string hub_id;
        std::string hub_id_error;
        pogohome_api::HubIdLoadResult hub_id_result =
            pogohome_api::LoadOrCreateHubId(gAppConfig.state_dir, hub_id, hub_id_error);
        if (hub_id_result == pogohome_api::HubIdLoadResult::Error) {
            fprintf(stderr, "%s: hub_id: %s\n", argv[0], hub_id_error.c_str());
            return -1;
        }
        fprintf(stdout, "pogohome_api: hub_id %s (%s)\n", hub_id.c_str(),
                hub_id_result == pogohome_api::HubIdLoadResult::Created ? "created" : "loaded");

        pogohome_api::ServerConfig api_config;
        api_config.bind_address = resolved.numeric;
        api_config.address_family = resolved.family;
        api_config.port = gAppConfig.api_port;
        api_config.hub_id = hub_id;
        // Health names the build that answers (docs/versioning.md).
        api_config.report_build = true;

        // Home settings sharing. Anything wrong here (a damaged TLS key or
        // settings file) turns off only the settings routes -- they answer
        // 503 and health says `settings_state: "unavailable"` -- while the
        // sensor API, Bonjour and Matter start as always. Nothing damaged is
        // ever replaced; see tls_identity.hpp and settings_store.hpp.
        if (gAppConfig.api_settings) {
            api_config.settings_requested = true;
            const std::string settings_dir = gAppConfig.state_dir + "/pogohome";
            std::string settings_error;
            if (!pogohome_api::EnsurePrivateDirectory(settings_dir, settings_error)) {
                fprintf(stderr, "pogohome_api: settings sharing unavailable: %s\n", settings_error.c_str());
            } else {
                std::shared_ptr<pogohome_api::TlsContext> tls;
                pogohome_api::TlsIdentityResult tls_result =
                    pogohome_api::LoadOrCreateTlsIdentity(settings_dir, "Jogakbo Hub " + hub_id.substr(0, 8), tls,
                                                          settings_error);
                if (tls_result == pogohome_api::TlsIdentityResult::Error) {
                    fprintf(stderr, "pogohome_api: settings sharing unavailable: %s\n", settings_error.c_str());
                } else {
                    auto store = std::make_shared<pogohome_api::SettingsStore>(settings_dir);
                    store->Load();
                    pogohome_api::SettingsSnapshot settings = store->Get();
                    if (settings.state == pogohome_api::SettingsState::Unavailable) {
                        fprintf(stderr,
                                "pogohome_api: shared settings unavailable (%s) -- settings requests are refused "
                                "until %s is repaired or moved aside; it is never replaced automatically\n",
                                settings.unavailable_reason.c_str(), store->path().c_str());
                    }
                    api_config.settings = store;
                    api_config.device_auth = std::make_shared<pogohome_api::DeviceAuth>(settings_dir);
                    api_config.tls = tls;
                    // The fingerprint is public (every paired device pins it);
                    // it is logged so an operator can compare it.
                    fprintf(stdout, "pogohome_api: settings sharing on, TLS certificate %s (SHA-256 %s), settings %s\n",
                            tls_result == pogohome_api::TlsIdentityResult::Created ? "created" : "loaded",
                            tls->certificate_sha256().c_str(),
                            settings.state == pogohome_api::SettingsState::Ready
                                ? settings.revision.c_str()
                                : (settings.state == pogohome_api::SettingsState::Uninitialized ? "not created yet"
                                                                                                 : "unavailable"));
                }
            }
        }

        // Power Base enrollment. Nothing runs as root and nothing touches the
        // OTBR until a paired device starts a session; the helper is not
        // probed here.
        if (!gAppConfig.api_enroll_helper.empty()) {
            api_config.enrollment_requested = true;

            // Power Base management rides on enrollment: a board gets its
            // management credential when it is enrolled, and is unenrolled
            // through the removal routes. It needs the same private state
            // directory and TLS as settings sharing. A damaged state file
            // turns off only this (the removal routes answer 503, sessions
            // that ask for a credential fail); it is never replaced.
            api_config.management_requested = true;
            if (api_config.tls) {
                pogohome_api::PowerBaseManagementConfig management;
                management.directory = gAppConfig.state_dir + "/pogohome";
                management.hub_id = hub_id;
                management.enrollment_in_progress = [](const std::string &eui64) {
                    return gEnrollment && gEnrollment->InProgressFor(eui64);
                };
                // The room clean-up of a removed Base's modules. It reports
                // failure instead of only logging it: management keeps the
                // clean-up in its state file and retries it (also after a
                // restart) until this returns true.
                // LOCATE (docs/power-base-management.md, section 12): where a
                // Power Base that lost this hub finds it again. The address
                // is read from the Thread interface when it is asked for --
                // an OTBR whose Mesh-Local EID changed is described as it is
                // now -- and the port is the one the ingest listener below
                // is really started on.
                {
                    const std::string thread_interface = gAppConfig.interface;
                    const uint16_t ingest_port = gAppConfig.port;
                    management.ingest_endpoint = [thread_interface, ingest_port](std::array<uint8_t, 16> &address,
                                                                                 uint16_t &port) {
                        port = ingest_port;
                        return pogohome_api::ResolveThreadMeshLocalEid(thread_interface, address);
                    };
                }
                management.unassign_rooms = pogohome_api::MakeRoomUnassigner(api_config.settings, []() {
                    using namespace std::chrono;
                    return static_cast<int64_t>(
                        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
                });
                // What a clean-up is bound to: the room each module is in at
                // the moment its Base leaves the list (read from the settings
                // held in memory, under management's lock).
                management.read_rooms = pogohome_api::MakeRoomReader(api_config.settings);
                gPowerBaseManagement =
                    std::make_shared<pogohome_api::PowerBaseManagement>(gRegistry, std::move(management));
                std::string management_error;
                if (!gPowerBaseManagement->Load(management_error)) {
                    fprintf(stderr,
                            "pogohome_api: Power Base management unavailable (%s) -- removal requests are refused "
                            "until %s is repaired or moved aside; it is never replaced automatically\n",
                            management_error.c_str(), gPowerBaseManagement->path().c_str());
                } else {
                    fprintf(stdout, "pogohome_api: Power Base management on (%zu Power Base(s) with a management "
                                    "credential)\n",
                            gPowerBaseManagement->ManagedPowerBases().size());
                }
                // Retries pending settings clean-ups (none of that work runs
                // on the ingest or HTTP threads). Harmless when unavailable.
                gPowerBaseManagement->StartCleanupWorker();
                api_config.management = gPowerBaseManagement;
            }

            pogohome_api::HelperCommissionerConfig helper;
            helper.command = {"/usr/bin/sudo", "-n", "--", gAppConfig.api_enroll_helper};
            pogohome_api::EnrollmentConfig enrollment;
            enrollment.window = std::chrono::seconds(gAppConfig.api_enroll_window_s);
            // The port the ingest CoAP listener is really started on below
            // (--port, the same value handed to ingest::Server). A session
            // that delivers a bridge endpoint reports this, with the OTBR's
            // own Mesh-Local EID read through the helper at that time.
            enrollment.bridge_port = gAppConfig.port;
            enrollment.management = gPowerBaseManagement;
            gEnrollment = std::make_shared<pogohome_api::EnrollmentManager>(
                gRegistry, std::make_shared<pogohome_api::HelperCommissioner>(helper), enrollment);
            api_config.enrollment = gEnrollment;
            fprintf(stdout, "pogohome_api: Power Base enrollment on (%s via sudo, %d s window)%s\n",
                    gAppConfig.api_enroll_helper.c_str(), gAppConfig.api_enroll_window_s,
                    api_config.tls ? "" : " -- but settings sharing is unavailable, so it answers 503");
        }

        gApiServer = std::make_unique<pogohome_api::Server>(gRegistry, api_config);
        if (!gApiServer->Start()) {
            fprintf(stderr,
                    "%s: PogoHome Local API v1 server failed to start on interface '%s' -- see the error above\n",
                    argv[0], gAppConfig.api_interface.c_str());
            return -1;
        }

        // Advertised only now that the server is listening; see
        // pogohome_api/mdns_advertiser.hpp for when it is withdrawn again.
        if (gAppConfig.api_advertise) {
            pogohome_api::AvahiApi avahi;
            std::string avahi_error;
            if (!pogohome_api::LoadSystemAvahiApi(avahi, avahi_error)) {
                fprintf(stderr, "pogohome_api: Bonjour: not advertising (%s); the API is reachable by address only\n",
                        avahi_error.c_str());
            } else {
                char hostname[256] = {};
                gethostname(hostname, sizeof(hostname) - 1);

                pogohome_api::AdvertiserConfig advert;
                advert.interface_name = gAppConfig.api_interface;
                advert.address_family = resolved.family;
                advert.port = gApiServer->bound_port();
                advert.service_name = pogohome_api::DefaultBonjourServiceName(hostname);
                advert.txt = pogohome_api::BuildBonjourTxt(hub_id);
                const std::string ifname = gAppConfig.api_interface;
                const pogohome_api::ResolvedAddress bound = resolved;
                advert.available = [ifname, bound]() {
                    return gApiServer && gApiServer->accepting() &&
                           pogohome_api::InterfaceHasAddress(ifname, bound.numeric, bound.family);
                };
                advert.interface_index = [ifname]() { return if_nametoindex(ifname.c_str()); };

                gApiAdvertiser = std::make_unique<pogohome_api::MdnsAdvertiser>(avahi, advert);
                if (!gApiAdvertiser->Start()) {
                    gApiAdvertiser.reset();
                }
            }
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
    // The authenticated management resource (docs/power-base-management.md).
    // Without management it answers 4.04, and every data resource is
    // handled exactly as before.
    if (gPowerBaseManagement) {
        std::shared_ptr<pogohome_api::PowerBaseManagement> management = gPowerBaseManagement;
        ingest_config.management = [management](const std::vector<uint8_t> &request) {
            return management->HandleDatagram(request);
        };
        // A Power Base that lost this hub asks on PB_MGMT_LOCATE_PORT. When
        // --port is another port, that one is opened as well and answers
        // LOCATE only (ingest/server.hpp).
        ingest_config.locate = [management](const std::vector<uint8_t> &request) {
            return management->HandleLocateDatagram(request);
        };
        ingest_config.locate_port = PB_MGMT_LOCATE_PORT;
    }

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
    // Withdraw the Bonjour record first, so nothing announces a server that
    // is about to stop answering.
    if (gApiAdvertiser) {
        gApiAdvertiser->Stop();
    }
    if (gApiServer) {
        gApiServer->Stop();
    }
    // Ends a running enrollment session and removes its Joiner entry
    // (bounded by the helper's timeouts; the OTBR's own entry timeout closes
    // it anyway if this process is killed first).
    if (gEnrollment) {
        gEnrollment->Stop();
    }
    if (gIngestThread.joinable()) {
        gIngestThread.join();
    }
    if (gWatcherThread.joinable()) {
        gWatcherThread.join();
    }

    return 0;
}
