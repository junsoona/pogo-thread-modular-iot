#include "pogohome_api/otbr_helper_protocol.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>

namespace pogohome_api {

namespace {

bool IsLowerHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

const char *OpName(HelperOp op)
{
    switch (op) {
    case HelperOp::CommissionerState:
        return "commissioner-state";
    case HelperOp::CommissionerStart:
        return "commissioner-start";
    case HelperOp::JoinerAdd:
        return "joiner-add";
    case HelperOp::JoinerRemove:
        return "joiner-remove";
    case HelperOp::MeshLocalEid:
        return "mesh-local-eid";
    }
    return "";
}

std::vector<std::string> SplitSpaces(const std::string &s)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t sp = s.find(' ', pos);
        if (sp == std::string::npos) {
            sp = s.size();
        }
        out.push_back(s.substr(pos, sp - pos));
        pos = sp + 1;
    }
    return out;
}

// Decimal 1..999 without sign or leading zeros.
bool ParseSmallInt(const std::string &s, int &out)
{
    if (s.empty() || s.size() > 3 || s[0] == '0') {
        return false;
    }
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

bool ValidRequest(const HelperRequest &r)
{
    switch (r.op) {
    case HelperOp::CommissionerState:
    case HelperOp::CommissionerStart:
    case HelperOp::MeshLocalEid:
        return r.eui64.empty() && r.pskd.empty() && r.timeout_s == 0;
    case HelperOp::JoinerAdd:
        return IsValidEui64(r.eui64) && IsValidPskd(r.pskd) && r.timeout_s >= kMinJoinerTimeoutSeconds &&
               r.timeout_s <= kMaxJoinerTimeoutSeconds;
    case HelperOp::JoinerRemove:
        return IsValidEui64(r.eui64) && r.pskd.empty() && r.timeout_s == 0;
    }
    return false;
}

} // namespace

bool IsValidEui64(const std::string &eui64)
{
    if (eui64.size() != 16) {
        return false;
    }
    bool nonzero = false;
    for (char c : eui64) {
        if (!IsLowerHexDigit(c)) {
            return false;
        }
        nonzero = nonzero || c != '0';
    }
    return nonzero;
}

bool IsValidPskd(const std::string &pskd)
{
    if (pskd.size() < kMinPskdLength || pskd.size() > kMaxPskdLength) {
        return false;
    }
    for (char c : pskd) {
        const bool digit = c >= '0' && c <= '9';
        const bool upper = c >= 'A' && c <= 'Z';
        if (!(digit || upper) || c == 'I' || c == 'O' || c == 'Q' || c == 'Z') {
            return false;
        }
    }
    return true;
}

std::string BuildHelperRequest(const HelperRequest &request)
{
    if (!ValidRequest(request)) {
        return std::string();
    }
    std::string line = kHelperProtocolTag;
    line += ' ';
    line += OpName(request.op);
    if (request.op == HelperOp::JoinerAdd) {
        line += ' ' + request.eui64 + ' ' + std::to_string(request.timeout_s) + ' ' + request.pskd;
    } else if (request.op == HelperOp::JoinerRemove) {
        line += ' ' + request.eui64;
    }
    line += '\n';
    return line.size() <= kMaxHelperRequestBytes ? line : std::string();
}

bool ParseHelperRequest(const std::string &raw, HelperRequest &out)
{
    out = HelperRequest();
    if (raw.empty() || raw.size() > kMaxHelperRequestBytes || raw.back() != '\n') {
        return false;
    }
    // Every copy of the line may hold the PSKd; all are wiped on every path.
    std::string line = raw.substr(0, raw.size() - 1);
    std::vector<std::string> words;
    HelperRequest r;
    auto finish = [&](bool result) {
        if (result) {
            out = r;
        }
        SecureWipe(line);
        for (auto &w : words) {
            SecureWipe(w);
        }
        SecureWipe(r.pskd);
        return result;
    };
    for (char c : line) {
        // Printable ASCII and single spaces only: no control characters, no
        // second line.
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) {
            return finish(false);
        }
    }
    words = SplitSpaces(line);
    if (words.size() < 2 || words[0] != kHelperProtocolTag) {
        return finish(false);
    }
    const std::string op = words[1];
    if (op == "commissioner-state" && words.size() == 2) {
        r.op = HelperOp::CommissionerState;
    } else if (op == "commissioner-start" && words.size() == 2) {
        r.op = HelperOp::CommissionerStart;
    } else if (op == "mesh-local-eid" && words.size() == 2) {
        r.op = HelperOp::MeshLocalEid;
    } else if (op == "joiner-add" && words.size() == 5) {
        r.op = HelperOp::JoinerAdd;
        r.eui64 = words[2];
        if (!ParseSmallInt(words[3], r.timeout_s)) {
            return finish(false);
        }
        r.pskd = words[4];
    } else if (op == "joiner-remove" && words.size() == 3) {
        r.op = HelperOp::JoinerRemove;
        r.eui64 = words[2];
    } else {
        return finish(false);
    }
    return finish(ValidRequest(r));
}

