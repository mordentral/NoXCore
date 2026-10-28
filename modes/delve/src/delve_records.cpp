// Delving Mode's records table and its text file (one record a line: delve_records.hpp has the
// format). Every public call locks, as servers on other threads share one store.
#include "noxcore/server/delve_records.hpp"
#include "noxcore/server/record_class.hpp"

#include "record_file.hpp"

#include <algorithm>
#include <fstream>

namespace nox::server {

namespace {

constexpr auto clean = detail::cleanField;
constexpr auto split = detail::splitFields;

/// Deepest first; a tie keeps the order filed.
void order(std::vector<DelveRecord>& v)
{
    std::ranges::stable_sort(v, [](const DelveRecord& a, const DelveRecord& b) { return a.depth > b.depth; });
}

} // namespace

std::string DelveRecords::toLine(const DelveRecord& r)
{
    std::string party;
    for (const auto& [name, cls] : r.party) {
        party += (party.empty() ? "" : ",") + clean(name) + ":" + std::to_string(cls);
    }
    return std::to_string(r.depth) + "|" + std::to_string(r.act) + "|" + clean(r.date) + "|" + clean(r.difficulty) + "|" + party;
}

DelveRecord DelveRecords::fromLine(const std::string& line)
{
    DelveRecord r;
    const auto f = split(line, '|');
    // Fewer than five fields, or a depth or act that isn't a number: a bad line (depth 0, which the
    // loader skips).
    if (f.size() < 5) {
        return {};
    }
    try {
        r.depth = std::stoi(f[0]);
        r.act = std::stoi(f[1]);
    } catch (const std::exception&) {
        return {};
    }
    r.date = f[2];
    r.difficulty = f[3];
    for (const auto& member : split(f[4], ',')) {
        // The class after the last ':' (clean() turned any ':' in a name into '_').
        const auto colon = member.rfind(':');
        if (member.empty() || colon == std::string::npos) {
            continue;
        }
        // (Its class byte: record_class.hpp, multiclass and the NoX Core client's bit kept.)
        r.party.emplace_back(member.substr(0, colon), recordClassFromFile(std::atoi(member.substr(colon + 1).c_str())));
    }
    return r;
}

DelveRecords::DelveRecords(std::filesystem::path file)
    : file_(std::move(file))
{
    if (file_.empty()) {
        return;
    }
    std::ifstream in(file_);
    std::string line;
    while (std::getline(in, line)) {
        if (DelveRecord r = fromLine(line); r.depth > 0) {
            records_.push_back(std::move(r));
        }
    }
    order(records_);
    // Only the deepest kKept kept.
    if (records_.size() > kKept) {
        records_.resize(kKept);
    }
}

int DelveRecords::add(DelveRecord record)
{
    std::lock_guard lock(mutex_);
    if (record.depth <= 0) {
        return -1;
    }
    // (After the ties already there: the earlier keeps its place.)
    const auto at = std::ranges::find_if(records_, [&](const DelveRecord& r) { return r.depth < record.depth; });
    const int place = static_cast<int>(at - records_.begin());
    records_.insert(at, std::move(record));
    if (records_.size() > kKept) {
        records_.resize(kKept);
    }
    ++version_;
    save();
    return place < static_cast<int>(kKept) ? place : -1;
}

std::vector<DelveRecord> DelveRecords::top(std::size_t n) const
{
    std::lock_guard lock(mutex_);
    return {records_.begin(), records_.begin() + static_cast<std::ptrdiff_t>(std::min(n, records_.size()))};
}

int DelveRecords::best() const
{
    std::lock_guard lock(mutex_);
    return records_.empty() ? 0 : records_.front().depth;
}

std::uint64_t DelveRecords::version() const
{
    std::lock_guard lock(mutex_);
    return version_;
}

void DelveRecords::save() const
{
    detail::saveRecordLines(file_, records_, toLine);
}

} // namespace nox::server
