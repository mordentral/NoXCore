// A JSON value that keeps its objects' keys in the order the file had them, and each number as it
// was written: the mod editor's mode.json form reads a file, changes the fields it knows, and
// writes it back with every other field kept where it was (noxcore's readers keep keys sorted or as
// raw text; this one is for files a person also edits by hand).
#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noxsdl::editor::json {

struct Value {
    enum class Kind : unsigned char { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    std::string text;                                ///< a string's (UTF-8), or a number as written ("1", "2.5e3")
    std::vector<Value> items;                        ///< (Kind::Array)
    std::vector<std::pair<std::string, Value>> members; ///< (Kind::Object) in the file's order

    static Value null() { return {}; }
    static Value of(bool b);
    static Value of(double n);
    static Value of(int n);
    static Value of(std::string_view s);
    static Value of(const char* s) { return of(std::string_view(s)); } ///< (not the bool's)
    static Value array(std::vector<Value> items = {});
    static Value object();

    bool isNull() const noexcept { return kind == Kind::Null; }
    bool isBool() const noexcept { return kind == Kind::Bool; }
    bool isNumber() const noexcept { return kind == Kind::Number; }
    bool isString() const noexcept { return kind == Kind::String; }
    bool isArray() const noexcept { return kind == Kind::Array; }
    bool isObject() const noexcept { return kind == Kind::Object; }
    /// A number's value (0 for anything else).
    double number() const;

    /// An object's member; null: not an object, or no such key.
    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);
    /// An object's member set: replaced in place when it's there, else added at the end.
    void set(std::string_view key, Value v);
    /// Added after the member `after` (the end when that isn't there), or replaced in place.
    void setAfter(std::string_view key, Value v, std::string_view after);
    /// An object's member taken out; whether it was there.
    bool erase(std::string_view key);
};

/// What's wrong with the text, and where (a line, from 1).
struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// The one value `text` holds (whitespace round it, a UTF-8 mark first); throws ParseError.
/// Nesting deeper than 64 is refused.
Value parse(std::string_view text);

/// Written with two-space indents, one member a line; an array of plain values (or of small
/// objects of plain values) on one line, as people write them; '\n' line ends, one at the end.
std::string write(const Value& v);

} // namespace noxsdl::editor::json
