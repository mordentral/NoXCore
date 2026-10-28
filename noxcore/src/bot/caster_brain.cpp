// The casters' brains (caster_brain.hpp): CasterBrain's ranged fight, spell bar pages, shots,
// summons and buffs, then what WizardBrain and ConjurerBrain pick from their pages.
#include "noxcore/bot/caster_brain.hpp"

#include "noxcore/client/navigation.hpp"
#include "noxcore/things/spell_ids.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace nox::bot {

namespace {

// Enchantments (bit n = ENCHANT n) the buffs put on.
constexpr std::uint32_t kHasted = 1u << 9;
constexpr std::uint32_t kVampiric = 1u << 13;
constexpr std::uint32_t kInfravision = 1u << 21;
constexpr std::uint32_t kShocked = 1u << 22;
constexpr std::uint32_t kShielded = 1u << 26;

// The quiver we have on, if any.
const OwnItem* equippedQuiver(const BotView& view)
{
    const auto it = std::ranges::find_if(view.inventory, [](const OwnItem& i) {
        return i.kind == ItemKind::Quiver && i.equipped;
    });
    return it != view.inventory.end() ? &*it : nullptr;
}

} // namespace

// ------------------------------------------------------------------ CasterBrain

CasterBrain::CasterBrain(Skill skill, std::uint32_t seed)
    : PlayerBrain(skill, seed)
{
    reach_ = farthest() - 20;
}

bool CasterBrain::knows(const BotView& view, std::uint32_t spell)
{
    return std::ranges::find(view.spells, spell, &BotView::KnownSpell::id) != view.spells.end();
}

bool CasterBrain::worthOn(const BotView& view, const SeenPlayer& enemy, std::uint32_t spell) const
{
    // What each debuff puts on them (bit n = ENCHANT n; poison is the player's status).
    constexpr std::uint32_t kConfused = 1u << 3;
    constexpr std::uint32_t kSlowed = 1u << 4;
    constexpr std::uint32_t kHeld = 1u << 5;
    constexpr std::uint32_t kFrozen = 1u << 25;
    bool already = false;
    switch (spell) {
    case Spell::Slow: already = (enemy.buffs & (kSlowed | kHeld | kFrozen)) != 0; break;
    case Spell::Confuse: already = (enemy.buffs & kConfused) != 0; break;
    case Spell::Stun: already = (enemy.buffs & (kHeld | kFrozen)) != 0; break;
    case Spell::Poison: already = enemy.poisoned; break;
    case Spell::Fumble: // (nothing shows: not again on them for a while either)
    case Spell::Charm: break;
    default: return true;
    }
    if (already) {
        return false;
    }
    const auto it = debuffedAt_.find({enemy.netCode, spell});
    return it == debuffedAt_.end() || view.frame - it->second >= kDebuffAgain;
}

bool CasterBrain::has(const BotView& view, std::uint32_t spell)
{
    // Carrying the flag, movement spells don't take: not a cast wasted on them. Wall: never
    // (people find it a nuisance).
    if (spell == Spell::Wall ||
        (view.carryingFlag && (spell == Spell::TeleportToTarget || spell == Spell::Blink || spell == Spell::Haste))) {
        return false;
    }
    const auto it = std::ranges::find(view.spells, spell, &BotView::KnownSpell::id);
    return it != view.spells.end() && view.mana >= it->manaCost;
}

float CasterBrain::splash(std::uint32_t spell)
{
    // How near where it lands we'd be caught too (estimates, with a margin: the blasts' and
    // clouds' reach round the point they're cast at).
    switch (spell) {
    case Spell::Fireball: return 70;
    case Spell::Fist: return 80;
    case Spell::Meteor: return 70;
    case Spell::ToxicCloud: return 110;
    case Spell::Burn: return 50;
    default: return 0;
    }
}

std::uint32_t CasterBrain::blastFrames(std::uint32_t spell)
{
    // How long after the cast where it lands is still dangerous (estimates: a fist or a meteor
    // falls about a second later; a cloud lingers; flames burn a while).
    switch (spell) {
    case Spell::Fist: return 45;
    case Spell::Meteor: return 45;
    case Spell::ToxicCloud: return 180;
    case Spell::Burn: return 90;
    default: return 0;
    }
}

bool CasterBrain::onPage(std::uint32_t spell, Page page) const
{
    const auto& slots = pages()[static_cast<std::size_t>(page)];
    return spell != 0 && std::ranges::find(slots, spell) != slots.end();
}

bool CasterBrain::can(const BotView& view, std::uint32_t spell) const
{
    return has(view, spell) && onPage(spell, page_) && view.frame - pageAt_ >= kPageTurn;
}

bool CasterBrain::turnTo(const BotView& view, Page page)
{
    if (page_ != page) {
        page_ = page;
        pageAt_ = view.frame;
    }
    return view.frame - pageAt_ >= kPageTurn;
}

