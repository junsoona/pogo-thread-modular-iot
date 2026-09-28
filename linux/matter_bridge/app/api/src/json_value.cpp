#include "pogohome_api/json_value.hpp"

#include <cstdio>
#include <limits>

namespace pogohome_api {

const JsonValue *JsonValue::Find(const std::string &key) const
{
    if (type != Type::Object) {
        return nullptr;
    }
    for (const auto &member : object_members) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

namespace {

void AppendUtf8(std::string &out, uint32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Decodes one UTF-8 sequence starting at s[i]. Returns its length (1..4), or
// 0 when it is not a valid, shortest-form scalar value.
size_t DecodeUtf8(const std::string &s, size_t i, uint32_t &cp)
{
    const unsigned char c0 = static_cast<unsigned char>(s[i]);
    if (c0 < 0x80) {
        cp = c0;
        return 1;
    }
    size_t len;
    uint32_t min;
    if ((c0 & 0xE0) == 0xC0) {
        len = 2;
        cp = c0 & 0x1F;
        min = 0x80;
    } else if ((c0 & 0xF0) == 0xE0) {
        len = 3;
        cp = c0 & 0x0F;
        min = 0x800;
    } else if ((c0 & 0xF8) == 0xF0) {
        len = 4;
        cp = c0 & 0x07;
        min = 0x10000;
    } else {
        return 0;
    }
    if (i + len > s.size()) {
        return 0;
    }
    for (size_t k = 1; k < len; k++) {
        const unsigned char c = static_cast<unsigned char>(s[i + k]);
        if ((c & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (c & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    return len;
}

class Parser {
public:
    Parser(const std::string &text, const JsonParseLimits &limits) : text_(text), limits_(limits) {}

    bool ParseDocument(JsonValue &out, std::string &error)
    {
        SkipWhitespace();
        if (!ParseValue(out, 0)) {
            error = error_;
            return false;
        }
        SkipWhitespace();
        if (pos_ != text_.size()) {
            error = "unexpected data after the JSON value";
            return false;
        }
        return true;
    }

private:
    bool Fail(const char *message)
    {
        if (error_.empty()) {
            error_ = message;
        }
        return false;
    }

    void SkipWhitespace()
    {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                pos_++;
            } else {
                break;
            }
        }
    }

    bool Consume(const char *literal)
    {
        size_t i = 0;
        while (literal[i] != '\0') {
            if (pos_ + i >= text_.size() || text_[pos_ + i] != literal[i]) {
                return false;
            }
            i++;
        }
        pos_ += i;
        return true;
    }

    bool CountValue()
    {
        if (++values_ > limits_.max_values) {
            return Fail("too many JSON values");
        }
        return true;
    }

    bool ParseValue(JsonValue &out, size_t depth)
    {
        if (!CountValue()) {
            return false;
        }
        if (pos_ >= text_.size()) {
            return Fail("unexpected end of JSON");
        }
        char c = text_[pos_];
        if (c == '{') {
            return ParseObject(out, depth);
        }
        if (c == '[') {
            return ParseArray(out, depth);
        }
        if (c == '"') {
            out.type = JsonValue::Type::String;
            return ParseString(out.string_value);
        }
        if (c == 't') {
            if (!Consume("true")) {
                return Fail("invalid literal");
            }
            out.type = JsonValue::Type::Bool;
            out.bool_value = true;
            return true;
        }
        if (c == 'f') {
            if (!Consume("false")) {
                return Fail("invalid literal");
            }
            out.type = JsonValue::Type::Bool;
            out.bool_value = false;
            return true;
        }
        if (c == 'n') {
            if (!Consume("null")) {
                return Fail("invalid literal");
            }
            out.type = JsonValue::Type::Null;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return ParseNumber(out);
        }
        return Fail("unexpected character");
    }

    bool ParseObject(JsonValue &out, size_t depth)
    {
        if (depth + 1 > limits_.max_depth) {
            return Fail("JSON nested too deeply");
        }
        out.type = JsonValue::Type::Object;
        pos_++; // '{'
        SkipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            pos_++;
            return true;
        }
        while (true) {
            SkipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return Fail("expected an object key");
            }
            std::string key;
            if (!ParseString(key)) {
                return false;
            }
            for (const auto &member : out.object_members) {
                if (member.first == key) {
                    return Fail("duplicate object key");
                }
            }
            SkipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return Fail("expected ':'");
            }
            pos_++;
            SkipWhitespace();
            JsonValue value;
            if (!ParseValue(value, depth + 1)) {
                return false;
            }
            out.object_members.emplace_back(std::move(key), std::move(value));
            SkipWhitespace();
            if (pos_ >= text_.size()) {
                return Fail("unexpected end of JSON");
            }
            if (text_[pos_] == ',') {
                pos_++;
                continue;
            }
            if (text_[pos_] == '}') {
                pos_++;
                return true;
            }
            return Fail("expected ',' or '}'");
        }
    }

    bool ParseArray(JsonValue &out, size_t depth)
    {
        if (depth + 1 > limits_.max_depth) {
            return Fail("JSON nested too deeply");
        }
        out.type = JsonValue::Type::Array;
        pos_++; // '['
        SkipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            pos_++;
            return true;
        }
        while (true) {
            SkipWhitespace();
            JsonValue value;
            if (!ParseValue(value, depth + 1)) {
                return false;
            }
            out.array_items.push_back(std::move(value));
            SkipWhitespace();
            if (pos_ >= text_.size()) {
                return Fail("unexpected end of JSON");
            }
            if (text_[pos_] == ',') {
                pos_++;
                continue;
            }
            if (text_[pos_] == ']') {
                pos_++;
                return true;
            }
            return Fail("expected ',' or ']'");
        }
    }

    bool ParseHex4(uint32_t &value)
    {
        if (pos_ + 4 > text_.size()) {
            return Fail("truncated \\u escape");
        }
        value = 0;
        for (int i = 0; i < 4; i++) {
            char c = text_[pos_ + i];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return Fail("invalid \\u escape");
            }
        }
        pos_ += 4;
        return true;
    }

    bool ParseString(std::string &out)
    {
        pos_++; // opening quote
        out.clear();
        while (true) {
            if (pos_ >= text_.size()) {
                return Fail("unterminated string");
            }
            if (out.size() > limits_.max_string_bytes) {
                return Fail("string too long");
            }
            const unsigned char c = static_cast<unsigned char>(text_[pos_]);
            if (c == '"') {
                pos_++;
                if (out.size() > limits_.max_string_bytes) {
                    return Fail("string too long");
                }
                return true;
            }
            if (c < 0x20) {
                return Fail("control character in string");
            }
            if (c == '\\') {
                pos_++;
                if (pos_ >= text_.size()) {
                    return Fail("unterminated escape");
                }
                char e = text_[pos_++];
                switch (e) {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    uint32_t cp;
                    if (!ParseHex4(cp)) {
                        return false;
                    }
                    if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return Fail("unpaired surrogate");
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos_ + 2 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
                            return Fail("unpaired surrogate");
                        }
                        pos_ += 2;
                        uint32_t low;
                        if (!ParseHex4(low)) {
                            return false;
                        }
                        if (low < 0xDC00 || low > 0xDFFF) {
                            return Fail("unpaired surrogate");
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default:
                    return Fail("invalid escape");
                }
                continue;
            }
            uint32_t cp;
            size_t len = DecodeUtf8(text_, pos_, cp);
            if (len == 0) {
                return Fail("invalid UTF-8");
            }
            out.append(text_, pos_, len);
            pos_ += len;
        }
    }

    bool ParseNumber(JsonValue &out)
    {
        size_t start = pos_;
        bool negative = false;
        if (text_[pos_] == '-') {
            negative = true;
            pos_++;
        }
        if (pos_ >= text_.size()) {
            return Fail("invalid number");
        }
        if (text_[pos_] == '0') {
            pos_++;
        } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                pos_++;
            }
        } else {
            return Fail("invalid number");
        }
        size_t int_end = pos_;
        bool integer = true;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            integer = false;
            pos_++;
            size_t digits = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                pos_++;
            }
            if (pos_ == digits) {
                return Fail("invalid number");
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            integer = false;
            pos_++;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
                pos_++;
            }
            size_t digits = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                pos_++;
            }
            if (pos_ == digits) {
                return Fail("invalid number");
            }
        }
        out.type = JsonValue::Type::Number;
        out.is_integer = false;
        if (integer) {
            // Accumulate as a negative number so INT64_MIN fits.
            int64_t value = 0;
            bool overflow = false;
            for (size_t i = start + (negative ? 1 : 0); i < int_end; i++) {
                int digit = text_[i] - '0';
                if (value < (std::numeric_limits<int64_t>::min() + digit) / 10) {
                    overflow = true;
                    break;
                }
                value = value * 10 - digit;
            }
            if (!overflow) {
                if (negative) {
                    out.is_integer = true;
                    out.int_value = value;
                } else if (value != std::numeric_limits<int64_t>::min()) {
                    out.is_integer = true;
                    out.int_value = -value;
                }
            }
        }
        return true;
    }

    const std::string &text_;
    const JsonParseLimits &limits_;
    size_t pos_ = 0;
    size_t values_ = 0;
    std::string error_;
};

} // namespace

bool ParseJson(const std::string &text, JsonValue &out, std::string &error, const JsonParseLimits &limits)
{
    out = JsonValue();
    Parser parser(text, limits);
    return parser.ParseDocument(out, error);
}

void AppendJsonStringLiteral(std::string &out, const std::string &s)
{
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    out += '"';
}

bool IsValidUtf8(const std::string &s, size_t *code_points)
{
    size_t count = 0;
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp;
        size_t len = DecodeUtf8(s, i, cp);
        if (len == 0) {
            return false;
        }
        i += len;
        count++;
    }
    if (code_points != nullptr) {
        *code_points = count;
    }
    return true;
}

} // namespace pogohome_api
