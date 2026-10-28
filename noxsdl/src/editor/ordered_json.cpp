// The mod editor's order-keeping JSON (ordered_json.hpp).
#include "editor/ordered_json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace noxsdl::editor::json {

namespace {

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s)
    {
        if (s_.starts_with("\xEF\xBB\xBF")) {
            at_ = 3;
        }
    }

    Value document()
    {
        Value v = value(0);
        space();
        if (at_ != s_.size()) {
            fail("more after the value");
        }
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& why) const
    {
        const auto line = 1 + std::count(s_.begin(), s_.begin() + static_cast<std::ptrdiff_t>(std::min(at_, s_.size())), '\n');
        throw ParseError("line " + std::to_string(line) + ": " + why);
    }

    void space()
    {
        while (at_ < s_.size() && (s_[at_] == ' ' || s_[at_] == '\t' || s_[at_] == '\r' || s_[at_] == '\n')) {
            ++at_;
        }
    }

    bool take(char c)
    {
        space();
        if (at_ < s_.size() && s_[at_] == c) {
            ++at_;
            return true;
        }
        return false;
    }

    void expect(char c)
    {
        if (!take(c)) {
            fail(std::string("'") + c + "' expected");
        }
    }

    bool word(std::string_view w)
    {
        if (s_.substr(at_, w.size()) == w) {
            at_ += w.size();
            return true;
        }
        return false;
    }

    static void utf8(std::string& out, std::uint32_t cp)
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
        if (at_ + 4 > s_.size()) {
            fail("a \\u escape needs four hex digits");
        }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[at_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
            else fail("a \\u escape needs four hex digits");
        }
        return v;
    }

    std::string string()
    {
        expect('"');
        std::string out;
        while (true) {
            if (at_ >= s_.size()) {
                fail("a string isn't closed");
            }
            const char c = s_[at_++];
            if (c == '"') {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                fail("a control character in a string (write it as an escape)");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= s_.size()) {
                fail("a string isn't closed");
            }
            switch (const char e = s_[at_++]) {
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
                if (cp >= 0xD800 && cp < 0xDC00 && s_.substr(at_, 2) == "\\u") {
                    at_ += 2;
                    const std::uint32_t low = hex4();
                    cp = low >= 0xDC00 && low < 0xE000 ? 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00) : 0xFFFD;
                } else if (cp >= 0xD800 && cp < 0xE000) {
                    cp = 0xFFFD;
                }
                utf8(out, cp);
                break;
            }
            default: fail(std::string("an unknown escape \\") + e);
            }
        }
    }

    Value value(int depth)
    {
        if (depth > 64) {
            fail("nested deeper than 64");
        }
        space();
        if (at_ >= s_.size()) {
            fail("a value expected");
        }
        Value v;
        const char c = s_[at_];
        if (c == '{') {
            ++at_;
            v.kind = Value::Kind::Object;
            if (take('}')) {
                return v;
            }
            do {
                space();
                std::string key = string();
                expect(':');
                Value member = value(depth + 1);
                if (v.find(key)) {
                    fail("the key \"" + key + "\" is there twice");
                }
                v.members.emplace_back(std::move(key), std::move(member));
            } while (take(','));
            expect('}');
        } else if (c == '[') {
            ++at_;
            v.kind = Value::Kind::Array;
            if (take(']')) {
                return v;
            }
            do {
                v.items.push_back(value(depth + 1));
            } while (take(','));
            expect(']');
        } else if (c == '"') {
            v.kind = Value::Kind::String;
            v.text = string();
        } else if (word("true")) {
            v.kind = Value::Kind::Bool;
            v.boolean = true;
        } else if (word("false")) {
            v.kind = Value::Kind::Bool;
        } else if (word("null")) {
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            const std::size_t start = at_;
            if (s_[at_] == '-') ++at_;
            while (at_ < s_.size() && ((s_[at_] >= '0' && s_[at_] <= '9') || s_[at_] == '.' || s_[at_] == 'e' || s_[at_] == 'E' ||
                                       ((s_[at_] == '+' || s_[at_] == '-') && (s_[at_ - 1] == 'e' || s_[at_ - 1] == 'E')))) {
                ++at_;
            }
            v.kind = Value::Kind::Number;
            v.text = std::string(s_.substr(start, at_ - start));
            double d = 0;
            const auto [end, ec] = std::from_chars(v.text.data(), v.text.data() + v.text.size(), d);
            if (ec != std::errc() || end != v.text.data() + v.text.size()) {
                fail("\"" + v.text + "\" isn't a number");
            }
        } else {
            fail(std::string("unexpected '") + c + "'");
        }
        return v;
    }

    std::string_view s_;
    std::size_t at_ = 0;
};

