// ThingArt (thing_art.hpp): thing.bin types decoded into TypeArt once each, and the image and
// animation frame a drawable shows for its pose.
#include "thing_art.hpp"

#include "noxcore/client/fade.hpp"
#include "noxcore/client/state_draw.hpp"
#include "noxcore/things/object_class.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <exception>

namespace noxsdl {

namespace {

/// Leading integers of an attribute value such as "128 128".
template <std::size_t N>
std::array<int, N> readInts(std::string_view text)
{
    std::array<int, N> out{};
    std::size_t n = 0;
    const char* p = text.data();
    const char* end = text.data() + text.size();
    while (n < N && p < end) {
        while (p < end && (*p == ' ' || *p == '\t')) {
            ++p;
        }
        const auto [next, ec] = std::from_chars(p, end, out[n]);
        if (ec != std::errc{}) {
            break;
        }
        ++n;
        p = next;
    }
    return out;
}

// The animation's frame for the pose (null with no frames).
const nox::things::ImageRef* frameAt(const std::vector<nox::things::ImageRef>& frames, nox::things::AnimPlayback kind,
                                     std::uint8_t delay, const Pose& pose)
{
    if (frames.empty()) {
        return nullptr;
    }
    return &frames[animationFrame(kind, frames.size(), delay, pose)];
}

/// A type's still, or null.
const nox::things::ImageRef* stillOf(const TypeArt& art)
{
    return art.still ? &*art.still : nullptr;
}

/// The DRAW function by name (TypeArt::drawFn).
DrawFn drawFnOf(std::string_view name)
{
    static constexpr std::pair<std::string_view, DrawFn> kNames[] = {
        {"DoorDraw", DrawFn::Door},
        {"NPCDraw", DrawFn::NPC},
        {"MaidenDraw", DrawFn::Maiden},
        {"MonsterDraw", DrawFn::Monster},
        {"FlagDraw", DrawFn::Flag},
        {"SlaveDraw", DrawFn::Slave},
        {"PressurePlateDraw", DrawFn::PressurePlate},
        {"HarpoonDraw", DrawFn::Harpoon},
        {"MagicDraw", DrawFn::Magic},
        {"MagicMissileDraw", DrawFn::MagicMissile},
        {"PixieDraw", DrawFn::Pixie},
        {"SpiderSpitDraw", DrawFn::SpiderSpit},
        {"PlayerWaypointDraw", DrawFn::PlayerWaypoint},
        {"ArrowDraw", DrawFn::Arrow},
        {"WeakArrowDraw", DrawFn::WeakArrow},
        {"LevelUpDraw", DrawFn::LevelUp},
        {"OblivionUpDraw", DrawFn::OblivionUp},
        {"BlueRainDraw", DrawFn::BlueRain},
        {"BoulderDraw", DrawFn::Boulder},
        {"MonsterGeneratorDraw", DrawFn::MonsterGenerator},
        {"GlyphDraw", DrawFn::Glyph},
    };
    for (const auto& [n, fn] : kNames) {
        if (n == name) {
            return fn;
        }
    }
    return DrawFn::Other;
}

/// The CLIENTUPDATE function by name (TypeArt::updateFn).
UpdateFn updateFnOf(std::string_view name)
{
    static constexpr std::pair<std::string_view, UpdateFn> kNames[] = {
        {"FistUpdateDraw", UpdateFn::Fist},
        {"MeteorUpdateDraw", UpdateFn::Meteor},
        {"PitifulFireballUpdateDraw", UpdateFn::PitifulFireball},
        {"WeakFireballUpdateDraw", UpdateFn::WeakFireball},
        {"FireballUpdateDraw", UpdateFn::Fireball},
        {"StrongFireballUpdateDraw", UpdateFn::StrongFireball},
        {"TitanFireballUpdateDraw", UpdateFn::TitanFireball},
        {"MagicMissileUpdateDraw", UpdateFn::MagicMissile},
        {"MagicUpdateDraw", UpdateFn::Magic},
        {"SparkleTrailUpdateDraw", UpdateFn::SparkleTrail},
        {"DeathBallUpdateDraw", UpdateFn::DeathBall},
        {"DeathBallFragmentUpdateDraw", UpdateFn::DeathBallFragment},
        {"DeathBallChargeUpdateDraw", UpdateFn::DeathBallCharge},
        {"TeleportWakeUpdateDraw", UpdateFn::TeleportWake},
        {"ManaBombChargeClientUpdate", UpdateFn::ManaBombCharge},
        {"CloudUpdateDraw", UpdateFn::Cloud},
        {"SmallCloudUpdateDraw", UpdateFn::SmallCloud},
        {"VortexSourceClientUpdate", UpdateFn::VortexSource},
    };
    if (name.empty()) {
        return UpdateFn::None;
    }
    for (const auto& [n, fn] : kNames) {
        if (n == name) {
            return fn;
        }
    }
    return UpdateFn::Other;
}

} // namespace

std::size_t animationFrame(nox::things::AnimPlayback kind, std::size_t frames, std::uint8_t frameDelay, const Pose& pose)
{
    if (frames == 0) {
        return 0;
    }
    const double step = frameDelay + 1.0;
    switch (kind) {
    case nox::things::AnimPlayback::OneShot:
    case nox::things::AnimPlayback::OneShotRemove: {
        const double n = std::max(0.0, pose.time - pose.animStart) / step;
        return std::min(static_cast<std::size_t>(n), frames - 1);
    }
    case nox::things::AnimPlayback::Loop:
        return static_cast<std::size_t>((pose.netCode + std::max(0.0, pose.time)) / step) % frames;
    case nox::things::AnimPlayback::Slave:
        return pose.variant % frames;
    case nox::things::AnimPlayback::Random: {
        // A new random frame every game frame in the game; a fixed hash here, so it only changes
        // with the game frame rather than with every render frame.
        const auto t = static_cast<std::uint32_t>(std::max(0.0, pose.time));
        return ((t * 2654435761u) ^ (pose.netCode * 40503u)) % frames;
    }
    case nox::things::AnimPlayback::LoopAndFade:
    case nox::things::AnimPlayback::Other:
        break;
    }
    return 0;
}

SDL_Point doorLeaf(std::uint8_t angle)
{
    // 32 entries: (-23,-23), (-18,-27), (-12,-30), (-6,-31), (0,-32), ... round the
    // circle clockwise on screen.
    static constexpr SDL_Point kLeaf[32] = {
        {-23, -23}, {-18, -27}, {-12, -30}, {-6, -31}, {0, -32},  {6, -31},   {12, -30},  {18, -27},
        {23, -23},  {27, -18},  {30, -12},  {31, -6},  {32, 0},   {31, 6},    {30, 12},   {27, 18},
        {23, 23},   {18, 27},   {12, 30},   {6, 31},   {0, 32},   {-6, 31},   {-12, 30},  {-18, 27},
        {-23, 23},  {-27, 18},  {-30, 12},  {-31, 6},  {-32, 0},  {-31, -6},  {-30, -12}, {-27, -18},
    };
    return kLeaf[angle & 31];
}

std::size_t directionSlot(std::uint8_t direction)
{
    using nox::things::GridDirection;
    if (direction == 4 || direction > 8) {
        return static_cast<std::size_t>(GridDirection::S);
    }
    // AnimDirs 0-3 keep their slot, 5-8 move down one past the unused centre; anything else faces south.
    return direction < 4 ? direction : direction - 1u;
}

const nox::things::ImageRef* pickImage(const TypeArt& art, const Pose& pose)
{
    using nox::things::DrawKind;
    const auto& d = art.draw;
    switch (d.kind) {
    case DrawKind::Static:
        return d.images.empty() ? stillOf(art) : &d.images.front();
    case DrawKind::Random:
        // (BoulderDraw too: its variant is its roll's image, set + frame.)
        return d.images.empty() ? stillOf(art) : &d.images[pose.variant % d.images.size()];
    case DrawKind::Animate:
    case DrawKind::Conditional:
        if (art.drawFn == DrawFn::MonsterGenerator) {
            // MonsterGeneratorDraw: the animation its xStatus picks, whether enabled or not; its frame
            // by its kind, the last held with 0x800. Of the other kinds it draws nothing. (Every
            // generator's animations loop.)
            const std::size_t slot = nox::client::generatorAnimation(pose.xStatus);
            if (slot >= d.animations.size() || d.animations[slot].frames.empty()) {
                return nullptr;
            }
            const auto& a = d.animations[slot];
            using nox::things::AnimPlayback;
            if (a.playback != AnimPlayback::Loop && a.playback != AnimPlayback::Random && a.playback != AnimPlayback::Slave) {
                return nullptr;
            }
            return nox::client::generatorHoldsLast(pose.xStatus) ? &a.frames.back()
                                                                  : frameAt(a.frames, a.playback, a.frameDelay, pose);
        }
        if (!d.animations.empty()) {
            // ConditionalAnimateDraw plays its first animation while the object is
            // enabled (ObjFlags 0x1000000: a lit flame basin) and its second while it's off.
            const bool off = d.kind == DrawKind::Conditional && !pose.enabled && d.animations.size() > 1;
            const auto& a = d.animations[off ? 1 : 0];
            if (d.kind == DrawKind::Animate && a.playback == nox::things::AnimPlayback::Loop && !pose.enabled && !a.frames.empty()) {
                // The animation step's case 2: a loop turns only while the drawable is enabled (ObjFlags
                // 0x1000000); off, its first frame (a gear or pulley standing still).
                return &a.frames[0];
            }
            if (d.kind == DrawKind::Animate && a.playback == nox::things::AnimPlayback::LoopAndFade && !a.frames.empty()) {
                // The animation step's case 3: round and round from its start (as it fades: fadeAlpha).
                const double t = std::max(0.0, pose.time - pose.animStart) / (a.frameDelay + 1.0);
                return &a.frames[static_cast<std::size_t>(t) % a.frames.size()];
            }
            if (auto img = frameAt(a.frames, a.playback, a.frameDelay, pose)) {
                return img;
            }
        }
        break;
    case DrawKind::Vector:
        if (d.directional) {
            const auto& a = *d.directional;
            if (auto img = frameAt(a.directions[directionSlot(pose.direction)], a.playback, a.frameDelay, pose)) {
                return img;
            }
        }
        break;
    case DrawKind::Monster: {
        // The pose's state, else the first state the type has art for.
        const auto* state = pose.animState < d.monsterStates.size() && d.monsterStates[pose.animState]
                                ? &*d.monsterStates[pose.animState]
                                : nullptr;
        for (std::size_t i = 0; !state && i < d.monsterStates.size(); ++i) {
            if (d.monsterStates[i]) {
                state = &*d.monsterStates[i];
            }
        }
        if (state) {
            const auto& a = state->animation;
            if (auto img = frameAt(a.directions[directionSlot(pose.direction)], a.playback, a.frameDelay, pose)) {
                return img;
            }
        }
        break;
    }
    case DrawKind::AnimateState: {
        // AnimateStateDraw: the state by its xStatus: 2 the first (held at its first frame: its start
        // is now), 4 the second, 8 the third, none the first. One without frames draws nothing
        // (a trap door's Pit shut, then open).
        std::size_t slot = 0;
        Pose p = pose;
        if (pose.xStatus & 2u) {
            p.animStart = pose.time;
        } else if (pose.xStatus & 4u) {
            slot = 1;
        } else if (pose.xStatus & 8u) {
            slot = 2;
        }
        const auto& a = d.stateAnimations[slot];
        if (!a || a->frames.empty()) {
            return nullptr;
        }
        return frameAt(a->frames, a->playback, a->frameDelay, p);
    }
    case DrawKind::None:
    case DrawKind::Player:
        break;
    }
    return stillOf(art);
}

const nox::things::ImageRef* overImage(const TypeArt& art, const Pose& pose)
{
    // MonsterGeneratorDraw, after its image, while 0x400 and 0x800 are clear (and its image
    // was drawn): the fifth animation by net code and game frame, whatever its kind.
    const auto& d = art.draw;
    if (art.drawFn != DrawFn::MonsterGenerator || !nox::client::generatorOverlay(pose.xStatus) || d.animations.size() < 5) {
        return nullptr;
    }
    const auto& a = d.animations[4];
    return frameAt(a.frames, nox::things::AnimPlayback::Loop, a.frameDelay, pose);
}

const nox::things::ImageRef* mapObjectImage(const TypeArt& art, std::uint32_t extent, std::optional<std::uint8_t> doorAngle,
                                            std::optional<std::uint8_t> frame, bool enabled, std::uint32_t xStatus,
                                            double xStatusSince, double time)
{
    using nox::things::DrawKind;
    const auto& images = art.draw.images;
    if (doorAngle && !images.empty()) {
        return &images[*doorAngle % images.size()]; // DoorDraw: image[angle]
    }
    if (frame && art.drawFn == DrawFn::Slave && !images.empty()) {
        return &images[*frame % images.size()]; // image[AnimFrameSlave]
    }
    const DrawKind kind = art.draw.kind;
    if (kind == DrawKind::Animate || kind == DrawKind::Conditional || kind == DrawKind::AnimateState) {
        // (Conditional: Dun Mir's flame basins, lit or not. AnimateState: a pit shut or open.)
        Pose pose;
        pose.netCode = static_cast<std::uint16_t>(extent);
        pose.time = time;
        pose.animStart = xStatusSince;
        pose.enabled = enabled;
        pose.xStatus = xStatus;
        const nox::things::ImageRef* picked = pickImage(art, pose);
        if (!picked && kind == DrawKind::AnimateState) {
            return nullptr; // (a state without frames: nothing drawn)
        }
        return picked ? picked : stillOf(art);
    }
    return stillOf(art);
}

std::optional<std::uint8_t> fadeAlpha(const TypeArt& art, const Pose& pose)
{
    const auto& d = art.draw;
    if (d.kind != nox::things::DrawKind::Animate || d.animations.empty() || d.animations.front().playback != nox::things::AnimPlayback::LoopAndFade ||
        d.animations.front().frames.empty()) {
        return std::nullopt;
    }
    const auto& a = d.animations.front();
    const int frames = static_cast<int>(a.frames.size());
    const int t = static_cast<int>(std::max(0.0, pose.time - pose.animStart) / (a.frameDelay + 1.0));
    if (t >= 2 * frames) {
        return std::uint8_t{0}; // (the game deletes it then)
    }
    // From 200 down to 0 over twice the animation's length.
    return static_cast<std::uint8_t>(std::clamp(200 - t * 200 / (2 * frames), 0, 255));
}

const TypeArt* ThingArt::get(std::uint16_t typeId)
{
    if (typeId == 0 || typeId > things_.things.size()) {
        return nullptr;
    }
    if (byId_.size() <= typeId) {
        byId_.resize(things_.things.size() + 1u, nullptr);
    }
    const std::optional<TypeArt>*& art = byId_[typeId];
    if (!art) {
        art = &entry(things_.things[typeId - 1u].name);
    }
    return *art ? &**art : nullptr;
}

const TypeArt* ThingArt::get(std::string_view name)
{
    const std::optional<TypeArt>& art = entry(name);
    return art ? &*art : nullptr;
}

const std::optional<TypeArt>& ThingArt::entry(std::string_view name)
{
    auto it = types_.find(name);
    // First asked for: decoded from the thing's attributes and kept (a type that draws nothing too,
    // as nullopt).
    if (it == types_.end()) {
        std::optional<TypeArt> art;
        const auto* def = things_.findThing(name);
        if (def && def->draw) {
            try {
                TypeArt a;
                a.name = def->name;
                a.draw = nox::things::decodeDraw(*def->draw);
                // WeaponDraw / ArmorDraw (and their animated forms) set the item's colours first.
                const std::string_view drawType = def->draw->type;
                // FlagDraw colours the flag the same way: its team is a
                // modifier (MaterialTeamRed and so on).
                if (drawType == "WeaponDraw" || drawType == "WeaponAnimateDraw" || drawType == "FlagDraw") {
                    a.itemColours = ItemColours::Weapon;
                } else if (drawType == "ArmorDraw" || drawType == "ArmorAnimateDraw") {
                    a.itemColours = ItemColours::Armor;
                }
                a.still = a.draw.firstImage();
                // SIZE w h: the drawing box; the image is drawn from the position less half of it.
                if (const auto size = def->lastAttribute("SIZE")) {
                    const auto wh = readInts<2>(*size);
                    a.halfWidth = static_cast<float>(wh[0] / 2);
                    a.halfHeight = static_cast<float>(wh[1] / 2);
                }
                // Z: how high the image is drawn (taken off its y).
                if (const auto z = def->lastAttribute("Z")) {
                    a.z = static_cast<float>(readInts<1>(*z)[0]);
                }
                bool noCollide = false;
                bool allowOverlap = false;
                bool ownerVisible = false;
                if (const auto flags = def->attribute("FLAGS")) {
                    a.editorOnly = flags->find("EDIT_VISIBLE") != std::string_view::npos;
                    a.flicker = flags->find("FLICKER") != std::string_view::npos;
                    a.shadow = flags->find("SHADOW") != std::string_view::npos;
                    a.below = flags->find("BELOW") != std::string_view::npos;
                    bool airborne = false;
                    // FLAGS is a list joined by '+': these matched whole.
                    for (std::size_t at = 0; at <= flags->size();) {
                        const auto end = std::min(flags->find('+', at), flags->size());
                        const std::string_view flag = flags->substr(at, end - at);
                        airborne = airborne || flag == "AIRBORNE";
                        noCollide = noCollide || flag == "NO_COLLIDE";
                        allowOverlap = allowOverlap || flag == "ALLOW_OVERLAP";
                        ownerVisible = ownerVisible || flag == "OWNER_VISIBLE";
                        a.sightDestroy = a.sightDestroy || flag == "SIGHT_DESTROY";
                        at = end + 1;
                    }
                    a.hanging = airborne && noCollide && !a.editorOnly;
                }
                // EXTENT CIRCLE r, or BOX w h: the collision shape (a box's radius: half its larger side).
                if (const auto extent = def->lastAttribute("EXTENT")) {
                    const auto space = extent->find(' ');
                    const std::string_view shape = extent->substr(0, space);
                    if (space != std::string_view::npos && (shape == "CIRCLE" || shape == "BOX")) {
                        const auto size = readInts<2>(extent->substr(space + 1));
                        a.extentRadius = static_cast<float>(shape == "CIRCLE" ? size[0] : std::max(size[0], size[1]) / 2);
                        a.extentBox = shape == "BOX";
                        a.extentWidth = static_cast<float>(size[0]);
                        a.extentHeight = static_cast<float>(size[1]);
                    }
                }
                // ZSIZE min max: the height the thing spans (the cursor's hover test).
                if (const auto zsize = def->lastAttribute("ZSIZE")) {
                    const auto mm = readInts<2>(*zsize);
                    a.zSizeMin = static_cast<float>(mm[0]);
                    a.zSizeMax = static_cast<float>(mm[1]);
                }
                if (const auto li = def->lastAttribute("LIGHTINTENSITY")) {
                    a.lightIntensity = std::strtof(std::string(*li).c_str(), nullptr);
                }
                if (const auto lc = def->lastAttribute("LIGHTCOLOR")) {
                    const auto rgb = readInts<3>(*lc);
                    for (std::size_t i = 0; i < 3; ++i) {
                        a.lightColor[i] = static_cast<std::uint8_t>(std::clamp(rgb[i], 0, 255));
                    }
                }
                // LIGHTDIRECTION 0-359 sets the light's direction and makes it a cone (LightOmni 0);
                // LIGHTPENUMBRA 0-179 its half width; each as degrees / 360 * 65536 + 0.5 (the
                // thing.bin parsers; out of range: the line is refused).
                {
                    std::optional<lightcone::Cone> cone;
                    std::uint16_t width = 0;
                    if (const auto pen = def->lastAttribute("LIGHTPENUMBRA")) {
                        const int deg = readInts<1>(*pen)[0];
                        if (deg >= 0 && deg < 180) {
                            width = lightcone::angleFromDegrees(deg);
                        }
                    }
                    if (const auto dir = def->lastAttribute("LIGHTDIRECTION")) {
                        const int deg = readInts<1>(*dir)[0];
                        if (deg >= 0 && deg < 360) {
                            cone = lightcone::Cone{lightcone::angleFromDegrees(deg), width};
                        }
                    }
                    a.lightCone = cone;
                }
                const auto xfer = def->attribute("XFER");
                a.xfer = xfer ? std::string(*xfer) : "DefaultXfer";
                a.drawName = def->draw->type;
                if (const auto update = def->attribute("CLIENTUPDATE")) {
                    a.clientUpdate = std::string(*update);
                }
                a.drawFn = drawFnOf(a.drawName);
                a.updateFn = updateFnOf(a.clientUpdate);
                {
                    using enum DrawFn;
                    const DrawFn fn = a.drawFn;
                    a.stepsEffects = !a.clientUpdate.empty() || a.drawName.find("Magic") != std::string::npos ||
                                     fn == Pixie || fn == Arrow || fn == WeakArrow || fn == LevelUp || fn == OblivionUp ||
                                     fn == BlueRain || fn == SpiderSpit || fn == PlayerWaypoint;
                }
                a.subclasses = nox::things::thingSubclasses(*def);
                a.classes = nox::things::thingClasses(*def);
                if (def->attribute("WEIGHT")) {
                    a.classes |= nox::things::ObjectClass::Pickup; // the client's WEIGHT reader
                }
                a.visibleEnable = (a.classes & nox::things::ObjectClass::VisibleEnable) != 0;
                namespace oc = nox::things::ObjectClass;
                a.floorDecal = a.below && !a.editorOnly && (a.drawName == "StaticDraw" || a.drawName == "StaticRandomDraw") &&
                               !(a.classes & (oc::Pickup | oc::VisibleEnable)) &&
                               (allowOverlap || noCollide || (a.classes & oc::Immobile)) && !ownerVisible;
                a.fades = nox::client::drawFades(a.drawName);
                a.ghost = a.name == "Ghost";
                a.polyp = a.name == "Polyp";
                a.nameShadowOrBridge = a.name.find("Shadow") != std::string::npos || a.name.find("Bridge") != std::string::npos;
                a.nameObelisk = a.name.find("Obelisk") != std::string::npos;
                a.nameShield = a.name.find("Shield") != std::string::npos;
                // Kept if it draws anything, gives light (ColorLight has no image) or makes
                // effects (TeleportWake draws nothing itself; LevelUpDraw and the like draw
                // effects rather than an image).
                const bool effectDraw = a.draw.kind == nox::things::DrawKind::None && a.drawName != "NoDraw";
                if (a.still || a.draw.kind == nox::things::DrawKind::Player ||
                    (a.classes & nox::things::ObjectClass::Light) || !a.clientUpdate.empty() || effectDraw) {
                    art = std::move(a);
                }
            } catch (const std::exception& ex) {
                SDL_Log("%s: %s", def->name.c_str(), ex.what());
            }
        }
        it = types_.emplace(std::string(name), std::move(art)).first;
    }
    return it->second;
}

} // namespace noxsdl
