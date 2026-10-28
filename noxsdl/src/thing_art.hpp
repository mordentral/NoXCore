// How each thing type is drawn: its decoded DRAW payload and draw offset, looked up once per type.
//
// A drawable's image is drawn at (x - w/2, y - h/2 - Z - z) plus the image's own offset, where
// SIZE = w h and Z come from the THNG (SIZE stored / 2, Z as it is; copied into the drawable
// when it's made and applied as it's drawn), and z is the drawable's current height.
#pragma once

#include "light_cone.hpp"

#include "noxcore/things/draw.hpp"
#include "noxcore/things/thing_db.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noxsdl {

/// Items drawn with their weapon or armour definition's colours (WeaponDraw / ArmorDraw).
enum class ItemColours { None, Weapon, Armor };

/// The DRAW functions the client tells apart, named once when the type's art is made (from
/// TypeArt::drawName: DoorDraw is Door, NPCDraw NPC and so on); any other is Other.
enum class DrawFn : std::uint8_t {
    Other, Door, NPC, Maiden, Monster, Flag, Slave, PressurePlate, Harpoon,
    Magic, MagicMissile, Pixie, SpiderSpit, PlayerWaypoint, Arrow, WeakArrow, LevelUp, OblivionUp, BlueRain,
    Boulder, MonsterGenerator, Glyph,
};

/// The CLIENTUPDATE functions the effects run, named once like DrawFn (from TypeArt::clientUpdate:
/// FistUpdateDraw is Fist, ManaBombChargeClientUpdate ManaBombCharge and so on). None: it has no
/// CLIENTUPDATE; Other: one the effects don't run.
enum class UpdateFn : std::uint8_t {
    None, Other, Fist, Meteor, PitifulFireball, WeakFireball, Fireball, StrongFireball, TitanFireball,
    MagicMissile, Magic, SparkleTrail, DeathBall, DeathBallFragment, DeathBallCharge, TeleportWake,
    ManaBombCharge, Cloud, SmallCloud, VortexSource,
};

struct TypeArt {
    std::string name; ///< the thing's name
    ItemColours itemColours = ItemColours::None;
    float halfWidth = 0;
    float halfHeight = 0;
    float z = 0;
    nox::things::DrawSpec draw;
    std::optional<nox::things::ImageRef> still; ///< image for a static view (first frame, facing south)
    std::uint32_t classes = 0;   ///< CLASS bits (nox::things::ObjectClass); PICKUP if it has a WEIGHT
    std::uint32_t subclasses = 0;
    std::string xfer;            ///< XFER function name (DefaultXfer when absent)
    /// LIGHTINTENSITY (negative: an anti-light) and LIGHTCOLOR (white when unset, as the drawable's making leaves it).
    float lightIntensity = 0;
    std::array<std::uint8_t, 3> lightColor{255, 255, 255};
    /// LIGHTDIRECTION (degrees, 0-359; it makes the light a cone: LightOmni 0) and LIGHTPENUMBRA
    /// (the cone's half width, 0-179 degrees), as the drawable takes them from its type (the
    /// fireplaces, the gauntlet exits); nullopt: all round.
    std::optional<lightcone::Cone> lightCone;
    bool flicker = false;        ///< FLAGS has FLICKER
    bool editorOnly = false;     ///< FLAGS has EDIT_VISIBLE: markers such as PlayerStart
    bool visibleEnable = false;  ///< CLASS has VISIBLE_ENABLE: shown only while enabled
    /// The DRAW and CLIENTUPDATE function names: effects.hpp runs the ones that make effects.
    std::string drawName;
    std::string clientUpdate;
    DrawFn drawFn = DrawFn::Other;       ///< drawName, as the client tells them apart
    UpdateFn updateFn = UpdateFn::None;  ///< clientUpdate, likewise
    /// Effects::stepDrawables runs it each game frame: it has a CLIENTUPDATE, or one of the effect
    /// DRAW functions handled there (any with "Magic" in its name, PixieDraw, the arrows, LevelUpDraw,
    /// OblivionUpDraw, BlueRainDraw, SpiderSpitDraw, PlayerWaypointDraw).
    bool stepsEffects = false;
    bool shadow = false;     ///< FLAGS has SHADOW (0x10000): blocks line of sight
    /// FLAGS has SIGHT_DESTROY: deleted once a frame is drawn without it in sight (client/fade.hpp).
    bool sightDestroy = false;
    bool below = false;      ///< FLAGS has BELOW (1): lies on the floor, drawn under what stands on it
    /// FLAGS has AIRBORNE and NO_COLLIDE, not EDIT_VISIBLE: a wall hanging (shields,
    /// banners, torches), drawn after the back walls and before every sorted object.
    bool hanging = false;
    /// A BELOW thing the scene draws with the floor's decals (the scene's second list: StaticDraw
    /// or StaticRandomDraw, not PICKUP or VISIBLE_ENABLE, ALLOW_OVERLAP, NO_COLLIDE or IMMOBILE,
    /// not OWNER_VISIBLE): Fade Objects never fades it.
    bool floorDecal = false;
    /// Its DRAW goes through the generic drawable draw (client/fade.hpp's drawFades): lingering,
    /// it fades; anything else lingers at full alpha.
    bool fades = false;
    bool ghost = false;      ///< the Ghost: the generic draw gives it its own alpha (client/fade.hpp's ghostAlpha)
    bool polyp = false;      ///< the Polyp: hovered though it's none of the hover's classes (liveSprites)
    float extentRadius = 0; ///< EXTENT CIRCLE r, or half the larger side of a BOX
    bool extentBox = false;  ///< EXTENT is a BOX (extentWidth x extentHeight), else a CIRCLE
    float extentWidth = 0;
    float extentHeight = 0;
    float zSizeMin = 0;      ///< ZSIZE's bottom
    float zSizeMax = 0;      ///< ZSIZE's top: how tall it stands (enchantment effects sit by it)
    /// What its name says to the 3D view (view3d.hpp): "Shadow" or "Bridge" in it (laid in the
    /// floor), "Obelisk" (a box), "Shield" (held on the left).
    bool nameShadowOrBridge = false;
    bool nameObelisk = false;
    bool nameShield = false;
};

