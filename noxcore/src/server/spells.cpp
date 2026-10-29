// Spells on the server (spells.hpp): a player's spoken spell from MSG_TRY_SPELL to its cast,
// each spell's cast function (Spells::cast), the lasting (duration) spells' start, frame and
// end, the Wall spell's walls, and staves that cast. Glyphs (traps) are in glyphs.cpp.
#include "noxcore/server/spells.hpp"

#include "noxcore/server/combat.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/item_effects.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/missiles.hpp"
#include "noxcore/server/mode_hooks.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/sight.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/spell_ids.hpp"
#include "noxcore/util/bytes.hpp"
#include "noxcore/util/random.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;

/// A number in a thing's text, read to nearest as it always was (the server's chop rounding is for
/// the game's figuring: x87.hpp; a function of its own, so none of the caller's is moved into it).
NOX_NOINLINE float textNumber(std::string_view text)
{
    const util::RoundingScope nearest(false);
    return std::strtof(std::string(text).c_str(), nullptr);
}

constexpr std::uint32_t kFireball = 0x1B;
constexpr std::uint32_t kLesserHeal = 0x29;
constexpr std::uint32_t kMagicMissile = 0x32;

/// The spell's flags (thing.bin's SPEL): 0x20 hostile, 0x1000000 / 0x2000000 / 0x4000000 whose.
constexpr std::uint32_t kHostile = 0x20;

using util::hi;
using util::lo;
using util::X87;

/// A spell missile's start: the caster's radius + 4 (stored) along (fx, fy), plus its velocity
/// (createSpellFly, Spell_CastMagicMissile: x's sum kept on the stack for its velocity, y's
/// stored first).
std::pair<float, float> launchPoint(const ServerObject& caster, float fx, float fy)
{
    const float r = (X87(caster.radius) + 4.0f).f();
    const X87 x = X87(r) * fx + caster.x;
    const float y = (X87(r) * fy + caster.y).f();
    return {(x + caster.vx).f(), (X87(y) + caster.vy).f()};
}

} // namespace

void walkPhoneme(const things::ThingDb& things, std::optional<std::vector<std::uint8_t>>& node, int phoneme)
{
    // updateSpellRelated: off the tree stays off; a phoneme past 0-8 leaves it.
    if (!node) {
        return;
    }
    if (phoneme < 0 || 9 <= phoneme) {
        node.reset();
        return;
    }
    node->push_back(static_cast<std::uint8_t>(phoneme));
    // A node is there when some spell's phonemes go through it (the tree as built).
    bool there = false;
    for (std::size_t i = 0; i < things.spells.size() && !there; ++i) {
        const things::SpellDef& s = things.spells[i];
        there = things.spellNumber(i) != 0 && node->size() <= s.phonemes.size() && std::equal(node->begin(), node->end(), s.phonemes.begin());
    }
    if (!there) {
        node.reset();
    }
}

std::uint32_t phonemeSpell(const things::ThingDb& things, const std::vector<std::uint8_t>& node)
{
    // The last of thing.bin's spells to end there (each sets the node's spell as it's read).
    std::uint32_t spell = 0;
    for (std::size_t i = 0; i < things.spells.size(); ++i) {
        const things::SpellDef& s = things.spells[i];
        const std::uint32_t id = things.spellNumber(i);
        if (id != 0 && s.phonemes.size() == node.size() && std::equal(node.begin(), node.end(), s.phonemes.begin())) {
            spell = id;
        }
    }
    return spell;
}

int Spells::power(const ServerObject& caster, std::uint32_t spell) const
{
    if (caster.typeName == "ImaginaryCaster") {
        return 1;
    }
    // (noxcpp: versusSpells, a mode's; a mode's book, World::modeBook, casts a player's spells at
    // the levels it granted, as a quest does.)
    if ((world.versus() || world.versusSpells) && !(caster.player && world.modeBook())) {
        return 3;
    }
    if (caster.player) {
        return spell < caster.player->spellLevels.size() ? caster.player->spellLevels[spell] : 0;
    }
    if (caster.monster) {
        return caster.monster->spellPower;
    }
    return 3;
}

void Spells::tell(const ServerObject& player, std::vector<std::uint8_t> bytes)
{
    world.broadcasts().push_back({std::move(bytes), true, player.extent});
}

std::pair<float, float> Spells::dropSpot(const ServerObject& o, float reach)
{
    // invForceDropItem's spot (objects.hpp clearPointNear, the same routine): each try turns 0.6 pi
    // round from the last and comes 1/64 of the reach nearer.
    return clearPointNear(world, rng, o.x, o.y, reach);
}

void Spells::jiggle(float x, float y, int strength)
{
    nox::server::jiggle(world, x, y, strength); // (objects.hpp)
}

int Spells::whoseSpell(const ServerObject& caster, std::uint32_t spell) const
{
    const things::SpellDef* def = things.findSpell(spell);
    if (!def || spell == 0 || world.spellDisabled[spell & 0xFF]) {
        return 10; // (not enabled: spellIsEnabled)
    }
    if (!caster.player) {
        // (a summoning, only with at most 4 creatures under its player's control:
        // with the summons, none yet.)
        return 0;
    }
    // (playerCheckSpellClass) Whose spell it is. (noxcpp: a multiclass player,
    // a mode's, casts any class's.)
    if (caster.player->multiclass) {
        return 0;
    }
    // Class 1 is a wizard, 2 a conjurer; the spell's flags say which class may cast it (or
    // either); a warrior (0) casts none.
    const std::uint8_t c = caster.player->classId;
    const std::uint32_t whose = c == 1 ? 0x2000000u : c == 2 ? 0x4000000u : 0;
    if (whose == 0 || (!(def->flags & 0x1000000u) && !(def->flags & whose))) {
        return 9;
    }
    return 0;
}

int Spells::cantCast(const ServerObject& caster, std::uint32_t spell, bool trap) const
{
    if (const int why = whoseSpell(caster, spell)) {
        return why;
    }
    return cantCastSpell(caster, spell, trap);
}

int Spells::cantCastSpell(const ServerObject& caster, std::uint32_t spell, bool trap) const
{
    const int why = gameCantCastSpell(caster, spell, trap);
    // (noxcpp: what the game lets a mode may still refuse, with a reason the player is told.)
    if (why == 0 && world.modeHooks) {
        return world.modeHooks->cantCast(world, caster, spell, trap);
    }
    return why;
}

int Spells::gameCantCastSpell(const ServerObject& caster, std::uint32_t spell, bool trap) const
{
    const things::SpellDef* def = things.findSpell(spell);
    // (checkPlrCantCastSpell) The holders' limits, on the spells of flag
    // 0x80000: King of the Realm's crown (0x11, on a team), the flagball's ball (0x10), a CTF
    // flag carried (0xD). Not for a trap's spells.
    World& w = const_cast<World&>(world);
    const auto owns = [&](std::string_view type) {
        return static_cast<int>(std::ranges::count_if(w.objects(), [&](const auto& e) {
            return e.second.owner == caster.extent && e.second.typeName == type && !e.second.gone();
        }));
    };
    if (!trap && def && (def->flags & 0x80000u)) {
        if (world.gameFlags & GameFlag::KingOfTheRealm) {
            if (owns("Crown") > 0 && caster.team != 0) {
                return 0x11;
            }
        } else if (world.gameFlags & GameFlag::FlagBall) {
            if (owns("GameBall") > 0) {
                return 0x10;
            }
        } else if (world.gameFlags & GameFlag::CaptureTheFlag) {
            for (const std::uint32_t e : caster.inventory) {
                const ServerObject* o = w.byExtent(e);
                if (o && (o->classes & oc::Flag)) {
                    return 0xD;
                }
            }
        }
    }
    if (enchanted(caster, Enchant::AntiMagic)) {
        return 0xE;
    }
    // (a quest game's own spells, 0x6F-0x72, only there.)
    if (!(world.gameFlags & GameFlag::Quest) && spell >= 0x6F && spell <= 0x72) {
        return 10;
    }
    // Its own still out: one fist, one death ball, one meteor; magic missiles and pixies up to
    // their count at its power.
    switch (spell) {
    case 0x1D: // Fist of Vengeance
        if (owns("LargeFist") > 0 || owns("MediumFist") > 0 || owns("SmallFist") > 0) {
            return 3;
        }
        break;
    case 0x1F: // Force of Nature
        return owns("DeathBall") > 0 ? 3 : 0;
    case 0x34: // Meteor
        return owns("Meteor") > 0 ? 3 : 0;
    case 0x32:
    case 0x3A: {
        const int out = owns(spell == 0x32 ? "MagicMissile" : "Pixie");
        const int most = static_cast<int>(world.number(spell == 0x32 ? "MagicMissileCount" : "PixieCount",
                                                       static_cast<std::size_t>(power(caster, spell) - 1)));
        return most <= out ? 3 : 0;
    }
    default:
        break;
    }
    return 0;
}

ServerObject* Spells::cursorTarget(const ServerObject& caster)
{
    // (its cursor object, found at the frame's start: objectAtCursor)
    if (!caster.player || caster.player->cursorObject == 0) {
        return nullptr;
    }
    ServerObject* o = world.byExtent(caster.player->cursorObject);
    return o && !o->gone() ? o : nullptr;
}

int Spells::manaCost(const ServerObject& caster, std::uint32_t spell, bool trap) const
{
    return priceOf(world, things, spell, static_cast<bool>(caster.player), trap);
}

int Spells::priceOf(const World& world, const things::ThingDb& things, std::uint32_t spell, bool player, bool trap)
{
    if (things::isSummonSpell(spell)) {
        // The summonings' prices (the exe's own table, things::kSummonManaCosts).
        return player ? things::summonManaCost(spell) : 0;
    }
    // spellManaCost (2: a trap's).
    if (trap && (spell == 0x18 || spell == 0x2B || spell == 0x38)) {
        return static_cast<int>(world.number(spell == 0x18 ? "EnergyBoltTrapCost" : spell == 0x2B ? "LightningTrapCost" : "ManaBombTrapCost"));
    }
    const things::SpellDef* def = things.findSpell(spell);
    return def ? def->manaCost : 0;
}

ModeHooks::CastPrice Spells::castPrice(const ServerObject& caster, std::uint32_t spell, bool trap) const
{
    const int mana = manaCost(caster, spell, trap);
    if (world.modeHooks) {
        if (const auto price = world.modeHooks->castPrice(world, caster, spell, mana, trap)) {
            return {std::clamp(price->mana, 0, 0xFFFF), std::clamp(price->health, 0, 0xFFFF)};
        }
    }
    return {mana, 0};
}

bool Spells::canPay(const ServerObject& caster, const ModeHooks::CastPrice& price)
{
    const int mana = caster.player ? caster.player->mana : 0;
    return price.mana <= mana && (price.health == 0 || price.health < caster.health);
}

void Spells::pay(ServerObject& caster, const ModeHooks::CastPrice& price)
{
    if (caster.player) {
        caster.player->mana = static_cast<std::uint16_t>(std::max(caster.player->mana - price.mana, 0));
    }
    if (price.health > 0) {
        caster.health = static_cast<std::uint16_t>(std::max(caster.health - price.health, 1)); // (a plain loss: no hit, no hurt)
        caster.touch();
    }
}

void Spells::refund(ServerObject& caster, const ModeHooks::CastPrice& price)
{
    if (caster.player) {
        caster.player->mana = static_cast<std::uint16_t>(std::min<int>(caster.player->mana + price.mana, caster.player->maxMana));
    }
    if (price.health > 0) {
        caster.health = static_cast<std::uint16_t>(std::min<int>(caster.health + price.health, caster.maxHealth));
        caster.touch();
    }
}

void Spells::request(ServerObject& caster, const std::array<std::uint32_t, 5>& spells, bool targetSelf)
{
    // SrvNet_OnPacket's MSG_TRY_SPELL: the hostile check reads the first spell.
    if (!caster.player) {
        return;
    }
    // Not while observing (status bit 1, told GeneralPrint:NoSpellWarningGeneral) nor
    // watching a creature (bit 2, ConjureNoSpellWarning1), both told; not in the air out of a
    // solo game (ObjFlags 0x4000), nor in a chat game (game flag 0x80). (SrvNet_SendInformString:
    // not while its .plr is applied.)
    const auto inform = [&](std::string_view id) {
        if (caster.player->transfer) {
            return;
        }
        tell(caster, informStringBytes(id));
    };
    const bool notObserving = !(caster.player->statusFlags & 1u);
    if (!notObserving) {
        inform("GeneralPrint:NoSpellWarningGeneral");
    }
    const bool notWatching = !(caster.player->statusFlags & 2u);
    if (!notWatching) {
        inform("GeneralPrint:ConjureNoSpellWarning1");
    }
    bool may = notWatching && notObserving;
    if (!(world.gameFlags & GameFlag::Solo) && (caster.flags & ObjFlag::Airborne)) {
        may = false;
    }
    if ((world.gameFlags & GameFlag::Chat) || !may) {
        return;
    }
    const auto count = std::ranges::count_if(spells, [](std::uint32_t s) { return s != 0; });
    const things::SpellDef* first = spells[0] <= 0x88 ? things.findSpell(spells[0]) : nullptr;
    const ServerObject* onCursor = cursorTarget(caster);
    if (count == 1 && first && (first->flags & kHostile) && onCursor && !isEnemy(world, caster, *onCursor) && !world.quest()) {
        return;
    }
    if (!start(caster, spells, targetSelf) && count == 1) {
        for (const std::uint32_t s : spells) {
            if (s != 0) {
                tell(caster, {0xDF, lo(s), hi(s), 0, 0, 0}); // MSG_REPORT_SPELL_STAT: failed
            }
        }
    }
}

bool Spells::start(ServerObject& caster, const std::array<std::uint32_t, 5>& spells, bool targetSelf)
{
    // Spell_StartCast, `count` the handler's: the non-zero spells.
    if (!caster.player || (caster.flags & (ObjFlag::Dead | ObjFlag::Destroyed | ObjFlag::NoUpdate))) {
        return false;
    }
    int count = 0;
    for (const std::uint32_t s : spells) {
        // Spell ids run up to 0x88.
        if (s > 0x88) {
            return false;
        }
        if (s != 0) {
            ++count;
        }
    }
    PlayerControl& p = *caster.player;
    // Not while trading. Each in its book.
    if (p.trade != 0) {
        return false;
    }
    for (const std::uint32_t s : spells) {
        if (s != 0 && p.spellLevels[s] == 0) {
            // (noxcpp: in a mode's book, World::modeBook, where a client's book may still list a
            // spell that wasn't granted, its player is told as a spell spoken by hand that it
            // doesn't know is: Plr_FinishSpellCast's line.)
            if (world.modeBook()) {
                world.textTo(caster, "plyrspel.c:SpellUnknown");
            }
            return false;
        }
    }
    // Not while a spell's being spoken, by book or by hand (SpellCastStart). (A pattern's end in
    // the middle of a book's spell clears that with the spell still spoken: the exe would let a
    // second begin beside it, kept here as one at a time.)
    if (p.castStart != 0 || p.chant) {
        return false;
    }
    const auto refuse =[&](int why, std::uint32_t sound = 0xE7) { // (MSG_INFORM 0: why not, and the "can't")
        tell(caster, {0xA9, 0, lo(why), hi(why), 0, 0});
        playSound(world, sound, caster);
        return false;
    };
    // A list holding Glyph builds a trap: the first `count` of them (the rest dropped, zeros
    // and all).
    const auto listed = std::span(spells).first(static_cast<std::size_t>(count));
    const bool trap = std::ranges::find(listed, 0x22u) != listed.end();
    if (!trap) {
        if (const int why = cantCast(caster, spells[0])) {
            return refuse(why);
        }
    } else {
        // (spellCheckSmth) Its mana must reach each in turn, at a trap's prices. (noxcpp: a mode's
        // price, its health too, kept at 1 or more.)
        int mana = p.mana;
        int health = caster.health;
        for (const std::uint32_t s : listed) {
            const ModeHooks::CastPrice price = castPrice(caster, s, true);
            if (mana < price.mana || (price.health != 0 && health <= price.health)) {
                return refuse(0xC, 0xE8);
            }
            mana -= price.mana;
            health -= price.health;
        }
        if (p.classId != 2) {
            // A wizard: not past MaxTrapCount glyphs out.
            if (static_cast<int>(world.number("MaxTrapCount")) <= p.curTraps) {
                return refuse(5);
            }
        } else {
            // A conjurer: room for a Bomber among its summons (the guide's fifth; else 4), and
            // fewer than MaxBomberCount of its own (unitCountSlaves).
            if (!roomFor(world, things, caster, 5)) {
                return refuse(4);
            }
            const auto bombers = std::ranges::count_if(world.objects(), [&](const auto& e) {
                return e.second.owner == caster.extent && (e.second.classes & oc::Monster) && (e.second.subclasses & 0x2000u) && !e.second.gone();  // (BOMBER)
            });
            if (static_cast<int>(world.number("MaxBomberCount")) <= bombers) {
                return refuse(5);
            }
        }
        for (const std::uint32_t s : listed) {
            if (const int why = cantCast(caster, s, true)) {
                return refuse(why);
            }
        }
    }
    setPlayerState(world, caster, 2, frame); // casting (CAST_SPELL)
    p.hearsOwnPhonemes = true;
    p.castStart = frame;
    PlayerControl::Chant chant;
    std::ranges::copy(listed, chant.spells.begin());
    chant.count = count;
    chant.next = frame;
    chant.targetSelf = targetSelf;
    chant.trap = trap;
    p.chant = chant;
    // (Its magicEnt at the pending-cast list's head.)
    auto& pending = world.pendingCasts();
    std::erase(pending, caster.extent); // (a stale one of an earlier chant: passed over anyway)
    pending.insert(pending.begin(), caster.extent);
    return true;
}

void Spells::update()
{
    // Spell_UpdatePendingCasts: the pending-cast list from its head (newest first), a
    // step a frame for each when its time's come; one done, or its caster DEAD or DESTROYED
    // (0x8020), leaves it.
    auto& pending = world.pendingCasts();
    for (std::size_t i = 0; i < pending.size();) {
        ServerObject* caster = world.byExtent(pending[i]);
        if (!caster || !caster->player || !caster->player->chant) {
            pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(i)); // (its spell already gone)
            continue;
        }
        PlayerControl& p = *caster->player;
        if ((caster->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) || caster->pendingDelete) {
            p.chant.reset(); // (a dead caster's spell goes)
            pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        PlayerControl::Chant& c = *p.chant;
        if (frame < c.next) {
            ++i;
            continue;
        }
        // The chant: `index` the spell being spoken, `spoken` its phonemes so far, `next` the frame
        // the next step is due.
        const std::uint32_t spell = c.spells[static_cast<std::size_t>(c.index)];
        if (c.spoken == 0) {
            tell(*caster, {0x70, static_cast<std::uint8_t>(spell)}); // MSG_REPORT_SPELL_START
        }
        const things::SpellDef* def = things.findSpell(spell);
        const int phonemes = def ? static_cast<int>(def->phonemes.size()) : 0;
        if (c.spoken < phonemes) {
            // The phoneme, to each other player (mode 2: its own client says it); in
            // a WOL game only with the host's Gestures option on.
            const auto ph = static_cast<int>(def->phonemes[static_cast<std::size_t>(c.spoken)]);
            if (!world.wolGame || world.gestures) {
                const std::uint32_t id = phonemeSound(*caster, ph);
                for (const ServerObject* o : world.playerUnits()) {
                    if (o->player && o != caster) {
                        playSound(world, id, *caster, 2, o->extent);
                    }
                }
            }
            // Down the phoneme tree, its player's node with it (Plr_FinishSpellCast
            // reads the spell there).
            walkPhoneme(things, c.node, ph);
            p.phonemeLeaf = c.node;
            ++c.spoken;
            c.next = frame + 3;  // (a phoneme every 3 frames)
            ++i;
            continue;
        }
        // Spoken. (The fifth's next is read past the list in the exe: an original bug.)
        const std::uint32_t next = c.index + 1 < 5 ? c.spells[static_cast<std::size_t>(c.index + 1)] : 0;
        if (c.trap && spell != 0x22) {
            // A trap's spell: into the trap, once each (MSG_INFORM 6 for a second), its trap
            // price paid (short: MSG_INFORM 0xB).
            const auto queued = std::span(p.trapSpells).first(p.trapCount);
            if (std::ranges::find(queued, spell) != queued.end()) {
                tell(*caster, {0xA9, 0, 6, 0, 0, 0});
            } else if (const ModeHooks::CastPrice price = castPrice(*caster, spell, true); !canPay(*caster, price)) {
                tell(*caster, {0xA9, 0, 0xB, 0, 0, 0});
                playSound(world, 0xE8, *caster);
            } else {
                pay(*caster, price);
                p.trapSpells[p.trapCount++] = spell;
            }
        }
        // The next spell, or (the last, or a trap's Glyph) cast.
        if (spell != 0x22 && next != 0) {
            ++c.index;
            c.spoken = 0;
            c.node = std::vector<std::uint8_t>{}; // (the tree's root)
            c.next = frame + 3;
            ++i;
            continue;
        }
        // Cast where its cursor is, at itself or what its cursor's on.
        p.castX = p.cursorX;
        p.castY = p.cursorY;
        p.spellTarget = c.targetSelf ? caster->extent : p.cursorObject;
        const std::uint32_t casterExtent = caster->extent;
        finishCast(*caster);
        p.castStart = 0;
        p.hearsOwnPhonemes = false;
        p.trapCount = 0;
        p.chant.reset();
        // Off the list (found again: the cast may have changed it); the walk goes on from its next.
        if (const auto it = std::ranges::find(pending, casterExtent); it != pending.end()) {
            i = static_cast<std::size_t>(it - pending.begin());
            pending.erase(it);
        }
    }
    // Then the lasting spells (spellBookReact).
    updateLasting();
}

