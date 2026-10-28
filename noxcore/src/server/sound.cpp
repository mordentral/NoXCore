// The server's sounds (sound.hpp): the sound table read from thing.bin, sounds played into the
// frame's list, and each player's share of them as audio event messages.
#include "noxcore/server/sound.hpp"

#include "noxcore/audio/sound_names.hpp"
#include "noxcore/client/spell_bars.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace nox::server {

namespace oc = things::ObjectClass;

SoundTable::SoundTable(const things::ThingDb& things)
{
    // AUD: its hearing, volume, range (x 15, when above 0), most at once, its
    // files counted, flags 2.
    for (const things::AudioDef& a : things.audio) {
        const auto id = audio::soundId(a.name);
        if (!id || *id >= info_.size()) {
            continue;
        }
        SoundInfo& s = info_[*id];
        if (0 < a.range) {
            s.range = a.range * 15;
        }
        s.hearing = a.hearing;
        s.volume = a.volume;
        s.most = static_cast<std::uint8_t>(a.maxInstances);
        s.files += static_cast<int>(a.files.size());
        s.flags = 2;
    }
    // AVNT: its fields one by one.
    for (const things::AvntDef& a : things.avnts) {
        const auto id = audio::soundId(a.name);
        if (!id || *id >= info_.size()) {
            continue;
        }
        SoundInfo& s = info_[*id];
        if (a.flags) {
            s.flags = *a.flags;
        }
        if (a.volume) {
            s.volume = *a.volume;
        }
        if (a.maxInstances) {
            s.most = *a.maxInstances;
        }
        if (a.files) {
            s.files += static_cast<int>(a.files->size());
        }
        if (a.range) {
            s.range = *a.range * 15;
        }
        if (a.hearing) {
            s.hearing = *a.hearing;
        }
    }
}

namespace {

/// (The exe drops a sound with no files: getSevenDwords3, checked at the send
/// here, where the table is.) Game flag 0x80000: no sounds.
bool silent(const World& world) { return (world.gameFlags & 0x80000u) != 0; }

/// A host's list is full (World::soundLimit: the "AudEvent" class's 0x80 records
/// finding none free): the sound isn't made, nor noted for the monsters. Under a limit, a sound
/// with no files isn't made either, as the exe's (its record never taken).
bool noRecord(World& world, std::uint32_t id)
{
    if (world.soundLimit == 0) {
        return false;
    }
    if (world.soundTable && (*world.soundTable)[id].files <= 0) {
        return true;
    }
    return world.soundLimit <= world.sounds().size();
}

} // namespace

void playSound(World& world, std::uint32_t id, const ServerObject& source, int mode, std::uint32_t arg)
{
    if (silent(world) || id == 0 || noRecord(world, id)) {
        return;
    }
    // A quest player's own while its .plr is applied: none.
    if (world.quest() && (source.classes & oc::Player) && source.player && source.player->transfer) {
        return;
    }
    const std::uint32_t from = (source.flags & ObjFlag::Destroyed) ? 0u : source.extent;
    world.sounds().push_back({id, source.x, source.y, from, static_cast<std::uint8_t>(mode), arg});
    // For the monsters' ears, but one to one player: a sound with a hearing and
    // files, from a creature or a player or something of one's (up its owners while it's neither).
    // (A sound at a point has no source: never heard.)
    if (mode == 2 || !world.soundTable || (*world.soundTable)[id].hearing == 0 || (*world.soundTable)[id].files <= 0) {
        return;
    }
    const ServerObject* o = &source;
    for (int guard = 0; o && !(o->classes & (oc::Monster | oc::Player)) && o->owner != 0 && guard < 16; ++guard) {
        o = world.byExtent(o->owner);
    }
    // (Its "MonsterListen" class holds 0x80 at once, freed only as the monsters' hearing passes
    // over the stale: full, the sound goes unnoted.)
    if (o && (o->classes & (oc::Monster | oc::Player)) && world.heardSounds.size() < 0x80) {
        world.heardSounds.push_back({id, from, source.x, source.y, world.frameNow});
    }
}

int soundLoudness(const SoundInfo& info, float x, float y, float atX, float atY)
{
    // within the range each way (the signed difference: anything left or up passes
    // that test), its distance (+ 0.1, truncated) short of it; 1-100 (the host's fade-out
    // threshold: 0).
    const int range = info.range;
    const float fx = (util::X87(x) - atX).f(); // (each FSTP'd)
    const float fy = (util::X87(y) - atY).f();
    if (!(fx < static_cast<float>(range) && fy < static_cast<float>(range) && 0 < range)) {
        return 0;
    }
    // (The squares, their sum and the 0.1 on the x87's stack, FSQRT, FSTP double, Math_DoubleToInt.)
    const int d = static_cast<int>(util::sqrt(util::X87(fy) * fy + util::X87(fx) * fx + 0.1f).d());
    if (range <= d) {
        return 0;
    }
    return std::clamp((range - d) * 100 / range, 0, 100);
}

