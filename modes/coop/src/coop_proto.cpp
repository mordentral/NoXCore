// MSG_NOXCPP_COOP's encoding (coop_proto.hpp has the layouts).
#include "noxcore/coop/coop_proto.hpp"
#include "noxcore/modes/mode_frame.hpp"

#include <algorithm>

namespace nox::coop::coopmsg {

namespace {

// Little-endian, as every Nox message.
void put16(std::vector<std::uint8_t>& out, std::uint16_t v)
{
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

// A u8 length, then the bytes (cut to `most`).
void putText(std::vector<std::uint8_t>& out, std::string_view s, std::size_t most)
{
    const std::size_t n = std::min(s.size(), most);
    out.push_back(static_cast<std::uint8_t>(n));
    out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

// A varint's bytes (SeenCells' wall cells): 7 bits a byte, low first, the top bit "more".
std::size_t varintSize(std::uint32_t v)
{
    return v < 0x80u ? 1 : v < 0x4000u ? 2 : 3;
}

void putVarint(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    while (v >= 0x80u) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80u));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

/// {op, sub, u16 the rest's length, the rest}: every mode message's frame (mode_frame.hpp).
std::vector<std::uint8_t> framed(std::uint8_t sub, const std::vector<std::uint8_t>& rest)
{
    return nox::modes::framed(kOp, sub, rest);
}

// Reads a sub's rest; past its end `ok` goes false and every read after gives 0.
struct Reader {
    std::span<const std::uint8_t> d;
    std::size_t at = 0;
    bool ok = true;
    std::uint8_t u8()
    {
        if (at + 1 > d.size()) {
            ok = false;
            return 0;
        }
        return d[at++];
    }
    std::uint16_t u16()
    {
        const std::uint16_t lo = u8();
        return static_cast<std::uint16_t>(lo | (u8() << 8));
    }
    std::string text()
    {
        const std::size_t n = u8();
        if (!ok || at + n > d.size()) {
            ok = false;
            return {};
        }
        std::string s(d.begin() + static_cast<std::ptrdiff_t>(at), d.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
        return s;
    }
};

} // namespace

std::vector<std::uint8_t> helloMessage(const Hello& h)
{
    std::vector<std::uint8_t> r{h.campaignClass, h.maxPlayers,
                                static_cast<std::uint8_t>((h.youAreHost ? 1u : 0u) | (h.characterKept ? 2u : 0u))};
    put16(r, h.hostNetCode);
    return framed(kHello, r);
}

std::vector<std::uint8_t> noticeMessage(Notice kind, std::string_view text)
{
    std::vector<std::uint8_t> r{static_cast<std::uint8_t>(kind)};
    putText(r, text, kTextMost);
    return framed(kNotice, r);
}

std::vector<std::uint8_t> rosterMessage(const Roster& roster)
{
    std::vector<std::uint8_t> r;
    const std::size_t n = std::min<std::size_t>(roster.members.size(), 16);
    r.push_back(static_cast<std::uint8_t>(n));
    for (std::size_t i = 0; i < n; ++i) {
        const Roster::Member& m = roster.members[i];
        put16(r, m.netCode);
        r.push_back(m.playerClass);
        r.push_back(m.level);
        r.push_back(m.flags);
        put16(r, m.respawnIn);
        putText(r, m.name, kNameMost);
    }
    return framed(kRoster, r);
}

std::vector<std::uint8_t> cineMessage(const Cine& c)
{
    std::vector<std::uint8_t> r{static_cast<std::uint8_t>(c.call), c.a};
    put16(r, c.b);
    put16(r, c.actor);
    putText(r, c.text, kTextMost);
    return framed(kCine, r);
}

std::vector<std::uint8_t> chapterMessage(const Chapter& c)
{
    return framed(kChapter, {c.chapter, static_cast<std::uint8_t>(c.startup ? 1 : 0)});
}

std::vector<std::uint8_t> readyStateMessage(std::uint8_t reading, std::uint8_t of)
{
    return framed(kReadyState, {reading, of});
}

std::vector<std::uint8_t> dialogMessage(const DialogMirror& d)
{
    std::vector<std::uint8_t> r{static_cast<std::uint8_t>(d.open ? 1 : 0)};
    put16(r, d.talker);
    put16(r, d.npc);
    putText(r, d.text, kTextMost);
    return framed(kDialog, r);
}

std::vector<std::uint8_t> creaturesMessage(std::span<const std::uint16_t> netCodes)
{
    std::vector<std::uint8_t> r;
    const std::size_t n = std::min(netCodes.size(), kCreaturesMost);
    r.push_back(static_cast<std::uint8_t>(n));
    for (std::size_t i = 0; i < n; ++i) {
        put16(r, netCodes[i]);
    }
    return framed(kCreatures, r);
}

std::optional<std::vector<std::uint16_t>> parseCreatures(std::span<const std::uint8_t> rest)
{
    Reader in{rest};
    const std::size_t n = in.u8();
    if (n > kCreaturesMost) {
        return std::nullopt;
    }
    std::vector<std::uint16_t> codes;
    codes.reserve(n);
    for (std::size_t i = 0; i < n && in.ok; ++i) {
        codes.push_back(in.u16());
    }
    if (!in.ok) {
        return std::nullopt;
    }
    std::ranges::sort(codes);
    return codes;
}

std::vector<std::uint8_t> chapterReadyMessage()
{
    return framed(kChapterReady, {});
}

std::vector<std::uint8_t> hostCmdMessage(HostCmd cmd)
{
    return framed(kHostCmd, {static_cast<std::uint8_t>(cmd)});
}

std::vector<std::vector<std::uint8_t>> characterMessages(std::span<const std::uint8_t> plr)
{
    std::vector<std::vector<std::uint8_t>> out;
    if (plr.empty() || plr.size() > kCharacterMost) {
        return out;
    }
    for (std::size_t at = 0; at < plr.size(); at += kCharacterPart) {
        const std::size_t n = std::min(kCharacterPart, plr.size() - at);
        std::vector<std::uint8_t> r;
        put16(r, static_cast<std::uint16_t>(plr.size()));
        put16(r, static_cast<std::uint16_t>(at));
        r.insert(r.end(), plr.begin() + static_cast<std::ptrdiff_t>(at), plr.begin() + static_cast<std::ptrdiff_t>(at + n));
        out.push_back(framed(kCharacter, r));
    }
    return out;
}

std::optional<CharacterPart> parseCharacter(std::span<const std::uint8_t> rest)
{
    Reader in{rest};
    CharacterPart p;
    p.size = in.u16();
    p.at = in.u16();
    if (!in.ok || p.at >= p.size || rest.size() - 4 > static_cast<std::size_t>(p.size - p.at)) {
        return std::nullopt;
    }
    p.bytes.assign(rest.begin() + 4, rest.end());
    return p;
}

std::vector<std::vector<std::uint8_t>> seenParts(std::string_view map, std::span<const std::uint16_t> walls,
                                                 std::span<const std::uint16_t> obelisks)
{
    std::vector<std::uint16_t> cells(walls.begin(), walls.end());
    std::ranges::sort(cells);
    cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
    std::vector<std::uint16_t> marks;
    for (const std::uint16_t e : obelisks) {
        if (e <= 0x7FFFu) {
            marks.push_back(e);
        }
    }
    std::ranges::sort(marks);
    marks.erase(std::unique(marks.begin(), marks.end()), marks.end());
    std::vector<std::vector<std::uint8_t>> out;
    std::size_t w = 0;
    std::size_t o = 0;
    while (w < cells.size() || o < marks.size()) {
        std::vector<std::uint8_t> r;
        putText(r, map, kNameMost);
        const std::size_t countAt = r.size();
        put16(r, 0);
        // As many cells as kSeenPart's bytes take, each from the last (-1 at the part's start).
        std::int32_t last = -1;
        std::size_t bytes = 0;
        std::uint16_t n = 0;
        for (; w < cells.size(); ++w) {
            const auto v = static_cast<std::uint32_t>(cells[w] - last - 1);
            if (bytes + varintSize(v) > kSeenPart) {
                break;
            }
            putVarint(r, v);
            bytes += varintSize(v);
            last = cells[w];
            ++n;
        }
        r[countAt] = static_cast<std::uint8_t>(n);
        r[countAt + 1] = static_cast<std::uint8_t>(n >> 8);
        const std::size_t m = std::min(kSeenPartObelisks, marks.size() - o);
        r.push_back(static_cast<std::uint8_t>(m));
        for (std::size_t i = 0; i < m; ++i) {
            put16(r, marks[o++]);
        }
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<std::vector<std::uint8_t>> seenMessages(std::uint8_t sub, std::string_view map, std::span<const std::uint16_t> walls,
                                                    std::span<const std::uint16_t> obelisks)
{
    std::vector<std::vector<std::uint8_t>> out = seenParts(map, walls, obelisks);
    for (auto& part : out) {
        part = framed(sub, part);
    }
    return out;
}

std::optional<SeenCells> parseSeen(std::span<const std::uint8_t> rest)
{
    Reader in{rest};
    SeenCells s;
    s.map = in.text();
    const std::size_t n = in.u16();
    if (!in.ok || s.map.empty() || s.map.size() > kNameMost || n > kSeenPart) {
        return std::nullopt;
    }
    s.walls.reserve(n);
    std::int32_t last = -1;
    const std::size_t from = in.at;
    for (std::size_t i = 0; i < n; ++i) {
        std::uint32_t v = 0;
        std::uint8_t b = 0x80;
        for (int shift = 0; (b & 0x80u) && in.ok; shift += 7) {
            if (shift > 14) {
                return std::nullopt; // (past three bytes: past the grid)
            }
            b = in.u8();
            v |= static_cast<std::uint32_t>(b & 0x7Fu) << shift;
        }
        const std::int64_t cell = static_cast<std::int64_t>(last) + 1 + v;
        if (!in.ok || cell > 0xFFFF) {
            return std::nullopt;
        }
        last = static_cast<std::int32_t>(cell);
        s.walls.push_back(static_cast<std::uint16_t>(cell));
    }
    if (in.at - from > kSeenPart) {
        return std::nullopt;
    }
    const std::size_t m = in.u8();
    if (!in.ok || m > kSeenPartObelisks) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < m; ++i) {
        const std::uint16_t e = in.u16();
        if (e > 0x7FFFu) {
            return std::nullopt;
        }
        s.obelisks.push_back(e);
    }
    if (!in.ok || in.at != rest.size()) {
        return std::nullopt;
    }
    return s;
}

std::optional<Cine> parseCine(std::span<const std::uint8_t> rest)
{
    Reader in{rest};
    Cine c;
    const std::uint8_t call = in.u8();
    c.a = in.u8();
    c.b = in.u16();
    c.actor = in.u16();
    c.text = in.text();
    if (!in.ok || call < static_cast<std::uint8_t>(CineCall::Blind) || call > static_cast<std::uint8_t>(CineCall::StopAllFades)) {
        return std::nullopt;
    }
    c.call = static_cast<CineCall>(call);
    return c;
}

std::optional<Chapter> parseChapter(std::span<const std::uint8_t> rest)
{
    Reader in{rest};
    Chapter c;
    c.chapter = in.u8();
    c.startup = in.u8() != 0;
    if (!in.ok) {
        return std::nullopt;
    }
    return c;
}

std::optional<std::pair<std::uint8_t, std::uint8_t>> parseReadyState(std::span<const std::uint8_t> rest)
{
    Reader in{rest};
    const std::uint8_t reading = in.u8();
    const std::uint8_t of = in.u8();
    if (!in.ok) {
        return std::nullopt;
    }
    return std::pair{reading, of};
}

std::optional<DialogMirror> parseDialog(std::span<const std::uint8_t> rest)
{
    Reader in{rest};
    DialogMirror d;
    d.open = in.u8() != 0;
    d.talker = in.u16();
    d.npc = in.u16();
    d.text = in.text();
    if (!in.ok) {
        return std::nullopt;
    }
    return d;
}

std::optional<HostCmd> parseHostCmd(std::span<const std::uint8_t> rest)
{
    Reader in{rest};
    const std::uint8_t cmd = in.u8();
    if (!in.ok || cmd != static_cast<std::uint8_t>(HostCmd::SkipBriefing)) {
        return std::nullopt;
    }
    return static_cast<HostCmd>(cmd);
}

std::optional<std::pair<std::uint8_t, std::span<const std::uint8_t>>> parse(std::span<const std::uint8_t> m)
{
    return nox::modes::unframed(kOp, m);
}

std::optional<Hello> parseHello(std::span<const std::uint8_t> rest)
{
    Reader in{rest};
    Hello h;
    h.campaignClass = in.u8();
    h.maxPlayers = in.u8();
    const std::uint8_t flags = in.u8();
    h.youAreHost = (flags & 1u) != 0;
    h.characterKept = (flags & 2u) != 0;
    h.hostNetCode = in.u16();
    if (!in.ok || h.campaignClass > 2) {
        return std::nullopt;
    }
    return h;
}

std::optional<std::pair<Notice, std::string>> parseNotice(std::span<const std::uint8_t> rest)
{
    Reader in{rest};
    const std::uint8_t kind = in.u8();
    std::string text = in.text();
    if (!in.ok || kind < static_cast<std::uint8_t>(Notice::Rules) || kind > static_cast<std::uint8_t>(Notice::Info)) {
        return std::nullopt;
    }
    return std::pair{static_cast<Notice>(kind), std::move(text)};
}

std::optional<Roster> parseRoster(std::span<const std::uint8_t> rest)
{
    Reader in{rest};
    Roster r;
    const std::size_t n = in.u8();
    for (std::size_t i = 0; i < n && in.ok; ++i) {
        Roster::Member m;
        m.netCode = in.u16();
        m.playerClass = in.u8();
        m.level = in.u8();
        m.flags = in.u8();
        m.respawnIn = in.u16();
        m.name = in.text();
        r.members.push_back(std::move(m));
    }
    if (!in.ok) {
        return std::nullopt;
    }
    return r;
}

} // namespace nox::coop::coopmsg
