// The class-independent half of a bot's play (player_brain.hpp): the navigator that walks the
// wall grid's paths, target choice, items, potions and gear, mana, flags and team roles. The
// class brains (warrior_brain.cpp, caster_brain.cpp) fill in the fight through its hooks.
#include "noxcore/bot/player_brain.hpp"

#include "noxcore/bot/personas.hpp"
#include "noxcore/client/navigation.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace nox::bot {

namespace {

// How many items of `kind` we carry.
std::size_t carried(const BotView& view, ItemKind kind)
{
    return static_cast<std::size_t>(
        std::ranges::count_if(view.inventory, [&](const OwnItem& i) { return i.kind == kind; }));
}

// The wall grid's cell holding the point.
std::pair<int, int> cellOf(float x, float y)
{
    return {static_cast<int>(x) / client::NavGrid::kCellSize, static_cast<int>(y) / client::NavGrid::kCellSize};
}

} // namespace

// ------------------------------------------------------------------ helpers

float PlayerBrain::distance(float ax, float ay, float bx, float by)
{
    return std::hypot(ax - bx, ay - by);
}

bool PlayerBrain::clearBetween(const BotView& view, float x, float y, bool sight, bool safe)
{
    return !view.nav || view.nav->empty() ||
           view.nav->clearLine({static_cast<int>(view.x), static_cast<int>(view.y)},
                               {static_cast<int>(x), static_cast<int>(y)}, sight, safe);
}

bool PlayerBrain::sees(const BotView& view, float x, float y)
{
    return std::abs(x - view.x) <= view.viewHalfWidth && std::abs(y - view.y) <= view.viewHalfHeight &&
           clearBetween(view, x, y, true);
}

void PlayerBrain::runCursor(const BotView& view, BotBody& body, std::uint8_t direction)
{
    // Past 100 units the server runs us (Plr_UpdateState).
    // Directions run 0-255 round the circle (64 a quarter turn, 0 east).
    const double a = direction * 2.0 * std::numbers::pi / 256.0;
    body.setCursor(static_cast<int>(view.x + 160 * std::cos(a)), static_cast<int>(view.y + 160 * std::sin(a)));
}

bool PlayerBrain::hazardAt(const BotView& view, float x, float y)
{
    return view.nav && x >= 0 && y >= 0 &&
           view.nav->hazard(static_cast<int>(x) / client::NavGrid::kCellSize, static_cast<int>(y) / client::NavGrid::kCellSize);
}

bool PlayerBrain::blockedAt(const BotView& view, float x, float y)
{
    return view.nav && !view.nav->empty() &&
           (x < 0 || y < 0 ||
            view.nav->blocked(static_cast<int>(x) / client::NavGrid::kCellSize, static_cast<int>(y) / client::NavGrid::kCellSize));
}

std::pair<float, float> PlayerBrain::unitOf(std::uint8_t direction)
{
    const double a = direction * 2.0 * std::numbers::pi / 256.0;
    return {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
}

bool PlayerBrain::openRun(const BotView& view, std::uint8_t direction, float upTo)
{
    const auto [ux, uy] = unitOf(direction);
    for (float r = 12; r <= upTo; r += 12) {
        if (hazardAt(view, view.x + ux * r, view.y + uy * r) || blockedAt(view, view.x + ux * r, view.y + uy * r)) {
            return false;
        }
    }
    return true;
}

std::optional<std::uint8_t> PlayerBrain::safeWay(const BotView& view, std::uint8_t toward, int turn)
{
    for (const int s : {turn, -turn}) {
        const auto d = static_cast<std::uint8_t>(toward + s);
        if (openRun(view, d, 72)) {
            return d;
        }
    }
    return std::nullopt;
}

void PlayerBrain::follow(std::map<std::uint16_t, Track>& tracks, std::uint16_t netCode, float x, float y,
                         std::uint32_t frame)
{
    // A first sighting starts a track with no velocity yet.
    auto [it, fresh] = tracks.try_emplace(netCode, Track{x, y, frame, 0, 0, false});
    Track& t = it->second;
    if (fresh || frame <= t.frame) {
        return;
    }
    const auto dt = static_cast<float>(frame - t.frame);
    if (dt > 6) {
        t.vx = 0;
        t.vy = 0; // seen again after a while: start over
        t.measured = false;
    } else if (!t.measured) {
        t.vx = (x - t.x) / dt; // the first step as it is
        t.vy = (y - t.y) / dt;
        t.measured = true;
    } else {
        // Smoothed: the server's steps are uneven (6/8/10 units a frame running).
        t.vx = 0.5f * t.vx + 0.5f * (x - t.x) / dt;
        t.vy = 0.5f * t.vy + 0.5f * (y - t.y) / dt;
    }
    t.x = x;
    t.y = y;
    t.frame = frame;
}

std::pair<float, float> PlayerBrain::lead(const SeenPlayer& enemy, float frames) const
{
    const auto it = players_.find(enemy.netCode);
    if (it == players_.end()) {
        return {enemy.x, enemy.y};
    }
    return {enemy.x + it->second.vx * frames, enemy.y + it->second.vy * frames};
}

const SeenMissile* PlayerBrain::incoming(const BotView& view, float frames, float radius) const
{
    for (const auto& m : view.missiles) {
        const auto it = missiles_.find(m.netCode);
        if (it == missiles_.end()) {
            continue;
        }
        const float vx = it->second.vx;
        const float vy = it->second.vy;
        const float speed2 = vx * vx + vy * vy;
        if (speed2 < 4) {
            continue; // not flying (or not yet measured)
        }
        const float rx = view.x - m.x;
        const float ry = view.y - m.y;
        const float t = (rx * vx + ry * vy) / speed2; // frames to its closest pass
        if (t <= 0 || t > frames) {
            continue; // going away, or not here yet
        }
        if (std::hypot(rx - vx * t, ry - vy * t) <= radius) {
            return &m;
        }
    }
    return nullptr;
}

std::uint8_t PlayerBrain::aimAt(const BotView& view, float x, float y)
{
    // A random error of up to aimError direction units either way.
    const int error = static_cast<int>(std::lround(skill_.aimError));
    const int off = error > 0 ? nox::util::uniform_int_distribution<int>(-error, error)(rng_) : 0;
    return static_cast<std::uint8_t>((directionTo(view.x, view.y, x, y) + off) & 0xFF);
}

bool PlayerBrain::chance(int percent)
{
    return nox::util::uniform_int_distribution<int>(0, 99)(rng_) < percent;
}

// ------------------------------------------------------------------ the navigator

std::optional<std::uint8_t> Navigator::steer(const BotView& view, float gx, float gy, float arrive)
{
    const auto distance = [](float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); };
    if (!view.placed || distance(view.x, view.y, gx, gy) <= arrive) {
        path_.clear();
        stuckFrames_ = 0;
        backOutUntil_.reset();
        return std::nullopt;
    }
    // Progress: moving at least 8 units now and then. (Not the sidestep's own way, nor backing
    // out: in a pocket a sidestep moves us, and that counted as getting on, so we turned back
    // into the same wall for ever.)
    const bool sidestepping = stuckFrames_ >= 30 && stuckFrames_ < 50;
    const bool backingOut = backOutUntil_ && view.frame < *backOutUntil_;
    if (!backingOut) { // (backing out: counted from where it ends)
        if (!sidestepping && distance(view.x, view.y, progressFrom_.first, progressFrom_.second) > 8) {
            progressFrom_ = {view.x, view.y};
            stuckFrames_ = 0;
        } else {
            ++stuckFrames_;
            if (stuckFrames_ == 50) {
                progressFrom_ = {view.x, view.y}; // (from where the sidestep left us)
            }
        }
    }
    const bool goalMoved = distance(gx, gy, goal_.first, goal_.second) > 32;
    // Pressed against something for a while without getting on (a wall or door the grid has
    // open): the cell just ahead is planned round for a while.
    std::erase_if(bumped_, [&](const auto& b) { return view.frame >= b.second; });
    if (stuckFrames_ == 45 && !path_.empty() && view.nav && !view.nav->empty()) {
        const auto [ux, uy] = PlayerBrain::unitOf(
            directionTo(view.x, view.y, float(path_.front().first), float(path_.front().second)));
        const auto here = cellOf(view.x, view.y);
        const auto ahead = cellOf(view.x + ux * 16, view.y + uy * 16);
        if (ahead != here) {
            bumped_[ahead] = view.frame + kBumpFrames;
        }
    }
    // Still stuck after the sidesteps (both sides walled too: a corner walled on three sides):
    // those cells planned round as well, and out the way we came for a moment (a new way is
    // planned after), not pressed into the same corner for ever.
    if (stuckFrames_ == kBackOutAt && !backOutUntil_ && view.nav && !view.nav->empty()) {
        const std::uint8_t heading = path_.empty() ? directionTo(view.x, view.y, gx, gy)
                                                   : directionTo(view.x, view.y, float(path_.front().first),
                                                                 float(path_.front().second));
        const auto here = cellOf(view.x, view.y);
        for (const int turn : {64, -64}) {
            const auto [ux, uy] = PlayerBrain::unitOf(static_cast<std::uint8_t>(heading + turn));
            const auto side = cellOf(view.x + ux * 16, view.y + uy * 16);
            if (side != here) {
                bumped_[side] = view.frame + kBumpFrames;
            }
        }
        // Straight back, else the nearest to it the grid has open (and no hazard) a step or two.
        backOutDirection_ = static_cast<std::uint8_t>(heading + 128);
        for (const int off : {0, 32, -32, 64, -64}) {
            const auto d = static_cast<std::uint8_t>(heading + 128 + off * side_);
            if (PlayerBrain::openRun(view, d, 48)) {
                backOutDirection_ = d;
                break;
            }
        }
        backOutUntil_ = view.frame + kBackOutFrames;
        path_.clear(); // (planned again, round what we ran into, once out)
    }
    if (backOutUntil_) {
        if (view.frame < *backOutUntil_) {
            return backOutDirection_;
        }
        backOutUntil_.reset();
        progressFrom_ = {view.x, view.y}; // (a fresh start from here, on a new way)
        stuckFrames_ = 0;
    }
    // The cells we bumped into, as the avoid set findPath takes.
    avoid_.clear();
    for (const auto& [cell, until] : bumped_) {
        avoid_.insert(cell);
    }
    // (At the end of a way that doesn't get there: planned again only now and then, as a search
    // with no way through looks at everywhere it can reach.)
    if ((path_.empty() && !unreachable_) || goalMoved || view.frame - plannedAt_ > 45 || stuckFrames_ == 45) {
        const std::pair<int, int> from{static_cast<int>(view.x), static_cast<int>(view.y)};
        const std::pair<int, int> to{static_cast<int>(gx), static_cast<int>(gy)};
        // No way there (a Wall spell's wall on the bridge, a river): as near as we can get, where
        // a wall can be broken, or a wizard can teleport across.
        bool complete = true;
        path_ = view.nav && !view.nav->empty()
                    ? view.nav->findPath(from, to, avoid_.empty() ? nullptr : &avoid_, &complete)
                    : std::vector{to};
        goal_ = {gx, gy};
        plannedAt_ = view.frame;
        unreachable_ = !complete || path_.empty();
        if (path_.empty()) {
            return std::nullopt; // no way there, nor nearer
        }
    }
    // Waypoints already reached (within 12 units) are dropped.
    while (!path_.empty() && distance(view.x, view.y, float(path_.front().first), float(path_.front().second)) <= 12) {
        path_.erase(path_.begin());
    }
    if (path_.empty()) {
        return std::nullopt;
    }
    // Next onto an elevator whose car is up (or going): too high to step onto. Wait beside it
    // for it to come down (it does on its own every few seconds), not walk into it; one that
    // never comes is planned round for a while.
    for (const auto& car : view.elevatorCars) {
        const float toCar = distance(view.x, view.y, car.x, car.y);
        if (!car.down && distance(float(path_.front().first), float(path_.front().second), car.x, car.y) <= 24 &&
            toCar > 20 && toCar < 80) {
            if (!carWaitSince_) {
                carWaitSince_ = view.frame;
            }
            if (view.frame - *carWaitSince_ > kCarPatience) {
                bumped_[cellOf(car.x, car.y)] = view.frame + kBumpFrames;
                carWaitSince_.reset();
                path_.clear(); // (planned again next frame, round it)
                return std::nullopt;
            }
            stuckFrames_ = 0;
            progressFrom_ = {view.x, view.y};
            plannedAt_ = view.frame; // (no replanning while we wait)
            return std::nullopt;
        }
    }
    carWaitSince_.reset(); // (no car up in the way: every one that is returns above)
    // On a ride's pad with its exit next: stand and wait to be carried (a teleporter at once,
    // an elevator once it goes down), not walk off it toward where it leads.
    if (view.nav) {
        const auto* link = view.nav->linkAt({static_cast<int>(view.x), static_cast<int>(view.y)});
        // (Stepped off a pentagram to touch it again: still waiting on it.)
        if (!link && waitingOn_ && waitingSince_ &&
            distance(view.x, view.y, float(waitingOn_->from.first), float(waitingOn_->from.second)) < 30) {
            link = &*waitingOn_;
        }
        if (link && distance(float(link->to.first), float(link->to.second), float(path_.front().first),
                             float(path_.front().second)) <= 30) {
            if (!waitingSince_) {
                waitingSince_ = view.frame;
                waitingOn_ = *link;
            }
            // (Its patience: an elevator coming back down, a whole cycle; a ride a script has
            // switched off isn't waited on for ever.)
            if (view.frame - *waitingSince_ <= link->patienceFrames) {
                stuckFrames_ = 0;
                progressFrom_ = {view.x, view.y};
                plannedAt_ = view.frame; // (no replanning while we wait)
                if (link->restep) {
                    // A pentagram cooling down won't start on someone standing still: each
                    // second, a few steps off it and back to its middle, so the touch that
                    // starts it comes once it's ready (and we're on it 8 frames later).
                    const std::uint32_t phase = (view.frame - *waitingSince_) % 30;
                    const float cx = float(link->from.first);
                    const float cy = float(link->from.second);
                    if (phase >= 18 && phase < 21) {
                        return distance(view.x, view.y, cx, cy) < 2 ? std::uint8_t{0}
                                                                    : directionTo(cx, cy, view.x, view.y);
                    }
                    if (phase >= 21 && distance(view.x, view.y, cx, cy) > 4) {
                        return directionTo(view.x, view.y, cx, cy);
                    }
                }
                return std::nullopt;
            }
        } else {
            waitingSince_.reset();
            waitingOn_.reset();
        }
    }
    const std::uint8_t direction = directionTo(view.x, view.y, float(path_.front().first), float(path_.front().second));
    // Pressed against something the grid doesn't know (a player, a monster, a moved object):
    // a step to one side for a moment, the other side next time.
    if (stuckFrames_ == 30) {
        side_ = -side_;
    }
    if (stuckFrames_ >= 30 && stuckFrames_ < 50) {
        return static_cast<std::uint8_t>(direction + side_ * 64);  // a quarter turn to the side
    }
    return direction;
}

