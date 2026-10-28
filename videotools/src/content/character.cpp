// Characters (.plr) in the Content Editor (character.hpp).
#include "content/character.hpp"

#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/io/file.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace videotools::content {

using nox::player::SectionId;

// (As the colour picker has them: noxsdl's SelColor screen.)
const std::array<PlrRgb, 32> kCreatorHairColors{{
    {0xf3, 0xb7, 0x9f}, {0xc7, 0x84, 0x3a}, {0xff, 0xaa, 0x56}, {0x92, 0x79, 0x6a}, {0xff, 0xfb, 0xba},
    {0xf7, 0x44, 0x44}, {0x3f, 0xbb, 0xef}, {0xc4, 0xc2, 0xb1}, {0xfd, 0xf6, 0x66}, {0x73, 0x4d, 0x22},
    {0xe5, 0x79, 0x19}, {0x52, 0x44, 0x3c}, {0xeb, 0xe5, 0x89}, {0x93, 0x00, 0x00}, {0x00, 0x25, 0x44},
    {0xd6, 0xd6, 0xd6}, {0x95, 0x6c, 0x2b}, {0x34, 0x19, 0x00}, {0x96, 0x37, 0x00}, {0x09, 0x00, 0x0c},
    {0x96, 0x92, 0x51}, {0x83, 0x68, 0xb3}, {0x79, 0xa5, 0x42}, {0x75, 0x75, 0x75}, {0x64, 0x82, 0x7d},
    {0x38, 0x30, 0x28}, {0x6c, 0x51, 0x3a}, {0x34, 0x28, 0x27}, {0x8f, 0x8e, 0x7c}, {0x38, 0x1d, 0x68},
    {0x0f, 0x58, 0x00}, {0x57, 0x35, 0x35},
}};
const std::array<PlrRgb, 32> kCreatorSkinColors{{
    {0xe9, 0xde, 0xcd}, {0xde, 0xb6, 0xa8}, {0xda, 0x9a, 0x6e}, {0xec, 0xca, 0xb2}, {0x9a, 0x83, 0x69},
    {0x75, 0x5f, 0x4a}, {0xef, 0xe9, 0xc1}, {0xe4, 0xe4, 0xc1}, {0xcb, 0xb9, 0x9c}, {0xc8, 0xa4, 0x6e},
    {0xa9, 0x83, 0x4f}, {0xf3, 0x9e, 0x77}, {0x73, 0x4d, 0x22}, {0x5b, 0x37, 0x14}, {0xf9, 0xcd, 0x8a},
    {0xf0, 0xeb, 0xab}, {0x86, 0x8f, 0x9a}, {0x87, 0x66, 0x43}, {0x73, 0x50, 0x2e}, {0xa5, 0x5d, 0x46},
    {0x63, 0x41, 0x25}, {0x53, 0x34, 0x1d}, {0xa8, 0x7f, 0x54}, {0xab, 0xa0, 0x60}, {0x8d, 0x88, 0x76},
    {0x7f, 0x74, 0x52}, {0x60, 0x54, 0x33}, {0xa2, 0x81, 0x64}, {0x4e, 0x40, 0x2e}, {0x41, 0x32, 0x2d},
    {0xab, 0x9e, 0x74}, {0xaf, 0x74, 0x83},
}};

Character Character::parse(std::span<const std::uint8_t> plain)
{
    Character c;
    c.file_ = nox::player::parsePlayerFile(plain);
    const auto* info = c.file_.find(SectionId::FileInfo);
    if (!info) {
        throw std::runtime_error("no File Info section");
    }
    c.info_ = nox::player::FileInfo::parse(info->data);
    if (const auto* a = c.file_.find(SectionId::Attrib)) {
        c.attrib_ = nox::player::Attrib::parse(a->data);
    }
    return c;
}

Character Character::load(const fs::path& file)
{
    auto data = nox::readFileBytes(file);
    if (data.size() % 8 != 0) {
        throw std::runtime_error("its size isn't a whole number of cipher blocks");
    }
    nox::crypto::cipherForKey(nox::crypto::FileKey::PlayerFile).decrypt(data);
    Character c = parse(data);
    c.path_ = file;
    return c;
}

bool Character::setName(std::u16string name)
{
    if (name.empty() || name.size() > 24) {
        return false;
    }
    if (name != info_.name) {
        info_.name = name;
        if (attrib_) {
            attrib_->name = name;
        }
        changed_ = true;
    }
    return true;
}

void Character::setColour(BodyPart part, PlrRgb colour)
{
    auto& colours = info_.colors;
    const PlrRgb skin = colours[std::size_t(BodyPart::Skin)];
    if (part == BodyPart::Skin) {
        for (auto& c : colours) {
            if (c == skin) {
                c = colour; // (the skin, and what's off with it)
            }
        }
    } else {
        colours[std::size_t(part)] = colour;
    }
    if (attrib_) {
        attrib_->colors = colours;
    }
    changed_ = true;
}

bool Character::setClothing(Garment g, std::uint8_t userColor)
{
    if (userColor >= kUserColors) {
        return false;
    }
    info_.clothing[std::size_t(g)] = userColor;
    if (attrib_) {
        attrib_->clothing = info_.clothing;
    }
    changed_ = true;
    return true;
}