void playSoundAt(World& world, std::uint32_t id, float x, float y, int mode, std::uint32_t arg)
{
    if (silent(world) || id == 0 || noRecord(world, id)) {
        return;
    }
    world.sounds().push_back({id, x, y, 0, static_cast<std::uint8_t>(mode), arg});
}

void playSound(World& world, std::string_view name, const ServerObject& source, int mode, std::uint32_t arg)
{
    if (const auto id = audio::soundId(name)) {
        playSound(world, *id, source, mode, arg);
    }
}

void playSoundAt(World& world, std::string_view name, float x, float y, int mode, std::uint32_t arg)
{
    if (const auto id = audio::soundId(name)) {
        playSoundAt(world, *id, x, y, mode, arg);
    }
}

std::string_view spellSound(const things::ThingDb& things, std::uint32_t spell, int which)
{
    // (fills them in SPEL's order: cast, on, off.)
    const things::SpellDef* def = things.findSpell(spell);
    if (!def) {
        return {};
    }
    return which == 0 ? def->castSound : which == 1 ? def->onSound : def->offSound;
}

std::string_view abilitySound(const things::ThingDb& things, int ability, int which)
{
    // (the record by its name, abilityNameToN.)
    if (ability <= 0 || ability >= static_cast<int>(client::kAbilityNames.size()) || which < 0 || which > 2) {
        return {};
    }
    for (const things::AbilityDef& a : things.abilities) {
        if (a.name == client::kAbilityNames[static_cast<std::size_t>(ability)]) {
            return a.sounds[static_cast<std::size_t>(which)];
        }
    }
    return {};
}

bool soundHasFiles(const World& world, std::uint32_t id)
{
    return world.soundTable && id != 0 && 0 < (*world.soundTable)[id].files;
}

std::uint32_t creatureSound(const World& world, const ServerObject& o, things::SoundSlot slot)
{
    // monsterGetSoundSet: a monster's (MonsterCreate's, by its thing's name; an
    // NPC's voice, npc.hpp).
    if (!(o.classes & oc::Monster) || !world.soundSets) {
        return 0;
    }
    const things::SoundSet* set = world.soundSets->find(o.monster && o.monster->soundSet ? std::string_view(*o.monster->soundSet) : std::string_view(o.typeName));
    return set ? (*set)[slot] : 0;
}

std::uint32_t phonemeSound(const ServerObject& speaker, int phoneme)
{
    // Phonemes 0-8 to their sound ids (phoneme 4 has none).
    static constexpr std::uint32_t kMale[9] = {0xC1, 0xBA, 0xBB, 0xC0, 0, 0xBC, 0xBF, 0xBE, 0xBD};
    static constexpr std::uint32_t kFemale[9] = {0xC9, 0xC2, 0xC3, 200, 0, 0xC4, 199, 0xC6, 0xC5};
    if (phoneme < 0 || phoneme > 8) {
        return 0;
    }
    const bool female = (speaker.classes & oc::Player) && speaker.typeName == "PlayerFemale";
    return (female ? kFemale : kMale)[phoneme];
}

void playCreatureSound(World& world, const ServerObject& o, things::SoundSlot slot)
{
    playSound(world, creatureSound(world, o, slot), o);
}

// (Materials: 0x10 metal, 8 wood, 4 leather, 2 cloth.)
void weaponDropSound(World& world, const ServerObject& item)
{
    if (item.classes & 0x1000u) {  // (Wand: a staff)
        playSound(world, 0x33F, item);
    } else if (item.material & 0x10u) {
        playSound(world, 0x34B, item);
    } else if (item.material & 8u) {
        playSound(world, 0x34D, item);
    }
}

void armorDropSound(World& world, const ServerObject& item)
{
    if (item.material & 0x10u) {
        playSound(world, 0x325, item);
    } else if (item.material & 8u) {
        playSound(world, 0x32B, item);
    } else if (item.material & 4u) {
        playSound(world, 0x328, item);
    } else if (item.material & 2u) {
        playSound(world, (item.subclasses & 0x20u) ? 0x331u : 0x32Eu, item);
    }
}

void weaponPickupSound(World& world, const ServerObject& taker, const ServerObject& item)
{
    if (item.classes & 0x1000u) {  // (Wand: a staff)
        playSound(world, 0x33E, taker);
    } else if (item.material & 0x10u) {
        playSound(world, 0x34A, taker);
    } else if (item.material & 8u) {
        playSound(world, 0x34C, taker);
    }
}

