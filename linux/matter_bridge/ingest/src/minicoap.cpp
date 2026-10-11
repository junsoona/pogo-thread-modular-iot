#include "ingest/minicoap.hpp"

namespace ingest {

namespace {

enum class OptionParseResult {
    Ok,
    PayloadMarker,
    Malformed,
};

// Reads one CoAP option header + value starting at *pos, advancing *pos
// past it. `running_number` is updated by the option's delta and checked
// against kCoapMaxOptionNumber (correction pass: the previous version
// silently truncated an overflowed running_number into a uint16_t instead
// of rejecting it). Returns Malformed on any out-of-bounds/overflowed
// option (caller aborts the whole parse), PayloadMarker if the byte at
// *pos is 0xFF rather than an option header -- see RFC 7252 3.1: a
// delta/length nibble pair of 15/15 is reserved and MUST NOT be used as an
// option header, so encountering that exact byte unambiguously means
// "payload starts here", the same simplification essentially every minimal
// CoAP parser relies on.
OptionParseResult parse_one_option(const uint8_t *data, size_t len, size_t *pos, uint32_t *running_number,
                                    uint16_t *out_number, std::vector<uint8_t> *out_value)
{
    if (*pos >= len) {
        return OptionParseResult::Malformed;
    }
    uint8_t header = data[*pos];
    if (header == 0xFF) {
        return OptionParseResult::PayloadMarker;
    }
    uint8_t delta_nibble = (header >> 4) & 0x0F;
    uint8_t length_nibble = header & 0x0F;
    (*pos)++;

    uint32_t delta;
    if (delta_nibble <= 12) {
        delta = delta_nibble;
    } else if (delta_nibble == 13) {
        if (*pos >= len) {
            return OptionParseResult::Malformed;
        }
        delta = 13u + data[*pos];
        (*pos)++;
    } else if (delta_nibble == 14) {
        if (*pos + 1 >= len) {
            return OptionParseResult::Malformed;
        }
        delta = 269u + ((uint32_t)data[*pos] << 8) + data[*pos + 1];
        *pos += 2;
    } else {
        return OptionParseResult::Malformed; // 15 as delta alone (without length also 15) is invalid
    }

    uint32_t length;
    if (length_nibble <= 12) {
        length = length_nibble;
    } else if (length_nibble == 13) {
        if (*pos >= len) {
            return OptionParseResult::Malformed;
        }
        length = 13u + data[*pos];
        (*pos)++;
    } else if (length_nibble == 14) {
        if (*pos + 1 >= len) {
            return OptionParseResult::Malformed;
        }
        length = 269u + ((uint32_t)data[*pos] << 8) + data[*pos + 1];
        *pos += 2;
    } else {
        return OptionParseResult::Malformed;
    }

    if (*pos + length > len) {
        return OptionParseResult::Malformed;
    }
    uint64_t new_running_number = (uint64_t)*running_number + delta;
    if (new_running_number > kCoapMaxOptionNumber) {
        // Correction pass: reject outright instead of silently truncating
        // into a uint16_t (a truncated option number could previously have
        // been misidentified as a DIFFERENT, smaller option number).
        return OptionParseResult::Malformed;
    }
    *running_number = (uint32_t)new_running_number;
    *out_number = static_cast<uint16_t>(*running_number);
    out_value->assign(data + *pos, data + *pos + length);
    *pos += length;
    return OptionParseResult::Ok;
}

} // namespace

ParseStatus coap_parse(const uint8_t *data, size_t len, CoapMessage &out)
{
    if (data == nullptr || len < 4) {
        return ParseStatus::Malformed;
    }
    uint8_t first = data[0];
    uint8_t version = (first >> 6) & 0x03;
    if (version != 1) {
        return ParseStatus::Malformed;
    }
    uint8_t type_bits = (first >> 4) & 0x03;
    uint8_t tkl = first & 0x0F;
    if (tkl > 8) {
        return ParseStatus::Malformed; // RFC 7252: 9-15 reserved, malformed
    }
    out.version = version;
    out.type = static_cast<CoapType>(type_bits);
    out.code = data[1];
    out.message_id = static_cast<uint16_t>((data[2] << 8) | data[3]);

    size_t pos = 4;
    if (pos + tkl > len) {
        return ParseStatus::Malformed;
    }
    out.token.assign(data + pos, data + pos + tkl);
    pos += tkl;

    out.uri_path.clear();
    out.payload.clear();
    uint32_t running_number = 0;
    while (pos < len) {
        uint16_t option_number = 0;
        std::vector<uint8_t> value;
        OptionParseResult r = parse_one_option(data, len, &pos, &running_number, &option_number, &value);
        if (r == OptionParseResult::Malformed) {
            return ParseStatus::Malformed;
        }
        if (r == OptionParseResult::PayloadMarker) {
            pos++; // skip the 0xFF marker itself
            if (pos >= len) {
                return ParseStatus::Malformed; // marker present but no payload bytes follow -- malformed
            }
            out.payload.assign(data + pos, data + len);
            break;
        }
        if (option_number == kCoapOptionUriPath) {
            if (value.size() > kCoapMaxUriPathLen) {
                return ParseStatus::Malformed; // correction pass: bound a single segment's length
            }
            if (out.uri_path.size() + 1 + value.size() > kCoapMaxUriPathLen) {
                return ParseStatus::Malformed; // correction pass: bound the assembled path's total length
            }
            if (!out.uri_path.empty()) {
                out.uri_path.push_back('/');
            }
            out.uri_path.append(reinterpret_cast<const char *>(value.data()), value.size());
        } else if (CoapOptionIsCritical(option_number)) {
            // Correction pass: an unrecognized CRITICAL option must reject
            // the whole message (4.02 Bad Option), not be silently
            // ignored like an elective one -- RFC 7252 5.4.1.
            return ParseStatus::UnknownCriticalOption;
        }
        // Every other elective (even-numbered) option (Content-Format, ...)
        // is intentionally ignored -- this ingest program has nothing to do
        // with them.
    }
    return ParseStatus::Ok;
}

std::vector<uint8_t> coap_build_ack(const CoapMessage &request, uint8_t response_code)
{
    std::vector<uint8_t> out;
    uint8_t tkl = static_cast<uint8_t>(request.token.size());
    out.push_back(static_cast<uint8_t>((1 << 6) | (static_cast<uint8_t>(CoapType::Acknowledgement) << 4) | tkl));
    out.push_back(response_code);
    out.push_back(static_cast<uint8_t>(request.message_id >> 8));
    out.push_back(static_cast<uint8_t>(request.message_id & 0xFF));
    out.insert(out.end(), request.token.begin(), request.token.end());
    return out;
}

std::vector<uint8_t> coap_build_ack(const CoapMessage &request, uint8_t response_code,
                                    const std::vector<uint8_t> &payload)
{
    std::vector<uint8_t> out = coap_build_ack(request, response_code);
    if (!payload.empty()) {
        out.push_back(0xFF);
        out.insert(out.end(), payload.begin(), payload.end());
    }
    return out;
}

} // namespace ingest
