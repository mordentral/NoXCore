// A small JSON reader for a mod's mode.json (the values: null, true/false, numbers, strings, arrays,
// objects; strings' \u escapes made UTF-8). (Internal to the script modes' sources.)
#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace nox::modes::script::json {

struct Value;
using List = std::vector<Value>;                         ///< an array's values
using Members = std::map<std::string, Value, std::less<>>; ///< an object's, by key

struct Value {
    enum class Kind : unsigned char { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::shared_ptr<List> array;     ///< (Kind::Array)
    std::shared_ptr<Members> object; ///< (Kind::Object)

    bool isNull() const noexcept { return kind == Kind::Null; }
    bool isBool() const noexcept { return kind == Kind::Bool; }
    bool isNumber() const noexcept { return kind == Kind::Number; }
    bool isString() const noexcept { return kind == Kind::String; }
    bool isArray() const noexcept { return kind == Kind::Array; }
    bool isObject() const noexcept { return kind == Kind::Object; }
    /// An object's member; none: not an object, or no such key.
    const Value* find(std::string_view key) const;
};

/// What's wrong with the text, and where (a line, from 1).
struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// The one value `text` holds (whitespace around it); throws ParseError. Nesting deeper than 64 is
/// refused (a mod's file, read on the host).
Value parse(std::string_view text);

} // namespace nox::modes::script::json
