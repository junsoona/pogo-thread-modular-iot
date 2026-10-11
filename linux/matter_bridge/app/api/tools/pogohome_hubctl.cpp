// pogohome-hubctl: the hub operator's side of settings sharing -- create a
// one-time pairing code for a new iPhone/iPad/Mac, list the devices that may
// read and change the shared home settings, and take that access away again.
// It works on the same files pogo_matter_bridge uses
// (<--state-dir>/pogohome/, see device_auth.hpp) and needs no running bridge
// and no restart: the bridge reads devices.json on every settings request,
// so a revocation applies to the very next request.
//
// Run it as the account the bridge runs as -- the owner of <--state-dir>/pogohome/ -- and not
// blindly with sudo: the files it writes there are 0600 and owned by whoever runs it, so a copy
// written by root locks a bridge running as a normal user out. E.g.
//   sudo -u <bridge-user> pogohome-hubctl --state-dir /var/lib/pogo-matter-bridge pair
// or, when the bridge runs as your own account, the same without "sudo -u <bridge-user>".
//
// The pairing code is printed to this terminal only. It is never written to
// a log, never announced over Bonjour, and it stops working after one use,
// after its lifetime (10 minutes by default), or after 5 wrong attempts.
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <openssl/pem.h>
#include <openssl/x509.h>

#include "pogohome_api/build_info.hpp"
#include "pogohome_api/atomic_file.hpp"
#include "pogohome_api/crypto_util.hpp"
#include "pogohome_api/device_auth.hpp"
#include "pogohome_api/settings_store.hpp"
#include "pogohome_api/tls_identity.hpp"

using namespace pogohome_api;

namespace {

constexpr const char kSubdirectory[] = "pogohome";

void PrintUsage(FILE *out)
{
    std::fprintf(out,
                 "Usage: pogohome-hubctl --state-dir <dir> <command>\n"
                 "       pogohome-hubctl --version\n"
                 "\n"
                 "  <dir> is pogo_matter_bridge's --state-dir (e.g. /var/lib/pogo-matter-bridge).\n"
                 "\n"
                 "Commands:\n"
                 "  pair [--minutes N]   Create a one-time pairing code for a new device (default 10 minutes,\n"
                 "                       at most 60). Replaces a code that is still waiting.\n"
                 "  pairing-status       Whether a pairing code is waiting, and until when.\n"
                 "  cancel-pairing       Remove the waiting pairing code.\n"
                 "  devices              List the devices that may read and change the shared settings.\n"
                 "  revoke <device-id>   Take one device's access away (applies to its next request).\n"
                 "  revoke-all           Take every device's access away.\n"
                 "  fingerprint          SHA-256 of the hub's TLS certificate (what paired devices pin).\n"
                 "  settings             Check the shared settings file and summarize it.\n");
}

int64_t NowUnixMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string FormatTime(int64_t unix_ms)
{
    if (unix_ms <= 0) {
        return "-";
    }
    time_t seconds = static_cast<time_t>(unix_ms / 1000);
    struct tm local{};
    localtime_r(&seconds, &local);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &local);
    return buf;
}

int Fail(const std::string &message)
{
    std::fprintf(stderr, "pogohome-hubctl: %s\n", message.c_str());
    return 1;
}

int CommandPair(DeviceAuth &auth, int argc, char **argv, int index)
{
    int64_t minutes = 10;
    for (int i = index; i < argc; i++) {
        if (std::strcmp(argv[i], "--minutes") == 0 && i + 1 < argc) {
            char *end = nullptr;
            long parsed = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || parsed < 1 || parsed > 60) {
                return Fail("--minutes must be 1..60");
            }
            minutes = parsed;
        } else {
            return Fail(std::string("unknown argument: ") + argv[i]);
        }
    }
    std::string code;
    int64_t expires_at = 0;
    std::string error;
    if (!auth.CreatePairingCode(NowUnixMs(), minutes * 60 * 1000, code, expires_at, error)) {
        return Fail(error);
    }
    std::printf("Pairing code:  %s\n", FormatPairingCode(code).c_str());
    std::printf("Valid until:   %s (one device, at most %d wrong attempts)\n", FormatTime(expires_at).c_str(),
                kMaxPairingFailures);
    std::printf("\nIn Jogakbo Home: Settings > Hub Settings Sharing > Connect with Code.\n");
    return 0;
}

int CommandPairingStatus(DeviceAuth &auth)
{
    PairingStatus status;
    std::string error;
    if (!auth.GetPairingStatus(NowUnixMs(), status, error)) {
        return Fail(error);
    }
    if (!status.active) {
        std::printf("No pairing code is waiting.\n");
    } else {
        std::printf("A pairing code is waiting until %s (%d wrong attempt(s) so far).\n",
                    FormatTime(status.expires_at_unix_ms).c_str(), status.failed_attempts);
    }
    return 0;
}

int CommandDevices(DeviceAuth &auth)
{
    std::vector<DeviceRecord> devices;
    std::string error;
    if (!auth.ListDevices(devices, error)) {
        return Fail(error);
    }
    if (devices.empty()) {
        std::printf("No devices are paired.\n");
        return 0;
    }
    std::printf("%-16s  %-25s  %-25s  %s\n", "DEVICE ID", "PAIRED", "LAST USED (~10 min)", "NAME");
    for (const auto &d : devices) {
        std::printf("%-16s  %-25s  %-25s  %s\n", d.id.c_str(), FormatTime(d.created_at_unix_ms).c_str(),
                    FormatTime(d.last_used_at_unix_ms).c_str(), d.name.c_str());
    }
    return 0;
}