Page CasterBrain::choosePage(const BotView& view, const SeenPlayer& enemy, float d)
{
    // Hurt: the way out, at once (if there's one we can pay for).
    const auto affordable = [&](Page p) {
        return std::ranges::any_of(pages()[static_cast<std::size_t>(p)], [&](std::uint32_t s) { return s != 0 && has(view, s); });
    };
    if (view.maxHealth > 0 && view.health < hurtLine(rescueAt()) * view.maxHealth && affordable(Page::Defence)) {
        return Page::Defence;
    }
    if (page_ != Page::Defence && pageChosenAt_ != 0 && view.frame - pageChosenAt_ < kPageHold) {  // (on Defence it isn't held: back to the fight's page at once)
        return page_; // (held a moment: no flicking between pages)
    }
    pageChosenAt_ = view.frame;
    // Control: for what it's for (a thief with our flag, a warrior closing in, an enemy's
    // creature for a conjurer's Charm), or now and then for a curse they haven't got.
    if (const auto s = control(view, enemy, d)) {
        const bool thief = enemy.hasFlag && d >= 100;
        const bool closing = !enemy.creature && enemy.playerClass == 0 && d < 130;
        const bool cursing = (*s == Spell::Slow || *s == Spell::Confuse || *s == Spell::Stun || *s == Spell::Poison) &&
                             chance(curse(15));
        if (thief || closing || enemy.creature || cursing) {
            return Page::Control;
        }
    }
    return Page::Attack;
}

namespace {

// Arrows or bolts left in any quiver we carry (an unreported count counts as some).
bool hasBolts(const BotView& view)
{
    // (The server reloads from any quiver we carry when the equipped one runs out, and takes an
    // empty one away: Obj_ShootBowOrCrossbow.)
    return std::ranges::any_of(view.inventory, [](const OwnItem& i) {
        return i.kind == ItemKind::Quiver && (!i.charges || *i.charges > 0);
    });
}

// A shooting staff with charges left (unknown counts as some).
bool charged(const OwnItem& staff)
{
    return staff.kind == ItemKind::Staff && staff.wand && (!staff.charges || *staff.charges > 0);
}

} // namespace

const OwnItem* CasterBrain::launcher(const BotView& view)
{
    const auto it = std::ranges::find_if(view.inventory, [](const OwnItem& i) {
        return i.equipped && ((i.kind == ItemKind::Weapon && i.ranged) || (i.kind == ItemKind::Staff && i.wand));
    });
    return it != view.inventory.end() ? &*it : nullptr;
}

bool CasterBrain::loaded(const BotView& view)
{
    const OwnItem* l = launcher(view);
    return l && (l->kind == ItemKind::Staff ? charged(*l) : hasBolts(view));
}

void CasterBrain::tendGear(const BotView& view, BotBody& body)
{
    // In hand: what shoots best with something to shoot (a conjurer's crossbow with bolts, then
    // a staff with charges by its rank), else what strikes. Not swapped back and forth faster
    // than the server answers.
    const OwnItem* best = nullptr;
    float bestScore = -1;
    for (const auto& i : view.inventory) {
        if (!i.usable || (i.kind != ItemKind::Weapon && i.kind != ItemKind::Staff)) {
            continue;
        }
        // Score: a launcher with something to shoot beats any staff, which beats any striking weapon;
        // an empty launcher scores -1 and is never chosen. Ties keep what's in hand.
        float score = i.value;
        if (i.kind == ItemKind::Weapon && i.ranged) {
            score = hasBolts(view) ? 200 + i.value : -1;
        } else if (i.kind == ItemKind::Staff && i.wand) {
            score = charged(i) ? 100 + i.value : -1;
        }
        if (score > bestScore || (score == bestScore && best && i.equipped)) {
            best = &i;
            bestScore = score;
        }
    }
    if (best && bestScore >= 0 && !best->equipped && view.frame >= nextSwap_) {
        body.equip(best->netCode);
        nextSwap_ = view.frame + 45;
        nextItemAction_ = view.frame + 20;
        return;
    }
    tendArmor(view, body);
}

std::optional<CasterBrain::Cast> CasterBrain::rescue(const BotView&, const SeenPlayer*)
{
    return std::nullopt;
}

std::optional<std::uint32_t> CasterBrain::buff(const BotView&)
{
    return std::nullopt;
}

bool CasterBrain::usesMana() const
{
    return true;
}

void CasterBrain::prepare(const BotView& view)
{
    reach_ = farthest() - 20; // (counts as "got to them" for the chase)
    // A shot: the equipped quiver's count went down (each shot reports it), or came for the
    // first time.
    const OwnItem* quiver = equippedQuiver(view);
    const std::optional<std::uint8_t> bolts = quiver ? quiver->charges : std::nullopt;
    justShot_ = bolts && (!bolts_ || *bolts < *bolts_);
    bolts_ = bolts;
    // Teleported: the way from here is planned again (the old one starts behind us).
    if (teleportedFrom_) {
        if (distance(view.x, view.y, teleportedFrom_->first, teleportedFrom_->second) > 60) {
            nav_.reset();
            teleportedFrom_.reset();
        } else if (view.frame - castAt_ > 30) {
            teleportedFrom_.reset(); // it didn't take
        }
    }
}

