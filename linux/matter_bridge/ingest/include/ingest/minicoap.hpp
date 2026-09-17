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
constexpr uint8_t kCoapCodeChanged = CoapCode(2, 4);   // 2.04
constexpr uint8_t kCoapCodeValid = CoapCode(2, 3);     // 2.03
constexpr uint8_t kCoapCodeBadRequest = CoapCode(4, 0);       // 4.00
constexpr uint8_t kCoapCodeServiceUnavailable = CoapCode(5, 3); // 5.03

struct CoapMessage {
    uint8_t version = 1;
    CoapType type = CoapType::Confirmable;
    uint8_t code = 0;
    uint16_t message_id = 0;
    std::vector<uint8_t> token;
    std::string uri_path; // Uri-Path option segments joined with '/', e.g. "pogo/v1/sample"
    std::vector<uint8_t> payload;
};

// Parses `data`/`len` into `out`. Returns false for anything this minimal
// subset cannot safely interpret (too short, bad version, a token/option
// length that runs past the buffer, ...) -- never guesses.
bool coap_parse(const uint8_t *data, size_t len, CoapMessage &out);

// Builds a complete Acknowledgement datagram for `request` with the given
// response code, no options, no payload (this ingest program's own
// responses never need to carry one).
std::vector<uint8_t> coap_build_ack(const CoapMessage &request, uint8_t response_code);

} // namespace ingest
