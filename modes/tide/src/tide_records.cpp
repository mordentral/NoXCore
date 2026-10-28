// Undead Tide's record table: the best runs, kept sorted in memory and saved one per line to a
// text file (the line format is in tide_records.hpp). The tide director files a record at each
// run's end; the lobby board reads the top of the table.
#include "noxcore/server/tide_records.hpp"
#include "noxcore/server/record_class.hpp"

#include "record_file.hpp"

#include <algorithm>
#include <fstream>
#include <set>

namespace nox::server {

namespace {

constexpr auto clean = detail::cleanField;
constexpr auto split = detail::splitFields;

} // namespace

int TideRecord::kills() const
{
    int n = 0;
    for (const Member& m : party) {
        n += m.kills;
    }
    return n;
}

bool tideRecordBefore(const TideRecord& a, const TideRecord& b)
{
    if (a.stage != b.stage) {
        return a.stage > b.stage;
    }
    if (a.outcome != b.outcome) {
        return a.outcome > b.outcome;
    }
    if (a.seconds != b.seconds) {
        return a.outcome == 2 ? a.seconds < b.seconds : a.seconds > b.seconds;
    }
    return a.kills() > b.kills();
}

std::string TideRecords::toLine(const TideRecord& r)
{
    std::string party;
    for (const TideRecord::Member& m : r.party) {
        party += (party.empty() ? "" : ",") + clean(m.name) + ":" + std::to_string(m.cls) + ":" + std::to_string(m.level) + ":" + std::to_string(m.kills);
    }
    return std::to_string(r.outcome) + "|" + std::to_string(r.seconds) + "|" + std::to_string(r.minutes) + "|" + clean(r.biome) + "|" + clean(r.date) +
           "|" + party + "|" + clean(r.difficulty) + "|" + std::to_string(r.stage);
}

TideRecord TideRecords::fromLine(const std::string& line)
{
    TideRecord r;
    const auto f = split(line, '|');
    if (f.size() < 6) {
        r.seconds = -1;
        return r;
    }
    try {
        // (Clamped: 0 fell, 1 dawn, 2 guardian slain.)
        r.outcome = std::clamp(std::stoi(f[0]), 0, 2);
        r.seconds = std::stoi(f[1]);
        r.minutes = std::stoi(f[2]);
    } catch (const std::exception&) {
        r.seconds = -1;
        return r;
    }
    r.biome = f[3];
    r.date = f[4];
    if (f.size() > 6) {
        r.difficulty = f[6];
    }
    if (f.size() > 7) {
        r.stage = std::clamp(std::atoi(f[7].c_str()), 1, 999);
    }
    // The party: "name:class:level:kills" members separated by ','; a malformed one is skipped.
    for (const auto& member : split(f[5], ',')) {
        const auto p = split(member, ':');
        if (p.size() < 4 || p[0].empty()) {
            continue;
        }
        TideRecord::Member m;
        m.name = p[0];
        m.cls = recordClassFromFile(std::atoi(p[1].c_str())); // (record_class.hpp: multiclass, NoX Core)
        m.level = std::max(1, std::atoi(p[2].c_str()));
        m.kills = std::max(0, std::atoi(p[3].c_str()));
        r.party.push_back(std::move(m));
    }
    return r;
}

TideRecords::TideRecords(std::filesystem::path file)
    : file_(std::move(file))
{
    if (file_.empty()) {
        return;
    }
    // Load every good line, then keep the best kKept.
    std::ifstream in(file_);
    std::string line;
    while (std::getline(in, line)) {
        if (TideRecord r = fromLine(line); r.seconds >= 0 && !r.party.empty()) {
            records_.push_back(std::move(r));
        }
    }
    std::ranges::stable_sort(records_, tideRecordBefore);
    if (records_.size() > kKept) {
        records_.resize(kKept);
    }
}

int TideRecords::add(TideRecord record)
{
    std::lock_guard lock(mutex_);
    if (record.party.empty()) {
        return -1;
    }
    // The file's runs merged in first: another store on it (another server in the same game
    // folder) keeps its own.
    if (!file_.empty()) {
        std::set<std::string> have;
        for (const TideRecord& r : records_) {
            have.insert(toLine(r));
        }
        std::ifstream in(file_);
        std::string line;
        while (std::getline(in, line)) {
            if (TideRecord r = fromLine(line); r.seconds >= 0 && !r.party.empty() && have.insert(toLine(r)).second) {
                records_.push_back(std::move(r));
            }
        }
        std::ranges::stable_sort(records_, tideRecordBefore);
    }
    // (After the ties already there: the earlier keeps its place.)
    const auto at = std::ranges::find_if(records_, [&](const TideRecord& r) { return tideRecordBefore(record, r); });
    const int place = static_cast<int>(at - records_.begin());
    records_.insert(at, std::move(record));
    if (records_.size() > kKept) {
        records_.resize(kKept);
    }
    save();
    return place < static_cast<int>(kKept) ? place : -1;
}

std::vector<TideRecord> TideRecords::top(std::size_t n) const
{
    std::lock_guard lock(mutex_);
    return {records_.begin(), records_.begin() + static_cast<std::ptrdiff_t>(std::min(n, records_.size()))};
}

std::size_t TideRecords::size() const
{
    std::lock_guard lock(mutex_);
    return records_.size();
}

// Writes the whole table (the caller holds the lock).
void TideRecords::save() const
{
    detail::saveRecordLines(file_, records_, toLine);
}

} // namespace nox::server
