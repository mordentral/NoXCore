// The editor's sidecar file (sidecar.hpp): a small JSON object writer and reader.
#include "noxcore/mapedit/sidecar.hpp"

#include "noxcore/io/byte_reader.hpp"

#include <cctype>
#include <cstdio>

namespace nox::mapedit {

namespace {

std::string quote(std::string_view s)
{
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
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
    return out + "\"";
}

/// Reads one JSON value's extent from text (no semantic checks beyond nesting and strings).
class Scanner {
public:
    explicit Scanner(std::string_view t) : t_(t) {}

    void ws()
    {
        while (i_ < t_.size() && std::isspace(static_cast<unsigned char>(t_[i_]))) ++i_;
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
    [[noreturn]] void fail(const char* what) const { throw FormatError(std::string("sidecar: ") + what + " at " + std::to_string(i_)); }

    std::string string()
    {
        ws();
        if (i_ >= t_.size() || t_[i_] != '"') fail("expected a string");
        ++i_;
        std::string out;
        while (i_ < t_.size() && t_[i_] != '"') {
            char c = t_[i_++];
            if (c == '\\') {
                if (i_ >= t_.size()) fail("bad escape");
                const char e = t_[i_++];
                switch (e) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u': {
                    if (i_ + 4 > t_.size()) fail("bad escape");
                    c = static_cast<char>(std::stoi(std::string(t_.substr(i_, 4)), nullptr, 16) & 0xFF);
                    i_ += 4;
                    break;
                }
                default: c = e;
                }
            }
            out += c;
        }
        if (i_ >= t_.size()) fail("unterminated string");
        ++i_;
        return out;
    }

    /// The raw text of the next value.
    std::string_view value()
    {
        ws();
        const std::size_t start = i_;
        if (i_ >= t_.size()) fail("expected a value");
        const char c = t_[i_];
        if (c == '"') {
            (void)string();
        } else if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            ++i_;
            if (!eat(close)) {
                for (;;) {
                    if (c == '{') {
                        (void)string();
                        if (!eat(':')) fail("expected ':'");
                    }
                    (void)value();
                    if (eat(close)) break;
                    if (!eat(',')) fail("expected ','");
                }
            }
        } else {
            while (i_ < t_.size() && (std::isalnum(static_cast<unsigned char>(t_[i_])) || t_[i_] == '-' || t_[i_] == '+' || t_[i_] == '.')) ++i_;
            if (i_ == start) fail("unexpected character");
        }
        return t_.substr(start, i_ - start);
    }

    bool atEnd()
    {
        ws();
        return i_ == t_.size();
    }

private:
    std::string_view t_;
    std::size_t i_ = 0;
};

} // namespace

std::optional<std::string> Sidecar::text(std::string_view key) const
{
    const auto it = other.find(std::string(key));
    if (it == other.end()) {
        return std::nullopt;
    }
    try {
        Scanner s(it->second);
        return s.string();
    } catch (const FormatError&) {
        return std::nullopt;
    }
}

void Sidecar::setText(const std::string& key, std::string_view value)
{
    other[key] = quote(value);
}

std::string Sidecar::toJson() const
{
    std::string out = "{\n";
    out += "  \"format\": \"nxe\",\n";
    out += "  \"formatVersion\": " + std::to_string(formatVersion) + ",\n";
    out += "  \"origin\": " + quote(origin) + ",\n";
    out += "  \"originStock\": " + std::string(originStock ? "true" : "false") + ",\n";
    out += "  \"revision\": " + std::to_string(revision);
    for (const auto& [key, raw] : other) {
        out += ",\n  " + quote(key) + ": " + raw;
    }
    return out + "\n}\n";
}

Sidecar Sidecar::fromJson(std::string_view text)
{
    Scanner s(text);
    Sidecar out;
    if (!s.eat('{')) s.fail("expected an object");
    if (!s.eat('}')) {
        for (;;) {
            const std::string key = s.string();
            if (!s.eat(':')) s.fail("expected ':'");
            const std::string_view raw = s.value();
            const auto unquote = [&] {
                Scanner v(raw);
                return v.string();
            };
            if (key == "format") {
                if (unquote() != "nxe") s.fail("not an editor sidecar");
            } else if (key == "formatVersion") {
                out.formatVersion = std::stoi(std::string(raw));
            } else if (key == "origin") {
                out.origin = unquote();
            } else if (key == "originStock") {
                out.originStock = raw == "true";
            } else if (key == "revision") {
                out.revision = static_cast<std::uint32_t>(std::stoul(std::string(raw)));
            } else {
                out.other[key] = std::string(raw);
            }
            if (s.eat('}')) break;
            if (!s.eat(',')) s.fail("expected ','");
        }
    }
    if (!s.atEnd()) s.fail("text after the object");
    return out;
}

} // namespace nox::mapedit