int CommandFingerprint(const std::string &dir)
{
    std::string pem;
    std::string error;
    ReadFileResult read = ReadWholeFile(dir + "/" + kTlsCertFileName, 16 * 1024, pem, error);
    if (read == ReadFileResult::Missing) {
        return Fail("no TLS certificate yet -- start pogo_matter_bridge with --api-interface once to create it");
    }
    if (read == ReadFileResult::Error) {
        return Fail(error);
    }
    BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    X509 *cert = bio != nullptr ? PEM_read_bio_X509(bio, nullptr, nullptr, nullptr) : nullptr;
    BIO_free(bio);
    if (cert == nullptr) {
        return Fail("the TLS certificate file is damaged");
    }
    unsigned char *der = nullptr;
    int len = i2d_X509(cert, &der);
    X509_free(cert);
    if (len <= 0) {
        return Fail("cannot encode the TLS certificate");
    }
    std::printf("%s\n", Sha256Hex(der, static_cast<size_t>(len)).c_str());
    OPENSSL_free(der);
    return 0;
}

int CommandSettings(const std::string &dir)
{
    SettingsStore store(dir);
    store.Load();
    SettingsSnapshot snapshot = store.Get();
    switch (snapshot.state) {
    case SettingsState::Uninitialized:
        std::printf("No shared settings yet (the first device to turn on sharing uploads its own).\n");
        return 0;
    case SettingsState::Unavailable:
        std::printf("UNAVAILABLE: %s\n", snapshot.unavailable_reason.c_str());
        std::printf("The bridge refuses settings requests until %s is repaired or moved aside.\n",
                    store.path().c_str());
        return 2;
    case SettingsState::Ready:
        break;
    }
    const HomeSettings &s = snapshot.settings;
    std::printf("Revision:   %s (saved %s)\n", snapshot.revision.c_str(),
                FormatTime(snapshot.updated_at_unix_ms).c_str());
    std::printf("Home name:  %s\n", s.has_home_name ? s.home_name.c_str() : "(the app's default)");
    std::printf("Rooms:      %zu\n", s.rooms.size());
    for (const auto &room : s.rooms) {
        if (room.name_kind == RoomNameKind::Custom) {
            std::printf("  %s  %s  [%s]\n", room.id.c_str(), room.name.c_str(), room.symbol.c_str());
        } else {
            std::printf("  %s  (app name for %s #%d)  [%s]\n", room.id.c_str(), room.suggestion_symbol.c_str(),
                        room.suggestion_ordinal, room.symbol.c_str());
        }
    }
    std::printf("Modules:    %zu\n", s.modules.size());
    for (const auto &module : s.modules) {
        std::printf("  %s  %s  room %s\n", module.uuid.c_str(), module.has_name ? module.name.c_str() : "(product name)",
                    module.has_room ? module.room_id.c_str() : "(unspecified)");
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    // Before anything else: no state directory is opened for this.
    if (pogohome_api::WantsVersion(argc, argv)) {
        std::printf("%s\n", pogohome_api::VersionLine("pogohome-hubctl").c_str());
        return 0;
    }
    std::string state_dir;
    int index = 1;
    while (index < argc && argv[index][0] == '-') {
        if (std::strcmp(argv[index], "--state-dir") == 0 && index + 1 < argc) {
            state_dir = argv[index + 1];
            index += 2;
        } else if (std::strcmp(argv[index], "--help") == 0 || std::strcmp(argv[index], "-h") == 0) {
            PrintUsage(stdout);
            return 0;
        } else {
            PrintUsage(stderr);
            return 1;
        }
    }
    if (state_dir.empty() || index >= argc) {
        PrintUsage(stderr);
        return 1;
    }
    const std::string command = argv[index++];
    const std::string dir = state_dir + "/" + kSubdirectory;

    std::string error;
    if (!EnsurePrivateDirectory(dir, error)) {
        return Fail(error);
    }
    DeviceAuth auth(dir);

    if (command == "pair") {
        return CommandPair(auth, argc, argv, index);
    }
    if (index != argc && command != "revoke") {
        PrintUsage(stderr);
        return 1;
    }
    if (command == "pairing-status") {
        return CommandPairingStatus(auth);
    }
    if (command == "cancel-pairing") {
        if (!auth.CancelPairing(error)) {
            return Fail(error);
        }
        std::printf("No pairing code is waiting now.\n");
        return 0;
    }
    if (command == "devices") {
        return CommandDevices(auth);
    }
    if (command == "revoke") {
        if (index + 1 != argc) {
            PrintUsage(stderr);
            return 1;
        }
        bool found = false;
        if (!auth.RevokeDevice(argv[index], found, error)) {
            return Fail(error);
        }
        if (!found) {
            return Fail(std::string("no paired device has id ") + argv[index] + " (see `devices`)");
        }
        std::printf("Device %s can no longer read or change the shared settings.\n", argv[index]);
        return 0;
    }
    if (command == "revoke-all") {
        size_t revoked = 0;
        if (!auth.RevokeAllDevices(revoked, error)) {
            return Fail(error);
        }
        std::printf("Removed access for %zu device(s).\n", revoked);
        return 0;
    }
    if (command == "fingerprint") {
        return CommandFingerprint(dir);
    }
    if (command == "settings") {
        return CommandSettings(dir);
    }
    PrintUsage(stderr);
    return 1;
}