OtCliOutput ParseOtCliOutput(const std::string &text)
{
    OtCliOutput out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            break; // an unfinished last line never decides anything
        }
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        while (line.compare(0, 2, "> ") == 0) {
            line.erase(0, 2);
        }
        if (line == ">") {
            continue;
        }
        if (line == "Done") {
            out.kind = OtCliOutput::Kind::Done;
            return out;
        }
        if (line.compare(0, 6, "Error ") == 0) {
            out.kind = OtCliOutput::Kind::Error;
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                size_t b = colon + 1;
                while (b < line.size() && line[b] == ' ') {
                    b++;
                }
                out.error_name = line.substr(b);
            }
            return out;
        }
        if (!line.empty()) {
            out.lines.push_back(line);
        }
    }
    out.kind = OtCliOutput::Kind::Incomplete;
    return out;
}

bool OtCliOutputComplete(const std::string &text)
{
    return ParseOtCliOutput(text).kind != OtCliOutput::Kind::Incomplete;
}

bool ParseHelperResponse(const std::string &text, bool &ok, std::vector<std::string> &words)
{
    words.clear();
    std::string line = text;
    if (!line.empty() && line.back() == '\n') {
        line.pop_back();
    }
    if (line.empty() || line.size() > kMaxHelperResponseBytes) {
        return false;
    }
    std::vector<std::string> parts = SplitSpaces(line);
    for (const auto &w : parts) {
        if (w.empty()) {
            return false;
        }
        for (char c : w) {
            if (!((c >= 'a' && c <= 'z') || c == '_')) {
                return false;
            }
        }
    }
    if (parts[0] == "ok" && parts.size() >= 2 && parts.size() <= 3) {
        ok = true;
    } else if (parts[0] == "err" && parts.size() == 2) {
        ok = false;
    } else {
        return false;
    }
    words.assign(parts.begin() + 1, parts.end());
    return true;
}

bool ParseMeshLocalEid(const std::string &text, unsigned char (*bytes)[16], std::string *canonical)
{
    // The shortest literal is "::" (refused below anyway); the longest is
    // eight groups of four digits with seven colons.
    if (text.size() < 2 || text.size() > 39) {
        return false;
    }
    for (char c : text) {
        // No '%' (zone id), '.', brackets, upper case, spaces or anything else.
        if (!(IsLowerHexDigit(c) || c == ':')) {
            return false;
        }
    }
    in6_addr addr{};
    if (inet_pton(AF_INET6, text.c_str(), &addr) != 1) {
        return false;
    }
    const unsigned char *a = addr.s6_addr;
    // Named one by one although "fd00::/8" below already excludes them: each
    // is something a caller might otherwise be handed by mistake.
    static const unsigned char zero16[16] = {0};
    if (std::memcmp(a, zero16, 16) == 0) {
        return false; // unspecified
    }
    if (std::memcmp(a, zero16, 15) == 0 && a[15] == 1) {
        return false; // loopback
    }
    if (std::memcmp(a, zero16, 10) == 0 && ((a[10] == 0xff && a[11] == 0xff) || (a[10] == 0 && a[11] == 0))) {
        return false; // IPv4-mapped / IPv4-compatible
    }
    if (a[0] == 0xff) {
        return false; // multicast
    }
    if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) {
        return false; // link-local
    }
    if (a[0] != 0xfd) {
        return false; // not where a Thread Mesh-Local Prefix can be
    }
    if (a[8] == 0 && a[9] == 0 && a[10] == 0 && a[11] == 0xff && a[12] == 0xfe && a[13] == 0) {
        return false; // an RLOC or ALOC, not the EID
    }
    char printed[INET6_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET6, &addr, printed, sizeof(printed)) == nullptr) {
        return false;
    }
    if (bytes != nullptr) {
        std::memcpy(*bytes, a, 16);
    }
    if (canonical != nullptr) {
        *canonical = printed;
    }
    return true;
}

bool ParseHelperMleidResponse(const std::string &text, std::string &address)
{
    address.clear();
    std::string line = text;
    if (!line.empty() && line.back() == '\n') {
        line.pop_back();
    }
    static const char kPrefix[] = "ok mleid ";
    const size_t prefix_len = sizeof(kPrefix) - 1;
    if (line.size() <= prefix_len || line.size() > kMaxHelperResponseBytes || line.compare(0, prefix_len, kPrefix) != 0) {
        return false;
    }
    std::string canonical;
    // The rest must be the address and nothing else: ParseMeshLocalEid takes
    // no space, no second word, no newline.
    if (!ParseMeshLocalEid(line.substr(prefix_len), nullptr, &canonical)) {
        return false;
    }
    address = canonical;
    return true;
}

void SecureWipe(std::string &s)
{
    volatile char *p = s.empty() ? nullptr : &s[0];
    for (size_t i = 0; i < s.size(); i++) {
        p[i] = 0;
    }
    s.clear();
}

} // namespace pogohome_api
