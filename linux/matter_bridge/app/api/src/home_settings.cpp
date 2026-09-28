#include "pogohome_api/home_settings.hpp"

#include <algorithm>

namespace pogohome_api {

namespace {

bool Fail(SettingsError &error, const char *code, const std::string &message)
{
    error.code = code;
    error.message = message;
    return false;
}

bool IsAsciiSpace(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

std::string AsciiLower(const std::string &s)
{
    std::string out = s;
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool IsValidSymbol(const std::string &symbol)
{
    if (symbol.empty() || symbol.size() > kMaxSettingsSymbolBytes) {
        return false;
    }
    for (char c : symbol) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.')) {
            return false;
        }
    }
    return true;
}

// Only these members may appear; anything else is an error.
bool OnlyMembers(const JsonValue &object, std::initializer_list<const char *> allowed, SettingsError &error,
                 const char *where)
{
    for (const auto &member : object.object_members) {
        bool known = false;
        for (const char *name : allowed) {
            if (member.first == name) {
                known = true;
                break;
            }
        }
        if (!known) {
            return Fail(error, "unknown_field", std::string("unknown field in ") + where);
        }
    }
    return true;
}

bool ReadOptionalString(const JsonValue &object, const char *key, bool &has, std::string &out,
                        SettingsError &error)
{
    const JsonValue *value = object.Find(key);
    has = false;
    out.clear();
    if (value == nullptr || value->IsNull()) {
        return true;
    }
    if (!value->IsString()) {
        return Fail(error, "invalid_type", std::string(key) + " must be a string or null");
    }
    has = true;
    out = value->string_value;
    return true;
}

bool ReadRequiredString(const JsonValue &object, const char *key, std::string &out, SettingsError &error)
{
    const JsonValue *value = object.Find(key);
    if (value == nullptr || !value->IsString()) {
        return Fail(error, "invalid_type", std::string(key) + " must be a string");
    }
    out = value->string_value;
    return true;
}

bool ParseRoom(const JsonValue &value, RoomSetting &room, SettingsError &error)
{
    if (!value.IsObject()) {
        return Fail(error, "invalid_type", "each room must be an object");
    }
    if (!OnlyMembers(value, {"id", "name_kind", "name", "suggestion_symbol", "suggestion_ordinal", "symbol"}, error,
                     "room")) {
        return false;
    }
    std::string raw_id;
    if (!ReadRequiredString(value, "id", raw_id, error)) {
        return false;
    }
    room.id = CanonicalRoomId(raw_id);
    if (room.id.empty()) {
        return Fail(error, "invalid_room_id", "room id is not a UUID");
    }
    std::string kind;
    if (!ReadRequiredString(value, "name_kind", kind, error)) {
        return false;
    }
    if (!ReadRequiredString(value, "symbol", room.symbol, error)) {
        return false;
    }
    if (kind == "custom") {
        room.name_kind = RoomNameKind::Custom;
        if (!ReadRequiredString(value, "name", room.name, error)) {
            return false;
        }
        if (value.Find("suggestion_symbol") != nullptr || value.Find("suggestion_ordinal") != nullptr) {
            return Fail(error, "invalid_room_name", "a custom room name has no suggestion fields");
        }
    } else if (kind == "suggestion") {
        room.name_kind = RoomNameKind::Suggestion;
        if (value.Find("name") != nullptr) {
            return Fail(error, "invalid_room_name", "a suggested room name has no name text");
        }
        if (!ReadRequiredString(value, "suggestion_symbol", room.suggestion_symbol, error)) {
            return false;
        }
        const JsonValue *ordinal = value.Find("suggestion_ordinal");
        if (ordinal == nullptr || !ordinal->IsInteger()) {
            return Fail(error, "invalid_type", "suggestion_ordinal must be an integer");
        }
        if (ordinal->int_value < 1 || ordinal->int_value > kMaxSuggestionOrdinal) {
            return Fail(error, "invalid_room_name", "suggestion_ordinal is out of range");
        }
        room.suggestion_ordinal = static_cast<int>(ordinal->int_value);
    } else {
        return Fail(error, "invalid_room_name", "name_kind must be \"custom\" or \"suggestion\"");
    }
    return true;
}

bool ParseModule(const JsonValue &value, ModuleSetting &module, SettingsError &error)
{
    if (!value.IsObject()) {
        return Fail(error, "invalid_type", "each module must be an object");
    }
    if (!OnlyMembers(value, {"uuid", "name", "room_id"}, error, "module")) {
        return false;
    }
    if (!ReadRequiredString(value, "uuid", module.uuid, error)) {
        return false;
    }
    if (!ReadOptionalString(value, "name", module.has_name, module.name, error)) {
        return false;
    }
    std::string raw_room;
    if (!ReadOptionalString(value, "room_id", module.has_room, raw_room, error)) {
        return false;
    }
    if (module.has_room) {
        module.room_id = CanonicalRoomId(raw_room);
        if (module.room_id.empty()) {
            return Fail(error, "invalid_room_id", "module room_id is not a UUID");
        }
    }
    return true;
}

} // namespace

std::string CanonicalRoomId(const std::string &id)
{
    if (id.size() != 36) {
        return std::string();
    }
    std::string out = id;
    for (size_t i = 0; i < out.size(); i++) {
        char c = out[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') {
                return std::string();
            }
            continue;
        }
        if (c >= 'a' && c <= 'f') {
            out[i] = static_cast<char>(c - 'a' + 'A');
        } else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
            return std::string();
        }
    }
    return out;
}