float Navigator::hazardAhead(const BotView& view) const
{
    if (!view.nav || view.nav->empty() || path_.empty()) {
        return 0;
    }
    float longest = 0;
    float run = 0;
    std::pair<float, float> from{view.x, view.y};
    for (const auto& [px, py] : path_) {
        const std::pair<float, float> to{float(px), float(py)};
        const float length = std::hypot(from.first - to.first, from.second - to.second);
        // Sampled every 4 units along each leg: the longest unbroken stretch of hazard.
        for (float t = 0; t < length; t += 4) {
            const float x = from.first + (to.first - from.first) * t / length;
            const float y = from.second + (to.second - from.second) * t / length;
            run = PlayerBrain::hazardAt(view, x, y) ? run + 4 : 0;
            longest = std::max(longest, run);
        }
        from = to;
    }
    return longest;
}

float Navigator::remaining(const BotView& view) const
{
    return pathLength({view.x, view.y}, path_);
}

float Navigator::pathLength(std::pair<float, float> from, std::span<const std::pair<int, int>> path)
{
    float length = 0;
    for (const auto& [px, py] : path) {
        length += std::hypot(float(px) - from.first, float(py) - from.second);
        from = {float(px), float(py)};
    }
    return length;
}

std::optional<std::pair<float, float>> Navigator::farthestInSight(const BotView& view, float maxDistance) const
{
    // Along each leg of the way in steps (a leg across open ground can be long), until past
    // `maxDistance` from us.
    std::optional<std::pair<float, float>> best;
    std::pair<float, float> from{view.x, view.y};
    for (const auto& [px, py] : path_) {
        const std::pair<float, float> to{float(px), float(py)};
        const float length = std::hypot(to.first - from.first, to.second - from.second);
        for (float t = 16; t <= length; t += 16) {
            const float x = from.first + (to.first - from.first) * t / length;
            const float y = from.second + (to.second - from.second) * t / length;
            if (std::hypot(x - view.x, y - view.y) > maxDistance) {
                return best; // (further along is further away, near enough)
            }
            if (PlayerBrain::sees(view, x, y) && !PlayerBrain::hazardAt(view, x, y) && !PlayerBrain::blockedAt(view, x, y)) {
                best = std::pair{x, y};
            }
        }
        from = to;
    }
    return best;
}

// ------------------------------------------------------------------ the class's defaults

PlayerBrain::PlayerBrain(Skill skill, std::uint32_t seed)
    : skill_(skill)
    , baseSkill_(skill)
    , rng_(seed)
{
}

bool PlayerBrain::react(const BotView&, BotBody&)
{
    return false;
}

bool PlayerBrain::idle(const BotView&, BotBody&)
{
    return false;
}

bool PlayerBrain::underGeared(const BotView&) const
{
    return false;
}

bool PlayerBrain::usesMana() const
{
    return false;
}

bool PlayerBrain::shortcut(const BotView&, BotBody&, float, float)
{
    return false;
}

bool PlayerBrain::drains(const BotView&) const
{
    return false;
}

float PlayerBrain::wallStandOff(const BotView&) const
{
    return kWallReach;
}

void PlayerBrain::hitWall(const BotView& view, BotBody& body, float wx, float wy)
{
    // A swing or a punch: the blow damages the walls ahead of it (playerTraceAttack
    // into mapDamageToWalls), a magic wall's hit points worn down until it breaks.
    // A press each time a blow can come round (a heavy weapon's later), with the breath for it,
    // not a held Action. (What's in hand is the best we carry: a WarHammer, if we have one, wears
    // a wall down far faster than a Longsword or fists.)
    body.setFacing(directionTo(view.x, view.y, wx, wy));
    const auto weapon = std::ranges::find_if(view.inventory, [](const OwnItem& i) { return i.kind == ItemKind::Weapon && i.equipped; });
    const bool heavy = weapon != view.inventory.end() && weapon->value >= 100;  // a WarHammer or GreatSword (value 100 up)
    const std::uint8_t breath = weapon != view.inventory.end() ? weapon->swingStamina : 0;
    // Frames between blows: a heavy weapon's come round slower.
    const bool blow = view.frame - wallBlowAt_ >= (heavy ? 24u : 16u) && view.stamina >= breath;
    body.setAction(blow);
    if (blow) {
        wallBlowAt_ = view.frame;
        attacked(view);
    }
}

bool PlayerBrain::breakMagicWall(const BotView& view, BotBody& body, float gx, float gy)
{
    if (view.magicWalls.empty()) {
        return false;
    }
    // The nearest magic wall within 250 that's on our way (toward the goal, or right by us).
    const float toGoal = std::max(1.0f, distance(view.x, view.y, gx, gy));
    const float tx = (gx - view.x) / toGoal;
    const float ty = (gy - view.y) / toGoal;
    std::optional<std::pair<float, float>> wall;
    float wallD = 0;
    for (const auto& [cx, cy] : view.magicWalls) {
        // The cell's centre in world units.
        const float wx = static_cast<float>(cx * client::NavGrid::kCellSize + client::NavGrid::kCellSize / 2);
        const float wy = static_cast<float>(cy * client::NavGrid::kCellSize + client::NavGrid::kCellSize / 2);
        const float d = distance(view.x, view.y, wx, wy);
        // Cosine between the way to the wall and the way to the goal: ahead of us is near 1.
        const float along = d > 0 ? ((wx - view.x) * tx + (wy - view.y) * ty) / d : 1.0f;
        if (d <= 250 && (along > 0.3f || d <= 40) && (!wall || d < wallD)) {
            wall = std::pair{wx, wy};
            wallD = d;
        }
    }
    if (!wall) {
        return false;
    }
    const auto [wx, wy] = *wall;
    wallBreakAt_ = view.frame;
    // From where we break it: at hand for a blow, from off for a blast (wallStandOff), with a
    // clear line to its face (its own cell aside).
    const float standOff = wallStandOff(view);
    const float face = std::max(1.0f, wallD);
    const bool inLine = clearBetween(view, wx + (view.x - wx) / face * 16, wy + (view.y - wy) / face * 16, true);
    if (standOff > kWallReach && wallD <= standOff - 25) {
        // Too near for a blast: back off a little first.
        if (const auto away = safeWay(view, directionTo(wx, wy, view.x, view.y), 0)) {
            body.setAction(false);
            body.setMove(away);
            runCursor(view, body, *away);
            return true;
        }
    }
    if (wallD > standOff || !inLine) {
        // Up to it (its cell's blocked: the path ends beside it), else straight at it.
        auto direction = nav_.steer(view, wx, wy, standOff > kWallReach ? standOff - 10 : 30);
        if (!direction) {
            direction = directionTo(view.x, view.y, wx, wy);
        }
        body.setAction(false);
        body.setMove(direction);
        runCursor(view, body, *direction);
        return true;
    }
    body.setMove(std::nullopt);
    body.setCursor(static_cast<int>(wx), static_cast<int>(wy));
    hitWall(view, body, wx, wy);
    return true;
}

