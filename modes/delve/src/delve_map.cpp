// MSG_NOXCPP_MODE_F4's messages built and parsed (delve_map.hpp gives each layout). Every one is
// framed {0xF4, sub, u16 length, the rest}; strings and lists carry a u8 length, so they are cut
// at 255 bytes or entries.
#include "noxcore/server/delve_map.hpp"

#include "noxcore/modes/mode_frame.hpp"
#include "noxcore/net/transport.hpp"

#include "delve_data.hpp"

#include <algorithm>

namespace nox::server::delvemap {

namespace {

// The frame: op, sub, the rest's length (u16, little-endian), the rest (mode_frame.hpp).
std::vector<std::uint8_t> framed(std::uint8_t sub, const std::vector<std::uint8_t>& rest)
{
    return modes::framed(kOp, sub, rest);
}

// An act: u8 length and the biome's name, its six rows (each a u8 count and the rooms' numbers),
// u8 count and the path, u8 the gate taken.
void putAct(std::vector<std::uint8_t>& out, const Act& a)
{
    const std::size_t n = std::min<std::size_t>(a.biome.size(), 255);
    out.push_back(static_cast<std::uint8_t>(n));
    out.insert(out.end(), a.biome.begin(), a.biome.begin() + static_cast<std::ptrdiff_t>(n));
    const auto bytes = [&](const std::vector<std::uint8_t>& v) {
        const std::size_t k = std::min<std::size_t>(v.size(), 255);
        out.push_back(static_cast<std::uint8_t>(k));
        out.insert(out.end(), v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k));
    };
    for (const auto& row : a.rows) {
        bytes(row);
    }
    bytes(a.path);
    out.push_back(a.taken);
}

/// Reads an act at `at`, moving it on; false when the bytes run out.
bool getAct(std::span<const std::uint8_t> in, std::size_t& at, Act& a)
{
    const auto bytes = [&](std::vector<std::uint8_t>& v) {
        // Each list is a u8 count and that many bytes; false when the count runs past the end.
        if (at >= in.size() || in.size() < at + 1 + in[at]) {
            return false;
        }
        const std::size_t k = in[at++];
        v.assign(in.begin() + static_cast<std::ptrdiff_t>(at), in.begin() + static_cast<std::ptrdiff_t>(at + k));
        at += k;
        return true;
    };
    std::vector<std::uint8_t> name;
    if (!bytes(name)) {
        return false;
    }
    a.biome.assign(name.begin(), name.end());
    for (auto& row : a.rows) {
        if (!bytes(row)) {
            return false;
        }
    }
    if (!bytes(a.path) || at >= in.size()) {
        return false;
    }
    a.taken = in[at++];
    return true;
}

} // namespace

std::vector<std::uint8_t> runMessage(const Run& run)
{
    // u16 depth, u8 room, u8 mystery, u8 past count, the past acts, the act, u8 fork (0 / 1),
    // the fork's two acts, and only when acts done were left out, u16 how many; an act: u8
    // length, its biome's name, its six rows (each u8 count, the rooms), u8 count, its path, u8
    // the gate taken.
    std::vector<std::uint8_t> ahead;
    putAct(ahead, run.act);
    ahead.push_back(run.fork ? 1 : 0);
    if (run.fork) {
        putAct(ahead, (*run.fork)[0]);
        putAct(ahead, (*run.fork)[1]);
    }
    // The acts done from the last back, as many as fit in the message (the frame's 4 bytes, the
    // 5 before them, what's ahead and the count of those left out, when any are).
    std::vector<std::vector<std::uint8_t>> done(run.past.size());
    std::size_t size = 4 + 5 + ahead.size();
    for (std::size_t i = 0; i < run.past.size(); ++i) {
        putAct(done[i], run.past[i]);
        size += done[i].size();
    }
    std::size_t first = run.past.size() > 255 ? run.past.size() - 255 : 0;
    for (std::size_t i = 0; i < first; ++i) {
        size -= done[i].size();
    }
    while (first < done.size() && size + (run.earlier + first > 0 ? 2 : 0) > net::kMaxReliablePayload) {
        size -= done[first++].size();
    }
    std::vector<std::uint8_t> rest = {static_cast<std::uint8_t>(run.depth), static_cast<std::uint8_t>(run.depth >> 8), run.room, run.mystery,
                                      static_cast<std::uint8_t>(done.size() - first)};
    for (std::size_t i = first; i < done.size(); ++i) {
        rest.insert(rest.end(), done[i].begin(), done[i].end());
    }
    rest.insert(rest.end(), ahead.begin(), ahead.end());
    if (const std::size_t earlier = std::min<std::size_t>(run.earlier + first, 0xFFFF); earlier > 0) {
        rest.push_back(static_cast<std::uint8_t>(earlier));
        rest.push_back(static_cast<std::uint8_t>(earlier >> 8));
    }
    return framed(kRun, rest);
}

