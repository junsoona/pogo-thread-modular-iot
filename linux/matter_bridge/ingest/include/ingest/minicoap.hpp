// Minimal RFC 7252 (CoAP) subset: just enough to parse an incoming
// Confirmable POST (version/type/code/message-id/token/Uri-Path
// options/payload) and build back a matching ACK with a response code --
// nothing else this ingest program needs (no retransmission, no blockwise,
// no observe, no other option types).
//
// This is a deliberate, documented substitution for the "recommended"
// libcoap3 dependency in this follow-up's own design notes: it keeps this
// program buildable and testable with nothing beyond a C++17 compiler and
// CMake (no system package install required), while still speaking real
// enough CoAP-over-UDP for components/linux_bridge_exporter's actual
// Confirmable POSTs to be received and ACKed correctly. See
// docs/matter-bridge.md and this follow-up's final report for the
// libcoap3-swap note. The registry/session logic in registry.hpp/session.hpp
// is completely decoupled from this transport shim -- swapping it for a
// real libcoap3 server later only touches server.cpp/main.cpp.
//
// Correction pass (this follow-up, item 5): coap_parse() now distinguishes
// WHY a datagram failed to parse (Malformed vs. an unrecognized CRITICAL
// option, RFC 7252 5.4.1) so the caller can reply 4.00 vs. 4.02 instead of
// folding every parse failure into 4.00. Method (POST-only) and Uri-Path
// resource dispatch are NOT this layer's concern -- see server.cpp for the
// 4.05 Method Not Allowed check, which happens after a successful parse.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ingest {

enum class CoapType : uint8_t {
    Confirmable = 0,
    NonConfirmable = 1,
    Acknowledgement = 2,
    Reset = 3,
};

// (class << 5) | detail, matching real CoAP/OpenThread's own encoding
// exactly (components/power_base_transport's own OT_COAP_CODE_* macros use
// the identical layout).
constexpr uint8_t CoapCode(uint8_t cls, uint8_t detail)
{
    return static_cast<uint8_t>((cls << 5) | detail);
}
constexpr uint8_t kCoapCodePost = CoapCode(0, 2);
constexpr uint8_t kCoapCodeChanged = CoapCode(2, 4);            // 2.04
constexpr uint8_t kCoapCodeValid = CoapCode(2, 3);              // 2.03
constexpr uint8_t kCoapCodeContent = CoapCode(2, 5);            // 2.05, an answer with a payload
constexpr uint8_t kCoapCodeUnauthorized = CoapCode(4, 1);       // 4.01
constexpr uint8_t kCoapCodeForbidden = CoapCode(4, 3);          // 4.03
constexpr uint8_t kCoapCodeBadRequest = CoapCode(4, 0);         // 4.00
constexpr uint8_t kCoapCodeNotFound = CoapCode(4, 4);           // 4.04
constexpr uint8_t kCoapCodeMethodNotAllowed = CoapCode(4, 5);   // 4.05
constexpr uint8_t kCoapCodeBadOption = CoapCode(4, 2);          // 4.02
constexpr uint8_t kCoapCodePreconditionFailed = CoapCode(4, 12); // 4.12
constexpr uint8_t kCoapCodeServiceUnavailable = CoapCode(5, 3); // 5.03

// A CoAP option number is "critical" (RFC 7252 5.4.1) iff its LSB is 1 --
// an unrecognized critical option must reject the whole message (4.02); an
// unrecognized elective (even) option is safely ignored. This parser only
// ever recognizes Uri-Path (11, critical).
constexpr bool CoapOptionIsCritical(uint16_t option_number)
{
    return (option_number & 1) != 0;
}
constexpr uint16_t kCoapOptionUriPath = 11;

// Hard bounds this minimal parser enforces beyond the base RFC 7252 token
// length limit (8, already implied by the 4-bit TKL field) -- both
// generous for this program's own fixed, short "pogo/v1/..." resource
// paths, and small enough to bound per-datagram parse work/memory.
constexpr size_t kCoapMaxUriPathLen = 64;
constexpr size_t kCoapMaxOptionNumber = 0xFFFF;

enum class ParseStatus {
    Ok,
    Malformed,             // -> 4.00 Bad Request
    UnknownCriticalOption, // -> 4.02 Bad Option
};

struct CoapMessage {
    uint8_t version = 1;
    CoapType type = CoapType::Confirmable;
    uint8_t code = 0;
    uint16_t message_id = 0;
    std::vector<uint8_t> token;
    std::string uri_path; // Uri-Path option segments joined with '/', e.g. "pogo/v1/sample"
    std::vector<uint8_t> payload;
};

// Parses `data`/`len` into `out`. See ParseStatus above for the two
// distinct failure reasons this minimal subset reports; never guesses at a
// malformed or partially-understood datagram.
ParseStatus coap_parse(const uint8_t *data, size_t len, CoapMessage &out);

// Builds a complete Acknowledgement datagram for `request` with the given
// response code, no options, no payload (this ingest program's own
// responses never need to carry one).
std::vector<uint8_t> coap_build_ack(const CoapMessage &request, uint8_t response_code);

// The same Acknowledgement carrying `payload` (a piggybacked response, RFC
// 7252 5.2.1): the payload marker 0xFF and the bytes. An empty payload gives
// exactly coap_build_ack(request, response_code). Used only for the Power
// Base management resource; the data resources never answer with a payload.
std::vector<uint8_t> coap_build_ack(const CoapMessage &request, uint8_t response_code,
                                    const std::vector<uint8_t> &payload);

} // namespace ingest
