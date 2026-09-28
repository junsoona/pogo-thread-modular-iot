// pogohome_api::DeviceAuth -- pairing codes, the code-bound proofs, device
// tokens, expiry, retry limits and revocation (device_auth.hpp).
#include <string>
#include <vector>

#include "pogohome_api/crypto_util.hpp"
#include "pogohome_api/device_auth.hpp"
#include "test_check.hpp"
#include "test_temp_dir.hpp"

using namespace pogohome_api;
using test_support::MakeTempDir;
using test_support::ReadFile;
using test_support::RemoveTree;
using test_support::WriteFile;

namespace {

const char kHubId[] = "3f0c9a4e8b1d4c2a9e7f6b5a4d3c2b1a";
const std::string kCertFp = Sha256Hex(std::string("test-cert"));
const char kNonce[] = "000102030405060708090a0b0c0d0e0f";

ClaimRequest MakeClaim(const std::string &code, const std::string &name = "iPad",
                       const std::string &cert_fp = kCertFp)
{
    ClaimRequest r;
    r.device_name = name;
    r.client_nonce_hex = kNonce;
    r.proof_hex = PairingClientProof(code, kHubId, cert_fp, kNonce, name);
    return r;
}

std::string NewCode(DeviceAuth &auth, int64_t now, int64_t ttl = kDefaultPairingTtlMs)
{
    std::string code, error;
    int64_t expires = 0;
    auth.CreatePairingCode(now, ttl, code, expires, error);
    return code;
}

} // namespace

