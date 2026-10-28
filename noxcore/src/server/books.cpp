// Books, field guides, signs, spell pedestals and the Oblivion staves' pickup on the server:
// the USE and PICKUP functions those things name in thing.bin (books.hpp lists them).
#include "noxcore/server/books.hpp"

#include "noxcore/server/monsters.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/quest.hpp"
#include "noxcore/server/sight.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/things/object_class.hpp"

#include <string>
#include <vector>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;

/// SrvNet_SendInformString: MSG_INFORM 0x0F {isSign, the string's id} to a player, not
/// while its .plr is applied, nor an empty id or one past 48 letters.
void informString(World& world, const ServerObject& to, std::string_view id, std::uint8_t sign)
{
    id = id.substr(0, id.find('\0'));
    if (!(to.classes & oc::Player) || !to.player || to.player->transfer || id.empty() || 48 < id.size()) {
        return;
    }
    world.broadcasts().push_back({informStringBytes(id, sign), true, to.extent});
}

/// The refusal's sound to the player alone (aud(0x39D, it, 2, its net code)).
void refuse(World& world, const ServerObject& player)
{
    playSound(world, 0x39D, player, 2, netCodeOf(player));
}

/// delayedDeleteObject on a book: out of its holder's inventory, gone.
void useUp(Items& items, ServerObject& book)
{
    if (book.holder != 0) {
        if (ServerObject* h = items.world.byExtent(book.holder)) {
            items.removeFromInventory(*h, book);
        }
    }
    book.pendingDelete = true;
}

/// playerCheckSpellClass: 0 when the class (1 wizard, 2 conjurer) may have the
/// spell (its flags 0x1000000 everyone's, 0x2000000 a wizard's, 0x4000000 a conjurer's), else 9.
int spellClassCheck(const things::ThingDb& things, int classId, std::uint32_t spell)
{
    const things::SpellDef* def = things.findSpell(spell);
    const std::uint32_t flags = def ? def->flags : 0u; // (spellFlags)
    std::uint32_t own = 0;
    if (classId == 1) {
        own = 0x2000000u;
    } else if (classId == 2) {
        own = 0x4000000u;
    } else {
        return 9;
    }
    return (flags & 0x1000000u) || (flags & own) ? 0 : 9;
}

/// SpellRewardUse.
void useSpellReward(Items& items, ServerObject& user, ServerObject& book)
{
    World& world = items.world;
    if (!(user.classes & oc::Player) || !user.player) {
        return;
    }
    const int classId = user.player->classId;
    const std::uint32_t spell = book.bookEntry; // (a byte)
    // (noxcpp: a multiclass player, a NoX Core mode's, learns any class's.)
    if (user.player->multiclass || ((classId == 1 || classId == 2) && spellClassCheck(items.things, classId, spell) == 0)) {
        if (world.modeBook()) {
            refuse(world, user); // (noxcpp: a mode's book has only what the mode grants; the book kept)
            return;
        }
        const bool show = (world.gameFlags & (GameFlag::Solo | GameFlag::Quest)) && spell < user.player->spellLevels.size() &&
                          user.player->spellLevels[spell] == 0;
        if (!grantSpell(world, items.things, user, static_cast<int>(spell), true, show, 0)) { // (Spell_GrantToPlayer)
            refuse(world, user);
            return;
        }
        useUp(items, book);
        return;
    }
    informString(world, user, "use.c:SpellRewardClassFail", 0);
    refuse(world, user);
}

/// AbilityRewardUse.
void useAbilityReward(Items& items, ServerObject& user, ServerObject& book)
{
    World& world = items.world;
    if (!(user.classes & oc::Player) || !user.player) {
        return;
    }
    if (!user.player->usesAbilities()) { // (a warrior's; noxcpp's multiclass player's too)
        informString(world, user, "pickup.c:ObjectEquipClassFail", 0);
        refuse(world, user);
        return;
    }
    if (world.modeBook()) {
        refuse(world, user); // (noxcpp: a mode's book has only what the mode grants; the book kept)
        return;
    }
    const std::uint32_t ability = book.bookEntry;
    // (Its level read from the spell book's slot of that number: where abilities live
    // in a player; a multiclass player's own book.)
    const bool show = (world.gameFlags & (GameFlag::Solo | GameFlag::Quest)) && ability < 6 && user.player->abilityLevel(ability) == 0;
    if (!grantPlayerAbility(world, user, static_cast<int>(ability), show)) { // (Abil_GrantToPlayer)
        refuse(world, user);
        return;
    }
    useUp(items, book);
}

/// FieldGuideUse (Obj_FieldGuideUse).
void useFieldGuide(Items& items, ServerObject& user, ServerObject& guide)
{
    World& world = items.world;
    if (!(user.classes & oc::Player) || !user.player) {
        return;
    }
    if (world.modeBook()) {
        refuse(world, user); // (noxcpp: a mode's book has only what the mode grants; the guide kept)
        return;
    }
    const int creature = guideCreatureIndex(guide.guideCreature);
    if (world.quest() && user.player->classId != 2 && !user.player->multiclass) {
        informString(world, user, "pickup.c:ObjectEquipClassFail", 0);
        return;
    }
    if (user.player->fieldGuide[static_cast<std::size_t>(creature)]) {
        informString(world, user, "objcoll.c:AlreadyHaveGuide", 0);
        return;
    }
    // (Plr_AwardFieldGuide's refusal isn't looked at: a guide of no known creature gives
    // "AwardGuideError" and is used up all the same: an original bug.)
    awardFieldGuide(world, user, creature, true);
    useUp(items, guide);
}

} // namespace

