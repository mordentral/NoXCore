// Where a saved character was last played (character_notes.hpp).
#include "noxcore/client/character_notes.hpp"

#include "noxcore/util/text.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace nox::client::character_notes {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kFirstKey = "noxcore-character-notes";
constexpr int kVersion = 1;

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

std::string_view kindName(Place::Kind k)
{
    return k == Place::Kind::Coop ? "coop" : "mode";
}

} // namespace

fs::path fileOf(const fs::path& plr)
{
    fs::path p = plr;
    p += ".noxcore";
    return p;
}

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

std::string format(const Notes& notes)
{
    std::ostringstream out;
    out << kFirstKey << " = " << kVersion << "\n";
    out << "name = " << cleanText(notes.name, kMaxTitleBytes) << "\n";
    out << "class = " << std::clamp(notes.characterClass, 0, 2) << "\n";
    const std::size_t n = std::min(notes.places.size(), kMaxPlaces);
    for (std::size_t i = 0; i < n; ++i) {
        const Place& p = notes.places[i];
        out << "[place]\n";
        out << "kind = " << kindName(p.kind) << "\n";
        out << "id = " << cleanText(p.id, kMaxIdBytes) << "\n";
        out << "title = " << cleanText(p.title, kMaxTitleBytes) << "\n";
        out << "last = " << std::max<std::int64_t>(p.last, 0) << "\n";
        if (const std::string note = cleanText(p.note, kMaxNoteBytes); !note.empty()) {
            out << "note = " << note << "\n";
        }
    }
    return out.str();
}

std::optional<Notes> parse(std::string_view text)
{
    if (text.size() > kMaxFileBytes) {
        return std::nullopt;
    }
    Notes notes;
    bool first = true;
    Place* place = nullptr;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        std::string_view line = util::trim(text.substr(at, end - at));
        at = end + 1;
        if (first) {
            // (the first line names it: anything else isn't a note)
            const auto eq = line.find('=');
            if (eq == std::string_view::npos || util::trim(line.substr(0, eq)) != kFirstKey) {
                return std::nullopt;
            }
            first = false;
            continue;
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line == "[place]") {
            if (notes.places.size() >= kMaxPlaces) {
                place = nullptr; // (past the most: let go)
                continue;
            }
            notes.places.emplace_back();
            place = &notes.places.back();
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string_view key = util::trim(line.substr(0, eq));
        const std::string_view value = util::trim(line.substr(eq + 1));
        if (!place) {
            if (key == "name") {
                notes.name = cleanText(value, kMaxTitleBytes);
            } else if (key == "class") {
                int c = 0;
                std::from_chars(value.data(), value.data() + value.size(), c);
                notes.characterClass = std::clamp(c, 0, 2);
            }
            continue;
        }
        if (key == "kind") {
            place->kind = value == "coop" ? Place::Kind::Coop : Place::Kind::Mode;
        } else if (key == "id") {
            place->id = cleanText(value, kMaxIdBytes);
        } else if (key == "title") {
            place->title = cleanText(value, kMaxTitleBytes);
        } else if (key == "last") {
            std::int64_t t = 0;
            std::from_chars(value.data(), value.data() + value.size(), t);
            place->last = std::max<std::int64_t>(t, 0);
        } else if (key == "note") {
            place->note = cleanText(value, kMaxNoteBytes);
        }
    }
    if (first) {
        return std::nullopt;
    }
    // (a mode with neither id nor title names nowhere: let go)
    std::erase_if(notes.places, [](const Place& p) { return p.kind == Place::Kind::Mode && p.id.empty() && p.title.empty(); });
    std::stable_sort(notes.places.begin(), notes.places.end(), [](const Place& a, const Place& b) { return a.last > b.last; });
    return notes;
}

std::optional<Notes> read(const fs::path& plr, std::string_view name, int characterClass)
{
    const fs::path file = fileOf(plr);
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(file, ec);
    if (ec || size > kMaxFileBytes || !fs::is_regular_file(file, ec)) {
        return std::nullopt;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::optional<Notes> notes = parse(text);
    if (!notes || notes->name != cleanText(name, kMaxTitleBytes) || notes->characterClass != std::clamp(characterClass, 0, 2)) {
        return std::nullopt; // (another character's: one deleted that had this file name)
    }
    return notes;
}

bool write(const fs::path& plr, const Notes& notes)
{
    const fs::path file = fileOf(plr);
    fs::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        const std::string text = format(notes);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            out.close();
            std::error_code ec;
            fs::remove(temp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, file, ec);
    if (ec) {
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

void remove(const fs::path& plr)
{
    std::error_code ec;
    fs::remove(fileOf(plr), ec);
}

void move(const fs::path& from, const fs::path& to)
{
    std::error_code ec;
    const fs::path a = fileOf(from), b = fileOf(to);
    if (fs::exists(a, ec)) {
        fs::rename(a, b, ec);
    } else {
        fs::remove(b, ec);
    }
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