bool CasterBrain::cast(const BotView& view, BotBody& body, std::uint32_t spell, float x, float y, bool self)
{
    // (A summoning is from the creature panel, not the bar: any time we have it.)
    const bool summoning = 0x4A < spell && spell < 0x73;  // the summon spell ids lie in this range
    if (!(summoning ? has(view, spell) : can(view, spell))) {
        return false;
    }
    // MSG_TRY_SPELL at the cursor; on ourselves with the slot's "on self" flag too, as the
    // game's own spell bar sends it (guiSpellSetCursor). Without it the server takes whoever is under the
    // cursor: a teammate standing by got our Haste and Shield, and we, never getting them,
    // cast them again and again.
    body.setCursor(static_cast<int>(self ? view.x : x), static_cast<int>(self ? view.y : y));
    const std::array<std::uint32_t, 1> spells{spell};
    body.castSpells(spells, self);
    castAt_ = view.frame;
    lastSpell_ = spell;
    ++castCounts_[spell];
    if (const std::uint32_t lasts = blastFrames(spell); lasts > 0 && !self) {
        blast_ = Blast{x, y, splash(spell), view.frame + lasts};
    }
    attacked(view);
    if (spell == Spell::TeleportToTarget) {
        teleportedFrom_ = std::pair{view.x, view.y};
    }
    return true;
}

bool CasterBrain::cast(const BotView& view, BotBody& body, const Cast& c)
{
    return c.at ? cast(view, body, c.spell, c.at->first, c.at->second) : cast(view, body, c.spell, view.x, view.y, true);
}

void CasterBrain::fight(const BotView& view, BotBody& body, const SeenPlayer& enemy)
{
    const float d = distance(view.x, view.y, enemy.x, enemy.y);
    const bool inSight = sees(view, enemy.x, enemy.y);
    // Ready for a cast: castEvery, plus the skill's slowdown, since the last.
    const bool ready = view.frame - castAt_ >= castEvery() + static_cast<std::uint32_t>(skill_.slowdown);
    body.setAction(false);
    body.setCursor(static_cast<int>(enemy.x), static_cast<int>(enemy.y));
    turnTo(view, choosePage(view, enemy, d)); // (the bar's page the moment calls for)
    // Hurt and pressed: the class's way out (a heal, a teleport) comes first.
    if (ready) {
        if (const auto c = rescue(view, &enemy); c && cast(view, body, *c)) {
            body.setMove(std::nullopt);
            return;
        }
        // A summoner's creature to fight beside it, as the fight starts (not with them on us).
        if (d > 90 && summon(view, body, enemy.x, enemy.y)) {
            return;
        }
    }
    // A missile coming at us: a step across its line (not into lava or a wall).
    if (view.frame < dodgeUntil_) {
        body.setMove(dodgeDirection_);
        return;
    }
    if (const SeenMissile* m = incoming(view, 10 + view.lagFrames, 24); m && chance(skill_.dodgeChance)) {
        const std::uint8_t along = directionTo(m->x, m->y, view.x, view.y);
        if (const auto way = safeWay(view, along, (chance(50) ? 1 : -1) * 64)) {
            dodgeDirection_ = *way;
            dodgeUntil_ = view.frame + 8;
            body.setMove(dodgeDirection_);
            return;
        }
    }
    // A spell at where they'll be when it gets there (missiles fly about 12 a frame).
    bool casted = false;
    const bool clear = inSight && clearBetween(view, enemy.x, enemy.y, true);
    if (ready && clear) {
        const auto s = page_ == Page::Control ? control(view, enemy, d)
                       : page_ == Page::Attack ? attack(view, enemy, d)
                                               : std::nullopt;
        // Lead: our lag plus the spell's flight time (12 units a frame).
        const auto [lx, ly] = lead(enemy, view.lagFrames + d / 12);
        // (Clear of where it lands, and of them too: one rushing us brings it onto us.)
        // (Only with a spell chosen: none, and *s read an empty optional.)
        const auto clearOfIt = [&] { return distance(view.x, view.y, lx, ly) > splash(*s) && d > splash(*s); };
        if (s && can(view, *s) && worthOn(view, enemy, *s) && clearOfIt()) {
            casted = cast(view, body, *s, lx, ly);
            if (casted) {
                debuffedAt_[{enemy.netCode, *s}] = view.frame; // (only debuffs are ever looked up)
            }
        }
    }
    // Our own blast still to come down (or lingering) about us: out of it first. (Not the frame
    // we cast: the run's cursor would move the spell.)
    if (!casted && blast_ && view.frame < blast_->until &&
        distance(view.x, view.y, blast_->x, blast_->y) < blast_->radius + 20) {
        const std::uint8_t away = directionTo(blast_->x, blast_->y, view.x, view.y);
        std::optional<std::uint8_t> way;
        // Straight away, else ever wider turns to either side.
        for (const int turn : {0, 24, 56, 88}) {
            if ((way = safeWay(view, away, turn))) {
                break;
            }
        }
        if (way) {
            body.setMove(way);
            runCursor(view, body, *way);
            return;
        }
    }
    // Else a shot, from far enough (too near, we back off first, unless there's nowhere to back
    // off to: then point-blank); after a taunt-cut crossbow shot, on the move for the reload's
    // time first.
    // A way back: a turn of 104 (well past a quarter) to either side of them.
    const auto backOff = [&] { return safeWay(view, directionTo(view.x, view.y, enemy.x, enemy.y), 104); };
    if (!casted && loaded(view) && clear && d <= kShootRange && (d >= nearest() - 40 || !backOff()) &&
        view.frame >= moveAfterShotUntil_ && clearBetween(view, enemy.x, enemy.y)) {
        shoot(view, body, enemy, d);
        return;
    }
    // No spell to cast now and none coming back soon (no mana for any, none given), and no
    // shot: fight with what's in hand.
    const bool anySpell = std::ranges::any_of(view.spells, [&](const BotView::KnownSpell& s) {
        return s.manaCost > 0 && view.mana >= s.manaCost;
    });
    if (!casted && !anySpell && !loaded(view)) {
        armed(view, body, enemy, d);
        return;
    }
    // At range: in if too far or out of sight, back if too near, else round them.
    if (!inSight || d > farthest()) {
        approach(view, body, enemy, farthest() - 40);
        return;
    }
    nav_.reset();
    if (view.frame >= circleFlipAt_) {
        circleSide_ = chance(50) ? 1 : -1;
        circleFlipAt_ = view.frame + nox::util::uniform_int_distribution<std::uint32_t>(30, 75)(rng_);
    }
    const std::uint8_t toward = directionTo(view.x, view.y, enemy.x, enemy.y);
    const int turn = d < nearest() ? 104 : 64; // back off, or round them
    const auto way = safeWay(view, toward, circleSide_ * turn);
    if (way && *way != static_cast<std::uint8_t>(toward + circleSide_ * turn)) {
        circleSide_ = -circleSide_; // (that side was blocked)
    }
    body.setMove(way);
}