bool PlayerBrain::recharge(const BotView& view, BotBody& body, const BotView::ManaSource& s, float d)
{
    if (!s.refills || d > kObeliskReach - 8 || !clearBetween(view, s.x, s.y, true)) {
        return false;
    }
    body.setMove(std::nullopt); // (ObeliskUpdate gives to whoever's near: standing is enough)
    body.setAction(false);
    return true;
}

float PlayerBrain::gearWorth(const BotView& view, const SeenItem& item) const
{
    // Only gear our class may use (the server refuses the rest: ArmorEquipClassFail), that isn't
    // one we have (pickup.c:MaxSameItem) and beats what we have of its kind (on its piece).
    if (!item.usable || std::ranges::count(view.inventory, item.typeId, &OwnItem::typeId) > 0) {
        return 0;
    }
    float have = -1;
    for (const auto& o : view.inventory) {
        if (o.kind == item.kind && (item.kind != ItemKind::Armor || o.slot == item.slot)) {
            have = std::max(have, o.value);
        }
    }
    return item.value > have ? std::max(item.value - have, 1.0f) : 0.0f;
}

void PlayerBrain::tendGear(const BotView& view, BotBody& body)
{
    // The best weapon we carry if it beats the one in hand, else the armour piece that beats
    // most what's worn there. Each tried once: the server says no to what doesn't fit.
    float wielded = -1;
    for (const auto& i : view.inventory) {
        if (i.kind == ItemKind::Weapon && i.equipped) {
            wielded = std::max(wielded, i.value);
        }
    }
    if (const OwnItem* gear = weaponUpgrade(view, wielded, false)) {
        equipOnce(view, body, *gear);
        return;
    }
    tendArmor(view, body);
}

const OwnItem* PlayerBrain::weaponUpgrade(const BotView& view, float wielded, bool twoHandedOnly) const
{
    const OwnItem* gear = nullptr;
    float gain = 0;
    for (const auto& i : view.inventory) {
        if (i.kind == ItemKind::Weapon && !i.equipped && i.usable && !triedEquip_.contains(i.netCode) &&
            i.value > wielded && (!twoHandedOnly || i.twoHanded) && (!gear || i.value - wielded > gain)) {
            gear = &i;
            gain = i.value - wielded;
        }
    }
    return gear;
}

const OwnItem* PlayerBrain::armorUpgrade(const BotView& view, bool noShields) const
{
    const OwnItem* gear = nullptr;
    float gain = 0;
    for (const auto& i : view.inventory) {
        if (i.kind != ItemKind::Armor || i.equipped || !i.usable || triedEquip_.contains(i.netCode) ||
            (noShields && i.blocks)) {
            continue;
        }
        float worn = -1; // nothing on that piece
        for (const auto& o : view.inventory) {
            if (o.kind == ItemKind::Armor && o.equipped && o.slot == i.slot) {
                worn = std::max(worn, o.value);
            }
        }
        if (i.value > worn && (!gear || i.value - worn > gain)) {
            gear = &i;
            gain = i.value - worn;
        }
    }
    return gear;
}

void PlayerBrain::equipOnce(const BotView& view, BotBody& body, const OwnItem& item)
{
    body.equip(item.netCode);
    triedEquip_.insert(item.netCode);
    nextItemAction_ = view.frame + 20;
}

bool PlayerBrain::tendArmor(const BotView& view, BotBody& body)
{
    const OwnItem* gear = armorUpgrade(view, false);
    if (!gear) {
        // A bow or crossbow in hand: a quiver on with it (it shoots from the equipped one).
        const bool shooting = std::ranges::any_of(view.inventory, [](const OwnItem& i) { return i.ranged && i.equipped; });
        const bool quivered = std::ranges::any_of(view.inventory, [](const OwnItem& i) {
            return i.kind == ItemKind::Quiver && i.equipped;
        });
        if (shooting && !quivered) {
            for (const auto& i : view.inventory) {
                if (i.kind == ItemKind::Quiver && i.usable && !triedEquip_.contains(i.netCode)) {
                    gear = &i;
                    break;
                }
            }
        }
    }
    if (!gear) {
        return false;
    }
    equipOnce(view, body, *gear);
    return true;
}

// ------------------------------------------------------------------ the frame

bool PlayerBrain::unstall(const BotView& view, BotBody& body, const SeenPlayer& enemy)
{
    const float d = distance(view.x, view.y, enemy.x, enemy.y);
    const auto goAtThem = [&] {
        const std::uint8_t toward = directionTo(view.x, view.y, enemy.x, enemy.y);
        body.setMove(d > 24 ? std::optional{toward} : std::nullopt);
        body.setFacing(toward);
        runCursor(view, body, toward);
        body.setAction(d <= 60 && (thinks_ % 2) == 0); // (blows, not a held Action: each press is one)
    };
    if (view.frame < unstickUntil_ && clearBetween(view, enemy.x, enemy.y)) {
        goAtThem();
        return true;
    }
    if (fightAnchorAt_ == 0 || distance(view.x, view.y, fightAnchor_.first, fightAnchor_.second) > 12) {
        fightAnchor_ = {view.x, view.y};
        fightAnchorAt_ = view.frame;
    }
    // A target near, and for 3 s neither a step nor a blow, cast or shot: whatever the fight
    // waits on isn't coming. At them, swinging, for a moment, if there's a clear way; with a wall
    // between, someone else for a while (charging the wall gets nowhere).
    if (d <= 250 && view.frame - fightAnchorAt_ > kFightStall && view.frame - attackedAt_ > kFightStall) {
        if (!clearBetween(view, enemy.x, enemy.y)) {
            ignored_ = enemy.netCode;
            ignoredUntil_ = view.frame + 150;
            target_ = 0;
            fightAnchorAt_ = view.frame;
            nav_.reset();
            return false;
        }
        ++stallsBroken_;
        unstickUntil_ = view.frame + 20;
        fightAnchorAt_ = view.frame;
        nav_.reset();
        goAtThem();
        return true;
    }
    return false;
}

namespace {

/// The body as it is, noting whether its held movement is on (`moving`).
class NotedBody final : public BotBody {
public:
    NotedBody(BotBody& inner, bool& moving)
        : inner_(inner)
        , moving_(moving)
    {
    }
    void setMove(std::optional<std::uint8_t> direction) override
    {
        inner_.setMove(direction);
        moving_ = direction.has_value();
    }
    void setFacing(std::uint8_t direction) override { inner_.setFacing(direction); }
    void setCursor(int x, int y) override { inner_.setCursor(x, y); }
    void setAction(bool held) override { inner_.setAction(held); }
    void pressAction() override { inner_.pressAction(); }
    void jump() override { inner_.jump(); }
    void useAbility(std::uint8_t ability) override { inner_.useAbility(ability); }
    void castSpells(std::span<const std::uint32_t> spells, bool onSelf) override { inner_.castSpells(spells, onSelf); }
    void pickup(std::uint16_t netCode) override { inner_.pickup(netCode); }
    void useItem(std::uint16_t netCode) override { inner_.useItem(netCode); }
    void equip(std::uint16_t netCode) override { inner_.equip(netCode); }
    void say(std::string_view text, bool team) override { inner_.say(text, team); }
    void taunt() override { inner_.taunt(); }
    void joinTeam(std::uint32_t team) override { inner_.joinTeam(team); }
    void observe() override { inner_.observe(); }

private:
    BotBody& inner_;
    bool& moving_;
};

/// Player animations of a spell under way (CAST_SPELL, CONCENTRATE, STAFF_SPELL_BLAST).
bool casting(std::uint8_t anim)
{
    return anim == 21 || anim == 22 || anim == 31;
}

} // namespace

// The frame: play() decides, then in play a teammate's buff is let in past our shield.
void PlayerBrain::think(const BotView& view, BotBody& body)
{
    NotedBody noted(body, moving_);
    blocked_ = false;
    play(view, noted);
    if (view.inGame && view.placed && !view.dead && !view.observer && !view.standDown) {
        letBuffsIn(view, noted);
    }
}

void PlayerBrain::letBuffsIn(const BotView& view, BotBody& body)
{
    // Standing still with a shield, it comes up within a quarter second and stays up until we
    // move (Plr_UpdateState, states 0x0D-0x10), and a raised shield turns whatever
    // comes at it, a teammate's buff too. So: standing, not blocking a missile, out of a fight
    // and not waiting on a ride, with a teammate near casting, a slow walk to them keeps it down.
    const bool shield = std::ranges::any_of(view.inventory, [](const OwnItem& i) {
        return i.equipped && i.blocks && i.kind == ItemKind::Armor;
    });
    if (!shield || moving_ || blocked_ || target_ != 0 || nav_.waiting() || view.team == 0) {
        return;
    }
    // The nearest teammate casting within kBuffReach, `d` away.
    const SeenPlayer* caster = nullptr;
    float d = 0;
    for (const auto& p : view.players) {
        if (p.team != view.team || !p.placed || p.dead || !casting(p.anim)) {
            continue;
        }
        const float pd = distance(view.x, view.y, p.x, p.y);
        if (pd <= kBuffReach && (!caster || pd < d)) {
            caster = &p;
            d = pd;
        }
    }
    if (!caster) {
        return;
    }
    const std::uint8_t toward = directionTo(view.x, view.y, caster->x, caster->y);
    const auto way = d > 50 ? std::optional{toward} : safeWay(view, toward, 64); // (beside them: round them)
    if (!way) {
        return;
    }
    body.setMove(way);
    const auto [ux, uy] = unitOf(*way);
    body.setCursor(static_cast<int>(view.x + 50 * ux), static_cast<int>(view.y + 50 * uy)); // (a walk, not a run)
    doing_ = "let buff in";
}

