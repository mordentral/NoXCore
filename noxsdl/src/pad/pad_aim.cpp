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
    case AimAssist::Off: return {0, 0, 0, 0};
    case AimAssist::Weak: return {20, 7, 24, 8};
    case AimAssist::Normal: return {35, 12, 40, 14};
    case AimAssist::Strong: return {50, 12, 56, 14};
    }
    return {35, 12, 40, 14};
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

std::optional<float> reticleScore(const AimCandidate& c, WorldPoint reticle, const AimTuning& t)
{
    const float snap = c.player ? t.playerSnap : t.snap;
    const float d = distance(reticle, c.at);
    if (snap <= 0 || d > snap) {
        return std::nullopt;
    }
    return d / snap;
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
    const auto score = [&](const AimCandidate& c) {
        return f.reticle ? reticleScore(c, *f.reticle, t) : coneScore(c, f.from, f.dir, t, f.reach, f.idle);
    };
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

RightStickFrame RightStick::update(StickVec v, float seconds, float sensitivity)
{
    RightStickFrame out;
    const float m = v.length();
    switch (phase_) {
    case Phase::Idle:
    case Phase::Lingering:
        if (m > 0) {
            phase_ = Phase::Pending;
            t_ = 0;
            peak_ = m;
            peakDir_ = {v.x / m, v.y / m};
        } else if (phase_ == Phase::Lingering) {
            t_ += seconds;
            if (t_ >= kReticleLinger) {
                phase_ = Phase::Idle;
                shown_ = false;
            }
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
            phase_ = shown_ ? Phase::Lingering : Phase::Idle;
            t_ = 0;
        } else if (t_ >= kFlickSeconds) {
            phase_ = Phase::Aiming;
            shown_ = true;
            offset_ = v;
        }
        break;
    case Phase::Aiming:
        if (m <= 0) {
            phase_ = Phase::Lingering;
            t_ = 0;
        } else if (m >= kReticleHoldTilt) {
            // (eased toward the stick, so it glides; let go, it stays where it was aimed)
            const float k = std::clamp(seconds * 14.0f * std::clamp(sensitivity, 0.25f, 4.0f), 0.0f, 1.0f);
            offset_.x += (v.x - offset_.x) * k;
            offset_.y += (v.y - offset_.y) * k;
        }
        break;
    }
    if (shown_) {
        out.reticle = offset_;
    }
    return out;
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

std::optional<WorldPoint> castPoint(SpellAim kind, std::optional<WorldPoint> target, std::optional<WorldPoint> reticle,
                                    WorldPoint from, StickVec dir)
{
    if (kind == SpellAim::Self) {
        return std::nullopt;
    }
    if (target) {
        return target;
    }
    if (reticle) {
        return reticle;
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
            const float s = 0.55f * d / kTalkReach + 0.45f * angle / 180.0f; // (one scale for all: the nearer wins)
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

StickVec freeCursorStep(StickVec v, float seconds, float sensitivity, float scale, float& heldFor)
{
    const float m = v.length();
    if (m <= 0) {
        heldFor = 0;
        return {};
    }
    heldFor = m > 0.9f ? heldFor + seconds : 0.0f;
    const float accel = 1.0f + 1.5f * std::min(heldFor, 1.0f);
    const float speed = kFreeCursorSpeed * std::clamp(sensitivity, 0.25f, 4.0f) * std::max(scale, 0.25f) * m * accel; // (m * |v| = tilt squared)
    return {v.x * speed * seconds, v.y * speed * seconds};
}

} // namespace noxsdl::pad
