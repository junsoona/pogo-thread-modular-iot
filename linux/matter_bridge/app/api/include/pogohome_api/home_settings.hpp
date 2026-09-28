// The home settings PogoHome devices share through this hub (see
// docs/pogohome-api-v1.md "Home settings"): the home name, the rooms (id,
// name, icon, order) and each sensor module's custom name and room.
//
// What is deliberately NOT here: sensor readings or online state (the
// Registry owns those), Apple Home's homes/rooms/accessories, simulated
// (mock) data, and anything per device (language, appearance, selected tab,
// Watch sensor choice, permissions).
//
// Identity rules:
// - a module is keyed by its real module UUID from the Registry (16
//   lowercase hex digits, the same `uuid` /api/v1/snapshot lists), never by
//   Power Base, position or name, so a module keeps its name and room when
//   it is detached, re-docked or moved to another Power Base;
// - a room is keyed by a client-generated RFC 4122 UUID, never by its name;
// - the app's fixed "unspecified" room is implicit: it is never listed in
//   `rooms`, and a module in it has `room_id: null`.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "pogohome_api/json_value.hpp"

namespace pogohome_api {

constexpr int kSettingsSchemaVersion = 1;
constexpr size_t kMaxSettingsRooms = 64;
constexpr size_t kMaxSettingsModules = 256;
// Server-side safety bounds. The app itself limits names to 30 characters
// (user-perceived); one character can be several code points (emoji
// sequences), so these are looser than the app's own rule.
constexpr size_t kMaxSettingsNameCodePoints = 120;
constexpr size_t kMaxSettingsNameBytes = 480;
constexpr size_t kMaxSettingsSymbolBytes = 64;
constexpr int kMaxSuggestionOrdinal = 999;
// ModuleCatalog.unspecifiedRoomID in the app -- reserved, never a room here.
constexpr const char kUnspecifiedRoomId[] = "9C6B8B2E-0000-4000-8000-000000000000";

enum class RoomNameKind {
    // Typed by the user; shown verbatim on every device.
    Custom,
    // The app's own name for `suggestion_symbol` (e.g. "Living Room" / "거실"),
    // numbered by `suggestion_ordinal`; each device shows it in its own language.
    Suggestion,
};

struct RoomSetting {
    std::string id; // canonical: uppercase 8-4-4-4-12 hex
    RoomNameKind name_kind = RoomNameKind::Custom;
    std::string name;              // Custom only
    std::string suggestion_symbol; // Suggestion only
    int suggestion_ordinal = 1;    // Suggestion only
    std::string symbol;
};

struct ModuleSetting {
    std::string uuid; // 16 lowercase hex digits
    bool has_name = false;
    std::string name;
    bool has_room = false;
    std::string room_id; // canonical, one of the document's rooms
};

struct HomeSettings {
    bool has_home_name = false;
    std::string home_name;
    std::vector<RoomSetting> rooms;     // display order
    std::vector<ModuleSetting> modules; // any order in; serialized sorted by uuid
};

// A stable machine-readable `code` (returned to clients in `detail`) plus a
// short English explanation.
struct SettingsError {
    std::string code;
    std::string message;
};

// Parses the `settings` object of a request or a stored file and validates
// it completely (see ValidateHomeSettings). Room ids are canonicalized to
// uppercase. Unknown members are rejected, so a typo can never be silently
// dropped.
bool ParseHomeSettings(const JsonValue &value, HomeSettings &out, SettingsError &error);

// Every rule a stored document must satisfy:
// - at most kMaxSettingsRooms rooms and kMaxSettingsModules modules;
// - names: valid UTF-8, not empty, no surrounding whitespace, no control
//   characters, within the length bounds above;
// - room ids unique, well formed, never the unspecified room's id;
// - no two custom room names equal (ignoring ASCII letter case), no two
//   suggestion rooms with the same icon and number;
// - symbols: 1..64 of [a-z0-9.];
// - module uuids unique and 16 lowercase hex digits;
// - a module's room_id names one of `rooms`.
bool ValidateHomeSettings(const HomeSettings &settings, SettingsError &error);

// Appends the canonical `settings` object (modules sorted by uuid).
void AppendHomeSettingsJson(std::string &out, const HomeSettings &settings);

// Canonical (uppercase) form of an RFC 4122-shaped id, or "" if malformed.
std::string CanonicalRoomId(const std::string &id);

bool IsValidModuleUuid(const std::string &uuid);

// The name rules above (shared with device names at pairing, with its own
// length bound).
bool IsValidDisplayName(const std::string &name, size_t max_code_points, size_t max_bytes);

} // namespace pogohome_api
