// (noxcpp) The game controller's aim (pad_aim.hpp).
#include "pad/pad_aim.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace noxsdl::pad {

namespace {

constexpr float kDegrees = 180.0f / std::numbers::pi_v<float>;
constexpr float kTalkReach = 125; ///< the longest hand's reach (Talk, Trade)

const AimCandidate* findCandidate(std::span<const AimCandidate> list, std::uint16_t id)
{
    for (const AimCandidate& c : list) {
        if (c.id == id) {
            return &c;
        }
    }
    return nullptr;
}

/// The angle of a way round us, 0 to 360 (screen: clockwise, y down).
float bearing(StickVec v)
{
    float a = std::atan2(v.y, v.x) * kDegrees;
    return a < 0 ? a + 360.0f : a;
}

} // namespace

float angleBetween(StickVec a, StickVec b)
{
    const float la = a.length();
    const float lb = b.length();
    if (la <= 0 || lb <= 0) {
        return 0;
    }
    const float c = std::clamp((a.x * b.x + a.y * b.y) / (la * lb), -1.0f, 1.0f);
    return std::acos(c) * kDegrees;
}

float distance(WorldPoint a, WorldPoint b)
{
    return std::hypot(b.x - a.x, b.y - a.y);
}

StickVec wayTo(WorldPoint a, WorldPoint b)
{
    const float d = distance(a, b);
    if (d <= 1e-4f) {
        return {1, 0};
    }
    return {(b.x - a.x) / d, (b.y - a.y) / d};
}

Side creatureSide(const CreatureFacts& c, std::uint32_t ownTeam, std::uint32_t gameFlags)
{
    constexpr std::uint32_t kSoloOrQuest = 0x1800;
    constexpr std::uint32_t kShopkeeper = 0x8;
    if (c.player) {
        if ((gameFlags & kSoloOrQuest) || (ownTeam != 0 && c.team == ownTeam)) {
            return Side::Ally;
        }
        return Side::Enemy;
    }
    if (c.friendListed || c.partyCreature) {
        return Side::Ally;
    }
    if ((c.subclasses & kShopkeeper) || c.talks) {
        return Side::Neutral;
    }
    return Side::Enemy;
}

AimTuning aimTuning(AimAssist a)
{
    switch (a) {
    case AimAssist::Off: return {0, 0};
    case AimAssist::Weak: return {20, 7};
    case AimAssist::Normal: return {35, 12};
    case AimAssist::Strong: return {50, 12};
    }
    return {35, 12};
}

std::optional<float> coneScore(const AimCandidate& c, WorldPoint from, StickVec dir, const AimTuning& t, float reach, bool idle)
{
    const float d = distance(from, c.at);
    if (d > reach || reach <= 0) {
        return std::nullopt;
    }
    float limit = c.player ? t.playerDeg : t.coneDeg;
    if (!c.player && limit > 0 && idle && d < kIdleRange) {
        limit = 180; // (the sticks at rest: a monster at hand, whichever way it is)
    } else if (!c.player && d < kCloseRange) {
        limit *= 2; // (one at our elbow is ours whichever way we lean)
    }
    if (limit <= 0) {
        return std::nullopt;
    }
    const float angle = d < 1.0f ? 0.0f : angleBetween(dir, wayTo(from, c.at));
    if (angle > limit) {
        return std::nullopt;
    }
    return 0.65f * angle / limit + 0.35f * d / reach;
}

void Targeter::clear()
{
    *this = Targeter{};
}

std::optional<std::uint16_t> Targeter::update(const TargetFrame& f)
{
    const AimCandidate* cur = current_ ? findCandidate(f.enemies, *current_) : nullptr;
    if (current_ && !cur) {
        clear(); // dead, gone, or out of sight
    }
    held_ = false;
    // A flick: the nearest monster that way, from the one locked (stepping on) or from us.
    if (f.flick && f.flick->length() > 0) {
        const WorldPoint origin = cur ? cur->at : f.from;
        const AimCandidate* best = nullptr;
        float bestCost = 0;
        for (const AimCandidate& c : f.enemies) {
            if (c.player || (cur && c.id == cur->id) || distance(f.from, c.at) > f.reach) {
                continue;
            }
            const float angle = angleBetween(*f.flick, wayTo(origin, c.at));
            if (angle > kFlickConeDeg) {
                continue;
            }
            const float cost = distance(origin, c.at) * (1.0f + angle / kFlickConeDeg);
            if (!best || cost < bestCost) {
                best = &c;
                bestCost = cost;
            }
        }
        if (best) {
            current_ = best->id;
            cur = best;
            manual_ = true;
            outFor_ = 0;
        }
    }
    // TargetNext / Previous: the next monster round us, clockwise or back.
    if (f.step != 0) {
        const float from = bearing(cur ? wayTo(f.from, cur->at) : f.dir);
        const AimCandidate* best = nullptr;
        float bestTurn = 0;
        for (const AimCandidate& c : f.enemies) {
            if (c.player || (cur && c.id == cur->id) || distance(f.from, c.at) > f.reach) {
                continue;
            }
            float turn = (bearing(wayTo(f.from, c.at)) - from) * (f.step > 0 ? 1.0f : -1.0f);
            while (turn <= 0) {
                turn += 360.0f;
            }
            if (!best || turn < bestTurn) {
                best = &c;
                bestTurn = turn;
            }
        }
        if (best) {
            current_ = best->id;
            cur = best;
            manual_ = true;
            outFor_ = 0;
        }
    }
    if (manual_ && cur) {
        if (distance(f.from, cur->at) <= f.reach * 1.5f) {
            held_ = true;
            return current_;
        }
        clear();
        cur = nullptr;
    }
    manual_ = false;
    if (f.assist == AimAssist::Off) {
        current_.reset();
        return std::nullopt;
    }
    const AimTuning t = aimTuning(f.assist);
    const auto score = [&](const AimCandidate& c) { return coneScore(c, f.from, f.dir, t, f.reach, f.idle); };
    // Held by the attack or a slot button: a monster stays (players get the soft rule's short keep).
    if (cur && f.lockHeld && f.lockMode == LockMode::WhileHeld && !cur->player && distance(f.from, cur->at) <= f.reach * 1.5f) {
        held_ = true;
        outFor_ = 0;
        return current_;
    }
    const AimCandidate* best = nullptr;
    float bestScore = 0;
    for (const AimCandidate& c : f.enemies) {
        if (const auto s = score(c); s && (!best || *s < bestScore)) {
            best = &c;
            bestScore = *s;
        }
    }
    if (cur) {
        if (const auto s = score(*cur)) {
            outFor_ = 0;
            if (best && best->id != cur->id && bestScore < *s - kSwitchMargin) {
                current_ = best->id;
            }
            return current_;
        }
        outFor_ += f.seconds;
        if (outFor_ < (cur->player ? kPlayerKeepSeconds : kKeepSeconds)) {
            return current_;
        }
    }
    outFor_ = 0;
    current_ = best ? std::optional<std::uint16_t>(best->id) : std::nullopt;
    return current_;
}