void PlayerBrain::outOfPlay(BotBody& body)
{
    body.setMove(std::nullopt);
    body.setAction(false);
    target_ = 0;
    fetching_ = 0;
    roam_.reset();
    nav_.reset();
}

void PlayerBrain::lounge(const BotView& view, BotBody& body)
{
    doing_ = "waiting for the host";
    body.setAction(false);
    target_ = 0;
    fetching_ = 0;
    roam_.reset();
    if (!loungeHome_ || distance(view.x, view.y, loungeHome_->first, loungeHome_->second) > 2 * kLoungeRadius) {
        // (Here first, or moved: a respawn, a teleporter.)
        loungeHome_ = std::pair{view.x, view.y};
        loungeSpot_.reset();
        loungeUntil_ = view.frame + nox::util::uniform_int_distribution<std::uint32_t>(30, 240)(rng_);
        nav_.reset();
    }
    if (!loungeSpot_) {
        if (view.frame < loungeUntil_) {
            body.setMove(std::nullopt); // standing about
            return;
        }
        // A few steps somewhere near where we came in.
        nox::util::uniform_real_distribution<float> turn(0, 2 * std::numbers::pi_v<float>);
        nox::util::uniform_real_distribution<float> out(30, kLoungeRadius);
        for (int tries = 0; tries < 20 && !loungeSpot_; ++tries) {
            const float a = turn(rng_);
            const float r = out(rng_);
            const float x = loungeHome_->first + r * std::cos(a);
            const float y = loungeHome_->second + r * std::sin(a);
            if (!blockedAt(view, x, y) && !hazardAt(view, x, y) && clearBetween(view, x, y, false, true)) {
                loungeSpot_ = std::pair{x, y};
            }
        }
        if (!loungeSpot_) {
            loungeUntil_ = view.frame + 90;
            body.setMove(std::nullopt);
            return;
        }
        nav_.reset();
    }
    if (distance(view.x, view.y, loungeSpot_->first, loungeSpot_->second) <= 12 || stuckFor(view)) {
        loungeSpot_.reset();
        loungeUntil_ = view.frame + nox::util::uniform_int_distribution<std::uint32_t>(90, 300)(rng_); // (3-10 s)
        body.setMove(std::nullopt);
        return;
    }
    moveTo(view, body, loungeSpot_->first, loungeSpot_->second, 12);
    // A walk, not a run: the cursor near (past 100 the server runs us).
    const auto [ux, uy] = unitOf(directionTo(view.x, view.y, loungeSpot_->first, loungeSpot_->second));
    body.setCursor(static_cast<int>(view.x + 50 * ux), static_cast<int>(view.y + 50 * uy));
}

void PlayerBrain::play(const BotView& view, BotBody& body)
{
    ++thinks_;
    frame_ = view.frame;
    noteBodies(view);
    // Everyone's and every missile's movement, for leading shots and dodging.
    for (const auto* seen : {&view.players, &view.creatures}) {
        for (const auto& p : *seen) {
            if (p.placed) {
                follow(players_, p.netCode, p.x, p.y, view.frame);
            }
        }
    }
    for (const auto& m : view.missiles) {
        follow(missiles_, m.netCode, m.x, m.y, view.frame);
    }
    // Every 2 s, tracks not seen for 2 s are dropped.
    if (view.frame % 60 == 0) {
        std::erase_if(players_, [&](const auto& t) { return view.frame - t.second.frame > 60; });
        std::erase_if(missiles_, [&](const auto& t) { return view.frame - t.second.frame > 60; });
    }
    // Not in a game, or our position not known yet: nothing to do.
    if (!view.inGame || !view.placed) {
        doing_ = "";
        body.setMove(std::nullopt);
        body.setAction(false);
        return;
    }
    if (view.standDown) {
        doing_ = "standing down";
        // Out of play: watch until wanted (asked again now and then until the server has us
        // observing), then the respawn below brings us back.
        outOfPlay(body);
        respawnAt_ = 0;
        if (!view.observer && view.frame - observeAskedAt_ >= 90) {
            body.observe();
            observeAskedAt_ = view.frame;
        }
        return;
    }
    if (view.dead || view.observer) {
        doing_ = view.dead ? "dead" : "observing";
        // Back in after a moment, as a person would (Action respawns: Plr_UpdateObserver).
        outOfPlay(body);
        noticed_ = 0;
        if (respawnAt_ == 0) {
            respawnAt_ = view.frame + static_cast<std::uint32_t>(skill_.reactionFrames) +
                         nox::util::uniform_int_distribution<std::uint32_t>(15, 75)(rng_);
        } else if (view.frame >= respawnAt_) {
            body.pressAction();
            respawnAt_ = view.frame + 30; // again if it didn't take
        }
        return;
    }
    respawnAt_ = 0;
    if (view.gameFlags & kChatMode) {
        lounge(view, body); // (no game yet: waiting for the host)
        return;
    }
    loungeHome_.reset();
    // In play: the class's reach, potions and gear, a block, hazards, then the target.
    watchForLoops(view);
    prepare(view);
    tend(view, body);
    if (react(view, body)) {
        doing_ = "block";
        blocked_ = true;
        return;
    }
    if (escapeHazard(view, body)) {
        doing_ = "out of a hazard";
        return;
    }
    const SeenPlayer* enemy = pickTarget(view);
    const bool underAttack = hurtAt_ != 0 && view.frame - hurtAt_ < kHurtMemory;
    if (!enemy && underAttack && !view.carryingFlag) {
        // Hurt by someone we can't see (off our screen, shooting): the nearest enemy about, as
        // a player turns to where the hits come from.
        float best = kHurtSearch;
        for (const auto* list : {&view.players, &view.creatures}) {
            for (const auto& p : *list) {
                if (!isEnemy(view, p)) {
                    continue;
                }
                if (const float d = distance(view.x, view.y, p.x, p.y); d < best) {
                    enemy = &p;
                    best = d;
                }
            }
        }
        if (enemy && target_ != enemy->netCode) {
            target_ = enemy->netCode;
            reachedAt_ = view.frame;
            nav_.reset();
        }
    }
    if (enemy && view.role == BotView::Role::Guard && !enemy->hasFlag && !underAttack &&
        distance(view.x, view.y, enemy->x, enemy->y) > kGuardReach) {
        // A guard doesn't go after them far from the flag: back to it instead (whoever has our
        // flag excepted, anyone near it or near the guard, and whoever's hurting it: those are
        // fought wherever the guard is, not let pick it off from range).
        if (const auto base = flagBase(view, true); base && distance(enemy->x, enemy->y, base->first, base->second) > kGuardBase) {
            enemy = nullptr;
            target_ = 0;
        }
    }
    if (const SeenPlayer* carrier = enemy && !enemy->hasFlag ? escorted(view) : nullptr;
        carrier && !underAttack && distance(enemy->x, enemy->y, carrier->x, carrier->y) > kEscortGuard &&
        distance(view.x, view.y, enemy->x, enemy->y) > reach_ + 40) {
        // With our flag carrier: only who comes at them (or at us), not off after anyone else.
        enemy = nullptr;
        target_ = 0;
    }
    if (enemy && view.carryingFlag && !enemy->hasFlag &&
        distance(view.x, view.y, enemy->x, enemy->y) > (underAttack ? kFlagDefendReach : kFlagFightReach)) {
        // Carrying their flag: home with it, fighting only who's in the way, whoever is hurting
        // us (running from them only gets the flag dropped), and whoever has ours (it must be
        // home for a capture).
        enemy = nullptr;
        target_ = 0;
    }
    // A gentle (easy) bot goes easy on someone who's having a hard time against the bots.
    easing_ = baseSkill_.gentle && enemy && !enemy->creature &&
              std::ranges::find(view.struggling, enemy->netCode) != view.struggling.end();
    skill_ = easing_ ? baseSkill_.gently() : baseSkill_;
    if (flagDash(view, body, enemy)) {
        return;
    }
    if (enemy) {
        // Poorly armed: a real upgrade in sight comes first, unless they're on top of us (or
        // we have the flag, or they have ours).
        if (underGeared(view) && !view.carryingFlag && !enemy->hasFlag &&
            distance(view.x, view.y, enemy->x, enemy->y) > 160) {
            if (const SeenItem* upgrade = chooseItem(view, 25)) {
                doing_ = "gear";
                fetch(view, body, *upgrade);
                return;
            }
            if (gearRun(view, body, 25)) {
                doing_ = "gear run";
                return; // to where the map keeps one
            }
        }
        fetching_ = 0;
        roam_.reset();
        doing_ = enemy->creature ? "fight creature" : easing_ ? "fight gently" : "fight";
        if (unstall(view, body, *enemy)) {
            return;
        }
        fight(view, body, *enemy);
        return;
    }
    fightAnchorAt_ = 0;
    body.setAction(false);
    if (idle(view, body)) {
        doing_ = "buff";
        return;
    }
    look(view, body);
}

bool PlayerBrain::flagDash(const BotView& view, BotBody& body, const SeenPlayer* enemy)
{
    if (view.team == 0 || view.carryingFlag || view.frame < flagSkipUntil_) {
        return false; // (carrying theirs: home with it, look's)
    }
    // A dropped flag close by, ours (a touch sends it home) before theirs (a touch takes it):
    // to it before a fight, as a player would, unless whoever we're fighting is nearer than it
    // and at hand (or has our flag).
    const BotView::FlagBase* flag = nullptr;
    for (const bool own : {true, false}) {
        if (const auto* f = looseFlag(view, own); f && !flag && distance(view.x, view.y, f->x, f->y) <= kFlagDash) {
            flag = f;
        }
    }
    if (!flag) {
        return false;
    }
    const float d = distance(view.x, view.y, flag->x, flag->y);
    if (enemy) {
        const float e = distance(view.x, view.y, enemy->x, enemy->y);
        if (e < d && (e <= reach_ + 40 || enemy->hasFlag)) {
            return false;
        }
    }
    return toFlag(view, body, *flag);
}