void CasterBrain::shoot(const BotView& view, BotBody& body, const SeenPlayer& enemy, float d)
{
    const OwnItem* bow = launcher(view);
    // The shot's speed for leading: a staff's, a bolt's, or an arrow's.
    const float speed = !bow                          ? kArrowSpeed
                        : bow->kind == ItemKind::Staff ? kStaffShotSpeed
                        : bow->crossbow                ? kBoltSpeed
                                                       : kArrowSpeed;
    // At their feet, where they'll be: a player's position is the foot of the figure, and
    // that's what a shot must cross (not the body drawn above it).
    const auto [lx, ly] = lead(enemy, view.lagFrames + 3 + d / speed);
    // The shot flies the way we face (shootBowCrossbow2): standing, facing there,
    // with the skill's (small) shot error.
    const int error = static_cast<int>(std::lround(skill_.shotError));
    const int off = error > 0 ? nox::util::uniform_int_distribution<int>(-error, error)(rng_) : 0;
    body.setMove(std::nullopt);
    body.setFacing(static_cast<std::uint8_t>((directionTo(view.x, view.y, lx, ly) + off) & 0xFF));
    body.setCursor(static_cast<int>(lx), static_cast<int>(ly));
    // A crossbow shoots as its attack starts (the animation's second frame), then reloads for
    // the rest of it. The taunt ends the attack (Plr_SetState leaves state 1 for it): players
    // cut the reload short that way, when there are bolts left to shoot, and move at once.
    if (bow && bow->crossbow && justShot_ && loaded(view) && chance(skill_.abilityChance)) {
        body.taunt();
        body.setAction(false);
        moveAfterShotUntil_ = view.frame + kMoveAfterShot;
        return;
    }
    body.setAction(true); // (held: the next attack starts as soon as the server allows)
    attacked(view);
}

void CasterBrain::armed(const BotView& view, BotBody& body, const SeenPlayer& enemy, float d)
{
    const auto inHand = std::ranges::find_if(view.inventory, [](const OwnItem& i) {
        return (i.kind == ItemKind::Weapon || i.kind == ItemKind::Staff) && i.equipped;
    });
    if (inHand != view.inventory.end() && !inHand->ranged && !(inHand->kind == ItemKind::Staff && inHand->wand)) {
        // A sword or a wooden staff: in close and swing.
        if (d <= 50) {
            body.setMove(std::nullopt);
            body.setFacing(aimAt(view, enemy.x, enemy.y));
            const bool swing = view.frame - swingAt_ >= 16u + static_cast<std::uint32_t>(skill_.slowdown);
            body.setAction(swing);
            if (swing) {
                swingAt_ = view.frame;
            }
            return;
        }
        approach(view, body, enemy, 40);
        return;
    }
    // Nothing to fight with: off for mana, else away from them until there's some again; with
    // nowhere to go and them on us, fists.
    if (seekMana(view, body)) {
        return;
    }
    const auto away = safeWay(view, directionTo(view.x, view.y, enemy.x, enemy.y), 128);
    body.setMove(away);
    if (!away && d <= 50) {
        body.setFacing(aimAt(view, enemy.x, enemy.y));
        const bool punch = view.frame - swingAt_ >= 12;
        body.setAction(punch);
        if (punch) {
            swingAt_ = view.frame;
            attacked(view);
        }
    }
}

