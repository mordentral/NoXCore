// DRAW payload decoder; the per-kind layouts are listed in draw.hpp.
#include "noxcore/things/draw.hpp"

#include "thing_io.hpp"

#include <utility>

namespace nox::things {

namespace {

using detail::readAnimation;
using detail::readImageRef;
using detail::readImageRefs;
using detail::readString8;

constexpr std::uint32_t kEndTag = 0x454E4420; // "END " stored byte-reversed

struct NamedKind {
    std::string_view name;
    DrawKind kind;
};

// Draw type name -> payload kind (the rows of the exe's draw table that carry images).
constexpr NamedKind kKinds[] = {
    {"StaticDraw", DrawKind::Static},
    {"WeaponDraw", DrawKind::Static},
    {"ArmorDraw", DrawKind::Static},
    {"BaseDraw", DrawKind::Static},
    {"StaticRandomDraw", DrawKind::Random},
    {"DoorDraw", DrawKind::Random},
    {"SlaveDraw", DrawKind::Random},
    {"BoulderDraw", DrawKind::Random},
    {"ArrowDraw", DrawKind::Random},
    {"WeakArrowDraw", DrawKind::Random},
    {"HarpoonDraw", DrawKind::Random},
    {"AnimateDraw", DrawKind::Animate},
    {"GlyphDraw", DrawKind::Animate},
    {"WeaponAnimateDraw", DrawKind::Animate},
    {"ArmorAnimateDraw", DrawKind::Animate},
    {"FlagDraw", DrawKind::Animate},
    {"SphericalShieldDraw", DrawKind::Animate},
    {"SummonEffectDraw", DrawKind::Animate},
    {"ConditionalAnimateDraw", DrawKind::Conditional},
    {"MonsterGeneratorDraw", DrawKind::Conditional},
    {"VectorAnimateDraw", DrawKind::Vector},
    {"ReleasedSoulDraw", DrawKind::Vector},
    {"MonsterDraw", DrawKind::Monster},
    {"MaidenDraw", DrawKind::Monster},
    {"AnimateStateDraw", DrawKind::AnimateState},
    {"PlayerDraw", DrawKind::Player},
};

// Animation header, then `frames` images for each of the 8 directions in GridDirection order.
DirectionalAnimation readDirectional(ByteReader& r)
{
    DirectionalAnimation a;
    const std::uint8_t frames = r.u8();
    a.frameDelay = r.u8();
    a.kind = readString8(r);
    a.playback = animPlayback(a.kind);
    for (auto& dir : a.directions) {
        dir = readImageRefs(r, frames);
    }
    return a;
}

constexpr std::uint32_t kStateTag = 0x53544154; // "STAT" stored byte-reversed

template <std::size_t N>
std::optional<std::size_t> indexOf(const std::array<std::string_view, N>& names, std::string_view name)
{
    for (std::size_t i = 0; i < N; ++i) {
        if (names[i] == name) {
            return i;
        }
    }
    return std::nullopt;
}

// States until "END "; each state's parts run until the next "STAT" tag (another state) or "END ".
std::vector<std::optional<PlayerState>> readPlayerStates(ByteReader& r)
{
    std::vector<std::optional<PlayerState>> states(kPlayerStates.size());
    std::uint32_t tag = r.u32();
    while (tag != kEndTag) {
        const std::string name = readString8(r);
        const auto index = indexOf(kPlayerStates, name);
        if (!index) {
            r.fail("unknown player state " + name);
        }
        PlayerState s;
        s.frames = r.u8();
        s.frameDelay = r.u8();
        s.kind = readString8(r);
        s.playback = animPlayback(s.kind);
        // `tag` is reused: the tag that ends this state's parts is the one the outer loop tests.
        for (tag = r.u32(); tag != kEndTag && tag != kStateTag; tag = r.u32()) {
            const std::string part = readString8(r);
            PlayerPart frames;
            for (auto& dir : frames) {
                dir = readImageRefs(r, s.frames);
            }
            if (part == "NAKED") {
                s.naked = std::move(frames);
            } else if (const auto armor = indexOf(kArmorParts, part)) {
                s.armor[*armor] = std::move(frames);
            } else if (const auto weapon = indexOf(kWeaponParts, part)) {
                s.weapons[*weapon] = std::move(frames);
            } else {
                r.fail("unknown player part " + part);
            }
        }
        states[*index] = std::move(s);
    }
    return states;
}

} // namespace

// Tries each kind's fields in turn and takes the first frame found (south-facing where there
// are directions; for players, the naked body).
std::optional<ImageRef> DrawSpec::firstImage() const
{
    if (!images.empty()) {
        return images.front();
    }
    for (const auto& a : animations) {
        if (!a.frames.empty()) {
            return a.frames.front();
        }
    }
    const auto south = [](const DirectionalAnimation& d) -> std::optional<ImageRef> {
        const auto& frames = d.directions[static_cast<std::size_t>(GridDirection::S)];
        return frames.empty() ? std::nullopt : std::optional(frames.front());
    };
    if (directional) {
        return south(*directional);
    }
    for (const auto& s : monsterStates) {
        if (s) {
            if (auto img = south(s->animation)) {
                return img;
            }
        }
    }
    for (const auto& a : stateAnimations) {
        if (a && !a->frames.empty()) {
            return a->frames.front();
        }
    }
    for (const auto& s : playerStates) {
        if (s && s->naked) {
            const auto& frames = (*s->naked)[static_cast<std::size_t>(GridDirection::S)];
            if (!frames.empty()) {
                return frames.front();
            }
        }
    }
    return std::nullopt;
}

DrawKind drawKind(std::string_view type)
{
    for (const auto& k : kKinds) {
        if (k.name == type) {
            return k.kind;
        }
    }
    return DrawKind::None;
}

DrawSpec decodeDraw(const ThingDraw& draw)
{
    DrawSpec spec;
    spec.kind = drawKind(draw.type);
    ByteReader r(draw.data, draw.type);
    // One payload per kind (layouts in draw.hpp); the reader must end exactly at the payload's end.
    switch (spec.kind) {
    case DrawKind::None:
        return spec;
    case DrawKind::Player:
        spec.playerStates = readPlayerStates(r);
        break;
    case DrawKind::Static:
        spec.images.push_back(readImageRef(r));
        break;
    case DrawKind::Random:
        spec.images = readImageRefs(r, r.u8());  // u8 count, then that many image refs
        break;
    case DrawKind::Animate:
        spec.animations.push_back(readAnimation(r));
        break;
    case DrawKind::Conditional: {
        // u8 count, then that many animations (one per condition).
        const std::uint8_t n = r.u8();
        for (int i = 0; i < n; ++i) {
            spec.animations.push_back(readAnimation(r));
        }
        break;
    }
    case DrawKind::Vector:
        spec.directional = readDirectional(r);
        break;
    case DrawKind::Monster:
        // Each state record starts with a u32 tag; "END " finishes the list.
        while (r.u32() != kEndTag) {
            const std::uint8_t state = r.u8();
            if (state >= spec.monsterStates.size()) {
                r.fail("monster state out of range");
            }
            MonsterState s;
            s.name = readString8(r);
            s.sound = readString8(r);
            s.animation = readDirectional(r);
            spec.monsterStates[state] = std::move(s);
        }
        break;
    case DrawKind::AnimateState:
        while (r.u32() != kEndTag) {
            const std::uint32_t flags = r.u32();
            // The lowest of flags 2, 4, 8 picks the slot.
            std::size_t slot = 0;
            if (flags & 2) {
                slot = 0;
            } else if (flags & 4) {
                slot = 1;
            } else if (flags & 8) {
                slot = 2;
            } else {
                r.fail("animation state without a slot flag");
            }
            spec.stateNames[slot] = readString8(r);
            readString8(r); // sound
            spec.stateAnimations[slot] = readAnimation(r);
        }
        break;
    }
    r.expectEnd();
    return spec;
}

} // namespace nox::things
