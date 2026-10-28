// A player's or NPC's layer stack and each layer's material colours (player_art.hpp gives the
// rules, as PlayerDraw and NPCDraw follow them).
#include "player_art.hpp"

#include "noxcore/things/object_class.hpp"

#include <algorithm>

namespace noxsdl {

namespace {

// Player animation states (AnimInd) the layer order tests.
constexpr std::uint8_t kIdle = 0;
constexpr std::uint8_t kWalk = 4;
constexpr std::uint8_t kRun = 6;
constexpr std::uint8_t kAxeStrike = 35;
constexpr std::uint8_t kGreatSwordStrike = 37;
constexpr std::uint8_t kHammerStrike = 39;
constexpr std::uint8_t kRaiseShield = 40;
constexpr std::uint8_t kSneak = 53;
constexpr std::uint32_t kEnchantSneak = 1u << 31;  // (enchantment 31)

// Armour class bits.
constexpr std::uint32_t kCloak = 0x2;
constexpr std::uint32_t kShirts = 0xC00;
constexpr std::uint32_t kWizardRobe = 0x4000;
constexpr std::uint32_t kShields = 0x3000000;
constexpr std::uint32_t kRoundShield = 0x1000000; // else the kite shield

// Whether an AnimDir is one of `set`.
bool dirIn(std::uint8_t dir, std::initializer_list<std::uint8_t> set)
{
    for (const auto d : set) {
        if (d == dir) {
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<PlayerLayer> playerLayers(const nox::things::DrawSpec& draw, const Pose& pose, const PlayerLook& look)
{
    std::vector<PlayerLayer> out;
    std::uint8_t stateIndex = pose.animState;
    const bool npc = look.drawableState.has_value();
    if (!npc) {
        if (stateIndex == kWalk && (look.buffs & kEnchantSneak)) {
            stateIndex = kSneak;
        }
        // A state without art falls back to IDLE (a player's only).
        if (stateIndex >= draw.playerStates.size() || !draw.playerStates[stateIndex]) {
            stateIndex = kIdle;
        }
    }
    if (stateIndex >= draw.playerStates.size() || !draw.playerStates[stateIndex] ||
        draw.playerStates[stateIndex]->frames == 0) {
        return out;
    }
    const auto& state = *draw.playerStates[stateIndex];
    const std::size_t slot = directionSlot(pose.direction);
    const std::size_t frame = animationFrame(state.playback, state.frames, state.frameDelay, pose);
    const std::uint8_t dir = pose.direction;
    // The state the order tests read: the drawable's AnimInd (an NPC's is its monster state).
    const std::uint32_t order = npc ? *look.drawableState : stateIndex;

    const auto add = [&](const std::optional<nox::things::PlayerPart>& part, LayerSource source, std::uint32_t bit) {
        if (!part) {
            return false;
        }
        const auto& frames = (*part)[slot];
        if (frames.empty()) {
            return false;
        }
        out.push_back({frames[std::min(frame, frames.size() - 1)], source, bit});
        return true;
    };
    // Armour piece `index` is class bit 1 << index.
    const auto addArmor = [&](std::size_t index) { return add(state.armor[index], LayerSource::Armor, 1u << index); };

    // The body first, then armour and weapons in an order set by the facing.
    add(state.naked, LayerSource::Body, 0);

    const auto armour = [&] {
        const std::uint32_t equip = look.armorEquip;
        // Facing south (AnimDirs 6-8: toward the eye) the cloak goes first, under the rest, except in
        // these states.
        const bool cloakFirst = (equip & kCloak) && dirIn(dir, {6, 7, 8}) && order != kRun && order != kAxeStrike &&
                                order != kHammerStrike && order != kRaiseShield;
        if (cloakFirst) {
            addArmor(1);
        }
        bool shieldDrawn = false;
        for (std::size_t i = 0; i < state.armor.size(); ++i) {
            const std::uint32_t bit = 1u << i;
            if (bit == kCloak || !(equip & bit)) {
                continue;
            }
            // Facing AnimDirs 0, 3, 6 or 7 the shield is left for last (drawn after the cloak, below).
            if ((bit & kShields) && dirIn(dir, {0, 3, 6, 7})) {
                continue;
            }
            // A wizard robe hides the shirts.
            if ((bit & kShirts) && (equip & kWizardRobe)) {
                continue;
            }
            if (addArmor(i) && (bit & kShields)) {
                shieldDrawn = true;
            }
        }
        if ((equip & kCloak) && !cloakFirst) {
            addArmor(1);
        }
        if (!shieldDrawn && (equip & kShields)) {
            addArmor((equip & kRoundShield) ? 24 : 25);  // (bit 24 the round shield, 25 the kite)
        }
    };
    const auto weapons = [&] {
        for (std::size_t i = 2; i < state.weapons.size(); ++i) {
            if (look.weaponEquip & (1u << i)) {
                add(state.weapons[i], LayerSource::Weapon, 1u << i);
            }
        }
    };
    // Facing AnimDirs 0-3 or 6 (not in a great sword strike) the weapons go under the armour; else over.
    if (dirIn(dir, {0, 1, 2, 3, 6}) && order != kGreatSwordStrike) {
        weapons();
        armour();
    } else {
        armour();
        weapons();
    }
    return out;
}

SpriteCache::Materials playerMaterials(const std::array<std::uint8_t, 15>& colors)
{
    const auto rgb = [&](std::size_t i) {
        return std::array<std::uint8_t, 3>{colors[i * 3], colors[i * 3 + 1], colors[i * 3 + 2]};
    };
    const std::array<std::uint8_t, 3> white{255, 255, 255};
    // Slot n = Colors[1, 3, 5, 4, 2, 0][n - 1], where Colors = {rgb1, rgb0, rgb2, rgb3, rgb4, white}.
    return {white, rgb(0), rgb(3), white, rgb(4), rgb(2), rgb(1)};
}

bool applyItemColours(SpriteCache::Materials& materials, const nox::things::EquipDef* def,
                      const std::array<std::uint8_t, 4>* modifiers, const nox::things::ModifierDb& db)
{
    if (!def) {
        return false;
    }
    for (std::size_t slot = 1; slot <= 6; ++slot) {
        materials[slot] = def->colors[slot - 1];
    }
    if (modifiers) {
        for (std::size_t i = 0; i < 4; ++i) {
            if (const auto* mod = db.modifier((*modifiers)[i])) {
                materials[def->modifierSlots[i]] = mod->color;
            }
        }
    }
    return true;
}

std::optional<SpriteCache::Materials> itemMaterials(const TypeArt& art, const std::array<std::uint8_t, 4>* modifiers,
                                                    const nox::things::ModifierDb& db)
{
    if (art.itemColours == ItemColours::None) {
        return std::nullopt;
    }
    SpriteCache::Materials materials{};
    materials.fill({255, 255, 255});
    const auto* def = art.itemColours == ItemColours::Weapon ? db.findWeapon(art.name) : db.findArmor(art.name);
    if (!applyItemColours(materials, def, modifiers, db)) {
        return std::nullopt;
    }
    return materials;
}

SpriteCache::Materials maidenMaterials(const nox::client::NpcEntry& npc)
{
    SpriteCache::Materials m{};
    m.fill({255, 255, 255});
    for (std::size_t slot = 1; slot <= 6; ++slot) {
        m[slot] = npc.colors[slot - 1];
    }
    return m;
}

namespace {

// Each body slot's green raised by 100, held at 255 (poison).
void greener(SpriteCache::Materials& m)
{
    for (std::size_t slot = 1; slot <= 6; ++slot) {
        auto& g = m[slot][1];
        g = g < 155 ? static_cast<std::uint8_t>(g + 100) : 255;
    }
}

} // namespace

Wearer playerWearer(const nox::client::PlayerEntry* player)
{
    constexpr std::uint32_t kPoisoned = 0x400;
    Wearer w;
    if (player) {
        w.body = playerMaterials(player->colors);
        if (player->status & kPoisoned) {
            greener(*w.body);
        }
        w.armorModifiers = &player->armorModifiers;
        w.weaponModifiers = &player->weaponModifiers;
    }
    return w;
}

Wearer npcWearer(const nox::client::NpcEntry& npc)
{
    Wearer w;
    w.body = maidenMaterials(npc);
    if (npc.poison == 1) {
        greener(*w.body);
    }
    w.armorModifiers = &npc.armorModifiers;
    w.weaponModifiers = &npc.weaponModifiers;
    return w;
}

std::vector<SpriteCache::Materials> layerMaterials(const std::vector<PlayerLayer>& layers,
                                                   const nox::client::PlayerEntry* player,
                                                   const nox::things::ModifierDb& modifiers, std::uint32_t buffs,
                                                   std::uint32_t frame)
{
    return layerMaterials(layers, playerWearer(player), modifiers, buffs, frame);
}

std::vector<SpriteCache::Materials> layerMaterials(const std::vector<PlayerLayer>& layers, const Wearer& wearer,
                                                   const nox::things::ModifierDb& modifiers, std::uint32_t buffs,
                                                   std::uint32_t frame)
{
    constexpr std::uint32_t kInvulnerable = 1u << 23;
    constexpr std::uint32_t kFrozen = 1u << 25;
    const std::array<std::uint8_t, 3> blue{0, 160, 255};
    const std::array<std::uint8_t, 3> flash = (frame & 1) ? std::array<std::uint8_t, 3>{255, 255, 255} : blue;  // white and blue on alternate frames
    const auto fill = [](SpriteCache::Materials& m, const std::array<std::uint8_t, 3>& c) {
        for (std::size_t slot = 1; slot <= 6; ++slot) {
            m[slot] = c;
        }
    };

    std::vector<SpriteCache::Materials> out;
    SpriteCache::Materials current{};
    current.fill({255, 255, 255});
    // Each layer starts from the colours the one below left (the game keeps one set for the stack).
    for (const auto& layer : layers) {
        if (layer.source == LayerSource::Body) {
            if (buffs & kInvulnerable) {
                fill(current, flash);
            } else if (buffs & kFrozen) {
                fill(current, blue);
            } else if (wearer.body) {
                current = *wearer.body;
            }
        } else if ((layer.source == LayerSource::Armor && (buffs & kInvulnerable)) ||
                   (layer.source == LayerSource::Weapon && (buffs & kFrozen))) {
            // Invulnerable armour and frozen weapons flash too.
            fill(current, flash);
        } else if (const auto* list = layer.source == LayerSource::Armor ? wearer.armorModifiers : wearer.weaponModifiers) {
            const bool armor = layer.source == LayerSource::Armor;
            const auto& equipped = *list;
            // Only items the game has an equip record for recolour (looks the bit up in
            // the wearer's equipment list first).
            if (const auto it = equipped.find(layer.bit); it != equipped.end()) {
                const auto name = armor ? nox::things::armorItemName(layer.bit) : nox::things::weaponItemName(layer.bit);
                applyItemColours(current, armor ? modifiers.findArmor(name) : modifiers.findWeapon(name), &it->second,
                                 modifiers);
            }
        }
        out.push_back(current);
    }
    return out;
}

// --- A map record's look ---------------------------------------------------------------------------

namespace {

/// The four modifier ids a record names (0xFF for an empty or unknown slot), as the map view and
/// the server's load look them up by name.
std::array<std::uint8_t, 4> modifierIds(const nox::map::xfer::Modifiers& names, const nox::things::ModifierDb& db)
{
    std::array<std::uint8_t, 4> ids{};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto* m = names[i].empty() ? nullptr : db.findModifier(names[i]);
        ids[i] = m ? m->id : std::uint8_t{0xFF};
    }
    return ids;
}

const nox::map::xfer::Modifiers* itemModifiers(const nox::map::xfer::Data& data)
{
    namespace xf = nox::map::xfer;
    if (const auto* w = std::get_if<xf::Weapon>(&data)) return &w->modifiers;
    if (const auto* a = std::get_if<xf::Armor>(&data)) return &a->modifiers;
    if (const auto* m = std::get_if<xf::Ammo>(&data)) return &m->modifiers;
    if (const auto* t = std::get_if<xf::Team>(&data)) return &t->modifiers;
    return nullptr;
}

std::uint32_t mix(std::uint32_t h, std::uint32_t v)
{
    return (h ^ v) * 16777619u;
}

} // namespace

std::uint32_t RecordLook::hash() const
{
    if (kind == Kind::Plain) {
        return 0;
    }
    std::uint32_t h = mix(2166136261u, static_cast<std::uint32_t>(kind) + 1);
    for (const auto& c : npc.colors) {
        h = mix(h, static_cast<std::uint32_t>(c[0]) << 16 | static_cast<std::uint32_t>(c[1]) << 8 | c[2]);
    }
    h = mix(mix(mix(h, npc.poison), npc.weaponEquip), npc.armorEquip);
    for (const auto* list : {&npc.weaponModifiers, &npc.armorModifiers}) {
        for (const auto& [bit, ids] : *list) {
            h = mix(mix(h, bit), static_cast<std::uint32_t>(ids[0]) << 24 | static_cast<std::uint32_t>(ids[1]) << 16 |
                                     static_cast<std::uint32_t>(ids[2]) << 8 | ids[3]);
        }
    }
    if (modifiers) {
        const auto& m = *modifiers;
        h = mix(h, static_cast<std::uint32_t>(m[0]) << 24 | static_cast<std::uint32_t>(m[1]) << 16 | static_cast<std::uint32_t>(m[2]) << 8 | m[3]);
    }
    return h == 0 ? 1 : h;
}

RecordLook recordLook(const TypeArt& art, const nox::map::xfer::Data& data, std::span<const RecordItem> inventory,
                      const nox::things::ThingDb& things, const nox::things::ModifierDb& modifiers)
{
    namespace xf = nox::map::xfer;
    namespace oc = nox::things::ObjectClass;
    RecordLook look;
    if (art.drawFn == DrawFn::Maiden) {
        // MaidenDraw: the record's six colours (a FEMALE_NPC's xfer carries them).
        if (const auto* m = std::get_if<xf::Monster>(&data); m && m->colors) {
            look.kind = RecordLook::Kind::Maiden;
            look.npc.colors = *m->colors;
            look.npc.poison = m->poison;
        }
        return look;
    }
    if (art.drawFn == DrawFn::NPC) {
        const auto* n = std::get_if<xf::Npc>(&data);
        if (!n) {
            return look;
        }
        look.kind = RecordLook::Kind::Npc;
        look.npc.colors = n->colors;
        look.npc.poison = n->poison;
        // What it wears: its EQUIPPED weapons and armour, put on in inventory order as the server
        // puts them on at load (npcWearLoaded: the first two-handed weapon or shield keeps the other
        // kind off; then each through NPCEquipWeapon / NPCEquipArmor, which take off what it
        // replaces).
        struct Worn {
            bool weapon = false;
            std::uint32_t subclasses = 0;
            std::uint32_t bit = 0;
            std::array<std::uint8_t, 4> ids{0xFF, 0xFF, 0xFF, 0xFF};
        };
        constexpr std::uint32_t kTwoHanded = 0x7FFE40Cu; // weapon bits: bows, crossbows, the great sword, staves
        constexpr std::uint32_t kShieldBits = 0x3000000u; // armour bits
        std::vector<Worn> list;
        bool twoHanded = false;
        bool shield = false;
        for (const auto& item : inventory) {
            if (!item.worn) {
                continue;
            }
            const auto* def = things.findThing(item.type);
            if (!def) {
                continue;
            }
            const std::uint32_t classes = nox::things::thingClasses(*def);
            Worn w;
            w.weapon = (classes & (oc::Weapon | oc::Wand)) != 0;
            if (!w.weapon && !(classes & oc::Armor)) {
                continue;
            }
            w.subclasses = nox::things::thingSubclasses(*def);
            w.bit = w.weapon ? nox::things::weaponBitOf(item.type) : nox::things::armorBitOf(item.type);
            if (item.modifiers) {
                w.ids = modifierIds(*item.modifiers, modifiers);
            }
            if (w.weapon && (w.subclasses & kTwoHanded)) {
                if (shield) continue;
                twoHanded = true;
            } else if (!w.weapon && (w.subclasses & 2u)) {
                if (twoHanded) continue;
                shield = true;
            }
            list.push_back(w);
        }
        std::vector<Worn> on;
        const auto takeOffFirst = [&on](const auto& pred) {
            if (const auto it = std::ranges::find_if(on, pred); it != on.end()) on.erase(it);
        };
        for (const Worn& w : list) {
            if (w.weapon) {
                if (!(w.subclasses & 0xCu)) {
                    takeOffFirst([](const Worn& o) { return o.weapon && o.bit == 2; }); // (the quiver, but for a bow)
                }
                if (!(w.subclasses & 2u)) {
                    takeOffFirst([&w](const Worn& o) { return o.weapon && (!(w.subclasses & 0xCu) || !(o.subclasses & 2u)); });
                }
                on.push_back(w);
                if (w.bit & kTwoHanded) {
                    std::erase_if(on, [&](const Worn& o) { return !o.weapon && (o.bit & kShieldBits); });
                }
            } else {
                takeOffFirst([&w](const Worn& o) { return !o.weapon && o.subclasses == w.subclasses; });
                on.push_back(w);
                if (w.subclasses & 2u) {
                    std::erase_if(on, [&](const Worn& o) { return o.weapon && (o.bit & kTwoHanded); });
                }
            }
        }
        for (const Worn& w : on) {
            (w.weapon ? look.npc.weaponEquip : look.npc.armorEquip) |= w.bit;
            (w.weapon ? look.npc.weaponModifiers : look.npc.armorModifiers).try_emplace(w.bit, w.ids); // (the first report of a bit)
        }
        return look;
    }
    if (art.itemColours != ItemColours::None) {
        look.kind = RecordLook::Kind::Item;
        if (const auto* names = itemModifiers(data)) {
            look.modifiers = modifierIds(*names, modifiers);
        }
    }
    return look;
}

std::vector<LookLayer> lookLayers(const TypeArt& art, const RecordLook& look, std::optional<nox::things::ImageRef> image,
                                  ThingArt& arts, const nox::things::ModifierDb& modifiers, std::uint8_t direction, double time)
{
    std::vector<LookLayer> out;
    if (look.kind == RecordLook::Kind::Npc) {
        // NPCDraw: NewPlayer's layers in the player state its monster state turns into (standing:
        // the monster's IDLE, 8), with its colours and its worn things'.
        const TypeArt* body = arts.get("NewPlayer");
        if (!body) {
            return out;
        }
        constexpr std::uint8_t kMonsterIdle = 8;
        std::uint8_t choice = 0;
        Pose pose;
        pose.direction = direction;
        pose.time = time;
        pose.animState = nox::client::npcPlayerState(kMonsterIdle, look.npc.weaponEquip, look.npc.armorEquip, 0, choice,
                                                     [](int low, int) { return low; });
        PlayerLook pl{look.npc.armorEquip, look.npc.weaponEquip, 0};
        pl.drawableState = kMonsterIdle;
        const auto layers = playerLayers(body->draw, pose, pl);
        const auto materials = layerMaterials(layers, npcWearer(look.npc), modifiers);
        for (std::size_t i = 0; i < layers.size(); ++i) {
            out.push_back({layers[i].image, materials[i]});
        }
        return out;
    }
    if (!image) {
        return out;
    }
    LookLayer one{*image, std::nullopt};
    if (look.kind == RecordLook::Kind::Maiden) {
        one.colours = maidenMaterials(look.npc);
    } else if (look.kind == RecordLook::Kind::Item || art.itemColours != ItemColours::None) {
        one.colours = itemMaterials(art, look.modifiers ? &*look.modifiers : nullptr, modifiers);
    }
    out.push_back(one);
    return out;
}

} // namespace noxsdl