void Spells::finishCast(ServerObject& caster)
{
    // Plr_FinishSpellCast: the spell at its node of the phoneme tree.
    if (!caster.player) {
        return;
    }
    PlayerControl& p = *caster.player;
    const bool atRoot = p.phonemeLeaf && p.phonemeLeaf->empty(); // (nothing spoken)
    std::uint32_t spell = p.phonemeLeaf ? phonemeSpell(things, *p.phonemeLeaf) : 0;
    bool unknown = !atRoot; // ("plyrspel.c:SpellUnknown" unless it's a spell in its book, or Glyph)
    int why = 0;
    if (!atRoot && spell != 0) {
        const things::SpellDef* def = things.findSpell(spell);
        const std::uint32_t flags = def ? def->flags : 0;
        // Out of a quest, a hostile spell with the cursor on a friend does nothing at all: no
        // mana, no report, the caster left casting (an original bug).
        const ServerObject* onCursor = cursorTarget(caster);
        if (!world.quest() && (flags & kHostile) && onCursor && !isEnemy(world, caster, *onCursor)) {
            return;
        }
        if (p.spellLevels[spell] != 0 || spell == 0x22) {
            unknown = false;
            why = whoseSpell(caster, spell);
            if (why == 0) {
                why = cantCastSpell(caster, spell, false);
            }
            if (why == 0) {
                // Its mana, taken now and given back if the spell does nothing. (noxcpp: a mode's
                // price, its health too: castPrice.)
                const ModeHooks::CastPrice price = castPrice(caster, spell, false);
                if (!canPay(caster, price)) {
                    why = 0xB;
                    tell(caster, {0xA9, 0, lo(why), hi(why), 0, 0}); // MSG_INFORM 0: short of mana
                    playSound(world, 0xE8, caster);
                } else {
                    pay(caster, price);
                    // At its chosen target (itself, or the creature its cursor was
                    // on as a book's spell ended; in a quest not a friend for a hostile spell) and
                    // the cursor.
                    ServerObject* target = p.spellTarget ? world.byExtent(p.spellTarget) : nullptr;
                    if (world.quest() && (flags & kHostile) && target && !isEnemy(world, caster, *target)) {
                        target = nullptr;
                    }
                    // (noxcpp: where the new list stood, for a mode told of the cast: what it made.)
                    const std::size_t madeFrom = world.modeHooks ? world.newList().size() : 0;
                    paying = price;
                    const bool went = castByUser(spell, caster, target, p.cursorX, p.cursorY); // (a refund inside clears paying)
                    paying.reset();
                    if (went) {
                        tell(caster, {0xA9, 1, lo(spell), hi(spell), 0, 0}); // MSG_INFORM 1: cast
                        if (world.modeHooks) {
                            const std::span<const std::uint32_t> fresh = world.newList();
                            world.modeHooks->spellCast(world, caster, spell, target, p.cursorX, p.cursorY,
                                                       fresh.subspan(std::min(madeFrom, fresh.size()))); // (noxcpp: told a mode)
                        }
                    } else {
                        refund(caster, price);
                        why = 8;
                    }
                }
            } else {
                tell(caster, {0xA9, 0, lo(why), hi(why), 0, 0}); // MSG_INFORM 0: why not
                playSound(world, 0xE7, caster);
            }
        }
    }
    // Back to idle from casting (state 2).
    if (p.state == 2) {
        setPlayerState(world, caster, 0xD, frame);
    }
    if (unknown) {
        world.textTo(caster, "plyrspel.c:SpellUnknown"); // (Server/Magic/plyrspel.c's line)
        return;
    }
    // MSG_REPORT_SPELL_STAT: the spell, and 15 when it went off (none for spell flag 0x100000).
    const things::SpellDef* def = things.findSpell(spell);
    if (why == 0 && def && (def->flags & 0x100000u)) {
        return;
    }
    tell(caster, {0xDF, lo(spell), hi(spell), 0, 0, static_cast<std::uint8_t>(why == 0 ? 0xF : 0)});
}

bool Spells::castByUser(std::uint32_t spell, ServerObject& caster, ServerObject* target, float x, float y)
{
    // castSpellByUser: a hostile spell shows the caster (invisibility and
    // invulnerability end, an oval shield too); one of flag 4 at another (or at nothing) flies
    // there, "worked" either way; the rest go off at its power.
    const things::SpellDef* def = things.findSpell(spell);
    const std::uint32_t flags = def ? def->flags : 0;
    if (flags & kHostile) {
        disenchant(world, caster, Enchant::Invisible);
        disenchant(world, caster, Enchant::Invulnerable);
        cancelLasting(0x43, caster);
    }
    if ((flags & 4u) && target != &caster) {
        flySpell(caster, target, spell);
        return true;
    }
    return cast(spell, caster, target, x, y);
}

bool Spells::castQueued(ServerObject& caster, ServerObject* target, bool last)
{
    // Plr_CastQueuedSpellFirst / Last: the trap's first (or last) spell
    // (TrapSpells, paid for as it went in) at `target` and its cast point; one it can't
    // (checkPlrCantCastSpell alone) told why with the "can't" sound. Out of the
    // queue either way: the first's place filled from behind, the last's dropped.
    if (!caster.player || caster.player->trapCount == 0) {
        return false;
    }
    PlayerControl& p = *caster.player;
    const std::size_t at = last ? p.trapCount - 1u : 0u;
    const std::uint32_t spell = p.trapSpells[at];
    if (const int why = cantCastSpell(caster, spell, false)) {
        tell(caster, {0xA9, 0, lo(why), hi(why), 0, 0});
        playSound(world, 0xE7, caster);
    } else {
        castByUser(spell, caster, target, p.castX, p.castY);
    }
    if (!last) {
        for (std::size_t i = 1; i < p.trapCount; ++i) {
            p.trapSpells[i - 1] = p.trapSpells[i];
        }
        p.trapSpells[p.trapCount - 1u] = 0;
    }
    --p.trapCount;
    return true;
}

void Spells::flySpell(ServerObject& caster, ServerObject* target, std::uint32_t spell)
{
    // createSpellFly.
    if (!target) {
        const things::SpellDef* def = things.findSpell(spell);
        float cursor[2] = {};
        const float* at = nullptr;
        if (caster.player) {
            cursor[0] = caster.player->cursorX;
            cursor[1] = caster.player->cursorY;
            at = cursor;
        }
        target = searchTarget(world, at, &caster, def ? def->flags : 0, 600.0f, 0, &caster);
    }
    const auto [fx, fy] = directionVector(caster.direction & 0xFF);
    const auto [sx, sy] = launchPoint(caster, fx, fy);
    // (A wall that close: no projectile, but the spell was cast, its mana spent:
    // an original bug.)
    if (!world.traceRay(caster.x, caster.y, sx, sy, Trace::Doors | Trace::ThroughWindows) || !things.findThing("Magic")) {
        return;
    }
    ServerObject& m = world.spawn(things, "Magic", sx, sy);
    m.owner = caster.extent;
    m.flyCaster = caster.extent;
    m.seekTarget = target ? target->extent : 0;
    m.castBy = spell;
    m.value = power(caster, spell);
    m.born = m.timer = frame;
    m.flags |= ObjFlag::Active;
    m.direction = caster.direction;
    m.vx = (X87((X87(fx) * m.speedCur).f()) + caster.vx).f(); // (each step stored)
    m.vy = (X87((X87(fy) * m.speedCur).f()) + caster.vy).f();
    m.newX = sx;
    m.newY = sy;
    if (enchanted(caster, Enchant::Infravision)) {
        enchant(world, m, Enchant::Infravision, enchantTime(caster, Enchant::Infravision), enchantPower(caster, Enchant::Infravision));
    }
    m.touch();
    playSound(world, spellSound(things, spell, 0), caster); // (its cast sound at the caster)
}

bool Spells::wandCast(ServerObject& user, ServerObject& wand)
{
    // useWandCastSpell.
    if (wand.maxCharges != 0 && wand.charges == 0) {
        playSound(world, 0xDE, user); // (spent)
        return false;
    }
    // Its wait less twice its READINESS, as an unsigned (as the WandUse staves do it).
    if (frame - wand.wandLastShot < static_cast<std::uint32_t>(static_cast<int>(wand.wandDelay) - 2 * readiness(wand, modifiers))) {
        return false;
    }
    float x = user.x;
    float y = user.y;
    ServerObject* target = &user;
    if (user.player) {
        x = user.player->cursorX;
        y = user.player->cursorY;
        if (ServerObject* under = cursorTarget(user)) {
            target = under;
        }
    }
    wand.wandCasting = true;
    if (!cast(wand.wandSpell, user, target, x, y, 4)) {
        return true;
    }
    wand.wandLastShot = frame;
    // Chain Lightning's and Plasma's staves spend their charges as the spell runs, not here.
    if (!((wand.classes & oc::Wand) && (wand.subclasses & 0x4040000u)) && wand.maxCharges != 0) {
        --wand.charges;
        wand.chargePercent = wand.charges * 100 / wand.maxCharges;
        if (user.player) {
            tell(user, {0x64, lo(wand.extent), hi(wand.extent), wand.charges, wand.maxCharges}); // MSG_REPORT_CHARGES
        }
    }
    return true;
}

std::uint32_t enchantmentSpell(int enchantment)
{
    // By enchantment bit (world.hpp's Enchant): the spell it comes from, 0 for none.
    static constexpr std::uint8_t kSpells[32] = {0x27, 0x36, 0x03, 0x0C, 0x48, 0x4A, 0, 0, 0x46, 0x24, 0x83, 0x1A, 0, 0x82, 0x01, 0x2A,
                                                 0x0F, 0x3E, 0x40, 0x3F, 0x3D, 0x25, 0x47, 0x28, 0x7F, 0x20, 0x33, 0x43, 0x09, 0x37, 0, 0};
    return kSpells[enchantment & 31];
}

void cancelChannelledSpells(World& world, const ServerObject& caster)
{
    // Lightning (0x18), Chain Lightning (0x2B), Greater Heal (0x23), Channel Life (8), Drain Mana
    // (0x16), Plasma (0x3B), Oval Shield (0x43).
    for (DurationSpell& s : world.durationSpells()) {
        if (s.caster == caster.extent &&
            (s.spell == 0x18 || s.spell == 0x2B || s.spell == 0x23 || s.spell == 8 || s.spell == 0x16 || s.spell == 0x3B || s.spell == 0x43)) {
            cancelDurationSpell(world, s);
        }
    }
}

