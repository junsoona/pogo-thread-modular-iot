#include "ingest/minicoap.hpp"

namespace ingest {

namespace {

// Reads one CoAP option header + value starting at *pos, advancing *pos
// past it. `running_number` is updated by the option's delta. Returns
// false on any malformed/out-of-bounds option (caller aborts the whole
// parse). *is_payload_marker is set true (and nothing else is touched) if
// the byte at *pos is the payload marker (0xFF) rather than an option
// header -- see RFC 7252 3.1: a delta/length nibble pair of 15/15 is
// reserved and MUST NOT be used as an option header, so encountering that
// exact byte unambiguously means "payload starts here", the same
// simplification essentially every minimal CoAP parser relies on.
bool parse_one_option(const uint8_t *data, size_t len, size_t *pos, uint32_t *running_number, uint16_t *out_number,
                       std::vector<uint8_t> *out_value, bool *is_payload_marker)
{
    *is_payload_marker = false;
    if (*pos >= len) {
        return false;
    }
    uint8_t header = data[*pos];
    if (header == 0xFF) {
        *is_payload_marker = true;
        return true;
    }
    uint8_t delta_nibble = (header >> 4) & 0x0F;
    uint8_t length_nibble = header & 0x0F;
    (*pos)++;

    uint32_t delta;
    if (delta_nibble <= 12) {
        delta = delta_nibble;
    } else if (delta_nibble == 13) {
        if (*pos >= len) {
            return false;
        }
        delta = 13u + data[*pos];
        (*pos)++;
    } else if (delta_nibble == 14) {
        if (*pos + 1 >= len) {
            return false;
        }
        delta = 269u + ((uint32_t)data[*pos] << 8) + data[*pos + 1];
        *pos += 2;
    } else {
        return false; // 15 as delta alone (without length also 15) is invalid
    }

    uint32_t length;
    if (length_nibble <= 12) {
        length = length_nibble;
    } else if (length_nibble == 13) {
        if (*pos >= len) {
            return false;
        }
        length = 13u + data[*pos];
        (*pos)++;
    } else if (length_nibble == 14) {
        if (*pos + 1 >= len) {
            return false;
        }
        length = 269u + ((uint32_t)data[*pos] << 8) + data[*pos + 1];
        *pos += 2;
    } else {
        return false;
    }

    if (*pos + length > len) {
        return false;
    }
    *running_number += delta;
    *out_number = static_cast<uint16_t>(*running_number);
    out_value->assign(data + *pos, data + *pos + length);
    *pos += length;
    return true;
}

constexpr uint16_t kOptionUriPath = 11;

} // namespace

bool coap_parse(const uint8_t *data, size_t len, CoapMessage &out)
{
    if (data == nullptr || len < 4) {
        return false;
    }
    uint8_t first = data[0];
    uint8_t version = (first >> 6) & 0x03;
    if (version != 1) {
        return false;
    }
    uint8_t type_bits = (first >> 4) & 0x03;
    uint8_t tkl = first & 0x0F;
    if (tkl > 8) {
        return false; // RFC 7252: 9-15 reserved, malformed
    }
    out.version = version;
    out.type = static_cast<CoapType>(type_bits);
    out.code = data[1];
    out.message_id = static_cast<uint16_t>((data[2] << 8) | data[3]);

    size_t pos = 4;
    if (pos + tkl > len) {
        return false;
    }
    out.token.assign(data + pos, data + pos + tkl);
    pos += tkl;

    out.uri_path.clear();
    out.payload.clear();
    uint32_t running_number = 0;
    while (pos < len) {
        uint16_t option_number = 0;
        std::vector<uint8_t> value;
        bool is_payload_marker = false;
        if (!parse_one_option(data, len, &pos, &running_number, &option_number, &value, &is_payload_marker)) {
            return false;
        }
        if (is_payload_marker) {
            pos++; // skip the 0xFF marker itself
            if (pos >= len) {
                return false; // marker present but no payload bytes follow -- malformed
            }
            out.payload.assign(data + pos, data + len);
            break;
        }
        if (option_number == kOptionUriPath) {
            if (!out.uri_path.empty()) {
                out.uri_path.push_back('/');
            }
            out.uri_path.append(reinterpret_cast<const char *>(value.data()), value.size());
        }
        // Every other option (Content-Format, ...) is intentionally ignored --
        // this ingest program has nothing to do with them.
    }
    return true;
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

} // namespace ingest