std::vector<std::uint8_t> Character::plain() const
{
    nox::player::PlayerFile out = file_;
    for (auto& s : out.sections) {
        if (s.id == std::uint32_t(SectionId::FileInfo)) {
            s.data = info_.serialize();
        } else if (s.id == std::uint32_t(SectionId::Attrib) && attrib_) {
            s.data = attrib_->serialize();
        }
    }
    return nox::player::serializePlayerFile(out);
}

void Character::save(const fs::path& file)
{
    auto data = plain();
    nox::crypto::cipherForKey(nox::crypto::FileKey::PlayerFile).encrypt(data);
    nox::writeFileBytes(file, data);
    path_ = file;
    changed_ = false;
}

PlrRgb userColor(const nox::things::ModifierDb* modifiers, int index)
{
    const auto* m = modifiers ? modifiers->findModifier(std::format("UserColor{}", index + 1)) : nullptr;
    return m ? m->color : PlrRgb{0, 0, 0};
}

std::vector<DollLayer> dollLayers(const Character& c, const nox::things::ModifierDb* modifiers)
{
    const PlrRgb white{255, 255, 255};
    const PlrRgb skin = c.colour(BodyPart::Skin);
    const bool female = c.female();
    std::vector<DollLayer> layers;
    DollLayer body;
    body.materials.fill(white);
    body.materials[1] = c.colour(BodyPart::Hair);
    body.materials[2] = c.colour(BodyPart::Goatee); // (off: the skin's colour already)
    body.materials[3] = skin;
    body.materials[4] = c.colour(BodyPart::Beard);
    body.materials[5] = c.colour(BodyPart::Mustache);
    body.materials[6] = skin;
    const bool bald = !c.hasPart(BodyPart::Hair);
    body.image = female ? (bald ? "FemaleBaldBase" : "FemaleBase") : (bald ? "MaleBaldBase" : "MaleBase");
    layers.push_back(body);

    struct Item {
        const char* image;
        const char* armor;
        /// Modifier position -> the clothes colour on it (a position past the four: none).
        std::array<std::pair<std::size_t, Garment>, 2> slots;
    };
    // In the armour pictures' order: sneakers, pants, shirt.
    const Item items[] = {
        {"StreetSneakers", "StreetSneakers", {{{1, Garment::Shoes1}, {0, Garment::Shoes2}}}},
        {"StreetPants", "StreetPants", {{{1, Garment::Pants}, {9, Garment::Pants}}}},
        {"StreetShirt", "StreetShirt", {{{1, Garment::Shirt1}, {2, Garment::Shirt2}}}},
    };
    for (const auto& item : items) {
        DollLayer l;
        l.image = std::string(female ? "Female" : "Male") + item.image;
        l.materials.fill(white);
        if (const auto* def = modifiers ? modifiers->findArmor(item.armor) : nullptr) {
            for (std::size_t slot = 1; slot <= 6; ++slot) {
                l.materials[slot] = def->colors[slot - 1];
            }
            for (const auto& [position, garment] : item.slots) {
                if (position < def->modifierSlots.size() && def->modifierSlots[position] != 0) {
                    l.materials[def->modifierSlots[position]] = userColor(modifiers, c.clothing(garment));
                }
            }
        }
        layers.push_back(std::move(l));
    }
    return layers;
}

void shadeMaterials(std::vector<std::uint8_t>& rgba, const std::vector<std::uint8_t>& material, const std::array<PlrRgb, 7>& colours)
{
    const std::size_t n = std::min(material.size(), rgba.size() / 4);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t m = material[i];
        if (m == 0) {
            continue;
        }
        const PlrRgb& c = colours[std::min<std::size_t>(m - 1u, colours.size() - 1)];
        for (std::size_t ch = 0; ch < 3; ++ch) {
            auto& px = rgba[i * 4 + ch];
            px = static_cast<std::uint8_t>((px * c[ch]) >> 8);
        }
    }
}

std::string checkSaveOutsideGame(const fs::path& target, const fs::path& source, const fs::path& gameDir)
{
    if (target.empty() || !target.has_filename()) {
        return "no file name given";
    }
    std::error_code ec;
    if (!source.empty() && fs::exists(target, ec) && fs::equivalent(target, source, ec)) {
        return "that's the file it was read from: save a new one";
    }
    // Not in a game install, nor anywhere under one (its Save folder included).
    for (fs::path dir = fs::weakly_canonical(fs::absolute(target, ec).parent_path(), ec); !dir.empty(); dir = dir.parent_path()) {
        if (!gameDir.empty() && fs::exists(gameDir, ec) && fs::equivalent(dir, gameDir, ec)) {
            return "that's in the game's folder: save somewhere else (a copy of the game, say)";
        }
        if (fs::exists(nox::findNoCase(dir, "GAME.EXE"), ec)) {
            const auto u8 = dir.u8string();
            return std::string(u8.begin(), u8.end()) + " holds GAME.EXE (a game install): save somewhere else";
        }
        if (dir == dir.parent_path()) {
            break;
        }
    }
    return {};
}

} // namespace videotools::content
