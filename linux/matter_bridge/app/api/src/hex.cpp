#include "pogohome_api/hex.hpp"

namespace pogohome_api {

std::string FormatUuidHex(const ingest::Uuid &uuid)
{
    static const char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.resize(uuid.size() * 2);
    for (size_t i = 0; i < uuid.size(); i++) {
        out[i * 2] = kHexDigits[(uuid[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHexDigits[uuid[i] & 0x0F];
    }
    return out;
}

} // namespace pogohome_api
