// mode.json's reader (json.hpp).
#include "json.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>

namespace nox::modes::script::json {

const Value* Value::find(std::string_view key) const
{
    if (!object) {
        return nullptr;
    }
    const auto it = object->find(key);
    return it != object->end() ? &it->second : nullptr;
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view t) : t_(t) {}

    Value document()
    {
        Value v = value(0);
        ws();
        if (i_ != t_.size()) {
            fail("text after the value");
        }
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& what) const
    {
        int line = 1;
        for (std::size_t k = 0; k < i_ && k < t_.size(); ++k) {
            line += t_[k] == '\n' ? 1 : 0;
        }
        throw ParseError(what + " (line " + std::to_string(line) + ")");
    }

    void ws()
    {
        while (i_ < t_.size() && std::isspace(static_cast<unsigned char>(t_[i_]))) {
            ++i_;
        }
    }

    bool eat(char c)
    {
        ws();
        if (i_ < t_.size() && t_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }

    bool word(std::string_view w)
    {
        if (t_.substr(i_, w.size()) == w) {
            i_ += w.size();
            return true;
        }
        return false;
    }

    static void putUtf8(std::string& out, std::uint32_t cp)
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

    std::uint32_t hex4()
    {
        if (i_ + 4 > t_.size()) {
            fail("a short \\u escape");
        }
        std::uint32_t v = 0;
        const auto r = std::from_chars(t_.data() + i_, t_.data() + i_ + 4, v, 16);
        if (r.ptr != t_.data() + i_ + 4) {
            fail("a bad \\u escape");
        }
        i_ += 4;
        return v;
    }

    std::string string()
    {
        // (at the opening quote)
        ++i_;
        std::string out;
        for (;;) {
            if (i_ >= t_.size()) {
                fail("a string not closed");
            }
            const char c = t_[i_++];
            if (c == '"') {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                fail("a control character in a string");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= t_.size()) {
                fail("a string not closed");
            }
            switch (const char e = t_[i_++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint32_t cp = hex4();
                if (cp >= 0xD800 && cp < 0xDC00 && word("\\u")) {
                    const std::uint32_t lo = hex4();
                    cp = lo >= 0xDC00 && lo < 0xE000 ? 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00) : 0xFFFD;
                } else if (cp >= 0xD800 && cp < 0xE000) {
                    cp = 0xFFFD;
                }
                putUtf8(out, cp);
                break;
            }
            default: fail(std::string("a bad escape \\") + e);
            }
        }
    }

    Value number()
    {
        const std::size_t start = i_;
        if (i_ < t_.size() && t_[i_] == '-') {
            ++i_;
        }
        while (i_ < t_.size() && (std::isdigit(static_cast<unsigned char>(t_[i_])) || t_[i_] == '.' || t_[i_] == 'e' || t_[i_] == 'E' ||
                                  t_[i_] == '+' || t_[i_] == '-')) {
            ++i_;
        }
        Value v;
        v.kind = Value::Kind::Number;
        const auto r = std::from_chars(t_.data() + start, t_.data() + i_, v.number);
        if (r.ec != std::errc{} || r.ptr != t_.data() + i_ || !std::isfinite(v.number)) {
            i_ = start;
            fail("a bad number");
        }
        return v;
    }

    Value value(int depth)
    {
        if (depth > 64) {
            fail("nested too deep");
        }
        ws();
        if (i_ >= t_.size()) {
            fail("a value missing");
        }
        Value v;
        const char c = t_[i_];
        if (c == '"') {
            v.kind = Value::Kind::String;
            v.string = string();
        } else if (c == '{') {
            ++i_;
            v.kind = Value::Kind::Object;
            v.object = std::make_shared<Members>();
            if (!eat('}')) {
                for (;;) {
                    ws();
                    if (i_ >= t_.size() || t_[i_] != '"') {
                        fail("a key expected");
                    }
                    std::string key = string();
                    if (!eat(':')) {
                        fail("':' expected after \"" + key + "\"");
                    }
                    (*v.object)[std::move(key)] = value(depth + 1);
                    if (eat('}')) {
                        break;
                    }
                    if (!eat(',')) {
                        fail("',' or '}' expected");
                    }
                }
            }
        } else if (c == '[') {
            ++i_;
            v.kind = Value::Kind::Array;
            v.array = std::make_shared<List>();
            if (!eat(']')) {
                for (;;) {
                    v.array->push_back(value(depth + 1));
                    if (eat(']')) {
                        break;
                    }
                    if (!eat(',')) {
                        fail("',' or ']' expected");
                    }
                }
            }
        } else if (word("true")) {
            v.kind = Value::Kind::Bool;
            v.boolean = true;
        } else if (word("false")) {
            v.kind = Value::Kind::Bool;
        } else if (word("null")) {
            v.kind = Value::Kind::Null;
        } else if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            v = number();
        } else {
            fail(std::string("unexpected '") + c + "'");
        }
        return v;
    }

    std::string_view t_;
    std::size_t i_ = 0;
};

} // namespace

Value parse(std::string_view text)
{
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") {
        text.remove_prefix(3); // (a UTF-8 mark, as Notepad writes one)
    }
    return Parser(text).document();
}

} // namespace nox::modes::script::json
