#pragma once

// A small JSON reader and string quoting, enough for the core's events (objects, arrays,
// strings with escapes, numbers, booleans, null). Malformed input yields null values.

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }

    // Members of an object; null if absent or not an object.
    const Value& operator[](std::string_view key) const {
        if (type_ == Type::Object)
            for (const auto& [k, v] : object_)
                if (k == key) return v;
        return null();
    }
    const std::vector<Value>& array() const {
        static const std::vector<Value> empty;
        return type_ == Type::Array ? array_ : empty;
    }
    std::string str(std::string fallback = {}) const { return type_ == Type::String ? string_ : fallback; }
    bool boolean(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    int64_t i64(int64_t fallback = 0) const { return type_ == Type::Number ? integer_ : fallback; }
    uint64_t u64(uint64_t fallback = 0) const { return type_ == Type::Number ? uint64_t(integer_) : fallback; }
    double num(double fallback = 0) const { return type_ == Type::Number ? number_ : fallback; }

    static Value parse(std::string_view text) {
        Parser p{text};
        Value v = p.value();
        return p.ok ? v : Value();
    }

private:
    static const Value& null() {
        static const Value v;
        return v;
    }

    struct Parser {
        std::string_view s;
        size_t i = 0;
        bool ok = true;

        void ws() {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
        }
        bool eat(char c) {
            ws();
            if (i < s.size() && s[i] == c) return ++i, true;
            return false;
        }
        Value fail() { return ok = false, Value(); }

        Value value() {
            ws();
            if (i >= s.size()) return fail();
            char c = s[i];
            Value v;
            if (c == '{') {
                ++i;
                v.type_ = Type::Object;
                if (eat('}')) return v;
                do {
                    ws();
                    if (i >= s.size() || s[i] != '"') return fail();
                    std::string key = string();
                    if (!eat(':')) return fail();
                    v.object_.emplace_back(std::move(key), value());
                } while (ok && eat(','));
                if (!eat('}')) return fail();
            } else if (c == '[') {
                ++i;
                v.type_ = Type::Array;
                if (eat(']')) return v;
                do v.array_.push_back(value());
                while (ok && eat(','));
                if (!eat(']')) return fail();
            } else if (c == '"') {
                v.type_ = Type::String;
                v.string_ = string();
            } else if (s.substr(i, 4) == "true") {
                i += 4, v.type_ = Type::Bool, v.bool_ = true;
            } else if (s.substr(i, 5) == "false") {
                i += 5, v.type_ = Type::Bool, v.bool_ = false;
            } else if (s.substr(i, 4) == "null") {
                i += 4;
            } else if (c == '-' || (c >= '0' && c <= '9')) {
                size_t start = i;
                bool integral = true;
                while (i < s.size() && std::string_view("+-0123456789.eE").find(s[i]) != std::string_view::npos) {
                    if (s[i] == '.' || s[i] == 'e' || s[i] == 'E') integral = false;
                    ++i;
                }
                std::string digits(s.substr(start, i - start));
                v.type_ = Type::Number;
                v.number_ = std::strtod(digits.c_str(), nullptr);
                v.integer_ = integral ? int64_t(std::strtoull(digits.c_str() + (digits[0] == '-'), nullptr, 10)) *
                                            (digits[0] == '-' ? -1 : 1)
                                      : int64_t(v.number_);
            } else {
                return fail();
            }
            return v;
        }

        static void utf8(std::string& out, uint32_t cp) {
            if (cp < 0x80) out += char(cp);
            else if (cp < 0x800) out += char(0xC0 | (cp >> 6)), out += char(0x80 | (cp & 0x3F));
            else if (cp < 0x10000)
                out += char(0xE0 | (cp >> 12)), out += char(0x80 | ((cp >> 6) & 0x3F)), out += char(0x80 | (cp & 0x3F));
            else
                out += char(0xF0 | (cp >> 18)), out += char(0x80 | ((cp >> 12) & 0x3F)),
                    out += char(0x80 | ((cp >> 6) & 0x3F)), out += char(0x80 | (cp & 0x3F));
        }
        uint32_t hex4() {
            if (i + 4 > s.size()) return ok = false, 0;
            uint32_t v = uint32_t(std::strtoul(std::string(s.substr(i, 4)).c_str(), nullptr, 16));
            i += 4;
            return v;
        }
        std::string string() {
            std::string out;
            ++i;  // opening quote
            while (i < s.size() && s[i] != '"') {
                char c = s[i++];
                if (c != '\\') {
                    out += c;
                    continue;
                }
                if (i >= s.size()) break;
                char e = s[i++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        uint32_t cp = hex4();
                        if (cp >= 0xD800 && cp < 0xDC00 && s.substr(i, 2) == "\\u") {
                            i += 2;
                            uint32_t lo = hex4();
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        utf8(out, cp);
                        break;
                    }
                    default: out += e;  // \" \\ \/
                }
            }
            if (i >= s.size()) ok = false;
            else ++i;  // closing quote
            return out;
        }
    };

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    int64_t integer_ = 0;
    std::string string_;
    std::vector<Value> array_;
    std::vector<std::pair<std::string, Value>> object_;
};

// `text` as a JSON string literal, quotes included.
inline std::string quote(std::string_view text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 15];
                } else {
                    out += char(c);
                }
        }
    }
    return out + "\"";
}

}  // namespace json