RightStickFrame RightStick::update(StickVec v, float seconds)
{
    RightStickFrame out;
    const float m = v.length();
    switch (phase_) {
    case Phase::Idle:
        if (m > 0) {
            phase_ = Phase::Pending;
            t_ = 0;
            peak_ = m;
            peakDir_ = {v.x / m, v.y / m};
        }
        break;
    case Phase::Pending:
        t_ += seconds;
        if (m > peak_) {
            peak_ = m;
            peakDir_ = {v.x / m, v.y / m};
        }
        if (m < kFlickBack) {
            if (peak_ >= kFlickTilt) {
                out.flick = peakDir_;
            }
            phase_ = Phase::Idle;
        } else if (t_ >= kFlickSeconds) {
            phase_ = Phase::Aiming;
            way_ = {v.x / m, v.y / m};
        }
        break;
    case Phase::Aiming:
        if (m <= 0) {
            phase_ = Phase::Idle; // (let go: the way it was held is the caller's last way)
        } else if (m >= kAimHoldTilt) {
            way_ = {v.x / m, v.y / m};
        }
        break;
    }
    if (phase_ == Phase::Aiming) {
        out.aim = way_;
    }
    return out;
}

StickVec aimWay(std::optional<StickVec> right, StickVec left, std::optional<StickVec> last, StickVec fallback)
{
    const auto unit = [](StickVec v) -> std::optional<StickVec> {
        const float l = v.length();
        return l > 0 ? std::optional<StickVec>(StickVec{v.x / l, v.y / l}) : std::nullopt;
    };
    if (const auto r = right ? unit(*right) : std::nullopt) {
        return *r;
    }
    if (const auto l = unit(left)) {
        return *l;
    }
    if (const auto k = last ? unit(*last) : std::nullopt) {
        return *k;
    }
    return unit(fallback).value_or(StickVec{1, 0});
}

SpellAim spellAim(std::uint32_t spellFlags, bool onSelf)
{
    if (onSelf) {
        return SpellAim::Self;
    }
    if (spellFlags & (0x4u | 0x80u)) {
        return SpellAim::Creature;
    }
    return SpellAim::Point;
}

SpellAim abilityAim(std::uint32_t ability)
{
    return ability == 1 || ability == 3 ? SpellAim::Point : SpellAim::Self;
}

std::optional<WorldPoint> castPoint(SpellAim kind, std::optional<WorldPoint> target, WorldPoint from, StickVec dir)
{
    if (kind == SpellAim::Self) {
        return std::nullopt;
    }
    if (target) {
        return target;
    }
    const float l = dir.length();
    const StickVec d = l > 0 ? StickVec{dir.x / l, dir.y / l} : StickVec{1, 0};
    return WorldPoint{from.x + d.x * kCastAhead, from.y + d.y * kCastAhead};
}

std::optional<std::size_t> bestInteract(std::span<const InteractOption> options, WorldPoint from, StickVec dir,
                                        std::span<const std::uint32_t> skip)
{
    const auto pick = [&](bool skipping) -> std::optional<std::size_t> {
        std::optional<std::size_t> best;
        float bestScore = 0;
        for (std::size_t i = 0; i < options.size(); ++i) {
            const InteractOption& o = options[i];
            if (skipping && std::ranges::find(skip, o.key) != skip.end()) {
                continue;
            }
            const float d = distance(from, o.at);
            if (d > o.reach) {
                continue;
            }
            const float angle = d < kUnderfoot ? 0.0f : angleBetween(dir, wayTo(from, o.at));
            if (angle > 120.0f) {
                continue;
            }
            // (One scale for all, the angle weighing more than the distance: what's in front wins
            // over what's nearer but off to the side; someone to talk to in front more so.)
            float s = 0.4f * d / kTalkReach + 0.6f * angle / 180.0f;
            if (o.talks && angle <= kFacingCone) {
                s -= kTalkPreference;
            }
            if (!best || s < bestScore) {
                best = i;
                bestScore = s;
            }
        }
        return best;
    };
    if (const auto b = pick(true)) {
        return b;
    }
    return skip.empty() ? std::nullopt : pick(false);
}

} // namespace noxsdl::pad
