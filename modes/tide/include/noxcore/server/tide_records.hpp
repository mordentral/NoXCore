// Undead Tide's records: the best runs a server has seen, kept in a text
// file so they outlive restarts, as Delving Mode's (delve_records.hpp). One store for every tide
// server of a process (a pool's slots run on their own threads: it locks). The director files a
// record as each run ends; the lobby's board (a large sign) tells the table to whoever walks up to
// it.
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace nox::server {

struct TideRecord {
    /// 2 the night's guardian slain, 1 dawn reached, 0 everyone fell.
    int outcome = 0;
    int seconds = 0;     ///< the run's time as it ended
    int minutes = 20;    ///< the run's length (dawn)
    std::string biome;
    std::string date;    ///< "2026-10-11" (UTC)
    std::string difficulty; ///< its difficulty's name ("": Adventurer, as before difficulties)
    int stage = 1;       ///< the stage it ended in (seconds then the whole run's)
    struct Member {
        std::string name;        ///< its player name
        std::uint8_t cls = 0;    ///< 0 warrior, 1 wizard, 2 conjurer, 3 multiclass; NoX Core's bit (record_class.hpp)
        int level = 1;
        int kills = 0;
        bool operator==(const Member&) const = default;
    };
    std::vector<Member> party;
    int kills() const;
    bool operator==(const TideRecord&) const = default;
};

/// Better first: the later stage, then the guardian slain (sooner first), then dawn reached, then
/// longer survived; ties by the party's kills.
bool tideRecordBefore(const TideRecord& a, const TideRecord& b);

class TideRecords {
public:
    /// The store kept in `file` (read now if it's there; none: a store of no records). Empty: kept
    /// in memory alone (tests).
    explicit TideRecords(std::filesystem::path file = {});

    /// Files a record (written to the file at once, the file's own runs merged in first: two
    /// stores on one file keep each other's). Returns its place in the table (0 the best), or -1
    /// when it's past the kept ones.
    int add(TideRecord record);
    /// The best `n`, best first.
    std::vector<TideRecord> top(std::size_t n) const;
    std::size_t size() const;

    static constexpr std::size_t kKept = 100;

    /// One record as a line of the file and back ("outcome|seconds|minutes|biome|date|
    /// name:class:level:kills,...|difficulty|stage"; names' '|', ',' and ':' made '_'; a line
    /// without the difficulty is an Adventurer's, without the stage a first stage's); a bad line
    /// gives seconds -1.
    static std::string toLine(const TideRecord& r);
    static TideRecord fromLine(const std::string& line);
    /// A record as the board tells it ("Dawn, guardian slain at 18:49: Jack (wizard 23), Bob
    /// (warrior 18); 2,311 slain, Crypt, 2026-10-11"): the Tide's script's words (told.record), as
    /// the board's. Throws std::runtime_error when the built-in script fails (a fault in the build).
    static std::string describe(const TideRecord& r);

private:
    void save() const;

    std::filesystem::path file_;
    mutable std::mutex mutex_;
    std::vector<TideRecord> records_;
};

} // namespace nox::server