bool PlayerBrain::toFlag(const BotView& view, BotBody& body, const BotView::FlagBase& flag)
{
    if (noWay(view) || stuckFor(view)) {
        // No way onto it (dropped where feet can't go): left for a while.
        flagSkipUntil_ = view.frame + kFlagSkip;
        nav_.reset();
        return false;
    }
    doing_ = flag.team == view.team ? "return flag" : "grab flag";
    fetching_ = 0;
    goalSpot_.reset();
    body.setAction(false);
    moveTo(view, body, flag.x, flag.y, 4);
    return true;
}

void PlayerBrain::tend(const BotView& view, BotBody& body)
{
    if (view.frame < nextItemAction_) {
        return;
    }
    // Drinks the first of `kind` we carry; false if we have none.
    const auto drink = [&](ItemKind kind) {
        for (const auto& i : view.inventory) {
            if (i.kind == kind) {
                body.useItem(i.netCode);
                nextItemAction_ = view.frame + 30;
                return true;
            }
        }
        return false;
    };
    // (Carrying their flag, sooner: a carrier who falls drops it.)
    const float drinkBelow = view.carryingFlag ? std::max(skill_.retreatHealth, kCarrierDrinks) : skill_.retreatHealth;
    if (view.maxHealth > 0 && view.health < drinkBelow * view.maxHealth && drink(ItemKind::HealthPotion)) {
        return;
    }
    if (view.poisoned && drink(ItemKind::CurePotion)) {
        return;
    }
    if (usesMana() && view.maxMana > 0 && view.mana < 0.3f * view.maxMana && drink(ItemKind::ManaPotion)) {
        return;
    }
    tendGear(view, body);
}

// ------------------------------------------------------------------ moving

bool PlayerBrain::hop(const BotView& view, BotBody& body, std::uint8_t direction)
{
    if (hazardAt(view, view.x, view.y)) {
        return false; // in it already: escapeHazard walks us out
    }
    const auto [ux, uy] = unitOf(direction);
    bool ahead = false;
    // A hazard within 36 units ahead of us?
    for (float d = 12; d <= 36 && !ahead; d += 12) {
        ahead = hazardAt(view, view.x + ux * d, view.y + uy * d);
    }
    if (!ahead) {
        return false;
    }
    // Over it if there's somewhere safe to land within a jump (the path only crosses a hazard
    // there's no way round; a wide one it walks, and takes the burns).
    for (float d = 40; d <= kJumpReach; d += 8) {
        const float lx = view.x + ux * d;
        const float ly = view.y + uy * d;
        if (blockedAt(view, lx, ly)) {
            return false;
        }
        if (hazardAt(view, lx, ly)) {
            continue;
        }
        if (view.frame >= nextJump_ && view.stamina >= kJumpStamina) {
            body.jump(); // (it goes the way we face: the way we're running)
            nextJump_ = view.frame + kJumpEvery;
            return false;
        }
        if (view.frame + kJumpEvery - nextJump_ < 10) {
            return false; // just jumped (in the air, or the input's on its way): on with it
        }
        // Out of breath (a jump takes 90, back at 3 a frame), or the last one didn't take: at the
        // edge, facing over, until we can, not walking in.
        body.setMove(std::nullopt);
        body.setFacing(direction);
        return true;
    }
    return false;
}

bool PlayerBrain::escapeHazard(const BotView& view, BotBody& body)
{
    if (!hazardAt(view, view.x, view.y)) {
        return false;
    }
    // The nearest safe ground in one of eight directions, a few steps away.
    for (float d = 15; d <= 75; d += 15) {
        for (int k = 0; k < 8; ++k) {
            const auto direction = static_cast<std::uint8_t>(k * 32);
            const auto [ux, uy] = unitOf(direction);
            const float x = view.x + ux * d;
            const float y = view.y + uy * d;
            if (!hazardAt(view, x, y) && !blockedAt(view, x, y) &&
                view.nav->clearLine({static_cast<int>(view.x), static_cast<int>(view.y)}, {static_cast<int>(x), static_cast<int>(y)})) {
                body.setAction(false);
                body.setMove(direction);
                runCursor(view, body, direction);
                return true;
            }
        }
    }
    return false; // nowhere near: carry on, the path will take us out
}

std::optional<std::pair<float, float>> PlayerBrain::pickupSpot(const BotView& view, const SeenItem& item) const
{
    if (!hazardAt(view, item.x, item.y)) {
        return std::pair{item.x, item.y};
    }
    // In the lava (or the flames): from safe ground within reach, if there's any, as far out as
    // reach allows (the server's steps overshoot); of those, the nearest to us.
    for (float d = kPickupReach - 6; d >= 12; d -= 8) {
        if (const auto best = openOnRing(view, item.x, item.y, d)) {
            return best;
        }
    }
    return std::nullopt;
}

std::optional<std::pair<float, float>> PlayerBrain::openOnRing(const BotView& view, float cx, float cy, float r)
{
    std::optional<std::pair<float, float>> best;
    float bestD = 0;
    for (int k = 0; k < 16; ++k) {
        const auto [ux, uy] = unitOf(static_cast<std::uint8_t>(k * 16));
        const float x = cx + ux * r;
        const float y = cy + uy * r;
        const float fromUs = distance(view.x, view.y, x, y);
        if (!hazardAt(view, x, y) && !blockedAt(view, x, y) && (!best || fromUs < bestD)) {
            best = std::pair{x, y};
            bestD = fromUs;
        }
    }
    return best;
}

void PlayerBrain::moveTo(const BotView& view, BotBody& body, float x, float y, float arrive)
{
    // At a Wall spell's wall in the way: on at it (not planning the way on each frame meanwhile).
    if (wallBreakAt_ != 0 && view.frame - wallBreakAt_ <= 30 && breakMagicWall(view, body, x, y)) {
        return;
    }
    const auto direction = nav_.steer(view, x, y, arrive);
    if (distance(view.x, view.y, x, y) > arrive && shortcut(view, body, x, y)) {
        return; // (a teleport there, or over what's in the way)
    }
    // No way there (walking as near as we can get), or stuck: a magic wall that way, near, is
    // broken.
    if ((nav_.unreachable() || nav_.stuck()) && breakMagicWall(view, body, x, y)) {
        return; // (a Wall spell's wall closes the way: through it)
    }
    body.setMove(direction);
    if (direction) {
        runCursor(view, body, *direction);
        hop(view, body, *direction);
    }
}

void PlayerBrain::approach(const BotView& view, BotBody& body, const SeenPlayer& enemy, float arrive)
{
    const float d = distance(view.x, view.y, enemy.x, enemy.y);
    auto direction = nav_.steer(view, enemy.x, enemy.y, arrive);
    const bool inSight = clearBetween(view, enemy.x, enemy.y, true);
    if (!direction && nav_.unreachable() && d <= 140 && inSight && clearBetween(view, enemy.x, enemy.y, true, true)) {
        // Close and in sight, but the grid has no way (they're in among obelisks or pillars, or
        // against a wall, whose cells it counts whole): straight at them, sidestepping if it
        // sticks. (Never through a wall: right behind one, they're out of reach for now.)
        direction = directionTo(view.x, view.y, enemy.x, enemy.y);
    }
    body.setMove(direction);
    if (direction) {
        hop(view, body, *direction); // over flames or lava in the way (the cursor stays on them)
        return;
    }
    body.setFacing(aimAt(view, enemy.x, enemy.y));
    if (nav_.unreachable() && breakMagicWall(view, body, enemy.x, enemy.y)) {
        return; // (behind a Wall spell's wall: through it to them)
    }
    if (nav_.unreachable() && (d > 100 || !inSight)) {
        // Seen but no way to them (across a gap, behind bars): someone else for a while (never
        // one right beside us).
        ignored_ = enemy.netCode;
        ignoredUntil_ = view.frame + 150;
        target_ = 0;
        body.setAction(false);
    }
}

// ------------------------------------------------------------------ targets

// Someone to fight: in sight, alive, playing, visible, not a body, and not a teammate (team 0 is no team).
bool PlayerBrain::isEnemy(const BotView& view, const SeenPlayer& p) const
{
    return p.netCode != view.netCode && p.placed && !p.dead && !p.observer && !p.invisible &&
           !bodies_.contains(p.netCode) && !(view.team != 0 && p.team == view.team);
}

void PlayerBrain::noteBodies(const BotView& view)
{
    // Seen fall (DIE / DEAD): a body until they're elsewhere (respawned) or seen moving. The
    // animation can come only once, the corpse's drawable going back to an ordinary state, and
    // casters at range kept casting at it.
    const auto seen = {&view.players, &view.creatures};
    for (const auto* list : seen) {
        for (const auto& p : *list) {
            if (!p.placed) {
                continue;
            }
            if (p.dead) {
                bodies_[p.netCode] = {p.x, p.y};
            } else if (const auto it = bodies_.find(p.netCode);
                       it != bodies_.end() && distance(p.x, p.y, it->second.first, it->second.second) > 12) {
                bodies_.erase(it); // up and about again
            }
        }
    }
    std::erase_if(bodies_, [&](const auto& b) {
        return std::ranges::none_of(seen, [&](const auto* list) {
            return std::ranges::any_of(*list, [&](const SeenPlayer& p) { return p.netCode == b.first; });
        });
    });
}

