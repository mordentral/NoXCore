// A game's world as the scene draws it (app.hpp): the drawables as scene sprites (who's seen,
// their poses, name plates and the things over them, what's under the mouse), the sight
// blockers, the lights, the looping sounds, the walls' state, and the viewer's effects demo.
#include "app.hpp"
#include "draw_scale.hpp"
#include "friend_dot.hpp"
#include "player_art.hpp"
#include "team_colors.hpp"

#include "noxcore/audio/sound_names.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>
#include <variant>

namespace noxsdl {

namespace {

/// The enchantments (a drawable's buffs bits) that decide who's seen.
constexpr std::uint32_t kEnchantInvisible = 1u << 0;
constexpr std::uint32_t kEnchantInfravision = 1u << 21; ///< shows the invisible

/// Our drawable has enchantment 21, which shows the invisible.
bool seesInvisible(const nox::client::HeadlessClient& client)
{
    const auto& drawables = client.world().drawables();
    const auto ours = drawables.find(client.netCode());
    return ours != drawables.end() && (ours->second.buffs & kEnchantInfravision);
}

/// `netCode` is a player on our team (one other than 0).
bool teammate(const nox::client::HeadlessClient& client, std::uint16_t netCode)
{
    const auto& players = client.world().players();
    const auto* self = ownPlayer(client);
    const auto them = players.find(netCode);
    return self && them != players.end() && self->team != 0 && self->team == them->second.team;
}

/// Whether PlayerDraw draws the things over a player (the name, the skull and crown):
/// not over an invisible player (enchantment 0) unless it's us, a teammate, or we're observing
/// or have enchantment 21 (which shows the invisible).
bool showsOverhead(const nox::client::HeadlessClient& client, const nox::client::PlayerEntry& player,
                   const nox::client::Drawable& d)
{
    if (!(d.buffs & kEnchantInvisible) || player.netCode == client.netCode()) {
        return true;
    }
    // (By the player entry's team, as given.)
    const auto* self = ownPlayer(client);
    const std::uint32_t ourTeam = self ? self->team : 0;
    if (ourTeam != 0 && player.team == ourTeam) {
        return true;
    }
    return observing(client) || seesInvisible(client);
}

/// How an invisible drawable (enchantment 0) is drawn; nullopt: as it is (not
/// invisible). Someone else's that's standing isn't drawn at all (alpha 0); moving, it's a ghost
/// at 0x80. Ours and a teammate's always show: standing, green at 0x80. With enchantment 21 we
/// see the invisible: green and solid. While we observe: 0x80.
///
/// Not quite the game's: it hides one that moved under 4 units each way since its last update
/// and fades the ghost by speed (x 16 up to 0x80), so a walker (3 to 4 units a frame, under 3
/// each way on a diagonal) and uneven updates flicker it in and out. Here any movement shows it,
/// steady at 0x80, held kWalkHold frames past its last move.
struct Ghost {
    std::uint8_t alpha = 0;
    bool green = false;
};
std::optional<Ghost> ghostLook(const nox::client::HeadlessClient& client, std::uint16_t netCode,
                               const nox::client::Drawable& d)
{
    if (!(d.buffs & kEnchantInvisible)) {
        return std::nullopt;
    }
    if (observing(client)) {
        return Ghost{0x80, false};
    }
    const bool friendly = netCode == client.netCode() || teammate(client, netCode);
    const bool infravision = seesInvisible(client);
    constexpr std::uint32_t kWalkHold = 6;
    const bool walking = d.movedFrame != 0 && client.frame() >= d.movedFrame && client.frame() - d.movedFrame <= kWalkHold;
    if (!walking && !infravision && !friendly) {
        return Ghost{0, false}; // still: unseen
    }
    if (infravision && !friendly) {
        return Ghost{255, true};
    }
    if (friendly && !walking) {
        return Ghost{0x80, true}; // ours, standing: green
    }
    return Ghost{0x80, false};
}

/// Whether a drawable's enchantment effects show: always unless it's invisible, then
/// only if it's us or a teammate, or we have enchantment 21 (which shows the invisible).
bool effectsShown(const nox::client::HeadlessClient& client, std::uint16_t netCode, const nox::client::Drawable& d)
{
    if (!(d.buffs & kEnchantInvisible) || netCode == client.netCode()) {
        return true;
    }
    return teammate(client, netCode) || seesInvisible(client);
}

/// Whether `netCode` has a chat bubble up at `frame`.
bool hasChatBubble(const nox::client::World& world, std::uint16_t netCode, std::uint32_t frame)
{
    const auto it = world.chatBubbles().find(netCode);
    return it != world.chatBubbles().end() && it->second.expires > frame;
}

/// The name plate PlayerDraw puts over a player, or nullopt if it draws none.
///
/// The name is grey, or the team's text colour when they're on a team (observing too). No name
/// in solo games (`solo`: soloGame, game flag 0x800), while the player has
/// a chat bubble up (`bubbleUp`), or when showsOverhead says no.
std::optional<NamePlate> namePlate(const nox::client::HeadlessClient& client, const nox::client::PlayerEntry& player,
                                   const nox::client::Drawable& d, float x, float y, bool bubbleUp, bool solo)
{
    const auto& world = client.world();
    if (player.name.empty() || solo || bubbleUp || !showsOverhead(client, player, d)) {
        return std::nullopt;
    }
    NamePlate plate{x, y, player.name, {155, 155, 155, 255}};
    // In the team's colour whenever they're on one (PlayerDraw, with the game's teams), whether
    // or not we are: an observer has no team and still sees the sides.
    const bool coloured = player.netCode == client.netCode() || player.team != 0;
    if (const auto color = teamColor(world, player.team); coloured && color) {
        plate.color = *color;
    }
    return plate;
}

/// MonsterDraw's and NPCDraw's last: the column beside a creature on our monitored list.
void pushCreatureBar(const nox::client::HeadlessClient& client, const TypeArt& art, std::uint16_t netCode, float x, float y,
                     float z, std::vector<MapView::SceneSprite>& out)
{
    const auto& monitored = client.world().self().monitored;
    const auto it = monitored.find(netCode);
    if (it == monitored.end()) {
        return;
    }
    const auto bar = creatureBar(art.extentRadius, art.zSizeMin, art.zSizeMax, it->second.health, it->second.maxHealth);
    if (!bar) {
        return;
    }
    // Sorted at the creature's feet; its callback draws it.
    out.push_back({y, x, y, nullptr, x, y});
    out.back().draw = [b = *bar, x, fy = y - z, poisoned = it->second.poison != 0](SDL_Renderer* r, const Camera& cam) {
        drawCreatureBar(r, cam, b, x, fy, poisoned);
    };
}

} // namespace

/// Live: shows the server's wall state (broken, opened, Wall-spell walls) when it changes.
void syncWalls(App& app)
{
    const auto& game = app.live->client().world().game();
    if (!app.view || app.wallsApplied == game.wallsVersion) {
        return;
    }
    // The Wall spell's walls, as map walls the view can draw.
    std::vector<std::pair<std::pair<int, int>, nox::map::Wall>> magic;
    for (const auto& [cell, m] : game.magicWalls) {
        nox::map::Wall w;
        w.material = m.tile;
        w.direction = m.direction;
        w.variant = m.variation;
        magic.push_back({{cell.first, cell.second}, w});
    }
    app.view->setWallState(game.destroyedWalls, game.openWalls, game.closedWalls, magic);
    app.wallsApplied = game.wallsVersion;
}

/// Live: the objects that block line of sight: SHADOW things that aren't invisible;
/// doors (not gates) along their leaf, boxes by their faces, the rest as their circle.
void sightBlockers(App& app, std::vector<SightMap::Blocker>& out)
{
    namespace ObjectClass = nox::things::ObjectClass;
    constexpr std::uint32_t kGate = 4; // the GATE subclass
    if (!app.live) {
        return;
    }
    for (const auto& [netCode, d] : app.live->client().world().drawables()) {
        const TypeArt* a = app.art->get(d.typeId);
        if (!a || !a->shadow || !d.enabled || (d.buffs & kEnchantInvisible)) {
            continue;
        }
        const auto p = app.interpolator.position(netCode, d);
        const std::uint32_t key = SightMap::liveKey(netCode);
        if (a->classes & ObjectClass::Door) {
            if (!(a->subclasses & kGate)) {
                const SDL_Point leaf = doorLeaf(d.special.value_or(0));
                out.push_back({p.x, p.y, p.x + static_cast<float>(leaf.x), p.y + static_cast<float>(leaf.y), 0, 0, 0, key});
            }
        } else if (a->extentBox) {
            out.push_back({p.x, p.y, p.x, p.y, 0, a->extentWidth, a->extentHeight, key});
        } else if (a->extentRadius > 0) {
            out.push_back({p.x, p.y, p.x, p.y, a->extentRadius, 0, 0, key});
        }
    }
}

namespace {

/// Something PlayerDraw draws with a drawable of its own at a fixed offset from the player: the
/// carried flag at (+15, -25) before the body, the skull and crown at (0, -50) after it. They
/// ignore the player's height. (wx, wy) is the icon's position; it sorts and is lit with the
/// player at (baseX, baseY).
void heldIcon(App& app, ThingArt::Named& slot, std::string_view type, std::uint16_t netCode, double time, float wx, float wy,
              float baseX, float baseY, const std::array<std::uint8_t, 4>* modifiers, std::vector<MapView::SceneSprite>& out)
{
    const TypeArt* art = app.art->get(slot, type);
    if (!art) {
        return;
    }
    Pose pose;
    pose.netCode = netCode;
    pose.time = time;
    const auto image = pickImage(*art, pose);
    if (!image) {
        return;
    }
    const auto colours = itemMaterials(*art, modifiers, app.modifiers);
    const Sprite* s = colours ? app.sprites->tinted(*image, *colours) : app.sprites->get(*image);
    if (s) {
        out.push_back({baseY, wx - art->halfWidth, wy - art->halfHeight - art->z, s, baseX, baseY});
    }
}

/// The icons PlayerDraw and NPCDraw draw over a player or NPC at (x, y), by its enchantments: the
/// skull (16), then the crown (30), at (0, -50) (heldIcon).
void overheadIcons(App& app, std::uint16_t netCode, std::uint32_t buffs, double time, float x, float y,
                   std::vector<MapView::SceneSprite>& out)
{
    constexpr std::uint32_t kEnchantDeath = 1u << 16;
    constexpr std::uint32_t kEnchantCrown = 1u << 30;
    if (buffs & kEnchantDeath) {
        heldIcon(app, app.skullArt, "SpinningSkull", netCode, time, x, y - 50, x, y, nullptr, out);
    }
    if (buffs & kEnchantCrown) {
        heldIcon(app, app.crownArt, "SpinningCrown", netCode, time, x, y - 50, x, y, nullptr, out);
    }
}

} // namespace

/// Live: the world's drawables as scene sprites.
/// `unsprited` gets the players drawn without an image, `bars` other players' health bars (as the
/// scene's sort draws them). Also finds what's under the mouse (the front pick: hovered,
/// hoveredUseable; the cursor's: cursorHovered, cursorUseable) and refreshes the effects' anchors.
std::vector<MapView::SceneSprite> liveSprites(App& app, std::vector<SDL_FPoint>& unsprited, std::vector<HealthBar>& bars)
{
    std::vector<MapView::SceneSprite> out;
    const auto& client = app.live->client();
    const double time = app.interpolator.gameTime();
    std::unordered_map<std::uint16_t, AnimTrack> tracks;
    std::unordered_map<std::uint16_t, App::NpcPick> picks;
    std::unordered_map<std::uint16_t, App::BoulderPick> rolls;
    app.anchors.clear();
    app.minionDots.clear();
    app.flagLabels.clear();
    app.damageShown.clear();
    app.modeLabelSpots.clear();
    // The drawables with damage numbers up (a few at most).
    std::vector<std::uint16_t> floaters;
    for (const auto& f : client.world().healthFloaters()) {
        floaters.push_back(f.netCode);
    }
    // What's under the mouse (clientOnCursorHover), found as the drawables are gone through.
    const SDL_FPoint mouse = mousePos(app);
    const float cursorX = app.camera.left() + mouse.x / app.camera.zoom;
    const float cursorY = app.camera.top() + mouse.y / app.camera.zoom;
    app.hovered.reset();
    app.hoveredUseable.reset();
    app.cursorHovered.reset();
    app.cursorUseable.reset();
    // The front pick and the cursor's (cursor_pick.hpp): a Glyph is the cursor's only for a
    // wizard, and then only while nothing else is under it.
    pick::Picks<std::variant<std::uint16_t, MapView::Useable>> hover;
    const TypeArt* glyphArt = app.art->get("Glyph");
    const auto* ownRecord = ownPlayer(client);
    const pick::CursorRole glyphRole =
        ownRecord && ownRecord->playerClass == 1 ? pick::CursorRole::LastResort : pick::CursorRole::None;
    // The creatures the summon cage points at (guiDrawSummonBox).
    const std::set<std::uint16_t> caged =
        app.summon && app.renderGui
            ? app.summon->highlighted(mouse.x, mouse.y, app.camera.viewWidth, app.camera.viewHeight, client.world())
            : std::set<std::uint16_t>{};
    const bool seeInvisible = seesInvisible(client);
    // Our own drawable, looked up once: GlyphDraw's alpha goes by how far a glyph is from it.
    const nox::client::Drawable* own = nullptr;
    if (const auto it = client.world().drawables().find(client.netCode()); it != client.world().drawables().end()) {
        own = &it->second;
    }
    // The hover tests each drawable with the drawable sight test (clientOnCursorHover), so nothing
    // out of sight, lingering ones included: last frame's region (this one's is built after the
    // drawables).
    const SightMap* sight =
        app.lineOfSight.value_or(true) && app.sight.polygon().size() >= 3 ? &app.sight : nullptr;
    // A drawable's body sprites carry its Fade Objects marks for the scene's sort (SceneSprite::fade)
    // and what its sight test takes (its type, key, height `z`, door angle, the last image drawn).
    const auto fadeTag = [&](MapView::SceneSprite& s, std::uint16_t nc, const nox::client::Drawable& d, const TypeArt& a,
                             bool keepAlpha, float z) {
        s.fade = &d.fade;
        s.fadeGeneral = !a.below && !a.hanging;
        s.fadeNever = a.floorDecal;
        s.fadeSelf = nc == client.netCode();
        s.fadeCreature = (a.classes & (nox::things::ObjectClass::Monster | nox::things::ObjectClass::Player)) != 0;
        s.fadeKeepAlpha = keepAlpha || !a.fades; // (only the generic drawable draw fades)
        s.sightArt = &a;
        s.sightKey = SightMap::liveKey(nc);
        s.sightZ = z;
        s.sightDoorAngle = d.special.value_or(0);
        s.drawnImage = &d.drawn;
    };
    // The enchantment effects pushed since `from`: the scene draw draws them round each drawable
    // its sort keeps (lingering too), before and after its DRAW, and the generic draw's fade is
    // over by then. So they carry the body's marks and its sight test (whichever comes first
    // decides for all), at their own alpha.
    const auto enchantFollow = [&](std::size_t from, std::uint16_t nc, const nox::client::Drawable& d, const TypeArt& a,
                                   float z) {
        for (std::size_t i = from; i < out.size(); ++i) {
            fadeTag(out[i], nc, d, a, true, z); // (their light point is the drawable's ground spot, as its body's)
        }
    };
    // What a drawable's DRAW (or the scene draw right after it) draws round it beyond its image: a
    // name plate, a health bar, a team's name, a friend dot, damage numbers, the effects of a DRAW
    // the effects draw. It's drawn exactly when the scene's sort draws the drawable (lingering
    // too), at full alpha (the generic draw's fade is off by then): an item with the body's marks
    // and sight test at its ground spot, whose `draw` runs in the scene's order. Those drawn over
    // the scene note themselves there and are drawn afterwards.
    // (`draw` goes straight into the item's function, made once there.)
    const auto overlay = [&](std::uint16_t nc, const nox::client::Drawable& d, const TypeArt& a, float x, float y, float z,
                             auto&& draw) {
        out.push_back({y, x, y, nullptr, x, y});
        out.back().draw = std::forward<decltype(draw)>(draw);
        fadeTag(out.back(), nc, d, a, true, z);
    };
    const auto enchantments = [&](const Anchor& anchor, std::uint16_t nc, const nox::client::Drawable& d, const TypeArt& a,
                                  bool front) {
        const std::size_t from = out.size();
        app.effects->enchantSprites(anchor, nc, front, *app.sprites, out);
        enchantFollow(from, nc, d, a, anchor.z);
    };
    const auto hoverable =[&](std::uint16_t nc, const nox::client::Drawable& d, const TypeArt& a) {
        namespace oc = nox::things::ObjectClass;
        if (nc == client.netCode() || ((d.buffs & kEnchantInvisible) && !seeInvisible)) {
            return false; // not us; not the invisible unless we see them
        }
        if ((a.classes & oc::Monster) && (a.subclasses & 0x4000u)) {
            return false; // not a NO_TARGET creature (the fish)
        }
        if (!(a.classes & (oc::Pickup | oc::Immobile | oc::Trigger | oc::Monster | oc::Player)) && !a.polyp) {
            return false;
        }
        if (a.classes & oc::Player) {
            const auto pl = client.world().players().find(nc);
            if (pl == client.world().players().end() || (pl->second.status & 1)) {
                return false; // observers
            }
        }
        if ((a.classes & oc::Immobile) && !(a.subclasses & 0x80)) {
            return false;
        }
        return !((a.classes & oc::Monster) && d.animState == 10); // the dead
    };
    // The drawable draw's colourings (SceneSprite::light): unlit, full bright; the invulnerable's
    // blue; the afraid's grey; the seen invisible's green.
    constexpr Rgb kUnlit{255, 255, 255}, kBlue{0, 0xA0, 0xFF}, kGrey{0x73, 0x73, 0x73}, kGreen{0, 255, 0};
    constexpr std::uint32_t kAfraid = 1u << 11, kInvulnerable = 1u << 23, kFrozen = 1u << 25;
    // A creature lit blue / white by turns invulnerable (not in solo games), grey afraid; else nothing.
    const auto statusLight = [&](std::uint32_t buffs, std::uint32_t frame) -> std::optional<Rgb> {
        if ((buffs & kInvulnerable) && !soloGame(app)) {
            return (frame & 1) ? kUnlit : kBlue;
        }
        if (buffs & kAfraid) {
            return kGrey;
        }
        return std::nullopt;
    };
    // Its damage numbers (with one up: floaters), after its DRAW.
    const auto floating = [&](std::uint16_t nc) { return std::ranges::find(floaters, nc) != floaters.end(); };
    const auto damageNumbers = [&](std::uint16_t nc, const nox::client::Drawable& d, const TypeArt& a, const auto& p) {
        overlay(nc, d, a, p.x, p.y, p.z, [&app, nc](SDL_Renderer*, const Camera&) { app.damageShown.push_back(nc); });
    };
    // Each drawable: skip what isn't drawn, follow its animation state, test it against the mouse, then draw it
    // by kind: an NPC, a player, or anything else.
    for (const auto& [netCode, d] : client.world().drawables()) {
        const TypeArt* a = app.art->get(d.typeId);
        // EDIT_VISIBLE (a Mover, a GameBallStart) is on no draw list (the
        // lists all want flag 0x1000 clear): only the editor shows it.
        if (!a || !d.enabled || a->editorOnly) {
            continue;
        }
        // A player or creature the server hasn't updated for over 5 frames isn't drawn:
        // it went out of our view without a word, and what we have is only where it was. Not us,
        // and not a creature dying or dead (states 9, 10), which lie where they fell.
        if ((a->classes & (nox::things::ObjectClass::Monster | nox::things::ObjectClass::Player)) &&
            client.frame() > d.lastSeenFrame + 5) {
            const bool keep = (a->classes & nox::things::ObjectClass::Monster)
                                  ? d.animState == 9 || d.animState == 10
                                  : netCode == client.netCode();
            if (!keep) {
                continue;
            }
        }
        // One-shot animations run from when the state began, which we see as a state change.
        AnimTrack track{d.animState, time};
        if (const auto it = app.animTracks.find(netCode); it != app.animTracks.end()) {
            const AnimTrack& was = it->second;
            // A player landing from a jump (JUMP 3, RUNNING_JUMP 7) goes to WALK (4) for at least
            // a frame, IDLE (0) if that frame had no move, before its next move makes it RUN (6):
            // the server's state machine lands in walk (Plr_UpdateState). Not shown: the jump's
            // last frame is held up to kLandingHold game frames, so a runner lands straight into
            // the run, and anyone else walks or stands that much later.
            constexpr double kLandingHold = 3;
            const bool landing = a->draw.kind == nox::things::DrawKind::Player && (was.state == 3 || was.state == 7) &&
                                 (d.animState == 0 || d.animState == 4);
            if (was.state == d.animState) {
                track = was;
            } else if (landing && (was.landed < 0 || time - was.landed < kLandingHold)) {
                track = was;
                track.landed = was.landed < 0 ? time : was.landed;
            }
        }
        tracks[netCode] = track;

        Pose pose;
        pose.direction = d.direction;
        pose.animState = track.state;
        pose.variant = d.frame.value_or(static_cast<std::uint8_t>(netCode));
        if (a->drawFn == DrawFn::Door) {
            pose.variant = d.special.value_or(0); // image[angle], from MSG_DOOR_ANGLE
        } else if (a->drawFn == DrawFn::Boulder) {
            // BoulderDraw: its roll, stepped by where it is now each game frame it's drawn (on screen:
            // its box in the view) rather than any frame of the server's.
            auto& pick = rolls[netCode];
            if (const auto it = app.boulderRolls.find(netCode); it != app.boulderRolls.end()) {
                pick = it->second;
            }
            const auto frame = static_cast<std::uint32_t>(std::max(time, 0.0));
            const auto px = static_cast<float>(d.x);
            const auto py = static_cast<float>(d.y - d.z) - a->z;
            const bool onScreen = px + a->halfWidth >= app.camera.left() && px - a->halfWidth <= app.camera.right() &&
                                  py + a->halfHeight >= app.camera.top() && py - a->halfHeight <= app.camera.bottom();
            if (pick.frame != frame && onScreen) {
                pick.frame = frame;
                nox::client::boulderDraw(pick.roll, d.x, d.y);
            }
            pose.variant = pick.roll.image();
        }
        pose.netCode = netCode;
        pose.time = time;
        pose.animStart = track.start;
        pose.enabled = d.enabled;
        pose.xStatus = d.xStatus;
        auto p = app.interpolator.position(netCode, d);
        // A script mode's draw scale on it (App::modeDrawScales: nox.setDrawScale), for its body's
        // images; looked up only while a mode has given any.
        float drawScale = 1.0f;
        if (!app.modeDrawScales.empty()) {
            const auto ds = std::ranges::lower_bound(app.modeDrawScales, netCode, {}, &App::ModeDrawScale::object);
            if (ds != app.modeDrawScales.end() && ds->object == netCode && ds->type == d.typeId) {
                drawScale = ds->scale;
            }
        }
        if (const auto z = app.effects->drawableZ(netCode, d.typeId)) {
            p.z = *z; // a falling fist or meteor: its client update sets the height
        }
        pose.direction = app.view3d.seenDirection(pose.direction, p.x, p.y); // (the 3D view: as its eye sees it)
        const float x = p.x - a->halfWidth;
        const float y = p.y - a->halfHeight - a->z - p.z;
        // Its draw scale on each of its body's images, all about one pivot (draw_scale.hpp): a
        // missile's first image's middle, anything else's ground point, so its layers stay
        // together and what stands stays on the floor.
        std::optional<drawscale::Point> scalePivot;
        const auto scaled = [&](MapView::SceneSprite& e) {
            if (drawScale == 1.0f || !e.sprite) {
                return;
            }
            if (!scalePivot) {
                scalePivot = drawscale::pivotOf((a->classes & nox::things::ObjectClass::Missile) != 0, {p.x, p.y - p.z},
                                                {e.x + e.sprite->offsetX, e.y + e.sprite->offsetY, static_cast<float>(e.sprite->width),
                                                 static_cast<float>(e.sprite->height)});
            }
            e.scale = drawScale;
            e.pivotX = scalePivot->x;
            e.pivotY = scalePivot->y;
        };
        // In the 3D view's mouse look, only the thing its crosshair is on (the cursor is at its
        // foot, as it was a frame ago: the nearest within a creature's step of it): not whatever
        // else covers that spot in 2D, and nothing while it aims at the ground.
        const bool look3d = app.view3d.looking();
        const float fromFoot = std::hypot(p.x - cursorX, p.y - cursorY);
        const bool under = look3d ? app.view3d.onThing() && fromFoot < 10
                                  : underCursor(*a, p.x, p.y, p.z, cursorX, cursorY);
        // (Drawn lingering is no matter: the hover makes the sight test itself.)
        if (hoverable(netCode, d, *a) && under &&
            (!sight || sight->drawableSeen(*a, SightMap::liveKey(netCode), p.x, p.y, p.z, d.special.value_or(0), d.drawn))) {
            // The one furthest forward wins (in the mouse look, the one nearest its foot).
            const float depth = look3d ? 10 - fromFoot : hoverDepth(*a, p.y, p.z);
            hover.offer(netCode, depth, a == glyphArt ? glyphRole : pick::CursorRole::Any);
        }

        if (a->drawFn == DrawFn::NPC) {
            // NPCDraw: an NPC drawn as a player from its record (MSG_REPORT_NPC and
            // its worn things' reports) with the local player's drawable's art, NewPlayer's;
            // without a record, nothing (player_art.hpp).
            const nox::client::NpcEntry* npc = client.world().npc(netCode);
            const TypeArt* body = app.art->get(app.newPlayerArt, "NewPlayer");
            const auto frame = static_cast<std::uint32_t>(time);
            std::vector<PlayerLayer> layers;
            std::vector<SpriteCache::Materials> materials;
            if (npc && body) {
                // Its player state, worked out once a game frame as the game draws it.
                auto& pick = picks[netCode];
                if (const auto it = app.npcPicks.find(netCode); it != app.npcPicks.end()) {
                    pick = it->second;
                }
                if (pick.frame != frame) {
                    pick.frame = frame;
                    pick.state = nox::client::npcPlayerState(
                        d.animState, npc->weaponEquip, npc->armorEquip, d.frame.value_or(0), pick.choice,
                        [&app](int low, int high) { return nox::util::uniform_int_distribution<int>(low, high)(app.rng); });
                }
                Pose npcPose = pose;
                npcPose.animState = pick.state;
                PlayerLook look{npc->armorEquip, npc->weaponEquip, d.buffs};
                look.drawableState = d.animState;
                layers = playerLayers(body->draw, npcPose, look);
                materials = layerMaterials(layers, npcWearer(*npc), app.modifiers, d.buffs, frame);
            }
            Anchor anchor{p.x, p.y, x, y, std::nullopt, d.typeId, p.z, d.direction, d.buffs, d.animState, false,
                          effectsShown(client, netCode, d)};
            if (!layers.empty()) {
                anchor.image = app.sprites->indexOf(layers.front().image);
            }
            app.anchors[netCode] = anchor;
            enchantments(anchor, netCode, d, *a, false);
            // Each layer through the drawable draw as a creature's image: one blue while the summon cage
            // points at it or frozen; lit blue / white by turns invulnerable (not in solo games),
            // grey afraid; an invisible one a ghost or unseen (as below).
            const bool blue = caged.count(netCode) || (d.buffs & kFrozen);
            const std::optional<Rgb> light = blue ? std::optional<Rgb>{kUnlit} : statusLight(d.buffs, frame);
            const auto ghost = ghostLook(client, netCode, d);
            if (!(ghost && ghost->alpha == 0)) {
                float clipBottom = 1e9f;
                for (std::size_t i = 0; i < layers.size(); ++i) {
                    const Sprite* s = blue ? app.sprites->mono(layers[i].image, {0, 0xA0, 0xFF, 255})
                                           : app.sprites->tinted(layers[i].image, materials[i]);
                    if (!s) {
                        continue;
                    }
                    if (p.z < 0 && clipBottom == 1e9f) {
                        clipBottom = y + s->offsetY + s->height + p.z; // sunk into the floor (client_drawImg_bbb)
                    }
                    out.push_back({p.y, x, y, s, p.x, p.y});
                    out.back().clipBottom = clipBottom;
                    scaled(out.back());
                    out.back().light = light;
                    fadeTag(out.back(), netCode, d, *a, ghost.has_value(), p.z);
                    out.back().notesImage = a->fades;
                    if (ghost) {
                        out.back().alpha = ghost->alpha;
                        if (ghost->green) {
                            out.back().light = kGreen;
                        }
                    }
                }
            }
            // Over it, as over a player but always (drawn apart from the drawable draw, so even unseen):
            // the skull (enchantment 16) and crown (30), and the column of a creature we watch;
            // NPCDraw draws them, so they go with its sort.
            if (!layers.empty()) {
                const std::size_t from = out.size();
                overheadIcons(app, netCode, d.buffs, time, p.x, p.y, out);
                pushCreatureBar(client, *a, netCode, p.x, p.y, p.z, out);
                enchantFollow(from, netCode, d, *a, p.z);
            }
            if (floating(netCode)) {
                damageNumbers(netCode, d, *a, p);
            }
            enchantments(anchor, netCode, d, *a, true);
            continue;
        }

        if (a->draw.kind == nox::things::DrawKind::Player) {
            PlayerLook look{d.armorEquip, d.weaponEquip, d.buffs};
            const nox::client::PlayerEntry* player = nullptr;
            std::optional<NamePlate> plateHere;
            const auto frame = static_cast<std::uint32_t>(time);
            const auto pl = client.world().players().find(netCode);
            if (pl == client.world().players().end()) {
                // No player for it (a dedicated server's host): PlayerDraw draws nothing.
                continue;
            }
            {
                if ((pl->second.status & 1) && netCode != client.netCode()) {
                    continue; // other players' observers aren't drawn
                }
                player = &pl->second;
                look.armorEquip = player->armorEquip;
                look.weaponEquip = player->weaponEquip;
                // (a muted player's line makes no bubble: playerCanTalkMB)
                const bool bubbleUp = hasChatBubble(client.world(), netCode, frame) && !app.muted.contains(netCode);
                plateHere = namePlate(client, *player, d, p.x, p.y, bubbleUp, soloGame(app));
                // (Who's editing) An editor of the map editor session we're in: the pencil with
                // their name (shown where the name would be, so not over the invisible).
                if (app.editSession && app.editSession->inSession() && !bubbleUp && showsOverhead(client, *player, d)) {
                    if (const auto* ed = app.editSession->editorByNetCode(netCode)) {
                        if (!plateHere) {
                            plateHere = NamePlate{p.x, p.y, {}, {}};
                        }
                        plateHere->pencil = NamePlate::Pencil{peerRgb(ed->peer), ed->activity == nox::mapedit::session::Activity::Idle};
                    }
                }
                // Others' health in quest games, while the bars beside us are up (PlayerDraw).
                // (noxcpp: and a solo game's others, the co-op campaign's party, whose server tells
                // their health as a quest's does; a solo game alone has no other player.)
                constexpr std::uint32_t kQuestGame = 0x1000;
                constexpr std::uint32_t kSoloGame = 0x800;
                if (netCode != client.netCode() && (client.world().game().gameFlags & (kQuestGame | kSoloGame)) && app.hud &&
                    app.hud->barsShown() && showsOverhead(client, *player, d)) {
                    const HealthBar bar{p.x, p.y, p.z, std::round(2 * a->extentRadius), player->healthPct,
                                        (player->status & 0x400) != 0};  // status 0x400: poisoned
                    overlay(netCode, d, *a, p.x, p.y, p.z, [&bars, bar](SDL_Renderer*, const Camera&) { bars.push_back(bar); });
                }
            }
            const auto layers = playerLayers(a->draw, pose, look);
            const auto materials = layerMaterials(layers, player, app.modifiers, d.buffs, frame);
            Anchor anchor{p.x, p.y, x, y, std::nullopt, d.typeId, p.z, d.direction, d.buffs, d.animState, true,
                          effectsShown(client, netCode, d)};
            if (!layers.empty()) {
                anchor.image = app.sprites->indexOf(layers.front().image);
            }
            app.anchors[netCode] = anchor;
            // Equal sort keys keep this order: the enchantments behind, the carried flag, the
            // layers, the icons, the enchantments in front.
            enchantments(anchor, netCode, d, *a, false);
            if (look.weaponEquip & 1) {  // weapon equip bit 0: carrying a flag
                const std::array<std::uint8_t, 4>* flagMods = nullptr;
                if (player) {
                    if (const auto it = player->weaponModifiers.find(1); it != player->weaponModifiers.end()) {
                        flagMods = &it->second;
                    }
                }
                // (PlayerDraw draws it: it goes with the player's sort.)
                const std::size_t from = out.size();
                heldIcon(app, app.flagArt, "Flag", netCode, time, p.x + 15, p.y - 25, p.x, p.y, flagMods, out);
                enchantFollow(from, netCode, d, *a, p.z);
            }
            if (!layers.empty() && floating(netCode)) {
                // Its damage numbers, after its DRAW (an invisible one's too: PlayerDraw goes on
                // when the drawable draw draws nothing; without an image it draws nothing at all).
                damageNumbers(netCode, d, *a, p);
            }
            // While we observe, the drawable draw draws our own body at half alpha (0x80); the invisible
            // are ghosts or unseen (ghostLook).
            const auto ghost = ghostLook(client, netCode, d);
            if (ghost && ghost->alpha == 0) {
                continue;
            }
            std::uint8_t bodyAlpha = player && (player->status & 1) ? 0x80 : 255;
            if (ghost) {
                bodyAlpha = std::min(bodyAlpha, ghost->alpha);
            }
            // Below the floor (riding an elevator down): every layer cut at the line the first one's
            // bottom makes, its depth up from where it's drawn (client_drawImg_bbb).
            float clipBottom = 1e9f;
            for (std::size_t i = 0; i < layers.size(); ++i) {
                if (const Sprite* s = app.sprites->tinted(layers[i].image, materials[i])) {
                    if (p.z < 0 && clipBottom == 1e9f) {
                        clipBottom = y + s->offsetY + s->height + p.z;
                    }
                    out.push_back({p.y, x, y, s, p.x, p.y, bodyAlpha});
                    out.back().clipBottom = clipBottom;
                    scaled(out.back());
                    fadeTag(out.back(), netCode, d, *a, ghost.has_value() || bodyAlpha != 255, p.z);
                    out.back().notesImage = a->fades;
                    if (ghost && ghost->green) {
                        out.back().light = kGreen;
                    }
                }
            }
            if (layers.empty()) {
                unsprited.push_back({p.x, p.y - p.z});
            }
            if (player && showsOverhead(client, *player, d)) {
                const std::size_t from = out.size();
                overheadIcons(app, netCode, d.buffs, time, p.x, p.y, out);
                enchantFollow(from, netCode, d, *a, p.z);
            }
            // Its name, the last of PlayerDraw's: in the scene's order, so what's drawn
            // after it (a tree in front, a wall) covers it as in the original.
            if (plateHere && ((app.font && app.namePlates) || plateHere->pencil)) {
                overlay(netCode, d, *a, p.x, p.y, p.z,
                        [&app, plate = std::move(*plateHere)](SDL_Renderer*, const Camera&) { drawNamePlate(app, plate); });
            }
            enchantments(anchor, netCode, d, *a, true);
            continue;
        }

        // Anything else: one image for this pose from its type's draw.
        const auto image = pickImage(*a, pose);
        Anchor anchor{p.x, p.y, x, y, std::nullopt, d.typeId, p.z, d.direction, d.buffs, d.animState, false,
                      effectsShown(client, netCode, d)};
        if (image) {
            anchor.image = app.sprites->indexOf(*image);
        }
        anchor.lingering = d.fade.lingering; // (as the last sort left it)
        app.anchors[netCode] = anchor;
        enchantments(anchor, netCode, d, *a, false);
        auto colours = itemMaterials(*a, d.modifiers ? &*d.modifiers : nullptr, app.modifiers);
        bool drawn = image != nullptr;
        if (a->drawFn == DrawFn::Maiden) {
            // MaidenDraw: MonsterDraw with materials 1-6 the NPC record's colours; no
            // record, nothing.
            if (const nox::client::NpcEntry* npc = client.world().npc(netCode)) {
                colours = maidenMaterials(*npc);
            } else {
                drawn = false;
            }
        }
        // GlyphDraw: a trap glyph by how far it is from us, faint at the edge of 150 units and not
        // drawn past them; solid under INFRAVISION (client/state_draw.hpp).
        std::optional<std::uint8_t> glyphOwn;
        if (a->drawFn == DrawFn::Glyph) {
            glyphOwn = nox::client::glyphAlpha({.ownDrawable = own != nullptr,
                                                .infravision = seeInvisible,
                                                .dx = own ? d.x - own->x : 0,
                                                .dy = own ? d.y - own->y : 0});
            if (!glyphOwn) {
                drawn = false;
            }
        }
        const Sprite* s = !drawn ? nullptr : colours ? app.sprites->tinted(*image, *colours) : app.sprites->get(*image);
        // The drawable draw's colouring: a creature the summon cage points at (flag 0x40000000) and
        // anything frozen in one colour, blue and unlit; an invulnerable creature lit blue /
        // white by turns (not in solo games), an afraid one grey.
        std::optional<Rgb> light;
        if (a->classes & nox::things::ObjectClass::Light) {
            light = kUnlit; // a light source, on: unlit, full bright
        }
        const bool creature = (a->classes & nox::things::ObjectClass::Monster) != 0;
        const bool player = (a->classes & nox::things::ObjectClass::Player) != 0;
        if (drawn && ((creature && caged.count(netCode)) || (d.buffs & kFrozen))) {
            if (const Sprite* m = app.sprites->mono(*image, {0, 0xA0, 0xFF, 255})) {
                s = m;
                light = kUnlit;
            }
        } else if (!player) {
            if (const auto status = statusLight(d.buffs, static_cast<std::uint32_t>(time))) {
                light = status;
            }
        }
        auto ghost = ghostLook(client, netCode, d); // an invisible creature
        // The Ghost (the thing): the generic drawable draw gives it its own alpha by its distance
        // from the view's centre, the smaller of that and the fade while it lingers; seeing the
        // invisible, it's drawn as the invisible are then, green and solid.
        std::optional<std::uint8_t> ghostOwn;
        if (!ghost && a->ghost) {
            if (seeInvisible) {
                ghost = Ghost{255, true};
            } else {
                ghostOwn = nox::client::ghostAlpha(static_cast<std::int32_t>(std::floor(p.x)) - static_cast<std::int32_t>(std::floor(app.camera.x)),
                                                   static_cast<std::int32_t>(std::floor(p.y)) - static_cast<std::int32_t>(std::floor(app.camera.y)));
            }
        }
        if (s && !(ghost && ghost->alpha == 0)) {
            // BELOW things (scorch marks, blood, plates) lie under what stands on the floor; the
            // floor's decals under the other BELOW things.
            out.push_back({a->floorDecal ? p.y - MapView::kFloorDecal : a->below ? p.y - MapView::kBelow : p.y, x, y, s, p.x, p.y});
            scaled(out.back()); // (and the image drawn over it, below: a copy of this one, its pivot too)
            out.back().bloom = (a->classes & nox::things::ObjectClass::Light) != 0; // light sources bloom
            out.back().light = light;
            fadeTag(out.back(), netCode, d, *a, ghost.has_value(), p.z);
            out.back().notesImage = a->fades;
            if (const auto alpha = fadeAlpha(*a, pose)) {
                out.back().alpha = *alpha; // LoopAndFade smoke
            }
            if (ghostOwn) {
                out.back().alpha = *ghostOwn;
                out.back().fadeMin = true;
            }
            if (glyphOwn) {
                out.back().alpha = *glyphOwn; // (lingering, the fade's alpha instead: the generic draw's)
            }
            if (ghost) {
                out.back().alpha = ghost->alpha;
                if (ghost->green) {
                    out.back().light = kGreen;
                }
            }
            if (p.z < 0) {
                out.back().clipBottom = y + s->offsetY + s->height + p.z; // sunk into the floor (client_drawImg_bbb)
            }
            // A mode UI's label on it (App::modeLabels), drawn when it is: its top noted.
            if (!app.modeLabels.empty()) {
                const auto l = std::ranges::lower_bound(app.modeLabels, netCode, {}, &std::pair<std::uint16_t, std::uint16_t>::first);
                if (l != app.modeLabels.end() && l->first == netCode) {
                    App::ModeLabelSpot spot{l->second, p.x, p.y, p.y - p.z - a->zSizeMax};
                    if (scalePivot) {
                        // (Drawn at a scale: the label goes up as far as its image's top does.)
                        spot.top -= drawscale::rise(drawScale, *scalePivot, y + s->offsetY);
                    }
                    overlay(netCode, d, *a, p.x, p.y, p.z, [&app, spot](SDL_Renderer*, const Camera&) { app.modeLabelSpots.push_back(spot); });
                }
            }
            // What its DRAW draws over that image through the same generic draw (MonsterGeneratorDraw's
            // fifth animation): the same marks, coloured alike.
            if (const auto over = overImage(*a, pose)) {
                const bool mono = drawn && ((creature && caged.count(netCode)) || (d.buffs & kFrozen));
                const Sprite* s2 = mono       ? app.sprites->mono(*over, {0, 0xA0, 0xFF, 255})
                                   : colours ? app.sprites->tinted(*over, *colours)
                                             : app.sprites->get(*over);
                if (s2) {
                    MapView::SceneSprite second = out.back();
                    second.sprite = s2;
                    if (p.z < 0) {
                        second.clipBottom = y + s2->offsetY + s2->height + p.z;
                    }
                    out.push_back(second);
                }
            }
        }
        // MonsterDraw's (and so MaidenDraw's) last, once its state has frames, even for a far
        // ghost or an invisible creature it didn't draw: the column beside a creature we watch,
        // and the dot over a creature on our friends list (our summons), of radius 3 in our team's
        // colour, twice its height over its feet, when we're on a team (friend_dot.hpp: not a
        // quest's shopkeepers; in a campaign game only the co-op party's creatures).
        if (drawn && (a->drawFn == DrawFn::Monster || a->drawFn == DrawFn::Maiden)) {
            const std::size_t from = out.size();
            pushCreatureBar(client, *a, netCode, p.x, p.y, p.z, out);
            enchantFollow(from, netCode, d, *a, p.z);
            const auto* me = ownPlayer(client);
            if (const std::uint32_t team = friendDotTeam(client.world(), netCode, a->classes, a->subclasses, me ? me->team : 0,
                                                         app.partyDots, app.partyCreatures)) {
                if (const auto c = teamColor(client.world(), team)) {
                    const MinionDot dot{p.x, p.y - p.z - 2 * a->zSizeMax, *c};
                    overlay(netCode, d, *a, p.x, p.y, p.z, [&app, dot](SDL_Renderer*, const Camera&) { app.minionDots.push_back(dot); });
                }
            }
        }
        // An obelisk the sort draws goes on the minimap (the scene draw, after its DRAW).
        if ((a->classes & nox::things::ObjectClass::Immobile) && (a->subclasses & 0x8u) && app.minimap) {  // SUBCLASS VISIBLE_OBELISK
            overlay(netCode, d, *a, p.x, p.y, p.z, [&app, netCode](SDL_Renderer*, const Camera&) { app.minimap->markDrawn(netCode); });
        }
        // FlagDraw's team name over the flag (drawFlagLabels).
        if (a->drawFn == DrawFn::Flag && app.font && app.namePlates && (client.world().game().gameFlags & 0x80)) {
            overlay(netCode, d, *a, p.x, p.y, p.z, [&app, netCode](SDL_Renderer*, const Camera&) { app.flagLabels.push_back(netCode); });
        }
        // The DRAW functions the effects draw (magic and missiles' glows, pixies, spider spit,
        // waypoints: Effects::drawDrawables).
        if (a->drawFn == DrawFn::Magic || a->drawFn == DrawFn::MagicMissile || a->drawFn == DrawFn::Pixie ||
            a->drawFn == DrawFn::SpiderSpit || a->drawFn == DrawFn::PlayerWaypoint) {
            overlay(netCode, d, *a, p.x, p.y, p.z, [&app, netCode](SDL_Renderer*, const Camera&) { app.effects->showDrawable(netCode); });
        }
        // Creatures' and generators' damage numbers, after their DRAW.
        if ((a->classes & (nox::things::ObjectClass::Monster | nox::things::ObjectClass::Player |
                           nox::things::ObjectClass::MonsterGenerator)) &&
            floating(netCode)) {
            damageNumbers(netCode, d, *a, p);
        }
        enchantments(anchor, netCode, d, *a, true);
    }
    app.animTracks = std::move(tracks);
    app.npcPicks = std::move(picks);
    app.boulderRolls = std::move(rolls);
    // The map's useable objects (levers, switches, buttons) are under the mouse too; the one
    // furthest forward wins against the drawables (clientOnCursorHover goes through both alike).
    if (app.view) {
        // (In the 3D view's mouse look: the one the crosshair is on, at its foot, before a drawable.)
        const bool look3d = app.view3d.looking();
        if (const auto u = app.view->useableAt(cursorX, cursorY, sight)) {
            if (!look3d) {
                hover.offer(*u, u->depth, pick::CursorRole::Any);
            } else if (app.view3d.onThing() && std::hypot(u->x - cursorX, u->y - cursorY) < 4) {
                hover.offer(*u, 1e9f, pick::CursorRole::Any);
            }
        }
    }
    const auto keep = [](const auto& thing, std::optional<std::uint16_t>& drawable, std::optional<MapView::Useable>& useable) {
        if (!thing) {
            return;
        }
        if (const auto* nc = std::get_if<std::uint16_t>(&*thing)) {
            drawable = *nc;
        } else {
            useable = std::get<MapView::Useable>(*thing);
        }
    };
    keep(hover.front, app.hovered, app.hoveredUseable);
    keep(hover.cursor, app.cursorHovered, app.cursorUseable);
    return out;
}

/// --fx-demo: every two seconds, the FX messages a server would send, laid out around the view
/// centre, for looking at the effects without a game. A row of bears below stand in for the
/// drawables the particle effects (MSG_FX_PARTICLEFX 0-5) attach to.
void fxDemo(App& app, std::uint32_t frame, std::vector<MapView::SceneSprite>& extra)
{
    constexpr std::uint16_t kFirstBear = 0x7FF0;
    constexpr int kBears = 6;
    const TypeArt* bear = app.art->get("Bear");
    const auto bearAt = [&](int i) {
        return SDL_FPoint{std::floor(app.camera.x) + static_cast<float>((i - 2) * 100 - 50),
                          std::floor(app.camera.y) + 280};
    };
    const auto show = [&](std::string_view group) { return app.fxDemoOnly.empty() || app.fxDemoOnly == group; };
    const bool enchant = !app.fxDemoOnly.empty() && show("enchant");
    const bool bears = show("particles") || show("beams") || enchant;
    // "enchant": each bear shows some enchantments: the protections; haste and running; the
    // shield; the reflective shield and anchoring; confusion and vampirism; slow, infravision
    // and shock.
    constexpr std::array<std::uint32_t, kBears> kBearBuffs{
        (1u << 17) | (1u << 18) | (1u << 20), (1u << 8) | (1u << 9), 1u << 26, (1u << 27) | (1u << 14),
        (1u << 3) | (1u << 13), (1u << 4) | (1u << 21) | (1u << 22)};
    app.anchors.clear();
    // The stand-in bears, with their enchantments round them.
    for (int i = 0; bears && bear && bear->still && i < kBears; ++i) {
        const SDL_FPoint at = bearAt(i);
        const float x = at.x - bear->halfWidth;
        const float y = at.y - bear->halfHeight - bear->z;
        const auto netCode = static_cast<std::uint16_t>(kFirstBear + i);
        Anchor anchor{at.x, at.y, x, y, app.sprites->indexOf(*bear->still)};
        anchor.typeId = static_cast<std::uint16_t>(app.things.findThing("Bear") - app.things.things.data() + 1);
        anchor.direction = 7;
        anchor.buffs = enchant ? kBearBuffs[static_cast<std::size_t>(i)] : 0;
        app.anchors[netCode] = anchor;
        app.effects->enchantSprites(anchor, netCode, false, *app.sprites, extra);
        if (const Sprite* s = app.sprites->get(*bear->still)) {
            extra.push_back({at.y, x, y, s, at.x, at.y});
        }
        app.effects->enchantSprites(anchor, netCode, true, *app.sprites, extra);
    }
    // "shields": eight bears in two rows, each facing one of the eight ways, with a reflective
    // shield; the second row's last also with a Force Field.
    constexpr std::array<std::uint8_t, 8> kFacings{0, 1, 2, 3, 5, 6, 7, 8};
    for (std::size_t i = 0; app.fxDemoOnly == "shields" && bear && bear->still && i < kFacings.size(); ++i) {
        const SDL_FPoint at{std::floor(app.camera.x) + (static_cast<float>(i % 4) - 1.5f) * 100,
                            std::floor(app.camera.y) + static_cast<float>(i / 4) * 130 - 40};
        const float x = at.x - bear->halfWidth;
        const float y = at.y - bear->halfHeight - bear->z;
        const auto netCode = static_cast<std::uint16_t>(kFirstBear - 16 + static_cast<int>(i));
        Anchor anchor{at.x, at.y, x, y, app.sprites->indexOf(*bear->still)};
        anchor.typeId = static_cast<std::uint16_t>(app.things.findThing("Bear") - app.things.things.data() + 1);
        anchor.direction = kFacings[i];
        anchor.buffs = (1u << 27) | (i == 7 ? 1u << 26 : 0u);
        app.anchors[netCode] = anchor;
        app.effects->enchantSprites(anchor, netCode, false, *app.sprites, extra);
        if (const Sprite* s = app.sprites->get(*bear->still)) {
            extra.push_back({at.y, x, y, s, at.x, at.y});
        }
        app.effects->enchantSprites(anchor, netCode, true, *app.sprites, extra);
    }
    // "rain": objects that keep making effects where they stand.
    constexpr std::array<std::string_view, 6> kStanding{"ToxicCloud", "VortexSource", "LevelUp",
                                                        "BlueRain",   "PlayerWaypoint", "ManaBombCharge"};
    for (std::size_t i = 0; !app.fxDemoOnly.empty() && show("rain") && i < kStanding.size(); ++i) {
        const auto* def = app.things.findThing(kStanding[i]);
        if (!def) {
            continue;
        }
        Anchor anchor;
        // The first four in a row; the waypoint below, and the mana bomb in the middle.
        anchor.x = std::floor(app.camera.x) + static_cast<float>(i < 4 ? static_cast<int>(i) * 200 - 300 : 0);
        anchor.y = std::floor(app.camera.y) + (i < 4 ? 60.0f : i == 4 ? 200.0f : -150.0f);
        anchor.typeId = static_cast<std::uint16_t>(def - app.things.things.data() + 1);
        app.anchors[static_cast<std::uint16_t>(0x7FD0 + i)] = anchor;
    }
    // Objects whose own DRAW / CLIENTUPDATE functions make effects, circling the centre.
    constexpr std::array<std::string_view, 6> kMovers{"Fireball", "MagicMissile", "Magic", "DeathBall", "Pixie",
                                                      "TeleportWake"};
    const double seconds = static_cast<double>(SDL_GetTicks()) / 1000;
    for (std::size_t i = 0; show("objects") && i < kMovers.size(); ++i) {
        const auto* def = app.things.findThing(kMovers[i]);
        const TypeArt* art = app.art->get(kMovers[i]);
        if (!def || !art) {
            continue;
        }
        const double a = seconds * 1.5 + static_cast<double>(i) * 2 * std::numbers::pi / kMovers.size();
        const float mx = std::floor(app.camera.x) + static_cast<float>(std::cos(a) * 170);
        const float my = std::floor(app.camera.y) + static_cast<float>(std::sin(a) * 110);
        const float x = mx - art->halfWidth;
        const float y = my - art->halfHeight - art->z;
        const auto typeId = static_cast<std::uint16_t>(def - app.things.things.data() + 1);
        Pose pose;
        // Facing along the circle: the 3 x 3 AnimDir grid from the velocity's signs.
        const double vx = -std::sin(a);
        const double vy = std::cos(a);
        const int col = vx < -0.38 ? 0 : vx > 0.38 ? 2 : 1;
        const int row = vy < -0.38 ? 0 : vy > 0.38 ? 2 : 1;
        pose.direction = static_cast<std::uint8_t>(row * 3 + col == 4 ? 7 : row * 3 + col);  // the middle cell (4) has no facing: south (7)
        pose.time = seconds * 30;
        pose.netCode = static_cast<std::uint16_t>(i);
        const auto image = pickImage(*art, pose);
        Anchor anchor{mx, my, x, y, std::nullopt, typeId, 0};
        if (image) {
            anchor.image = app.sprites->indexOf(*image);
            if (const Sprite* s = app.sprites->get(*image)) {
                extra.push_back({my, x, y, s, mx, my});
            }
        }
        app.anchors[static_cast<std::uint16_t>(0x7FE0 + i)] = anchor;
    }
    const int cx = static_cast<int>(app.camera.x);
    const int cy = static_cast<int>(app.camera.y);
    // The FX messages below go out at most once a frame.
    if (frame == app.fxDemoFrame) {
        return;
    }
    app.fxDemoFrame = frame;
    using nox::net::Op;
    const auto post = [&](const std::vector<std::uint8_t>& bytes) {
        if (auto fx = nox::net::decodeFx(bytes)) {
            app.effects->add({frame, std::move(*fx)});
        }
    };
    // A message's bytes as the server lays them out: the op, the bytes, then the 16-bit values little-endian.
    const auto bytesOf = [](Op op, std::initializer_list<int> u16s, std::initializer_list<int> u8s = {}) {
        std::vector<std::uint8_t> b{static_cast<std::uint8_t>(op)};
        for (const int v : u8s) {
            b.push_back(static_cast<std::uint8_t>(v));
        }
        for (const int v : u16s) {
            b.push_back(static_cast<std::uint8_t>(v));
            b.push_back(static_cast<std::uint8_t>(v >> 8));
        }
        return b;
    };
    // Held beams: the server sends these every frame while the spell lasts.
    if (show("beams")) {
        post(bytesOf(Op::FxLightning, {cx - 250, cy - 190, cx + 250, cy - 170}));
        post(bytesOf(Op::FxSentryRay, {cx - 250, cy + 230, cx + 250, cy + 240}));
    }
    if (app.fxDemoOnly == "chain") {
        // A chain lightning on sixteen targets, every frame (a drawing load test).
        for (int i = 0; i < 16; ++i) {
            const double a = i * 2 * std::numbers::pi / 16;
            post(bytesOf(Op::FxChainLightningBolt,
                         {cx, cy, cx + static_cast<int>(260 * std::cos(a)), cy + static_cast<int>(200 * std::sin(a))}));
        }
    }
    // The rest every 60 frames (two seconds).
    if (frame < app.fxDemoNext) {
        return;
    }
    const bool first = app.fxDemoNext == 0;
    app.fxDemoNext = frame + 60;
    if (first && show("beams")) {
        // Duration beams between the bears (MSG_FX_DURATION_SPELL: kind, level, two net codes).
        post(bytesOf(Op::FxDurationSpell, {kFirstBear + 0, kFirstBear + 1}, {3, 1})); // chain lightning
        post(bytesOf(Op::FxDurationSpell, {kFirstBear + 2, kFirstBear + 3}, {4, 5})); // energy bolt, level 5
        post(bytesOf(Op::FxDurationSpell, {kFirstBear + 4, kFirstBear + 5}, {5, 1})); // drain mana
        post(bytesOf(Op::FxDurationSpell, {kFirstBear + 1, kFirstBear + 4}, {7, 1})); // harpoon rope
    }
    if (show("beams")) {
        post(bytesOf(Op::FxGreenBolt, {cx - 250, cy - 140, cx - 60, cy - 120, 20}));
    }
    const auto send = [&](Op op, std::initializer_list<int> values, std::optional<std::uint8_t> tail = {}) {
        auto bytes = bytesOf(op, values);
        if (tail) {
            bytes.push_back(*tail);
        }
        post(bytes);
    };
    if (show("bursts")) {
        send(Op::FxBlueSparks, {cx - 130, cy - 90});
        send(Op::FxYellowSparks, {cx, cy - 90});
        send(Op::FxCyanSparks, {cx + 130, cy - 90});
        send(Op::FxVioletSparks, {cx - 130, cy + 10});
        send(Op::FxSparkExplosion, {cx, cy + 10}, std::uint8_t{255});
        send(Op::FxExplosion, {cx + 130, cy + 10});
        send(Op::FxSmokeBlast, {cx - 130, cy + 110});
        send(Op::FxRicochet, {cx, cy + 110});
        send(Op::FxDamagePoof, {cx + 130, cy + 110});
        send(Op::FxTeleport, {cx + 200, cy - 90});
        send(Op::FxDeathRay, {cx - 200, cy + 190, cx + 200, cy + 190});
        send(Op::FxVampirism, {cx + 200, cy + 10, cx + 280, cy + 110, 28});
        send(Op::FxArrowTrap, {cx - 250, cy - 20}, std::uint8_t{1});
        send(Op::FxArrowTrap, {cx - 250, cy + 40}, std::uint8_t{2});
    }
    if (show("undead")) {
        send(Op::FxTurnUndead, {cx, cy});
    }
    if (show("summon")) {
        // MSG_FX_SUMMON: point, id, creature type, facing, duration; a bear over 50 frames.
        if (const auto* def = app.things.findThing("Bear")) {
            const int typeId = static_cast<int>(def - app.things.things.data() + 1);
            auto bytes = bytesOf(Op::FxSummon, {cx, cy, 1, typeId});
            bytes.push_back(64); // facing south
            bytes.push_back(50);  // duration 50, 16-bit little-endian
            bytes.push_back(0);
            post(bytes);
        }
    }
    if (show("particles")) {
        // MSG_FX_SHIELD on the first bear, facing south.
        auto bytes = bytesOf(Op::FxShield, {kFirstBear});
        bytes.push_back(7);  // shield type 7: SphericalShieldS
        post(bytes);
        if (!first) {
            send(Op::FxManaBombCancel, {cx, cy});
        }
    }
    // MSG_FX_PARTICLEFX: code, count, flag, max, net code, 4 unused bytes.
    for (int code = 0; show("particles") && code < kBears; ++code) {
        std::vector<std::uint8_t> bytes{static_cast<std::uint8_t>(Op::FxParticlefx), static_cast<std::uint8_t>(code)};
        const int count = code == 1 ? 20 : 60;
        for (const int v : {count, code == 3 ? 1 : 0, 40, static_cast<int>(kFirstBear + code)}) {
            bytes.push_back(static_cast<std::uint8_t>(v));
            bytes.push_back(static_cast<std::uint8_t>(v >> 8));
        }
        bytes.resize(14, 0);  // the 4 unused bytes
        post(bytes);
    }
}

/// The AUDIOLOOP sounds of everything near, heard from our player (the
/// view's centre in the viewer): the map's objects the client loads itself and the server's
/// drawables. A loop plays while its thing is enabled and not hidden (REPORT_X_STATUS 4 / 8).
void soundLoops(App& app, const std::set<std::uint32_t>& liveCodes)
{
    if (!app.audio) {
        return;
    }
    Audio::Listener listener;
    listener.x = app.camera.x;
    listener.y = app.camera.y;
    listener.viewCentreX = app.camera.x;
    listener.halfViewWidth = static_cast<float>(app.camera.viewWidth) / (2 * app.camera.zoom);
    const nox::client::World* world = app.live ? &app.live->client().world() : nullptr;
    if (world) {
        if (const auto* self = world->drawable(app.live->client().netCode())) {
            const auto p = app.interpolator.position(app.live->client().netCode(), *self);
            listener.x = p.x;
            listener.y = p.y;
        }
    }
    app.audio->beginLoops(listener);
    if (app.view) {
        constexpr std::uint64_t kMapObject = 1ull << 32;  // keeps map objects' keys apart from net codes
        for (const auto& s : app.view->soundSources()) {
            // Live: only the client-loaded ones the server doesn't also send (by extent, or extent with bit 15).
            if (world && (!s.clientLoaded || liveCodes.contains(s.extent) || liveCodes.contains(s.extent | 0x8000))) {
                continue; // the server sends that one
            }
            if (const auto id = s.soundId) {
                app.audio->loop(kMapObject | s.extent, *id, s.x, s.y, s.enabled);
            }
        }
    }
    if (world) {
        for (const auto& [netCode, d] : world->drawables()) {
            const std::uint32_t id = d.typeId < app.loopSounds.size() ? app.loopSounds[d.typeId] : 0;
            if (id == 0) {
                continue;
            }
            const auto p = app.interpolator.position(netCode, d);
            app.audio->loop(netCode, id, p.x, p.y, d.enabled && (d.xStatus & 0xC) == 0);
        }
    }
    if (app.weather) {
        app.weather->sound(*app.audio); // (the weather's wind, and its thunder)
    }
    app.audio->endLoops();
}

/// This frame's light sources. The viewer lights everything the map places; a live game lights
/// the map's client-loaded lights (ColorLight) plus the drawables the server sent: LIGHT-class
/// objects, with the server's reported colour and intensity when it sent them, and the glow the
/// client gives monsters and some enchantments.
std::vector<LightSource> frameLights(App& app, const std::set<std::uint32_t>& liveCodes)
{
    std::vector<LightSource> out;
    const auto frame = app.live ? gameFrame(app)
                                : static_cast<std::uint32_t>(SDL_GetTicks() * 30 / 1000);
    for (const auto& ml : app.view->lights()) {
        if (ml.enabled && (!app.live || (ml.clientLoaded && !liveCodes.contains(ml.extent)))) {
            out.push_back(ml.at(frame)); // ColorLights animate their colour and intensity
        }
    }
    app.effects->lights(out, app.light.perPixel()); // per-pixel: the glows light the ground too
    if (!app.live) {
        return out;
    }
    namespace ObjectClass = nox::things::ObjectClass;
    constexpr std::uint32_t kEnchantLight = 1u << 15;
    constexpr std::uint32_t kEnchantBlueGlow = 1u << 23;
    constexpr std::uint8_t kMonsterDead = 10;
    for (const auto& [netCode, d] : app.live->client().world().drawables()) {
        const TypeArt* a = app.art->get(d.typeId);
        if (!a || !d.enabled) {
            continue;
        }
        const auto p = app.interpolator.position(netCode, d);
        if (auto own = app.effects->drawableLight(netCode)) {
            // Set by its DRAW function this frame (a missile's flicker).
            own->x = p.x;
            own->y = p.y;
            own->seed = netCode;
            out.push_back(*own);
            continue;
        }
        LightSource light;
        light.x = p.x;
        light.y = p.y;
        light.seed = netCode;
        light.flicker = a->flicker;
        light.cone = a->lightCone; // (the drawable's direction and cone are its type's, whatever lights it)
        if (d.buffs & kEnchantBlueGlow) {
            light.color = {0x80, 0x80, 0xFF};
            light.intensity = 300;
        } else if ((d.buffs & kEnchantLight) ||
                   (netCode == app.live->client().netCode() && (app.live->client().world().self().itemBuffs & 8))) {
            // The Light enchantment, or the local player's items giving light.
            light.intensity = 200;
        } else if (a->classes & ObjectClass::Light) {
            light.intensity = d.lightIntensity.value_or(a->lightIntensity);
            light.color = d.lightColor.value_or(a->lightColor);
        } else if (a->classes & ObjectClass::Monster) {
            if (d.animState == kMonsterDead) {
                continue;
            }
            // SMALL, MEDIUM, LARGE monster subclasses glow faintly.
            light.intensity = (a->subclasses & 1) ? 25.0f : (a->subclasses & 2) ? 35.0f : (a->subclasses & 4) ? 45.0f : 0.0f;
        }
        // A negative intensity is an anti-light: it darkens.
        if (light.intensity < 0) {
            light.intensity = -light.intensity;
            light.anti = true;
        }
        if (light.intensity > 1) {
            out.push_back(light);
        }
    }
    return out;
}

} // namespace noxsdl