void armorPickupSound(World& world, const ServerObject& taker, const ServerObject& item)
{
    if (item.material & 0x10u) {
        playSound(world, 0x324, taker);
    } else if (item.material & 8u) {
        playSound(world, 0x32A, taker);
    } else if (item.material & 4u) {
        playSound(world, 0x327, taker);
    } else if (item.material & 2u) {
        playSound(world, (item.subclasses & 0x20u) ? 0x330u : 0x32Du, taker);
    }
}

void materialSound(World& world, const ServerObject& at, std::uint16_t struck, std::uint16_t by)
{
    // the struck's material picks the row, the striker's the column (metal 0x10,
    // flesh / cloth / leather 1, 2, 4, wood 8, stone 0x20, Material7 armour 0x2000).
    const auto column = [&]() -> int {
        switch (by) {
        case 0x10: return 0;
        case 1: case 2: case 4: return 1;
        case 8: return 2;
        case 0x20: return 3;
        case 0x2000: return 4;
        default: return -1;
        }
    }();
    if (column < 0) {
        return;
    }
    static constexpr std::uint16_t kStone[5] = {0x35A, 0x35F, 0x355, 0x350, 0x364};
    static constexpr std::uint16_t kSoft[5] = {0x35E, 0x363, 0x359, 0x354, 0x368};
    static constexpr std::uint16_t kWood[5] = {0x35C, 0x361, 0x357, 0x352, 0x366};
    static constexpr std::uint16_t kMetal[5] = {0x35D, 0x362, 0x358, 0x353, 0x367};
    static constexpr std::uint16_t kEarth[5] = {0x35B, 0x360, 0x356, 0x351, 0x365};
    static constexpr std::uint16_t kArmor7[5] = {0x367, 0x368, 0x366, 0x364, 0x367};
    const std::uint16_t* row = nullptr;
    switch (struck) {
    case 0x20: row = kStone; break;
    case 1: case 2: case 4: row = kSoft; break;
    case 8: row = kWood; break;
    case 0x10: row = kMetal; break;
    case 0x40: case 0x400: row = kEarth; break;
    case 0x2000: row = kArmor7; break;
    default: return;
    }
    playSound(world, row[column], at);
}

void damagedSound(World& world, const ServerObject& at, std::uint16_t material)
{
    // A sound for each material that has one: wood 8, metal 0x10, stone 0x20, earth 0x40 / 0x400, 0x1000.
    switch (material) {
    case 8: playSound(world, 0x36B, at); return;
    case 0x10: playSound(world, 0x36C, at); return;
    case 0x20: playSound(world, 0x369, at); return;
    case 0x40: case 0x400: playSound(world, 0x36A, at); return;
    case 0x1000: playSound(world, 0x36D, at); return;
    default: return;
    }
}

void damageSound(World& world, const things::ModifierDb* modifiers, ServerObject& t, ServerObject* src, ServerObject* obj3)
{
    if (&t == obj3 && (t.classes & 0x1001000u)) {  // (Weapon or Wand)
        return;
    }
    if (src && (src->classes & oc::Monster) && soundHasFiles(world, creatureSound(world, *src, things::SoundSlot::MeleeAttackHitting))) {
        return; // (its own hit sound plays)
    }
    ServerObject* by = obj3 ? obj3 : src;
    if (t.damageSoundFn == "PlayerDamageSound") {
        // soundPlayerDamageSound: the body armour's material, the highest of what's
        // carried (worn or not: an original bug), 0x2000 for a Material7 piece; else flesh.
        if (!by) {
            return;
        }
        std::uint16_t material = 1;
        const things::ModifierDef* m7 = modifiers ? modifiers->findModifier("Material7") : nullptr;
        for (const std::uint32_t e : t.inventory) {
            const ServerObject* i = world.byExtent(e);
            if (!i || !(i->classes & oc::Armor) || !(i->subclasses & 4u)) {
                continue;
            }
            if (m7 && (i->classes & 0x13001000u) && i->modifiers[1] == m7->id) {
                material = 0x2000;
            } else if (material < i->material) {
                material = i->material;
            }
        }
        materialSound(world, t, material, by->material);
        return;
    }
    defaultDamageSound(world, t, by);
}

void defaultDamageSound(World& world, const ServerObject& t, const ServerObject* by)
{
    // soundDefaultDamageSound: not an item someone holds; not by nothing (NONE,
    // 0x4000); for blows only (types 0, 2, 3, 10, 11): a breakable of wood, stone
    // or earth its own sound, else the two materials'.
    if ((t.classes & 0x1001000u) && t.holder != 0) {  // (Weapon or Wand)
        return;
    }
    if (!by || by->material == 0x4000) {
        return;
    }
    switch (t.hurtType) {
    case 0: case 2: case 3: case 10: case 11: break;
    default: return;
    }
    if (t.maxHealth != 0 && (t.material == 8 || t.material == 0x20 || t.material == 0x40)) {
        damagedSound(world, t, t.material);
        return;
    }
    materialSound(world, t, t.material, by->material);
}

