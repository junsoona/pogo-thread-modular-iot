// Lowercase-hex encoding for an ingest::Uuid -- the SAME convention
// linux/matter_bridge/app/chip/chip_endpoint.cpp's own FormatUuidHex()
// already uses for this bridge's Matter EndpointUniqueID (16 lowercase hex
// characters for the 8-byte module_uuid, no separators, no truncation).
// Duplicated here (rather than shared) because chip_endpoint.cpp lives in
// the CHIP-SDK-dependent part of this application and this library must
// stay buildable/testable with nothing beyond a C++17 compiler + CMake --
// see this directory's own CMakeLists.txt top comment.
#pragma once

#include <string>

#include "ingest/registry.hpp"

namespace pogohome_api {

std::string FormatUuidHex(const ingest::Uuid &uuid);

} // namespace pogohome_api