const SeenPlayer* PlayerBrain::pickTarget(const BotView& view)
{
    // Whoever has our flag, in sight, comes before anyone: the nearest such, at once, chased
    // however long it takes.
    const SeenPlayer* carrier = nullptr;
    float carrierD = 0;
    for (const auto& p : view.players) {
        if (!p.hasFlag || !isEnemy(view, p) || !sees(view, p.x, p.y)) {
            continue;
        }
        if (const float d = distance(view.x, view.y, p.x, p.y); !carrier || d < carrierD) {
            carrier = &p;
            carrierD = d;
        }
    }
    if (carrier) {
        if (target_ != carrier->netCode) {
            target_ = carrier->netCode;
            nav_.reset();
        }
        reachedAt_ = view.frame;
        lastSeen_ = *carrier;
        lastSeenAt_ = view.frame;
        lastHealth_ = view.health;
        if (ignored_ == carrier->netCode) {
            ignored_ = 0;
        }
        return carrier;
    }
    // Players and creatures alike, a creature ranked as if kCreatureHandicap further off: a
    // player about as near comes first, a creature at hand before a player across the room.
    const auto rankOf = [](const SeenPlayer& p, float d) { return p.creature ? d + kCreatureHandicap : d; };
    const SeenPlayer* current = nullptr;
    float currentDistance = 0;
    const SeenPlayer* best = nullptr;
    float bestDistance = 0;
    for (const auto* list : {&view.players, &view.creatures}) {
        for (const auto& p : *list) {
            if (!isEnemy(view, p) || (p.netCode == ignored_ && view.frame < ignoredUntil_)) {
                continue;
            }
            const float d = distance(view.x, view.y, p.x, p.y);
            const bool seen = sees(view, p.x, p.y);
            if (p.netCode == target_) {
                if (seen) {
                    current = &p;
                    currentDistance = d;
                    lastSeen_ = p;
                    lastSeenAt_ = view.frame;
                } else if (lastSeen_ && lastSeen_->netCode == p.netCode && view.frame - lastSeenAt_ <= kMemoryFrames &&
                           distance(view.x, view.y, lastSeen_->x, lastSeen_->y) > 30) {
                    // Out of sight: after them to where they were last seen, as a player would
                    // (the server still says where they are; a player doesn't see it), not round
                    // the map.
                    current = &*lastSeen_;
                    currentDistance = distance(view.x, view.y, lastSeen_->x, lastSeen_->y);
                }
            }
            if (seen && (!best || rankOf(p, d) < rankOf(*best, bestDistance))) {
                best = &p;
                bestDistance = d;
            }
        }
    }
    // Hurt since the last frame: noted, so whoever's doing it can be looked for (kHurtMemory).
    const bool hurt = lastHealth_ != 0 && view.health < lastHealth_;
    lastHealth_ = view.health;
    if (hurt) {
        hurtAt_ = view.frame;
    }
    if (current) {
        if (currentDistance <= reach_ + 10) {
            reachedAt_ = view.frame;
        }
        // Chased for long without getting to them (they run, or the way round is long): leave
        // them be for a while.
        if (view.frame - reachedAt_ > 240) {  // 8 s
            ignored_ = target_;
            ignoredUntil_ = view.frame + 150;
            current = nullptr;
            if (best && best->netCode == ignored_) {
                best = nullptr;
            }
        }
    }
    if (current) {
        // Someone else much nearer, or beside us while we're being hit and the target isn't:
        // them instead, at once (they're right there).
        const bool other = best && best != current;
        const bool nearer = other && (rankOf(*best, bestDistance) < rankOf(*current, currentDistance) * 0.6f ||
                                      (bestDistance <= reach_ + 20 && currentDistance > reach_ + 40));
        // (Held up by a ride, whoever we can see hurting us is fought, at any range: an elevator
        // or teleporter isn't worth standing still to be shot for.)
        const bool attacked = other && hurt && (bestDistance <= reach_ + 30 || nav_.waiting()) &&
                              currentDistance > reach_ + 10;
        if (!nearer && !attacked) {
            return current;
        }
        target_ = best->netCode;
        reachedAt_ = view.frame;
        nav_.reset();
        return best;
    }
    target_ = 0;
    if (!best) {
        noticed_ = 0;
        return nullptr;
    }
    if (noticed_ != best->netCode) {
        noticed_ = best->netCode;
        noticedAt_ = view.frame;
    }
    if (view.frame - noticedAt_ < static_cast<std::uint32_t>(skill_.reactionFrames)) {
        return nullptr; // not reacted yet
    }
    target_ = best->netCode;
    reachedAt_ = view.frame; // (the chase clock starts now)
    nav_.reset();
    return best;
}

// ------------------------------------------------------------------ items and goals

float PlayerBrain::worth(const BotView& view, const SeenItem& item) const
{
    if (!pickupSpot(view, item)) {
        return 0; // out in the lava: not worth the burns
    }
    const auto same = std::ranges::count(view.inventory, item.typeId, &OwnItem::typeId);
    // Obj_PickupDefault refuses a 4th of one food or potion (a 10th in quest and coop
    // games: flags 0x1800).
    const std::ptrdiff_t potionCap = (view.gameFlags & 0x1800) ? 9 : 3;
    switch (item.kind) {
    case ItemKind::HealthPotion:
        return same < potionCap ? 30.0f : 0.0f;
    case ItemKind::ManaPotion:
        return usesMana() && same < potionCap ? 25.0f : 0.0f;
    case ItemKind::CurePotion:
        return carried(view, item.kind) < 1 ? 15.0f : 0.0f;
    case ItemKind::Weapon:
    case ItemKind::Armor:
    case ItemKind::Staff:
        return gearWorth(view, item);
    case ItemKind::Quiver:
        // Ammunition for a bow or crossbow we carry, when we have none.
        return item.usable && carried(view, ItemKind::Quiver) == 0 &&
                       std::ranges::any_of(view.inventory, [](const OwnItem& i) { return i.ranged && i.usable; })
                   ? 30.0f
                   : 0.0f;
    default:
        return 0;
    }
}

const SeenItem* PlayerBrain::chooseItem(const BotView& view, float atLeast, float maxDistance)
{
    // Items given up on are considered again once their time is up.
    std::erase_if(skipItems_, [&](const auto& s) { return view.frame >= s.second; });
    if (fetching_ != 0) {
        const auto it = std::ranges::find(view.items, fetching_, &SeenItem::netCode);
        if (it == view.items.end()) {
            fetching_ = 0; // taken
        } else if (distance(view.x, view.y, it->x, it->y) > maxDistance + 50) {
            fetching_ = 0; // further than we'd go for one now
        } else if (view.frame - fetchSince_ > 300 || stuckFor(view) || noWay(view) || pickupTries_ >= 3) {
            // Out of reach (on a table, behind a wall cell): leave it be for a while.
            skipItems_[fetching_] = view.frame + 600;
            fetching_ = 0;
            nav_.reset();
        } else {
            return worth(view, *it) >= atLeast ? &*it : nullptr;
        }
    }
    // Nearest first, but a big upgrade is worth the longer walk.
    const SeenItem* item = nullptr;
    float best = 0;
    for (const auto& i : view.items) {
        const float w = worth(view, i);
        const float d = distance(view.x, view.y, i.x, i.y);
        // A score: the distance, less 3 units for each point the item is worth (lowest wins).
        const float score = d - 3 * w;
        if (w > 0 && w >= atLeast && d <= maxDistance && !skipItems_.contains(i.netCode) && (!item || score < best)) {
            item = &i;
            best = score;
        }
    }
    if (item) {
        fetching_ = item->netCode;
        fetchSince_ = view.frame;
        pickupTries_ = 0;
        roam_.reset();
        nav_.reset();
    }
    return item;
}

void PlayerBrain::fetch(const BotView& view, BotBody& body, const SeenItem& item)
{
    body.setAction(false);
    const auto spot = pickupSpot(view, item);
    if (!spot) {
        skipItems_[item.netCode] = view.frame + 600;
        fetching_ = 0;
        body.setMove(std::nullopt);
        return;
    }
    const float d = distance(view.x, view.y, item.x, item.y);
    if (d <= 40 && view.frame % 10 == 0) {
        body.pickup(item.netCode); // in reach: ask (the server says whether)
        ++pickupTries_;            // still there after three asks: refused, leave it
    }
    const bool atEdge = spot->first != item.x || spot->second != item.y; // (it's in a hazard)
    if (!atEdge && d <= 24) {
        body.setMove(std::nullopt);
        return;
    }
    moveTo(view, body, spot->first, spot->second, atEdge ? 6.0f : 16.0f);
    if (nav_.hazardAhead(view) > kJumpableHazard) {
        // The only way there is across more lava than a jump clears: not for an item.
        skipItems_[item.netCode] = view.frame + 600;
        fetching_ = 0;
        nav_.reset();
        body.setMove(std::nullopt);
    }
}

bool PlayerBrain::gearRun(const BotView& view, BotBody& body, float atLeast)
{
    if (!view.mapItems || view.mapItems->empty()) {
        return false;
    }
    const auto& spots = *view.mapItems;
    std::erase_if(checkedSpots_, [&](const auto& c) { return view.frame >= c.second; });
    const auto checked = [&](std::size_t i) {
        checkedSpots_[i] = view.frame + kSpotRecheck;
        goalSpot_.reset();
        nav_.reset();
    };
    if (goalSpot_ && *goalSpot_ < spots.size()) {
        const SeenItem& s = spots[*goalSpot_];
        const float d = distance(view.x, view.y, s.x, s.y);
        const bool lying = std::ranges::any_of(view.items, [&](const SeenItem& i) {
            return i.typeId == s.typeId && distance(i.x, i.y, s.x, s.y) <= 40;
        });
        if ((d <= 40 || (d <= 200 && sees(view, s.x, s.y))) && !lying) {
            checked(*goalSpot_); // there (or in sight of it), and it's gone: taken, back later
        } else if (view.frame - goalSince_ > kGoalTimeout || noWay(view) || stuckFor(view) ||
                   worth(view, s) < std::max(atLeast, 1.0f)) {
            checked(*goalSpot_); // no way there, or no longer wanted (we have as good now)
        }
    } else {
        goalSpot_.reset();
    }
    if (!goalSpot_) {
        // The few best by worth for the walk (a big upgrade is worth a long one), one of them
        // at random: bots on a team don't all run for the same spot.
        std::vector<std::pair<float, std::size_t>> best;
        for (std::size_t i = 0; i < spots.size(); ++i) {
            const float w = worth(view, spots[i]);
            if (w <= 0 || w < atLeast || checkedSpots_.contains(i)) {
                continue;
            }
            best.emplace_back(distance(view.x, view.y, spots[i].x, spots[i].y) - 6 * w, i);  // distance less 6 per point of worth
        }
        if (best.empty()) {
            return false;
        }
        std::ranges::sort(best);
        const std::size_t pick = nox::util::uniform_int_distribution<std::size_t>(0, std::min<std::size_t>(best.size(), 3) - 1)(rng_);
        goalSpot_ = best[pick].second;
        goalSince_ = view.frame;
        roam_.reset();
        nav_.reset();
    }
    const SeenItem& s = spots[*goalSpot_];
    moveTo(view, body, s.x, s.y, 20);
    return true;
}

std::optional<std::pair<float, float>> PlayerBrain::flagBase(const BotView& view, bool own) const
{
    if (!view.flagBases || view.team == 0) {
        return std::nullopt;
    }
    for (const auto& f : *view.flagBases) {
        if ((f.team == view.team) == own) {
            return std::pair{f.x, f.y};
        }
    }
    return std::nullopt;
}

const BotView::FlagBase* PlayerBrain::looseFlag(const BotView& view, bool own) const
{
    if (view.team == 0) {
        return nullptr;
    }
    const auto base = flagBase(view, own);
    for (const auto& f : view.flags) {
        if (f.team != 0 && (f.team == view.team) == own && base && distance(f.x, f.y, base->first, base->second) > 40) {
            return &f;
        }
    }
    return nullptr;
}