bool Spells::cast(std::uint32_t spell, ServerObject& caster, ServerObject* target, float x, float y, int powerOverride,
                  ServerObject* sourceObject, ServerObject* firstCaster)
{
    // spellAccept, at the caster's power (spellGetPower: 1-5; gamedata's tables by it).
    // `caster` is its a2 and a3 (the same for a player's spell and a glyph's), `source` its a4:
    // where a spell comes from, a glyph's itself.
    if (spell == 0) {
        return false;
    }
    ServerObject& source = sourceObject ? *sourceObject : caster;
    ServerObject& first = firstCaster ? *firstCaster : caster;
    // A spell of flag 0x80 goes only at a creature. (The capture zones' magic, gameCaptureMagic: not
    // yet.)
    const things::SpellDef* def = things.findSpell(spell);
    if (def && (def->flags & 0x80u) && target && !(target->classes & (oc::Monster | oc::Player))) {
        return false;
    }
    const int power = std::clamp(powerOverride != 0 ? powerOverride : this->power(caster, spell), 1, 5);
    const auto level = static_cast<std::size_t>(power - 1);
    // The spell's own sound (SPEL's; 0 cast, 1 on), where its function plays it.
    const auto ownSound = [&](const ServerObject& at, int which = 0) { playSound(world, spellSound(things, spell, which), at); };
    bool viaLasting = false; // (a lasting spell's start answers for itself)
    // A lasting spell begun (startLasting): `on` its record's caster, `frames` its first wait.
    const auto lasting = [&](ServerObject* on, std::uint32_t frames) {
        viaLasting = true;
        return startLasting(spell, &first, on, &source, target, x, y, power, frames);
    };
    // The point unseen from the source: a player source told so (MSG_INFORM 0, 2), and no cast.
    const auto unseen = [&] {
        if (source.player) {
            tell(source, {0xA9, 0, 2, 0, 0, 0});
        }
        return false;
    };
    // A player casting at another player (not itself) noted for the kill feed's assist.
    const auto noted = [&] {
        if (caster.player && target->player && target != &caster) {
            target->player->assistBy = caster.extent;
            target->player->assistFrame = frame;
        }
    };
    const bool worked = [&]() -> bool {
    switch (spell) {
    case 0x06: // Cancel, Detonate, Phantom: nothing
    case 0x12: // but "cast" (an original bug)
    case 0x39:
        return true;
    case 0x2C: { // Lock (Spell_CastLock): of the doors in a 150 box about the caster, the
        // nearest within 150 of the source whose leaf's middle it sees (ray flags 0); unless
        // another's magic lock is on it, it and the doors about its wall cell (a 34 box)
        // are the caster's for 60 s: only it passes (DoorCollide).
        ServerObject* door = nullptr;
        float best = 1e8f;
        world.unitsInRect(caster.x - 150.0f, caster.y - 150.0f, caster.x + 150.0f, caster.y + 150.0f, [&](ServerObject& o) { // (by the grid)
            if (!(o.classes & oc::Door)) { // (no 0x20 test)
                return;
            }
            const float dx = source.x - o.x;
            const float dy = source.y - o.y;
            const float d2 = dx * dx + dy * dy;
            const auto [lx, ly] = doorLeaf(o.doorDirection);
            if (d2 <= 22500.0f && d2 < best &&
                world.traceRay(source.x, source.y, static_cast<float>(lx) * 0.5f + o.x, static_cast<float>(ly) * 0.5f + o.y, 0)) {
                door = &o;
                best = d2;
            }
        });
        if (!door) {
            return false;
        }
        if (door->owner != 0 && door->owner != caster.extent) {
            if (caster.player) {
                tell(caster, informStringBytes("ExecSpel.c:DoorAlreadyLocked"));
            }
            return false;
        }
        const std::uint32_t until = frame + 30u * 60u;
        door->owner = caster.extent;
        door->timer = until;
        if (const auto cell = world.doorCell(door->extent)) {
            const float cellX = static_cast<float>(cell->first * 23);
            const float cellY = static_cast<float>(cell->second * 23);
            world.unitsInRect(cellX - 34.0f, cellY - 34.0f, cellX + 34.0f, cellY + 34.0f, [&](ServerObject& o) { // (by the grid)
                if (o.classes & oc::Door) { // (no 0x20 test)
                    o.owner = caster.extent;
                    o.timer = until;
                }
            });
        }
        ownSound(*door);
        return true;
    }
    case 0x22: // Glyph (Spell_CastGlyph)
        return castGlyph(caster, source, x, y);
    case 0x13: // Detonate Glyphs
        return detonateGlyphs(caster, source);
    case 0x81: // Trigger Glyph
        return triggerGlyph(caster);
    case 0x02: { // Arachnaphobia (Spell_CastArachnaphobia): where the source sees the
        // point (traceRay: flags 9) an ArachnaphobiaFocus of the caster's, which
        // brings small spiders (ArachnaphobiaUpdate); else a player source told it can't (2).
        if (!world.traceRay(source.x, source.y, x, y, 9)) {
            return unseen();
        }
        if (ServerObject* focus = world.createAt(things, "ArachnaphobiaFocus", x, y, frame)) {
            focus->owner = caster.extent;
        }
        return true;
    }
    case 0x15: // Turn Undead: lasting 70 frames (spellTurnUndeadCreate)
        return lasting(&caster, 0x46);
    case 0x1A: // Fear: AFRAID, FearEnchantDuration
        if (!target) {
            return false;
        }
        enchant(world, *target, Enchant::Afraid, static_cast<int>(world.number("FearEnchantDuration")), power);
        return true;
    case 0x41: // Pull, Push (castPush): all about the source within 600, from 10
    case 0x42: { // out at full, by (Pull|Push)PowerCoeff x its power (x 10)
        // (Its force stored as the float argument it is.)
        const float coeff = (X87(world.number(spell == 0x41 ? "PullPowerCoeff" : "PushPowerCoeff")) * X87::fromDouble(power)).f();
        Missiles{world, things, modifiers, rng, frame}.pushFrom(source.x, source.y, 600.0f, 10.0f, spell == 0x41 ? -coeff : coeff);
        ownSound(caster);
        return true;
    }
    case 0x21: { // Fumble (castFumble)
        if (!target) {
            return false;
        }
        if (target->player || ((target->classes & oc::Monster) && (target->subclasses & 0x10u))) {
            // What it holds and its shield fall, each somewhere within 50 (invForceDropItem).
            for (const std::uint32_t e : std::vector<std::uint32_t>(target->inventory)) {
                ServerObject* item = world.byExtent(e);
                if (item && (item->flags & ObjFlag::Equipped) &&
                    ((item->classes & (oc::Weapon | oc::Wand)) || ((item->classes & oc::Armor) && (item->subclasses & 2u)))) {
                    const auto [dx, dy] = dropSpot(*target, 50.0f);
                    Items{world, things, modifiers, rng, frame}.drop(*target, *item, dx, dy);
                }
            }
            // The flagball it holds knocked away (force 100), nobody's, with 0x39E at it.
            if (ServerObject* ball = heldBall(world, *target)) {
                applyForce(world, *ball, target->x, target->y, 100.0f);
                clearOwner(world, *ball);
                playSound(world, 0x39E, *target);
            }
        } else if (!(target->classes & oc::Monster) || !(target->subclasses & 0x2000u)) {
            // Anything else (but a bomber): all it carries dropped (dropAllItems),
            // pushed off 50 from the source.
            Items{world, things, modifiers, rng, frame}.dropAll(*target);
            applyForce(world, *target, source.x, source.y, 50.0f);
        }
        ownSound(*target);
        return true;
    }
    case 0x17: { // Earthquake (castEquake), about the source
        const float range = world.number("EarthquakeRange");
        const float most = world.number("EarthquakeDamage", level);
        Fight fight{world, rng, frame, &things, modifiers};
        ServerObject* from = fight.parentPlayer(&source);
        // (unitsGetInCircle by the grid; each shaken as it's found,
        // equakeDamage: less with the distance)
        world.unitsInCircle(source.x, source.y, range, [&](ServerObject& o) {
            if (&o != &source && !(o.flags & ObjFlag::Airborne) && o.health != 0) { // (no 0x20 test: a deleted one shaken too)
                // (The share stored; the damage on the stack, chopped by __ftol.)
                const float share = (X87(1.0f) - X87(edgeDistance(o, source)) / range).f();
                fight.damage(o, from, source.extent, util::ftol(X87(most) * share), 0xB);
            }
        });
        jiggle(source.x, source.y, static_cast<int>(world.number("EarthquakeJiggle", level)));
        ownSound(source);
        return true;
    }
    case 0x10: { // Death Ray (Spell_CastDeathRay): at the cursor if the ray (flags 9) from the
        // source is clear, its harm the caster's
        if (!world.traceRay(source.x, source.y, x, y, Trace::Doors | Trace::NearWindows)) {
            return unseen();
        }
        Missiles{world, things, modifiers, rng, frame}.damageAt(x, y, world.number("DeathRayOutRadius"), world.number("DeathRayInRadius"),
                                                              static_cast<int>(world.number("DeathRayDamage")), 0x10, &caster, nullptr, true);
        const auto i16 = [](float v) { return static_cast<std::uint16_t>(static_cast<int>(v)); };
        const std::uint16_t ray[4] = {i16(source.x), i16(source.y), i16(x), i16(y)};
        std::vector<std::uint8_t> fx = {0x94}; // MSG_FX_DEATH_RAY (to those who see the line)
        for (const std::uint16_t v : ray) {
            fx.push_back(lo(v));
            fx.push_back(hi(v));
        }
        world.broadcasts().push_back({std::move(fx), false, 0});
        makeScorch(world, things, x, y, 1, rng, frame); // (sMakeScorch, medium)
        ownSound(source);
        return true;
    }
    case 0x05: // Burn (Spell_CastBurn): a MediumFlame where the cursor is, if the ray's clear
    case 0x1D: // Fist of Vengeance (Spell_CastFistOfVengeance)
    case 0x34: // Meteor (Spell_CastMeteor)
    case 0x35: // Meteor Shower (Spell_CastMeteorShower)
    case 0x80: { // Toxic Cloud (Spell_CastToxicCloud)
        const auto owns = [&](std::initializer_list<std::string_view> types) {
            return std::ranges::any_of(world.objects(), [&](const auto& e) {
                return e.second.owner == caster.extent && !e.second.gone() && !e.second.pendingDelete &&
                       std::ranges::find(types, std::string_view(e.second.typeName)) != types.end();
            });
        };
        const auto says = [&](std::string_view id) {
            if (!caster.player) {
                return;
            }
            tell(caster, informStringBytes(id));
        };
        if (spell == 0x1D && owns({"SmallFist", "MediumFist", "LargeFist"})) {
            says("ExecSpel.c:TooManyFists");
            return false;
        }
        if (spell == 0x34 && owns({"Meteor"})) {
            says("ExecSpel.c:TooManyMeteors");
            return false;
        }
        // The ray from the source: flags 9 (the falling ones: + 0x40, but in the campaign). A
        // glyph's Burn needs none: its flame goes on the glyph.
        const bool burnOnGlyph = spell == 0x05 && source.typeName == "Glyph";
        const std::uint8_t flags = (spell == 0x1D || spell == 0x34 || spell == 0x35) && !(world.gameFlags & GameFlag::Solo)
                                       ? static_cast<std::uint8_t>(Trace::Doors | Trace::NearWindows | Trace::SightOnly)
                                       : static_cast<std::uint8_t>(Trace::Doors | Trace::NearWindows);
        if (!burnOnGlyph && !world.traceRay(source.x, source.y, x, y, flags)) {
            return unseen();
        }
        static constexpr const char* kFists[6] = {"", "SmallFist", "MediumFist", "LargeFist", "LargeFist", "LargeFist"};
        const char* kind = spell == 0x05 ? "MediumFlame" : spell == 0x1D ? kFists[power] : spell == 0x34 ? "Meteor"
                                                         : spell == 0x35 ? "MeteorShower" : "ToxicCloud";
        if (!things.findThing(kind)) {
            return true;
        }
        ServerObject& o = burnOnGlyph ? world.spawn(things, kind, source.x, source.y) : world.spawn(things, kind, x, y);
        // (Burn's flame and the fist are the source's, the rest the caster's.)
        o.owner = spell == 0x05 || spell == 0x1D ? source.extent : caster.extent;
        o.born = frame;
        o.flags |= ObjFlag::Active;
        o.touch();
        switch (spell) {
        case 0x05: { // burning BurnDuration, a spark burst (64) to show it
            o.decayAt = frame + static_cast<std::uint32_t>(world.number("BurnDuration"));
            const auto ix = static_cast<std::uint16_t>(static_cast<int>(o.x));
            const auto iy = static_cast<std::uint16_t>(static_cast<int>(o.y));
            world.broadcasts().push_back({{0x93, lo(ix), hi(ix), lo(iy), hi(iy), 0x40}, false, 0});
            break;
        }
        case 0x1D: // from 255 at FistSpeed, bouncing
            o.value = static_cast<int>(world.number("FistOfVengeanceDamage", level));
            o.xStatus |= 0x20; // (its height goes as MSG_FX_DELTAZ_SPELL_START: SrvNet_SendUnitHeight)
            o.raise(255.0f); // (unitRaise)
            o.zVel = -world.number("FistSpeed");
            o.flags |= ObjFlag::Bouncing;
            o.bounce = 9.0f;
            break;
        case 0x34: // from 255 at MeteorSpeed
            o.value = static_cast<int>(world.number("MeteorDamage", level));
            o.xStatus |= 0x20; // (its height goes as MSG_FX_DELTAZ_SPELL_START: SrvNet_SendUnitHeight)
            o.raise(255.0f); // (unitRaise)
            o.zVel = -world.number("MeteorSpeed");
            break;
        case 0x35:
            o.value = static_cast<int>(world.number("MeteorDamage", level));
            break;
        default: // the cloud: ToxicCloudLifetime seconds
            o.value = util::floatToInt(X87(world.number("ToxicCloudLifetime")) * X87::fromDouble(30)); // (stored, Math_FloatToInt)
            break;
        }
        // Its sound: Burn's at its flame and Meteor's at the point (audCreate), the
        // fist's at the source, the shower's at the caster, the cloud's at the cloud.
        if (spell == 0x05 || spell == 0x34) {
            playSoundAt(world, spellSound(things, spell, 0), spell == 0x05 ? o.x : x, spell == 0x05 ? o.y : y);
        } else {
            ownSound(spell == 0x1D ? source : spell == 0x35 ? caster : o);
        }
        return true;
    }
    case 0x0E: { // Cure Poison (Spell_CastCurePoison)
        if (!target) {
            return false;
        }
        Fight fight{world, rng, frame, &things, modifiers};
        if (target->poison == 0) {
            if (target == &caster && caster.player && def) { // (on itself unpoisoned: its mana back)
                if (paying) {
                    refund(caster, *paying); // (noxcpp: what the cast paid, a mode's price too)
                    paying.reset();
                } else {
                    caster.player->mana = static_cast<std::uint16_t>(std::min<int>(caster.player->mana + def->manaCost, caster.player->maxMana));
                }
            }
            return true;
        }
        const char* said = "ExecSpel.c:PoisonClean";
        if (power < target->poison) {
            fight.reducePoison(*target, power);
            said = "ExecSpel.c:PoisonCure";
        } else {
            fight.removePoison(*target);
        }
        if (target->player) {
            tell(*target, informStringBytes(said));
        }
        ownSound(*target);
        return true;
    }
    case 0x3C: { // Poison (castPoison): its power, to at most its power
        if (!target) {
            return false;
        }
        Fight{world, rng, frame, &things, modifiers}.applyPoison(*target, power, power);
        noted();
        return true;
    }
    case 0x0D: { // Counterspell (castCounterSpell), about the source
        const float range = world.number("CounterspellRange");
        const auto near = [&](float px, float py) { // (both squares on the stack)
            const X87 dx = X87(px) - source.x;
            const X87 dy = X87(py) - source.y;
            return !(X87(range) * range < dx * dx + dy * dy);
        };
        const auto pointFx = [&](float px, float py) { this->pointFx(0x87, px, py); }; // (0x87, netSendPointFx)
        // Spell missiles that can be countered (subclass 1) burst or go (cspellRemoveSpell).
        Missiles missiles{world, things, modifiers, rng, frame};
        // (getMissilesInCircle by the grid, each as it's found)
        world.missilesInCircle(source.x, source.y, range, [&](ServerObject& missile) {
            ServerObject* m = &missile;
            if (!(m->subclasses & 1u) || m->gone() || m->pendingDelete) {
                return;
            }
            pointFx(m->x, m->y);
            if (m->typeName == NameId::DeathBall) {
                // deathBallCreateFragments: three DeathBallFragments where it is,
                // its owner's (createAt), each a random way (randInt 0-255) at its
                // speed; then it goes.
                ServerObject* owner = m->owner ? world.byExtent(m->owner) : nullptr;
                for (int i = 0; i < 3; ++i) {
                    if (ServerObject* f = world.createAt(things, "DeathBallFragment", m->x, m->y, frame)) {
                        if (owner) {
                            setOwner(world, owner, *f);
                        }
                        const int way = nox::util::uniform_int_distribution<int>(0, 255)(rng);
                        f->direction = static_cast<std::uint16_t>(way);
                        const auto [dx, dy] = directionVector(way);
                        f->vx = (X87(dx) * f->speedCur).f();
                        f->vy = (X87(dy) * f->speedCur).f();
                    }
                }
                m->pendingDelete = true;
            } else if (m->typeName != NameId::Magic) {
                missiles.burst(*m);
                m->pendingDelete = true;
            } else {
                m->pendingDelete = true;
            }
        });
        // Lasting spells that can be countered (flag 0x40000), cast within its reach (not one a
        // staff cast, but Chain Lightning).
        for (DurationSpell& s : world.durationSpells()) {
            const things::SpellDef* d = things.findSpell(s.spell);
            if (d && (d->flags & 0x40000u) && (!s.fromStaff || s.spell == 0x2B) && near(s.x, s.y)) {
                cancelDurationSpell(world, s);
            }
        }
        // The creatures about it lose their counterable timed enchantments.
        world.unitsInCircle(source.x, source.y, range, [&](ServerObject& o) { // (by the grid)
            if (&o == &source || (o.flags & (ObjFlag::Dead | ObjFlag::Destroyed)) || o.pendingDelete || o.buffs == 0) {
                return;
            }
            bool shown = false;
            for (int e = 0; e < 32; ++e) {
                const std::uint32_t from = enchanted(o, e) && enchantTime(o, e) != 0 ? enchantmentSpell(e) : 0;
                const things::SpellDef* d = from ? things.findSpell(from) : nullptr;
                if (d && (d->flags & 0x40000u)) {
                    if (!shown) {
                        pointFx(o.x, o.y);
                        shown = true;
                    }
                    disenchant(world, o, e);
                }
            }
        });
        ownSound(source);
        return true;
    }
    case 7: case 0x14: case 0x19: case 0x1E: case 0x49: case 0x4F: case 0x86: case 0x87: case 0x88:
        return true; // (the dispatcher's default: nothing, but it "worked")
    // The enchantments at a target: each its gamedata time at the spell's
    // power, and a player casting at a player noted for the kill feed's assist.
    case 0x01: case 0x03: case 0x0C: case 0x0F: case 0x11: case 0x20: case 0x24: case 0x25: case 0x27: case 0x28: case 0x2A:
    case 0x37: case 0x3D: case 0x3E: case 0x40: case 0x46: case 0x47: case 0x48: case 0x4A: case 0x82: case 0x83: {
        if (!target) {
            return false;
        }
        const auto frames = [&](const char* name) { return static_cast<int>(world.number(name)); };
        const auto onTarget = [&](int e, int time) { enchant(world, *target, e, time, power); };
        switch (spell) {
        case 0x01: onTarget(Enchant::Anchored, frames("AnchorEnchantDuration")); noted(); break;      // Anchor
        case 0x03: onTarget(Enchant::Blinded, 30 * 4); noted(); break;                                // Blind
        case 0x0C: onTarget(Enchant::Confused, frames("ConfuseEnchantDuration")); noted(); break;     // Confuse
        case 0x0F: // Death: not on one marked already; 15 s
            if (enchanted(*target, Enchant::Death)) {
                return false;
            }
            onTarget(Enchant::Death, 450);
            noted();
            break;
        case 0x11: onTarget(Enchant::Detecting, 30 * 60); break;                                      // Detect Magic
        case 0x20: onTarget(Enchant::Freeze, 30 * 4); noted(); break;                                 // Freeze
        case 0x24: onTarget(Enchant::Hasted, frames("HasteEnchantDuration")); break;                  // Haste
        case 0x25: onTarget(Enchant::Infravision, frames("InfravisionEnchantDuration")); break;       // Infravision
        case 0x27: // Invisibility: its own hostile lasting spells end first
        case 0x28: { // Invulnerability
            endHostileLasting(*target);
            if (spell == 0x27) {
                onTarget(Enchant::Invisible, frames("InvisibilityEnchantDuration"));
                pointFx(0x8A, target->x, target->y); // (point FX 0x8A)
            } else {
                onTarget(Enchant::Invulnerable, frames("InvulnerabilityEnchantDuration"));
            }
            break;
        }
        case 0x2A: onTarget(Enchant::Light, frames("LightEnchantDuration")); noted(); break;          // Light
        case 0x37: // Nullify: anti-magic for War Cry's time (the ability table's)
            onTarget(Enchant::AntiMagic, frames("WarCryDuration"));
            noted();
            break;
        case 0x3D: onTarget(Enchant::ProtectFromElectricity, frames("ProtectElectricityEnchantDuration")); break;
        case 0x3E: onTarget(Enchant::ProtectFromFire, frames("ProtectFireEnchantDuration")); break;
        case 0x40: onTarget(Enchant::ProtectFromPoison, frames("ProtectPoisonEnchantDuration")); break;
        case 0x46: onTarget(Enchant::Run, frames("RunEnchantDuration")); noted(); break;              // Run
        case 0x47: // Shock (useShock); a glyph's: ShockTrapDamage at once (type 9)
            if (source.typeName == "Glyph") {
                Fight{world, rng, frame, &things, modifiers}.damage(*target, &caster, caster.extent,
                                                                    static_cast<int>(world.number("ShockTrapDamage", level)), 9);
                break;
            }
            onTarget(Enchant::Shock, frames("ShockEnchantDuration"));
            break;
        case 0x48: onTarget(Enchant::Slowed, frames("SlowEnchantDuration")); noted(); break;          // Slow
        case 0x4A: { // Stun: held, but a warrior (and a monster heavier than 15) slowed
            const bool slowed = target->player ? target->player->classId == 0
                                               : (target->classes & oc::Monster) && 15.0f < target->mass;
            onTarget(slowed ? Enchant::Slowed : Enchant::Held, frames("StunEnchantDuration"));
            noted();
            break;
        }
        case 0x82: { // Vampirism (a quest's x QuestVampDurMultiplier)
            int time = frames("VampirismEnchantDuration");
            if (world.quest()) {
                time = util::floatToInt(X87(world.number("QuestVampDurMultiplier")) * X87::fromDouble(time)); // (stored, Math_FloatToInt)
            }
            onTarget(Enchant::Vampirism, time);
            break;
        }
        case 0x83: onTarget(Enchant::Villain, 30 * power * 12); noted(); break;                       // Villain
        default: break;
        }
        return true;
    }
    case 0x3F: // Protection from Magic: nothing (its cast returns 1; an original bug)
        return true;
    case 0x44: // Restore Health
    case 0x85: // Wink (the same function: castSpellWinkORrestoreHealth)
        if (!target) {
            return false;
        }
        if (target->maxHealth != 0) {
            target->health = target->maxHealth;
            playSound(world, 0x2F2, *target);
            target->touch();
        }
        return true;
    case 0x45: // Restore Mana: a player's whole again (playerManaAdd)
        if (!target) {
            return false;
        }
        if (target->player) {
            target->player->mana = target->player->maxMana;
            playSound(world, 0x2F3, *target);
        }
        return true;
    case 0x26: { // Inversion: spell missiles near the source that seek the caster turn (InversionRange)
        const float range = world.number("InversionRange");
        world.missilesInCircle(source.x, source.y, range, [&](ServerObject& o) { // (by the grid: changeOwner)
            turnSpellMissile(world, o, caster, frame); // (no 0x20 test: a deleted one turned too)
        });
        ownSound(source);
        return true;
    }
    case 0x33: // Shield, Moonglow, Oval Shield: lasting, on their target (the record's caster is
    case 0x36: // the target)
    case 0x43:
        return lasting(target, 0);
    case 0x1C: // Firewalk: lasting 3 s, flames left in its target's wake
        return lasting(&caster, 90);
    case 0x3A: { // Pixie Swarm (castPixies): up to PixieCount of its own
        const int owned = static_cast<int>(std::ranges::count_if(world.objects(), [&](const auto& e) {
            return e.second.owner == caster.extent && e.second.typeName == "Pixie" && !e.second.pendingDelete && !e.second.gone();
        }));
        const int most = static_cast<int>(world.number("PixieCount", level));
        if (owned < most && things.findThing("Pixie")) { // (about the source, the caster's)
            const float r = (X87(source.radius) + 4.0f).f(); // (stored; so is each coordinate)
            for (int i = most - owned; 0 < i; --i) {
                const int way = nox::util::uniform_int_distribution<int>(0, 255)(rng);
                const auto [dx, dy] = directionVector(way);
                const float px = (X87(r) * dx + source.x).f();
                const float py = (X87(r) * dy + source.y).f();
                if (!world.traceRay(source.x, source.y, px, py, Trace::Doors | Trace::ThroughWindows)) {
                    continue;
                }
                ServerObject& p = world.spawn(things, "Pixie", px, py);
                p.owner = caster.extent;
                p.team = caster.team;
                p.born = frame;
                p.flags |= ObjFlag::Active;
                p.direction = static_cast<std::uint16_t>(way);
                p.vx = p.vy = 0;
                p.newX = px;
                p.newY = py;
                p.homeX = source.x; // (Pos39: where it came from)
                p.homeY = source.y;
                ServerObject* prey = searchTarget(world, nullptr, &p, 0x20, 600.0f, 0, &caster);
                p.seekTarget = prey ? prey->extent : 0;
                p.castBy = spell;
                p.expiresAt = frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(30, 90)(rng)) * 30u;
                p.seenOwnerAt = frame;
                p.touch();
            }
            ownSound(source); // (only with room for some: castPixies)
        }
        return true;
    }
    case 0x0A: // Cleansing Flame, Cleansing Mana Flame (Spell_CastCleansingFlame)
    case 0x0B: {
        static constexpr const char* kFlames[10] = {"SmallFlameCleanse", "SmallFlameCleanse", "MediumFlameCleanse", "FlameCleanse", "LargeFlameCleanse",
                                                    "SmallBlueFlameCleanse", "SmallBlueFlameCleanse", "MediumBlueFlameCleanse", "BlueFlameCleanse",
                                                    "LargeBlueFlameCleanse"};
        for (const auto& [e, o] : world.objects()) { // one ring of its own at a time
            if (o.owner == caster.extent && !o.pendingDelete && std::ranges::find(kFlames, std::string_view(o.typeName)) != std::end(kFlames)) {
                if (caster.player) {
                    tell(caster, informStringBytes("plyrspel.c:TooManySpells"));
                }
                return false;
            }
        }
        // Outside a solo game, always size 4. (At 5, a red ring reads the blue table and a blue
        // one past its end: an original bug; here clamped to it.)
        const int size = (world.gameFlags & GameFlag::Solo) ? power : 4;
        for (int i = 0; i < 48; ++i) {
            const int s = size - nox::util::uniform_int_distribution<int>(0, 1)(rng);
            const int index = std::min(s + (spell != 0x0A ? 5 : 0), 9);
            if (s <= 0 || !things.findThing(kFlames[index])) {
                continue;
            }
            const int way = nox::util::uniform_int_distribution<int>(0, 255)(rng);
            const auto [dx, dy] = directionVector(way);
            // Its radius (its thing's EXTENT) and the caster's, and 4, out from it.
            float radius = 0;
            if (const auto e = things.findThing(kFlames[index])->lastAttribute("EXTENT"); e && e->starts_with("CIRCLE")) {
                radius = textNumber(e->substr(6));
            }
            // (From the source, and the source's: a glyph's ring isn't the caster's.)
            // (On the x87: the reach kept on the stack for both; each place one store.)
            const X87 reach = X87(radius) + source.radius + 4.0f;
            const float fx = (reach * dx + source.x).f();
            const float fy = (reach * dy + source.y).f();
            if (!world.traceRay(source.x, source.y, fx, fy, Trace::Doors | Trace::SightOnly)) {
                continue;
            }
            ServerObject& f = world.spawn(things, kFlames[index], fx, fy);
            f.owner = source.extent;
            f.born = frame;
            f.direction = static_cast<std::uint16_t>(way);
            f.vx = dx * 4.0f;
            f.vy = dy * 4.0f;
            f.timer = frame + static_cast<std::uint32_t>(nox::util::uniform_int_distribution<int>(30 * 3, 30 * 6)(rng));
            f.homeX = source.x;
            f.homeY = source.y;
            f.lastX = fx;
            f.lastY = fy;
            f.drag = 0; // (Float28: no friction)
            f.updateFn = "FlameCleanseUpdate"; // (updateFlameCleanse, set by hand)
            f.updates = true;
            f.alwaysMoves = true;
            world.addUpdatable(f); // (Spell_CastCleansingFlame's unitAddToUpdatable)
            f.classes |= 0x40000000u; // (not streamed: the clients run it themselves)
            f.flags |= ObjFlag::Active;
            // MSG_CLIENT_PREDICT_LINEAR (0xB5) to every player: its code, type, spot, facing and
            // velocities x 16.
            const auto u16 = [](float v) { return static_cast<std::uint16_t>(static_cast<int>(v)); };
            const auto i8 = [](float v) { return static_cast<std::uint8_t>(static_cast<std::int8_t>(static_cast<int>(v * 16.0f))); };
            const std::uint16_t px = u16(fx);
            const std::uint16_t py = u16(fy);
            world.broadcasts().push_back({{0xB5, lo(f.extent), hi(f.extent), lo(f.type), hi(f.type), lo(px), hi(px), lo(py), hi(py), lo(f.direction),
                                           hi(f.direction), 0, i8(f.vx), i8(f.vy)},
                                          false,
                                          0});
        }
        ownSound(source);
        return true;
    }
    case 0x84: // Wall: lasting (power + 5) minutes
    case 0x74: // Tag: lasting TagDurationPerLevel x power
        return lasting(&caster, 0);
    case 0x7F: { // Telekinesis (castTelekinesis): a player's hand at its cursor
        if (!target) {
            return false;
        }
        if (target->player && things.findThing("TelekinesisHand")) {
            ServerObject& hand = world.spawn(things, "TelekinesisHand", target->x, target->y);
            hand.owner = target->extent;
            hand.born = frame;
            hand.flags |= ObjFlag::Active;
            enchant(world, *target, Enchant::Telekinesis, 30 * 20, power);
            cancelLastingSpells(world, 0x18, *target);
            cancelLastingSpells(world, 0x2B, *target);
            ownSound(*target, 1); // (its "on" sound)
        }
        return true;
    }
    case 0x1F: // Force of Nature: charging 2/3 s, then its DeathBall
        return lasting(&caster, 20);
    case 0x38: // Mana Bomb: charging till the caster's mana is gone
        return lasting(&caster, 0);
    case 0x18: // Lightning, Chain Lightning: lasting, 30 frames to find a target (each hit:
    case 0x2B: // LightningSearchTime more)
        return lasting(&caster, 30);
    case 0x3B: // Plasma: lasting, PlasmaSearchTime to find a target (each frame it strikes, more)
        return lasting(&caster, static_cast<std::uint32_t>(world.number("PlasmaSearchTime")));
    case 0x04: // Blink, Swap, Teleport to Mark 1-4, Teleport to Target: lasting till they go
    case 0x73: // (in a solo game TeleportDelay; else the next pass)
    case 0x7A:
    case 0x7B:
    case 0x7C:
    case 0x7D:
    case 0x7E:
        return lasting(&caster, 0);
    case 0x2D: // Mark Location: the first free mark, else the longest unused
    case 0x2E: // Mark 1-4: that mark
    case 0x2F:
    case 0x30:
    case 0x31: {
        if (!caster.player) {
            return true;
        }
        PlayerControl& p = *caster.player;
        int slot = 0;
        if (spell == 0x2D) {
            // The first slot with no live mark.
            while (slot < 4 && p.marks[static_cast<std::size_t>(slot)] != 0 && world.byExtent(p.marks[static_cast<std::size_t>(slot)])) {
                ++slot;
            }
            if (slot == 4) { // (all four set: the one used longest ago)
                std::uint32_t oldest = frame;
                slot = 0;
                for (int i = 0; i < 4; ++i) {
                    const ServerObject* m = world.byExtent(p.marks[static_cast<std::size_t>(i)]);
                    if (m && m->timer < oldest) {
                        oldest = m->timer;
                        slot = i;
                    }
                }
            }
        } else {
            slot = static_cast<int>(spell) - 0x2E;
        }
        // Where a glyph cast it, else where the caster is; moved (its last use now) or
        // made; three uses either way.
        const ServerObject& at = source.typeName == "Glyph" ? source : caster;
        const auto i = static_cast<std::size_t>(slot);
        ServerObject* mark = p.marks[i] ? world.byExtent(p.marks[i]) : nullptr;
        if (mark) {
            setPosition(world, *mark, at.x, at.y, frame);
            mark->timer = frame;
        } else {
            static constexpr const char* kMarks[4] = {"TeleportGlyph1", "TeleportGlyph2", "TeleportGlyph3", "TeleportGlyph4"};
            if (things.findThing(kMarks[i])) {
                ServerObject& m = world.spawn(things, kMarks[i], at.x, at.y);
                m.owner = caster.extent;
                m.born = m.timer = frame;
                m.touch();
                p.marks[i] = m.extent;
                mark = &m;
            }
        }
        if (mark) {
            p.markUses[i] = 3;
        }
        ownSound(caster);
        return true;
    }
    case 0x08: // Channel Life, Drain Mana, Greater Heal: lasting while they work
    case 0x16:
    case 0x23:
    case 0x09: // Charm: lasting while the creature's being charmed (Spell_CharmStart)
        return lasting(&caster, 0);
    case kFireball: { // castFireball
        static constexpr const char* kKinds[6] = {"", "Fireball", "StrongFireball", "TitanFireball", "TitanFireball", "TitanFireball"};
        const char* kind = kKinds[power];
        if (!things.findThing(kind)) {
            return true;
        }
        // From the source (its own: a glyph's fireball is the glyph's): twice its radius ahead,
        // and its motion on; at it if a wall's between.
        // (Twice the radius kept on the stack; x's sum stored before its velocity's added, y's
        // kept.)
        const auto [fx, fy] = directionVector(source.direction & 0xFF);
        const X87 r = X87(source.radius) + source.radius;
        float sx = (X87((r * fx + source.x).f()) + source.vx).f();
        float sy = (r * fy + source.y + source.vy).f();
        if (!world.traceRay(source.x, source.y, sx, sy, Trace::Doors | Trace::ThroughWindows)) {
            sx = source.x;
            sy = source.y;
        }
        ServerObject& m = world.spawn(things, kind, sx, sy);
        m.owner = source.extent;
        m.team = caster.team;
        m.born = frame;
        m.flags |= ObjFlag::Active;
        // (The speed kept on the stack as it's stored; x's part stored before the source's
        // velocity's added, y's kept.)
        const X87 speed = X87(world.number("FireballSpeedCoeff", level)) * m.speedCur;
        m.speedCur = speed.f();
        m.vx = (X87((speed * fx).f()) + source.vx).f();
        m.vy = (speed * fy + source.vy).f();
        m.newX = sx;
        m.newY = sy;
        m.direction = source.direction;
        m.touch();
        ownSound(source);
        return true;
    }
    case kLesserHeal: {
        if (!target) {
            return false;
        }
        if (target->health == target->maxHealth && target == &caster) {
            return false; // (nothing to heal on itself: its mana back)
        }
        const int amount = static_cast<int>(world.number("LesserHealAmount")); // (x the class's 1.0)
        adjustHealth(world, *target, amount);
        ownSound(*target, 1);
        return true;
    }
    case kMagicMissile: { // Spell_CastMagicMissile
        if (!things.findThing("MagicMissile")) {
            return true;
        }
        // MagicMissileCount at its power, less those of its own still flying.
        const int most = static_cast<int>(world.number("MagicMissileCount", level));
        const auto flying = static_cast<int>(std::ranges::count_if(world.objects(), [&](const auto& e) {
            return e.second.owner == caster.extent && e.second.typeName == "MagicMissile" && !e.second.gone() && !e.second.pendingDelete;
        }));
        // (Below zero, with more of its own flying than it may have: nothing fired, the mana
        // taken: an original bug.)
        const int count = flying > 0 ? most - flying : most;
        if (count == 0) {
            if (caster.player) {
                tell(caster, informStringBytes("mmissile.c:TooManyMissiles"));
            }
            return false;
        }
        Missiles missiles{world, things, modifiers, rng, frame};
        static constexpr int kSpread[5] = {0, -0x10, 0x10, -0x20, 0x20};
        // (From the source, the caster's.)
        // (The exe's switch has five cases: a sixth missile (a quest's level 5, the ARENA table's
        // 6) keeps the fifth's heading: an original bug.)
        for (int i = 0; i < count; ++i) {
            const int d = ((source.direction & 0xFF) + kSpread[std::min(i, 4)]) & 0xFF;
            const auto [fx, fy] = directionVector(d);
            const auto [sx, sy] = launchPoint(source, fx, fy);
            if (!world.traceRay(source.x, source.y, sx, sy, Trace::Doors | Trace::ThroughWindows)) {
                continue;
            }
            ServerObject& m = world.spawn(things, "MagicMissile", sx, sy);
            m.owner = caster.extent;
            m.team = caster.team;
            m.born = frame;
            m.flags |= ObjFlag::Active | ObjFlag::Airborne;
            // (The speed kept on the stack as it's stored; each part stored, then the source's
            // velocity added.)
            const X87 speed = util::randomFloat(rng, 0.8f, 1.2f) * m.speedCur; // (randomFloat's stack value)
            m.speedCur = speed.f();
            m.direction = static_cast<std::uint16_t>(d);
            m.vx = (X87((speed * fx * 0.1f).f()) + source.vx).f();
            m.vy = (X87((speed * fy * 0.1f).f()) + source.vy).f();
            m.newX = sx;
            m.newY = sy;
            m.touch();
            // After the one the player picked, or the nearest enemy to its cursor (from the
            // missile, if it sees the cursor). A glyph's: the nearest to the missile.
            const ServerObject* t = nullptr;
            if (source.player) {
                t = target && target != &caster ? target : nullptr;
                if (!t) {
                    const bool sees = world.traceRay(sx, sy, source.player->cursorX, source.player->cursorY, Trace::Doors | Trace::ThroughWindows);
                    t = missiles.seekTarget(sees ? source.player->cursorX : sx, sees ? source.player->cursorY : sy, m, 600.0f, &caster);
                }
            } else {
                t = searchTarget(world, nullptr, &m, kHostile, 600.0f, 0, &caster);
            }
            m.seekTarget = t ? t->extent : 0;
        }
        ownSound(caster);
        return true;
    }
    default:
        if (0x4A < spell && spell < 0x73) {
            // A summoning: lasting while the creature comes (Spell_SummonStart).
            return lasting(&caster, 0);
        }
        return false; // (not yet: its mana back)
    }
    }();
    if (!worked && !viaLasting && !direct) {
        playSound(world, 0xE7, source); // (its function said no: spellAccept's "can't" at the source)
    }
    return worked;
}

