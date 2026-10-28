// A script's data read into C++ (script_data.hpp), and the built-in scripts by path
// (builtin_scripts.hpp).
#include "noxcore/modes/script/script_data.hpp"

#include "noxcore/modes/script/builtin_scripts.hpp"
#include "noxcore/modes/script/luau_vm.hpp"
#include "noxcore/util/x87.hpp"

#include "lua.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace nox::modes::script {

namespace {

constexpr int kDeepest = 16;
constexpr std::size_t kMostValues = 65536;

const char* kindName(DataValue::Kind kind)
{
    switch (kind) {
    case DataValue::Kind::Nil: return "nil";
    case DataValue::Kind::Boolean: return "a boolean";
    case DataValue::Kind::Number: return "a number";
    case DataValue::Kind::String: return "a string";
    case DataValue::Kind::Table: return "a table";
    }
    return "?";
}

std::string placeOf(const std::string& where)
{
    return where.empty() ? std::string("the data") : where;
}

} // namespace

// In integers, so no thread's rounding can reach it: a conversion in a rounding scope of its own
// isn't enough (the compiler may convert before the switch, the value already in a register).
float nearestFloat(double v) noexcept
{
    const auto b = std::bit_cast<std::uint64_t>(v);
    const std::uint32_t sign = static_cast<std::uint32_t>(b >> 63) << 31;
    const int exponent = static_cast<int>((b >> 52) & 0x7FF);
    const std::uint64_t fraction = b & ((std::uint64_t{1} << 52) - 1);
    if (exponent == 0x7FF) {
        return std::bit_cast<float>(sign | 0x7F800000u | (fraction != 0 ? 0x400000u : 0u)); // (infinity, NaN)
    }
    if (exponent == 0) {
        return std::bit_cast<float>(sign); // (zero, and a double's denormals: far under a float's)
    }
    // The 53 bits, shifted down to a float's 24 (fewer for a float's denormal), rounded to nearest
    // with ties to even; a carry out of the fraction goes into the exponent by itself.
    int e = exponent - 1023 + 127;
    const std::uint64_t bits53 = fraction | (std::uint64_t{1} << 52);
    int shift = 52 - 23;
    if (e <= 0) {
        shift += 1 - e;
        e = 0;
        if (shift > 54) {
            return std::bit_cast<float>(sign);
        }
    }
    std::uint64_t kept = bits53 >> shift;
    const std::uint64_t rest = bits53 & ((std::uint64_t{1} << shift) - 1);
    const std::uint64_t half = std::uint64_t{1} << (shift - 1);
    if (rest > half || (rest == half && (kept & 1) != 0)) {
        ++kept;
    }
    if (e == 0) {
        return std::bit_cast<float>(sign | static_cast<std::uint32_t>(kept)); // (a denormal, or the least normal)
    }
    if (e >= 0xFF) {
        return std::bit_cast<float>(sign | 0x7F800000u);
    }
    const std::uint64_t out = (static_cast<std::uint64_t>(e) << 23) + (kept - (std::uint64_t{1} << 23));
    return std::bit_cast<float>(sign | static_cast<std::uint32_t>(std::min<std::uint64_t>(out, 0x7F800000u)));
}

struct DataValue::Builder {
    lua_State* L = nullptr;
    std::size_t values = 0;

    DataValue read(int idx, std::string where, int depth)
    {
        if (++values > kMostValues) {
            throw std::runtime_error("the data holds more than " + std::to_string(kMostValues) + " values");
        }
        DataValue v;
        v.where_ = std::move(where);
        switch (lua_type(L, idx)) {
        case LUA_TNIL:
            break;
        case LUA_TBOOLEAN:
            v.kind_ = Kind::Boolean;
            v.boolean_ = lua_toboolean(L, idx) != 0;
            break;
        case LUA_TNUMBER:
            v.kind_ = Kind::Number;
            v.number_ = lua_tonumber(L, idx);
            break;
        case LUA_TSTRING: {
            std::size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            v.kind_ = Kind::String;
            v.string_.assign(s, len);
            break;
        }
        case LUA_TTABLE:
            v.kind_ = Kind::Table;
            readTable(v, idx, depth);
            break;
        default:
            throw std::runtime_error(placeOf(v.where_) + " is a " + lua_typename(L, lua_type(L, idx)) + ", not data");
        }
        return v;
    }

