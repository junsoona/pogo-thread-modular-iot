// pogohome_api::ParseJson -- the strict reader behind settings requests and
// the settings/credential files (json_value.hpp).
#include <string>

#include "pogohome_api/json_value.hpp"
#include "test_check.hpp"

using pogohome_api::JsonParseLimits;
using pogohome_api::JsonValue;
using pogohome_api::ParseJson;

namespace {

bool Parses(const std::string &text)
{
    JsonValue v;
    std::string error;
    return ParseJson(text, v, error);
}

} // namespace

void RunJsonValueTests(int *failures)
{
    printf("\n-- scenario: well-formed documents parse into the expected values --\n");
    {
        JsonValue v;
        std::string error;
        bool ok = ParseJson(" {\"a\": [1, -2, true, null, \"x\\u00e9\\ud83d\\ude00\"], \"b\": {\"c\": 9223372036854775807}} ",
                            v, error);
        API_CHECK(ok, "an object with arrays, literals, escapes and a surrogate pair parses");
        const JsonValue *a = v.Find("a");
        API_CHECK(a != nullptr && a->IsArray() && a->array_items.size() == 5, "the array keeps its five items");
        API_CHECK(a != nullptr && a->array_items[1].IsInteger() && a->array_items[1].int_value == -2,
                  "a negative integer keeps its value");
        API_CHECK(a != nullptr && a->array_items[4].string_value == "x\xC3\xA9\xF0\x9F\x98\x80",
                  "\\u escapes (including a surrogate pair) become UTF-8");
        const JsonValue *c = v.Find("b") != nullptr ? v.Find("b")->Find("c") : nullptr;
        API_CHECK(c != nullptr && c->IsInteger() && c->int_value == INT64_MAX, "INT64_MAX is an integer");
        API_CHECK(v.Find("missing") == nullptr, "a missing key is null");
    }

    printf("\n-- scenario: malformed or ambiguous documents are rejected --\n");
    {
        API_CHECK(!Parses("{\"a\":1,\"a\":2}"), "a duplicate key is rejected");
        API_CHECK(!Parses("{\"a\":1} x"), "data after the value is rejected");
        API_CHECK(!Parses("{\"a\":\"\\ud800\"}"), "a lone high surrogate is rejected");
        API_CHECK(!Parses("{\"a\":\"\\udc00\"}"), "a lone low surrogate is rejected");
        API_CHECK(!Parses(std::string("{\"a\":\"\xC0\xAF\"}")), "an overlong UTF-8 sequence is rejected");
        API_CHECK(!Parses(std::string("{\"a\":\"\xFF\"}")), "an invalid UTF-8 byte is rejected");
        API_CHECK(!Parses(std::string("{\"a\":\"\x01\"}")), "a raw control character in a string is rejected");
        API_CHECK(!Parses("{\"a\":01}"), "a leading zero is rejected");
        API_CHECK(!Parses("{\"a\":tru}"), "a truncated literal is rejected");
        API_CHECK(!Parses("[1,2,]"), "a trailing comma is rejected");
        API_CHECK(!Parses(""), "an empty body is rejected");
        JsonValue v;
        std::string error;
        API_CHECK(ParseJson("{\"n\":9223372036854775808}", v, error) && !v.Find("n")->IsInteger(),
                  "an integer beyond int64 parses as a non-integer number (and is then refused by any field)");
        API_CHECK(ParseJson("{\"n\":1.5}", v, error) && !v.Find("n")->IsInteger(), "a fraction is not an integer");
    }

    printf("\n-- scenario: the limits bound depth, string size and value count --\n");
    {
        std::string deep;
        for (int i = 0; i < 40; i++) {
            deep += '[';
        }
        for (int i = 0; i < 40; i++) {
            deep += ']';
        }
        API_CHECK(!Parses(deep), "nesting past max_depth is rejected");
        JsonParseLimits limits;
        limits.max_string_bytes = 8;
        JsonValue v;
        std::string error;
        API_CHECK(!ParseJson("\"123456789\"", v, error, limits), "a string past max_string_bytes is rejected");
        limits = JsonParseLimits();
        limits.max_values = 3;
        API_CHECK(!ParseJson("[1,2,3]", v, error, limits), "more values than max_values is rejected");
        API_CHECK(error.find('1') == std::string::npos, "an error message never echoes input");
    }
}
