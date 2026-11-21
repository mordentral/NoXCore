// Where a saved character was last played (character_notes.hpp).
#include "noxcore/client/character_notes.hpp"

#include "noxcore/player/character_store.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <limits>

namespace nox::client::character_notes {

namespace {

/// A character the notes never keep: a control character, a line or paragraph separator, a
/// direction override or isolate.
bool unwanted(char32_t c)
{
    return c < 0x20 || c == 0x7F || (c >= 0x80 && c <= 0x9F) || c == 0x2028 || c == 0x2029 || (c >= 0x202A && c <= 0x202E) ||
           (c >= 0x2066 && c <= 0x2069) || c == 0xFEFF;
}

/// The next UTF-8 character of `s` from `i` (moved past it): none for a broken sequence (one
/// byte passed over), an overlong form, a surrogate or past U+10FFFF.
std::optional<char32_t> nextChar(std::string_view s, std::size_t& i)
{
    const auto b = static_cast<unsigned char>(s[i]);
    std::size_t n = 0;
    char32_t c = 0;
    if (b < 0x80) {
        ++i;
        return b;
    }
    if ((b & 0xE0) == 0xC0) {
        n = 1;
        c = b & 0x1F;
    } else if ((b & 0xF0) == 0xE0) {
        n = 2;
        c = b & 0x0F;
    } else if ((b & 0xF8) == 0xF0) {
        n = 3;
        c = b & 0x07;
    } else {
        ++i;
        return std::nullopt;
    }
    if (i + n >= s.size()) {
        ++i; // (cut short)
        return std::nullopt;
    }
    for (std::size_t k = 1; k <= n; ++k) {
        const auto t = static_cast<unsigned char>(s[i + k]);
        if ((t & 0xC0) != 0x80) {
            ++i;
            return std::nullopt;
        }
        c = (c << 6) | (t & 0x3F);
    }
    static constexpr char32_t kLeast[4] = {0, 0x80, 0x800, 0x10000};
    if (c < kLeast[n] || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        ++i;
        return std::nullopt;
    }
    i += n + 1;
    return c;
}

void appendUtf8(std::string& out, char32_t c)
{
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

/// The section's data read in order: each read false past its end (nothing read then).
struct Reader {
    std::span<const std::uint8_t> data;
    std::size_t at = 0;

    bool bytes(std::size_t n, std::span<const std::uint8_t>& out)
    {
        if (n > data.size() - at) {
            return false;
        }
        out = data.subspan(at, n);
        at += n;
        return true;
    }
    bool u8(std::uint8_t& v)
    {
        std::span<const std::uint8_t> b;
        if (!bytes(1, b)) {
            return false;
        }
        v = b[0];
        return true;
    }
    bool little(std::size_t n, std::uint64_t& v)
    {
        std::span<const std::uint8_t> b;
        if (!bytes(n, b)) {
            return false;
        }
        v = 0;
        for (std::size_t i = 0; i < n; ++i) {
            v |= static_cast<std::uint64_t>(b[i]) << (8 * i);
        }
        return true;
    }
    /// A u8 length and that many bytes, refused past `most`; cleaned.
    bool text(std::size_t most, std::string& out)
    {
        std::uint8_t n = 0;
        std::span<const std::uint8_t> b;
        if (!u8(n) || n > most || !bytes(n, b)) {
            return false;
        }
        out = cleanText(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()), most);
        return true;
    }
};

void putLittle(std::vector<std::uint8_t>& out, std::uint64_t v, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

void putText(std::vector<std::uint8_t>& out, std::string_view text, std::size_t most)
{
    const std::string clean = cleanText(text, most);
    out.push_back(static_cast<std::uint8_t>(clean.size()));
    out.insert(out.end(), clean.begin(), clean.end());
}

} // namespace

std::string cleanText(std::string_view text, std::size_t maxBytes)
{
    std::string out;
    bool space = false;
    for (std::size_t i = 0; i < text.size();) {
        const std::optional<char32_t> c = nextChar(text, i);
        if (!c) {
            continue;
        }
        if (*c == ' ' || *c == '\t' || *c == 0xA0 || *c == 0x3000) {
            space = !out.empty(); // (runs of spaces one; none at the start)
            continue;
        }
        if (unwanted(*c)) {
            continue;
        }
        std::string piece;
        if (space) {
            piece += ' ';
        }
        appendUtf8(piece, *c);
        if (out.size() + piece.size() > maxBytes) {
            break; // (a whole character, or nothing)
        }
        out += piece;
        space = false;
    }
    return out;
}

std::vector<std::uint8_t> encode(const Notes& notes)
{
    std::vector<std::uint8_t> out;
    const std::size_t n = std::min(notes.places.size(), kMaxPlaces);
    if (n == 0) {
        return out;
    }
    putLittle(out, kVersion, 2);
    out.push_back(static_cast<std::uint8_t>(n));
    for (std::size_t i = 0; i < n; ++i) {
        const Place& p = notes.places[i];
        out.push_back(p.kind == Place::Kind::Coop ? 1 : 0);
        putText(out, p.id, kMaxIdBytes);
        putText(out, p.title, kMaxTitleBytes);
        putLittle(out, static_cast<std::uint64_t>(std::max<std::int64_t>(p.last, 0)), 8);
        putText(out, p.note, kMaxNoteBytes);
    }
    return out;
}

std::optional<Notes> decode(std::span<const std::uint8_t> data)
{
    if (data.size() > player::character_store::kMaxLabelBytes) {
        return std::nullopt;
    }
    Reader r{data};
    std::uint64_t version = 0;
    std::uint8_t count = 0;
    if (!r.little(2, version) || version != kVersion || !r.u8(count) || count > kMaxPlaces) {
        return std::nullopt;
    }
    Notes notes;
    for (std::uint8_t i = 0; i < count; ++i) {
        Place p;
        std::uint8_t kind = 0;
        std::uint64_t last = 0;
        if (!r.u8(kind) || kind > 1 || !r.text(kMaxIdBytes, p.id) || !r.text(kMaxTitleBytes, p.title) || !r.little(8, last) ||
            !r.text(kMaxNoteBytes, p.note)) {
            return std::nullopt;
        }
        p.kind = kind == 1 ? Place::Kind::Coop : Place::Kind::Mode;
        p.last = static_cast<std::int64_t>(std::min<std::uint64_t>(last, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
        // (a mode with neither id nor title names nowhere: let go)
        if (p.kind == Place::Kind::Mode && p.id.empty() && p.title.empty()) {
            continue;
        }
        notes.places.push_back(std::move(p));
    }
    std::stable_sort(notes.places.begin(), notes.places.end(), [](const Place& a, const Place& b) { return a.last > b.last; });
    return notes;
}

bool matches(const Place& place, const Target& target)
{
    if (!target.any || place.kind != target.kind) {
        return false;
    }
    if (!place.id.empty() && !target.id.empty()) {
        return place.id == target.id;
    }
    // (the target's title as a note keeps it: a remote host's mode name is as sent, its spaces,
    // control characters and length not yet cleaned)
    return util::equalsNoCase(place.title, cleanText(target.title, kMaxTitleBytes));
}

void played(Notes& notes, Place place, std::int64_t now)
{
    place.last = now;
    const auto same = std::ranges::find_if(notes.places, [&](const Place& p) {
        if (p.kind != place.kind) {
            return false;
        }
        if (!p.id.empty() && !place.id.empty()) {
            return p.id == place.id;
        }
        return util::equalsNoCase(p.title, place.title);
    });
    if (same != notes.places.end()) {
        if (place.note.empty()) {
            place.note = same->note;
        }
        if (place.id.empty()) {
            place.id = same->id; // (a remote host's, named alone: the id known from before kept)
        }
        if (place.title.empty()) {
            place.title = same->title;
        }
        notes.places.erase(same);
    }
    notes.places.insert(notes.places.begin(), std::move(place));
    if (notes.places.size() > kMaxPlaces) {
        notes.places.resize(kMaxPlaces);
    }
}

std::vector<std::size_t> order(const std::vector<const Notes*>& of, const Target& target, bool onlyTarget)
{
    std::vector<std::size_t> out;
    out.reserve(of.size());
    if (!target.any) {
        for (std::size_t i = 0; i < of.size(); ++i) {
            out.push_back(i);
        }
        return out;
    }
    // (each one's last time at the target, or none)
    std::vector<std::pair<std::size_t, std::int64_t>> there;
    std::vector<std::size_t> rest;
    for (std::size_t i = 0; i < of.size(); ++i) {
        std::optional<std::int64_t> last;
        if (of[i]) {
            for (const Place& p : of[i]->places) {
                if (matches(p, target) && (!last || p.last > *last)) {
                    last = p.last;
                }
            }
        }
        if (last) {
            there.emplace_back(i, *last);
        } else {
            rest.push_back(i);
        }
    }
    std::stable_sort(there.begin(), there.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [i, last] : there) {
        out.push_back(i);
    }
    if (!onlyTarget) {
        out.insert(out.end(), rest.begin(), rest.end());
    }
    return out;
}

const Place* shownPlace(const Notes& notes, const Target& target)
{
    for (const Place& p : notes.places) {
        if (matches(p, target)) {
            return &p; // (newest first)
        }
    }
    return notes.places.empty() ? nullptr : &notes.places.front();
}

} // namespace nox::client::character_notes