void revertMagicWalls(World& world, const std::function<bool(const MagicWall&)>& which)
{
    auto& walls = world.magicWalls();
    for (auto it = walls.begin(); it != walls.end();) {
        if (!which(*it)) {
            ++it;
            continue;
        }
        // (SrvNet_BroadcastMagicWallRevert) Those sent it: the old wall
        // back (0x3D) or none (MSG_REMOVE_WALL_MAGIC 0x3E).
        const auto x = static_cast<std::uint8_t>(it->x);
        const auto y = static_cast<std::uint8_t>(it->y);
        if (it->sentTo != 0) {
            world.wallNotices().push_back({it->replaced ? std::vector<std::uint8_t>{0x3D, it->oldMaterial, it->oldDirection, it->oldVariant, x, y}
                                                        : std::vector<std::uint8_t>{0x3E, x, y},
                                           it->sentTo});
        }
        if (!it->replaced) {
            world.walls().erase({it->x, it->y});
        } else if (WallCell* c = world.wall(it->x, it->y)) {
            c->material = it->oldMaterial;
            c->direction = it->oldDirection;
            c->variant = it->oldVariant;
        }
        it = walls.erase(it);
    }
}

void cancelDurationSpell(World& world, DurationSpell& s)
{
    // spellCancelSpellDo.
    const ServerObject* caster = s.caster ? world.byExtent(s.caster) : nullptr;
    if (caster && (caster->classes & oc::Player)) {
        // Chain Lightning reports 0, the rest 15.
        const std::uint8_t value = s.spell == 0x2B ? 0 : 0xF;
        world.broadcasts().push_back({{0xDF, lo(s.spell), hi(s.spell), 0, 0, value}, true, caster->extent});
    }
    // A ray's beam stops (SrvNet_BroadcastStopRaySpell); Chain Lightning's bolts' all.
    if (s.spell == 0x2B) {
        for (const auto& [from, to] : s.bolts) {
            boltFx(world, s, false, from, to);
        }
    } else {
        rayFx(world, s, false, s.target);
    }
    s.cancelled = true;
}

void boltFx(World& world, const DurationSpell& chain, bool start, std::uint32_t from, std::uint32_t to)
{
    // A bolt record's (spell 7) beam: 3 to start, 10 to stop. (Its power byte is the pool's
    // leftover, the record never set (an original bug): the chain's sent.)
    if (from == 0 || to == 0 || !world.byExtent(from) || !world.byExtent(to)) {
        return;
    }
    world.broadcasts().push_back({{0x9E, static_cast<std::uint8_t>(start ? 3 : 10), static_cast<std::uint8_t>(chain.power), lo(from), hi(from), lo(to), hi(to)},
                                  true,
                                  0});
}

void rayFx(World& world, const DurationSpell& s, bool start, std::uint32_t to)
{
    // SrvNet_BroadcastStartRaySpell / SrvNet_BroadcastStopRaySpell:
    // MSG_FX_DURATION_SPELL to everyone; both ends on everyone's minimap while it lasts (bit 2).
    ServerObject* caster = s.caster ? world.byExtent(s.caster) : nullptr;
    ServerObject* target = to ? world.byExtent(to) : nullptr;
    if (!caster || !target) {
        return;
    }
    std::uint8_t kind = 0;
    switch (s.spell) {
    case 7: kind = 3; break;
    case 9: kind = 2; break;
    case 0x16: kind = 5; break;
    case 0x18: kind = 4; break;
    case 0x23: kind = 6; break;
    case 0x3B: kind = 1; break;
    default: return;
    }
    if (!start) {
        kind = static_cast<std::uint8_t>(kind + 7);  // (+7: the stop's kind)
    }
    if (s.spell == 0x23 && caster == target) {
        return;
    }
    // Greater Heal's ends are the other way round; Plasma's byte is the caster's facing.
    const std::uint32_t a = netCodeOf(s.spell == 0x23 ? *target : *caster);
    const std::uint32_t b = netCodeOf(s.spell == 0x23 ? *caster : *target); // (an obelisk: bit 15)
    const auto byte = static_cast<std::uint8_t>(s.spell == 0x3B ? caster->direction : static_cast<std::uint32_t>(s.power));
    world.broadcasts().push_back({{0x9E, kind, byte, lo(a), hi(a), lo(b), hi(b)}, true, 0});
    if (start) {
        markMinimapForAll(world, *caster, 2);
        markMinimapForAll(world, *target, 2);
    } else {
        unmarkMinimapForAll(world, *caster, 2);
        unmarkMinimapForAll(world, *target, 2);
    }
}