    void readTable(DataValue& v, int idx, int depth)
    {
        if (depth >= kDeepest) {
            throw std::runtime_error(placeOf(v.where_) + " is more than " + std::to_string(kDeepest) + " tables deep");
        }
        if (!lua_checkstack(L, 4)) {
            throw std::runtime_error("the data's reader ran out of stack");
        }
        std::map<std::size_t, DataValue> numbered;
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            const int value = lua_gettop(L);
            if (lua_type(L, -2) == LUA_TSTRING) {
                std::size_t len = 0;
                const char* s = lua_tolstring(L, -2, &len);
                std::string key(s, len);
                std::string where = v.where_.empty() ? key : v.where_ + "." + key;
                DataValue field = read(value, std::move(where), depth + 1);
                v.fields_.emplace(std::move(key), std::move(field));
            } else if (lua_type(L, -2) == LUA_TNUMBER) {
                const double k = lua_tonumber(L, -2);
                if (!(k >= 1 && k <= static_cast<double>(kMostValues) && std::floor(k) == k)) {
                    throw std::runtime_error(placeOf(v.where_) + " has a number key that isn't a list's (" + std::to_string(k) + ")");
                }
                const auto n = static_cast<std::size_t>(k);
                numbered.emplace(n, read(value, v.where_ + "[" + std::to_string(n) + "]", depth + 1));
            } else {
                throw std::runtime_error(placeOf(v.where_) + " has a key that's neither a string nor a number");
            }
            lua_pop(L, 1); // (the value; the key stays for lua_next)
        }
        // The list: keys 1 to n, no holes.
        std::size_t next = 1;
        for (auto& [n, item] : numbered) {
            if (n != next) {
                throw std::runtime_error(placeOf(v.where_) + "'s list has a hole at [" + std::to_string(next) + "]");
            }
            v.list_.push_back(std::move(item));
            ++next;
        }
    }
};

void DataValue::need(Kind kind) const
{
    if (kind_ != kind) {
        throw std::runtime_error(placeOf(where_) + " is " + kindName(kind_) + ", not " + kindName(kind));
    }
}

bool DataValue::asBool() const
{
    need(Kind::Boolean);
    return boolean_;
}

double DataValue::asNumber() const
{
    need(Kind::Number);
    return number_;
}

int DataValue::asInt() const
{
    need(Kind::Number);
    if (!(number_ >= std::numeric_limits<int>::min() && number_ <= std::numeric_limits<int>::max() && std::floor(number_) == number_)) {
        throw std::runtime_error(placeOf(where_) + " isn't a whole number in an int's range");
    }
    return static_cast<int>(number_);
}

std::uint32_t DataValue::asUint() const
{
    need(Kind::Number);
    if (!(number_ >= 0 && number_ <= std::numeric_limits<std::uint32_t>::max() && std::floor(number_) == number_)) {
        throw std::runtime_error(placeOf(where_) + " isn't a whole number from 0 to 2^32 - 1");
    }
    return static_cast<std::uint32_t>(number_);
}

float DataValue::asFloat() const
{
    need(Kind::Number);
    return nearestFloat(number_);
}

const std::string& DataValue::asString() const
{
    need(Kind::String);
    return string_;
}

std::size_t DataValue::size() const
{
    need(Kind::Table);
    return list_.size();
}

const DataValue& DataValue::operator[](std::size_t index) const
{
    need(Kind::Table);
    if (index >= list_.size()) {
        throw std::runtime_error(placeOf(where_) + " has no [" + std::to_string(index + 1) + "] (its list has " + std::to_string(list_.size()) + ")");
    }
    return list_[index];
}

const std::vector<DataValue>& DataValue::list() const
{
    need(Kind::Table);
    return list_;
}

const DataValue& DataValue::operator[](std::string_view key) const
{
    const DataValue* v = find(key);
    if (!v) {
        throw std::runtime_error(placeOf(where_) + " has no " + std::string(key));
    }
    return *v;
}

const DataValue* DataValue::find(std::string_view key) const
{
    need(Kind::Table);
    const auto it = fields_.find(key);
    return it != fields_.end() ? &it->second : nullptr;
}

