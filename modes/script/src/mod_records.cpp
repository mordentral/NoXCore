// A script mode's records (mod_records.hpp), and the toolbox's nox.records.
#include "noxcore/modes/script/mod_records.hpp"

#include "mod_parts.hpp"
#include "mod_store.hpp"
#include "toolbox.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <system_error>
#include <utility>

namespace nox::modes::script {

namespace fs = std::filesystem;

namespace {

/// "2026-10-29 14:05" (UTC, now).
std::string utcMinute()
{
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char s[32];
    std::snprintf(s, sizeof s, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    return s;
}

bool nameOk(std::string_view name)
{
    return !name.empty() && name.size() <= ModRecords::kMaxName &&
           std::ranges::all_of(name, [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; });
}

/// A date as put stamps it, or a test's: printable, no tab.
bool dateOk(std::string_view date)
{
    return !date.empty() && date.size() <= 32 && std::ranges::all_of(date, [](char c) { return c >= 0x20 && c != 0x7F; });
}

std::string escaped(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        default: out += c;
        }
    }
    return out;
}

std::optional<std::string> unescaped(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\') {
            out += s[i];
            continue;
        }
        if (++i == s.size()) {
            return std::nullopt;
        }
        switch (s[i]) {
        case '\\': out += '\\'; break;
        case 't': out += '\t'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        default: return std::nullopt;
        }
    }
    return out;
}

std::vector<std::string_view> splitTabs(std::string_view line)
{
    std::vector<std::string_view> out;
    for (std::size_t start = 0;;) {
        const std::size_t end = line.find('\t', start);
        out.push_back(line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) {
            return out;
        }
        start = end + 1;
    }
}

std::string numberText(double v)
{
    char s[40];
    std::snprintf(s, sizeof s, "%.17g", v);
    return s;
}

} // namespace

// ------------------------------------------------------------------------- a row

const ModRecords::Field* ModRecords::Row::find(std::string_view name) const
{
    const auto it = std::ranges::find(fields, name, &Field::name);
    return it != fields.end() ? &*it : nullptr;
}

double ModRecords::Row::number(std::string_view name, double fallback) const
{
    const Field* f = find(name);
    return f && f->number ? f->n : fallback;
}

std::string ModRecords::Row::text(std::string_view name) const
{
    const Field* f = find(name);
    return f && !f->number ? f->s : std::string();
}

std::string ModRecords::toLine(const Row& row)
{
    std::string out = "+\t" + row.key + "\t" + row.date;
    for (const Field& f : row.fields) {
        out += "\t" + f.name + "=" + (f.number ? "n" + numberText(f.n) : "s" + escaped(f.s));
    }
    return out;
}

std::optional<ModRecords::Row> ModRecords::fromLine(std::string_view line)
{
    const std::vector<std::string_view> parts = splitTabs(line);
    if (parts.size() < 3 || parts[0] != "+" || !ModStore::keyProblem(parts[1]).empty() || !dateOk(parts[2])) {
        return std::nullopt;
    }
    Row row{.key = std::string(parts[1]), .date = std::string(parts[2]), .fields = {}};
    for (std::size_t i = 3; i < parts.size(); ++i) {
        const std::string_view p = parts[i];
        const std::size_t eq = p.find('=');
        if (eq == std::string_view::npos || eq + 1 >= p.size() || !nameOk(p.substr(0, eq))) {
            return std::nullopt;
        }
        Field f{.name = std::string(p.substr(0, eq))};
        const std::string_view value = p.substr(eq + 2);
        if (p[eq + 1] == 'n') {
            const std::string text(value);
            char* end = nullptr;
            f.number = true;
            f.n = std::strtod(text.c_str(), &end);
            if (text.empty() || end != text.c_str() + text.size() || !std::isfinite(f.n)) {
                return std::nullopt;
            }
        } else if (p[eq + 1] == 's') {
            auto s = unescaped(value);
            if (!s) {
                return std::nullopt;
            }
            f.s = std::move(*s);
        } else {
            return std::nullopt;
        }
        row.fields.push_back(std::move(f));
    }
    if (!fieldsProblem(row.fields).empty()) {
        return std::nullopt;
    }
    return row;
}

// ------------------------------------------------------------------------- the store

ModRecords::ModRecords(fs::path file) : file_(std::move(file))
{
    if (file_.empty()) {
        return;
    }
    std::ifstream in(file_, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        ++lines_;
        if (line.starts_with("-\t")) {
            const std::string_view key = std::string_view(line).substr(2);
            if (const auto it = rows_.find(key); it != rows_.end()) {
                rows_.erase(it);
            }
        } else if (auto row = fromLine(line); row && (rows_.size() < kMaxRows || rows_.contains(row->key))) {
            std::string key = row->key;
            rows_[std::move(key)] = std::move(*row);
        }
    }
    in.close();
    if (lines_ > 2 * rows_.size() + 64) {
        rewriteLocked(); // (no other thread has it yet)
    }
}

std::string ModRecords::fieldsProblem(const std::vector<Field>& fields)
{
    if (fields.size() > kMaxFields) {
        return "a row holds at most " + std::to_string(kMaxFields) + " fields";
    }
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const Field& f = fields[i];
        if (!nameOk(f.name)) {
            return "a field's name is 1-" + std::to_string(kMaxName) + " of a-z, A-Z, 0-9 and _";
        }
        for (std::size_t k = 0; k < i; ++k) {
            if (fields[k].name == f.name) {
                return "the field " + f.name + " twice";
            }
        }
        if (f.number && !std::isfinite(f.n)) {
            return f.name + ": a number (not inf or nan)";
        }
        if (!f.number && (f.s.size() > kMaxText || std::ranges::any_of(f.s, [](char c) { return static_cast<unsigned char>(c) < 0x20; }))) {
            return f.name + ": a text of at most " + std::to_string(kMaxText) + " bytes, no control characters";
        }
    }
    return {};
}

