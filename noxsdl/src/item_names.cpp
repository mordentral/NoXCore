// Item names as their tooltips (and identify) show them (item_names.hpp).
#include "item_names.hpp"

#include "noxcore/things/field_guide.hpp"
#include "noxcore/things/object_class.hpp"

namespace noxsdl {

std::string prettyName(const nox::things::ThingDef& t, const nox::strings::StringTable* strings)
{
    std::string id;
    if (const auto p = t.attribute("PRETTYNAME")) {
        id = std::string(*p);
    } else if (const auto w = t.attribute("WEIGHT"); w && std::atoi(std::string(*w).c_str()) != 0) {
        id = "thing.db:" + t.name + "PrettyName";
    }
    if (strings && !id.empty()) {
        if (const auto s = strings->text(id)) {
            return nox::strings::toUtf8(*s);
        }
    }
    return {};
}

std::string ItemNames::text(const std::string& id, const char* file) const
{
    if (strings_ && !id.empty()) {
        if (const auto t = strings_->text(id)) {
            return nox::strings::toUtf8(*t);
        }
        if (file) {
            if (const auto t = strings_->text(std::string(file) + ":" + id)) {
                return nox::strings::toUtf8(*t);
            }
        }
    }
    return {};
}

std::string ItemNames::name(std::uint16_t netCode, std::uint16_t typeId, const std::array<std::uint8_t, 4>& mods,
                            const nox::client::World& world,
                            const std::function<void(std::uint16_t, std::uint8_t)>& ask)
{
    namespace oc = nox::things::ObjectClass;
    const TypeArt* a = art_.get(typeId);
    if (!a || typeId == 0 || typeId > things_.things.size()) {
        return {};
    }
    const nox::things::ThingDef& t = things_.things[typeId - 1u];
    const auto pretty = [&] { return prettyName(t, strings_); };
    if (a->classes & (oc::Weapon | oc::Armor | oc::Wand | oc::Flag)) {
        const nox::things::EquipDef* def = (a->classes & (oc::Weapon | oc::Wand | oc::Flag)) ? modifiers_.findWeapon(t.name)
                                                                                         : modifiers_.findArmor(t.name);
        // Not in modifier.bin: ToolTip.c:NoArmsInfo with the thing's name.
        if (!def) {
            std::string s = text("NoArmsInfo", "ToolTip.c");
            if (const auto at = s.find("%S"); at != std::string::npos) {
                s.replace(at, 2, t.name);
            } else if (const auto at2 = s.find("%s"); at2 != std::string::npos) {
                s.replace(at2, 2, t.name);
            }
            return s;
        }
        // Staffs (weapon SUBCLASS 0x7800000) take no modifier words.
        const bool words = !(a->classes & oc::Weapon) || !(a->subclasses & 0x7800000);
        const auto word = [&](std::size_t i, bool secondary) {
            const nox::things::ModifierDef* m = words ? modifiers_.modifier(mods[i]) : nullptr;
            return m ? text(secondary ? m->secondaryDesc : m->desc) : std::string();
        };
        // English's order (Gui_Tooltip_GetDrawableText's default case).
        // Modifier slots 0 and 1 go before the item's own name, 2 after it, then 3 in its secondary wording.
        std::string out;
        for (const std::size_t i : {std::size_t{0}, std::size_t{1}}) {
            if (const auto w = word(i, false); !w.empty()) {
                out += w + " ";
            }
        }
        out += text(def->desc);
        if (const auto w = word(2, false); !w.empty()) {
            out += " " + w;
        }
        if (const auto w = word(3, true); !w.empty()) {
            out += " " + w;
        }
        return out;
    }
    if (a->classes & oc::InfoBook) {
        // Spell book (SUBCLASS 1), lore scroll (2), ability book (4): what it holds, asked once.
        const std::uint8_t kind = (a->subclasses & 1) ? 1 : (a->subclasses & 2) ? 2 : (a->subclasses & 4) ? 4 : 0;
        if (kind == 0) {
            return pretty();
        }
        const auto& books = world.self().bookContents;
        const auto it = books.find(netCode);
        // Not known yet: asked of the server once; nothing shown till the answer comes.
        if (it == books.end()) {
            if (asked_.insert(netCode).second && ask) {
                ask(netCode, kind);
            }
            return {};
        }
        const std::uint8_t id = it->second;
        if (kind == 2) {
            if (id < 1 || id > nox::things::kGuideCreatures) {
                return {};
            }
            const std::string creature =
                text("creature:" + std::string(nox::things::kGuideNames[static_cast<std::size_t>(id - 1)]));
            return creature + " " + text("LoreScroll", "ToolTip.c");
        }
        // A spell book (kind 1) or an ability book (kind 4): "Book of" and what it teaches.
        std::string title;
        if (kind == 1) {
            if (const auto* s = things_.findSpell(id)) {
                title = text(s->title);
            }
        } else if (id >= 1 && id <= things_.abilities.size()) {
            title = text(things_.abilities[id - 1u].title);
        }
        return title.empty() ? std::string() : text("BookOf", "ToolTip.c") + " " + title;
    }
    return pretty();
}

} // namespace noxsdl
