// A character (.plr) as the Content Editor edits it: its name and colours, kept the same in the
// two sections that hold them (File Info, which the load menu shows, and Attrib, which the game
// reads), every other section written back as it was read. And the doll: the character as the
// game's character creator draws it, in layers to tint.
//
// What a .plr can hold: the body colours (hair, skin, mustache, goatee, beard) are any RGB
// colour; the clothes are numbers 0-31 picking modifier.bin's UserColor1-32 (the game adds the
// number to UserColor1's modifier id, so past 31 it would take some other enchantment).
#pragma once

#include "noxcore/player/player_file.hpp"
#include "noxcore/things/modifier.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace videotools::content {

using PlrRgb = nox::player::Rgb;

/// The colour picker's own tables (SelColor.wnd's hair and skin palettes); the clothes' table is
/// modifier.bin's UserColor1-32.
extern const std::array<PlrRgb, 32> kCreatorHairColors;
extern const std::array<PlrRgb, 32> kCreatorSkinColors;

enum class BodyPart { Hair = 0, Skin = 1, Mustache = 2, Goatee = 3, Beard = 4 };
enum class Garment { Pants = 0, Shirt1 = 1, Shirt2 = 2, Shoes1 = 3, Shoes2 = 4 };
inline constexpr int kUserColors = 32;

class Character {
public:
    /// Reads a .plr. Throws.
    static Character load(const std::filesystem::path& file);
    /// From decrypted .plr bytes (tests). Throws.
    static Character parse(std::span<const std::uint8_t> plain);

    const std::filesystem::path& path() const { return path_; }
    const nox::player::FileInfo& info() const { return info_; }
    const nox::player::Attrib* attrib() const { return attrib_ ? &*attrib_ : nullptr; }

    std::u16string name() const { return info_.name; }
    /// At most 24 characters (the game's limit); false (unchanged) for an empty or longer one.
    bool setName(std::u16string name);
    PlrRgb colour(BodyPart part) const { return info_.colors[std::size_t(part)]; }
    /// Any colour. The skin also takes the hair and facial hair that are off with it.
    void setColour(BodyPart part, PlrRgb colour);
    /// Hair, mustache, goatee or beard: off is the skin's colour (as the game saves it).
    bool hasPart(BodyPart part) const { return part == BodyPart::Skin || colour(part) != colour(BodyPart::Skin); }
    std::uint8_t clothing(Garment g) const { return info_.clothing[std::size_t(g)]; }
    /// 0-31 (UserColor1-32); false for another.
    bool setClothing(Garment g, std::uint8_t userColor);
    bool female() const { return info_.isFemale != 0; }
    bool changed() const { return changed_; }

    /// The decrypted bytes with the changes (every other section as read).
    std::vector<std::uint8_t> plain() const;
    /// Writes an encrypted .plr. Throws. (The caller checks where: never the game's folders.)
    void save(const std::filesystem::path& file);

private:
    std::filesystem::path path_;
    nox::player::PlayerFile file_;
    nox::player::FileInfo info_;
    std::optional<nox::player::Attrib> attrib_;
    bool changed_ = false;
};

/// One picture of the doll, by its thing.bin image name, and the colour of each material slot
/// (0-6) its grey pixels are multiplied by.
struct DollLayer {
    std::string image;
    std::array<PlrRgb, 7> materials{};
};

/// The doll as the character creator draws it (SelColor's preview): the body in the skin and hair
/// colours (bald when the hair is off), then the street sneakers, pants and shirt, each in its
/// armour definition's colours with the chosen clothes colours on the slots its modifiers take.
/// The creator always shows a man; a woman's .plr gets the Female pictures.
std::vector<DollLayer> dollLayers(const Character& c, const nox::things::ModifierDb* modifiers);

/// A UserColor's colour (black if modifier.bin hasn't it).
PlrRgb userColor(const nox::things::ModifierDb* modifiers, int index);

/// The game's own shading: each pixel's grey times its slot's colour (rgba in place).
void shadeMaterials(std::vector<std::uint8_t>& rgba, const std::vector<std::uint8_t>& material, const std::array<PlrRgb, 7>& colours);

/// Where an edited file (a character, a text file) may be saved: "" or why not (no folder of a game
/// install or anywhere under one, nor the file it was read from).
std::string checkSaveOutsideGame(const std::filesystem::path& target, const std::filesystem::path& source,
                                 const std::filesystem::path& gameDir);

} // namespace videotools::content
