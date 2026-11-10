// The book's entries and its way through them (guiSpellSortList, bookDrawList, the arrows), apart
// from its drawing: what it lists, what it keeps unlisted, and where each arrow turns it.
//
//   unlisted   a caster's general Summon Creature, Mark Location and Teleport to Marker (flags
//              0x1000, 0x4000 or 0x10000) are in the book when known, but after the rest and left
//              off its list: the list, its page count, a click on it and the arrows go only as far
//              as the last listed entry. An award still finds one and opens the book on its page;
//              there the forward arrow is hidden and the back arrow goes to the list's first page.
//   order      the entries are found in id order, then all but the unlisted ones (the last
//              entries: those spells have the highest ids) are sorted by title.
#pragma once

#include "noxcore/things/spell_ids.hpp"
#include "noxcore/things/thing_db.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace noxsdl::bookpages {

/// The spells kept in the book but off its list.
constexpr std::uint32_t kUnlisted = 0x15000;
/// The numbered summons (the field guide's), never in the spell side.
constexpr std::uint32_t kGuideSummon = 0x2000;
/// The trap glyph, kept on the trap bar.
constexpr std::uint32_t kGlyph = 0x22;

/// A caster's spells for the book: each one of `classBit` (or everyone's 0x1000000) known in
/// `levels` (`all`: a network game's book, every one), but the trap glyph and the numbered summons,
/// appended to `entries` by id. Returns how many of them are unlisted.
inline std::size_t addSpells(const nox::things::ThingDb& things, const std::map<std::uint8_t, std::uint8_t>& levels,
                             bool all, std::uint32_t classBit, std::vector<std::uint32_t>& entries)
{
    std::size_t unlisted = 0;
    for (std::uint32_t id = 1; id < nox::things::kSpellCount; ++id) {
        const auto* s = things.findSpell(id);
        if (!s || id == kGlyph || !(s->flags & (0x1000000 | classBit))) {
            continue;
        }
        const auto it = levels.find(static_cast<std::uint8_t>(id));
        if (!all && (it == levels.end() || it->second == 0)) {
            continue;
        }
        if (s->flags & kUnlisted) {
            ++unlisted;
        }
        if (!(s->flags & kGuideSummon)) {
            entries.push_back(id);
        }
    }
    return unlisted;
}

/// Sorts all but the last `unlisted` entries by `less` (the titles), leaving those after them as
/// found. Returns how many are listed.
template <class Less>
int order(std::vector<std::uint32_t>& entries, std::size_t unlisted, Less less)
{
    const std::size_t listed = entries.size() - std::min(unlisted, entries.size());
    std::stable_sort(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(listed), less);
    return static_cast<int>(listed);
}

/// The index of `entry` among all the entries, listed or not (where an award turns the book), or
/// nullopt if it isn't in the book.
inline std::optional<int> indexOf(const std::vector<std::uint32_t>& entries, std::uint32_t entry)
{
    const auto it = std::find(entries.begin(), entries.end(), entry);
    if (it == entries.end()) {
        return std::nullopt;
    }
    return static_cast<int>(it - entries.begin());
}

/// The list's pages for `listed` entries, `perPage` to a page (one more when the last is exactly full).
inline int pageCount(int listed, int perPage)
{
    return listed / perPage + 1;
}

/// Where the book is turned to: a page of the list, or an entry's page.
struct Place {
    bool listMode = true;
    int listPage = 0;
    int entry = 0;

    bool operator==(const Place&) const = default;
};

/// The back arrow shows unless on the list's first page.
inline bool backShown(const Place& p)
{
    return !(p.listMode && p.listPage == 0);
}

/// The forward arrow shows on the list, or on an entry's page before the last listed one.
inline bool forwardShown(const Place& p, int listed)
{
    return p.listMode || p.entry < listed - 1;
}

/// The back arrow (when shown): a list page back; an entry back, or from the first entry to the
/// list's last page; from an unlisted entry's page to the list's first page.
inline Place back(Place p, int listed, int pages)
{
    if (p.listMode) {
        --p.listPage;
    } else if (p.entry < listed) {
        if (p.entry > 0) {
            --p.entry;
        } else {
            p.listMode = true;
            p.listPage = pages - 1;
        }
    } else {
        p.listMode = true;
        p.listPage = 0;
    }
    return p;
}

/// The forward arrow (only when shown): an entry on, a list page on, or from the list's last page to
/// the first entry.
inline Place forward(Place p, int pages)
{
    if (!p.listMode) {
        ++p.entry;
    } else if (p.listPage + 1 >= pages) {
        p.listMode = false;
        p.entry = 0;
    } else {
        ++p.listPage;
    }
    return p;
}

} // namespace noxsdl::bookpages