bool Spells::startLasting(std::uint32_t spell, ServerObject* source, ServerObject* caster, ServerObject* origin, ServerObject* target,
                          float x, float y, int power, std::uint32_t frames, bool noFunctions)
{
    // A lasting spell's start.
    const bool glyph = origin && origin->typeName == "Glyph";
    // Begins only with a live caster, or with no origin given, or from a glyph.
    if (!((caster && !(caster->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) || !origin || glyph)) {
        return false;
    }
    auto& list = world.durationSpells();
    if (caster) {
        // Chain Lightning and Plasma: one still running of its own stays.
        if ((spell == 0x3B || spell == 0x2B) && std::ranges::any_of(list, [&](const DurationSpell& s) {
                return !s.glyph && s.spell == spell && s.caster == caster->extent && !s.cancelled;
            })) {
            return true;
        }
        cancelLasting(spell, *caster);
    }
    // The cancelled ones go now.
    for (auto it = list.begin(); it != list.end();) {
        const auto next = std::next(it);
        if (it->cancelled) {
            finishLasting(it);
        }
        it = next;
    }
    DurationSpell s;
    s.serial = ++world.nextSpellSerial;
    s.spell = spell;
    s.power = power;
    s.caster = caster ? caster->extent : 0;
    s.source = source ? source->extent : 0;
    if (!glyph) {
        s.x = caster ? caster->x : 0;
        s.y = caster ? caster->y : 0;
    } else {
        s.glyph = true;
        s.glyphExtent = origin->extent;
        s.x = origin->x;
        s.y = origin->y;
    }
    s.target = target ? target->extent : 0;
    s.targetX = x;
    s.targetY = y;
    s.start = s.last = frame;
    s.end = frame + frames;
    s.noFunctions = noFunctions;
    list.push_front(s);
    // Its sound at its caster: a flag-4 spell's "on", else its cast.
    if (caster) {
        const things::SpellDef* d = things.findSpell(spell);
        playSound(world, spellSound(things, spell, d && (d->flags & 4u) ? 1 : 0), *caster);
    }
    if (!noFunctions && lastingStart(list.front()) != 0) {
        cancelDurationSpell(world, list.front());
        return false;
    }
    return true;
}

void Spells::cancelLasting(std::uint32_t spell, const ServerObject& caster)
{
    cancelLastingSpells(world, spell, caster);
}

void cancelLastingSpells(World& world, std::uint32_t spell, const ServerObject& caster)
{
    // spellCancelDurSpell: the same spell, or any summoning for a summoning.
    const auto summoning = [](std::uint32_t s) { return 0x4A < s && s < 0x73; };
    for (DurationSpell& s : world.durationSpells()) {
        if (s.caster == caster.extent && (s.spell == spell || (summoning(spell) && summoning(s.spell)))) {
            cancelDurationSpell(world, s);
        }
    }
}

void Spells::updateLasting()
{
    // spellCastByPlayer.
    auto& list = world.durationSpells();
    // (A spell's update may lead to a cast (damage: a generator's onHurt script), whose start ends
    // the spells cancelled meanwhile: the next one perhaps, or this one. So the next isn't held
    // across the update, and this one, held, is only marked ended then (finishLasting), let go
    // here once its update is over: the exe walks on through freed records.)
    for (auto it = list.begin(); it != list.end();) {
        DurationSpell& s = *it;
        if (s.cancelled) {
            const auto next = std::next(it);
            finishLasting(it);
            it = next;
            continue;
        }
        if (const ServerObject* c = s.caster ? world.byExtent(s.caster) : nullptr; !c || (c->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            s.caster = 0;
        }
        if (const ServerObject* o = s.source ? world.byExtent(s.source) : nullptr; !o || o->gone()) {
            s.source = 0;
        }
        // With its caster gone it ends (a glyph's goes on without one).
        if (s.caster == 0 && !s.glyph) {
            cancelDurationSpell(world, s);
        } else {
            if (const ServerObject* g = s.glyphExtent ? world.byExtent(s.glyphExtent) : nullptr; !g || g->gone()) {
                s.glyphExtent = 0;
            }
            // An end equal to its start (frames 0) is no set end: only its update ends it.
            s.walked = true;
            const bool over = (s.end != s.start && s.end <= frame) || lastingUpdate(s) != 0;
            s.walked = false;
            if (s.finished) {
                it = list.erase(it); // (ended by what its update led to: over already)
                continue;
            }
            if (over) {
                cancelDurationSpell(world, s);
            }
        }
        it = std::next(it);
    }
}

void Spells::finishLasting(std::list<DurationSpell>::iterator it)
{
    if (it->finished) {
        return; // (ended already, its update's walk still on it: let go there)
    }
    // Its end sound (2) at its caster, if it's still there.
    if (const ServerObject* c = it->caster ? world.byExtent(it->caster) : nullptr) {
        playSound(world, spellSound(things, it->spell, 2), *c);
    }
    lastingEnd(*it);
    if (ServerObject* c = it->caster ? world.byExtent(it->caster) : nullptr) {
        // Its caster idle again (a player's; not while berserking: with the abilities); a
        // monster casting it ends that cast.
        if (c->monster && 0 <= c->monster->top) {
            const auto& top = c->monster->stack[static_cast<std::size_t>(c->monster->top)];
            // A monster's CastDurationSpell action (0x14), its spell in args[0].
            if (top.action == 0x14 && top.args[0] == it->spell) {
                Fight f{world, rng, frame, &things, modifiers};
                Monsters{f}.pop(*c);
            }
        }
        if (c->player && !(c->flags & ObjFlag::Dead) && c->player->state != 0xD) {
            c->player->previousState = c->player->state;
            c->player->state = 0xD;
            c->player->stateFrame = frame;
            c->player->animFrame = 0;
        }
    }
    if (it->walked) {
        it->finished = true; // (its update's call is on it: updateLasting lets it go)
        return;
    }
    world.durationSpells().erase(it);
}

namespace {

/// A player's weapon of `subclass` that's casting (its use flag 4), or
/// null.
const ServerObject* staffCasting(World& world, const ServerObject& caster, std::uint32_t subclass)
{
    if (!caster.player || caster.player->currentWeapon == 0) {
        return nullptr;
    }
    const ServerObject* w = world.byExtent(caster.player->currentWeapon);
    return w && (w->subclasses & subclass) && w->wandCasting ? w : nullptr;
}

/// Both on one team (unitsHaveSameTeam).
bool teamed(const ServerObject& a, const ServerObject& b) { return a.team != 0 && a.team == b.team; }

/// Hurt in the last two frames.
bool justHurt(const ServerObject& o, std::uint32_t frame)
{
    return o.maxHealth != 0 && frame - o.hurtFrame < 2u;
}

/// A monster that has moved 5 from where it cast.
bool movedFrom(const ServerObject& o, float x, float y)
{
    return !(std::fabs(x - o.x) < 5.0f && std::fabs(y - o.y) < 5.0f);
}

} // namespace

// A lasting spell's own start, by spell: 0 when it began, else it failed (startLasting
// then cancels it).
int Spells::lastingStart(DurationSpell& s)
{
    const auto inform = [&](const ServerObject& p, std::string_view id) {
        if (p.player) { // (SrvNet_SendInformString)
            tell(p, informStringBytes(id));
        }
    };
    if (s.spell == 0x15) {  // Turn Undead
        // spellTurnUndeadCreate: its kill points (TurnUndeadKillPoints by its
        // power), and a ring of 43 UndeadKillers (every 6th of the 256 ways, at 4 a frame) from
        // the glyph (one's) or the caster, the caster's, each the spell's (its CollideData);
        // MSG_FX 0xA0 there.
        s.value = static_cast<int>(world.number("TurnUndeadKillPoints", static_cast<std::size_t>(std::max(s.power - 1, 0))));
        const ServerObject* from = s.glyph ? (s.glyphExtent ? world.byExtent(s.glyphExtent) : nullptr) : (s.caster ? world.byExtent(s.caster) : nullptr);
        const float fx = from ? from->x : s.x;
        const float fy = from ? from->y : s.y;
        for (int d = 0; d < 0x100; d += 6) {
            if (ServerObject* k = world.createAt(things, "UndeadKiller", fx, fy, frame)) {
                k->value = static_cast<int>(s.serial);
                k->owner = s.caster;
                k->direction = static_cast<std::uint16_t>(d);
                const auto [vx, vy] = directionVector(static_cast<std::uint8_t>(d));
                k->vx = vx * 4.0f;
                k->vy = vy * 4.0f; // (and its Float28, 0)
            }
        }
        pointFx(0xA0, fx, fy);
        return 0;
    }
    if (0x4A < s.spell && s.spell < 0x73) {  // the summon spells
        // Spell_SummonStart: not a glyph's; a player's needs the creature in its field
        // guide (a co-op or quest game) and room for it; where it comes (Spell_SummonFindSpawnPos
        // Spell_SummonFindSpawnPos: a player's at its cursor, at most 50 off, in sight (ray flags 9) and not where
        // teleports are barred; another's at the point if it sees it, else at itself), facing as
        // the caster; it comes in SummonDuration by its size (small, medium, large); the clients
        // shown (MSG_FX_SUMMON 0x7E).
        ServerObject* caster = s.caster ? world.byExtent(s.caster) : nullptr;
        const int index = static_cast<int>(s.spell) - 0x4A;
        if (!caster || (caster->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) || s.glyph) {
            return 1;
        }
        if (caster->player) {
            // (noxcpp: a mode's book, World::modeBook, asks for the guide as a quest does.)
            if (((world.gameFlags & (GameFlag::Coop | GameFlag::Quest)) || world.modeBook()) && !caster->player->fieldGuide[static_cast<std::size_t>(index)]) {
                inform(*caster, "Summon.c:NeedGuideToSummon");
                return 1;
            }
            if (!roomFor(world, things, *caster, index)) {
                inform(*caster, "Summon.c:CreatureControlFailed");
                return 1;
            }
        }
        float px = s.targetX;
        float py = s.targetY;
        if (!caster->player) {
            if (!world.traceRay(caster->x, caster->y, px, py, 9)) {  // ray flags 9: Doors | NearWindows
                px = caster->x;
                py = caster->y;
            }
        } else {
            // (Spell_SummonFindSpawnPos: x's difference kept on the stack, y's stored for its
            // square; the distance on the stack against 50, then stored.)
            const X87 dx = X87(px) - caster->x;
            const X87 dy53 = X87(py) - caster->y;
            const float dy = dy53.f();
            const X87 d53 = util::sqrt(dy53 * dy + dx * dx);
            const float d = d53.f();
            if (X87(50.0f) < d53) {
                px = (dx * 50.0f / d + caster->x).f();
                py = (X87(dy) * 50.0f / d + caster->y).f();
            }
            if (!world.traceRay(caster->x, caster->y, px, py, 9) || world.noTeleportAt(px, py)) {
                tell(*caster, {0xA9, 0, 2, 0, 0, 0}); // (MSG_INFORM 0, 2: "UnseenTarget")
                return 1;
            }
        }
        const char* name = guideName(index);
        const things::ThingDef* def = name ? things.findThing(name) : nullptr;
        const auto typeId = static_cast<std::uint16_t>(def ? def - things.things.data() + 1 : 0);
        s.summonType = name ? name : "";
        s.summonX = px;
        s.summonY = py;
        s.summonDir = static_cast<std::uint8_t>(caster->direction);
        s.summonId = world.nextSummonId;
        world.nextSummonId = static_cast<std::uint16_t>(world.nextSummonId + 1);
        // Summon ids wrap after 64999.
        if (64999 < world.nextSummonId) {
            world.nextSummonId = 0;
        }
        s.summoned = false;
        const int size = guideSize(things, index);
        const int duration = static_cast<int>(world.number("SummonDuration", size == 1 ? 0 : size == 2 ? 1 : 2));
        s.end = frame + static_cast<std::uint32_t>(duration);
        // MSG_FX_SUMMON (0x7E): the point, the summon id, the creature's type (16 bits each), its
        // facing, then the time it takes (16 bits).
        const auto ix = static_cast<std::uint16_t>(static_cast<int>(px));
        const auto iy = static_cast<std::uint16_t>(static_cast<int>(py));
        world.broadcasts().push_back({{0x7E, lo(ix), hi(ix), lo(iy), hi(iy), lo(s.summonId), hi(s.summonId), lo(typeId), hi(typeId), s.summonDir,
                                       lo(static_cast<std::uint32_t>(duration)), hi(static_cast<std::uint32_t>(duration))},
                                      true, 0});
        return 0;
    }
    if (s.spell == 0x09) {  // Charm
        // Spell_CharmStart: a glyph's confuses its target instead. Else the creature
        // nearest the point within 300 (spellFlySearchTarget): a monster not its
        // own already, in the guide (a player's: in its field guide too); charmed (CHARMING) for
        // Charm(Small|Medium|Large)Duration by its size, the beam shown.
        ServerObject* caster = s.caster ? world.byExtent(s.caster) : nullptr;
        if (s.glyph) {
            if (ServerObject* t = s.target ? world.byExtent(s.target) : nullptr) {
                enchant(world, *t, Enchant::Confused, static_cast<int>(world.number("ConfuseEnchantDuration")), s.power);
                // (on the caster and target: with the glyphs.)
            }
            return 1;
        }
        if (!caster) {
            return 1;
        }
        const things::SpellDef* sd = things.findSpell(s.spell);
        const float at[2] = {s.targetX, s.targetY};
        ServerObject* t = searchTarget(world, at, caster, sd ? sd->flags : 0, 300.0f, 0, caster);
        s.target = t ? t->extent : 0;
        if (!t) {
            inform(*caster, "Summon.c:CharmNoCreatureCloseEnough");
            playSound(world, 0x10, *caster);
            return 1;
        }
        if (!(t->classes & oc::Monster) || controls(world, *caster, *t)) {
            s.target = 0;
            playSound(world, 0x10, *caster);
            return 1;
        }
        if (t->monster && t->monster->notCharmable) { // (noxcpp: a mode's, nox.setCharmable)
            inform(*caster, "Summon.c:CreatureNotCharmable");
            playSound(world, 0x10, *caster);
            s.target = 0;
            return 1;
        }
        const int index = guideIndex(t->typeName);
        if (caster->player) {
            if (index == 0) {
                inform(*caster, "Summon.c:CreatureNotCharmable");
                playSound(world, 0x10, *caster);
                s.target = 0;
                return 1;
            }
            if (!caster->player->fieldGuide[static_cast<std::size_t>(index)]) {
                s.target = 0;
                inform(*caster, "Summon.c:NeedGuideToCharm");
                playSound(world, 0x10, *caster);
                return 1;
            }
        }
        const int size = guideSize(things, index);
        const char* table = size == 1 ? "CharmSmallDuration" : size == 2 ? "CharmMediumDuration" : "CharmLargeDuration";
        const int duration = static_cast<int>(world.number(table, static_cast<std::size_t>(std::max(s.power - 1, 0))));
        s.end = frame + static_cast<std::uint32_t>(duration);
        enchant(world, *t, Enchant::Charming, duration + 1, 5);
        rayFx(world, s, true, s.target);
        return 0;
    }
    switch (s.spell) {
    case 0x33: { // castShield1
        ServerObject* t = s.target ? world.byExtent(s.target) : nullptr;
        if (!t || (t->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            return 1;
        }
        // Not on a large monster (subclass 4).
        if ((t->classes & oc::Monster) && (t->subclasses & 4u)) {
            return 1;
        }
        if (!(t->classes & (oc::Monster | oc::Player))) {
            return 1;
        }
        const int frames = static_cast<int>(world.number("ShieldDuration", static_cast<std::size_t>(s.power - 1)));
        enchant(world, *t, Enchant::Shield, frames, s.power);
        s.end = frame + static_cast<std::uint32_t>(frames);
        s.value = static_cast<int>(world.number("ShieldHealth", static_cast<std::size_t>(s.power - 1)));
        return 0;
    }
    case 0x23: { // Spell_GreaterHealOnStart
        ServerObject* caster = s.caster ? world.byExtent(s.caster) : nullptr;
        const things::SpellDef* def = things.findSpell(s.spell);
        const std::uint32_t flags = def ? def->flags : 0;
        if (!caster && !s.glyph) {
            return 1;
        }
        const float at[2] = {s.targetX, s.targetY};
        if (caster && !s.glyph) {
            // The nearest creature to where it was cast (itself too), within 400.
            ServerObject* t = searchTarget(world, at, caster, flags, 400.0f, 1, caster);
            s.target = t ? t->extent : 0;
            if (!t) {
                tell(*caster, informStringBytes("ExecDur.c:GreaterHealNoTarget"));
                return 1;
            }
            if (isEnemy(world, *caster, *t)) {
                return 1;
            }
            rayFx(world, s, true, s.target);
            return 0;
        }
        // A glyph's (or one whose caster's gone): 20 at once to the nearest, and done.
        if (ServerObject* t = searchTarget(world, at, nullptr, flags, 400.0f, 1, nullptr)) {
            addHealth(*t, 20);
        }
        return 1;
    }
    case 0x18: // spellEnergyBoltStop: Chain Lightning's off; a flash where it's cast
        if (const ServerObject* caster = s.caster ? world.byExtent(s.caster) : nullptr) {
            cancelLastingSpells(world, 0x2B, *caster);
        }
        pointFx(0x82, s.x, s.y);
        return 0;
    case 0x36: { // Moonglow (spellCreateMoonglow)
        // (Its duration's name is one gamedata hasn't: 0, for good; the light goes with its
        // Moonglow at 300 s. A non-player's LIGHT is put on and the spell ends at once, taking
        // it off again: an original bug.)
        const int frames = static_cast<int>(world.number("MoonglowEnchantmentDuration"));
        ServerObject* t = s.target ? world.byExtent(s.target) : nullptr;
        if (!t || (t->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            return 1;
        }
        if (!t->player) {
            enchant(world, *t, Enchant::Light, frames, s.power);
            return 1;
        }
        if (!things.findThing("Moonglow")) {
            return 1;
        }
        const bool aimed = (t->player->statusFlags & 0x10u) != 0;
        ServerObject& glow = world.spawn(things, "Moonglow", aimed ? t->player->cursorX : 2944.0f, aimed ? t->player->cursorY : 2944.0f);
        glow.owner = t->extent;
        glow.born = frame;
        glow.touch();
        s.value = static_cast<int>(glow.extent);
        enchant(world, *t, Enchant::Moonglow, frames, s.power);
        return 0;
    }
    case 0x84: { // Wall (Spell_WallCreate)
        ServerObject* caster = s.caster ? world.byExtent(s.caster) : nullptr;
        if (!caster || (caster->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            return 1;
        }
        if (!world.traceRay(s.x, s.y, s.targetX, s.targetY, Trace::Doors | Trace::NearWindows)) {
            if (caster->player) {
                tell(*caster, {0xA9, 0, 2, 0, 0, 0});
            }
            return 1;
        }
        // Its line across the way it was cast (the 8-way of the cast's direction),
        // centred on the target's cell (an even one), power's length each way.
        // Its way by the direction routine of the differences, each stored (directionOf: FPATAN on the
        // x87, + 2 pi, x 256 / 2 pi, + 0.5, stored, Math_FloatToInt).
        const int way = directionOf((X87(s.targetX) - s.x).f(), (X87(s.targetY) - s.y).f());
        const auto [gx, gy] = gridDirection(way);
        const int code = gy * 3 + 4 + gx;
        int cx = static_cast<int>(s.targetX) / 23;
        const int cy = static_cast<int>(s.targetY) / 23;
        // The line's centre must be a cell whose x + y is even, as the wall grid's are.
        if (((cx + cy) & 1) == 1) {
            ++cx;
        }
        // Cells each way by power: 3 at power 1 up to 7 at 5.
        static constexpr int kLengths[6] = {7, 3, 4, 5, 6, 7};
        const int length = kLengths[std::clamp(s.power, 0, 5)];
        if (raiseWall(s, cx, cy, wallShapeFor(code))) {
            for (int side = 0; side < 2; ++side) {
                int x = cx;
                int y = cy;
                for (int i = 0; i < length; ++i) {
                    x = wallStepX(code, cx, x, side != 0);
                    y = wallStepY(code, cy, y, side != 0);
                    if (!raiseWall(s, x, y, wallCornerShape(code, cx, cy, x, y))) {
                        break;
                    }
                }
            }
        }
        s.end = frame + static_cast<std::uint32_t>((s.power + 5) * 30 * 60);  // power + 5 minutes
        return 0;
    }
    case 0x74: { // Tag (Spell_TagOnStart): a live player's mark on a live target, shown
        // on its map (MSG_INTERESTING_ID 0xD2, add, kind 1)
        ServerObject* caster = s.caster ? world.byExtent(s.caster) : nullptr;
        ServerObject* t = s.target ? world.byExtent(s.target) : nullptr;
        if (!caster || (caster->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) || !caster->player || !t ||
            (t->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            return 1;
        }
        s.end = frame + static_cast<std::uint32_t>(world.number("TagDurationPerLevel")) * static_cast<std::uint32_t>(s.power);
        markMinimap(world, *caster, *t, 1); // (netMarkMinimapObject)
        tell(*caster, {0xD2, lo(t->extent), hi(t->extent), lo(t->type), hi(t->type), 1, 1});
        return 0;
    }
    case 0x1F: { // Force of Nature: its charge glowing where it's cast from, the
        // caster's facing kept; from a player's staff of it (subclass 0x200000, casting), marked so
        const ServerObject* from = s.glyph ? world.byExtent(s.glyphExtent) : world.byExtent(s.caster);
        if (!s.glyph && from && staffCasting(world, *from, 0x200000u)) {
            s.fromStaff = true;
        }
        if (!from) {
            return 0;
        }
        s.value = from->direction;
        if (things.findThing("ForceOfNatureCharge")) {
            ServerObject& charge = world.spawn(things, "ForceOfNatureCharge", from->x, from->y);
            charge.born = frame;
            charge.touch();
            s.object = charge.extent;
        }
        return 0;
    }
    case 0x38: { // Mana Bomb (manaBomb): a player caster held, anchored and
        // anti-magic for 10 s (power 5); a glyph's goes off after ManaBombGlyphDuration
        s.value = static_cast<int>(world.number("ManaBombInitPower", static_cast<std::size_t>(s.power - 1)));
        s.object = 0;
        s.burst = false;
        ServerObject* caster = s.caster ? world.byExtent(s.caster) : nullptr;
        if ((!caster || caster->player) && !s.glyph) {
            if (caster) {
                enchant(world, *caster, Enchant::Held, 30 * 10, 5);
                enchant(world, *caster, Enchant::Anchored, 30 * 10, 5);
                enchant(world, *caster, Enchant::AntiMagic, 30 * 10, 5);
            }
        } else {
            s.end = frame + static_cast<std::uint32_t>(world.number("ManaBombGlyphDuration"));
        }
        if (things.findThing("ManaBombCharge")) {
            ServerObject& charge = world.spawn(things, "ManaBombCharge", s.x, s.y);
            charge.born = frame;
            charge.touch();
            s.object = charge.extent;
        }
        return 0;
    }
    case 0x43: { // Oval Shield: a player's, power x 20 s; its own hostile lasting
        // spells end first
        ServerObject* t = s.target ? world.byExtent(s.target) : nullptr;
        if (!t || !t->player) {
            return 1;
        }
        endHostileLasting(*t);
        const int frames = 30 * s.power * 20;
        enchant(world, *t, Enchant::ReflectiveShield, frames, s.power);
        s.end = frame + static_cast<std::uint32_t>(frames);
        return 0;
    }
    case 0x04: // Blink (spellBlink2): a quest's glyph can't
        if (world.quest() && s.glyph) {
            return 1;
        }
        teleportDelay(s);
        return 0;
    case 0x7A: // Teleport to Mark 1-4: a player's needs that mark
    case 0x7B:
    case 0x7C:
    case 0x7D: {
        const ServerObject* source = s.source ? world.byExtent(s.source) : nullptr;
        if (source && (source->classes & oc::Player) && (!source->player || source->player->marks[s.spell - 0x7A] == 0)) {
            return 1;
        }
        teleportDelay(s);
        return 0;
    }
    case 0x7E: { // Teleport to Target: itself if no other; not into water
        // that can't be reached, nor through a wall (flags 9)
        if (s.target == 0) {
            s.target = s.caster;
        }
        ServerObject* t = s.target ? world.byExtent(s.target) : nullptr;
        ServerObject* caster = s.caster ? world.byExtent(s.caster) : nullptr;
        if (!t) {
            return 1;
        }
        if (world.noTeleportAt(s.targetX, s.targetY)) {
            world.textTo(*t, "ExecDur.c:UnseenTarget");
            playSound(world, 0xE7, *t);
            return 1;
        }
        if (caster && !world.traceRay(t->x, t->y, s.targetX, s.targetY, Trace::Doors | Trace::NearWindows)) {
            if (caster->player) {
                tell(*caster, {0xA9, 0, 2, 0, 0, 0});
            }
            return 1;
        }
        teleportDelay(s);
        return 0;
    }
    case 0x73: { // Swap: with a live other one, not a monster of subclass 0x4000
        const ServerObject* t = s.target ? world.byExtent(s.target) : nullptr;
        if (s.caster == 0 || !t || (t->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) || t->extent == s.caster ||
            ((t->classes & oc::Monster) && (t->subclasses & 0x4000u))) {
            return 1;
        }
        teleportDelay(s);
        return 0;
    }
    case 0x2B: // onStartLightning: Lightning's off; a flash where it's cast. A
        // player's lightning staff casting it (its weapon, subclass 0x40000, casting) pays in
        // its charges, not mana.
        s.carry = 0;
        s.staff = 0;
        if (const ServerObject* caster = s.caster ? world.byExtent(s.caster) : nullptr) {
            cancelLastingSpells(world, 0x18, *caster);
            if (const ServerObject* w = !s.glyph ? staffCasting(world, *caster, 0x40000u) : nullptr) {
                s.staff = w->extent;
                s.fromStaff = true;
            }
        }
        pointFx(0x81, s.x, s.y);
        return 0;
    case 0x3B: { // Plasma (plasmaSmth): only a player's from its Oblivion Orb
        // (subclass 0x4000000, casting: it pays in its charges); MSG_FX 0x83 where it's cast
        s.value = 0;
        s.staff = 0;
        s.target = 0;
        pointFx(0x83, s.x, s.y);
        const ServerObject* caster = s.caster ? world.byExtent(s.caster) : nullptr;
        if (caster && caster->player) {
            const ServerObject* w = caster->player->currentWeapon ? world.byExtent(caster->player->currentWeapon) : nullptr;
            if (!w || !(w->subclasses & 0x4000000u)) {
                return 1;
            }
            if (w->wandCasting) {
                s.staff = w->extent;
                s.fromStaff = true;
            }
        }
        return 0;
    }
    default:
        return 0;
    }
}

int Spells::wallShapeFor(int code)
{
    // The wall shape across an 8-way direction.
    switch (code) {
    case 1: return 7;
    case 2: case 6: return 1;
    case 3: return 8;
    case 5: return 10;
    case 7: return 9;
    default: return 0;
    }
}

int Spells::wallCornerShape(int code, int cx, int cy, int x, int y)
{
    // A diagonal line's cells off its centre's row or column turn the other way.
    if ((code == 1 && y != cy) || (code == 7 && y != cy)) {
        return wallShapeFor(code == 1 ? 7 : 1);
    }
    if ((code == 3 && x != cx) || (code == 5 && x != cx)) {
        return wallShapeFor(code == 3 ? 5 : 3);
    }
    return wallShapeFor(code);
}

int Spells::wallStepX(int code, int cx, int x, bool back)
{
    // The next cell's x along the line, one way (back: the other).
    if (back) {
        static constexpr int kMirror[9] = {8, 7, 6, 3, 4, 5, 2, 1, 0};
        if (code == 3 || code == 5) { // (3 and 5 mirror to themselves; 4 stays)
            code = code == 3 ? 3 : 5;
        } else if (0 <= code && code < 9) {
            code = kMirror[code];
        }
    }
    switch (code) {
    case 0: case 1: case 2: return x + 1;
    case 3: return x != cx ? x - 1 : x + 1;
    case 5: return x == cx ? x - 1 : x + 1;
    case 6: case 7: case 8: return x - 1;
    default: return x;
    }
}

int Spells::wallStepY(int code, int cy, int y, bool back)
{
    // The next cell's y along the line.
    if (back) {
        static constexpr int kMirror[9] = {8, 1, 6, 5, 4, 3, 2, 7, 0};
        if (0 <= code && code < 9) {
            code = kMirror[code];
        }
    }
    switch (code) {
    case 0: return y - 1;
    case 1: return y == cy ? y - 1 : y + 1;
    case 2: return y + 1;
    case 3: return y - 1;
    case 5: return y + 1;
    case 6: return y - 1;
    case 7: return y == cy ? y + 1 : y - 1;
    case 8: return y + 1;
    default: return y;
    }
}

bool Spells::raiseWall(DurationSpell& s, int x, int y, int shape)
{
    // A magic wall in an empty cell (breakable, its material's health; a straight
    // one's look by its x); a wall there already (not magic or invisible, not secret, breakable
    // or a door's) takes the new shape into its own and stops the line.
    // The shape a wall takes when a magic wall joins it: [its own shape][the new one's].
    static constexpr std::uint8_t kCombine[13][13] = {
        {0x00, 0x02, 0x02, 0x03, 0x02, 0x05, 0x02, 0x03, 0x05, 0x05, 0x03, 0x0B, 0x02},
        {0x02, 0x01, 0x02, 0x02, 0x04, 0x02, 0x06, 0x04, 0x04, 0x06, 0x06, 0x02, 0x0C},
        {0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02},
        {0x03, 0x02, 0x02, 0x03, 0x02, 0x02, 0x02, 0x03, 0x02, 0x02, 0x03, 0x03, 0x02},
        {0x02, 0x04, 0x02, 0x02, 0x04, 0x02, 0x02, 0x04, 0x04, 0x02, 0x02, 0x02, 0x04},
        {0x05, 0x02, 0x02, 0x02, 0x02, 0x05, 0x02, 0x02, 0x05, 0x05, 0x02, 0x05, 0x02},
        {0x02, 0x06, 0x02, 0x02, 0x02, 0x02, 0x06, 0x02, 0x02, 0x06, 0x06, 0x02, 0x06},
        {0x03, 0x04, 0x02, 0x03, 0x04, 0x02, 0x02, 0x07, 0x04, 0x02, 0x03, 0x03, 0x04},
        {0x05, 0x04, 0x02, 0x02, 0x04, 0x05, 0x02, 0x04, 0x08, 0x05, 0x02, 0x05, 0x04},
        {0x05, 0x06, 0x02, 0x02, 0x02, 0x05, 0x06, 0x02, 0x05, 0x09, 0x06, 0x05, 0x06},
        {0x03, 0x06, 0x02, 0x03, 0x02, 0x02, 0x06, 0x03, 0x02, 0x06, 0x0A, 0x03, 0x06},
        {0x0B, 0x02, 0x02, 0x03, 0x02, 0x05, 0x02, 0x03, 0x05, 0x05, 0x03, 0x0B, 0x02},
        {0x02, 0x0C, 0x02, 0x02, 0x04, 0x02, 0x06, 0x04, 0x04, 0x06, 0x06, 0x02, 0x0C},
    };
    const int magic = world.wallMaterial("MagicWallSystemUseOnly");
    const int invisible = world.wallMaterial("InvisibleWallSet");
    const int blocking = world.wallMaterial("InvisibleBlockingWallSet");
    MagicWall record;
    record.spell = s.serial;
    record.x = x;
    record.y = y;
    WallCell* there = world.gridWall(x, y);
    if (!there) {
        // Only within the map's 256 x 256 wall grid.
        if (x < 0 || 0x100 <= x || y < 0 || 0x100 <= y) {
            return false;
        }
        WallCell& c = world.createWall(x, y); // (Map_CreateWallAt: the cell's own if it has one)
        c.material = static_cast<std::uint8_t>(magic);
        c.direction = static_cast<std::uint8_t>(shape);
        record.direction = c.direction; // (the wall's shape now)
        const int looks = world.wallVariations(magic);
        c.variant = (shape == 0 || shape == 1) && looks != 0 ? static_cast<std::uint8_t>(x % looks) : 0;
        c.destructible = true;
        c.health = world.wallHealth(magic);
        world.magicWalls().push_back(record);
        return true;
    }
    if (there->material == magic || there->material == invisible || there->material == blocking) {
        return false;
    }
    if (there->secret || there->destructible || there->door != 0) {
        return false;
    }
    record.replaced = true;
    record.oldMaterial = there->material;
    record.oldDirection = there->direction;
    record.oldVariant = there->variant;
    there->direction = there->direction < 13 && shape < 13 ? kCombine[there->direction][shape] : there->direction;
    there->variant = 0;
    record.direction = there->direction;
    world.magicWalls().push_back(record);
    return false;
}

bool restoreMagicWalls(World& world, const ServerObject& unit)
{
    std::uint32_t serial = 0;
    if (!world.pendingMagicWalls.empty()) {
        // (spellCastedFirst on: the first Wall it cast, not cancelled)
        DurationSpell* wall = nullptr;
        for (DurationSpell& s : world.durationSpells()) {
            if (s.spell == 0x84 && s.caster == unit.extent && !s.cancelled) {
                wall = &s;
                break;
            }
        }
        if (!wall) {
            return false;
        }
        wall->noFunctions = false; // (Spell_WallCreate, spellWallUpdate, spellWallDestroy)
        wall->target = 0;
        serial = wall->serial;
    }
    for (const World::PendingWall& p : world.pendingMagicWalls) {
        WallCell* c = world.gridWall(p.x, p.y);
        if (!c) {
            c = &world.createWall(p.x, p.y); // (Map_CreateWallAt)
            c->destructible = true;           // (its flags | 8)
            c->material = p.material;
            c->variant = p.variant;
            c->direction = p.direction;
            c->health = p.health;
        } else {
            c->direction = p.direction;
        }
        // The record at the list's head.
        MagicWall m;
        m.spell = serial;
        m.x = p.x;
        m.y = p.y;
        m.replaced = p.replaced != 0;
        m.oldMaterial = p.oldMaterial;
        m.oldDirection = p.oldDirection;
        m.oldVariant = p.oldVariant;
        m.direction = c->direction;
        world.magicWalls().push_back(m);
    }
    world.pendingMagicWalls.clear();
    return true;
}

void Spells::endHostileLasting(const ServerObject& o)
{
    // The hostile (flag 0x20) lasting spells it cast end.
    for (DurationSpell& s : world.durationSpells()) {
        const things::SpellDef* d = things.findSpell(s.spell);
        if (s.caster == o.extent && d && (d->flags & kHostile)) {
            cancelDurationSpell(world, s);
        }
    }
}

void Spells::teleportDelay(DurationSpell& s)
{
    // A solo game's teleports wait TeleportDelay at their power; elsewhere they go the next
    // pass (their update acts at the end's frame less one).
    if (world.gameFlags & GameFlag::Solo) {
        s.end = frame + static_cast<std::uint32_t>(world.number("TeleportDelay", static_cast<std::size_t>(s.power - 1)));
    } else {
        s.end = frame + 1;
    }
}

// The teleports' frame (Blink, Teleport to Mark 1-4, Teleport to Target, Swap): each waits for
// the frame before its end (0), then moves its target once and is done (1). Anchored ones stay.
int Spells::teleportSpell(DurationSpell& s, ServerObject* caster, ServerObject* target)
{
    ServerObject* source = s.source ? world.byExtent(s.source) : nullptr;
    // The flashes where it went from and to (0x89), unless it's invisible.
    const auto shown = [&](const ServerObject& o) { return !enchanted(o, Enchant::Invisible); };
    // Anchored: it stays, with the "can't" at it (0xE7).
    const auto anchored = [&](const ServerObject& o) {
        if (!enchanted(o, Enchant::Anchored)) {
            return false;
        }
        playSound(world, 0xE7, o);
        return true;
    };
    // The spell's sound (its cast, the marks' on) where it lands: for everyone, an invisible
    // player's to itself alone, an invisible creature's not at all.
    const auto landed = [&](const ServerObject& o, int which) {
        const std::string_view id = spellSound(things, s.spell, which);
        if (shown(o)) {
            playSound(world, id, o);
        } else if (o.classes & oc::Player) {
            playSound(world, id, o, 2, o.extent);
        }
    };
    switch (s.spell) {
    case 0x04: { // Blink (spellBlink1): to a random waypoint, else a player start
        if (!target || (target->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            return 1;
        }
        // The teleports act on the frame before their end (teleportDelay sets it).
        if (s.end - 1 != frame) {
            return 0;
        }
        if (anchored(*target)) {
            return 1;
        }
        float tx = 0;
        float ty = 0;
        // A quest player's near its checkpoint (within 60) or at a player start; else a
        // random waypoint, or a player start.
        if (world.quest() && (target->classes & oc::Player) && target->player) {
            const ServerObject* gate = target->player->soulGate ? world.byExtent(target->player->soulGate) : nullptr;
            if (gate) {
                std::tie(tx, ty) = clearPointNear(world, rng, gate->x, gate->y, 60.0f);
            } else {
                std::tie(tx, ty) = findPlayerStart(world, target, rng);
            }
        } else if (!randomWaypoint(world, rng, target, tx, ty)) {
            std::tie(tx, ty) = findPlayerStart(world, target, rng);
        }
        teleportWake(world, things, *target, tx, ty, frame);
        pointFx(0x89, target->x, target->y);
        playSound(world, spellSound(things, s.spell, 0), *target);
        if (shown(*target)) {
            pointFx(0x89, target->x, target->y);
            pointFx(0x89, tx, ty);
        }
        teleport(world, *target, tx, ty, frame);
        if (shown(*target)) {
            pointFx(0x89, target->x, target->y);
        }
        landed(*target, 0);
        teleportedBy(caster, target, frame);
        return 1;
    }
    case 0x7A: // Teleport to Mark: its caster's (source's) mark, which wears out
    case 0x7B:
    case 0x7C:
    case 0x7D: {
        if (!source || (source->flags & ObjFlag::Destroyed) || !target || (target->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            return 1;
        }
        if (target != source && !s.glyph) {
            return 1;
        }
        if (s.end - 1 != frame) {
            return 0;
        }
        if (anchored(*target)) {
            return 1;
        }
        if (source->player) {
            const auto slot = static_cast<std::size_t>(s.spell - 0x7A);
            ServerObject* mark = source->player->marks[slot] ? world.byExtent(source->player->marks[slot]) : nullptr;
            if (!mark) {
                return 1;
            }
            const float fromX = target->x;
            const float fromY = target->y;
            teleportWake(world, things, *target, mark->x, mark->y, frame);
            playSound(world, spellSound(things, s.spell, 1), *target);
            teleport(world, *target, mark->x, mark->y, frame);
            if (shown(*target)) {
                pointFx(0x89, fromX, fromY);
                pointFx(0x89, target->x, target->y);
            }
            landed(*target, 1);
            mark->timer = frame; // (last used)
            if (--source->player->markUses[slot] == 0) {
                pointFx(0x81, mark->x, mark->y);
                mark->pendingDelete = true;
                source->player->marks[slot] = 0;
            }
        }
        if (caster && !(caster->flags & ObjFlag::Destroyed)) {
            teleportedBy(caster, target, frame);
        }
        return 1;
    }
    case 0x7E: { // Teleport to Target (castTTT)
        if (!target || !caster || (target->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) || (caster->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            return 1;
        }
        if (s.end - 1 != frame) {
            return 0;
        }
        if (anchored(*target)) {
            return 1;
        }
        pointFx(0x89, target->x, target->y);
        playSound(world, spellSound(things, s.spell, 0), *target);
        teleportWake(world, things, *target, s.targetX, s.targetY, frame);
        teleport(world, *target, s.targetX, s.targetY, frame);
        if (shown(*target)) {
            pointFx(0x89, target->x, target->y);
        }
        landed(*target, 0);
        teleportedBy(caster, target, frame);
        return 1;
    }
    case 0x73: { // Swap (Spell_SwapOnUpdate)
        if (!target || !caster || (target->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) || (caster->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            return 1;
        }
        if (s.end - 1 != frame) {
            return 0;
        }
        if (anchored(*target) || anchored(*caster)) {
            return 1;
        }
        if (!s.glyph) { // it must see it, and a player have it on its screen
            const auto refuse = [&] {
                tell(*caster, informStringBytes("ExecDur.c:NeedClearLOSForSwap"));
                return 1;
            };
            if (!canInteract(world, *caster, *target)) {
                return refuse();
            }
            if (caster->player) {
                const PlayerControl& p = *caster->player;
                if (!(caster->x - p.viewHalfWidth <= target->x && target->x <= caster->x + p.viewHalfWidth &&
                      caster->y - p.viewHalfHeight <= target->y && target->y <= caster->y + p.viewHalfHeight)) {
                    return refuse();
                }
            }
        }
        const float tx = target->x;
        const float ty = target->y;
        teleport(world, *target, caster->x, caster->y, frame);
        teleport(world, *caster, tx, ty, frame);
        if (shown(*target) && shown(*caster)) {
            pointFx(0x89, caster->x, caster->y);
            pointFx(0x89, target->x, target->y);
        }
        playSound(world, spellSound(things, s.spell, 0), *target);
        playSound(world, spellSound(things, s.spell, 0), *caster);
        teleportedBy(caster, target, frame);
        return 1;
    }
    default:
        return 1;
    }
}

ServerObject* Spells::chainTarget(float x, float y, float range, ServerObject& from, const ServerObject* owner,
                                  const std::vector<ServerObject*>& struck)
{
    // Within `range` of (x, y), each by lightningCanAttackCheck: a creature, an
    // enemy of the caster (not it), not one struck already, that `from` sees; the nearest to
    // `from`.
    ServerObject* best = nullptr;
    // (The nearest so far kept as a float, from the reach squared, stored; each distance squared
    // on the stack against it. A hop's starts from the whole range's square, or its square's
    // square for the fourth hop: no different, the circle's own test being the tighter.)
    float bestD2 = (X87(range) * range).f();
    world.unitsInCircle(x, y, range, [&](ServerObject& o) { // (unitsGetInCircle, by the grid)
        if (o.pendingDelete) { // (0x8020: ours' 0x20)
            return;
        }
        if (!(o.classes & (oc::Monster | oc::Player | oc::MonsterGenerator)) || (owner && !isEnemy(world, *owner, o))) {
            return;
        }
        // Not a NO_SPELL_TARGET monster (subclass 0x8000).
        if (((o.classes & oc::Monster) && (o.subclasses & 0x8000u)) || (o.flags & (ObjFlag::Dead | ObjFlag::Destroyed)) || &o == &from ||
            &o == owner || std::ranges::find(struck, &o) != struck.end()) {
            return;
        }
        const X87 dx = X87(o.x) - from.x;
        const X87 dy = X87(o.y) - from.y;
        const X87 d2 = dx * dx + dy * dy;
        if (canInteract(world, from, o) && d2 < X87(bestD2)) {
            best = &o;
            bestD2 = d2.f();
        }
    });
    return best;
}

namespace {

/// The lasting drains' first test (onFrameLightning, spellDrainMana, spellEnergyBoltTick): ended
/// when the caster is gone (but a glyph's, which goes on without one) or running.
bool casterGoneOrRunning(const DurationSpell& s, const ServerObject* caster)
{
    return caster ? enchanted(*caster, Enchant::Run) : !s.glyph;
}

} // namespace

// Chain Lightning's frame. A glyph's strikes every enemy in range once. A caster's chains up to
// its power (1-5) bolts from it through the nearest enemies, LightningDamage a frame each, paid in
// mana or its staff's charges; 0 keeps it going, 1 ends it.
int Spells::chainLightning(DurationSpell& s, ServerObject* caster)
{
    // onFrameLightning.
    if (casterGoneOrRunning(s, caster)) {
        return 1;
    }
    const float range = world.number("LightningRange");
    Fight fight{world, rng, frame, &things, modifiers};
    if (s.glyph) { // a glyph's: every enemy within range it sees (flags 9) struck once
        world.unitsInCircle(s.x, s.y, range, [&](ServerObject& o) { // (by the grid, each struck as it's found)
            if (o.pendingDelete || &o == caster) { // (lightningSpellTrapEffect's 0x8020: ours' 0x20)
                return;
            }
            if (!(o.classes & (oc::Monster | oc::Player)) || (o.flags & (ObjFlag::Dead | ObjFlag::Destroyed)) ||
                ((o.classes & oc::Monster) && (o.subclasses & 0x8000u)) || (caster && !isEnemy(world, *caster, o))) {
                return;
            }
            if (world.traceRay(s.x, s.y, o.x, o.y, Trace::Doors | Trace::NearWindows)) {
                fight.damage(o, nullptr, 0, static_cast<int>(world.number("LightningGlyphDamage")), 17);
                pointFx(0x81, o.x, o.y);
                playSound(world, spellSound(things, 0x2B, 0), o); // (Chain Lightning's, at each struck)
            }
        });
        return 1;
    }
    // It stops when the player is out of mana, when the caster is hurt (after its first 2
    // frames), or when a monster caster has moved from where it cast.
    if (caster->player && caster->player->mana == 0) {
        return 1;
    }
    if (2u < frame - s.start && justHurt(*caster, frame)) {
        return 1;
    }
    if ((caster->classes & oc::Monster) && movedFrom(*caster, s.x, s.y)) {
        return 1;
    }
    // Last frame's bolts kept, so only the beams that changed are sent below.
    s.lastBolts = std::move(s.bolts);
    s.bolts.clear();
    const int most = std::clamp(s.power, 1, 5); // (its power)
    std::vector<ServerObject*> struck;
    ServerObject* first = nullptr;
    // The first link: the enemy under a player's cursor within range, else the nearest it sees.
    if (caster->player && caster->player->cursorObject != 0) {
        ServerObject* under = world.byExtent(caster->player->cursorObject);
        if (under && isEnemy(world, *caster, *under) && edgeDistance53(*caster, *under) <= X87(range)) {
            first = under;
        }
    }
    if (!first) {
        first = chainTarget(s.x, s.y, range, *caster, caster, struck);
    }
    // No one to strike: last frame's beams taken down; it keeps searching till its end runs out.
    if (!first) {
        for (const auto& [from, to] : s.lastBolts) {
            boltFx(world, s, false, from, to);
        }
        s.lastBolts.clear();
        if (frame % 10u == 0) { // (gameFPS / 3: its crackle, searching)
            playSound(world, 0x4E, *caster);
        }
        return 0;
    }
    struck.push_back(first);
    // The hops: from the first at 0.95 and 0.9 of the range, from the second found at 0.85, from
    // the third at 0.8.
    const auto hop = [&](ServerObject* from, float scale) {
        if (ServerObject* t = chainTarget(from->x, from->y, (X87(range) * scale).f(), *from, caster, struck)) { // (the reach stored)
            struck.push_back(t);
        }
    };
    if (1 < most) {
        hop(struck[0], 0.95f);
    }
    if (2 < most) {
        hop(struck[0], 0.9f);
    }
    if (3 < most && 1 < struck.size()) {
        hop(struck[1], 0.85f);
    }
    if (4 < most && 2 < struck.size()) {
        hop(struck[2], 0.8f);
    }
    // The bolts: caster to the first, the first to the 2nd and 3rd, the 4th from the 2nd (or 3rd),
    // the 5th from the 3rd (or 2nd).
    const auto at = [&](std::size_t i) { return i < struck.size() ? struck[i] : nullptr; };
    const auto bolt = [&](const ServerObject* from, const ServerObject* to) { // (newest first)
        s.bolts.insert(s.bolts.begin(), {from->extent, to->extent});
    };
    bolt(caster, at(0));
    if (1 < most && at(1)) {
        bolt(at(0), at(1));
    }
    if (2 < most && at(2)) {
        bolt(at(0), at(2));
    }
    if (3 < most && at(3) && (at(1) || at(2))) {
        bolt(at(1) ? at(1) : at(2), at(3));
    }
    if (4 < most && at(4) && (at(2) || at(1))) {
        bolt(at(2) ? at(2) : at(1), at(4));
    }
    // LightningDamage a frame to each, the fraction carried; the beams that changed shown anew.
    const float amount = (X87(world.number("LightningDamage")) + s.carry).f(); // (stored)
    const int whole = static_cast<int>(amount);
    s.carry = (X87(amount) - X87::fromDouble(whole)).f();
    // Paired with last frame's bolts in order: only the beams that changed are sent again.
    std::size_t old = 0;
    // (walked as they stand now: a damage's onHurt script may end the spell meanwhile, its bolts
    // cleared under the walk: updateLasting)
    const auto bolts = s.bolts;
    for (const auto& [from, to] : bolts) {
        if (old < s.lastBolts.size()) {
            const auto& [oldFrom, oldTo] = s.lastBolts[old];
            if (to != oldTo || from != oldFrom) {
                boltFx(world, s, false, oldFrom, oldTo);
                boltFx(world, s, true, from, to);
            }
            ++old;
        } else {
            boltFx(world, s, true, from, to);
        }
        if (ServerObject* t = world.byExtent(to)) {
            if (0 < whole) {
                fight.damage(*t, caster, 0, whole, 17);
            }
            if (t->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) {
                pointFx(0x81, t->x, t->y);
            }
        }
    }
    for (; old < s.lastBolts.size(); ++old) {
        boltFx(world, s, false, s.lastBolts[old].first, s.lastBolts[old].second);
    }
    if (caster->player) {
        if (s.staff == 0) { // a point of mana a frame
            fight.setPlayerState(*caster, 10);
            addMana(*caster, -1);
            if (caster->player->mana == 0) {
                return 1;
            }
        } else { // a charge of its staff a frame (MSG_REPORT_CHARGES), the staff's pose (0x16)
            ServerObject* w = world.byExtent(s.staff);
            if (!w || w->charges == 0) {
                return 1;
            }
            --w->charges;
            fight.setPlayerState(*caster, 0x16);
            tell(*caster, {0x64, lo(w->extent), hi(w->extent), w->charges, w->maxCharges});
            if (w->charges == 0) {
                return 1;
            }
        }
    }
    if (frame % 10u == 0) { // its crackle at both ends, three times a second (gameFPS / 3)
        playSound(world, 0x4E, *caster);
        playSound(world, 0x4E, *first);
    }
    s.end = frame + static_cast<std::uint32_t>(world.number("LightningSearchTime"));  // renewed each frame it strikes
    return 0;
}

void Spells::addHealth(ServerObject& t, int amount)
{
    adjustHealth(world, t, amount); // (unitAdjustHP)
}

void Spells::pointFx(std::uint8_t op, float x, float y)
{
    nox::server::pointFx(world, op, x, y); // (world.hpp)
}

ServerObject* Spells::drainTarget(float x, float y, ServerObject* caster)
{
    // Within ManaDrainRange of (x, y) (on each), the one with mana
    // nearest it in sight (ray flags 5), the players counted at half their distance.
    const float range = world.number("ManaDrainRange");
    ServerObject* best = nullptr;
    float bestScore = 1e8f;
    const float range2 = (X87(range) * range).f();
    world.unitsInCircle(x, y, range, [&](ServerObject& o) { // (unitsGetInCircle, by the grid)
        // (Its circle's test, and the score: the distance squared on the stack.)
        const X87 dx = X87(x) - o.x;
        const X87 dy = X87(y) - o.y;
        const X87 d2 = dx * dx + dy * dy;
        if (o.pendingDelete || o.holder != 0 || !(d2 < X87(range2)) || &o == caster) { // (0x8020: ours' 0x20 pendingDelete)
            return;
        }
        float weight = 1.0f;
        if ((o.classes & oc::Immobile) && (o.subclasses & 0x18u)) { // an obelisk, with mana
            if (!(0 < o.value)) {
                return;
            }
        } else if (o.classes & oc::Monster) {
            // A caster (its status 0x20) is one with mana; an enemy's.
            if (!o.monster || !(o.monster->status & 0x20u) || (caster && !isEnemy(world, *caster, o))) {
                return;
            }
        } else if (o.player) {
            if (o.player->mana == 0 || (caster && !isEnemy(world, *caster, o)) || (caster && teamed(o, *caster)) ||
                (o.player->statusFlags & 1u)) {
                return;
            }
            weight = 0.5f;
        } else {
            return;
        }
        if (o.flags & (ObjFlag::Dead | ObjFlag::Destroyed)) {
            return;
        }
        const X87 score = d2 * weight; // (on the stack against the best, kept as a float)
        if (score < X87(bestScore) && world.traceRay(x, y, o.x, o.y, Trace::Doors | Trace::ThroughWindows)) {
            best = &o;
            bestScore = score.f();
        }
    });
    return best;
}

bool Spells::drainMana(ServerObject& caster, ServerObject& from, int amount)
{
    // A full caster takes none.
    if (caster.player && caster.player->maxMana <= caster.player->mana) {
        return false;
    }
    if ((from.classes & oc::Immobile) && (from.subclasses & 0x18u)) {
        // An obelisk: not another team's; a quest's never runs dry.
        if (from.team != 0 && from.team != caster.team) {
            return false;
        }
        if (from.value <= amount) {
            amount = from.value;
            if (!world.quest()) {
                from.value = 0;
            }
        } else if (!world.quest()) {
            from.value -= amount;
        }
        if (amount == 0) {
            return false;
        }
        if (!world.quest()) {
            from.touch();
        }
    } else if (from.classes & oc::Monster) {
        amount = 1; // (a caster gives a point at a time, its own never less)
    } else if (from.player) {
        amount = std::min<int>(amount, from.player->mana);
        addMana(from, -amount);
    }
    if (amount < 1) {
        return false;
    }
    // (A quest's: x the class's mana multiplier, 1.)
    addMana(caster, amount);
    return true;
}

/// A lasting spell's whole points this frame: `perFrame` with the fraction its record carried,
/// the new fraction carried on.
static int wholePoints(DurationSpell& s, float perFrame)
{
    const float amount = (X87(perFrame) + s.carry).f(); // (stored, Math_FloatToInt)
    const int whole = static_cast<int>(amount);
    s.carry = (X87(amount) - X87::fromDouble(whole)).f();
    return whole;
}

// A lasting spell's frame, by spell: non-zero ends it (updateLasting then cancels it).
int Spells::lastingUpdate(DurationSpell& s)
{
    if (s.noFunctions) {
        return 0; // (no update function)
    }
    ServerObject* caster = s.caster ? world.byExtent(s.caster) : nullptr;
    ServerObject* target = s.target ? world.byExtent(s.target) : nullptr;
    Fight fight{world, rng, frame, &things, modifiers};
    const auto inform = [&](const ServerObject& p, std::string_view id) {
        if (p.player) {
            tell(p, informStringBytes(id));
        }
    };
    if (0x4A < s.spell && s.spell < 0x73) {  // the summon spells
        // Spell_SummonFinish: the frame before its end it comes, unless the caster's
        // gone (a player's checked again: its field guide in a co-op game, its room); 899 at it.
        if (!caster || (caster->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            return 1;
        }
        if (s.end - 1 != frame) {
            return 0;
        }
        const int index = static_cast<int>(s.spell) - 0x4A;
        if (caster->player) {
            if ((world.gameFlags & GameFlag::Coop) && !caster->player->fieldGuide[static_cast<std::size_t>(index)]) {
                inform(*caster, "Summon.c:NeedGuideToSummon");
                return 1;
            }
            if (!roomFor(world, things, *caster, index)) {
                inform(*caster, "Summon.c:CreatureControlFailed");
                return 1;
            }
        }
        if (ServerObject* made = summonAt(fight, s.summonType, s.summonX, s.summonY, caster, s.summonDir)) {
            playSound(world, 899, *made);
        }
        s.summoned = true;
        return 1;
    }
    if (s.spell == 0x09) {  // Charm
        // Spell_CharmFinish: broken by its going or by the caster's straying past 300;
        // at the end (but for those never controlled: subclass 0x2000; a player's: its room) the
        // creature's the caster's: SUMMONED, out of its team, a quest's health its own again, at
        // the caster's creature order (a monster's: escorting it), its player told.
        // Broken: its caster (a player) told why, and the charm's end sound (0x10) at it.
        const auto broken = [&](std::string_view why) {
            inform(*caster, why);
            playSound(world, 0x10, *caster);
            return 1;
        };
        if (!target || (target->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) || !caster) {
            if (caster) {
                playSound(world, 0x10, *caster);
            }
            return 1;
        }
        if (X87(300.0f) < edgeDistance53(*caster, *target)) {
            return broken("Summon.c:CharmBrokenDistance");
        }
        if (target->monster && target->monster->notCharmable) { // (noxcpp: made so mid-charm, a mode's: ended now)
            return broken("Summon.c:CreatureControlImpossible");
        }
        if (s.end - 1 != frame) {
            return 0;
        }
        if (target->subclasses & 0x2000u) {
            return broken("Summon.c:CreatureControlImpossible");
        }
        if (caster->player && !roomFor(world, things, *caster, guideIndex(target->typeName))) {
            return broken("Summon.c:CreatureControlFailed");
        }
        disenchant(world, *target, Enchant::Charming);
        if (fight.parentPlayer(target) == caster) {
            return broken("Summon.c:CreatureAlreadyOwned");
        }
        clearOwner(world, *target); // (another player's: lost to it)
        setOwner(world, caster, *target);
        if (target->team != 0) {
            removeTeamMember(world, *target);
        }
        if (target->monster) {
            target->monster->status |= things::MonsterStatus::Summoned;
            if (world.quest()) {
                const auto most = static_cast<std::uint16_t>(target->monster->def ? target->monster->def->questHealth : target->maxHealth);
                target->maxHealth = most;
                if (most < target->health) {
                    target->health = most;
                    target->touch();
                }
            }
        }
        if (!caster->player) {
            orderCreatures(fight, *caster, target, 4);
        } else {
            orderCreatures(fight, *caster, target, caster->player->creatureOrder);
            target->subclasses |= 0x180u;  // monster subclasses MONITOR (0x80) and MIGRATE (0x100)
            reportAcquired(world, *caster, *target); // (and its minimap mark)
            simpleObjTo(world, *caster, *target);    // (SrvNet_SendSimpleObjReliable)
            if (world.quest()) {
                unlinkSpawn(world, *target); // (its generator's no more)
            }
        }
        playSound(world, spellSound(things, s.spell, 1), *caster); // (at the record's caster)
        return 1;
    }
    switch (s.spell) {
    case 0x33: { // Shield: ends with its target
        return !target || (target->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) ? 1 : 0;
    }
    case 0x23: { // Greater Heal
        if (!target || (target->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            return 1;
        }
        if (caster && enchanted(*caster, Enchant::Run)) {
            return 1;
        }
        if (!caster || !canInteract(world, *caster, *target)) {
            return 1;
        }
        if (manaOf(*caster) == 0) {
            return 1;
        }
        if (((caster->classes & oc::Monster) && movedFrom(*caster, s.x, s.y)) || justHurt(*caster, frame)) {
            return 1;
        }
        if (target->maxHealth == target->health) {
            return 1;
        }
        // Its amount a frame at its power (0.5-1.5), the fraction kept (x a player
        // caster's class health multiplier, 1).
        static constexpr float kHeal[6] = {0, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f};
        const int whole = wholePoints(s, kHeal[std::clamp(s.power, 0, 5)]);
        addHealth(*target, whole);
        addMana(*caster, -1);
        return 0;
    }
    case 0x16: { // Drain Mana (spellDrainMana)
        if (casterGoneOrRunning(s, caster)) {
            return 1;
        }
        if (s.glyph) { // a glyph's: 50 off the nearest at once
            const float px = caster ? caster->x : s.x;
            const float py = caster ? caster->y : s.y;
            if (ServerObject* t = drainTarget(px, py, caster)) {
                addMana(*t, -50);
            }
            return 1;
        }
        if (caster->player && caster->player->maxMana <= caster->player->mana) {
            return 1;
        }
        if ((caster->classes & oc::Monster) && movedFrom(*caster, s.x, s.y)) {
            return 1;
        }
        // It stands where it cast and isn't hurt.
        if (justHurt(*caster, frame) || !(std::fabs(s.x - caster->x) < 5.0f) || !(std::fabs(s.y - caster->y) < 5.0f)) {
            return 1;
        }
        ServerObject* from = drainTarget(caster->x, caster->y, caster);
        s.target = from ? from->extent : 0;
        if (!from) {
            if (s.rayTarget != 0) {
                rayFx(world, s, false, s.rayTarget);
            }
            return 1;
        }
        const int whole = wholePoints(s, world.number("ManaDrainCoeff", static_cast<std::size_t>(s.power - 1)));
        if (s.target != s.rayTarget) {
            if (s.rayTarget != 0) {
                rayFx(world, s, false, s.rayTarget);
            }
            rayFx(world, s, true, s.target);
        }
        if (drainMana(*caster, *from, whole) && frame % 15u == 0) { // (gameFPS >> 1: its hum, twice a second)
            playSound(world, 0xE6, *caster);
            playSound(world, 0xE5, *from);
        }
        s.rayTarget = s.target;
        return 0;
    }
    case 0x08: { // Channel Life: the caster's health into the target's mana
        if (!target || (target->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            return 1;
        }
        if (s.glyph) {
            addMana(*target, 20);
            fight.takeHealth(*target, 20);
            return 1;
        }
        if (caster && enchanted(*caster, Enchant::Run)) {
            return 1;
        }
        if ((target->classes & oc::Monster) && movedFrom(*target, s.x, s.y)) {
            return 1;
        }
        // Full: ends (a monster's most, 0, is never its 1000: it runs on, the caster paying).
        if ((target->player ? target->player->maxMana : 0) == manaOf(*target)) {
            return 1;
        }
        if (!caster || caster->health < 2) {
            return 1;
        }
        const int whole = wholePoints(s, world.number("ChannelLifeCoeff", static_cast<std::size_t>(s.power - 1)));
        addMana(*target, whole);
        fight.takeHealth(*caster, 1);
        return 0;
    }
    case 0x2B:
        return chainLightning(s, caster);
    case 0x3B: { // Plasma (plasmaShot)
        if (!caster) {
            return 1;
        }
        if ((caster->classes & oc::Monster) && movedFrom(*caster, s.x, s.y)) {
            return 1;
        }
        // Its target kept while it lives, within 400 and in reach (FPS x 3 frames more, once it's
        // behind the caster: s.value counts them down); else the enemy under the cursor within
        // 400 (a monster's record read as a player's: with the monsters), else the nearest
        // enemy creature within 400 it sees.
        bool kept = false;
        if (target) {
            if (!(target->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
                if (!(facingSides(caster->x, caster->y, caster->direction, target->x, target->y) & 2)) {  // bit 2: behind the caster
                    s.value = 0;
                } else if (s.value == 0) {
                    s.value = 30 * 3;
                }
                kept = edgeDistance53(*target, *caster) <= X87(400.0f) && canInteract(world, *caster, *target);
            }
            if (!kept) {
                s.target = 0;
                target = nullptr;
            }
        }
        if (!kept) {
            ServerObject* found = nullptr;
            if (caster->player && caster->player->cursorObject != 0) {
                ServerObject* under = world.byExtent(caster->player->cursorObject);
                if (under && isEnemy(world, *caster, *under) && edgeDistance53(*caster, *under) <= X87(400.0f)) {
                    found = under;
                }
            }
            if (!found) {
                float best = 160000.0f; // (0x481C4000)
                world.unitsInCircle(caster->x, caster->y, 400.0f, [&](ServerObject& o) { // (unitsGetInCircle, by the grid)
                    if (!(o.classes & 0x20006u) || (o.flags & (ObjFlag::Dead | ObjFlag::Destroyed)) || o.pendingDelete || &o == caster || o.holder != 0 ||  // classes Monster | Player | MonsterGenerator
                        ((o.classes & oc::Monster) && (o.subclasses & 0x8000u)) || !isEnemy(world, *caster, o)) {
                        return;
                    }
                    const float dx = o.x - caster->x;
                    const float dy = o.y - caster->y;
                    const float d2 = dx * dx + dy * dy;
                    if (d2 < best && canInteract(world, *caster, o)) {
                        best = d2;
                        found = &o;
                    }
                });
            }
            target = found;
            s.target = found ? found->extent : 0;
            s.value = 0;
        }
        if (s.value != 0 && --s.value == 0) {  // its 3 s behind the caster ran out
            s.target = 0;
            target = nullptr;
        }
        // The beam (MSG_FX_DURATION_SPELL 1 / 8) to it as it changes.
        if (!target) {
            if (s.rayTarget != 0) {
                rayFx(world, s, false, s.rayTarget);
                s.rayTarget = 0;
            }
            return 0;
        }
        if (target->extent != s.rayTarget) {
            if (s.rayTarget != 0) {
                rayFx(world, s, false, s.rayTarget);
            }
            rayFx(world, s, true, target->extent);
        }
        const bool hecubah = target->typeName == "Hecubah" || target->typeName == "HecubahWithOrb";
        fight.damage(*target, caster, 0, static_cast<int>(world.number(hecubah ? "PlasmaDamageHecubah" : "PlasmaDamage")), 0xE);
        if (target->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) {
            pointFx(0x83, target->x, target->y);
        }
        s.rayTarget = target->extent;
        if (caster->player) {
            fight.setPlayerState(*caster, 0x16);
        }
        if (frame % 10u == 0) { // (gameFPS / 3: its hum at both ends)
            playSound(world, 0x62, *caster);
            playSound(world, 0x62, *target);
        }
        if (s.value == 0) {  // not behind (no count): its search time renewed
            s.end = frame + static_cast<std::uint32_t>(world.number("PlasmaSearchTime"));
        }
        if (s.staff != 0) { // a charge of its staff a frame
            ServerObject* w = world.byExtent(s.staff);
            if (w && w->charges != 0) {
                --w->charges;
                if (caster->player) {
                    fight.setPlayerState(*caster, 0x16);
                    tell(*caster, {0x64, lo(w->extent), hi(w->extent), w->charges, w->maxCharges});
                }
                if (w->charges != 0) {
                    return 0;
                }
            }
            return 1;
        }
        return 0;
    }
    case 0x74: // Tag: its target gone
        return !target || (target->flags & ObjFlag::Destroyed) ? 1 : 0;
    case 0x1F: { // Force of Nature
        if (caster && enchanted(*caster, Enchant::Run)) {
            return 1;
        }
        if (s.end - 7 == frame && s.object) { // the charge goes just before
            if (ServerObject* charge = world.byExtent(s.object)) {
                charge->pendingDelete = true;
            }
        }
        if (s.end - 1 != frame) {
            if (!s.glyph && caster && caster->player) {
                fight.setPlayerState(*caster, 10);
            }
            return 0;
        }
        if (!things.findThing("DeathBall")) {
            return 1;
        }
        // From the caster, its shape's reach + 4 ahead (a point's 4, else 24) the way it faces
        // now; a glyph's from where it was cast, its facing then, at rest.
        float fx = s.x;
        float fy = s.y;
        int way = s.value;
        X87 ahead = X87(0.0f); // (on the stack)
        if (!s.glyph && caster) {
            fx = caster->x;
            fy = caster->y;
            way = caster->direction;
            switch (caster->shape) {
            case ShapeKind::Center: ahead = X87(4.0f); break;
            case ShapeKind::Circle: ahead = X87(caster->radius) + 4.0f; break;
            case ShapeKind::Box: ahead = X87(std::max(caster->boxWidth, caster->boxHeight)) + 4.0f; break;
            default: ahead = X87(24.0f); break;
            }
        }
        const auto [dx, dy] = directionVector(way & 0xFF);
        float bx = (X87(dx) * ahead + fx).f();
        float by = (X87(dy) * ahead + fy).f();
        if (!world.traceRay(fx, fy, bx, by, Trace::Doors | Trace::ThroughWindows)) {
            bx = fx;
            by = fy;
        }
        ServerObject& ball = world.spawn(things, "DeathBall", bx, by);
        ball.owner = caster ? caster->extent : 0;
        ball.born = frame;
        ball.flags |= ObjFlag::Active;
        ball.newX = bx;
        ball.newY = by;
        if (s.glyph) {
            ball.vx = ball.vy = 0;
        } else {
            ball.vx = (X87(dx) * ball.speedCur).f();
            ball.vy = (X87(dy) * ball.speedCur).f();
        }
        ball.direction = static_cast<std::uint16_t>(way);
        ball.touch();
        if (caster) {
            playSound(world, 0x26, *caster);
        }
        return 1;
    }
    case 0x38: { // Mana Bomb (manaBombBoom)
        if (!caster && !s.glyph) {
            return 1;
        }
        bool goes = s.end - 1 == frame;
        if (caster && caster->player && caster->player->mana == 0) {
            goes = true;
        }
        if (s.object) { // the charge goes when the mana's nearly out (a glyph's: 9 frames before)
            const bool keep = !s.glyph && caster && caster->player ? 14 < caster->player->mana : 9u < s.end - frame;
            if (!keep) {
                if (ServerObject* charge = world.byExtent(s.object)) {
                    charge->pendingDelete = true;
                }
                s.object = 0;
            }
        }
        const int power = s.value;  // the power charged so far; it grows each frame till it goes
        if (!goes) {
            s.value += static_cast<int>(world.number("ManaBombDeltaPower", static_cast<std::size_t>(s.power - 1)));
            if (!s.glyph && caster && caster->player) {
                addMana(*caster, -s.power);
            }
            return 0;
        }
        float bx = s.x;
        float by = s.y;
        if (!s.glyph) {
            if (!caster) {
                return 1;
            }
            bx = caster->x;
            by = caster->y;
        }
        // (The walls take it too: with the walls' damage.) Its power, less past ManaBombInRadius,
        // out to ManaBombOutRadius, as mana-bomb harm (15), from its caster.
        Missiles{world, things, modifiers, rng, frame}.damageAt(bx, by, world.number("ManaBombOutRadius"), world.number("ManaBombInRadius"),
                                                                 power, 15, caster, nullptr, true);
        jiggle(bx, by, static_cast<int>(world.number("ManaBombShakeMag")));
        pointFx(0x81, bx, by);
        pointFx(0x9A, bx, by);
        if (caster) {
            playSound(world, 0x51, *caster);
        }
        s.burst = true;
        return 1;
    }
    case 0x43: // Oval Shield: its target running, a monster moved, or dead
        if (!target || enchanted(*target, Enchant::Run)) {
            return 1;
        }
        if ((target->classes & oc::Monster) && movedFrom(*target, s.x, s.y)) {
            return 1;
        }
        return (target->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) ? 1 : 0;
    case 0x1C: { // Firewalk (firewalkTick)
        if (!target || (target->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            return 1;
        }
        if (s.start == s.last) { // the first pass: the trail starts where it stands
            s.trail = {target->x, target->y, target->x, target->y};
            ++s.last;
            return 0;
        }
        const X87 dx = X87(target->x) - s.trail[0]; // (the distance on the stack)
        const X87 dy = X87(target->y) - s.trail[1];
        // (Small flames at 1, up to medium at 2-3, up to big at 4-5.)
        const int most = s.power < 2 ? 0 : (3 < s.power) + 1;
        if (X87(15.0f) < util::sqrt(dx * dx + dy * dy) - target->radius) {
            static constexpr const char* kFlames[3] = {"SmallFlame", "MediumFlame", "Flame"};
            float px = s.trail[0];
            float py = s.trail[1];
            for (int i = 0; i < 2; ++i) { // two, the second halfway back towards the last
                const char* kind = kFlames[nox::util::uniform_int_distribution<int>(0, most)(rng)];
                if (things.findThing(kind)) {
                    ServerObject& f = world.spawn(things, kind, px, py);
                    f.born = frame;
                    f.flags |= ObjFlag::Active;
                    f.decayAt = frame + 30 * 25;
                    playSoundAt(world, 0x2E, px, py);
                }
                // (Only when it moved both ways from the last: a straight run along an axis lays
                // both on one spot.)
                // (Each difference stored.)
                const float backX = (X87(px) - s.trail[2]).f();
                const float backY = (X87(py) - s.trail[3]).f();
                if (backX != 0 && backY != 0) {
                    px = (X87(px) - X87(backX) * 0.5f).f();
                    py = (X87(py) - X87(backY) * 0.5f).f();
                }
            }
            s.trail = {target->x, target->y, s.trail[0], s.trail[1]};
        }
        return 0;
    }
    case 0x04:
    case 0x73:
    case 0x7A:
    case 0x7B:
    case 0x7C:
    case 0x7D:
    case 0x7E:
        return teleportSpell(s, caster, target);
    case 0x18: { // Lightning (spellEnergyBoltTick)
        if (casterGoneOrRunning(s, caster)) {
            return 1;
        }
        const float range = world.number("LightningRange");
        if (s.glyph) { // a glyph's: one strike at the nearest enemy, and done
            if (ServerObject* t = boltTarget(s.x, s.y, range, caster, true)) {
                ServerObject* source = s.source ? world.byExtent(s.source) : nullptr;
                fight.damage(*t, source, 0, static_cast<int>(world.number("EnergyBoltGlyphDamage")), 17);
                pointFx(0x82, t->x, t->y);
                playSound(world, spellSound(things, 0x18, 0), *t);
            }
            return 1;
        }
        if ((caster->classes & oc::Monster) && movedFrom(*caster, s.x, s.y)) {
            return 1;
        }
        if (2u < frame - s.start && justHurt(*caster, frame)) {
            return 1;
        }
        // Its target kept while it lives, is in front, in range and in sight; else the enemy
        // under a player's cursor in range, else the nearest it can strike.
        if (target && ((target->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) ||
                       !(facingSides(caster->x, caster->y, caster->direction, target->x, target->y) & 1) ||
                       X87(range) < edgeDistance53(*target, *caster) || !canInteract(world, *caster, *target))) {
            target = nullptr;
        }
        if (!target) {
            s.target = 0;
            if (caster->player && caster->player->cursorObject != 0) {
                ServerObject* under = world.byExtent(caster->player->cursorObject);
                if (under && isEnemy(world, *caster, *under) && edgeDistance53(*caster, *under) <= X87(range)) {
                    target = under;
                }
            }
            if (!target) {
                target = boltTarget(caster->x, caster->y, range, caster, false);
            }
            s.target = target ? target->extent : 0;
        }
        if (!target) {
            if (s.rayTarget != 0) {
                rayFx(world, s, false, s.rayTarget);
                s.rayTarget = 0;
            }
            return 0;
        }
        const int whole = wholePoints(s, world.number("EnergyBoltDamage", static_cast<std::size_t>(s.power - 1)));
        if (s.target != s.rayTarget) {
            if (s.rayTarget != 0) {
                rayFx(world, s, false, s.rayTarget);
            }
            rayFx(world, s, true, s.target);
        }
        fight.damage(*target, caster, 0, whole, 17);
        if (target->flags & (ObjFlag::Dead | ObjFlag::Destroyed)) {
            pointFx(0x82, target->x, target->y);
        }
        s.rayTarget = s.target;
        if (caster->player) {
            fight.setPlayerState(*caster, 10);
        }
        if (frame % 10u == 0) { // (gameFPS / 3: its hum at both ends)
            playSound(world, 0x20, *caster);
            playSound(world, 0x20, *target);
        }
        s.end = frame + static_cast<std::uint32_t>(world.number("LightningSearchTime"));  // renewed each frame it strikes
        if (caster->player) {
            fight.setPlayerState(*caster, 10);
            addMana(*caster, -1);
            if (caster->player->mana == 0) {
                return 1;
            }
        }
        if (target->flags & ObjFlag::Dead) {  // a kill ends it next frame
            s.end = frame + 1;
        }
        return 0;
    }
    default:
        return 0;
    }
}

ServerObject* Spells::boltTarget(float x, float y, float range, ServerObject* caster, bool glyph)
{
    // Within `range` of (x, y), each by spellEnergyBoltSetTarget: a live
    // creature, not the caster, an enemy of it (a caster's own: in front of it and in its sight
    // too); the nearest.
    ServerObject* best = nullptr;
    // (The reach squared stored, the nearest kept as a float; each distance squared on the stack.)
    float bestD2 = (X87(range) * range).f();
    const float range2 = bestD2;
    world.unitsInCircle(x, y, range, [&](ServerObject& o) { // (unitsGetInCircle, by the grid: spellEnergyBoltSetTarget)
        const X87 dx = X87(o.x) - x;
        const X87 dy = X87(o.y) - y;
        const X87 d2 = dx * dx + dy * dy;
        if (o.pendingDelete || o.holder != 0 || !(d2 < X87(range2))) { // (0x8020: ours' 0x20)
            return;
        }
        if (!(o.classes & (oc::Monster | oc::Player | oc::MonsterGenerator)) || (o.flags & (ObjFlag::Dead | ObjFlag::Destroyed)) || &o == caster) {
            return;
        }
        if ((o.classes & oc::Monster) && (o.subclasses & 0x8000u)) {
            return;
        }
        if (caster && (!isEnemy(world, *caster, o) ||
                       (!glyph && (!(facingSides(caster->x, caster->y, caster->direction, o.x, o.y) & 1) || !canInteract(world, *caster, o))))) {
            return;
        }
        if (d2 < X87(bestD2)) {
            best = &o;
            bestD2 = d2.f();
        }
    });
    return best;
}

void Spells::lastingEnd(DurationSpell& s)
{
    if (s.noFunctions) {
        return; // (no destroy function)
    }
    if (s.spell == 0x15) {
        // spellTurnUndeadDelete: its UndeadKillers go.
        for (auto& [e, o] : world.objects()) {
            if (o.typeName == "UndeadKiller" && o.value == static_cast<int>(s.serial)) {
                o.pendingDelete = true;
            }
        }
        return;
    }
    if (0x4A < s.spell && s.spell < 0x73) {  // the summon spells
        // summonCancel: never came: the clients' effect off (MSG_FX_SUMMON_CANCEL
        // 0x7F), and 900 where it would have come.
        if (!s.summoned) {
            world.broadcasts().push_back({{0x7F, lo(s.summonId), hi(s.summonId)}, true, 0});
            playSoundAt(world, 900, s.summonX, s.summonY);
        }
        return;
    }
    if (s.spell == 0x09) {  // Charm
        // charmCreature2: its hold and charm off.
        if (ServerObject* t = s.target ? world.byExtent(s.target) : nullptr; t && !(t->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
            disenchant(world, *t, Enchant::Held);
            disenchant(world, *t, Enchant::Charming);
        }
        return;
    }
    switch (s.spell) {
    case 0x33:  // Shield: its enchantment off
        if (ServerObject* t = s.target ? world.byExtent(s.target) : nullptr) {
            disenchant(world, *t, Enchant::Shield);
        }
        break;
    case 0x2B: // its bolts gone, its staff's casting flag off
    case 0x3B: // Plasma: its staff's casting flag off
        s.bolts.clear();
        s.lastBolts.clear();
        if (ServerObject* w = s.staff ? world.byExtent(s.staff) : nullptr) {
            w->wandCasting = false;
        }
        break;
    case 0x36: // Moonglow: its Moonglow gone, the light off
        if (ServerObject* t = s.target ? world.byExtent(s.target) : nullptr) {
            if (t->player) {
                if (ServerObject* glow = s.value ? world.byExtent(static_cast<std::uint32_t>(s.value)) : nullptr) {
                    glow->pendingDelete = true;
                }
                disenchant(world, *t, Enchant::Moonglow);
            } else {
                disenchant(world, *t, Enchant::Light);
            }
        }
        break;
    case 0x43: // Oval Shield
        if (ServerObject* t = s.target ? world.byExtent(s.target) : nullptr) {
            disenchant(world, *t, Enchant::ReflectiveShield);
        }
        break;
    case 0x84: // Wall (spellWallDestroy): each of its walls back as it was
        revertMagicWalls(world, [&](const MagicWall& w) { return w.spell == s.serial; });
        break;
    case 0x74: // Tag (Spell_TagOnEnd): a player caster told it's off (0xD2, remove), a
        // target not a player off its minimap
        if (ServerObject* c = s.caster ? world.byExtent(s.caster) : nullptr; c && c->player) {
            if (ServerObject* t = s.target ? world.byExtent(s.target) : nullptr) {
                if (!(t->classes & oc::Player)) {
                    unmarkMinimap(world, *c, *t, 1);
                }
                tell(*c, {0xD2, lo(t->extent), hi(t->extent), lo(t->type), hi(t->type), 2, 1});
            }
        }
        break;
    case 0x1F: // Force of Nature: its charge gone; its caster's staff of it (its
        // weapon, subclass 0x200000) not casting
        if (ServerObject* charge = s.object ? world.byExtent(s.object) : nullptr) {
            charge->pendingDelete = true;
        }
        if (const ServerObject* c = s.caster ? world.byExtent(s.caster) : nullptr; c && c->player && c->player->currentWeapon) {
            if (ServerObject* w = world.byExtent(c->player->currentWeapon); w && (w->subclasses & 0x200000u)) {
                w->wandCasting = false;
            }
        }
        break;
    case 0x38: // Mana Bomb: its charge gone, its caster free; a fizzle (0xA3) if it
        // never went off
        if (ServerObject* charge = s.object ? world.byExtent(s.object) : nullptr) {
            charge->pendingDelete = true;
        }
        if (!s.glyph) {
            if (ServerObject* c = s.caster ? world.byExtent(s.caster) : nullptr; c && c->player) {
                disenchant(world, *c, Enchant::Held);
                disenchant(world, *c, Enchant::Anchored);
                disenchant(world, *c, Enchant::AntiMagic);
            }
        }
        if (!s.burst) {
            pointFx(0xA3, s.x, s.y);
        }
        break;
    default:
        break;
    }
}

} // namespace nox::server