void RunDeviceAuthTests(int *failures)
{
    printf("\n-- scenario: pairing codes are typed forgivingly and proofs match the shared test vector --\n");
    {
        std::string code;
        API_CHECK(NormalizePairingCode("7kqm-3xwd-pa9t", code) && code == "7KQM3XWDPA9T",
                  "lower case and hyphens are accepted");
        API_CHECK(NormalizePairingCode(" 7KQM 3XWD PA9T ", code) && code == "7KQM3XWDPA9T", "spaces are ignored");
        API_CHECK(NormalizePairingCode("OIL1-0000-0000", code) && code == "0111" "00000000",
                  "O reads as 0 and I/L read as 1");
        API_CHECK(!NormalizePairingCode("7KQM-3XWD-PA9", code), "too few characters are rejected");
        API_CHECK(!NormalizePairingCode("7KQM-3XWD-PA9TX", code), "too many characters are rejected");
        API_CHECK(!NormalizePairingCode("7KQM-3XWD-PA9U", code), "U is not a code character");
        API_CHECK(FormatPairingCode("7KQM3XWDPA9T") == "7KQM-3XWD-PA9T", "codes are shown in groups of four");
        std::string generated = GeneratePairingCode();
        API_CHECK(NormalizePairingCode(generated, code) && code == generated, "a generated code is a valid code");
        API_CHECK(GeneratePairingCode() != generated, "two generated codes differ");

        // The same vector is checked by the app's HubPairingProofTests, so the
        // two independent implementations cannot drift apart.
        API_CHECK(kCertFp == "ac978e63f273ba9ee1132bfecc103c767c52512fd5b065cc0ee224f9ec49d6a2",
                  "test vector: certificate hash");
        API_CHECK(PairingClientProof("7KQM3XWDPA9T", kHubId, kCertFp, kNonce, "Jun\xEC\x9D\x98 iPhone") ==
                      "096c8f9cdd9997de40cb91226f7e34a89a8f06e4b8798878eeb49a178b3f3c99",
                  "test vector: client proof");
        API_CHECK(PairingServerProof("7KQM3XWDPA9T", kHubId, kCertFp, kNonce, "0123456789abcdef", std::string(43, 'A')) ==
                      "4d9e5bc4d035fbc41252d190a746e3dd14dae31ca3d713531967e20fd23994a0",
                  "test vector: server proof");
    }

    printf("\n-- scenario: a device pairs once with the right code, and its token then authenticates --\n");
    {
        std::string dir = MakeTempDir();
        DeviceAuth auth(dir);
        std::string code = NewCode(auth, 1000);
        API_CHECK(!code.empty(), "a pairing code is created");

        ClaimResponse response;
        std::string error;
        ClaimOutcome outcome = auth.Claim(MakeClaim(code), kHubId, kCertFp, 2000, response, error);
        API_CHECK(outcome == ClaimOutcome::Paired, "the right proof pairs the device");
        API_CHECK(IsLowerHex(response.device_id, 16) && response.token.size() == 43, "a device id and token are issued");
        API_CHECK(response.server_proof_hex ==
                      PairingServerProof(code, kHubId, kCertFp, kNonce, response.device_id, response.token),
                  "the hub proves it knows the code over the same certificate, nonce, id and token");

        DeviceRecord device;
        API_CHECK(auth.Authenticate("Bearer " + response.token, 3000, device, error) == AuthOutcome::Ok &&
                      device.id == response.device_id && device.name == "iPad",
                  "the token authenticates as that device");
        API_CHECK(auth.Authenticate("Bearer " + std::string(43, 'x'), 3000, device, error) == AuthOutcome::Invalid,
                  "another token does not");
        API_CHECK(auth.Authenticate(response.token, 3000, device, error) == AuthOutcome::Invalid,
                  "a token without the Bearer scheme does not");

        std::string devices_file, pairing_left;
        ReadFile(dir + "/devices.json", devices_file);
        API_CHECK(devices_file.find(response.token) == std::string::npos &&
                      devices_file.find(Sha256Hex(response.token)) != std::string::npos,
                  "devices.json keeps only the token's SHA-256, never the token");
        API_CHECK(test_support::FileMode(dir + "/devices.json") == 0600, "devices.json is only readable by its owner");
        API_CHECK(!ReadFile(dir + "/pairing.json", pairing_left), "the used code is gone");

        ClaimResponse again;
        API_CHECK(auth.Claim(MakeClaim(code, "Mac"), kHubId, kCertFp, 2500, again, error) == ClaimOutcome::NoActiveCode,
                  "the same code cannot pair a second device");
        RemoveTree(dir);
    }

    printf("\n-- scenario: a device that saw another certificate (an impostor in between) is refused --\n");
    {
        std::string dir = MakeTempDir();
        DeviceAuth auth(dir);
        std::string code = NewCode(auth, 1000);
        ClaimResponse response;
        std::string error;
        ClaimRequest via_impostor = MakeClaim(code, "iPad", Sha256Hex(std::string("impostor-cert")));
        API_CHECK(auth.Claim(via_impostor, kHubId, kCertFp, 2000, response, error) == ClaimOutcome::BadProof,
                  "a proof over another certificate's hash is rejected");
        API_CHECK(response.token.empty(), "no token is issued");
        API_CHECK(auth.Claim(MakeClaim(code), kHubId, kCertFp, 2100, response, error) == ClaimOutcome::Paired,
                  "the genuine device can still pair with the same code afterwards");
        RemoveTree(dir);
    }

    printf("\n-- scenario: wrong codes are limited, and the code is destroyed after the limit --\n");
    {
        std::string dir = MakeTempDir();
        DeviceAuth auth(dir);
        std::string code = NewCode(auth, 1000);
        ClaimResponse response;
        std::string error;
        for (int i = 0; i < kMaxPairingFailures; i++) {
            ClaimOutcome o = auth.Claim(MakeClaim("000000000000"), kHubId, kCertFp, 2000 + i, response, error);
            if (o != ClaimOutcome::BadProof) {
                API_CHECK(false, "every wrong code is BadProof");
            }
        }
        PairingStatus status;
        auth.GetPairingStatus(3000, status, error);
        API_CHECK(!status.active, "after 5 wrong codes no code is waiting any more");
        API_CHECK(auth.Claim(MakeClaim(code), kHubId, kCertFp, 3000, response, error) == ClaimOutcome::NoActiveCode,
                  "even the right code no longer works (the operator makes a new one)");
        RemoveTree(dir);
    }

    printf("\n-- scenario: an expired code does not pair --\n");
    {
        std::string dir = MakeTempDir();
        DeviceAuth auth(dir);
        std::string code = NewCode(auth, 1000, 60 * 1000);
        ClaimResponse response;
        std::string error;
        API_CHECK(auth.Claim(MakeClaim(code), kHubId, kCertFp, 1000 + 60 * 1000, response, error) ==
                      ClaimOutcome::Expired,
                  "a claim at the expiry time is refused as Expired");
        API_CHECK(auth.Claim(MakeClaim(code), kHubId, kCertFp, 1000, response, error) == ClaimOutcome::NoActiveCode,
                  "the expired code was removed");
        std::string over_limit;
        int64_t expires = 0;
        API_CHECK(!auth.CreatePairingCode(1000, kMaxPairingTtlMs + 1, over_limit, expires, error),
                  "a code lifetime over 60 minutes is refused");
        RemoveTree(dir);
    }

    printf("\n-- scenario: malformed claims are rejected before any code is touched --\n");
    {
        std::string dir = MakeTempDir();
        DeviceAuth auth(dir);
        std::string code = NewCode(auth, 1000);
        ClaimResponse response;
        std::string error;
        ClaimRequest bad = MakeClaim(code);
        bad.client_nonce_hex = "zz";
        API_CHECK(auth.Claim(bad, kHubId, kCertFp, 2000, response, error) == ClaimOutcome::InvalidRequest,
                  "a malformed nonce is InvalidRequest");
        ClaimRequest no_name = MakeClaim(code, "");
        API_CHECK(auth.Claim(no_name, kHubId, kCertFp, 2000, response, error) == ClaimOutcome::InvalidRequest,
                  "an empty device name is InvalidRequest");
        PairingStatus status;
        auth.GetPairingStatus(2000, status, error);
        API_CHECK(status.active && status.failed_attempts == 0, "malformed claims do not use up attempts");
        RemoveTree(dir);
    }

    printf("\n-- scenario: revoking a device takes its access away at once --\n");
    {
        std::string dir = MakeTempDir();
        DeviceAuth auth(dir);
        ClaimResponse a, b;
        std::string error;
        auth.Claim(MakeClaim(NewCode(auth, 1000), "iPhone"), kHubId, kCertFp, 1100, a, error);
        auth.Claim(MakeClaim(NewCode(auth, 1200), "Mac"), kHubId, kCertFp, 1300, b, error);
        std::vector<DeviceRecord> devices;
        API_CHECK(auth.ListDevices(devices, error) && devices.size() == 2, "two devices are listed");

        bool found = false;
        API_CHECK(auth.RevokeDevice(a.device_id, found, error) && found, "the iPhone is revoked");
        DeviceRecord device;
        API_CHECK(auth.Authenticate("Bearer " + a.token, 2000, device, error) == AuthOutcome::Invalid,
                  "the revoked token no longer authenticates");
        API_CHECK(auth.Authenticate("Bearer " + b.token, 2000, device, error) == AuthOutcome::Ok,
                  "the other device keeps its access");
        API_CHECK(auth.RevokeDevice("ffffffffffffffff", found, error) && !found, "an unknown id is reported as not found");
        size_t revoked = 0;
        API_CHECK(auth.RevokeAllDevices(revoked, error) && revoked == 1, "revoke-all removes the rest");
        API_CHECK(auth.Authenticate("Bearer " + b.token, 2000, device, error) == AuthOutcome::Invalid,
                  "after revoke-all no token works");
        RemoveTree(dir);
    }

    printf("\n-- scenario: a damaged devices.json fails closed and is never rewritten --\n");
    {
        std::string dir = MakeTempDir();
        DeviceAuth auth(dir);
        WriteFile(dir + "/devices.json", "{\"format\":\"pogohome-devices\",");
        DeviceRecord device;
        std::string error;
        API_CHECK(auth.Authenticate("Bearer " + std::string(43, 'A'), 1, device, error) == AuthOutcome::StorageError,
                  "authentication reports a storage error (it never lets a request through)");
        ClaimResponse response;
        std::string code = NewCode(auth, 1000);
        API_CHECK(auth.Claim(MakeClaim(code), kHubId, kCertFp, 1100, response, error) == ClaimOutcome::StorageError,
                  "pairing is refused rather than replacing the damaged list");
        std::string bytes;
        ReadFile(dir + "/devices.json", bytes);
        API_CHECK(bytes == "{\"format\":\"pogohome-devices\",", "the damaged file keeps its bytes");
        PairingStatus status;
        auth.GetPairingStatus(1100, status, error);
        API_CHECK(status.active, "the code is not used up by a pairing that could not be stored");
        RemoveTree(dir);
    }

    printf("\n-- scenario: at most kMaxRegisteredDevices devices --\n");
    {
        std::string dir = MakeTempDir();
        DeviceAuth auth(dir);
        ClaimResponse response;
        std::string error;
        for (size_t i = 0; i < kMaxRegisteredDevices; i++) {
            auth.Claim(MakeClaim(NewCode(auth, 1000), "D" + std::to_string(i)), kHubId, kCertFp, 1001, response, error);
        }
        API_CHECK(auth.Claim(MakeClaim(NewCode(auth, 1000), "one too many"), kHubId, kCertFp, 1001, response, error) ==
                      ClaimOutcome::TooManyDevices,
                  "the 33rd device is refused");
        RemoveTree(dir);
    }
}