std::string ModRecords::put(std::string_view key, std::vector<Field> fields, std::string date)
{
    if (std::string why = ModStore::keyProblem(key); !why.empty()) {
        return why;
    }
    if (std::string why = fieldsProblem(fields); !why.empty()) {
        return why;
    }
    if (date.empty()) {
        date = utcMinute();
    } else if (!dateOk(date)) {
        return "a date is printable text";
    }
    std::ranges::sort(fields, {}, &Field::name);
    const std::lock_guard lock(mutex_);
    if (!rows_.contains(key) && rows_.size() >= kMaxRows) {
        return "full (" + std::to_string(kMaxRows) + " rows)";
    }
    Row row{.key = std::string(key), .date = std::move(date), .fields = std::move(fields)};
    const std::string line = toLine(row);
    rows_[std::string(key)] = std::move(row);
    ++version_;
    appendLocked(line);
    return {};
}

bool ModRecords::remove(std::string_view key)
{
    const std::lock_guard lock(mutex_);
    const auto it = rows_.find(key);
    if (it == rows_.end()) {
        return false;
    }
    rows_.erase(it);
    ++version_;
    appendLocked("-\t" + std::string(key));
    return true;
}

std::vector<ModRecords::Row> ModRecords::rows() const
{
    const std::lock_guard lock(mutex_);
    std::vector<Row> out;
    out.reserve(rows_.size());
    for (const auto& [key, row] : rows_) {
        out.push_back(row);
    }
    return out;
}

std::size_t ModRecords::size() const
{
    const std::lock_guard lock(mutex_);
    return rows_.size();
}

std::uint64_t ModRecords::version() const
{
    const std::lock_guard lock(mutex_);
    return version_;
}

void ModRecords::appendLocked(const std::string& line)
{
    if (file_.empty()) {
        return;
    }
    ++lines_;
    if (lines_ > 4 * rows_.size() + 1024) {
        rewriteLocked();
        return;
    }
    std::ofstream out(file_, std::ios::binary | std::ios::app);
    out << line << '\n';
}

void ModRecords::rewriteLocked()
{
    if (file_.empty()) {
        return;
    }
    fs::path temp = file_;
    temp += ".new";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        for (const auto& [key, row] : rows_) {
            out << toLine(row) << '\n';
        }
        out.flush();
        if (!out) {
            return; // (the old file stays, the lines appended to it as before)
        }
    }
    std::error_code ec;
    fs::rename(temp, file_, ec);
    if (!ec) {
        lines_ = rows_.size();
    }
}

// ------------------------------------------------------------------------- nox.records

namespace {

ModRecords& recordsOf(lua_State* L)
{
    // (A script mode's ServerContext::records is always its own Mode::openRecords': a ModRecords.)
    void* const r = toolbox::contextOf(L).records.get();
    if (!r) {
        luaL_error(L, "this host keeps no records");
    }
    return *static_cast<ModRecords*>(r);
}

int recordsPut(lua_State* L)
{
    // nox.records.put(key, { name = number | text, ... }): true, or false and why.
    toolbox::refuseInsideAsked(L, "nox.records.put");
    ModRecords& records = recordsOf(L);
    size_t n = 0;
    const char* k = luaL_checklstring(L, 1, &n);
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<ModRecords::Field> fields;
    std::string error;
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
        if (error.empty()) {
            if (lua_type(L, -2) != LUA_TSTRING) {
                error = "a row's fields are named (string keys)";
            } else if (fields.size() >= ModRecords::kMaxFields) {
                error = "a row holds at most " + std::to_string(ModRecords::kMaxFields) + " fields";
            } else {
                ModRecords::Field f{.name = lua_tostring(L, -2)};
                if (lua_type(L, -1) == LUA_TNUMBER) {
                    f.number = true;
                    f.n = lua_tonumber(L, -1);
                } else if (lua_type(L, -1) == LUA_TSTRING) {
                    size_t len = 0;
                    const char* s = lua_tolstring(L, -1, &len);
                    f.s.assign(s, len);
                } else {
                    error = f.name + ": a number or a text";
                }
                fields.push_back(std::move(f));
            }
        }
        lua_pop(L, 1);
    }
    if (error.empty()) {
        error = records.put(std::string_view(k, n), std::move(fields));
    }
    if (!error.empty()) {
        lua_pushboolean(L, 0);
        lua_pushlstring(L, error.data(), error.size());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int recordsRemove(lua_State* L)
{
    // nox.records.remove(key): whether there was one.
    toolbox::refuseInsideAsked(L, "nox.records.remove");
    ModRecords& records = recordsOf(L);
    size_t n = 0;
    const char* k = luaL_checklstring(L, 1, &n);
    lua_pushboolean(L, records.remove(std::string_view(k, n)));
    return 1;
}

} // namespace

void pushRecordsTable(lua_State* L)
{
    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, recordsPut, "put");
    lua_setfield(L, -2, "put");
    lua_pushcfunction(L, recordsRemove, "remove");
    lua_setfield(L, -2, "remove");
    lua_setreadonly(L, -1, 1);
}

} // namespace nox::modes::script
