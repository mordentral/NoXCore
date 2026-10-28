// World: the client's model of the game, updated one server message at a time (apply) and by the
// update stream (applyStream). It keeps drawables, players, teams, our own stats and inventory,
// walls and game state, and queues effects, sounds and spellbook events for a renderer or bot.
#include "noxcore/client/world.hpp"

#include "noxcore/net/messages.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/field_guide.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/bytes.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iterator>

namespace nox::client {

using net::Op;

namespace {

// Net code bit 15: a static (map-placed) drawable.
constexpr std::uint16_t kStaticBit = 0x8000;

// Little-endian field readers; the caller keeps the offset inside the message.
using util::u16At;
using util::u32At;

float f32At(std::span<const std::uint8_t> m, std::size_t off)
{
    return std::bit_cast<float>(u32At(m, off));
}

// A zero-terminated UTF-16LE string of at most `chars` characters, returned as UTF-8.
std::string utf16At(std::span<const std::uint8_t> m, std::size_t off, std::size_t chars)
{
    std::u16string s;
    for (std::size_t i = 0; i < chars && off + 2 * i + 1 < m.size(); ++i) {
        const auto c = static_cast<char16_t>(u16At(m, off + 2 * i));
        if (c == 0) {
            break;
        }
        s.push_back(c);
    }
    return strings::toUtf8(s);
}

// A zero-terminated 8-bit string of at most `n` bytes.
std::string asciiAt(std::span<const std::uint8_t> m, std::size_t off, std::size_t n)
{
    std::string s;
    for (std::size_t i = 0; i < n && off + i < m.size() && m[off + i] != 0; ++i) {
        s.push_back(static_cast<char>(m[off + i]));
    }
    return s;
}

} // namespace

// ------------------------------------------------------------------------------- helpers

// A type id's object class bits (type ids are the thing.bin index + 1); nullopt if unknown.
std::optional<std::uint32_t> World::classOf(std::uint16_t typeId) const
{
    if (!things_ || typeId == 0 || typeId > things_->things.size()) {
        return std::nullopt;
    }
    return things::thingClasses(things_->things[typeId - 1u]);
}

std::optional<std::uint32_t> World::drawableClass(std::uint16_t netCode, std::uint16_t typeId) const
{
    if (const auto it = drawables_.find(netCode); it != drawables_.end()) {
        if (const auto cls = classOf(it->second.typeId)) {
            return cls;
        }
    }
    return classOf(typeId);
}

const Drawable* World::drawable(std::uint16_t netCode) const
{
    const auto it = drawables_.find(netCode);
    return it == drawables_.end() ? nullptr : &it->second;
}

std::string World::thingName(std::uint16_t typeId) const
{
    if (things_ && typeId != 0 && typeId <= things_->things.size()) {
        return things_->things[typeId - 1u].name;
    }
    return "#" + std::to_string(typeId);
}

std::vector<FxEvent> World::takeEffects()
{
    std::vector<FxEvent> out(std::make_move_iterator(effects_.begin()), std::make_move_iterator(effects_.end()));
    effects_.clear();
    return out;
}

std::vector<AudioCue> World::takeAudio()
{
    std::vector<AudioCue> out(audio_.begin(), audio_.end());
    audio_.clear();
    return out;
}

std::vector<BookEvent> World::takeBookEvents()
{
    std::vector<BookEvent> out(book_.begin(), book_.end());
    book_.clear();
    return out;
}

void World::queueBook(const BookEvent& e)
{
    book_.push_back(e);
    if (book_.size() > kMaxBookEvents) {
        book_.pop_front();
    }
}

void World::setSpellLevel(std::uint8_t spell, std::uint8_t level)
{
    self_.spellLevels[spell] = level;
    if (!things_) {
        return;
    }
    const auto* def = things_->findSpell(spell);
    // Linked spells: flag 0x1000, 0x4000 or 0x10000 on this spell gives every spell carrying the
    // matching 0x2000, 0x8000 or 0x20000 flag the same level.
    const std::uint32_t flags = def ? def->flags : 0;
    const std::uint32_t linked = flags & 0x1000 ? 0x2000 : flags & 0x4000 ? 0x8000 : flags & 0x10000 ? 0x20000 : 0;
    if (linked == 0) {
        return;
    }
    // 0x89 is kSpellCount: one past the last spell id.
    for (std::uint32_t id = 1; id < 0x89; ++id) {
        if (const auto* s = things_->findSpell(id); s && (s->flags & linked)) {
            self_.spellLevels[static_cast<std::uint8_t>(id)] = level;
        }
    }
}

void World::queueAudio(const AudioCue& cue)
{
    audio_.push_back(cue);
    if (audio_.size() > kMaxAudio) {
        audio_.pop_front();
    }
}

void World::queueEffect(std::span<const std::uint8_t> m, std::uint32_t frame)
{
    if (auto fx = net::decodeFx(m)) {
        effects_.push_back({frame, std::move(*fx)});
        if (effects_.size() > kMaxEffects) {
            effects_.pop_front();
        }
    }
}

void placeSummons(LocalPlayer& p)
{
    // The places tried, in order: cell (x, y).
    static constexpr std::array<std::array<int, 2>, 4> kPlaces{{{0, 0}, {0, 1}, {1, 0}, {1, 1}}};
    p.summonCells.fill(-1);
    // The cells a creature covers, {width, height}: small 1 x 1, medium 1 x 2, large 2 x 2.
    const auto span = [](std::uint8_t size) {
        return size == 1 ? std::array<int, 2>{1, 1} : size == 2 ? std::array<int, 2>{1, 2} : std::array<int, 2>{2, 2};
    };
    for (const std::uint8_t size : {std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{4}}) {
        for (std::size_t slot = 0; slot < p.summons.size(); ++slot) {
            auto& s = p.summons[slot];
            if (!s || s->size != size) {
                continue;
            }
            const auto [w, h] = span(size);
            // Stepping the place index by the size: a medium one tries (0,0) and (1,0), a large one (0,0) only.
            for (int i = 0; i < 4; i += size) {
                s->x = kPlaces[static_cast<std::size_t>(i)][0];
                s->y = kPlaces[static_cast<std::size_t>(i)][1];
                bool fits = true;
                for (int x = s->x; x < s->x + w; ++x) {
                    for (int y = s->y; y < s->y + h; ++y) {
                        fits = fits && x < 2 && y < 2 && p.summonCells[static_cast<std::size_t>(y + x * 2)] < 0;
                    }
                }
                if (fits) {
                    for (int x = s->x; x < s->x + w; ++x) {
                        for (int y = s->y; y < s->y + h; ++y) {
                            p.summonCells[static_cast<std::size_t>(y + x * 2)] = static_cast<int>(slot);
                        }
                    }
                    break;
                }
            }
        }
    }
}
void World::gameOverSound(std::span<const std::uint8_t> m, std::uint32_t frame)
{
    // The four winner messages (0x56-0x59): GameOver, unless it's older than the map.
    if (m.size() >= 8 && u32At(m, 4) > mapFrame_) {
        queueAudio({frame, AudioCue::Kind::Local, 0x135, 100, 0});
        game_.over = true; // (Game_SetFlags(8), Gui_Rank_ShowOnGameOver)
        game_.overKind = m[0];
        game_.overWinner = static_cast<std::uint16_t>(u16At(m, 1) & 0x7FFF);
        game_.overTimeLimit = m[3] == 1;
        ++game_.overCount;
    }
}

void World::informSound(std::span<const std::uint8_t> m, std::uint32_t frame)
{
    // CliNet_OnInform: the flag notices' sounds, for players the client knows.
    if (m.size() < 6) {
        return;
    }
    const bool known = players_.count(static_cast<std::uint16_t>(u32At(m, 2))) != 0;
    std::uint16_t id = 0;
    switch (m[1]) {
    case 4: // a flag returned
        id = known ? 0x131 : 0; // FlagRespawn
        break;
    case 5:
        id = known ? 0x132 : 0; // FlagCapture
        break;
    case 6:
        id = known ? 0x12F : 0; // FlagPickup
        break;
    case 7:
        id = known ? 0x130 : 0; // FlagDrop
        break;
    case 8: // a flag back at its base
        id = 0x131;
        break;
    default:
        break;
    }
    if (id != 0) {
        queueAudio({frame, AudioCue::Kind::Local, id, 100, 0});
    }
}

void World::sentryRaySound(std::int32_t x, std::int32_t y, std::uint32_t frame)
{
    // MSG_FX_SENTRY_RAY (case 0x95): one ray in four makes the wall it hits ring, by its
    // distance from us, out to 600.
    // The visual random number: a linear congruential step, its high half taken 0-100.
    rng_ = rng_ * 1103515245u + 12345u;
    if ((rng_ >> 16) % 101 >= 25) {
        return;
    }
    const auto self = drawables_.find(self_.netCode);
    if (self_.netCode == 0 || self == drawables_.end()) {
        return;
    }
    const double dx = x - self->second.x;
    const double dy = y - self->second.y;
    const auto d = static_cast<int>(std::sqrt(dx * dx + dy * dy));
    if (d < 600) {
        queueAudio({frame, AudioCue::Kind::Local, 0x129, static_cast<std::uint8_t>((600 - d) * 100 / 600), 0});
    }
}

Drawable& World::upsert(std::uint16_t netCode, std::uint16_t typeId, std::int32_t x, std::int32_t y, std::uint32_t frame)
{
    // Cli_CreateOrUpdateDrawable: create if unknown, else move; stamp the frame.
    auto [it, created] = drawables_.try_emplace(netCode);
    Drawable& d = it->second;
    if (created) {
        d.netCode = netCode;
        d.typeId = typeId;
        d.prevX = x;
        d.prevY = y;
        d.prevFrame = frame;
        // Its standing reports, if they came first (early_).
        if (const auto e = early_.find(netCode); e != early_.end()) {
            if (e->second.xStatus) {
                d.xStatus = *e->second.xStatus;
            }
            if (e->second.buffs) {
                d.buffs = *e->second.buffs;
            }
            if (e->second.modifiers) {
                d.modifiers = e->second.modifiers;
            }
            if (e->second.enabled) {
                d.enabled = *e->second.enabled;
            }
            early_.erase(e);
        }
        d.fade.inSightFrame = frame; // (the drawable's making stamps it)
    } else if (frame != d.lastSeenFrame) {
        // Where it was at its last update (the drawable's previous position, for the
        // movement test for the invisible).
        d.prevX = d.x;
        d.prevY = d.y;
        d.prevFrame = d.lastSeenFrame;
    }
    // An update takes back a word of shadows and its pending expiry (the marks it set stay).
    d.fade.told = false;
    d.fade.expireAt = 0;
    d.x = x;
    d.y = y;
    if (!created && (x != d.prevX || y != d.prevY)) {
        d.movedFrame = frame;
    }
    d.lastSeenFrame = frame;
    return d;
}

void World::predict(std::uint32_t frame)
{
    // A predicted drawable: frames since the start plus one steps, each taking the friction
    // off the velocity and then adding it; gone once off the map (0 < x, y < 0x1700). The game
    // also drops it once it's out of sight (dropPredicted).
    std::erase_if(drawables_, [frame](auto& kv) {
        Drawable& d = kv.second;
        if (!d.linear) {
            return false;
        }
        const Drawable::Linear& l = *d.linear;
        float vx = l.vx;
        float vy = l.vy;
        float dx = 0;
        float dy = 0;
        const std::uint32_t steps = (frame >= l.startFrame ? frame - l.startFrame : 0) + 1;
        for (std::uint32_t i = 0; i < steps; ++i) {
            vx -= vx * l.friction;
            vy -= vy * l.friction;
            dx += vx;
            dy += vy;
        }
        // Math_FloatToInt: FISTP in the game's chop mode (its main loop sets it), so truncated.
        d.x = static_cast<std::int32_t>(static_cast<float>(l.startX) + dx);
        d.y = static_cast<std::int32_t>(static_cast<float>(l.startY) + dy);
        return d.x <= 0 || d.y <= 0 || d.x >= 0x1700 || d.y >= 0x1700;
    });
}

void World::dropPredicted(const std::function<bool(const Drawable&)>& gone)
{
    std::erase_if(drawables_, [&](const auto& kv) { return kv.second.linear && gone(kv.second); });
}

// The anim byte: bits 4-6 the direction 0-7, stored as 0-3, 5-8 (the 3 x 3 grid skipping
// its centre, 4); the low nibble the state.
void World::setAnim(Drawable& d, std::uint8_t anim)
{
    d.anim = anim;
    std::uint8_t dir = (anim >> 4) & 7;
    d.direction = dir > 3 ? static_cast<std::uint8_t>(dir + 1) : dir;
    d.animState = anim & 0x0F;
}

bool World::removesItself(const Drawable& d) const
{
    // CliNet_OnPacket leaves "AnimateDraw" objects whose animation kind is OneShotRemove (1,
    // get_animation_kind_id) alone: they remove themselves when the animation ends.
    if (!things_ || d.typeId == 0 || d.typeId > things_->things.size()) {
        return false;
    }
    const auto& draw = things_->things[d.typeId - 1u].draw;
    if (!draw || draw->type != "AnimateDraw" || draw->data.size() < 3) {
        return false;
    }
    // The AnimateDraw data: byte 2 is the animation kind's name length, the name follows it.
    const std::size_t len = draw->data[2];
    if (draw->data.size() < 3 + len) {
        return false;
    }
    return std::string(reinterpret_cast<const char*>(draw->data.data() + 3), len) == "OneShotRemove";
}

// Drops a drawable unless it removes itself (OneShotRemove). With `keepSelf` our own player
// stays: out of sight or in shadows doesn't drop us.
void World::remove(std::uint16_t netCode, bool keepSelf)
{
    const auto it = drawables_.find(netCode);
    if (it == drawables_.end() || removesItself(it->second)) {
        return;
    }
    if (keepSelf && self_.netCode != 0 && netCode == self_.netCode) {
        return;
    }
    drawables_.erase(it);
}

// MSG_OBJECT_IN_SHADOWS: the drawable's marks all set (seen, lingering, told); then with Fade
// Objects on it's kept a game second (but our own, kept as it is), else dropped as out of sight.
void World::inShadows(std::uint16_t netCode, std::uint32_t frame)
{
    const auto it = drawables_.find(netCode);
    if (it == drawables_.end()) {
        return;
    }
    FadeState& f = it->second.fade;
    f.seen = f.lingering = f.told = true;
    if (!fadeObjects_) {
        remove(netCode, true);
        return;
    }
    if (self_.netCode != 0 && netCode == self_.netCode) {
        return;
    }
    if (f.expireAt == 0) {
        expiring_.push_back(netCode);
    }
    f.expireAt = frame + kGameFps;
}

// The expiry list: drawables told into shadows go once their frame comes.
void World::expire(std::uint32_t frame)
{
    std::erase_if(expiring_, [&](std::uint16_t netCode) {
        const auto it = drawables_.find(netCode);
        if (it == drawables_.end() || it->second.fade.expireAt == 0) {
            return true; // gone already, or updated since
        }
        if (it->second.fade.expireAt > frame) {
            return false;
        }
        drawables_.erase(it);
        return true;
    });
}

// The client's frame runs the expiry list after it has taken in all of that frame's messages and
// drawn the scene, for the frame they brought: an update in a drawable's last frame still keeps
// it. Here: once a message or update of a later frame comes, the list runs for the frame before.
void World::nextFrame(std::uint32_t frame)
{
    if (frame == expiryFrame_) {
        return;
    }
    if (!expiring_.empty()) {
        expire(expiryFrame_);
    }
    expiryFrame_ = frame;
}

// ---------------------------------------------------------------------------- the stream

void World::applyStream(const net::UpdateStream& s, std::uint32_t frame, bool late)
{
    // (noxcpp: a late stream, its datagram overtaken by a newer frame's, only adds what it
    // introduces; the vanilla client applies it whole, putting things back a frame for a frame.)
    nextFrame(frame);
    if (late) {
        const auto& h = s.header;
        if ((h.netCode != 0 || h.typeId != 0) && !drawables_.contains(h.netCode)) {
            upsert(h.netCode, h.typeId, h.x, h.y, frame);
        }
        for (std::size_t i = 0; i < s.entries.size(); ++i) {
            const auto& e = s.entries[i];
            if (!(s.aborted && i + 1 == s.entries.size()) && !drawables_.contains(e.netCode)) {
                Drawable& d = upsert(e.netCode, e.typeId, e.x, e.y, frame);
                if (e.hasAnim) {
                    animate(d, e);
                }
            }
        }
        return;
    }
    const auto& h = s.header;
    camera_ = std::pair{h.x, h.y};
    if (h.netCode != 0 || h.typeId != 0) {
        animate(upsert(h.netCode, h.typeId, h.x, h.y, frame), h);
    }
    for (std::size_t i = 0; i < s.entries.size(); ++i) {
        if (s.aborted && i + 1 == s.entries.size()) {
            break; // the rejected entry never got a drawable
        }
        const auto& e = s.entries[i];
        Drawable& d = upsert(e.netCode, e.typeId, e.x, e.y, frame);
        if (e.hasAnim) {
            animate(d, e);
        }
    }
}

void World::animate(Drawable& d, const net::StreamObject& o)
{
    setAnim(d, o.anim);
    d.frame = o.frame;
    if (o.state) {
        d.animState = *o.state;
    }
}

// -------------------------------------------------------------------------------- messages

// MSG_TEAM_MSG: byte 1 the sub-op, then mostly the team id as a u32 at +2.
void World::team(std::span<const std::uint8_t> m)
{
    const std::uint8_t sub = m[1];
    switch (sub) {
    case 0x00: { // create/update
        // Layout: id +2, score +10, name length +15, colour +16, UTF-16 name from +18.
        Team& t = teams_[u32At(m, 2)];
        t.id = u32At(m, 2);
        t.score = u32At(m, 10);
        t.color = m[16];
        t.name = utf16At(m, 18, m[15]);
        break;
    }
    case 0x01: // add member
    case 0x03: { // change team
        const std::uint32_t id = u32At(m, 2);
        const std::uint16_t nc = u16At(m, 6);
        for (auto& [_, t] : teams_) {
            t.members.erase(nc);
        }
        teams_[id].id = id;
        teams_[id].members.insert(nc);
        if (const auto p = players_.find(nc); p != players_.end()) {
            if (sub == 0x01 ? p->second.team != id : p->second.team != 0) {
                p->second.flagIconTeam = id;
            }
            p->second.team = id;
        }
        break;
    }
    case 0x02: { // remove from team
        const auto nc = static_cast<std::uint16_t>(u32At(m, 2));
        for (auto& [_, t] : teams_) {
            t.members.erase(nc);
        }
        if (const auto p = players_.find(nc); p != players_.end()) {
            p->second.team = 0;
        }
        break;
    }
    case 0x04:
        teams_[u32At(m, 2)].name = utf16At(m, 6, 20);
        break;
    case 0x05:
        teams_[u32At(m, 2)].members.clear();
        break;
    case 0x06:
        teams_.erase(u32At(m, 2));
        break;
    case 0x07:
        teams_.clear();
        break;
    case 0x08:
        teams_[u32At(m, 2)].score = u32At(m, 6);
        break;
    case 0x09:
        for (auto& [_, t] : teams_) {
            t.score = 0;
        }
        break;
    case 0x0C: // a player's health percentage (quest games: CliNet_OnPacket 0xC4)
        if (const auto p = players_.find(u16At(m, 2)); p != players_.end()) {
            p->second.healthPct = m[4];
        }
        break;
    default:
        break;
    }
}

void World::apply(std::span<const std::uint8_t> m, std::uint32_t frame)
{
    if (m.empty()) {
        return;
    }
    applyFrame_ = frame;
    nextFrame(frame);
    const auto drawableAt = [&](std::size_t off) -> Drawable* {
        const auto it = drawables_.find(u16At(m, off));
        return it == drawables_.end() ? nullptr : &it->second;
    };
    // A static drawable (net code bit 15: Cli_GetStaticDrawableByNetCode), one of the map's objects
    // the client loads itself; noted by its extent for the renderer.
    const auto staticAt = [&](std::size_t off) -> StaticObjectState* {
        const std::uint16_t code = u16At(m, off);
        return (code & 0x8000) ? &game_.statics[static_cast<std::uint16_t>(code & 0x7FFF)] : nullptr;
    };
    if (net::isFxOp(m[0])) {
        queueEffect(m, frame);
        if (m[0] == static_cast<std::uint8_t>(Op::FxSentryRay) && m.size() >= 9) {
            sentryRaySound(u16At(m, 5), u16At(m, 7), frame);
        }
        return;
    }
    // Every other message by opcode; offsets below count from the opcode at byte 0.
    switch (static_cast<Op>(m[0])) {
    // ---- session and players
    case Op::UseMap:
        game_.mapName = asciiAt(m, 1, 32);
        game_.mapCrc = u32At(m, 33);
        drawables_.clear();
        expiring_.clear();
        early_.clear();
        game_.destroyedWalls.clear();
        game_.openWalls.clear();
        game_.closedWalls.clear();
        game_.magicWalls.clear();
        game_.doorAngles.clear();
        game_.statics.clear();
        bubbles_.clear();
        floaters_.clear();
        ++game_.wallsVersion;
        game_.flags.clear();
        game_.over = false; // (CliNet_OnPacket case 0x2B: Game_ClearFlags(8))
        // (noxcpp: a new map's players are alive, so a death heard before it is forgotten. A
        // solo game's death loads its save without a MSG_PLAYER_RESPAWN, and noxsdl, which reads
        // this flag for "in play", kept every spell dark and offered no pickup, use or talk hand.
        // The game's own client reads only the drawables, which a map load makes anew.)
        for (auto& [nc, p] : players_) {
            p.dead = false;
        }
        mapFrame_ = frame;
        settingsAcquired_ = false; // (CliNet_SetConnected(0): Cli_SetGameSettingsAcquired(0))
        break;
    case Op::JoinData:
        self_.netCode = u16At(m, 1);
        settingsAcquired_ = false; // (case 0x2C: Cli_SetGameSettingsAcquired(0))
        break;
    case Op::NewPlayer: {
        PlayerEntry& p = players_[u16At(m, 1)];
        p.netCode = u16At(m, 1);
        p.name = utf16At(m, 3, 25);
        p.playerClass = m[69];
        p.female = m[70] != 0;
        std::memcpy(p.colors.data(), m.data() + 71, p.colors.size());
        std::memcpy(p.clothing.data(), m.data() + 86, p.clothing.size());
        p.lessons = static_cast<std::int16_t>(u16At(m, 100));
        p.armorEquip = u32At(m, 104);
        p.weaponEquip = u32At(m, 108);
        p.status = u32At(m, 112);
        p.account = asciiAt(m, 119, 10);
        break;
    }
    case Op::PlayerQuit: {
        const std::uint16_t nc = u16At(m, 1);
        players_.erase(nc);
        for (auto& [_, t] : teams_) {
            t.members.erase(nc);
        }
        break;
    }
   case Op::GameSettings:
        // the limits changing marks the options changed (the sound comes with 0xB0).
        optionsChanged_ = game_.fragLimit != m[18] || game_.timeLimitMinutes != m[19];
        {
            // The CTF flag icons: up with two, all at home, as the CTF flag comes on (Gui_Ctf_Show);
            // down while it's off (Gui_Ctf_Hide).
            constexpr std::uint32_t kCtf = 0x20;
            const bool wasCtf = (game_.gameFlags & kCtf) != 0;
            if (!wasCtf && (u32At(m, 9) & kCtf)) {
                game_.ctfIconsShown = true;
                game_.ctfIconCount = 2;
                game_.ctfIconStates.fill(0);
            } else if (!(u32At(m, 9) & kCtf)) {
                game_.ctfIconsShown = false;
            }
        }
        game_.gameFlags = u32At(m, 9);
        game_.playerLimit = m[17];
        game_.fragLimit = m[18];
        game_.timeLimitMinutes = m[19];
        {
            // (Slot 0's frag and time limits; not a chat game's, the low 16 bits of its game flags too.)
            auto& s = game_.settingsSlots[0];
            s[0x36] = m[18];
            s[0x37] = 0;
            s[0x38] = m[19];
            if (!(m[9] & 0x80)) {  // 0x80: a chat game
                s[0x34] = m[9];
                s[0x35] = m[10];
            }
        }
        break;
    case Op::GameSettings2: {
        game_.gameName = asciiAt(m, 1, 16);
        const std::array<std::uint32_t, 2> extra{u32At(m, 37), u32At(m, 41)};
        if (std::memcmp(game_.allowedSpells.data(), m.data() + 17, game_.allowedSpells.size()) != 0 ||
            extra != settings2Extra_) {
            optionsChanged_ = true;
        }
        std::memcpy(game_.allowedSpells.data(), m.data() + 17, game_.allowedSpells.size());
        settings2Extra_ = extra;
        {
            // (Slot 0's name, strncpy 15, its spells, weapons and armour.)
            auto& s = game_.settingsSlots[0];
            bool ended = false;
            for (std::size_t i = 0; i < 15; ++i) {
                ended = ended || m[1 + i] == 0;
                s[9 + i] = ended ? 0 : m[1 + i];
            }
            std::memcpy(s.data() + 0x18, m.data() + 17, 20);
            std::memcpy(s.data() + 0x2C, m.data() + 37, 8);
        }
        // ServerOptionsChange. Not for the first settings of a session: the game
        // compares with what the game list showed, which matches; we have nothing yet.
        if (optionsChanged_ && settingsSeen_) {
            queueAudio({frame, AudioCue::Kind::Local, 0x136, 100, 0});
        }
        settingsSeen_ = true;
        break;
    }
    case Op::UpdateGuiGameSettings: { // u8 slot, then the gamedata block
        // CliNet_OnPacket 0xB1: the record into its slot (Game_GetSettingsData: slots 0 and 1;
        // the player list redrawn when the flags' 0x20 changed). The first since JOIN_DATA or USE_MAP
        // (Cli_HasGameSettings), with the client's own Server Settings window shut (a guest's
        // view: none here), copies slot 1 to slot 0: the name and the limits shown (not the spells
        // allowed: Cli_ApplyAllowedSpells is GAME_SETTINGS_2's). (On a chat map then, a NameChange
        // notice for a player renamed by the server, and the sanctuary help: not here.)
        const std::uint8_t slot = m[1];
        if (slot < 2) {
            std::memcpy(game_.settingsSlots[slot].data(), m.data() + 2, game_.settingsSlots[slot].size());
        }
        if (!settingsAcquired_) {
            if (slot == 1) {
                game_.settingsSlots[0] = game_.settingsSlots[1];
                const auto& s = game_.settingsSlots[0];
                std::string name;
                for (std::size_t i = 9; i < 9 + 15 && s[i] != 0; ++i) {
                    name += static_cast<char>(s[i]);
                }
                game_.gameName = name;
                game_.fragLimit = static_cast<std::uint16_t>(s[0x36] | s[0x37] << 8);
                game_.timeLimitMinutes = s[0x38];
            }
            settingsAcquired_ = true;
        }
        break;
    }
    case Op::RateChange:
        game_.netRate = m[1];
        break;
    case Op::JournalMsg: {
        const std::string entry = asciiAt(m, 2, 64);
        auto& j = game_.journal;
        const auto it = std::find_if(j.begin(), j.end(), [&](const auto& e) { return e.entry == entry; });
        if (m[1] == 1) {
            j.push_back({entry, u16At(m, 66)}); // (the exe keeps duplicates too)
        } else if (m[1] == 2 && it != j.end()) {
            j.erase(it);
        } else if (m[1] == 3 && it != j.end()) {
            it->type = u16At(m, 66);
        }
        break;
    }
    case Op::Trade:
        tradeMessage(m);
        break;
    case Op::Dialog:
        dialogMessage(m);
        break;
    // ---- sound: played by the client as they come (CliNet_OnPacket cases 0xA6, 0xA7, 0xE5-0xE7)
    case Op::AudioEvent:
    case Op::AudioPlayerEvent: {
        // +1 the pan; u16 at +2: low 10 bits the sound id, top 6 bits the volume (shifted to 0-126).
        const std::uint16_t bits = u16At(m, 2);
        AudioCue c;
        c.frame = frame;
        c.kind = m[0] == static_cast<std::uint8_t>(Op::AudioPlayerEvent) ? AudioCue::Kind::PlayerSound
                                                                         : AudioCue::Kind::Sound;
        c.id = bits & 0x3FF;
        c.volume = static_cast<std::uint8_t>((bits & 0xFC00) >> 9);
        c.pan = static_cast<std::int8_t>(m[1]);
        queueAudio(c);
        break;
    }
    case Op::MusicEvent:
        queueAudio({frame, AudioCue::Kind::Music, m[1], m[2], 0});
        break;
    case Op::MusicPushEvent:
        queueAudio({frame, AudioCue::Kind::MusicPush, 0, 0, 0});
        break;
    case Op::MusicPopEvent:
        queueAudio({frame, AudioCue::Kind::MusicPop, 0, 0, 0});
        break;
    case Op::ReportLesson:
        if (auto p = players_.find(u16At(m, 1)); p != players_.end()) {
            p->second.lessons = static_cast<std::int32_t>(u32At(m, 3));
            p->second.kills = static_cast<std::int32_t>(u32At(m, 7));
            // Elimination games (flag 0x400): out of lives (0x4E case, PlayerEliminated).
            if ((game_.gameFlags & 0x400) && static_cast<std::int32_t>(game_.fragLimit) <= p->second.kills) {
                queueAudio({frame, AudioCue::Kind::Local, 0x138, 100, 0});
            }
        }
        break;
    case Op::ReportClientStatus:
        if (auto p = players_.find(u16At(m, 1)); p != players_.end()) {
            p->second.status = u32At(m, 3) & 0x423;
        }
        break;
    case Op::ReportAllLatency:
        if (auto p = players_.find(u16At(m, 1)); p != players_.end()) {
            p->second.latency = u16At(m, 3);
        }
        break;
    case Op::ReportTreasureCount:
        if (auto p = players_.find(u16At(m, 1)); p != players_.end()) {
            p->second.treasures = u16At(m, 3);
        }
        break;
    case Op::PlayerDied:
        if (auto p = players_.find(u16At(m, 1)); p != players_.end()) {
            p->second.dead = true;
            // (CliNet_OnPacket's 0xE8) Outside quest games the body drops everything: no weapons (nor a
            // carried flag, drawn while weapon bit 1 is on), no armour but the street clothes.
            if (!(game_.gameFlags & 0x1000)) {
                constexpr std::uint32_t kStreetClothes = 0xC0D;  // StreetSneakers, StreetPants, MedievalPants, StreetShirt, MedievalShirt
                p->second.weaponEquip = 0;
                p->second.weaponModifiers.clear();
                p->second.armorEquip &= kStreetClothes;
                std::erase_if(p->second.armorModifiers, [](const auto& e) { return (e.first & kStreetClothes) == 0; });
            }
        }
        break;
    case Op::PlayerRespawn: {
        const std::uint16_t nc = u16At(m, 1);
        if (auto p = players_.find(nc); p != players_.end()) {
            p->second.dead = false;
            if (m[8] & 1) {
                resetOutfit(p->second, m[7]); // the server sends one to us as we join
            }
        }
        // Ours, under game flag 0x2000: the u32 frame at +3 forgets drawables not seen since then.
        if (nc == self_.netCode && (game_.gameFlags & 0x2000)) {  // 0x2000: a network game
            apply(std::array<std::uint8_t, 5>{static_cast<std::uint8_t>(Op::ForgetDrawables), m[3], m[4], m[5], m[6]},
                  frame);
        }
        break;
    }
    case Op::TeamMsg:
        team(m);
        break;
   case Op::ReportFlagWinner:
    case Op::ReportFlagBallWinner:
    case Op::ReportDeathmatchTeamWinner:
        game_.lastWinner = u16At(m, 1) ? "team " + std::to_string(u16At(m, 1)) : "tie";
        gameOverSound(m, frame);
        break;
    case Op::ReportDeathmatchWinner:
        game_.lastWinner = u16At(m, 1) ? "player " + std::to_string(u16At(m, 1)) : "tie";
        gameOverSound(m, frame);
        break;
    case Op::Inform:
        informSound(m, frame);
        break;
    case Op::Gauntlet:
        gauntletMessage(m, frame);
        break;
    case Op::ReportSpellStart:
        // Cli_StartSpellChant: our spell's phonemes are chanted and their gestures shown.
        queueAudio({frame, AudioCue::Kind::SpellChant, m.size() >= 2 ? m[1] : std::uint8_t{0}, 0, 0});
        break;
    case Op::TimerStatus:
        game_.timerRunning = u32At(m, 1) != 0;
        game_.timeRemainingMs = u32At(m, 5);
        break;
    case Op::ReportFlagStatus: {
        FlagStatus& f = game_.flags[m[2]];
        f.state = m[1];
        f.side = m[3];
        f.carrier = f.state == 1 ? u16At(m, 4) : 0;
        if (m[2] >= 1 && m[2] <= game_.ctfIconStates.size()) {
            game_.ctfIconStates[m[2] - 1u] = m[1];
        }
        break;
    }
    case Op::ReportBallStatus:
        game_.ballState = m[1];
        // States 2 and 4 carry a carrier's net code at +2.
        game_.ballCarrier = (m[1] == 2 || m[1] == 4) ? u16At(m, 2) : 0;
        break;

    // ---- drawables
    case Op::SimpleObj:
        upsert(u16At(m, 1), u16At(m, 3), u16At(m, 5), u16At(m, 7), frame);
        break;
    case Op::ComplexObj:
    case Op::PlayerObj: {
        // Net code and type both 0: no object, only the camera position.
        if (u16At(m, 1) == 0 && u16At(m, 3) == 0) {
            camera_ = std::pair{static_cast<int>(u16At(m, 5)), static_cast<int>(u16At(m, 7))};
            break;
        }
        Drawable& d = upsert(u16At(m, 1), u16At(m, 3), u16At(m, 5), u16At(m, 7), frame);
        setAnim(d, m[9]);
        d.frame = m[10];
        break;
    }
    case Op::ClientPredictLinear: {
        // CliNet_OnPacket case 0xB5: the start, the velocity and friction bytes over 16, the frame;
        // from then on the client moves it (predict).
        Drawable& d = upsert(u16At(m, 1), u16At(m, 3), u16At(m, 5), u16At(m, 7), frame);
        Drawable::Linear l;
        l.startX = d.x;
        l.startY = d.y;
        l.vx = static_cast<float>(static_cast<std::int8_t>(m[12])) / 16;
        l.vy = static_cast<float>(static_cast<std::int8_t>(m[13])) / 16;
        l.friction = static_cast<float>(static_cast<std::int8_t>(m[11])) / 16;
        l.startFrame = frame;
        d.linear = l;
        predict(frame);
        break;
    }
    case Op::DestroyObject:
        remove(u16At(m, 1), false);
        break;
    case Op::ObjectOutOfSight:
        remove(u16At(m, 1), true);
        break;
    case Op::ObjectInShadows:
        inShadows(u16At(m, 1), frame);
        break;
    case Op::ForgetDrawables:
        // Cli_ForgetStaleDrawables: dynamic, non-player drawables not seen since `frame` (only with flag 0x2000).
        if (game_.gameFlags & 0x2000) {  // 0x2000: a network game
            const std::uint32_t since = u32At(m, 1);
            // (By the type's own name, not a copy of it: "#<id>" for an unknown type is never it.)
            const auto summonEffect = [&](std::uint16_t typeId) {
                return things_ && typeId != 0 && typeId <= things_->things.size() && things_->things[typeId - 1u].name == "SummonEffect";
            };
            std::erase_if(drawables_, [&](const auto& kv) {
                const Drawable& d = kv.second;
                const auto cls = classOf(d.typeId).value_or(0);
                return (d.netCode & kStaticBit) == 0 && (cls & things::ObjectClass::Player) == 0 &&
                       d.netCode != self_.netCode && !summonEffect(d.typeId) && d.lastSeenFrame < since;
            });
        }
        break;
    case Op::ObjectFriendAdd:
        friends_.insert(u16At(m, 1));
        break;
    case Op::ObjectFriendRemove:
        friends_.erase(u16At(m, 1));
        break;
    case Op::ResetFriends:
        friends_.clear();
        break;
    case Op::EnableObject:
    case Op::DisableObject:
        if (Drawable* d = drawableAt(1)) {
            d->enabled = m[0] == static_cast<std::uint8_t>(Op::EnableObject);
        } else if (!(u16At(m, 1) & 0x8000)) {
            keepEarly(u16At(m, 1)).enabled = m[0] == static_cast<std::uint8_t>(Op::EnableObject); // (ahead of its update: early_)
        }
        if (StaticObjectState* s = staticAt(1)) {
            s->enabled = m[0] == static_cast<std::uint8_t>(Op::EnableObject);
        }
        break;
    case Op::DrawFrame:
        if (Drawable* d = drawableAt(1)) {
            d->frame = m[3];
        }
        if (StaticObjectState* s = staticAt(1)) {
            s->frame = m[3];
        }
        break;
    case Op::ReportAnimationFrame:
        if (Drawable* d = drawableAt(1)) {
            d->frame = static_cast<std::uint8_t>(u32At(m, 3));
        }
        if (StaticObjectState* s = staticAt(1)) {
            s->frame = static_cast<std::uint8_t>(u32At(m, 3));
        }
        break;
    case Op::ReportEnchantment:
        if (Drawable* d = drawableAt(1)) {
            d->buffs = u32At(m, 3);
        } else {
            keepEarly(u16At(m, 1)).buffs = u32At(m, 3);
        }
        break;
    case Op::ReportXStatus:
        if (Drawable* d = drawableAt(1)) {
            d->xStatus = u32At(m, 3);
        } else if (!(u16At(m, 1) & 0x8000)) {
            keepEarly(u16At(m, 1)).xStatus = u32At(m, 3);
        }
        if (StaticObjectState* s = staticAt(1)) {
            s->xStatus = u32At(m, 3);
        }
        break;
    case Op::ReportLightColor:
        if (Drawable* d = drawableAt(1)) {
            d->lightColor = std::array<std::uint8_t, 3>{m[3], m[4], m[5]};
        }
        if (StaticObjectState* s = staticAt(1)) {
            s->lightColor = std::array<std::uint8_t, 3>{m[3], m[4], m[5]};
        }
        break;
    case Op::ReportLightIntensity:
        if (Drawable* d = drawableAt(1)) {
            d->lightIntensity = f32At(m, 3);
        }
        if (StaticObjectState* s = staticAt(1)) {
            s->lightIntensity = f32At(m, 3);
        }
        break;
    case Op::ReportZPlus:
        if (Drawable* d = drawableAt(1)) {
            d->z = m[3];
        }
        break;
    case Op::ReportZMinus:
        if (Drawable* d = drawableAt(1)) {
            d->z = -static_cast<int>(m[3]);
        }
        break;
    case Op::ReportHealth: // allies: health / 2 on the wire
        if (Drawable* d = drawableAt(1)) {
            d->health = static_cast<std::uint16_t>(m[3] * 2);
        }
        if (const auto it = self_.monitored.find(static_cast<std::uint16_t>(u16At(m, 1) & 0x7FFF));
            it != self_.monitored.end()) {
            it->second.health = static_cast<std::uint16_t>(m[3] * 2); // Cli_SetMonitoredCreatureCurHealth
        }
        break;
    case Op::ReportObjectPoison:
        if (Drawable* d = drawableAt(1)) {
            d->poison = m[3];
        }
        if (const auto it = self_.monitored.find(static_cast<std::uint16_t>(u16At(m, 1) & 0x7FFF));
            it != self_.monitored.end()) {
            it->second.poison = m[3]; // Cli_SetMonitoredCreaturePoison
        }
        if (const auto n = npcs_.find(static_cast<std::uint16_t>(u16At(m, 1) & 0x7FFF)); n != npcs_.end()) {
            n->second.poison = m[3]; // Cli_SetNpcPoison
        }
        break;
    case Op::ReportMonitorCreature:
        // CliNet_OnPacket case 0xDB: created if unknown, and marked for the minimap; its health kept.
        drawables_.try_emplace(u16At(m, 1), Drawable{.netCode = u16At(m, 1), .typeId = u16At(m, 3)})
            .first->second.monitored = true;
        self_.monitored.try_emplace(static_cast<std::uint16_t>(u16At(m, 1) & 0x7FFF));
        break;
    case Op::ReportUnmonitorCreature:
        if (Drawable* d = drawableAt(1)) {
            d->monitored = false;
        }
        self_.monitored.erase(static_cast<std::uint16_t>(u16At(m, 1) & 0x7FFF));
        break;
    case Op::ReportCreatureCmd:
        self_.creatureCommand = m[1]; // Gui_Summon_SetCommandIcon
        break;
    case Op::DoorAngle:
        // Doors are map objects the client loads itself, often with no drawable here.
        game_.doorAngles[u16At(m, 1)] = m[3];
        if (Drawable* d = drawableAt(1)) {
            d->special = m[3];
        }
        break;
    case Op::ObeliskCharge:
    case Op::PentagramActivate: {
        // CliNet_OnPacket: an obelisk's charge (0-50) is its light, charge * 80 / 50, and its image,
        // charge * 8 / 50 short of 8. "Pentagram activate" is in fact a pressure plate's state
        // (SrvNet_SendSpriteState): pressed, it gets a light of 41.958 (in the type's
        // light colour, black for a plate) and frame 1. Pentagrams animate by MSG_DRAW_FRAME.
        const std::uint8_t value = m[3];
        const bool obelisk = m[0] == static_cast<std::uint8_t>(Op::ObeliskCharge);
        const float intensity = obelisk ? static_cast<float>(value * 80u / 50u) : value ? 41.958f : 0.0f;
        const auto image = static_cast<std::uint8_t>(obelisk ? std::min(value * 8u / 50u, 7u) : value);
        if (Drawable* d = drawableAt(1)) {
            d->special = value;
            d->frame = image;
            d->lightIntensity = intensity;
        }
        if (StaticObjectState* s = staticAt(1)) {
            s->frame = image;
            s->lightIntensity = intensity;
        }
        break;
    }
    case Op::ReportMundaneArmorEquip:
    case Op::ReportMundaneWeaponEquip:
    case Op::ReportModifiableWeaponEquip:
    case Op::ReportModifiableArmorEquip: {
        // bit 15 set: the player list entry (Cli_SetPlayerEquip); else the drawable (Cli_NpcAddEquipment).
        const std::uint16_t flags = u16At(m, 1);
        const std::uint16_t nc = flags & 0x7FFF;
        const bool armor = m[0] == static_cast<std::uint8_t>(Op::ReportMundaneArmorEquip) ||
                           m[0] == static_cast<std::uint8_t>(Op::ReportModifiableArmorEquip);
        const std::uint32_t bits = u32At(m, 3);
        const bool modifiable = m[0] == static_cast<std::uint8_t>(Op::ReportModifiableArmorEquip) ||
                                m[0] == static_cast<std::uint8_t>(Op::ReportModifiableWeaponEquip);
        // A mundane report passes 0xFFFFFFFF as the four ids (CliNet_OnPacket 0x4F / 0x50).
        const std::array<std::uint8_t, 4> mods = modifiable ? std::array<std::uint8_t, 4>{m[7], m[8], m[9], m[10]}
                                                            : std::array<std::uint8_t, 4>{0xFF, 0xFF, 0xFF, 0xFF};
        if (flags & 0x8000) {
            if (auto p = players_.find(nc); p != players_.end()) {
                (armor ? p->second.armorEquip : p->second.weaponEquip) |= bits;
                auto& byBit = armor ? p->second.armorModifiers : p->second.weaponModifiers;
                // Each class bit in the report gets the same four modifier ids.
                for (std::uint32_t bit = 1; bit != 0; bit <<= 1) {
                    if (bits & bit) {
                        byBit[bit] = mods;
                    }
                }
            }
        } else {
            // Cli_NpcAddEquipment: into the NPC's record, if it has one (the drawable
            // keeps the bits too, though nothing of the game's reads them there).
            if (auto n = npcs_.find(nc); n != npcs_.end()) {
                (armor ? n->second.armorEquip : n->second.weaponEquip) |= bits;
                (armor ? n->second.armorModifiers : n->second.weaponModifiers).try_emplace(bits, mods);
            }
            if (auto d = drawables_.find(nc); d != drawables_.end()) {
                (armor ? d->second.armorEquip : d->second.weaponEquip) |= bits;
            }
        }
        break;
    }
    case Op::ReportNpc: {
        // CliNet_OnPacket 0x69: the record by the code less bit 15, made (Cli_NewNpc) or wiped
        // (Cli_InitNpc: its worn things too); its six colours; its poison = bit 15.
        if (m.size() < 21) {
            break;
        }
        const std::uint16_t flags = u16At(m, 1);
        const auto code = static_cast<std::uint16_t>(flags & 0x7FFF);
        // The table holds 64 (g_cliNpcs) and frees none till a game's start; full, a new code
        // gets no record and isn't drawn (an original bug).
        if (!npcs_.contains(code) && kNpcSlots <= npcs_.size()) {
            break;
        }
        NpcEntry& npc = npcs_[code];
        npc = NpcEntry{};
        // Six RGB triples from +3.
        for (std::size_t i = 0; i < npc.colors.size(); ++i) {
            npc.colors[i] = {m[3 + i * 3], m[4 + i * 3], m[5 + i * 3]};
        }
        npc.poison = flags >> 15;
        break;
    }
    case Op::ReportArmorDequip:
    case Op::ReportWeaponDequip: {
        const std::uint16_t nc = u16At(m, 1) & 0x7FFF;
        const bool armor = m[0] == static_cast<std::uint8_t>(Op::ReportArmorDequip);
        const std::uint32_t bits = u32At(m, 3);
        if (auto p = players_.find(nc); p != players_.end()) {
            (armor ? p->second.armorEquip : p->second.weaponEquip) &= ~bits;
            auto& byBit = armor ? p->second.armorModifiers : p->second.weaponModifiers;
            std::erase_if(byBit, [bits](const auto& e) { return (e.first & bits) != 0; });
        }
        if (auto d = drawables_.find(nc); d != drawables_.end()) {
            (armor ? d->second.armorEquip : d->second.weaponEquip) &= ~bits;
        }
        break;
    }

    // ---- walls
    case Op::DestroyWall:
        game_.destroyedWalls.insert(u16At(m, 1));
        ++game_.wallsVersion;
        break;
    case Op::OpenWall:
        game_.openWalls.insert(u16At(m, 1));
        game_.closedWalls.erase(u16At(m, 1));
        ++game_.wallsVersion;
        break;
    case Op::CloseWall:
        game_.openWalls.erase(u16At(m, 1));
        game_.closedWalls.insert(u16At(m, 1));
        ++game_.wallsVersion;
        break;

    // ---- you
    case Op::ReportPlayerHealth:
        self_.health = u16At(m, 1);
        break;
    case Op::ReportTotalHealth:
        if (u16At(m, 1) == self_.netCode) {
            self_.health = u16At(m, 3);
            self_.maxHealth = u16At(m, 5);
        }
        if (Drawable* d = drawableAt(1)) {
            d->health = u16At(m, 3);
            d->maxHealth = u16At(m, 5);
        }
        if (const auto it = self_.monitored.find(static_cast<std::uint16_t>(u16At(m, 1) & 0x7FFF));
            it != self_.monitored.end() && u16At(m, 1) != self_.netCode) {
            it->second.health = u16At(m, 3); // Cli_SetMonitoredCreatureHealth
            it->second.maxHealth = u16At(m, 5);
        }
        break;
    case Op::ReportMana:
        if (u16At(m, 1) == self_.netCode) {
            self_.mana = u16At(m, 3);
        }
        break;
    case Op::ReportTotalMana:
        if (u16At(m, 1) == self_.netCode) {
            self_.mana = u16At(m, 3);
            self_.maxMana = u16At(m, 5);
        }
        break;
    case Op::ReportHealthDelta:
        if (m.size() >= 5) {
            // {u16 netCode, i16 delta}; kept as long as the client draws one (31 frames).
            std::erase_if(floaters_, [frame](const HealthFloater& f) { return frame - f.frame >= 31; });
            floaters_.push_back({u16At(m, 1), static_cast<std::int16_t>(u16At(m, 3)), frame});
        }
        break;
    case Op::ReportStamina:
        self_.stamina = m[1];
        self_.staminaFrame = frame; // (only spending is reported: LocalPlayer::staminaAt)
        break;
    case Op::ReportStats:
        if (u16At(m, 1) == self_.netCode) {
            self_.maxHealth = u16At(m, 3);
            self_.maxMana = u16At(m, 5);
            self_.carryCapacity = u16At(m, 7);
            self_.speed = u16At(m, 9);
            self_.strength = u16At(m, 11);
            self_.level = m[13];
        }
        break;
    case Op::ReportArmorValue:
        self_.armorValue = f32At(m, 1);
        break;
    case Op::ReportGold:
        self_.gold = u32At(m, 1);
        break;
    case Op::ReportExperience:
        self_.experience = u32At(m, 1);
        break;
    case Op::ReportItemEnchantment:
        self_.itemBuffs = m[1];
        break;
    case Op::ReportPlayerStatus:
        self_.statusFlags = u32At(m, 1);
        break;
    case Op::StatMultipliers:
        for (int i = 0; i < 4; ++i) {
            self_.statMultipliers[i] = f32At(m, 1 + 4 * i);
        }
        break;
    case Op::ReportAbilityAward:
        // Cli_SetAbilityLevel: a new level other than 0 shows the award; bit 7 opens
        // the book on it and flies it to the bar.
        if (m[1] >= 1 && m[1] < self_.abilityLevel.size() && self_.abilityLevel[m[1]] != (m[2] & 0x7F)) {
            self_.abilityLevel[m[1]] = m[2] & 0x7F;
            if (m[2] & 0x7F) {
                const bool flag = (m[2] & 0x80) != 0;
                queueBook({frame, BookEvent::Kind::Ability, m[1], flag, flag});
            }
        }
        break;
    case Op::ReportAbilityState:
        if (m[1] < self_.abilityReady.size()) {
            self_.abilityReady[m[1]] = m[2] != 0;
            self_.abilityCooldownStart[m[1]] = m[2] != 0 ? 0 : frame;
        }
        break;
    case Op::InfoBookData:
        // The answer to a book's tooltip asking what it holds (CliNet_OnPacket case 0xE2).
        self_.bookContents[static_cast<std::uint16_t>(u16At(m, 1) & 0x7FFF)] = m[3];
        break;
    case Op::ReportActiveAbilities:
        if (m[1] < 8) {
            const auto bit = static_cast<std::uint8_t>(1u << m[1]);
            self_.activeAbilities = m[2] ? (self_.activeAbilities | bit) : (self_.activeAbilities & ~bit);
        }
        break;
    case Op::ResetAbilities:
        for (std::size_t i = 0; i < self_.abilityReady.size(); ++i) {
            // 6 means every ability.
            if (m[1] == 6 || m[1] == i) {
                self_.abilityReady[i] = true;
                self_.abilityCooldownStart[i] = 0;
            }
        }
        break;
    case Op::ReportSpellAward:
        // Cli_AwardSpell: flags & 0x7F open the book on it, bit 7 flies it to the bar.
        setSpellLevel(m[1], m[2]);
        queueBook({frame, BookEvent::Kind::Spell, m[1], (m[3] & 0x7F) != 0, (m[3] & 0x80) != 0});
        break;
    case Op::ReportGuideAward:
        // Cli_AwardGuideCreature: the creature and its group known (1, whatever the byte, which only
        // opens the book: a quiet award is known too).
        self_.guideLevels[m[1]] = 1;
        for (const int linked : things::guideGroup(m[1])) {
            self_.guideLevels[static_cast<std::uint8_t>(linked)] = 1;
        }
        queueBook({frame, BookEvent::Kind::Creature, m[1], (m[2] & 0x7F) != 0, false});
        break;
    case Op::ReportSecondaryWeapon:
        self_.secondaryWeapon = u16At(m, 1);
        break;
    case Op::ReportLastQuiver:
        self_.lastQuiver = u16At(m, 1);
        break;
    case Op::ReportAcquireCreature: {
        // Cli_Summon_AddCreature: into the cage's first free slot (none if it's full),
        // the cage placed again; it comes up with the first one, unless the message says quietly
        // (the type's bit 15). Then monitored and on the minimap (CliNet_OnPacket case 0x6C).
        const auto nc = static_cast<std::uint16_t>(u16At(m, 1) & 0x7FFF);
        const auto typeId = static_cast<std::uint16_t>(u16At(m, 3) & 0x7FFF);
        const bool listed = std::any_of(self_.summons.begin(), self_.summons.end(), [&](const auto& s) {
            return s && s->netCode == nc && s->typeId == typeId;
        });
        const auto free = std::find_if(self_.summons.begin(), self_.summons.end(), [](const auto& s) { return !s; });
        if (!listed && free != self_.summons.end()) {
            const bool first = self_.summonCount() == 0;
            std::uint8_t size = 4;
            if (things_ && typeId >= 1 && typeId <= things_->things.size()) {
                const auto sub = things_->things[typeId - 1u].attribute("SUBCLASS");
                const std::string_view sv = sub ? *sub : std::string_view{};
                size = sv.find("SMALL_MONSTER") != std::string_view::npos    ? 1
                       : sv.find("MEDIUM_MONSTER") != std::string_view::npos ? 2
                                                                              : 4;
            }
            *free = SummonSlot{.netCode = nc, .typeId = typeId, .size = size};
            placeSummons(self_);
            if (first && !(u16At(m, 3) & 0x8000)) {
                queueAudio({frame, AudioCue::Kind::Local, 0x321, 100, 0}); // CreatureCageAppears
            }
        }
        drawables_.try_emplace(nc, Drawable{.netCode = nc, .typeId = typeId}).first->second.monitored = true;
        self_.monitored.try_emplace(nc);
        break;
    }
    case Op::ReportLoseCreature: {
        // Cli_Summon_RemoveCreature: out of its slot, the cage placed again; the cage
        // goes with the last one (unless quiet: bit 15). No longer monitored.
        const auto nc = static_cast<std::uint16_t>(u16At(m, 1) & 0x7FFF);
        const auto it = std::find_if(self_.summons.begin(), self_.summons.end(),
                                     [&](const auto& s) { return s && s->netCode == nc; });
        if (it != self_.summons.end()) {
            it->reset();
            placeSummons(self_);
            if (self_.summonCount() == 0 && !(u16At(m, 1) & 0x8000)) {
                queueAudio({frame, AudioCue::Kind::Local, 0x322, 100, 0}); // CreatureCageHides
            }
        }
        self_.monitored.erase(nc);
        if (Drawable* d = drawableAt(1)) {
            d->monitored = false;
        }
        break;
    }

    // ---- inventory (spritePickup and friends)
    case Op::ReportPickup:
    case Op::ReportModifiablePickup: {
        InventoryItem& item = self_.inventory[u16At(m, 1)];
        item.netCode = u16At(m, 1);
        item.typeId = u16At(m, 3);
        item.pickupOrder = ++pickups_;
        if (m[0] == static_cast<std::uint8_t>(Op::ReportModifiablePickup)) {
            item.modifiers = {m[5], m[6], m[7], m[8]};
        }
        break;
    }
    case Op::ReportDrop:
        self_.inventory.erase(u16At(m, 1));
        break;
    case Op::ReportEquip:
    case Op::ReportDequip:
        if (auto it = self_.inventory.find(u16At(m, 1)); it != self_.inventory.end()) {
            it->second.equipped = m[0] == static_cast<std::uint8_t>(Op::ReportEquip);
        }
        break;
    case Op::ReportItemHealth:
        if (auto it = self_.inventory.find(u16At(m, 1)); it != self_.inventory.end()) {
            it->second.health = u16At(m, 3);
            it->second.maxHealth = u16At(m, 5);
        }
        break;
    case Op::ReportCharges:
        if (auto it = self_.inventory.find(u16At(m, 1)); it != self_.inventory.end()) {
            it->second.charges = m[3];
            it->second.maxCharges = m[4];
        }
        break;
    case Op::ReportModifier: {
        // Items in our inventory, and any drawable (ground items): the client stores the four
        // modifiers on the drawable too (cli_decode case 0x67; bit 15 marks a static object, which
        // our drawable keys keep).
        const std::array<std::uint8_t, 4> mods{m[3], m[4], m[5], m[6]};
        if (auto it = self_.inventory.find(u16At(m, 1)); it != self_.inventory.end()) {
            it->second.modifiers = mods;
        }
        if (auto d = drawables_.find(u16At(m, 1)); d != drawables_.end()) {
            d->second.modifiers = mods;
        } else if (!(u16At(m, 1) & 0x8000)) {
            keepEarly(u16At(m, 1)).modifiers = mods; // (ahead of the update that makes it: early_)
        }
        break;
    }
    case Op::ReportInventoryLoaded:
        self_.inventoryLoaded = true;
        break;

    // ---- markers and magic walls
    case Op::InterestingId: {
        // netCode (bit 15 = static), typeId, add (1: creates the drawable if unknown) or remove,
        // the kind: its bit in or out of the drawable's (CliNet_OnPacket's 0xD2: Cli_AddMinimapMarker
        // ORs it in; Cli_RemoveMinimapMarker takes it out of a known one, the others kept).
        if (m[5] == 1) {
            auto [it, created] = drawables_.try_emplace(u16At(m, 1));
            if (created) {
                it->second.netCode = u16At(m, 1);
                it->second.typeId = u16At(m, 3);
            }
            it->second.interestKinds |= m[6];
        } else if (auto it = drawables_.find(u16At(m, 1)); it != drawables_.end()) {
            it->second.interestKinds &= static_cast<std::uint8_t>(~m[6]);
        }
        break;
    }
    case Op::ChangeOrAddWallMagic:
        // Layout: tile +1, direction +2, variation +3, grid cell x +4, y +5.
        game_.magicWalls[{m[4], m[5]}] = MagicWall{m[1], m[2], m[3]};
        ++game_.wallsVersion;
        break;
    case Op::RemoveWallMagic:
        game_.magicWalls.erase({m[1], m[2]});
        ++game_.wallsVersion;
        break;
    case Op::TextMessage:
        chatText(m, frame);
        break;
    case Op::ChatKill:
        // 0xDEAD clears every bubble.
        if (u16At(m, 1) == 0xDEAD) {
            bubbles_.clear();
        } else {
            bubbles_.erase(u16At(m, 1));
        }
        break;
    default:
        break;
    }
}

void World::chatText(std::span<const std::uint8_t> m, std::uint32_t frame)
{
    // CliNet_OnPacket 0xA8: a notice (flag 0x10) is a dialog and speaker 0 a centred message;
    // anyone else's line gets a bubble (Cli_ShowChatBubble), unless the speaker is a player the
    // console's mute muted (playerCanTalkMB: status bit 8, set only locally; the
    // renderer knows who's muted).
    constexpr std::uint8_t kTeam = 0x01;
    constexpr std::uint8_t kNarrow = 0x02;
    constexpr std::uint8_t kStringId = 0x08;
    constexpr std::uint8_t kNotice = 0x10;
    constexpr std::uint32_t kFps = 30;
    if (m.size() < 11) {
        return;
    }
    // Layout: speaker +1, flags +3, x +4, y +6, length +8, duration +9 (u16), text from +11.
    const std::uint16_t speaker = u16At(m, 1);
    const std::uint8_t flags = m[3];
    if (speaker == 0 || (flags & kNotice)) {
        return;
    }
    std::erase_if(bubbles_, [&](const auto& b) { return b.second.expires <= frame; });
    const auto [it, created] = bubbles_.try_emplace(speaker);
    ChatBubble& b = it->second;
    if (created) {
        b.order = bubbleOrder_++;
    }
    const std::size_t size = m[8];
    if (flags & (kNarrow | kStringId)) {
        b.text = asciiAt(m, 11, size);
        if (flags & kStringId) {
            b.text = "[" + b.text + "]";
        }
    } else {
        b.text = utf16At(m, 11, size);
    }
    b.x = u16At(m, 4);
    b.y = u16At(m, 6);
    b.team = (flags & kTeam) && !(flags & kStringId); // (a string id gets no prefix)
    // An explicit duration in frames, else 2 seconds plus one per 8 characters, up to 8 more.
    const std::uint16_t duration = u16At(m, 9);
    b.expires = frame + (duration != 0 ? duration : static_cast<std::uint32_t>(std::min<std::size_t>(size / 8, 8) + 2) * kFps);
}

void World::tradeMessage(std::span<const std::uint8_t> m)
{
    // S->C TRADE sub-ops (client handlers in cli_decode 0xC9).
    const auto mods = [&](std::size_t off) { return std::array<std::uint8_t, 4>{m[off], m[off + 1], m[off + 2], m[off + 3]}; };
    auto& t = trade_;
    switch (m[1]) {
    case 0x01: // close P2P trade
    case 0x02: // close shop
        t = TradeState{};
        break;
    case 0x03:
        t.accepted = m[2];
        break;
    case 0x04: { // add item: side, typeId, netCode, value, modifiers
        TradeItem item{u16At(m, 5), u16At(m, 3), u32At(m, 7), 0, mods(11)};
        (m[2] == 1 ? t.ourOffer : t.theirOffer).push_back(item);
        break;
    }
    case 0x05: { // remove item
        const std::uint16_t nc = u16At(m, 2);
        std::erase_if(t.ourOffer, [&](const TradeItem& i) { return i.netCode == nc; });
        std::erase_if(t.theirOffer, [&](const TradeItem& i) { return i.netCode == nc; });
        break;
    }
    case 0x06:
        t.gold = {u32At(m, 2), u32At(m, 6), u32At(m, 10)};
        break;
    case 0x07: // prepare P2P
    case 0x0C: // start P2P: partner name
        t = TradeState{};
        t.kind = TradeState::Kind::Player;
        if (m[1] == 0x0C) {
            t.partner = utf16At(m, 2, 25);
        }
        break;
    case 0x08: // shop: add item (typeId, netCode, price, durability, modifiers)
        t.shopItems.push_back(TradeItem{u16At(m, 4), u16At(m, 2), u32At(m, 6), u32At(m, 10), mods(14)});
        break;
    case 0x09: {
        const std::uint16_t nc = u16At(m, 2);
        std::erase_if(t.shopItems, [&](const TradeItem& i) { return i.netCode == nc; });
        break;
    }
    case 0x0D: { // start shop: shopkeeper type, name, greeting
        const auto counts = std::array{t.shortOfGoldCount, t.sellOfferCount, t.repairOfferCount};
        t = TradeState{};
        t.shortOfGoldCount = counts[0];
        t.sellOfferCount = counts[1];
        t.repairOfferCount = counts[2];
        t.kind = TradeState::Kind::Shop;
        t.shopkeeperType = u16At(m, 2);
        t.partner = utf16At(m, 4, 25);
        t.greeting = asciiAt(m, 54, 32);
        break;
    }
    case 0x1B:
        t.shortOfGold = u16At(m, 2);
        ++t.shortOfGoldCount;
        queueAudio({applyFrame_, AudioCue::Kind::Local, 0x39D, 100, 0}); // NoCanDo (Gui_Shop_ShowNotEnoughGold)
        break;
    case 0x1D:
        t.sellOffer = std::pair{u16At(m, 2), u32At(m, 4)};
        ++t.sellOfferCount;
        break;
    case 0x1F:
        t.repairOffer = std::pair{u16At(m, 2), u32At(m, 4)};
        ++t.repairOfferCount;
        break;
    default:
        break;
    }
}

void World::dialogMessage(std::span<const std::uint8_t> m)
{
    if (m[1] == 0x03) { // open: textId, title, sound, portrait, flags
        dialog_.open = true;
        dialog_.textId = asciiAt(m, 2, 32);
        dialog_.title = utf16At(m, 34, 32);
        dialog_.sound = u32At(m, 98);
        dialog_.portrait = asciiAt(m, 102, 32);
        dialog_.flags = m[134];
        ++dialog_.count;
    } else if (m[1] == 0x04) {
        const auto count = dialog_.count;
        dialog_ = DialogState{};
        dialog_.count = count;
    }
}

// MSG_GAUNTLET: byte 1 the sub-op; each needs the bytes it reads.
void World::gauntletMessage(std::span<const std::uint8_t> m, std::uint32_t frame)
{
    if (m.size() < 2) {
        return;
    }
    switch (m[1]) {
    case 0x01:
        queueAudio({frame, AudioCue::Kind::Local, 0x3F0, 100, 0}); // QuestPlayerJoinGame
        break;
    case 0x02:
        if (m.size() >= 8) {
            // Gui_GameOver_Show: QuestFinalDeath, and GGOver.wnd with the tallies.
            queueAudio({frame, AudioCue::Kind::Local, 0x3EF, 100, 0});
            auto& over = game_.questGameOver;
            over.generators = u16At(m, 2);
            over.secrets = u16At(m, 4);
            over.kills = u16At(m, 6);
            over.frame = frame;
            ++over.count;
        }
        break;
    case 0x0F:
        if (m.size() >= 4) {
            // A static drawable's lock mark cleared (Cli_GetStaticDrawableByNetCode: the extent as
            // sent): a quest door opened.
            game_.statics[static_cast<std::uint16_t>(u16At(m, 2) & 0x7FFF)].lockMark = false;
        }
        break;
    case 0x10:
    case 0x19:
    case 0x1A:
        // The GreenZap (a GreenBoltDraw drawable between two points), a warp's explosion
        // (FireBoom and GreenSpark explosion sparks) and a spark burst (GreenSpark): for the
        // effects, as the FX messages.
        queueEffect(m, frame);
        break;
    case 0x16:
    case 0x17:
        if (m.size() >= 5) {
            // A player's SilverKey / GoldKey held (byte 2: whether it's held).
            if (const auto it = players_.find(u16At(m, 3)); it != players_.end()) {
                (m[1] == 0x16 ? it->second.silverKey : it->second.goldKey) = m[2];
            }
        }
        break;
    case 0x1D:
        if (m.size() >= 4) {
            self_.questStage = u16At(m, 2); // (Gui_Inv_SetQuestStage: g_cliQuestStage)
        }
        break;
    case 0x18:
        if (m.size() >= 3) {
            // Gui_SetSKeyWindowState: off to on shows SKey.wnd and plays
            // EnableSharedKeyMode; on to off hides it; anything else does nothing.
            if (!game_.sharedKeys && m[2] == 1) {
                game_.sharedKeys = true;
                ++game_.sharedKeysShown;
                queueAudio({frame, AudioCue::Kind::Local, 0x3FE, 100, 0});
            } else if (game_.sharedKeys && m[2] == 0) {
                game_.sharedKeys = false;
            }
        }
        break;
    case 0x04:
        if (m.size() >= 5) {
            // A player's extra lives (CliNet_OnPacket; ours, Gui_Inv_SetQuestLives too).
            const std::uint16_t code = u16At(m, 3);
            if (const auto it = players_.find(code); it != players_.end()) {
                it->second.questLives = m[2];
            }
            if (code == self_.netCode) {
                self_.questLives = m[2];
            }
        }
        break;
    case 0x11:
    case 0x12:
    case 0x13: {
        // A quest death's penalty: a spell, ability or guide creature taken away
        // (Cli_RevokeSpell, Cli_RevokeAbility, Cli_RevokeGuideCreature).
        if (m.size() < 4) {
            break;
        }
        const auto id = static_cast<std::uint8_t>(u16At(m, 2));
        if (m[1] == 0x11) {
            setSpellLevel(id, 0);
            queueBook({frame, BookEvent::Kind::RevokeSpell, id, false, false});
        } else if (m[1] == 0x12) {
            if (id < self_.abilityLevel.size()) {
                self_.abilityLevel[id] = 0;
            }
            queueBook({frame, BookEvent::Kind::RevokeAbility, id, false, false});
        } else {
            self_.guideLevels.erase(id);
            queueBook({frame, BookEvent::Kind::RevokeCreature, id, false, false});
        }
        break;
    }
    default:
        break;
    }
}

void World::resetOutfit(PlayerEntry& p, std::uint8_t itemMask)
{
    // Cli_ResetPlayerDefaultOutfit: what a player starts with, put on by the client
    // itself (the server doesn't report it to the player): the street clothes in the player's
    // colours (UserColor1 + index as their material modifier), and in deathmatch a wizard's robe
    // or a warrior's longsword and wooden shield; in a quest the class's weapon (a sword, a
    // flare wand with Replenishment1, a bow), in a solo game its sword or wooden staff.
    // `itemMask` picks which (0xFF: all; the quest's weapons regardless).
    constexpr std::uint32_t kSolo = 0x800, kQuest = 0x1000;
    const bool solo = (game_.gameFlags & kSolo) != 0;
    const bool quest = (game_.gameFlags & kQuest) != 0;
    p.armorEquip = 0;
    p.weaponEquip = 0;
    p.armorModifiers.clear();
    p.weaponModifiers.clear();
    const auto color = [&](std::uint8_t index) -> std::uint8_t {
        return userColorBase_ ? static_cast<std::uint8_t>(*userColorBase_ + index) : 0xFF;
    };
    const std::array<std::uint8_t, 4> none{0xFF, 0xFF, 0xFF, 0xFF};
    const auto armor = [&](std::uint32_t bit, const std::array<std::uint8_t, 4>& mods) {
        p.armorEquip |= bit;
        p.armorModifiers[bit] = mods;
    };
    const auto weapon = [&](std::uint32_t bit, const std::array<std::uint8_t, 4>& mods) {
        p.weaponEquip |= bit;
        p.weaponModifiers[bit] = mods;
    };
    // clothing: pants, shirt 1, shirt 2, shoes 1, shoes 2.
    // Player classes: 0 warrior, 1 wizard, 2 conjurer; a warrior wears no shirt outside solo games.
    if ((p.playerClass != 0 || solo) && (itemMask & 1)) {
        armor(0x400, {0xFF, color(p.clothing[1]), color(p.clothing[2]), 0xFF}); // StreetShirt
    }
    if (itemMask & 2) {
        armor(0x4, {0xFF, color(p.clothing[0]), 0xFF, 0xFF}); // StreetPants
    }
    if (itemMask & 4) {
        armor(0x1, {color(p.clothing[4]), color(p.clothing[3]), 0xFF, 0xFF}); // StreetSneakers
    }
    const auto [armorQuality1, material1, replenishment1] = outfitModifiers_;
    switch (p.playerClass) {
    case 1: // wizard
        if (!solo) {
            if (quest) {
                weapon(0x10000, {0xFF, 0xFF, replenishment1, 0xFF}); // SulphorousFlareWand (no mask test)
            } else if (itemMask & 0x10) {
                armor(0x4000, none); // WizardRobe
            }
        } else if (itemMask & 8) {
            weapon(0x8000, {armorQuality1, 0xFF, 0xFF, 0xFF}); // StaffWooden
        }
        break;
    case 0: // warrior
        if (!solo) {
            if (!quest) {
                if (itemMask & 0x40) {
                    weapon(0x200, none); // Longsword
                }
                if (itemMask & 0x80) {
                    armor(0x1000000, none); // WoodenShield
                }
            } else {
                weapon(0x100, none); // Sword (no mask test)
            }
        } else if (itemMask & 0x20) {
            // Sword: the exe writes ArmorQuality1 and then Material1 into the same first byte
            // (an original bug); the server's has both.
            (void)armorQuality1;
            weapon(0x100, {material1, 0xFF, 0xFF, 0xFF});
        }
        break;
    case 2: // conjurer
        if (!solo) {
            if (quest) {
                weapon(0x4, none); // Bow (no mask test)
            }
        } else if (itemMask & 8) {
            weapon(0x8000, {armorQuality1, 0xFF, 0xFF, 0xFF}); // StaffWooden
        }
        break;
    default:
        break;
    }
}

std::uint8_t npcPlayerState(std::uint8_t monsterState, std::uint32_t weaponEquip, std::uint32_t armorEquip,
                            std::uint32_t animFrame, std::uint8_t& choice, const std::function<int(int, int)>& roll)
{
    // The exe's strike table: the strike for weapon class bit n (2-26), the first one worn.
    static constexpr std::array<std::uint8_t, 27> kStrike = {
        0,    0,    0x21, 0x22, 0,    0,    0x2C, 0x2C, 0x1B, 0x1C, 0x25, 0x1A, 0x23, 0x27,
        0x27, 0x1D, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x20, 0x20, 0x20, 0x1F};
    constexpr std::uint32_t kGreatSword = 0x400;
    constexpr std::uint32_t kStaves = 0x7FF8000;
    constexpr std::uint32_t kShields = 0x3000000;
    const std::uint8_t saved = choice;
    choice = 0;
    const auto pick = [&](int low, int high) {
        choice = (saved == 0 || animFrame == 0) ? static_cast<std::uint8_t>(roll(low, high)) : saved;
        return choice;
    };
    switch (monsterState) {
    case 1: // ATTACK, ATTACK_FINISH, ATTACK_FAR, ATTACK_FAR_FINISH
    case 2:
    case 3:
    case 4:
        if (const std::uint32_t weapons = weaponEquip & ~3u) {
            for (std::size_t bit = 2; bit < kStrike.size(); ++bit) {
                if (weapons & (1u << bit)) {
                    return kStrike[bit];
                }
            }
            return 0;
        }
        return pick(0x17, 0x18); // PUNCH_LEFT, PUNCH_RIGHT
    case 5: // DEFEND, DEFEND_FINISH
    case 6:
        if (weaponEquip & kGreatSword) {
            return pick(0x2F, 0x31); // GREAT_SWORD_BLOCK_LEFT / RIGHT / DOWN
        }
        if (weaponEquip & kStaves) {
            return 0x1E; // STAFF_BLOCK
        }
        return (armorEquip & kShields) ? 0x28 : 0; // RAISE_SHIELD
    case 7:
        return 0x15; // CAST_SPELL
    case 8:
        return (weaponEquip & kGreatSword) ? 0x26 : 0; // GREAT_SWORD_IDLE
    case 9:
        return 1; // DIE
    case 10:
        return 2; // DEAD
    case 12:
        return 4; // WALK
    case 13:
        return 6; // RUN
    case 14:
        return 0x32; // ELECTRIC_ZAP
    default:
        return 0;
    }
}

} // namespace nox::client