int guideCreatureIndex(std::string_view name)
{
    // The 41 names from "GUIDE_INVALID" (g_creatureGuideNames): its index, the Bomber's too; 0 none.
    name = name.substr(0, name.find('\0'));
    for (int i = 1; i < 41; ++i) {
        if (const char* g = guideName(i); g && name == g) {
            return i;
        }
    }
    return 0;
}

bool bookPickup(Items& items, ServerObject& who, ServerObject& book, bool everyone)
{
    World& world = items.world;
    if (world.gameFlags & (GameFlag::Solo | GameFlag::Quest)) {
        items.use(who, book); // (Obj_UseItem)
    }
    if (book.pendingDelete || (book.flags & ObjFlag::Destroyed)) {
        return true;
    }
    if (!items.pickUpDefault(who, book, everyone)) { // (Obj_PickupDefault)
        return false;
    }
    // SpellBookPickup: 0x33A for a spell book (subclass 1), 0x33C for the rest; AbilityBookPickup
    // 0x33A.
    const bool spellBook = book.pickup == "AbilityBookPickup" || (book.subclasses & 1u);
    playSound(world, spellBook ? 0x33Au : 0x33Cu, who);
    return true;
}

bool useBook(Items& items, ServerObject& user, ServerObject& item)
{
    if (item.useFn == "SpellRewardUse") {
        useSpellReward(items, user, item);
    } else if (item.useFn == "AbilityRewardUse") {
        useAbilityReward(items, user, item);
    } else if (item.useFn == "FieldGuideUse") {
        useFieldGuide(items, user, item);
    } else if (item.useFn == "ReadUse" || item.useFn == "WarpReadUse") {
        readSign(items.world, items.frame, user, item);
    } else {
        return false;
    }
    return true;
}

void readSign(World& world, std::uint32_t frame, ServerObject& user, ServerObject& sign)
{
    if (!(user.classes & oc::Player) || !user.player) {
        return;
    }
    // At most every 3 s (gameFPS x 3, unsigned), or never read.
    if (!(90u < frame - sign.readAt || sign.readAt == 0)) {
        return;
    }
    if (!bodiesClear(world, user, sign)) { // (mapCheck)
        return;
    }
    if (sign.useFn == "WarpReadUse") {
        if (!world.warpGatesOn) {
            informString(world, user, "GeneralPrint:WarpClosed", 1);
        } else {
            // SrvNet_SendInform: MSG_INFORM 0x15 {u32 the stage the warp leads to}.
            const auto stage = static_cast<std::uint32_t>(questNextStageThreshold(world.questStage));
            world.broadcasts().push_back({{0xA9, 0x15, static_cast<std::uint8_t>(stage), static_cast<std::uint8_t>(stage >> 8),
                                           static_cast<std::uint8_t>(stage >> 16), static_cast<std::uint8_t>(stage >> 24)},
                                          true,
                                          user.extent});
        }
    } else {
        informString(world, user, sign.text, 1);
    }
    sign.readAt = frame;
}

void oblivionPickedUp(Items& items, ServerObject& who, ServerObject& item)
{
    World& world = items.world;
    if (!(who.classes & oc::Player) || !who.player || who.player->transfer) {
        return;
    }
    struct Line {
        std::uint32_t subclass;
        std::string_view id;
        std::uint32_t sound;
    };
    static constexpr Line kLines[] = {{0x800000u, "weapon.c:PickupHalberdOblivion", 0x392},
                                      {0x1000000u, "weapon.c:PickupHeartOblivion", 0x393},
                                      {0x2000000u, "weapon.c:PickupWierdlingOblivion", 0x394},
                                      {0x4000000u, "weapon.c:PickupOrbOblivion", 0x395}};
    for (const Line& l : kLines) {
        if (item.subclasses & l.subclass) {
            informString(world, who, l.id, 0);
            playSound(world, l.sound, who);
            break;
        }
    }
    Fight fight{world, items.rng, items.frame, &items.things, items.modifiers};
    levelUpMoment(fight, &who, 1); // (its moment 1: OblivionUp)
    items.tryEquip(who, item);     // (Plr_TryEquip)
}

void awardSpellCollide(Fight& fight, ServerObject& pedestal, ServerObject* other)
{
    // (noxcpp: a mode's book, World::modeBook, has only what the mode grants.)
    if (other && fight.things && !fight.world.modeBook()) {
        // (Spell_GrantToPlayer(other, its CollideData's spell, 1, 0, 0))
        grantSpell(fight.world, *fight.things, *other, pedestal.collideArg, true, false, 0);
    }
}

} // namespace nox::server