bool IsValidModuleUuid(const std::string &uuid)
{
    if (uuid.size() != 16) {
        return false;
    }
    for (char c : uuid) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool IsValidDisplayName(const std::string &name, size_t max_code_points, size_t max_bytes)
{
    if (name.empty() || name.size() > max_bytes) {
        return false;
    }
    size_t code_points = 0;
    if (!IsValidUtf8(name, &code_points) || code_points > max_code_points) {
        return false;
    }
    if (IsAsciiSpace(static_cast<unsigned char>(name.front())) || IsAsciiSpace(static_cast<unsigned char>(name.back()))) {
        return false;
    }
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    return true;
}

bool ValidateHomeSettings(const HomeSettings &settings, SettingsError &error)
{
    if (settings.has_home_name &&
        !IsValidDisplayName(settings.home_name, kMaxSettingsNameCodePoints, kMaxSettingsNameBytes)) {
        return Fail(error, "invalid_name", "home_name is empty, too long, or has control characters");
    }
    if (settings.rooms.size() > kMaxSettingsRooms) {
        return Fail(error, "too_many_rooms", "at most " + std::to_string(kMaxSettingsRooms) + " rooms");
    }
    if (settings.modules.size() > kMaxSettingsModules) {
        return Fail(error, "too_many_modules", "at most " + std::to_string(kMaxSettingsModules) + " modules");
    }
    for (size_t i = 0; i < settings.rooms.size(); i++) {
        const RoomSetting &room = settings.rooms[i];
        if (CanonicalRoomId(room.id) != room.id) {
            return Fail(error, "invalid_room_id", "room id is not a canonical UUID");
        }
        if (room.id == kUnspecifiedRoomId) {
            return Fail(error, "reserved_room_id", "the unspecified room is implicit and cannot be listed");
        }
        if (!IsValidSymbol(room.symbol)) {
            return Fail(error, "invalid_symbol", "room symbol must be 1..64 of [a-z0-9.]");
        }
        if (room.name_kind == RoomNameKind::Custom) {
            if (!IsValidDisplayName(room.name, kMaxSettingsNameCodePoints, kMaxSettingsNameBytes)) {
                return Fail(error, "invalid_name", "room name is empty, too long, or has control characters");
            }
        } else {
            if (!IsValidSymbol(room.suggestion_symbol)) {
                return Fail(error, "invalid_symbol", "suggestion_symbol must be 1..64 of [a-z0-9.]");
            }
            if (room.suggestion_ordinal < 1 || room.suggestion_ordinal > kMaxSuggestionOrdinal) {
                return Fail(error, "invalid_room_name", "suggestion_ordinal is out of range");
            }
        }
        for (size_t j = 0; j < i; j++) {
            const RoomSetting &other = settings.rooms[j];
            if (other.id == room.id) {
                return Fail(error, "duplicate_room_id", "two rooms have the same id");
            }
            if (room.name_kind != other.name_kind) {
                continue;
            }
            if (room.name_kind == RoomNameKind::Custom && AsciiLower(room.name) == AsciiLower(other.name)) {
                return Fail(error, "duplicate_room_name", "two rooms have the same name");
            }
            if (room.name_kind == RoomNameKind::Suggestion && room.suggestion_symbol == other.suggestion_symbol &&
                room.suggestion_ordinal == other.suggestion_ordinal) {
                return Fail(error, "duplicate_room_name", "two rooms have the same suggested name");
            }
        }
    }
    for (size_t i = 0; i < settings.modules.size(); i++) {
        const ModuleSetting &module = settings.modules[i];
        if (!IsValidModuleUuid(module.uuid)) {
            return Fail(error, "invalid_module_uuid", "module uuid must be 16 lowercase hex digits");
        }
        if (module.has_name && !IsValidDisplayName(module.name, kMaxSettingsNameCodePoints, kMaxSettingsNameBytes)) {
            return Fail(error, "invalid_name", "module name is empty, too long, or has control characters");
        }
        if (module.has_room) {
            bool found = false;
            for (const auto &room : settings.rooms) {
                if (room.id == module.room_id) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return Fail(error, "unknown_room_id", "a module is placed in a room that does not exist");
            }
        }
        for (size_t j = 0; j < i; j++) {
            if (settings.modules[j].uuid == module.uuid) {
                return Fail(error, "duplicate_module_uuid", "two entries for the same module");
            }
        }
    }
    return true;
}

bool ParseHomeSettings(const JsonValue &value, HomeSettings &out, SettingsError &error)
{
    out = HomeSettings();
    if (!value.IsObject()) {
        return Fail(error, "invalid_type", "settings must be an object");
    }
    if (!OnlyMembers(value, {"home_name", "rooms", "modules"}, error, "settings")) {
        return false;
    }
    if (!ReadOptionalString(value, "home_name", out.has_home_name, out.home_name, error)) {
        return false;
    }
    const JsonValue *rooms = value.Find("rooms");
    const JsonValue *modules = value.Find("modules");
    if (rooms == nullptr || !rooms->IsArray()) {
        return Fail(error, "invalid_type", "rooms must be an array");
    }
    if (modules == nullptr || !modules->IsArray()) {
        return Fail(error, "invalid_type", "modules must be an array");
    }
    if (rooms->array_items.size() > kMaxSettingsRooms) {
        return Fail(error, "too_many_rooms", "at most " + std::to_string(kMaxSettingsRooms) + " rooms");
    }
    if (modules->array_items.size() > kMaxSettingsModules) {
        return Fail(error, "too_many_modules", "at most " + std::to_string(kMaxSettingsModules) + " modules");
    }
    for (const auto &item : rooms->array_items) {
        RoomSetting room;
        if (!ParseRoom(item, room, error)) {
            return false;
        }
        out.rooms.push_back(std::move(room));
    }
    for (const auto &item : modules->array_items) {
        ModuleSetting module;
        if (!ParseModule(item, module, error)) {
            return false;
        }
        out.modules.push_back(std::move(module));
    }
    return ValidateHomeSettings(out, error);
}

void AppendHomeSettingsJson(std::string &out, const HomeSettings &settings)
{
    out += "{\"home_name\":";
    if (settings.has_home_name) {
        AppendJsonStringLiteral(out, settings.home_name);
    } else {
        out += "null";
    }
    out += ",\"rooms\":[";
    for (size_t i = 0; i < settings.rooms.size(); i++) {
        const RoomSetting &room = settings.rooms[i];
        if (i > 0) {
            out += ',';
        }
        out += "{\"id\":";
        AppendJsonStringLiteral(out, room.id);
        if (room.name_kind == RoomNameKind::Custom) {
            out += ",\"name_kind\":\"custom\",\"name\":";
            AppendJsonStringLiteral(out, room.name);
        } else {
            out += ",\"name_kind\":\"suggestion\",\"suggestion_symbol\":";
            AppendJsonStringLiteral(out, room.suggestion_symbol);
            out += ",\"suggestion_ordinal\":";
            out += std::to_string(room.suggestion_ordinal);
        }
        out += ",\"symbol\":";
        AppendJsonStringLiteral(out, room.symbol);
        out += '}';
    }
    out += "],\"modules\":[";
    std::vector<const ModuleSetting *> sorted;
    sorted.reserve(settings.modules.size());
    for (const auto &module : settings.modules) {
        sorted.push_back(&module);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const ModuleSetting *a, const ModuleSetting *b) { return a->uuid < b->uuid; });
    for (size_t i = 0; i < sorted.size(); i++) {
        const ModuleSetting &module = *sorted[i];
        if (i > 0) {
            out += ',';
        }
        out += "{\"uuid\":";
        AppendJsonStringLiteral(out, module.uuid);
        out += ",\"name\":";
        if (module.has_name) {
            AppendJsonStringLiteral(out, module.name);
        } else {
            out += "null";
        }
        out += ",\"room_id\":";
        if (module.has_room) {
            AppendJsonStringLiteral(out, module.room_id);
        } else {
            out += "null";
        }
        out += '}';
    }
    out += "]}";
}

} // namespace pogohome_api
