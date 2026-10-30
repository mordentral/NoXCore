// nox.records: a script mode's records for a host that keeps them. A script mode's Mode::openRecords
// gives a ModRecords (one a mode a host, shared by every game of the mode the host runs, each on a
// thread of its own: it locks), the host hands it to each game as ServerContext::records, and the
// script's nox.records.put / remove write its rows. Where the host keeps none, nox.records is nil
// and nothing is made or written.
//
// A row: a key (as nox.store's keys: a-z, 0-9, _ and -, parts joined by /) and a few named fields,
// each a number or a short text, stamped with the UTC time it was put ("2026-10-29 14:05"). What
// the rows mean is the mode's; the host only reads them (rows()), and decides what, if anything,
// it shows of them.
//
// Kept in a text file, a line a change, each put or remove appended as it's made (a put costs one
// short write), read back when the store is made (a key's last line wins) and written whole again
// (through a temporary) when the lines outnumber the rows well:
//
//   +<TAB>key<TAB>date<TAB>name=<n|s>value<TAB>...    (\\, \t, \n and \r escaped in values)
//   -<TAB>key
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nox::modes::script {

class ModRecords {
public:
    struct Field {
        std::string name;
        bool number = false;
        double n = 0;  ///< (number)
        std::string s; ///< (text)
        bool operator==(const Field&) const = default;
    };
    struct Row {
        std::string key;
        std::string date; ///< UTC, when it was last put
        std::vector<Field> fields;
        const Field* find(std::string_view name) const;
        /// A number field's value; `fallback` when there's none (or it's a text).
        double number(std::string_view name, double fallback = 0) const;
        /// A text field's value; "" when there's none (or it's a number).
        std::string text(std::string_view name) const;
        bool operator==(const Row&) const = default;
    };

    static constexpr std::size_t kMaxRows = 10'000;
    static constexpr std::size_t kMaxFields = 16;
    static constexpr std::size_t kMaxName = 32;  ///< a field's name: a-z, A-Z, 0-9 and _
    static constexpr std::size_t kMaxText = 64;  ///< a text field's bytes

    /// The store kept in `file` (read now if it's there). Empty: kept in memory alone (tests).
    explicit ModRecords(std::filesystem::path file = {});

    /// Empty: `fields` are fields a row may hold; else why not.
    static std::string fieldsProblem(const std::vector<Field>& fields);
    /// A row put (replacing the key's), stamped `date` (empty: now). Empty: done; else why not.
    std::string put(std::string_view key, std::vector<Field> fields, std::string date = {});
    /// Whether there was one.
    bool remove(std::string_view key);
    /// Every row, by key.
    std::vector<Row> rows() const;
    std::size_t size() const;
    /// Bumped by each put and remove.
    std::uint64_t version() const;

    /// A row as its file line and back (none: not a row's line).
    static std::string toLine(const Row& row);
    static std::optional<Row> fromLine(std::string_view line);

private:
    void appendLocked(const std::string& line);
    void rewriteLocked();

    std::filesystem::path file_;
    mutable std::mutex mutex_;
    std::map<std::string, Row, std::less<>> rows_;
    std::size_t lines_ = 0; ///< the file's lines
    std::uint64_t version_ = 0;
};

} // namespace nox::modes::script
