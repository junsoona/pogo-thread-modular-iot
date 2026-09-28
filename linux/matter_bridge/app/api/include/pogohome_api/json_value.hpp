// A small, strict JSON reader for the few request bodies and state files the
// settings-sharing revision reads (docs/pogohome-api-v1.md "Home settings").
// Like minicoap.hpp and this directory's own HTTP subset, it is written here
// rather than pulled in as a library, and it only accepts what those
// documents need:
//
// - UTF-8 input only; invalid UTF-8, lone surrogates and raw control
//   characters inside strings are rejected;
// - duplicate object keys are rejected (two readers must never disagree on
//   which one counts);
// - bounded depth, string length and element count, so a hostile body can
//   never make the parser allocate or recurse without limit;
// - numbers keep whether they were a plain integer that fits int64_t --
//   nothing here ever needs a fraction.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pogohome_api {

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool bool_value = false;
    // Set for a number written as a plain integer (no fraction or exponent)
    // that fits int64_t; int_value is then its value.
    bool is_integer = false;
    int64_t int_value = 0;
    std::string string_value;
    std::vector<JsonValue> array_items;
    // In document order. Keys are unique (the parser rejects duplicates).
    std::vector<std::pair<std::string, JsonValue>> object_members;

    bool IsNull() const { return type == Type::Null; }
    bool IsString() const { return type == Type::String; }
    bool IsObject() const { return type == Type::Object; }
    bool IsArray() const { return type == Type::Array; }
    bool IsBool() const { return type == Type::Bool; }
    bool IsInteger() const { return type == Type::Number && is_integer; }

    // The member named `key` of an object, or null when absent (or when this
    // is not an object).
    const JsonValue *Find(const std::string &key) const;
};

struct JsonParseLimits {
    size_t max_depth = 12;
    size_t max_string_bytes = 4096;
    // Total values in the document (every element and member counts once).
    size_t max_values = 8192;
};

// Parses exactly one JSON value surrounded by optional whitespace. On failure
// returns false and sets `error` to a short English description (never
// echoing input bytes, so it is safe to log and to return to a client).
bool ParseJson(const std::string &text, JsonValue &out, std::string &error, const JsonParseLimits &limits = {});

// Appends `s` as a JSON string literal (with quotes), escaping what RFC 8259
// requires. `s` is copied byte for byte otherwise; callers only pass valid
// UTF-8.
void AppendJsonStringLiteral(std::string &out, const std::string &s);

// Whether `s` is well-formed UTF-8 (no overlongs, surrogates or code points
// above U+10FFFF). Sets *code_points to the number of code points when
// non-null.
bool IsValidUtf8(const std::string &s, size_t *code_points = nullptr);

} // namespace pogohome_api