bool CasterBrain::summon(const BotView& view, BotBody& body, float tx, float ty)
{
    if (style_ != Style::Summoner || view.carryingFlag || (summonAt_ != 0 && view.frame - summonAt_ < kSummonEvery)) {
        return false;
    }
    // The creature appears a step (40 units) toward the target.
    const float d = std::max(1.0f, distance(view.x, view.y, tx, ty));
    const float px = view.x + (tx - view.x) / d * 40;
    const float py = view.y + (ty - view.y) / d * 40;
    // Any creature it knows that fits the cage's free cells and leaves kSummonReserve mana to
    // fight with: the stronger a little likelier (by price), the last few called much less so,
    // so it calls all kinds (always the strongest was an ogre or a bear every time). Not the
    // Bomber (a trap's) nor the green wizard.
    std::vector<std::pair<std::uint32_t, float>> callable; // (spell, weight)
    float total = 0;
    for (const auto& s : view.spells) {
        if (s.creatureSize == 0 || s.id == kSummonBomber || s.id == kSummonWizard || s.creatureSize > view.summonRoom ||
            view.mana < s.manaCost + kSummonReserve) {
            continue;
        }
        // Weight by price: cheap creatures 3, middling 2, the dearest (85 mana up) 1.
        float weight = s.manaCost >= 85 ? 1.0f : s.manaCost >= 60 ? 2.0f : 3.0f;
        if (std::ranges::find(recentSummons_, s.id) != recentSummons_.end()) {
            weight *= 0.15f;
        }
        callable.emplace_back(s.id, weight);
        total += weight;
    }
    // A weighted roll among them; one that won't cast is struck off and the roll made again.
    while (!callable.empty()) {
        float roll = nox::util::uniform_real_distribution<float>(0, total)(rng_);
        std::size_t pick = 0;
        while (pick + 1 < callable.size() && roll >= callable[pick].second) {
            roll -= callable[pick].second;
            ++pick;
        }
        if (cast(view, body, callable[pick].first, px, py)) {
            summonAt_ = view.frame;
            recentSummons_.push_back(callable[pick].first);
            if (recentSummons_.size() > 3) {
                recentSummons_.erase(recentSummons_.begin());
            }
            return true;
        }
        total -= callable[pick].second;
        callable.erase(callable.begin() + static_cast<std::ptrdiff_t>(pick));
    }
    return false;
}