/// A door's leaf for each of its 32 angles (the exe's table): from the door's position to this
/// offset, 32 units long, stepping 11.25 degrees from north-west. DoorDraw draws image[angle].
SDL_Point doorLeaf(std::uint8_t angle);

/// Object flag 0x1000000: enabled (kept in map object headers).
inline constexpr std::uint32_t kObjectEnabled = 0x1000000;

/// What a live drawable is doing, for picking its image.
struct Pose {
    std::uint8_t direction = 7; ///< the client's AnimDir: 3 x 3 grid index (4, the centre, is unused)
    std::uint8_t animState = 0; ///< monster or player state
    /// Random-image pick (StaticRandomDraw etc.), slave frame; BoulderDraw's roll
    /// (nox::client::BoulderRoll::image).
    std::uint32_t variant = 0;
    std::uint16_t netCode = 0;  ///< staggers looping animations between objects
    double time = 0;            ///< game frames now
    double animStart = 0;       ///< game frame the current state began (one-shot animations)
    bool enabled = true;        ///< ConditionalAnimateDraw: on, its first animation; off, its second
    std::uint32_t xStatus = 0;  ///< AnimateStateDraw: which state's animation (MSG_REPORT_X_STATUS)
};

/// Frame index within an animation of `frames` frames, as the game picks it:
/// OneShot and OneShotRemove run once from the state's start and hold the last frame, Loop cycles
/// (offset by net code), Slave shows `variant`, Random picks per game frame, anything else
/// (LoopAndFade) the first frame. A frame lasts
/// frameDelay + 1 game frames.
std::size_t animationFrame(nox::things::AnimPlayback kind, std::size_t frames, std::uint8_t frameDelay, const Pose& pose);

/// An AnimateDraw thing's LoopAndFade (smoke, the toxic cloud's puffs): its frames loop
/// from its start while it's drawn with alpha 200 - 100 t / frames (t: animation frames since the
/// start), gone after two loops (0 then). The images are opaque; the alpha is what makes them
/// gas. nullopt for anything else (drawn as it is).
std::optional<std::uint8_t> fadeAlpha(const TypeArt& art, const Pose& pose);

/// Direction slot (GridDirection order) for an AnimDir.
std::size_t directionSlot(std::uint8_t direction);

/// The image to draw for a pose (one of `art`'s own); see draw.hpp for the layouts. Not for
/// PlayerDraw (player_art.hpp). Null: nothing to draw (an AnimateStateDraw state without frames),
/// or no art at all.
const nox::things::ImageRef* pickImage(const TypeArt& art, const Pose& pose);

/// What a DRAW function draws over the image pickImage gives, at the same spot, or null:
/// MonsterGeneratorDraw's fifth animation, looping, while its xStatus has neither 0x400 nor 0x800.
const nox::things::ImageRef* overImage(const TypeArt& art, const Pose& pose);

/// The image the scene draws a map object with (MapView::drawScene and the 3D view alike): its
/// door angle, slave frame or animation, else its still (which `art` must have); null for
/// nothing this frame (an AnimateStateDraw state without frames).
const nox::things::ImageRef* mapObjectImage(const TypeArt& art, std::uint32_t extent, std::optional<std::uint8_t> doorAngle,
                                            std::optional<std::uint8_t> frame, bool enabled, std::uint32_t xStatus,
                                            double xStatusSince, double time);

class ThingArt {
public:
    explicit ThingArt(const nox::things::ThingDb& things) : things_(things) {}

    /// Art for a thing type by name, or nullptr if the type is unknown or draws nothing.
    /// PlayerDraw types are returned without a still (their layered art isn't decoded yet).
    const TypeArt* get(std::string_view name);
    /// By network type id (THNG index + 1): the same as by its name, found by name only the first
    /// time (the drawing asks for every drawable's, several times a frame).
    const TypeArt* get(std::uint16_t typeId);

    /// A type the code names itself (a skull over a player, the potions in the HUD), looked up by
    /// that name once into `slot`, which its user keeps.
    struct Named {
        const TypeArt* art = nullptr;
        bool looked = false;
    };
    const TypeArt* get(Named& slot, std::string_view name)
    {
        if (!slot.looked) {
            slot.art = get(name);
            slot.looked = true;
        }
        return slot.art;
    }

private:
    /// The type's entry (decoded on its first call; never erased, so it stays where it is).
    const std::optional<TypeArt>& entry(std::string_view name);

    const nox::things::ThingDb& things_;
    std::map<std::string, std::optional<TypeArt>, std::less<>> types_;
    std::vector<const std::optional<TypeArt>*> byId_; ///< types_' entries by type id, as they're first asked for
};

} // namespace noxsdl