const SeenPlayer* PlayerBrain::escorted(const BotView& view) const
{
    if (view.role != BotView::Role::Escort) {
        return nullptr;
    }
    const auto it = std::ranges::find(view.players, view.leader, &SeenPlayer::netCode);
    return it != view.players.end() && it->placed && !it->dead && it->hasFlag ? &*it : nullptr;
}

bool PlayerBrain::teamGoal(const BotView& view, BotBody& body)
{
    using Role = BotView::Role;
    switch (view.role) {
    case Role::Guard:
        if (const auto base = flagBase(view, true)) {
            if (distance(view.x, view.y, base->first, base->second) > kPatrolRadius + 40) {
                patrol_.reset();
                moveTo(view, body, base->first, base->second, 60); // back to the flag
                return true;
            }
            // On watch: walking round the flag, from one spot near it to another, a pause at
            // each.
            if (patrol_ && (distance(view.x, view.y, patrol_->first, patrol_->second) <= 20 || nav_.unreachable() || nav_.stuck())) {
                patrol_.reset();
                nav_.reset();
                patrolPauseUntil_ = view.frame + nox::util::uniform_int_distribution<std::uint32_t>(30, 90)(rng_);
            }
            if (!patrol_ && view.frame >= patrolPauseUntil_) {
                nox::util::uniform_real_distribution<float> turn(0, 2 * std::numbers::pi_v<float>);
                nox::util::uniform_real_distribution<float> out(50, kPatrolRadius);
                for (int tries = 0; tries < 20 && !patrol_; ++tries) {
                    const float a = turn(rng_);
                    const float r = out(rng_);
                    const float px = base->first + r * std::cos(a);
                    const float py = base->second + r * std::sin(a);
                    if (!blockedAt(view, px, py) && !hazardAt(view, px, py) && distance(px, py, view.x, view.y) > 40) {
                        patrol_ = std::pair{px, py};
                    }
                }
            }
            if (!patrol_) {
                body.setMove(std::nullopt); // (pausing)
                return true;
            }
            moveTo(view, body, patrol_->first, patrol_->second, 16);
            // A walk, not a run: the cursor near (past 100 the server runs us).
            const auto [ux, uy] = unitOf(directionTo(view.x, view.y, patrol_->first, patrol_->second));
            body.setCursor(static_cast<int>(view.x + 60 * ux), static_cast<int>(view.y + 60 * uy));
            return true;
        }
        return false;
    case Role::Attack:
        if (view.carryingFlag) {
            if (const auto home = flagBase(view, true)) {
                moveTo(view, body, home->first, home->second, 10); // home with it: a capture
                return true;
            }
        }
        if (const auto* dropped = looseFlag(view, false)) {
            moveTo(view, body, dropped->x, dropped->y, 4); // theirs, dropped: a touch takes it there
            return true;
        }
        if (const auto theirs = flagBase(view, false)) {
            moveTo(view, body, theirs->first, theirs->second, 10); // onto their flag: a touch takes it
            return true;
        }
        return false;
    case Role::Hunt: {
        // After whoever has our flag: where they are when the game shows them, else at their
        // base, where they have to bring it. (In sight, pickTarget has them first anyway.)
        const SeenPlayer* carrier = nullptr;
        for (const auto& p : view.players) {
            if (p.hasFlag && p.placed && !p.dead && view.team != 0 && p.team != view.team) {
                carrier = &p;
            }
        }
        if (carrier) {
            moveTo(view, body, carrier->x, carrier->y, 40);
            return true;
        }
        if (const auto theirs = flagBase(view, false)) {
            if (distance(view.x, view.y, theirs->first, theirs->second) > 120) {
                moveTo(view, body, theirs->first, theirs->second, 80);
            } else {
                body.setMove(std::nullopt); // waiting for them there
            }
            return true;
        }
        return false;
    }
    case Role::Escort:
        if (const SeenPlayer* carrier = escorted(view)) {
            // A step ahead of them on their way home (to meet whoever's in the way first), or at
            // their side if home's not known.
            float sx = carrier->x;
            float sy = carrier->y;
            if (const auto home = flagBase(view, true)) {
                const float toHome = std::max(1.0f, distance(carrier->x, carrier->y, home->first, home->second));
                const float ahead = std::min(kEscortLead, toHome);
                sx += (home->first - carrier->x) / toHome * ahead;
                sy += (home->second - carrier->y) / toHome * ahead;
            }
            if (blockedAt(view, sx, sy) || hazardAt(view, sx, sy)) {
                sx = carrier->x;
                sy = carrier->y;
            }
            moveTo(view, body, sx, sy, 24);
            return true;
        }
        return false;
    case Role::Follow:
        for (const auto& p : view.players) {
            if (p.netCode == view.leader && p.placed && !p.dead && !p.observer) {
                if (distance(view.x, view.y, p.x, p.y) > 140) {
                    moveTo(view, body, p.x, p.y, 100); // keep up with the group
                    return true;
                }
                return false; // with them: whatever's to do here
            }
        }
        return false;
    case Role::Free:
        break;
    }
    return false;
}

bool PlayerBrain::seekMana(const BotView& view, BotBody& body)
{
    if (!usesMana() || view.maxMana == 0 || view.manaSources.empty()) {
        return false;
    }
    // Notes when our mana last went up (an obelisk working, or our own regeneration).
    if (lastMana_ != 0 && view.mana > lastMana_) {
        manaGainAt_ = view.frame; // (not the first look: that's no gain)
    }
    lastMana_ = std::max<std::uint16_t>(view.mana, 1);
    const float part = static_cast<float>(view.mana) / view.maxMana;
    // Full again: done. Not on the way to one and not low: nothing to do yet.
    if (part >= kFullMana || (!manaGoal_ && part >= kLowMana)) {
        manaGoal_.reset();
        manaArrivedAt_.reset();
        return false;
    }
    std::erase_if(skipSources_, [&](const auto& s) { return view.frame >= s.second; });
    const auto& sources = view.manaSources;
    // Given up on: that one, and (drained) its neighbours too, unless they show a charge: obelisks
    // stand in groups that a player beside them drains together, and refill slowly (one charge
    // every 15 frames, 25 s from empty), so the next one along is no better.
    const auto skip = [&](std::uint32_t frames, bool drained) {
        const auto& at = sources[*manaGoal_];
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const auto& o = sources[i];
            if (i == *manaGoal_ ||
                (drained && distance(o.x, o.y, at.x, at.y) <= kObeliskGroup && !(o.charge && *o.charge >= 10))) {
                skipSources_[i] = view.frame + frames;
            }
        }
        manaGoal_.reset();
        manaArrivedAt_.reset();
        nav_.reset();
    };
    if (manaGoal_ && *manaGoal_ >= sources.size()) {
        manaGoal_.reset();
    }
    if (!manaGoal_) {
        // The nearest ours to use (a team's gives only to its team) that has some left: one that
        // refills a player beside it, or any, for a class that drains from afar.
        float best = 0;
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const auto& s = sources[i];
            const bool usable = (s.team == 0 || s.team == view.team) && (s.refills || drains(view)) &&
                                !(s.charge && *s.charge < 10) && !skipSources_.contains(i);
            const float d = distance(view.x, view.y, s.x, s.y);
            if (usable && d <= kManaSearch && (!manaGoal_ || d < best)) {
                manaGoal_ = i;
                best = d;
            }
        }
        if (!manaGoal_) {
            return false;
        }
        manaSince_ = view.frame;
        manaArrivedAt_.reset();
        fetching_ = 0;
        goalSpot_.reset();
        roam_.reset();
        nav_.reset();
    }
    const auto& s = sources[*manaGoal_];
    const float d = distance(view.x, view.y, s.x, s.y);
    if (recharge(view, body, s, d)) {
        if (!manaArrivedAt_) {
            manaArrivedAt_ = view.frame;
            manaWindowAt_ = view.frame + kManaSettle; // (a drain takes a moment to start)
            manaWindowFrom_ = view.mana;
        }
        // Showing (nearly) empty, or under kManaDryGain coming in kManaDry (drained, or not ours
        // after all: a caster's own mana comes back by itself, a point every second or two, and
        // that's no obelisk's): off to another with some, this group left until it has refilled.
        bool dry = s.charge && *s.charge < 3;
        // (Signed: while settling the window's start is still ahead of the frame.)
        if (!dry && static_cast<std::int32_t>(view.frame - manaWindowAt_) >= static_cast<std::int32_t>(kManaDry)) {
            dry = view.mana < manaWindowFrom_ + kManaDryGain;
            manaWindowAt_ = view.frame;
            manaWindowFrom_ = view.mana;
        } else if (static_cast<std::int32_t>(view.frame - manaWindowAt_) < 0) {
            manaWindowFrom_ = view.mana; // (still settling: counted from when it's had time to start)
        }
        if (dry) {
            skip(kSpotRecheck, true);
            return seekMana(view, body); // (straight on to the next, not a frame standing)
        }
        return true;
    }
    manaArrivedAt_.reset();
    if (s.charge && *s.charge < 10) {
        skip(kSpotRecheck, true); // drained on the way there (by someone else): another
        return false;
    }
    if (view.frame - manaSince_ > kGoalTimeout || noWay(view) || stuckFor(view)) {
        skip(kSpotRecheck, false);
        return false;
    }
    // Beside it, on our side (an obelisk is solid: the grid counts its cells blocked).
    std::optional<std::pair<float, float>> spot;
    for (float r = 24; r <= kObeliskReach - 10 && !spot; r += 8) {
        spot = openOnRing(view, s.x, s.y, r);
    }
    if (!spot) {
        skip(kSpotRecheck, false);
        return false;
    }
    body.setAction(false);
    moveTo(view, body, spot->first, spot->second, 6);
    return true;
}

