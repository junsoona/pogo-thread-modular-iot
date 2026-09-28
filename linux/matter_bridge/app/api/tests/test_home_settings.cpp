// The shared home settings document: parsing, every validation rule, and
// the canonical serialization (home_settings.hpp).
#include <string>

#include "pogohome_api/home_settings.hpp"
#include "pogohome_api/json_value.hpp"
#include "test_check.hpp"

using namespace pogohome_api;

namespace {

const char kLiving[] = "8A3B1C10-0001-4000-8000-000000000001";
const char kBedroom[] = "8A3B1C10-0002-4000-8000-000000000002";

bool ParseSettings(const std::string &text, HomeSettings &out, SettingsError &error)
{
    JsonValue v;
    std::string parse_error;
    if (!ParseJson(text, v, parse_error)) {
        error.code = "json";
        return false;
    }
    return ParseHomeSettings(v, out, error);
}

std::string CodeOf(const std::string &text)
{
    HomeSettings s;
    SettingsError e;
    return ParseSettings(text, s, e) ? std::string("ok") : e.code;
}

std::string Doc(const std::string &rooms, const std::string &modules, const std::string &home = "null")
{
    return "{\"home_name\":" + home + ",\"rooms\":[" + rooms + "],\"modules\":[" + modules + "]}";
}

std::string CustomRoom(const std::string &id, const std::string &name, const std::string &symbol = "sofa.fill")
{
    return "{\"id\":\"" + id + "\",\"name_kind\":\"custom\",\"name\":\"" + name + "\",\"symbol\":\"" + symbol + "\"}";
}

std::string SuggestedRoom(const std::string &id, const std::string &symbol, int ordinal)
{
    return "{\"id\":\"" + id + "\",\"name_kind\":\"suggestion\",\"suggestion_symbol\":\"" + symbol +
           "\",\"suggestion_ordinal\":" + std::to_string(ordinal) + ",\"symbol\":\"" + symbol + "\"}";
}

} // namespace

