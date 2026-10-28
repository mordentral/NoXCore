// What Delving Mode's and Undead Tide's records tables share: their text files' fields (one
// record a line, '|' between fields, ',' and ':' within them) and the file written whole.
// (Internal to the modes' sources.)
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace nox::server::detail {

/// A field made safe for a line: its separators and line breaks become '_'.
inline std::string cleanField(std::string s)
{
    std::ranges::replace_if(s, [](char c) { return c == '|' || c == ',' || c == ':' || c == '\n' || c == '\r'; }, '_');
    return s;
}

/// Today's date, UTC ("2026-10-11"): a record's date field.
inline std::string recordDate()
{
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
    return buf;
}

/// Splits on `by`, empty fields kept (a trailing separator gives an empty last field too).
inline std::vector<std::string> splitFields(const std::string& s, char by)
{
    std::vector<std::string> out;
    std::string part;
    std::istringstream in(s);
    while (std::getline(in, part, by)) {
        out.push_back(part);
    }
    if (!s.empty() && s.back() == by) {
        out.emplace_back();
    }
    return out;
}

/// The table written to `file`, a record a line (toLine's), whole through a temporary: a crash
/// mid-write never loses the table. No file: nothing written.
template <class Record, class ToLine>
void saveRecordLines(const std::filesystem::path& file, const std::vector<Record>& records, ToLine toLine)
{
    if (file.empty()) {
        return;
    }
    const std::filesystem::path temp = file.string() + ".new";
    {
        std::ofstream out(temp, std::ios::trunc);
        for (const Record& r : records) {
            out << toLine(r) << '\n';
        }
        if (!out) {
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, file, ec);
}

} // namespace nox::server::detail
