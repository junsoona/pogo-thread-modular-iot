// What pogo_matter_bridge and pogohome-otbr-helper (tools/pogohome_otbr_helper.cpp)
// say to each other, and the input checks both sides make on their own.
//
// The bridge runs as a normal user and cannot open the OTBR control socket
// (/run/openthread-wpan0.sock is root:root 0755). For Power Base enrollment
// it asks the helper, which the operator allows it to run as root through
// one exact sudoers rule, to do exactly one of four things:
//
//   commissioner-state                        read the Commissioner state
//   commissioner-start                        start it unless already active
//   joiner-add <eui64> <timeout_s> <pskd>     allow one exact EUI-64
//   joiner-remove <eui64>                     remove that one entry
//
// Nothing else exists: no free-form OTBR CLI command, no Dataset or Network
// Key read, no Commissioner stop, no wildcard Joiner, no shell. The request
// is one line on the helper's STDIN -- never in its arguments or environment
// -- because the PSKd must not show up in `ps`, /proc/<pid>/cmdline or a
// sudo log. The helper takes no arguments at all.
//
// Request:  "pogohome-otbr/1 <op>[ <args>]\n", at most kMaxHelperRequestBytes,
//           nothing after the newline.
// Response: one line on stdout, "ok <word>[ <word>]" or "err <code>". It
//           never contains the PSKd or raw OTBR output.
//
// This file has no OpenSSL or Matter dependency so the helper stays small.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pogohome_api {

constexpr const char kHelperProtocolTag[] = "pogohome-otbr/1";
constexpr size_t kMaxHelperRequestBytes = 128;
constexpr size_t kMaxHelperResponseBytes = 256;
// The Joiner entry's own lifetime on the OTBR. Bounded on both sides so an
// entry always closes by itself, even if the bridge dies before removing it.
constexpr int kMinJoinerTimeoutSeconds = 10;
constexpr int kMaxJoinerTimeoutSeconds = 300;
constexpr size_t kMinPskdLength = 6;
constexpr size_t kMaxPskdLength = 32;

// Exactly 16 lowercase hex digits, not all zero. No '*', no discerner.
bool IsValidEui64(const std::string &eui64);

// A Thread Joiner PSKd: 6..32 characters of 0-9 and A-Z except I, O, Q, Z
// (the same rule OpenThread applies). Uppercase only.
bool IsValidPskd(const std::string &pskd);

enum class HelperOp { CommissionerState, CommissionerStart, JoinerAdd, JoinerRemove };

struct HelperRequest {
    HelperOp op = HelperOp::CommissionerState;
    std::string eui64;    // JoinerAdd, JoinerRemove
    int timeout_s = 0;    // JoinerAdd
    std::string pskd;     // JoinerAdd
};

// "" if the request is not valid (the same checks ParseHelperRequest makes).
std::string BuildHelperRequest(const HelperRequest &request);
// Strict: the exact format above, every field re-validated.
bool ParseHelperRequest(const std::string &raw, HelperRequest &out);

// One OTBR CLI command's output, as ot-ctl prints it: any lines, then "Done"
// or "Error <n>: <Name>". Prompts ("> ") and '\r' are ignored.
struct OtCliOutput {
    enum class Kind { Incomplete, Done, Error };
    Kind kind = Kind::Incomplete;
    std::string error_name; // "NotFound", "InvalidState", ... for Kind::Error
    std::vector<std::string> lines; // before the Done/Error line
};
OtCliOutput ParseOtCliOutput(const std::string &text);
// True once `text` ends a command (a Done or Error line was printed).
bool OtCliOutputComplete(const std::string &text);

// The helper's one response line (without the newline), split into words.
// False unless it is "ok ..." or "err <code>" with only [a-z_] words.
bool ParseHelperResponse(const std::string &text, bool &ok, std::vector<std::string> &words);

// Overwrites the string's bytes before clearing it (for the PSKd).
void SecureWipe(std::string &s);

} // namespace pogohome_api