void RunHomeSettingsTests(int *failures)
{
    printf("\n-- scenario: a full settings document parses and serializes canonically --\n");
    {
        std::string text = Doc(SuggestedRoom("8a3b1c10-0001-4000-8000-000000000001", "sofa.fill", 1) + "," +
                                   CustomRoom(kBedroom, "\xEC\xB9\xA8\xEC\x8B\xA4 \xF0\x9F\x9B\x8F", "bed.double.fill"),
                               "{\"uuid\":\"744dbdfffe65d357\",\"name\":\"Air\",\"room_id\":\"" + std::string(kBedroom) +
                                   "\"},{\"uuid\":\"0000000000000001\",\"name\":null,\"room_id\":null}",
                               "\"Our Home\"");
        HomeSettings s;
        SettingsError e;
        API_CHECK(ParseSettings(text, s, e), "a valid document parses");
        API_CHECK(s.rooms.size() == 2 && s.rooms[0].id == kLiving, "room ids are canonicalized to uppercase");
        API_CHECK(s.rooms[0].name_kind == RoomNameKind::Suggestion && s.rooms[0].suggestion_ordinal == 1,
                  "a suggested name keeps its icon and number");
        API_CHECK(s.has_home_name && s.home_name == "Our Home", "the home name is kept verbatim");

        std::string out;
        AppendHomeSettingsJson(out, s);
        API_CHECK(out.find("\"uuid\":\"0000000000000001\"") < out.find("\"uuid\":\"744dbdfffe65d357\""),
                  "modules are serialized sorted by uuid");
        HomeSettings again;
        API_CHECK(ParseSettings(out, again, e), "the serialized form parses again");
        std::string out2;
        AppendHomeSettingsJson(out2, again);
        API_CHECK(out == out2, "serialization is stable (a round trip gives the same bytes)");
    }

    printf("\n-- scenario: every validation rule rejects its bad input with a stable code --\n");
    {
        API_CHECK(CodeOf(Doc(CustomRoom(kLiving, "A") + "," + CustomRoom(kBedroom, "a"), "")) == "duplicate_room_name",
                  "two custom rooms whose names differ only in letter case are duplicates");
        API_CHECK(CodeOf(Doc(SuggestedRoom(kLiving, "sofa.fill", 1) + "," + SuggestedRoom(kBedroom, "sofa.fill", 1), "")) ==
                      "duplicate_room_name",
                  "two suggested rooms with the same icon and number are duplicates");
        API_CHECK(CodeOf(Doc(SuggestedRoom(kLiving, "sofa.fill", 1) + "," + SuggestedRoom(kBedroom, "sofa.fill", 2), "")) ==
                      "ok",
                  "the same suggestion with a different number is allowed");
        API_CHECK(CodeOf(Doc(CustomRoom(kLiving, "A") + "," + CustomRoom(kLiving, "B"), "")) == "duplicate_room_id",
                  "two rooms with one id are rejected");
        API_CHECK(CodeOf(Doc(CustomRoom("9c6b8b2e-0000-4000-8000-000000000000", "X"), "")) == "reserved_room_id",
                  "the unspecified room's fixed id is reserved");
        API_CHECK(CodeOf(Doc(CustomRoom("not-a-uuid", "X"), "")) == "invalid_room_id", "a malformed room id is rejected");
        API_CHECK(CodeOf(Doc(CustomRoom(kLiving, " padded"), "")) == "invalid_name",
                  "a name with surrounding whitespace is rejected");
        API_CHECK(CodeOf(Doc(CustomRoom(kLiving, ""), "")) == "invalid_name", "an empty name is rejected");
        API_CHECK(CodeOf(Doc(CustomRoom(kLiving, "tab\\there"), "")) == "invalid_name",
                  "a control character inside a name is rejected");
        API_CHECK(CodeOf(Doc(CustomRoom(kLiving, std::string(121, 'x')), "")) == "invalid_name",
                  "a name longer than the bound is rejected");
        API_CHECK(CodeOf(Doc(CustomRoom(kLiving, "X", "Bad Symbol"), "")) == "invalid_symbol",
                  "a symbol outside [a-z0-9.] is rejected");
        API_CHECK(CodeOf(Doc(SuggestedRoom(kLiving, "sofa.fill", 0), "")) == "invalid_room_name",
                  "a suggestion number below 1 is rejected");
        API_CHECK(CodeOf(Doc("", "{\"uuid\":\"744DBDFFFE65D357\",\"name\":null,\"room_id\":null}")) == "invalid_module_uuid",
                  "an uppercase module uuid is rejected (the Registry's form is lowercase)");
        API_CHECK(CodeOf(Doc("", "{\"uuid\":\"744dbd\",\"name\":null,\"room_id\":null}")) == "invalid_module_uuid",
                  "a short module uuid is rejected");
        API_CHECK(CodeOf(Doc("", "{\"uuid\":\"744dbdfffe65d357\",\"name\":null,\"room_id\":null},"
                                 "{\"uuid\":\"744dbdfffe65d357\",\"name\":\"x\",\"room_id\":null}")) ==
                      "duplicate_module_uuid",
                  "two entries for one module are rejected");
        API_CHECK(CodeOf(Doc(CustomRoom(kLiving, "A"),
                             "{\"uuid\":\"744dbdfffe65d357\",\"name\":null,\"room_id\":\"" + std::string(kBedroom) + "\"}")) ==
                      "unknown_room_id",
                  "a module placed in a room that is not in the document is rejected");
        API_CHECK(CodeOf("{\"home_name\":null,\"rooms\":[],\"modules\":[],\"extra\":1}") == "unknown_field",
                  "an unknown top-level field is rejected");
        API_CHECK(CodeOf(Doc("{\"id\":\"" + std::string(kLiving) +
                                 "\",\"name_kind\":\"custom\",\"name\":\"A\",\"symbol\":\"a\",\"color\":\"red\"}",
                             "")) == "unknown_field",
                  "an unknown room field is rejected");
        API_CHECK(CodeOf(Doc("", "", "\"\"")) == "invalid_name", "an empty home name is rejected (null means default)");
        std::string many_rooms;
        for (int i = 0; i < 65; i++) {
            char id[40];
            std::snprintf(id, sizeof(id), "8A3B1C10-0000-4000-8000-%012d", i);
            if (i > 0) {
                many_rooms += ',';
            }
            many_rooms += CustomRoom(id, "R" + std::to_string(i));
        }
        API_CHECK(CodeOf(Doc(many_rooms, "")) == "too_many_rooms", "more than 64 rooms are rejected");
    }
}