std::uint8_t soundGroup(World& world, const SoundEvent& e)
{
    // Its source's group first (a player's own, a monster's polygon's), else the polygon at its point's.
    if (const ServerObject* s = e.source ? world.byExtent(e.source) : nullptr) {
        std::uint8_t g = 0;
        if (s->player) {
            g = s->player->polygonGroup;
        } else if (s->monster && s->monster->polygon != 0 && s->monster->polygon != kPolygonUnknown &&
                   s->monster->polygon <= world.polygons().size()) {
            g = world.polygons()[s->monster->polygon - 1].group;
        }
        if (g != 0) {
            return g;
        }
    }
    const std::uint32_t at = world.polygonAt(static_cast<int>(e.x), static_cast<int>(e.y), 0);
    return at != 0 ? world.polygons()[at - 1].group : std::uint8_t{0};
}

std::vector<std::uint8_t> soundsFor(World& world, const SoundTable& table, const SoundListener& l)
{
    std::vector<std::uint8_t> out;
    soundsFor(world, table, l, out);
    return out;
}

void soundsFor(World& world, const SoundTable& table, const SoundListener& l, std::vector<std::uint8_t>& out)
{
    out.clear();
    const auto send = [&](const SoundEvent& e, int volume) {
        // SrvNet_SendAudioEventToPlayer.
        const int dx = static_cast<int>(e.x - l.camX);
        const int half = std::max(1, static_cast<int>(l.halfWidth));
        const auto pan = static_cast<std::uint8_t>(static_cast<std::int8_t>(dx * 50 / half));
        const auto word = static_cast<std::uint16_t>((volume << 10) | (e.id & 0x3FF));  // (the id in the low 10 bits, the volume above)
        out.insert(out.end(), {static_cast<std::uint8_t>(e.source == l.extent ? 0xA7 : 0xA6), pan, static_cast<std::uint8_t>(word),
                               static_cast<std::uint8_t>(word >> 8)});
    };
    // The capped sounds' loudest, by sound: each sound's list in its order, the lists by the
    // sound's id (one flat list, kept from call to call on this thread to save its allocations).
    struct Capped {
        std::uint32_t id;
        const SoundEvent* e;
        int volume;
    };
    thread_local std::vector<Capped> capped;
    capped.clear();
    // (Newest first: aud puts each at the list's head.)
    for (auto r = world.sounds().rbegin(); r != world.sounds().rend(); ++r) {
        const SoundEvent& e = *r;
        const SoundInfo& info = table[e.id];
        if (info.files <= 0) {
            continue;
        }
        if (e.mode == 1) {
            if (l.team == 0 || e.arg != l.team) {
                continue;
            }
        } else if (e.mode == 2 && e.arg != l.extent) {
            continue;
        }
        const std::uint8_t g = soundGroup(world, e);
        if (g != l.group && g != 0) {
            continue;
        }
        if (!l.hearsOwnPhonemes && 0xBA <= e.id && e.id <= 0xC1 && e.source == l.extent) {
            continue;
        }
        const int v = soundLoudness(info, e.x, e.y, l.camX, l.camY);
        if (v <= 0 || (v >> 1) <= 0) {  // (the volume sent is half the loudness)
            continue;
        }
        if (info.most == 0) {
            send(e, v >> 1);
            continue;
        }
        // in order, loudest first; within 5 of one already there, the new one goes
        // before it only with flag 0x10; past the most, dropped.
        const auto byId = [](const Capped& c, std::uint32_t id) { return c.id < id; };
        const auto first = std::lower_bound(capped.begin(), capped.end(), e.id, byId);
        const auto last = std::find_if(first, capped.end(), [&](const Capped& c) { return c.id != e.id; });
        int n = 1;
        auto it = first;
        bool dropped = false;
        for (; it != last; ++it) {
            const int diff = std::abs((v >> 1) - it->volume);
            if (diff < 5) {
                if (info.flags & 0x10u) {
                    break;
                }
            } else if (it->volume < (v >> 1)) {
                break;
            }
            if (info.most < ++n) {
                dropped = true;
                break;
            }
        }
        if (!dropped) {
            capped.insert(it, Capped{e.id, &e, v >> 1});
        }
    }
    // each capped sound's first `most`.
    for (auto it = capped.begin(); it != capped.end();) {
        const std::uint32_t id = it->id;
        int left = table[id].most;
        for (; it != capped.end() && it->id == id; ++it) {
            if (left-- > 0) {
                send(*it->e, it->volume);
            }
        }
    }
}

} // namespace nox::server
