// The words some cursors carry in yellow (cursor.hpp): GRAB, PICKUP, SHOPKEEPER, TALK, IDENTIFY,
// REPAIR and USE, looked up as Str_GetString does with guicurs.c as the source (the label
// "guicurs.c:<id>"), so the csf's text in its own language: "PICK UP" in the English one.
// (noxcpp) A csf without the label (or none at all) gives NoX Core's own words in the player's
// language (NOX_TRK, the English the bare id) where the exe would show its missing-string text.
// Resolved once and again only when the language changes (nox::l10n::languageRevision), never
// per frame. No SDL here, so they're testable alone.
#pragma once

#include "noxcore/strings/l10n.hpp"
#include "noxcore/strings/string_table.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace noxsdl {

enum class CursorLabel : std::uint8_t { Grab, Pickup, Shopkeeper, Talk, Identify, Repair, Use };
inline constexpr std::size_t kCursorLabels = 7;

/// Each label's csf id and NoX Core's words for it, by CursorLabel.
struct CursorLabelDef {
    const char* id;
    nox::l10n::Key fallback;
};
inline constexpr std::array<CursorLabelDef, kCursorLabels> kCursorLabelDefs{{
    {"GRAB", NOX_TRK("game.cursor.grab", "GRAB")},
    {"PICKUP", NOX_TRK("game.cursor.pickup", "PICKUP")},
    {"SHOPKEEPER", NOX_TRK("game.cursor.shopkeeper", "SHOPKEEPER")},
    {"TALK", NOX_TRK("game.cursor.talk", "TALK")},
    {"IDENTIFY", NOX_TRK("game.cursor.identify", "IDENTIFY")},
    {"REPAIR", NOX_TRK("game.cursor.repair", "REPAIR")},
    {"USE", NOX_TRK("game.cursor.use", "USE")},
}};

/// A label's words: the csf's "guicurs.c:<id>", else NoX Core's in the language in use. A lookup:
/// resolve once (CursorLabels), not per frame.
inline std::string cursorLabel(const nox::strings::StringTable* csf, CursorLabel which)
{
    const CursorLabelDef& def = kCursorLabelDefs[static_cast<std::size_t>(which)];
    if (csf) {
        if (const auto t = csf->text(std::string("guicurs.c:") + def.id)) {
            return nox::strings::toUtf8(*t);
        }
    }
    return nox::l10n::tr(def.fallback);
}

/// The labels, resolved on first use and when the language has changed since.
class CursorLabels {
public:
    explicit CursorLabels(const nox::strings::StringTable* csf) : csf_(csf) {}

    const std::string& get(CursorLabel which)
    {
        if (const std::uint64_t now = nox::l10n::languageRevision(); !resolved_ || now != revision_) {
            for (std::size_t i = 0; i < kCursorLabels; ++i) {
                text_[i] = cursorLabel(csf_, static_cast<CursorLabel>(i));
            }
            revision_ = now;
            resolved_ = true;
        }
        return text_[static_cast<std::size_t>(which)];
    }

private:
    const nox::strings::StringTable* csf_;
    std::array<std::string, kCursorLabels> text_;
    std::uint64_t revision_ = 0;
    bool resolved_ = false;
};

} // namespace noxsdl
