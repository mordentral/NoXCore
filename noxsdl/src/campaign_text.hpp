// (noxcpp) The words that name a mod's campaign on the save screens (campaign_line.hpp draws them):
// no SDL here, so they're testable alone. The game's own campaign, and every other game: as the
// game's, unchanged.
#pragma once

#include <cstdint>
#include <string>

namespace noxsdl {

/// A saved game's line on those screens ("<name> the <class> - <place>") with its campaign's
/// title after it; as it is for an empty `title`.
inline std::string campaignSaveLine(std::string line, const std::string& title)
{
    if (!title.empty()) {
        line += " - " + title;
    }
    return line;
}

/// Where a saved game is, as its line says it: the stage's label (`stage`, the game's own's) for the
/// game's own campaign (`title` empty), exactly as always; for a mod's campaign the map it was saved
/// on (`mapName`: the stock stages are the game's chapters, never a mod's maps), its label when the
/// save names none.
inline std::string campaignSavePlace(const std::string& stage, const std::string& mapName, const std::string& title)
{
    return title.empty() || mapName.empty() ? stage : mapName;
}

/// The class screen's shade over a portrait (black, this alpha; 0: none): the one picked bright,
/// the others half dark, as the game's screen draws them; a class the campaign doesn't offer (a
/// mod's campaign's, `offered` false: it can't be picked) darker still, never lit.
inline std::uint8_t classPortraitShade(bool picked, bool offered)
{
    return !offered ? 208 : picked ? 0 : 128;
}

} // namespace noxsdl
