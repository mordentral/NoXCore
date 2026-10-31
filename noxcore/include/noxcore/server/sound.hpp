// The server's sounds: what the game logic plays, queued for the frame, and sent to each
// player in earshot as the exe does.
//
//   - Played (aud at an object, audCreate at a point): not in a
//     game flagged 0x80000, not "NULL" (0), not a sound without files; the event keeps where it
//     was, its object (none once destroyed), and who hears it: mode 0 everyone, 1 a team (`arg`
//     its id), 2 one player (`arg` its net code). Not to one player (2), one from a creature or
//     player (or what's theirs) with a hearing is noted for the monsters' ears 2 frames (
//     their hearing, in monsters.cpp).
//   - Sent (netUpdateRemotePlr, each frame each player): those it may hear, in the
//     polygon group its camera's in (or group 0 either side), loud enough where its
//     camera is ((range - distance) x 100 / range, the distance from the camera, 0
//     past the range); the player's own spell phonemes (0xBA-0xC1) not to it. MSG_AUDIO_EVENT
//     0xA6 (MSG_AUDIO_PLAYER_EVENT 0xA7 to the player that made it): the pan (its x off the
//     camera, x 50 / half the screen), the id | half the volume << 10 (SrvNet_SendAudioEventToPlayer).
//     A sound with a most at once (AUD's maxInstances) sends only its loudest that many to each
//     player (within 5 of each other, flag 0x10 puts the newer
//     first).
//   - The numbers (7 dwords a sound, from thing.bin: AUD, AVNT):
//     range (x 15; 600 when not given), hearing, volume, its files' count, flags (AUD: 2), the most
//     at once.
#pragma once

#include "noxcore/server/world.hpp"
#include "noxcore/things/thing_db.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace nox::server {

struct SoundInfo {
    int range = 600;      ///< units
    int hearing = 0;      ///< monsters hear it: any an enemy's, 0x10 a friend's; how
                          ///< loud they need it: 0x20 89, 0x40 20, else 50
    int volume = 0;
    int files = 0;        ///< none: it isn't played
    std::uint8_t flags = 0;
    int most = 0;         ///< at once to each player, 0 no limit
};

class SoundTable {
public:
    SoundTable() = default;
    explicit SoundTable(const things::ThingDb& things);
    const SoundInfo& operator[](std::uint32_t id) const { return info_[id < info_.size() ? id : 0]; }

private:
    std::array<SoundInfo, 1024> info_{};
};

/// aud: `id` at `source` (its position; it as the source while not destroyed).
void playSound(World& world, std::uint32_t id, const ServerObject& source, int mode = 0, std::uint32_t arg = 0);
/// audCreate: `id` at (x, y), no source.
void playSoundAt(World& world, std::uint32_t id, float x, float y, int mode = 0, std::uint32_t arg = 0);
/// By name (audio/sound_names.hpp); nothing for a name that isn't one.
void playSound(World& world, std::string_view name, const ServerObject& source, int mode = 0, std::uint32_t arg = 0);
void playSoundAt(World& world, std::string_view name, float x, float y, int mode = 0, std::uint32_t arg = 0);

/// A spell's SPEL sound (spellGetAud44): 0 cast, 1 on, 2 off; "" if none.
std::string_view spellSound(const things::ThingDb& things, std::uint32_t spell, int which);
/// An ability's ABIL sound: 0 its start, 1, 2 its end; "" if none.
std::string_view abilitySound(const things::ThingDb& things, int ability, int which);

/// Whether a sound has files (getSevenDwords3 > 0), by the world's table.
bool soundHasFiles(const World& world, std::uint32_t id);
/// A creature's sound from its sound set (its thing's name in soundset.bin), 0 for none.
std::uint32_t creatureSound(const World& world, const ServerObject& o, things::SoundSlot slot);

/// A spell phoneme's sound (spellGetPhoneme): phonemes 0-8 (4 none) in the
/// speaker's voice, a female player's (PlayerFemale) 0xC2-0xC9, anyone else's 0xBA-0xC1.
std::uint32_t phonemeSound(const ServerObject& speaker, int phoneme);

/// A creature's own sound from its set, at it (nothing when it has no set or no such sound).
void playCreatureSound(World& world, const ServerObject& o, things::SoundSlot slot);

/// A weapon dropped, at it: a staff's (class 0x1000) 0x33F, metal 0x34B, wood 0x34D.
void weaponDropSound(World& world, const ServerObject& item);
/// A piece of armour dropped, at it: metal 0x325, wood 0x32B, leather 0x328,
/// cloth 0x32E (0x331 a cloth subclass 0x20).
void armorDropSound(World& world, const ServerObject& item);
/// A weapon picked up, at its taker: 0x33E, 0x34A, 0x34C as dropping's.
void weaponPickupSound(World& world, const ServerObject& taker, const ServerObject& item);

/// A piece of armour picked up (ArmorPickup's end), at its taker: metal 0x324, wood 0x32A,
/// leather 0x327, cloth 0x32D (0x330 a cloth subclass 0x20).
void armorPickupSound(World& world, const ServerObject& taker, const ServerObject& item);

/// two materials meeting, at `at` (MATERIAL bits: the struck's, then the
/// striker's; 0x2000 a Material7 piece of armour's).
void materialSound(World& world, const ServerObject& at, std::uint16_t struck, std::uint16_t by);
/// a thing of `material` damaged (wood, stone, earth, snow, cloth...).
void damagedSound(World& world, const ServerObject& at, std::uint16_t material);
/// The damage sound of a blow, as DefaultDamage plays it: none when what struck is the
/// thing itself and it's a weapon or a wand, nor when the attacker is a creature whose set has a
/// MELEE_ATTACK_HITTING sound with files (that plays instead); else its DAMAGESOUND,
/// PlayerDamageSound (its body armour's material against the striker's) or
/// DefaultDamageSound (by the two materials, or a breakable's own), with the thing that
/// struck (obj3, else the source).
void damageSound(World& world, const things::ModifierDb* modifiers, ServerObject& t, ServerObject* src,
                 ServerObject* obj3);
/// DefaultDamageSound alone: `t` struck by `by` (its last blow's type, t.hurtType).
void defaultDamageSound(World& world, const ServerObject& t, const ServerObject* by);

/// how loud (0-100) a sound at (x, y) is at (atX, atY), by its range.
int soundLoudness(const SoundInfo& info, float x, float y, float atX, float atY);

/// The listener for one player's sends (made for the frame's sends: its key, not a stored link).
struct SoundListener {
    ObjectKey extent = 0;      ///< its unit
    std::uint8_t team = 0;
    float camX = 0;            ///< its camera (PlayerControl::camX / camY)
    float camY = 0;
    std::uint8_t group = 0;    ///< its polygon group
    float halfWidth = 320;     ///< half its screen (the pan's scale)
    bool hearsOwnPhonemes = false;
};

/// The frame's MSG_AUDIO_EVENT / MSG_AUDIO_PLAYER_EVENT messages for one player, concatenated.
std::vector<std::uint8_t> soundsFor(World& world, const SoundTable& table, const SoundListener& listener);
/// The same into `out` (emptied first: the caller's buffer, kept to save its allocations).
void soundsFor(World& world, const SoundTable& table, const SoundListener& listener, std::vector<std::uint8_t>& out);

/// The polygon group a sound is in: its source player's, a monster's polygon's,
/// else the polygon at the point's (0 none).
std::uint8_t soundGroup(World& world, const SoundEvent& e);

} // namespace nox::server