bool CasterBrain::idle(const BotView& view, BotBody& body)
{
    // At a Wall spell's wall in the way: blasting it (the Attack page), not buffing.
    if (wallBreakAt_ != 0 && view.frame - wallBreakAt_ <= 30) {
        return false;
    }
    // Out of a fight: the bar's Defence page (the buffs, the heal, a wizard's teleports).
    const bool defending = turnTo(view, Page::Defence);
    if (view.frame - castAt_ < castEvery()) {
        return false;
    }
    // A summoner keeps a creature by it, mana allowing (most of it: the fight comes first).
    if (view.maxMana > 0 && view.mana >= 0.7f * view.maxMana) {
        const auto [fx, fy] = unitOf(directionTo(view.x, view.y, view.x + 1, view.y));  // direction 0: east of us
        if (summon(view, body, view.x + fx * 100, view.y + fy * 100)) {
            return true;
        }
    }
    if (!defending) {
        return false; // (turning to it: on with whatever else meanwhile)
    }
    if (const auto c = rescue(view, nullptr)) {
        return cast(view, body, *c);
    }
    if (const auto s = buff(view)) {
        // The same buff not again for a while whatever we see of it (it lasts longer than that;
        // if it didn't take, mana is better kept for the fight).
        if (const auto it = buffedAt_.find(*s); it != buffedAt_.end() && view.frame - it->second < kBuffAgain) {
            return false;
        }
        if (cast(view, body, *s, view.x, view.y, true)) {
            buffedAt_[*s] = view.frame;
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------ WizardBrain

WizardBrain::WizardBrain(Skill skill, std::uint32_t seed)
    : CasterBrain(skill, seed)
{
}

const SpellPages WizardBrain::kPages{{
    {Spell::DeathRay, Spell::Fireball, Spell::MagicMissile, Spell::Burn, Spell::Shock},
    {Spell::Slow, Spell::Confuse, Spell::Fumble, Spell::Pull, Spell::Push},
    {Spell::Shield, Spell::Haste, Spell::TeleportToTarget, Spell::Blink, Spell::DrainMana},
}};

const SpellPages& WizardBrain::pages() const
{
    return kPages;
}

namespace {

// What some spells need of their target (bit n = ENCHANT n).
constexpr std::uint32_t kStuck = (1u << 4) | (1u << 5) | (1u << 25); ///< slowed, held, frozen: can't step out of it

} // namespace

std::optional<std::uint32_t> WizardBrain::attack(const BotView& view, const SeenPlayer& enemy, float d)
{
    // (No traps: a wizard bot never lays Glyphs.)
    if (d <= 220 && can(view, Spell::DeathRay) && chance(25)) {
        return Spell::DeathRay;
    }
    if (d <= 200 && ((enemy.buffs & kStuck) || enemy.creature) && can(view, Spell::Burn) && chance(35)) {
        return Spell::Burn; // (flames under one who can't walk out of them)
    }
    if (d >= 70 && d <= 300 && can(view, Spell::Fireball) && chance(45)) {
        return Spell::Fireball;
    }
    if (d <= 300 && can(view, Spell::MagicMissile)) {
        return Spell::MagicMissile;
    }
    if (d <= 300 && can(view, Spell::Fireball)) {
        return Spell::Fireball;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> WizardBrain::control(const BotView& view, const SeenPlayer& enemy, float d)
{
    const auto fits = [&](std::uint32_t s) { return has(view, s) && worthOn(view, enemy, s); };
    // A thief running off with our flag: slowed, else pulled back to us.
    if (enemy.hasFlag && d >= 100 && d <= 280) {
        for (const auto s : {Spell::Slow, Spell::Pull}) {
            if (fits(s)) {
                return s;
            }
        }
    }
    // A warrior closing in: shoved off at hand, else made to drop his weapon, else slowed.
    if (!enemy.creature && enemy.playerClass == 0 && d < 130) {
        if (d < 70 && fits(Spell::Push)) {
            return Spell::Push;
        }
        for (const auto s : {Spell::Fumble, Spell::Slow}) {
            if (fits(s)) {
                return s;
            }
        }
    }
    // Curses they haven't got.
    if (d <= 200) {
        for (const auto s : {Spell::Slow, Spell::Confuse}) {
            if (fits(s)) {
                return s;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::pair<float, float>> WizardBrain::awayFrom(const BotView& view, const SeenPlayer& enemy) const
{
    // Teleport to Target goes to the cursor: somewhere on our screen, in sight, on safe ground,
    // as far from them as that allows.
    const float d = distance(view.x, view.y, enemy.x, enemy.y);
    std::optional<std::pair<float, float>> best;
    float bestD = d + 100; // (worth a teleport only if it gets us well clear)
    for (const float r : {260.0f, 200.0f, 150.0f}) {
        for (int k = 0; k < 16; ++k) {
            const auto [ux, uy] = unitOf(static_cast<std::uint8_t>(k * 16));
            const float x = view.x + ux * r;
            const float y = view.y + uy * r;
            if (std::abs(x - view.x) > view.viewHalfWidth - 20 || std::abs(y - view.y) > view.viewHalfHeight - 20 ||
                hazardAt(view, x, y) || blockedAt(view, x, y) || !sees(view, x, y)) {
                continue;
            }
            // (clear of them, and of any other enemy about: not out of one's reach into another's)
            const float fromThem = std::min(distance(x, y, enemy.x, enemy.y), nearestEnemy(view, x, y));
            if (fromThem > bestD) {
                best = std::pair{x, y};
                bestD = fromThem;
            }
        }
    }
    return best;
}

std::optional<CasterBrain::Cast> WizardBrain::rescue(const BotView& view, const SeenPlayer* enemy)
{
    if (!enemy) {
        return std::nullopt;
    }
    const float d = distance(view.x, view.y, enemy->x, enemy->y);
    if (view.maxHealth > 0 && view.health < hurtLine(0.35f) * view.maxHealth && d < 150) {
        // Away: Teleport to Target out of their reach, else Blink (anywhere on the map).
        if (can(view, Spell::TeleportToTarget)) {
            if (const auto to = awayFrom(view, *enemy)) {
                return Cast{Spell::TeleportToTarget, to};
            }
        }
        if (can(view, Spell::Blink)) {
            return Cast{Spell::Blink, std::nullopt};
        }
    }
    if (d < 70 && !(view.buffs & kShocked) && can(view, Spell::Shock)) {
        return Cast{Spell::Shock, std::nullopt}; // whoever touches us now is hit
    }
    return std::nullopt;
}

std::optional<std::uint32_t> WizardBrain::buff(const BotView& view)
{
    if (!(view.buffs & kShielded) && can(view, Spell::Shield)) {
        return Spell::Shield;
    }
    if (!(view.buffs & kHasted) && can(view, Spell::Haste)) {
        return Spell::Haste;
    }
    return std::nullopt;
}

float WizardBrain::nearestEnemy(const BotView& view, float x, float y) const
{
    float best = 1e9f;
    for (const auto* list : {&view.players, &view.creatures}) {
        for (const auto& p : *list) {
            if (isEnemy(view, p)) {
                best = std::min(best, distance(x, y, p.x, p.y));
            }
        }
    }
    return best;
}

bool WizardBrain::shortcut(const BotView& view, BotBody& body, float x, float y)
{
    if (view.carryingFlag || view.frame < nextTeleport_ || view.maxMana == 0 || !can(view, Spell::TeleportToTarget)) {
        return false;
    }
    // Not on the way to an enemy (a chase walks): Teleport to Target isn't for jumping on people.
    if (nearestEnemy(view, x, y) < kTeleportNotAt) {
        return false;
    }
    // Nor to anywhere near one.
    const auto clearOfThem = [&](float px, float py) { return nearestEnemy(view, px, py) >= kTeleportKeepOff; };
    const float straight = distance(view.x, view.y, x, y);
    const bool noWay = nav_.unreachable();
    // Half the mana kept for a fight, unless walking can't get there at all.
    if (straight <= 150 || (!noWay && view.mana < 0.5f * view.maxMana) || view.frame - teleportLookAt_ < 15) {
        return false;
    }
    teleportLookAt_ = view.frame; // (a look twice a second at most: it plans paths)
    const float walk = noWay ? 1e9f : nav_.remaining(view);
    const auto go = [&](std::pair<float, float> to) {
        if (!clearOfThem(to.first, to.second) || !cast(view, body, Spell::TeleportToTarget, to.first, to.second)) {
            return false;
        }
        nextTeleport_ = view.frame + 60;
        body.setMove(std::nullopt);
        return true;
    };
    // Over whatever's between that sight crosses and feet don't (a river, the void, a window,
    // bars): the landing in sight toward the goal, within reach, on floor, that leaves the least
    // to walk, when that cuts the walk by a good stretch (or there's no walking there at all).
    if (view.nav && !view.nav->empty()) {
        std::optional<std::pair<float, float>> best;
        float bestAfter = 0;
        const std::uint8_t toward = directionTo(view.x, view.y, x, y);
        // Straight toward the goal, then a little to either side (8 and 16 of 256).
        for (const int off : {0, 8, -8, 16, -16}) {
            const auto [ux, uy] = unitOf(static_cast<std::uint8_t>(toward + off));
            // From the furthest out inward in 25s: the first that's seen, open and clear of enemies.
            for (float r = std::min(kTeleportReach, straight + 20); r >= 100; r -= 25) {
                const float px = view.x + ux * r;
                const float py = view.y + uy * r;
                if (!sees(view, px, py) || blockedAt(view, px, py) || hazardAt(view, px, py) || !clearOfThem(px, py)) {
                    continue;
                }
                const float after = distance(px, py, x, y);
                if (!best || after < bestAfter) {
                    best = std::pair{px, py};
                    bestAfter = after;
                }
                break; // (the furthest that way)
            }
        }
        if (best) {
            // How far is left to walk from that landing (the path's length).
            const auto path = view.nav->findPath({static_cast<int>(best->first), static_cast<int>(best->second)},
                                                 {static_cast<int>(x), static_cast<int>(y)});
            const float length = Navigator::pathLength(*best, path);
            if (!path.empty() && walk - length >= kTeleportSaves) {
                return go(*best);
            }
        }
    }
    // Else a long way to walk: along it, as far as we can see of it.
    if (!noWay && walk > kTeleportReach) {
        if (const auto to = nav_.farthestInSight(view, kTeleportReach);
            to && distance(view.x, view.y, to->first, to->second) >= 150) {
            return go(*to);
        }
    }
    return false;
}

void CasterBrain::hitWall(const BotView& view, BotBody& body, float wx, float wy)
{
    // A blast at it (the explosion wears the walls round it down: mapDamageUnitsAround,
    // far faster than blows), from the Attack page, from off (wallStandOff: not from beside it,
    // the blast would catch us too); only blows with none to spare.
    const bool clearOfIt = distance(view.x, view.y, wx, wy) > kWallBlastRange - 25;
    const bool blasts = clearOfIt && std::ranges::any_of(kWallBlasts, [&](std::uint32_t s) { return has(view, s); });
    if (!blasts) {
        PlayerBrain::hitWall(view, body, wx, wy);
        return;
    }
    body.setAction(false);
    body.setFacing(directionTo(view.x, view.y, wx, wy));
    if (turnTo(view, Page::Attack) && view.frame - castAt_ >= castEvery()) {
        for (const auto s : kWallBlasts) {
            if (can(view, s) && cast(view, body, s, wx, wy)) {
                return;
            }
        }
    }
}

float CasterBrain::wallStandOff(const BotView& view) const
{
    return std::ranges::any_of(kWallBlasts, [&](std::uint32_t s) { return has(view, s); }) ? kWallBlastRange
                                                                                         : PlayerBrain::wallStandOff(view);
}

bool WizardBrain::drains(const BotView& view) const
{
    return knows(view, Spell::DrainMana);
}

bool WizardBrain::recharge(const BotView& view, BotBody& body, const BotView::ManaSource& s, float d)
{
    if (!drains(view) || d > view.numbers.drainRange - 30 || !clearBetween(view, s.x, s.y, true)) {
        return PlayerBrain::recharge(view, body, s, d);
    }
    // Drain Mana: it draws from the nearest source in sight within ManaDrainRange (BotView::numbers)
    // for as long as we stand (moving 5 from where it was cast ends it) and aren't full (spellDrainMana).
    // Cast again when the mana stops coming.
    body.setMove(std::nullopt);
    body.setAction(false);
    if (turnTo(view, Page::Defence) && view.frame - drainAt_ >= 45 && view.frame - manaGainAt_ > 15 &&
        cast(view, body, Spell::DrainMana, s.x, s.y)) {
        drainAt_ = view.frame;
    }
    return true;
}

// ------------------------------------------------------------------ ConjurerBrain

ConjurerBrain::ConjurerBrain(Skill skill, std::uint32_t seed)
    : CasterBrain(skill, seed)
{
}

const SpellPages ConjurerBrain::kPages{{
    // (Not Force of Nature: its ball bounces off the walls and came back on its caster.)
    {Spell::Fist, Spell::PixieSwarm, Spell::Meteor, Spell::Burn, Spell::ToxicCloud},
    {Spell::Stun, Spell::Slow, Spell::Poison, Spell::Charm, 0},
    {Spell::LesserHeal, Spell::CurePoison, Spell::Vampirism, Spell::Infravision, 0},
}};

const SpellPages& ConjurerBrain::pages() const
{
    return kPages;
}

std::optional<std::uint32_t> ConjurerBrain::attack(const BotView& view, const SeenPlayer& enemy, float d)
{
    // With a loaded crossbow the shots are the staple; spells now and then between them.
    const bool shooting = loaded(view) && d <= kShootRange;
    if (d <= 220 && ((enemy.buffs & kStuck) || enemy.creature) && can(view, Spell::ToxicCloud) && chance(30)) {
        return Spell::ToxicCloud; // (on one who can't walk out of it)
    }
    if (d <= 200 && ((enemy.buffs & kStuck) || enemy.creature) && can(view, Spell::Burn) && chance(25)) {
        return Spell::Burn; // (flames under one who can't walk out of them)
    }
    if (d <= 260 && can(view, Spell::Fist) && chance(shooting ? 25 : 35)) {
        return Spell::Fist; // where they'll be: it falls from above
    }
    if (d <= 260 && can(view, Spell::Meteor) && chance(shooting ? 15 : 30)) {
        return Spell::Meteor; // (the same, smaller and cheaper)
    }
    if (shooting) {
        return d <= 300 && can(view, Spell::PixieSwarm) && chance(15) ? std::optional{Spell::PixieSwarm} : std::nullopt;
    }
    for (const auto s : {Spell::PixieSwarm, Spell::Meteor, Spell::Fist}) {
        if (d <= (s == Spell::PixieSwarm ? 300.0f : 260.0f) && can(view, s)) {
            return s;
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> ConjurerBrain::control(const BotView& view, const SeenPlayer& enemy, float d)
{
    const auto fits = [&](std::uint32_t s) { return has(view, s) && worthOn(view, enemy, s); };
    // An enemy's creature: ours instead.
    if (enemy.creature) {
        return d <= 200 && fits(Spell::Charm) ? std::optional{Spell::Charm} : std::nullopt;
    }
    // A thief with our flag, a warrior closing in, or a curse they haven't got.
    const bool urgent = (enemy.hasFlag && d >= 100 && d <= 280) || (enemy.playerClass == 0 && d < 130);
    if (urgent || d <= 200) {
        for (const auto s : {Spell::Stun, Spell::Slow, Spell::Poison}) {
            if (fits(s) && (s != Spell::Poison || !urgent)) {
                return s;
            }
        }
    }
    return std::nullopt;
}

std::optional<CasterBrain::Cast> ConjurerBrain::rescue(const BotView& view, const SeenPlayer*)
{
    if (view.maxHealth > 0 && view.health < hurtLine(rescueAt()) * view.maxHealth && can(view, Spell::LesserHeal)) {
        return Cast{Spell::LesserHeal, std::nullopt};
    }
    return std::nullopt;
}

std::optional<std::uint32_t> ConjurerBrain::buff(const BotView& view)
{
    if (view.maxHealth > 0 && view.health < hurtLine(0.8f) * view.maxHealth && can(view, Spell::LesserHeal)) {
        return Spell::LesserHeal;
    }
    if (view.poisoned && can(view, Spell::CurePoison)) {
        return Spell::CurePoison;
    }
    if (!(view.buffs & kVampiric) && can(view, Spell::Vampirism)) {
        return Spell::Vampirism; // (our blows heal us)
    }
    if (!(view.buffs & kInfravision) && can(view, Spell::Infravision)) {
        return Spell::Infravision; // the invisible show
    }
    return std::nullopt;
}

bool ConjurerBrain::underGeared(const BotView& view) const
{
    // Nothing to shoot with (a crossbow or bow with bolts, a staff with charges): that first,
    // where the map has one.
    const bool bow = std::ranges::any_of(view.inventory, [](const OwnItem& i) { return i.ranged && i.usable; });
    const bool staff = std::ranges::any_of(view.inventory, [](const OwnItem& i) { return i.usable && charged(i); });
    return !staff && (!bow || !hasBolts(view));
}

} // namespace nox::bot