std::vector<std::uint8_t> endMessage()
{
    return framed(kEnd, {});
}

std::vector<std::uint8_t> multiclassMessage(bool on)
{
    return framed(kMulticlass, {static_cast<std::uint8_t>(on ? 1 : 0)});
}

std::vector<std::uint8_t> campfireMessage(std::span<const Upgradable> spells)
{
    std::vector<std::uint8_t> rest = {static_cast<std::uint8_t>(std::min<std::size_t>(spells.size(), 255))};
    for (std::size_t i = 0; i < spells.size() && i < 255; ++i) {
        rest.push_back(spells[i].spell);
        rest.push_back(spells[i].level);
    }
    return framed(kCampfire, rest);
}

std::optional<std::vector<Upgradable>> parseCampfire(std::span<const std::uint8_t> rest)
{
    // u8 count, then two bytes each.
    if (rest.empty() || rest.size() < 1 + 2 * static_cast<std::size_t>(rest[0])) {
        return std::nullopt;
    }
    std::vector<Upgradable> out;
    for (std::size_t i = 0; i < rest[0]; ++i) {
        out.push_back({rest[1 + 2 * i], rest[2 + 2 * i]});
    }
    return out;
}

std::vector<std::uint8_t> upgradeMessage(std::uint8_t spell)
{
    return framed(kUpgrade, {spell});
}

// (The blessings' words are the Delve's script's: data.luau's blessings, delve_data.hpp.)
const char* blessingName(Blessing b)
{
    const auto& all = delve_data::data().blessings;
    const auto i = static_cast<std::size_t>(b);
    return i < all.size() ? all[i].name.c_str() : "?";
}

const char* blessingText(Blessing b)
{
    const auto& all = delve_data::data().blessings;
    const auto i = static_cast<std::size_t>(b);
    return i < all.size() ? all[i].text.c_str() : "";
}

std::vector<std::uint8_t> blessingMessage(std::span<const BlessingOffer> offer)
{
    std::vector<std::uint8_t> rest = {static_cast<std::uint8_t>(std::min<std::size_t>(offer.size(), 255))};
    for (std::size_t i = 0; i < offer.size() && i < 255; ++i) {
        rest.push_back(static_cast<std::uint8_t>(offer[i].blessing));
        rest.push_back(offer[i].held);
    }
    return framed(kBlessing, rest);
}

std::optional<std::vector<BlessingOffer>> parseBlessing(std::span<const std::uint8_t> rest)
{
    if (rest.empty() || rest.size() < 1 + 2 * static_cast<std::size_t>(rest[0])) {
        return std::nullopt;
    }
    std::vector<BlessingOffer> out;
    for (std::size_t i = 0; i < rest[0]; ++i) {
        // A blessing number past the last: not a valid message.
        if (rest[1 + 2 * i] >= kBlessings) {
            return std::nullopt;
        }
        out.push_back({static_cast<Blessing>(rest[1 + 2 * i]), rest[2 + 2 * i]});
    }
    return out;
}

std::vector<std::uint8_t> blessMessage(Blessing b)
{
    return framed(kBless, {static_cast<std::uint8_t>(b)});
}

std::vector<std::uint8_t> objectiveMessage(const Objective& o)
{
    std::vector<std::uint8_t> rest = {o.kind,
                                      o.state,
                                      static_cast<std::uint8_t>(o.done),
                                      static_cast<std::uint8_t>(o.done >> 8),
                                      static_cast<std::uint8_t>(o.of),
                                      static_cast<std::uint8_t>(o.of >> 8),
                                      static_cast<std::uint8_t>(o.secondsLeft),
                                      static_cast<std::uint8_t>(o.secondsLeft >> 8)};
    const std::size_t n = std::min<std::size_t>(o.text.size(), 255);
    rest.push_back(static_cast<std::uint8_t>(n));
    rest.insert(rest.end(), o.text.begin(), o.text.begin() + static_cast<std::ptrdiff_t>(n));
    return framed(kObjective, rest);
}

std::optional<Objective> parseObjective(std::span<const std::uint8_t> rest)
{
    if (rest.size() < 9 || rest.size() < 9u + rest[8]) {
        return std::nullopt;
    }
    Objective o;
    o.kind = rest[0];
    o.state = rest[1];
    o.done = static_cast<std::uint16_t>(rest[2] | rest[3] << 8);
    o.of = static_cast<std::uint16_t>(rest[4] | rest[5] << 8);
    o.secondsLeft = static_cast<std::uint16_t>(rest[6] | rest[7] << 8);
    o.text.assign(rest.begin() + 9, rest.begin() + 9 + rest[8]);
    return o;
}