void quoted(std::string& out, std::string_view s)
{
    out += '"';
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

bool plain(const Value& v)
{
    return !v.isArray() && !v.isObject();
}

/// Whether it's written on one line: plain values, arrays of them, objects of those.
bool small(const Value& v)
{
    if (plain(v)) {
        return true;
    }
    if (v.isArray()) {
        return std::ranges::all_of(v.items, plain);
    }
    return std::ranges::all_of(v.members, [](const auto& m) { return plain(m.second) || (m.second.isArray() && std::ranges::all_of(m.second.items, plain)); });
}

void inlineForm(std::string& out, const Value& v)
{
    switch (v.kind) {
    case Value::Kind::Null: out += "null"; break;
    case Value::Kind::Bool: out += v.boolean ? "true" : "false"; break;
    case Value::Kind::Number: out += v.text.empty() ? "0" : v.text; break;
    case Value::Kind::String: quoted(out, v.text); break;
    case Value::Kind::Array:
        out += '[';
        for (std::size_t i = 0; i < v.items.size(); ++i) {
            out += i ? ", " : "";
            inlineForm(out, v.items[i]);
        }
        out += ']';
        break;
    case Value::Kind::Object:
        if (v.members.empty()) {
            out += "{}";
            break;
        }
        out += "{ ";
        for (std::size_t i = 0; i < v.members.size(); ++i) {
            out += i ? ", " : "";
            quoted(out, v.members[i].first);
            out += ": ";
            inlineForm(out, v.members[i].second);
        }
        out += " }";
        break;
    }
}

constexpr std::size_t kLineWidth = 110;

void block(std::string& out, const Value& v, int indent)
{
    const std::string pad(static_cast<std::size_t>(indent + 2), ' ');
    const std::string end(static_cast<std::size_t>(indent), ' ');
    if (v.isObject() && !v.members.empty()) {
        out += "{\n";
        for (std::size_t i = 0; i < v.members.size(); ++i) {
            out += pad;
            quoted(out, v.members[i].first);
            out += ": ";
            block(out, v.members[i].second, indent + 2);
            out += i + 1 < v.members.size() ? ",\n" : "\n";
        }
        out += end + "}";
        return;
    }
    if (v.isArray() && !v.items.empty()) {
        std::string one;
        inlineForm(one, v);
        if (std::ranges::all_of(v.items, plain) && one.size() + static_cast<std::size_t>(indent) <= kLineWidth) {
            out += one;
            return;
        }
        out += "[\n";
        for (std::size_t i = 0; i < v.items.size(); ++i) {
            out += pad;
            std::string item;
            inlineForm(item, v.items[i]);
            if (small(v.items[i]) && item.size() + pad.size() <= kLineWidth) {
                out += item;
            } else {
                block(out, v.items[i], indent + 2);
            }
            out += i + 1 < v.items.size() ? ",\n" : "\n";
        }
        out += end + "]";
        return;
    }
    inlineForm(out, v);
}

} // namespace

Value Value::of(bool b)
{
    Value v;
    v.kind = Kind::Bool;
    v.boolean = b;
    return v;
}

Value Value::of(double n)
{
    Value v;
    v.kind = Kind::Number;
    char buf[64];
    const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, std::isfinite(n) ? n : 0.0);
    v.text.assign(buf, ec == std::errc() ? end : buf);
    return v;
}

Value Value::of(int n)
{
    Value v;
    v.kind = Kind::Number;
    v.text = std::to_string(n);
    return v;
}

Value Value::of(std::string_view s)
{
    Value v;
    v.kind = Kind::String;
    v.text = std::string(s);
    return v;
}

Value Value::array(std::vector<Value> items)
{
    Value v;
    v.kind = Kind::Array;
    v.items = std::move(items);
    return v;
}

Value Value::object()
{
    Value v;
    v.kind = Kind::Object;
    return v;
}

double Value::number() const
{
    if (!isNumber()) {
        return 0;
    }
    double d = 0;
    std::from_chars(text.data(), text.data() + text.size(), d);
    return d;
}

const Value* Value::find(std::string_view key) const
{
    if (!isObject()) {
        return nullptr;
    }
    const auto it = std::ranges::find(members, key, [](const auto& m) { return std::string_view(m.first); });
    return it == members.end() ? nullptr : &it->second;
}

Value* Value::find(std::string_view key)
{
    return const_cast<Value*>(std::as_const(*this).find(key));
}

void Value::set(std::string_view key, Value v)
{
    if (Value* there = find(key)) {
        *there = std::move(v);
        return;
    }
    kind = Kind::Object;
    members.emplace_back(std::string(key), std::move(v));
}

void Value::setAfter(std::string_view key, Value v, std::string_view after)
{
    if (Value* there = find(key)) {
        *there = std::move(v);
        return;
    }
    kind = Kind::Object;
    const auto it = std::ranges::find(members, after, [](const auto& m) { return std::string_view(m.first); });
    members.emplace(it == members.end() ? members.end() : it + 1, std::string(key), std::move(v));
}

bool Value::erase(std::string_view key)
{
    if (!isObject()) {
        return false;
    }
    return std::erase_if(members, [&](const auto& m) { return m.first == key; }) > 0;
}

Value parse(std::string_view text)
{
    return Parser(text).document();
}

std::string write(const Value& v)
{
    std::string out;
    block(out, v, 0);
    out += '\n';
    return out;
}

} // namespace noxsdl::editor::json
