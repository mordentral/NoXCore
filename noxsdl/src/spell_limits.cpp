// SpellLimits (spell_limits.hpp): new objects claimed for our recent casts by spell, time and
// place, so the HUD can mark a spell the server would refuse now.
#include "spell_limits.hpp"

#include "noxcore/things/thing_db.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace noxsdl {

namespace {

constexpr std::uint32_t kFist = 0x1D, kForceOfNature = 0x1F, kMagicMissile = 0x32, kMeteor = 0x34, kPixies = 0x3A;  // (spell ids)

/// The spell an object of this type would belong to, and whether it starts at the caster
/// (else where the spell was aimed).
struct Tracked {
    std::string_view type;
    std::uint32_t spell;
    bool atCaster;
};
constexpr std::array<Tracked, 7> kTracked{{
    {"SmallFist", kFist, false},
    {"MediumFist", kFist, false},
    {"LargeFist", kFist, false},
    {"DeathBall", kForceOfNature, true},
    {"Meteor", kMeteor, false},
    {"MagicMissile", kMagicMissile, true},
    {"Pixie", kPixies, true},
}};

const Tracked* tracked(std::string_view type)
{
    for (const auto& t : kTracked) {
        if (t.type == type) {
            return &t;
        }
    }
    return nullptr;
}

/// How long after the cast its objects may appear: the cast itself, and Force of Nature's charge
/// (its duration spell runs (30 * 2) / 3 frames before the ball is made, spellAccept case 0x1F).
std::uint32_t window(std::uint32_t spell)
{
    return spell == kForceOfNature ? 90 : 60;
}

float dist2(float ax, float ay, float bx, float by)
{
    return (ax - bx) * (ax - bx) + (ay - by) * (ay - by);
}

} // namespace

void SpellLimits::cast(std::uint32_t spell, std::uint32_t frame, float x, float y, float cursorX, float cursorY)
{
    // (Only the spells whose objects kTracked names.)
    if (std::ranges::any_of(kTracked, [spell](const Tracked& t) { return t.spell == spell; })) {
        pending_.push_back({spell, frame, x, y, cursorX, cursorY});
    }
}

void SpellLimits::update(const nox::client::World& world, std::uint16_t us, std::uint32_t frame)
{
    // Casts too old to have made anything are forgotten; owned objects gone (or their net code reused
    // by another type) let go.
    std::erase_if(pending_, [&](const Pending& p) { return frame > p.frame + window(p.spell); });
    const auto& drawables = world.drawables();
    std::erase_if(owned_, [&](const auto& e) {
        const auto it = drawables.find(e.first);
        return it == drawables.end() || it->second.typeId != e.second.typeId;
    });

    // The tracked types by type id, by their names in the world's thing.bin (an unknown type id's
    // name, "#<id>", is none of them).
    if (const nox::things::ThingDb* things = world.things(); things != trackedOf_) {
        trackedOf_ = things;
        trackedByType_.assign(things ? things->things.size() + 1 : 0, std::int8_t{-1});
        for (std::size_t id = 1; id < trackedByType_.size(); ++id) {
            if (const Tracked* t = tracked(things->things[id - 1].name)) {
                trackedByType_[id] = static_cast<std::int8_t>(t - kTracked.data());
            }
        }
    }
    const auto seen = [](const std::vector<std::uint64_t>& bits, std::uint16_t netCode) {
        return (bits[netCode >> 6] >> (netCode & 63)) & 1;
    };
    const auto self = drawables.find(us);
    std::ranges::fill(now_, std::uint64_t{0});
    // Each drawable not seen before, of a tracked type: claimed for the latest matching cast near
    // enough.
    for (const auto& [netCode, d] : drawables) {
        now_[netCode >> 6] |= std::uint64_t{1} << (netCode & 63);
        if (seen(known_, netCode) || owned_.contains(netCode)) {
            continue;
        }
        const Tracked* t = d.typeId < trackedByType_.size() && trackedByType_[d.typeId] >= 0
                               ? &kTracked[static_cast<std::size_t>(trackedByType_[d.typeId])]
                               : nullptr;
        if (!t) {
            continue;
        }
        const auto x = static_cast<float>(d.x);
        const auto y = static_cast<float>(d.y);
        // The latest cast of that spell it could have come from.
        for (auto p = pending_.rbegin(); p != pending_.rend(); ++p) {
            if (p->spell != t->spell) {
                continue;
            }
            bool near = false;
            if (t->atCaster) {
                constexpr float kReach = 150; // it may have moved a frame or two before we saw it
                near = dist2(x, y, p->x, p->y) < kReach * kReach ||
                       (self != drawables.end() &&
                        dist2(x, y, static_cast<float>(self->second.x), static_cast<float>(self->second.y)) < kReach * kReach);
            } else {
                constexpr float kAim = 200; // the server aims at the cursor or the unit under it
                near = dist2(x, y, p->cursorX, p->cursorY) < kAim * kAim;
            }
            if (near) {
                owned_[netCode] = {t->spell, d.typeId};
                break;
            }
        }
    }
    known_.swap(now_);
}

std::set<std::uint32_t> SpellLimits::limited(const nox::client::World& world, const nox::things::GameData& gameData,
                                              std::uint32_t gameFlags) const
{
    std::map<std::uint32_t, int> count;
    for (const auto& [netCode, o] : owned_) {
        ++count[o.spell];
    }
    std::set<std::uint32_t> out;
    for (const auto spell : {kFist, kForceOfNature, kMeteor}) {
        if (count[spell] > 0) {
            out.insert(spell);
        }
    }
    // Power 3 in multiplayer games (flags 0x570), else the spell's level.
    const auto power = [&](std::uint32_t spell) -> int {
        if (world.game().gameFlags & 0x570) {
            return 3;
        }
        const auto it = world.self().spellLevels.find(static_cast<std::uint8_t>(spell));
        return it != world.self().spellLevels.end() && it->second > 0 ? it->second : 3;
    };
    const auto cap = [&](std::string_view table, std::uint32_t spell) {
        const auto index = static_cast<std::size_t>(std::clamp(power(spell), 1, 5) - 1);
        // (gamedataGetFloatTable: 0 for a table the file lacks.)
        return static_cast<int>(nox::things::gameNumber(&gameData, table, gameFlags, index));
    };
    if (count[kMagicMissile] > 0 && count[kMagicMissile] >= cap("MagicMissileCount", kMagicMissile)) {
        out.insert(kMagicMissile);
    }
    if (count[kPixies] > 0 && count[kPixies] >= cap("PixieCount", kPixies)) {
        out.insert(kPixies);
    }
    return out;
}

void SpellLimits::clear()
{
    pending_.clear();
    owned_.clear();
    std::ranges::fill(known_, std::uint64_t{0});
}

} // namespace noxsdl
