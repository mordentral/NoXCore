// Delving Mode's records: the deepest runs a server has
// seen, kept in a text file so they outlive restarts. One store for every delve server of a
// process (a pool's slots run on their own threads: it locks). The director files a record
// as each run ends; players ask for the table with "?record" in chat (a stock client), or see
// it behind noxsdl's map button (MSG_NOXCPP_MODE_F4 kRecords).
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace nox::server {

struct DelveRecord {
    int depth = 0;           ///< the deepest level the run reached
    int act = 1;             ///< that level's act
    std::string date;        ///< "2026-10-11" (UTC)
    std::string difficulty;  ///< its difficulty mode's name
    /// The party, as it began: names and classes (0 warrior, 1 wizard, 2 conjurer).
    std::vector<std::pair<std::string, std::uint8_t>> party;
    bool operator==(const DelveRecord&) const = default;
};

class DelveRecords {
public:
    /// The store kept in `file` (read now if it's there; none: a store of no records). Empty:
    /// kept in memory alone (tests).
    explicit DelveRecords(std::filesystem::path file = {});

    /// Files a record (written to the file at once). Returns its place in the table (0 the
    /// deepest), or -1 when it's past the kept ones.
    int add(DelveRecord record);
    /// The deepest `n`, deepest first (ties: the earlier first).
    std::vector<DelveRecord> top(std::size_t n) const;
    /// The deepest depth on record (0: none).
    int best() const;
    /// Bumped by each add (who's seen the table follows it).
    std::uint64_t version() const;

    /// How many it keeps.
    static constexpr std::size_t kKept = 100;

    /// One record as a line of the file and back ("depth|act|date|difficulty|name:class,...";
    /// names' '|', ',' and ':' made '_'); nullopt-like empty depth for a bad line.
    static std::string toLine(const DelveRecord& r);
    static DelveRecord fromLine(const std::string& line);

private:
    void save() const;

    std::filesystem::path file_;
    mutable std::mutex mutex_;
    std::vector<DelveRecord> records_;
    std::uint64_t version_ = 0;
};

} // namespace nox::server