std::vector<std::vector<std::uint8_t>> recordsMessages(std::span<const Record> records)
{
    // At most 10 records sent, and at most 16 of each party; whole records to a message while they
    // fit (each message's rest: u8 its count, its records).
    const auto recordBytes = [](const Record& r, std::size_t members) {
        std::vector<std::uint8_t> out;
        const auto text = [&](const std::string& s) {
            const std::size_t n = std::min<std::size_t>(s.size(), 255);
            out.push_back(static_cast<std::uint8_t>(n));
            out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
        };
        out.push_back(static_cast<std::uint8_t>(r.depth));
        out.push_back(static_cast<std::uint8_t>(r.depth >> 8));
        out.push_back(r.act);
        text(r.date);
        text(r.difficulty);
        out.push_back(static_cast<std::uint8_t>(members));
        for (std::size_t k = 0; k < members; ++k) {
            text(r.party[k].first);
            out.push_back(r.party[k].second);
        }
        return out;
    };
    const std::size_t room = net::kMaxReliablePayload - 4; // (a message's rest: the frame's 4 bytes off)
    std::vector<std::vector<std::uint8_t>> out;
    std::vector<std::uint8_t> rest = {0};
    const auto send = [&] {
        out.push_back(framed(out.empty() ? kRecords : kRecordsMore, rest));
        rest = {0};
    };
    for (std::size_t i = 0; i < records.size() && i < 10; ++i) {
        std::size_t members = std::min<std::size_t>(records[i].party.size(), 16);
        std::vector<std::uint8_t> bytes = recordBytes(records[i], members);
        while (1 + bytes.size() > room && members > 0) { // (a party too big for a message alone: its last left out)
            bytes = recordBytes(records[i], --members);
        }
        if (rest.size() + bytes.size() > room) {
            send();
        }
        rest.insert(rest.end(), bytes.begin(), bytes.end());
        ++rest[0];
    }
    send();
    return out;
}

std::optional<std::vector<Record>> parseRecords(std::span<const std::uint8_t> rest)
{
    std::size_t at = 0;
    const auto byte = [&](std::uint8_t& out) {
        if (at >= rest.size()) {
            return false;
        }
        out = rest[at++];
        return true;
    };
    const auto text = [&](std::string& out) {
        std::uint8_t n = 0;
        if (!byte(n) || rest.size() < at + n) {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(rest.data() + at), n);
        at += n;
        return true;
    };
    std::uint8_t count = 0;
    if (!byte(count)) {
        return std::nullopt;
    }
    std::vector<Record> out;
    for (std::uint8_t i = 0; i < count; ++i) {
        Record r;
        std::uint8_t lo = 0, hi = 0, members = 0;
        if (!byte(lo) || !byte(hi) || !byte(r.act) || !text(r.date) || !text(r.difficulty) || !byte(members)) {
            return std::nullopt;
        }
        r.depth = static_cast<std::uint16_t>(lo | hi << 8);
        for (std::uint8_t k = 0; k < members; ++k) {
            std::string name;
            std::uint8_t cls = 0;
            if (!text(name) || !byte(cls)) {
                return std::nullopt;
            }
            r.party.emplace_back(std::move(name), cls);
        }
        out.push_back(std::move(r));
    }
    return out;
}

std::optional<std::pair<std::uint8_t, std::span<const std::uint8_t>>> parse(std::span<const std::uint8_t> m)
{
    return modes::unframed(kOp, m);
}

std::optional<Run> parseRun(std::span<const std::uint8_t> rest)
{
    if (rest.size() < 5) {
        return std::nullopt;
    }
    Run run;
    run.depth = static_cast<std::uint16_t>(rest[0] | rest[1] << 8);
    run.room = rest[2];
    run.mystery = rest[3];
    const std::size_t past = rest[4];
    std::size_t at = 5;
    for (std::size_t i = 0; i < past; ++i) {
        Act a;
        if (!getAct(rest, at, a)) {
            return std::nullopt;
        }
        run.past.push_back(std::move(a));
    }
    if (!getAct(rest, at, run.act) || at >= rest.size()) {
        return std::nullopt;
    }
    if (rest[at++] != 0) {
        std::array<Act, 2> fork;
        if (!getAct(rest, at, fork[0]) || !getAct(rest, at, fork[1])) {
            return std::nullopt;
        }
        run.fork = std::move(fork);
    }
    if (rest.size() >= at + 2) { // (the acts done left out, when any were)
        run.earlier = static_cast<std::uint16_t>(rest[at] | rest[at + 1] << 8);
    }
    return run;
}

} // namespace nox::server::delvemap