const std::map<std::string, DataValue, std::less<>>& DataValue::fields() const
{
    need(Kind::Table);
    return fields_;
}

// (Out of line with its own rounding: a script's numerals parsed, and its numbers read, to nearest.)
NOX_NOINLINE DataValue readData(std::string_view name, std::string_view source)
{
    const util::RoundingScope nearest(false);
    LuauVm vm(LuauVm::Limits{16u << 20, 4'000'000, std::chrono::milliseconds(2000)});
    std::string error;
    if (!vm.run(name, source, 1, error)) {
        throw std::runtime_error(error);
    }
    try {
        DataValue::Builder builder{vm.thread()};
        return builder.read(lua_gettop(vm.thread()), "", 0);
    } catch (const std::runtime_error&) {
        throw;
    } catch (const std::exception& e) {
        // (A Lua error while reading: its memory.)
        throw std::runtime_error(std::string(name) + ": its data couldn't be read (" + e.what() + ")");
    }
}

DataValue readBuiltinData(std::string_view mode, std::string_view path)
{
    const auto source = builtinScript(mode, path);
    if (!source) {
        throw std::runtime_error(std::string(mode) + "'s " + std::string(path) + " isn't built in");
    }
    return readData(std::string(mode) + "/" + std::string(path), *source);
}

namespace {

// The length of the field name at `text[i]` (just past a '{') when a '}' closes it; 0: not a field.
std::size_t fieldAt(std::string_view text, std::size_t i)
{
    std::size_t n = 0;
    while (i + n < text.size() && (std::isalnum(static_cast<unsigned char>(text[i + n])) || text[i + n] == '_')) {
        ++n;
    }
    return n > 0 && i + n < text.size() && text[i + n] == '}' ? n : 0;
}

} // namespace

std::string fillText(std::string_view text, std::initializer_list<TextField> fields)
{
    std::string out;
    out.reserve(text.size() + 32);
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{') {
            if (const std::size_t n = fieldAt(text, i + 1)) {
                const std::string_view name = text.substr(i + 1, n);
                const TextField* f = nullptr;
                for (const TextField& x : fields) {
                    if (x.name == name) {
                        f = &x;
                        break;
                    }
                }
                if (f) {
                    out += f->value;
                    i += n + 1;
                    continue;
                }
            }
        }
        out += text[i];
    }
    return out;
}

std::vector<std::string> textFields(std::string_view text)
{
    std::vector<std::string> out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{') {
            if (const std::size_t n = fieldAt(text, i + 1)) {
                out.emplace_back(text.substr(i + 1, n));
                i += n + 1;
            }
        }
    }
    return out;
}

MapThings readMapThings(const DataValue& v)
{
    MapThings t;
    for (const DataValue& s : v["signs"].list()) {
        t.signs.push_back(s.asString());
    }
    if (t.signs.empty()) {
        throw std::runtime_error(v["signs"].where() + " names no sign");
    }
    t.portal = v["portal"].asString();
    t.board = v["board"].asString();
    if (v.fields().size() != 3) {
        throw std::runtime_error(v.where() + " has " + std::to_string(v.fields().size()) + " fields, not 3 (signs, portal, board)");
    }
    return t;
}

void needCount(const DataValue& v, std::size_t count)
{
    if (v.size() != count) {
        throw std::runtime_error(v.where() + " has " + std::to_string(v.size()) + ", not " + std::to_string(count));
    }
}

void needFields(const DataValue& v, std::string_view fields)
{
    std::vector<std::string> named = textFields(v.asString());
    std::ranges::sort(named);
    std::vector<std::string> want;
    for (std::size_t at = 0; at < fields.size();) {
        const std::size_t comma = std::min(fields.find(',', at), fields.size());
        want.emplace_back(fields.substr(at, comma - at));
        at = comma + 1;
    }
    std::ranges::sort(want);
    if (named != want) {
        throw std::runtime_error(v.where() + " must name " + (fields.empty() ? std::string("no fields") : "the fields " + std::string(fields)) + ", each once");
    }
}

std::optional<std::string_view> builtinScript(std::string_view mode, std::string_view path)
{
    for (const BuiltinScript& s : builtinScripts()) {
        if (s.mode == mode && s.path == path) {
            return s.text;
        }
    }
    return std::nullopt;
}

} // namespace nox::modes::script
