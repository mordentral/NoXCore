// A small streaming JSON writer, indented, for the export metadata.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace videotools {

class JsonWriter {
public:
    JsonWriter& beginObject() { return open('{'); }
    JsonWriter& endObject() { return close('}'); }
    JsonWriter& beginArray() { return open('['); }
    JsonWriter& endArray() { return close(']'); }

    /// The next value's key (inside an object).
    JsonWriter& key(std::string_view k)
    {
        separate();
        string(k);
        out_ += ": ";
        afterKey_ = true;
        return *this;
    }

    JsonWriter& value(std::string_view v) { return scalar([&] { string(v); }); }
    JsonWriter& value(const char* v) { return value(std::string_view(v)); }
    JsonWriter& value(const std::string& v) { return value(std::string_view(v)); }
    JsonWriter& value(bool v) { return scalar([&] { out_ += v ? "true" : "false"; }); }
    JsonWriter& value(std::nullptr_t) { return scalar([&] { out_ += "null"; }); }
    JsonWriter& value(std::int64_t v) { return scalar([&] { out_ += std::to_string(v); }); }
    JsonWriter& value(int v) { return value(std::int64_t{v}); }
    JsonWriter& value(unsigned v) { return value(std::int64_t{v}); }
    JsonWriter& value(std::size_t v) { return value(static_cast<std::int64_t>(v)); }

    template <class T>
    JsonWriter& field(std::string_view k, const T& v)
    {
        key(k);
        return value(v);
    }

    /// Text that is already valid JSON (a number, say).
    JsonWriter& raw(std::string_view json) { return scalar([&] { out_ += json; }); }

    const std::string& str() const { return out_; }

private:
    struct Level {
        char close;
        bool empty = true;
    };

    template <class F>
    JsonWriter& scalar(F write)
    {
        separate();
        write();
        return *this;
    }

    JsonWriter& open(char c)
    {
        separate();
        out_ += c;
        levels_.push_back({c == '{' ? '}' : ']'});
        return *this;
    }

    JsonWriter& close(char c)
    {
        const bool empty = levels_.back().empty;
        levels_.pop_back();
        if (!empty) {
            newline();
        }
        out_ += c;
        return *this;
    }

    // Comma and indentation before a value or key, unless it follows its own key.
    void separate()
    {
        if (afterKey_) {
            afterKey_ = false;
            return;
        }
        if (levels_.empty()) {
            return;
        }
        if (!levels_.back().empty) {
            out_ += ',';
        }
        levels_.back().empty = false;
        newline();
    }

    void newline()
    {
        out_ += '\n';
        out_.append(levels_.size() * 2, ' ');
    }

    void string(std::string_view s)
    {
        out_ += '"';
        for (const char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            switch (c) {
            case '"':
                out_ += "\\\"";
                break;
            case '\\':
                out_ += "\\\\";
                break;
            case '\n':
                out_ += "\\n";
                break;
            case '\r':
                out_ += "\\r";
                break;
            case '\t':
                out_ += "\\t";
                break;
            default:
                if (c < 0x20 || c >= 0x80) {
                    // Game strings are 8-bit; read bytes >= 0x80 as Latin-1 to keep the file UTF-8.
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out_ += buf;
                } else {
                    out_ += ch;
                }
            }
        }
        out_ += '"';
    }

    std::string out_;
    std::vector<Level> levels_;
    bool afterKey_ = false;
};

} // namespace videotools