void PlayerBrain::watchForLoops(const BotView& view)
{
    // A fight goes round in a small space by design, and so do a guard's patrol and an escort
    // by a carrier who waits: only the rest is watched.
    if (target_ != 0 || view.role == BotView::Role::Guard || escorted(view)) {
        loopSamples_.clear();
        return;
    }
    if (view.frame - loopSampledAt_ < kLoopSampleEvery) {
        return; // (by time: the server's frames come in jumps, not every one)
    }
    loopSampledAt_ = view.frame;
    loopSamples_.push_back({view.x, view.y});
    if (loopSamples_.size() > kLoopSamples) {
        loopSamples_.erase(loopSamples_.begin());
    }
    if (loopSamples_.size() < kLoopSamples) {
        return;
    }
    // How far we ran over the samples, and their centre.
    float travelled = 0;
    float cx = 0;
    float cy = 0;
    for (std::size_t i = 0; i < loopSamples_.size(); ++i) {
        cx += loopSamples_[i].first;
        cy += loopSamples_[i].second;
        if (i > 0) {
            travelled += distance(loopSamples_[i - 1].first, loopSamples_[i - 1].second, loopSamples_[i].first,
                                  loopSamples_[i].second);
        }
    }
    cx /= static_cast<float>(loopSamples_.size());
    cy /= static_cast<float>(loopSamples_.size());
    const bool tight = std::ranges::all_of(loopSamples_, [&](const auto& p) {
        return distance(p.first, p.second, cx, cy) <= kLoopRadius;
    });
    if (travelled < kLoopTravel || !tight) {
        return; // standing (waiting, guarding, drinking from an obelisk), or getting somewhere
    }
    // In a loop: whatever we were going for, not for a while; somewhere else now.
    ++loopsBroken_;
    if (goalSpot_) {
        checkedSpots_[*goalSpot_] = view.frame + kSpotRecheck;
        goalSpot_.reset();
    }
    if (fetching_ != 0) {
        skipItems_[fetching_] = view.frame + 600;
        fetching_ = 0;
    }
    if (manaGoal_) {
        skipSources_[*manaGoal_] = view.frame + kSpotRecheck;
        manaGoal_.reset();
        manaArrivedAt_.reset();
    }
    teamGoalPausedUntil_ = view.frame + 300;
    roam_.reset();
    nav_.reset();
    loopSamples_.clear();
}

bool PlayerBrain::carryHome(const BotView& view, BotBody& body)
{
    const auto home = flagBase(view, true);
    if (!home) {
        return false;
    }
    fetching_ = 0;
    goalSpot_.reset();
    manaGoal_.reset();
    // The nearest enemy's distance when within r, else 1e9 (none near).
    const auto enemiesNear = [&](float x, float y, float r) {
        float nearest = 1e9f;
        for (const auto& p : view.players) {
            if (isEnemy(view, p)) {
                nearest = std::min(nearest, distance(x, y, p.x, p.y));
            }
        }
        return nearest <= r ? nearest : 1e9f;
    };
    // Ours gone from its base (as we see it from near it): no capture until it's back. Held
    // nearby, off the base, where enemies are furthest, until it is.
    const bool baseInSight = distance(view.x, view.y, home->first, home->second) <= kHoldRadius * 2 && sees(view, home->first, home->second);
    const bool oursHome = std::ranges::any_of(view.flags, [&](const BotView::FlagBase& f) {
        return f.team == view.team && distance(f.x, f.y, home->first, home->second) <= 40;
    });
    if (baseInSight && !oursHome) {
        doing_ = "flag hold";
        escortWaitFrom_ = 0;
        // A hold spot, chosen again every 5 s if enemies come within 200 of it: of 16 points round the
        // base at kHoldRadius with a clear line from it, the one furthest from the enemies (counting up
        // to 600), a little in favour of nearer ones.
        if (!holdSpot_ || (view.frame - holdChosenAt_ > 150 && enemiesNear(holdSpot_->first, holdSpot_->second, 200) < 1e9f)) {
            holdChosenAt_ = view.frame;
            std::optional<std::pair<float, float>> best;
            float bestScore = 0;
            for (int k = 0; k < 16; ++k) {
                const auto [ux, uy] = unitOf(static_cast<std::uint8_t>(k * 16));  // 16 directions, 16 apart on the 256-step circle
                const float x = home->first + ux * kHoldRadius;
                const float y = home->second + uy * kHoldRadius;
                if (blockedAt(view, x, y) || hazardAt(view, x, y) ||
                    (view.nav && !view.nav->empty() && !view.nav->clearLine({static_cast<int>(home->first), static_cast<int>(home->second)},
                                                                            {static_cast<int>(x), static_cast<int>(y)}))) {
                    continue;
                }
                // Score: distance from the enemies (capped at 600), less 0.3 of the walk there.
                const float score = std::min(enemiesNear(x, y, 600), 600.0f) - 0.3f * distance(view.x, view.y, x, y);
                if (!best || score > bestScore) {
                    best = std::pair{x, y};
                    bestScore = score;
                }
            }
            holdSpot_ = best ? best : std::optional{std::pair{view.x, view.y}};
        }
        if (distance(view.x, view.y, holdSpot_->first, holdSpot_->second) > 16) {
            moveTo(view, body, holdSpot_->first, holdSpot_->second, 12);
        } else {
            body.setMove(std::nullopt);
        }
        return true;
    }
    holdSpot_.reset();
    // Our escort well behind and nobody about: a moment for them to catch up (not for ever,
    // and not again straight after).
    float escortGap = 0;
    bool escortSeen = false;
    for (const auto e : view.escorts) {
        if (const auto it = std::ranges::find(view.players, e, &SeenPlayer::netCode);
            it != view.players.end() && it->placed && !it->dead) {
            const float d = distance(view.x, view.y, it->x, it->y);
            escortGap = escortSeen ? std::min(escortGap, d) : d;
            escortSeen = true;
        }
    }
    // (Not hurt lately, nor badly hurt at all: then home's the safest place to be.)
    const bool hurting = (hurtAt_ != 0 && view.frame - hurtAt_ < kHurtMemory) ||
                         (view.maxHealth > 0 && view.health < view.maxHealth / 2);
    if (escortSeen && escortGap > kEscortGap && !hurting && enemiesNear(view.x, view.y, 350) == 1e9f &&
        view.frame >= noEscortWaitUntil_) {
        if (escortWaitFrom_ == 0) {
            escortWaitFrom_ = view.frame;
        }
        if (view.frame - escortWaitFrom_ <= kEscortPatience) {
            doing_ = "wait escort";
            body.setMove(std::nullopt);
            return true;
        }
        noEscortWaitUntil_ = view.frame + kEscortWaitAgain;
    }
    escortWaitFrom_ = 0;
    doing_ = "flag home";
    moveTo(view, body, home->first, home->second, 10);
    return true;
}

void PlayerBrain::look(const BotView& view, BotBody& body)
{
    // Carrying their flag: straight home with it, whatever our part, no stops for items.
    // (Our flag dropped on the way: over it first, as it must be home for a capture.)
    const auto* ownDropped = view.frame >= flagSkipUntil_ ? looseFlag(view, true) : nullptr;
    if (view.carryingFlag) {
        if (ownDropped && distance(view.x, view.y, ownDropped->x, ownDropped->y) <= kFlagDash * 2 &&
            toFlag(view, body, *ownDropped)) {
            return;
        }
        if (carryHome(view, body)) {
            return;
        }
    } else {
        holdSpot_.reset();
        escortWaitFrom_ = 0;
    }
    // Our flag dropped near enough: over it, which sends it home (whatever our part). Theirs:
    // taken where it lies, whatever our part, if it's not far.
    if (ownDropped && distance(view.x, view.y, ownDropped->x, ownDropped->y) <= 900 && toFlag(view, body, *ownDropped)) {
        return;
    }
    if (const auto* theirs = view.frame >= flagSkipUntil_ ? looseFlag(view, false) : nullptr;
        theirs && (view.role == BotView::Role::Attack || distance(view.x, view.y, theirs->x, theirs->y) <= 600) &&
        toFlag(view, body, *theirs)) {
        return;
    }
    // Hunting whoever has our flag, or with whoever has theirs: that before items.
    if ((view.role == BotView::Role::Hunt || view.role == BotView::Role::Escort) &&
        view.frame >= teamGoalPausedUntil_ && teamGoal(view, body)) {
        doing_ = roleName(view.role);
        return;
    }
    // An attacker with the gear it wants is off for their flag: only items on its way (a potion
    // a little further off when hurt).
    const bool onMission = view.role == BotView::Role::Attack && !underGeared(view) && flagBase(view, false);
    const bool hurt = view.maxHealth > 0 && view.health < 0.6f * view.maxHealth;
    if (const SeenItem* item = chooseItem(view, 0, onMission ? (hurt ? kMissionPotionReach : kMissionItemReach) : kItemReach)) {
        doing_ = "item";
        fetch(view, body, *item);
        return;
    }
    if (seekMana(view, body)) {
        doing_ = "mana";
        return;
    }
    // Poorly armed: the gear first, whatever the team wants; then the team's goal; then where
    // the map keeps something we'd want.
    if (underGeared(view) && view.role != BotView::Role::Guard && gearRun(view, body, 25)) {
        doing_ = "gear run";
        return;
    }
    if (view.frame >= teamGoalPausedUntil_ && teamGoal(view, body)) {
        doing_ = roleName(view.role);
        return;
    }
    if (gearRun(view, body, 1)) {
        doing_ = "gear run";
        return;
    }
    doing_ = "roam";
    // Roam: somewhere open within 500 that a path reaches (a random point can be off the map's
    // floor, which the wall grid doesn't know), then somewhere else.
    if (!roam_ || view.frame >= roamUntil_ || stuckFor(view) || noWay(view)) {
        roam_.reset();
        nav_.reset();
        nox::util::uniform_real_distribution<float> spread(-500, 500);
        for (int tries = 0; tries < 30 && !roam_; ++tries) {
            const float rx = view.x + spread(rng_);
            const float ry = view.y + spread(rng_);
            const int cx = static_cast<int>(rx) / client::NavGrid::kCellSize;
            const int cy = static_cast<int>(ry) / client::NavGrid::kCellSize;
            if (rx < 0 || ry < 0 ||
                (view.nav && !view.nav->empty() && (view.nav->blocked(cx, cy) || view.nav->hazard(cx, cy)))) {
                continue;
            }
            roam_ = std::pair{rx, ry};
        }
        roamUntil_ = view.frame + nox::util::uniform_int_distribution<std::uint32_t>(240, 480)(rng_);
    }
    if (roam_) {
        moveTo(view, body, roam_->first, roam_->second, 30);
        if (distance(view.x, view.y, roam_->first, roam_->second) <= 30) {
            roam_.reset();
        } else if (nav_.hazardAhead(view) > kJumpableHazard) {
            roam_.reset(); // across the lava: somewhere else next time
            nav_.reset();
            body.setMove(std::nullopt);
        }
    } else {
        body.setMove(std::nullopt);
    }
}

} // namespace nox::bot
