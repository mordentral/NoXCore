// A script's data read into C++: what a Luau chunk returns (nil, booleans, numbers, strings and
// tables of them) copied out of a VM of its own, which is then gone. For a native mode whose
// tables now live in its script ("data first"): it reads
// them once, at set-up, into its own structures, and never asks the script per frame.
//
// Exactness: the chunk is compiled and run with the rounding to nearest (whatever the thread's
// rounding is), so a number written in the script is the double C++ would make of the same
// literal; asFloat() is the float a C++ float literal of it would be (rounded to nearest once
// from that double, in integers: the same float for every value the modes' tables hold).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace nox::modes::script {

/// A value read out of a script. Every accessor that finds the wrong kind, or no such key or
/// index, throws std::runtime_error naming the value's place ("weapons[4].reach": the script's
/// own index, from 1).
class DataValue {
public:
    enum class Kind : std::uint8_t { Nil, Boolean, Number, String, Table };

    Kind kind() const noexcept { return kind_; }
    bool isNil() const noexcept { return kind_ == Kind::Nil; }
    /// Its place in what the script returned ("" the whole of it).
    const std::string& where() const noexcept { return where_; }

    bool asBool() const;
    double asNumber() const;
    /// A whole number in an int's range.
    int asInt() const;
    std::uint32_t asUint() const;
    /// The number as a C++ float literal of it would be (to nearest).
    float asFloat() const;
    const std::string& asString() const;

    /// A table's list (its keys 1 to n), from 0.
    std::size_t size() const;
    const DataValue& operator[](std::size_t index) const;
    const std::vector<DataValue>& list() const;
    /// A table's field by its (string) key.
    const DataValue& operator[](std::string_view key) const;
    /// The field, or null when the table has none of that key.
    const DataValue* find(std::string_view key) const;
    const std::map<std::string, DataValue, std::less<>>& fields() const;

    /// (Building one: script_data.cpp's.)
    struct Builder;

private:
    void need(Kind kind) const;

    Kind kind_ = Kind::Nil;
    bool boolean_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<DataValue> list_;
    std::map<std::string, DataValue, std::less<>> fields_;
    std::string where_;
};

/// The chunk `source` (named `name` in errors) run in a sandboxed VM of its own with no toolbox,
/// and the one value it returns read out (at most 16 tables deep, 64k values). Throws
/// std::runtime_error with the script's error, or what isn't data in what it returned (a
/// function, a key neither a string nor a whole number from 1, a list with holes).
DataValue readData(std::string_view name, std::string_view source);

/// A built-in mode's script (builtin_scripts.hpp) read as readData; throws when it isn't built in.
DataValue readBuiltinData(std::string_view mode, std::string_view path);

/// A double to a float rounded to nearest (ties to even), worked out in integers whatever the
/// thread's rounding is: the float a C++ float literal of the number would be (asFloat's).
float nearestFloat(double v) noexcept;

/// A field of a text template: its name and what it's filled with.
struct TextField {
    std::string_view name;
    std::string_view value;
};
/// A mode's words kept in its script as templates ("The tide takes {player}."): each "{name}"
/// (letters, digits, _) replaced by the value given for it, in one pass (what's filled in is
/// never read again); a field not given, or any other brace, is left as it is.
std::string fillText(std::string_view text, std::initializer_list<TextField> fields = {});
/// The fields a template names, in order (a mode checks its templates against its own list at
/// set-up, so a script that drops or misspells one is refused there, not in a frame).
std::vector<std::string> textFields(std::string_view text);

/// A mode's `things` (its data.luau's): the thing types its director finds on its own maps once
/// they load (thing.bin's names, the script's, never the code's): signs, a portal, a records board.
struct MapThings {
    std::vector<std::string> signs;
    std::string portal;
    std::string board;
};
/// `things` read: its signs (a list, at least one), its portal and board (strings), and nothing
/// else; else std::runtime_error naming its place.
MapThings readMapThings(const DataValue& v);

/// A list of `count` values, else std::runtime_error naming its place ("weapons has 3, not 37").
void needCount(const DataValue& v, std::size_t count);
/// Template `v` names `fields` (comma-separated), each once, and no others, else
/// std::runtime_error naming its place.
void needFields(const DataValue& v, std::string_view fields);

/// A line of a mode's text table: its key in the script, the string it's read into (null: a line
/// only the script says, checked as the rest are but not kept), the fields it takes
/// (comma-separated, each exactly once, in any order).
template <class Text>
struct TextLine {
    std::string_view key;
    std::string Text::*member;
    std::string_view fields;
};

/// A mode's text table read into `text` by its lines (each key's template, its fields checked:
/// needFields). The table holds these keys and no others: another is refused ("...: the Tide has
/// no such line", `mode` "the Tide").
template <class Text, std::size_t N>
void readText(const DataValue& table, Text& text, const TextLine<Text> (&lines)[N], std::string_view mode)
{
    for (const TextLine<Text>& line : lines) {
        const DataValue& v = table[line.key];
        needFields(v, line.fields);
        if (line.member) {
            text.*line.member = v.asString();
        } else {
            (void)v.asString();
        }
    }
    if (table.fields().size() != N) {
        for (const auto& [key, v] : table.fields()) {
            if (std::ranges::none_of(lines, [&](const TextLine<Text>& l) { return l.key == key; })) {
                throw std::runtime_error(v.where() + ": " + std::string(mode) + " has no such line");
            }
        }
    }
}

} // namespace nox::modes::script
