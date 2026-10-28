// An item's name as its tooltip shows it (ToolTip.c, Gui_Tooltip_GetDrawableText):
//   weapons, armour, staffs and flags: the definition's DESC with its modifiers' words round it,
//              in English "[effectiveness] [material] Longsword [primary enchantment]
//              [secondary enchantment's SECONDARYDESC]" (staffs take no modifier words); a type
//              modifier.bin doesn't define says NoArmsInfo;
//   info books: what they hold, asked of the server the first time (MSG_INFO_BOOK_DATA: 1 spell
//              book, 2 lore scroll, 4 ability book): "Book of Fireball", "Bat Lore Scroll",
//              "Book of Harpoon"; empty until the answer comes;
//   the rest:  the thing's PRETTYNAME.
#pragma once

#include "thing_art.hpp"

#include "noxcore/client/world.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/thing_db.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <string>

namespace noxsdl {

/// A thing's display name (its pretty_name, Cli_GetThingPrettyName): its PRETTYNAME
/// string, else for a thing with a WEIGHT (the loader's) "thing.db:<name>PrettyName";
/// empty for the rest (monsters among them), as the exe's is null.
std::string prettyName(const nox::things::ThingDef& t, const nox::strings::StringTable* strings);

class ItemNames {
public:
    ItemNames(const nox::things::ThingDb& things, ThingArt& art, const nox::things::ModifierDb& modifiers,
              const nox::strings::StringTable* strings)
        : things_(things), art_(art), modifiers_(modifiers), strings_(strings)
    {
    }

    /// The tooltip for item `netCode` of type `typeId` with `mods` (0xFF none). `ask` sends the
    /// book question (HeadlessClient::askBook) when a book's contents aren't known yet.
    std::string name(std::uint16_t netCode, std::uint16_t typeId, const std::array<std::uint8_t, 4>& mods,
                     const nox::client::World& world, const std::function<void(std::uint16_t, std::uint8_t)>& ask);

private:
    std::string text(const std::string& id, const char* file = nullptr) const;

    const nox::things::ThingDb& things_;
    ThingArt& art_;
    const nox::things::ModifierDb& modifiers_;
    const nox::strings::StringTable* strings_;
    std::set<std::uint16_t> asked_; ///< books asked about (the exe's pending marker)
};

} // namespace noxsdl
