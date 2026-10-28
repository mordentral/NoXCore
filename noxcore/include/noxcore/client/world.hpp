// The client's view of the game, built from the server's messages the way the 1.2b client's
// decoder (CliNet_OnPacket) builds it: drawables, players, teams, your own stats and
// inventory, walls and game state. No rendering; milestone 5 draws this.
//
// netCodes are kept as sent: bit 15 set = static (map-placed) object, which the client keeps in
// a separate table from dynamic objects, so the same low bits can name two different objects.
#pragma once

#include "noxcore/client/fade.hpp"
#include "noxcore/client/sight.hpp"
#include "noxcore/net/fx.hpp"
#include "noxcore/net/update_stream.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace nox::client {

/// An object the client knows about.
struct Drawable {
    std::uint16_t netCode = 0;
    std::uint16_t typeId = 0; ///< thing.bin THNG index + 1
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0; ///< height (REPORT_Z_PLUS/MINUS)
    std::uint8_t anim = 0;      ///< as sent: bits 4-6 direction, low nibble state (monsters)
    std::uint8_t direction = 0; ///< the client's AnimDir: 0-3, 5-8 (a 3x3 grid without the centre)
    std::uint8_t animState = 0;
    std::optional<std::uint8_t> frame;
    std::uint32_t lastSeenFrame = 0;
    /// Where it was at the update before (and that update's frame): how fast it's moving.
    std::int32_t prevX = 0;
    std::int32_t prevY = 0;
    std::uint32_t prevFrame = 0;
    /// The last update's frame at which it had moved since the update before (0: never).
    std::uint32_t movedFrame = 0;
    /// Moving enough to be seen while invisible (under 4 units each way since its
    /// last update it isn't drawn for others).
    bool movingVisibly() const noexcept { return std::abs(x - prevX) >= 4 || std::abs(y - prevY) >= 4; }
    /// Its speed in units a frame over its last update (the ghost's alpha).
    float speed() const noexcept
    {
        const float dx = static_cast<float>(x - prevX);
        const float dy = static_cast<float>(y - prevY);
        const auto frames = static_cast<float>(lastSeenFrame > prevFrame ? lastSeenFrame - prevFrame : 1);
        return std::sqrt(dx * dx + dy * dy) / frames;
    }
    bool enabled = true;
    std::uint32_t buffs = 0;   ///< enchantment bits (bit n = ENCHANT n)
    std::uint32_t xStatus = 0; ///< REPORT_X_STATUS
    std::optional<std::uint16_t> health;
    std::optional<std::uint16_t> maxHealth;
    std::uint8_t poison = 0;
    std::optional<std::array<std::uint8_t, 3>> lightColor;
    std::optional<float> lightIntensity;
    std::optional<std::uint8_t> special; ///< door angle, obelisk charge or pressure-plate state
    std::uint32_t armorEquip = 0;        ///< equipped class bits drawn on this object
    std::uint32_t weaponEquip = 0;
    /// An item's modifier ids (REPORT_MODIFIER): effectiveness, material, primary and secondary
    /// enchantment; 0xFF none. They recolour it on the ground as they do when worn.
    std::optional<std::array<std::uint8_t, 4>> modifiers;
    /// Marked "interesting" (INTERESTING_ID): the marker kinds on it as bits (
    /// Cli_AddMinimapMarker ORs a kind in, Cli_RemoveMinimapMarker takes it
    /// out; 1 Tag's, 2 a camper's); on the minimap while any is left. 0 if none.
    std::uint8_t interestKinds = 0;
    /// A creature we're shown (MSG_REPORT_MONITOR_CREATURE, until UNMONITOR or LOSE_CREATURE):
    /// the game puts it on the minimap-marker list.
    bool monitored = false;
    /// MSG_CLIENT_PREDICT_LINEAR (cleansing flames): the server sends it once and no more
    /// updates; the client moves it itself (sprite, World::predict).
    struct Linear {
        std::int32_t startX = 0, startY = 0;
        float vx = 0, vy = 0; ///< units per frame
        float friction = 0;   ///< the velocity loses this fraction every frame
        std::uint32_t startFrame = 0;
    };
    std::optional<Linear> linear;
    /// Fade Objects' marks (client/fade.hpp). MSG_OBJECT_IN_SHADOWS and updates set some; a
    /// renderer's scene sort and draw keep the rest as they draw from a const world.
    mutable FadeState fade;
    /// The last image the generic drawable draw drew of it (client/sight.hpp's drawable sight
    /// test sizes its box by it), noted by a renderer as it draws.
    mutable DrawnImage drawn;
};

struct PlayerEntry {
    std::uint16_t netCode = 0;
    std::string name;
    std::uint8_t playerClass = 0; ///< 0 warrior, 1 wizard, 2 conjurer
    bool female = false;
    std::array<std::uint8_t, 15> colors{};
    std::array<std::uint8_t, 5> clothing{};
    std::int32_t lessons = 0;
    std::int32_t kills = 0;
    std::uint32_t armorEquip = 0;
    std::uint32_t weaponEquip = 0;
    /// Modifier ids of the equipped items by class bit, in the network order (effectiveness,
    /// material, primary and secondary enchantment; 0xFF none). From the equip reports; items
    /// known only from the player info have no entry. They recolour the item's layer.
    std::map<std::uint32_t, std::array<std::uint8_t, 4>> armorModifiers;
    std::map<std::uint32_t, std::array<std::uint8_t, 4>> weaponModifiers;
    std::uint32_t status = 0; ///< 1 observer, 2 following, 0x400 poisoned (mask 0x423)
    std::uint16_t latency = 0;
    std::uint32_t team = 0;
    /// The team a TEAM 1 or 3 last put them in, kept when TEAM 2 takes them out: our own
    /// player's frames that team's CTF flag icon (Team_AddMember, Team_MoveMember). A TEAM 1
    /// naming the team they're in already, or a TEAM 3 while they're in none, leaves it.
    std::uint32_t flagIconTeam = 0;
    bool dead = false;
    std::uint16_t treasures = 0;
    std::string account; ///< the WOL account name (MSG_NEW_PLAYER +119)
    /// Health x 100 / max health, from TEAM 0x0C (quest games): the bar beside them.
    std::uint8_t healthPct = 0;
    /// Extra lives left, from MSG_GAUNTLET 4 (quest games).
    std::uint8_t questLives = 0;
    /// Holding a SilverKey / GoldKey (quest games), from MSG_GAUNTLET 0x16 / 0x17
    /// (the quest list's white and yellow key marks).
    std::uint8_t silverKey = 0;
    std::uint8_t goldKey = 0;
};

/// The client's record of an NPC (g_cliNpcs, Cli_NewNpc-Cli_NpcAddEquipment), by its net code: what NPCDraw
/// and MaidenDraw draw it with. MSG_REPORT_NPC makes it, or wipes it if
/// there is one; the equip-class reports that follow without bit 15 fill its worn things.
/// The table has kNpcSlots records, none freed in a session: with all taken a new code gets none.
inline constexpr std::size_t kNpcSlots = 64;
struct NpcEntry {
    /// Materials 1-6, in order (Draw_MakeColorRGB of the message's six RGB triples).
    std::array<std::array<std::uint8_t, 3>, 6> colors{};
    /// MSG_REPORT_NPC's bit 15, then MSG_REPORT_OBJECT_POISON's level; NPCDraw greens
    /// the body only while it is exactly 1.
    std::uint32_t poison = 0;
    std::uint32_t weaponEquip = 0; ///< weapon class bits worn
    std::uint32_t armorEquip = 0;  ///< armour class bits worn
    /// Each worn thing's modifier ids by class bit (effectiveness, material, primary and secondary
    /// enchantment; 0xFF none: a mundane report's). The first report of a bit is the one used
    /// (the lists are searched from the front).
    std::map<std::uint32_t, std::array<std::uint8_t, 4>> weaponModifiers;
    std::map<std::uint32_t, std::array<std::uint8_t, 4>> armorModifiers;
};

/// The player animation state an NPC is drawn in (spriteNPCInfo), from its
/// drawable's monster state and its record's worn bits:
///   1-4 (ATTACK, ATTACK_FINISH, ATTACK_FAR, ATTACK_FAR_FINISH): the strike of its first weapon of
///       class bits 2-26 (by its table), else PUNCH_LEFT or PUNCH_RIGHT at random
///   5, 6 (DEFEND, DEFEND_FINISH): a great sword's GREAT_SWORD_BLOCK_LEFT / RIGHT / DOWN at random,
///       a staff's STAFF_BLOCK, a shield's RAISE_SHIELD, else IDLE
///   7 CAST_SPELL; 8 IDLE (GREAT_SWORD_IDLE with a great sword); 9 DIE; 10 DEAD; 12 WALK; 13 RUN;
///   14 ELECTRIC_ZAP; anything else IDLE.
/// `choice` is kept on the drawable: a random pick is kept in it while the animation's frame
/// (`animFrame`, the server's) isn't 0 and made again at frame 0; any other state clears it.
/// `roll(a, b)` is the game's visual random number (Math_RandIntVisual), a to b inclusive.
std::uint8_t npcPlayerState(std::uint8_t monsterState, std::uint32_t weaponEquip, std::uint32_t armorEquip,
                            std::uint32_t animFrame, std::uint8_t& choice, const std::function<int(int, int)>& roll);

struct Team {
    std::uint32_t id = 0;
    std::string name;
    std::uint8_t color = 0;
    std::uint32_t score = 0;
    std::set<std::uint16_t> members;
};

struct InventoryItem {
    std::uint16_t netCode = 0;
    std::uint16_t typeId = 0;
    std::array<std::uint8_t, 4> modifiers{0xFF, 0xFF, 0xFF, 0xFF}; ///< effectiveness, material, 2 enchants; 0xFF none
    bool equipped = false;
    std::optional<std::uint16_t> health;
    std::optional<std::uint16_t> maxHealth;
    std::optional<std::uint8_t> charges;
    std::optional<std::uint8_t> maxCharges;
    /// Which pickup report brought it (counting from 1): the inventory window files things in the
    /// order their reports came (Gui_Inv_AddPickedUpItem), a thing reported again anew.
    std::uint32_t pickupOrder = 0;
};

/// A creature of ours in the summon panel's cage (guisumn.c's slots): its size class
/// (the thing's SUBCLASS: 1 SMALL_MONSTER, 2 MEDIUM_MONSTER, else 4) and where it sits in the
/// cage's 2 x 2 cells, a small one in one cell, a medium one a column of two, a large one all four.
struct SummonSlot {
    std::uint16_t netCode = 0;
    std::uint16_t typeId = 0;
    std::uint8_t size = 4;
    int x = 0; ///< its top-left cell
    int y = 0;
};

/// A monitored creature's health (the client's 32-entry table, Cli_AddMonitoredCreature):
/// MSG_REPORT_TOTAL_HEALTH, MSG_REPORT_HEALTH (x 2) and MSG_REPORT_OBJECT_POISON while it's ours
/// or monitored; 0 / 0 until the first report.
struct CreatureHealth {
    std::uint16_t health = 0;
    std::uint16_t maxHealth = 0;
    std::uint8_t poison = 0;
};

struct LocalPlayer;

/// Places the cage's creatures again: the cells cleared, then the small ones, the
/// medium and the large, each in slot order into the first of its places that's free (cells
/// (0,0), (0,1), (1,0), (1,1) for a small one; columns 0 and 1 for a medium one; the whole cage
/// for a large one). One with no room keeps the last place tried and takes no cell.
void placeSummons(LocalPlayer& p);

struct LocalPlayer {
    std::uint16_t netCode = 0;
    std::uint16_t health = 0;
    std::uint16_t maxHealth = 0;
    std::uint16_t mana = 0;
    std::uint16_t maxMana = 0;
    std::uint8_t stamina = 0;
    /// The frame of the last MSG_REPORT_STAMINA, nullopt before one.
    std::optional<std::uint32_t> staminaFrame;
    /// Stamina at `frame`. The server reports it only when it's spent (a jump, a swing:
    /// playerSubStamina), not as it comes back: up to 100, 100 / 30 a
    /// frame (updatePlayer). So: full before the first report, then counted up.
    std::uint8_t staminaAt(std::uint32_t frame) const
    {
        if (!staminaFrame) {
            return 100;
        }
        const std::uint32_t frames = frame > *staminaFrame ? frame - *staminaFrame : 0;
        return static_cast<std::uint8_t>(std::min<std::uint32_t>(100, stamina + frames * 3));
    }
    std::uint16_t carryCapacity = 0;
    std::uint16_t speed = 0;
    std::uint16_t strength = 0;
    std::uint8_t level = 0;
    std::uint32_t experience = 0;
    std::uint32_t gold = 0;
    /// Our extra lives in a quest (MSG_GAUNTLET 4 with our code: Gui_Inv_SetQuestLives),
    /// drawn by the ankh under the buff icons.
    std::uint8_t questLives = 0;
    /// The highest quest stage we've reached (MSG_GAUNTLET 0x1D: Gui_Inv_SetQuestStage,
    /// g_cliQuestStage): the File Info's stage when a quest's save is kept.
    std::uint16_t questStage = 0;
    float armorValue = 0; ///< damage fraction absorbed; the stats screen shows value x 1000
    std::uint8_t itemBuffs = 0;
    std::uint32_t statusFlags = 0; ///< REPORT_PLAYER_STATUS (drawable flags)
    std::array<float, 4> statMultipliers{1, 1, 1, 1}; ///< health, mana, strength, speed
    std::array<std::uint8_t, 6> abilityLevel{};
    std::array<bool, 6> abilityReady{};
    /// The frame each ability last went unready (Cli_SetAbilityReady; 0 while ready):
    /// its cooldown shading counts from it.
    std::array<std::uint32_t, 6> abilityCooldownStart{};
    /// What info books hold (MSG_INFO_BOOK_DATA answers, by the book's net code): a spell, a
    /// field-guide creature or an ability, by the book's kind.
    std::map<std::uint16_t, std::uint8_t> bookContents;
    std::uint8_t activeAbilities = 0;
    std::map<std::uint8_t, std::uint8_t> spellLevels;
    std::map<std::uint8_t, std::uint8_t> guideLevels;
    std::uint16_t secondaryWeapon = 0;
    std::uint16_t lastQuiver = 0;
    /// Our summoned and charmed creatures (MSG_REPORT_ACQUIRE / LOSE_CREATURE), the cage's four
    /// slots in slot order; placed again, small ones first, whenever one comes or goes.
    std::array<std::optional<SummonSlot>, 4> summons;
    /// The cage's cells ([y + x * 2]): the slot index in each, or -1.
    std::array<int, 4> summonCells{-1, -1, -1, -1};
    /// Our creatures' standing order (MSG_REPORT_CREATURE_CMD): 3 guard, 4 escort, 5 hunt; 0
    /// before the first report.
    std::uint8_t creatureCommand = 0;
    std::map<std::uint16_t, CreatureHealth> monitored;
    std::size_t summonCount() const
    {
        return static_cast<std::size_t>(
            std::count_if(summons.begin(), summons.end(), [](const auto& s) { return s.has_value(); }));
    }
    bool inventoryLoaded = false;
    std::map<std::uint16_t, InventoryItem> inventory;
};

struct FlagStatus {
    std::uint8_t state = 0; ///< 0 at home, 1 carried, 2 dropped
    std::uint8_t side = 0;  ///< 1 own team, 2 enemy
    std::uint16_t carrier = 0;
};

struct MagicWall {
    std::uint8_t tile = 0;
    std::uint8_t direction = 0;
    std::uint8_t variation = 0;
};

/// A client-loaded map object as the server has changed it; unset fields are as the map has them.
struct StaticObjectState {
    std::optional<std::uint8_t> frame;   ///< SlaveDraw image (MSG_REPORT_ANIMATION_FRAME, MSG_DRAW_FRAME...)
    std::optional<bool> enabled;          ///< MSG_ENABLE_OBJECT / MSG_DISABLE_OBJECT
    std::optional<std::uint32_t> xStatus; ///< MSG_REPORT_X_STATUS (AnimateStateDraw's state: a pit open or shut)
    std::optional<float> lightIntensity;  ///< MSG_REPORT_LIGHT_INTENSITY, an obelisk's charge, a pentagram
    std::optional<std::array<std::uint8_t, 3>> lightColor; ///< MSG_REPORT_LIGHT_COLOR
    /// A door's lock mark (set at load when it has a lock, DoorDraw drawing it in quests)
    /// cleared by MSG_GAUNTLET 0x0F (its door opened in a quest): false.
    std::optional<bool> lockMark;
};

/// A quest's last stand (MSG_GAUNTLET 2, Gui_GameOver_Show): its tallies for GGOver.wnd.
struct QuestGameOver {
    std::uint16_t generators = 0; ///< GeneratorsDestroyed
    std::uint16_t secrets = 0;    ///< NumSecretsFound
    std::uint16_t kills = 0;      ///< Kills
    std::uint32_t frame = 0;      ///< the frame it came (g_guiGameOverShownFrame: its 30 s count)
    std::uint32_t count = 0;      ///< game overs so far: each one shows the window anew
};

struct GameState {
    std::string mapName;
    std::uint32_t mapCrc = 0;
    std::uint32_t gameFlags = 0; ///< from GAME_SETTINGS
    std::uint8_t playerLimit = 0;
    std::uint16_t fragLimit = 0;
    std::uint8_t timeLimitMinutes = 0;
    std::array<std::uint8_t, 20> allowedSpells{}; ///< bit n = spell n allowed (GAME_SETTINGS_2: Cli_ApplyAllowedSpells)
    std::uint8_t netRate = 0;                     ///< RATE_CHANGE
    /// The settings' records as MSG_UPDATE_GUI_GAME_SETTINGS (0xB1) left them (Game_GetSettingsData:
    /// 58 bytes, the server's GameHost::SettingsRecord's layout; slot 1 the host's window's). The
    /// first since JOIN_DATA or USE_MAP (Cli_HasGameSettings) copies slot 1 into slot 0, which is
    /// what the fields here show: the name, the spells, the limits.
    std::array<std::array<std::uint8_t, 58>, 2> settingsSlots{};
    std::string gameName;
    std::map<std::pair<std::uint8_t, std::uint8_t>, MagicWall> magicWalls; ///< by grid cell (Wall spell)
    /// The game is over (the winner messages set the client's game flag 8; MSG_USE_MAP clears
    /// it), and how many times it has been: the scoreboard opens each time (Gui_Rank_ShowOnGameOver).
    bool over = false;
    std::uint32_t overCount = 0;
    /// The last winner message, for the game-over banner: its op (0x56 flag ball, 0x57 flag,
    /// 0x58 deathmatch, 0x59 team deathmatch), the winner (a team id, or for 0x58 a player's net
    /// code; 0 a tie), and whether the time limit ended it (its byte 3 is 1).
    std::uint8_t overKind = 0;
    std::uint16_t overWinner = 0;
    bool overTimeLimit = false;
    /// The journal (MSG_JOURNAL_MSG: 1 add, 2 remove, 3 a new type; Plr_AddJournalEntry),
    /// oldest first as the inventory's journal page lists it: the entry's label under "Journal:"
    /// and its type (1 white, 2 quest, 4 done, 8 hint, else green).
    struct JournalEntry {
        std::string entry;
        std::uint16_t type = 0;
    };
    std::vector<JournalEntry> journal;
    bool timerRunning = false;
    std::uint32_t timeRemainingMs = 0;
    std::map<std::uint8_t, FlagStatus> flags; ///< by CTF GUI slot
    /// The CTF flag icons at the bottom right (GUI_CTF.wnd). MSG_GAME_SETTINGS turning the CTF
    /// game flag (0x20) on puts them up (Gui_Ctf_Show: two of them, all at home), turning it off
    /// takes them down (Gui_Ctf_Hide). MSG_REPORT_FLAG_STATUS sets the state of icon team id - 1
    /// (Gui_Ctf_SetFlagStatus): 0 at home, 1 carried, 2 dropped.
    bool ctfIconsShown = false;
    std::uint8_t ctfIconCount = 0;
    std::array<std::uint8_t, 16> ctfIconStates{};
    std::uint8_t ballState = 0;
    std::uint16_t ballCarrier = 0;
    std::set<std::uint16_t> destroyedWalls; ///< DestructableWalls indexes (MSG_DESTROY_WALL)
    /// SecretWalls indexes the server opened or closed. Neither: as the map says (flag 8 = open).
    std::set<std::uint16_t> openWalls;
    std::set<std::uint16_t> closedWalls;
    std::uint32_t wallsVersion = 0; ///< bumped by every wall message, for renderers to resync
    /// MSG_DOOR_ANGLE by net code (bit 15 = a map object): 0-31, the door's leaf and image.
    std::map<std::uint16_t, std::uint8_t> doorAngles;
    /// What the server changed of map objects the client loads itself (flames, obelisks,
    /// pentagrams), by map extent: the messages that name a static drawable (net code bit 15).
    std::map<std::uint16_t, StaticObjectState> statics;
    std::string lastWinner; ///< e.g. "team 1", "player 2", "tie"
    /// The quest's shared keys (g_guiSKeyWindowState, MSG_GAUNTLET 0x18: Gui_SetSKeyWindowState):
    /// on, the inventory shows the SharedKeyMode icon. Each time it
    /// goes on, `sharedKeysShown` counts up: SKey.wnd shows and EnableSharedKeyMode
    /// plays; going off hides the window.
    bool sharedKeys = false;
    std::uint32_t sharedKeysShown = 0;
    QuestGameOver questGameOver;
};

struct TradeItem {
    std::uint16_t netCode = 0;
    std::uint16_t typeId = 0;
    std::uint32_t value = 0; ///< trade value, or the shop price
    std::uint32_t durability = 0; ///< shop items
    std::array<std::uint8_t, 4> modifiers{0xFF, 0xFF, 0xFF, 0xFF};
};

/// A shop or a player-to-player trade (MSG_TRADE, 0xC9).
struct TradeState {
    enum class Kind : std::uint8_t { None, Shop, Player };
    Kind kind = Kind::None;
    std::string partner;             ///< trade partner, or the shop's name
    std::uint16_t shopkeeperType = 0;
    std::string greeting;            ///< string id
    std::vector<TradeItem> shopItems;
    std::vector<TradeItem> ourOffer;
    std::vector<TradeItem> theirOffer;
    std::uint8_t accepted = 0; ///< bit 0 = we accepted, bit 1 = partner accepted
    std::array<std::uint32_t, 3> gold{};
    std::uint16_t shortOfGold = 0;
    std::optional<std::pair<std::uint16_t, std::uint32_t>> sellOffer;   ///< item, price
    std::optional<std::pair<std::uint16_t, std::uint32_t>> repairOffer; ///< item, cost
    /// Counted up with each 0x1B (not enough gold), 0x1D (sell offer) and 0x1F (repair offer),
    /// so a window sees each one, even the same twice.
    std::uint32_t shortOfGoldCount = 0;
    std::uint32_t sellOfferCount = 0;
    std::uint32_t repairOfferCount = 0;
};

/// An NPC dialog (MSG_DIALOG, 0xD0).
struct DialogState {
    bool open = false;
    std::string textId;
    std::string title;
    std::uint32_t sound = 0;
    std::string portrait;
    std::uint8_t flags = 0; ///< 1 Yes / No, 2 Next (Gui_NpcDialog_Open)
    std::uint32_t count = 0; ///< dialogs opened so far: a new page shows even while one is open
};

/// A chat bubble over a speaker (Cli_ShowChatBubble, Cli_ShowChatBubble): one per speaker, replaced by their
/// next line, gone at `expires` or on MSG_CHAT_KILL.
struct ChatBubble {
    std::string text;         ///< UTF-8; string-table ids (flag 8) as "[id]"
    std::int32_t x = 0;       ///< where it was said; the speaker's drawable wins while it exists
    std::int32_t y = 0;
    std::uint32_t expires = 0; ///< server frame
    bool team = false;         ///< team chat (the game prefixes Guirank.c:team, not resolved here)
    /// Its place in the game's list (Cli_ShowChatBubble links a new speaker's bubble last; their next line
    /// keeps the place): earlier bubbles stay put and later ones move off them.
    std::uint32_t order = 0;
};

/// A floating damage number (MSG_REPORT_HEALTH_DELTA, Cli_AddHealthChangeFloater): the
/// server sends one to each client that sees a unit hurt (SrvNet_SendHealthDelta, damage
/// only); the client shows it over the unit for 31 frames.
struct HealthFloater {
    std::uint16_t netCode = 0;
    std::int16_t delta = 0;  ///< negative: damage
    std::uint32_t frame = 0; ///< server frame it came
};

/// A visual effect (MSG_FX_*, 0x7C-0xA3; see net/fx.hpp): sparks, explosions, rays. The client
/// plays these as short-lived local drawables and keeps no game state for them, so the world
/// only queues them for a renderer.
struct FxEvent {
    std::uint32_t frame = 0; ///< server frame of the datagram it came in
    net::FxMessage fx;
};

/// A sound the client plays: the server's MSG_AUDIO_EVENT / MSG_AUDIO_PLAYER_EVENT (volume and
/// pan already worked out for us, from where it happened and where we look), the music messages,
/// and the sounds the client's own message handling plays (`Local`: game over, a flag taken...).
struct AudioCue {
    enum class Kind : std::uint8_t {
        Sound,       ///< id, volume 0-126, pan -50..50
        PlayerSound, ///< one we made ourselves (two priorities higher)
        Local,       ///< Snd_PlaySound(id, volume)
        Music,       ///< id = track (0 = none), volume 0-100
        MusicPush,
        MusicPop,
        SpellChant,  ///< id = the spell we started casting (MSG_REPORT_SPELL_START): chant its phonemes
    };
    std::uint32_t frame = 0; ///< server frame of the datagram it came in
    Kind kind = Kind::Sound;
    std::uint16_t id = 0;
    std::uint8_t volume = 0;
    std::int8_t pan = 0;
};

/// Something the spellbook and the spell bars act on (CliNet_OnPacket's award and revoke
/// messages): a spell, field-guide creature or ability learned, which may open the book on its
/// page (`show`) and fly its icon to an empty slot (`fill`); or one taken away again (the
/// gauntlet's revokes), which closes the book and empties its slots.
struct BookEvent {
    enum class Kind : std::uint8_t { Spell, Creature, Ability, RevokeSpell, RevokeCreature, RevokeAbility };
    std::uint32_t frame = 0;
    Kind kind = Kind::Spell;
    std::uint8_t id = 0;
    bool show = false;
    bool fill = false;
};

class World {
public:
    explicit World(const things::ThingDb* things = nullptr)
        : things_(things)
    {
    }

    /// Applies one server-to-client message (whole, opcode first). `frame` is the current server frame.
    void apply(std::span<const std::uint8_t> message, std::uint32_t frame);
    /// Applies a parsed UPDATE_STREAM. `late`: its datagram's frame is older than one already
    /// applied (it came out of order): what it introduces is added, but nothing already known
    /// moves back to where the older frame had it.
    void applyStream(const net::UpdateStream& stream, std::uint32_t frame, bool late = false);
    /// Moves the drawables the client predicts itself (Drawable::linear) to where they are at
    /// `frame`, as sprite does every frame; ones that leave the map go.
    void predict(std::uint32_t frame);
    /// Drops the client-predicted drawables `gone` says to: the game drops one as soon as it's
    /// outside the eye's sight polygon, which only a renderer knows. The server
    /// never sends them updates or a DESTROY_OBJECT, so nothing else removes them.
    void dropPredicted(const std::function<bool(const Drawable&)>& gone);
    /// Class bits the client would use to parse an update-stream entry: the existing drawable's
    /// type, else `typeId`'s. nullopt if neither is a known thing.
    std::optional<std::uint32_t> drawableClass(std::uint16_t netCode, std::uint16_t typeId) const;
    /// Fade Objects (nox.cfg FadeObjects; off unless a renderer turns it on): a drawable the
    /// server says went into shadows is kept a game second, for the renderer to fade out
    /// (client/fade.hpp), instead of dropped at once.
    void setFadeObjects(bool on) noexcept { fadeObjects_ = on; }
    bool fadeObjects() const noexcept { return fadeObjects_; }

    const std::map<std::uint16_t, Drawable>& drawables() const noexcept { return drawables_; }
    const Drawable* drawable(std::uint16_t netCode) const;
    /// noxcpp's MSG_NOXCPP_SUBSTEP: a drawable the server updates (not one we predict) moved to
    /// (x, y) between frames; false if there's none such.
    bool setSubstepPosition(std::uint16_t netCode, int x, int y)
    {
        const auto it = drawables_.find(netCode);
        if (it == drawables_.end() || it->second.linear) {
            return false;
        }
        it->second.x = x;
        it->second.y = y;
        return true;
    }
    /// On the friends list (g_cliFriendList: MSG_OBJECT_FRIEND_ADD / REMOVE, cleared by
    /// MSG_RESET_FRIENDS): our summons and the like. Kept by net code, as the game keeps it, so
    /// it holds for a creature we haven't been sent yet or that went out of view and came back.
    bool isFriend(std::uint16_t netCode) const { return friends_.contains(netCode); }
    const std::map<std::uint16_t, PlayerEntry>& players() const noexcept { return players_; }
    /// The player of that net code (null: none known).
    const PlayerEntry* player(std::uint16_t netCode) const
    {
        const auto it = players_.find(netCode);
        return it != players_.end() ? &it->second : nullptr;
    }
    /// The NPC records (NpcEntry). Kept for the session, as the game keeps its table (cleared only
    /// when a game starts): a map change doesn't drop them, MSG_REPORT_NPC renews them.
    const std::map<std::uint16_t, NpcEntry>& npcs() const noexcept { return npcs_; }
    const NpcEntry* npc(std::uint16_t netCode) const
    {
        const auto it = npcs_.find(netCode);
        return it == npcs_.end() ? nullptr : &it->second;
    }
    const std::map<std::uint32_t, Team>& teams() const noexcept { return teams_; }
    const LocalPlayer& self() const noexcept { return self_; }
    /// The client's own choice of secondary weapon (it tells the server; HeadlessClient).
    void setSecondaryWeapon(std::uint16_t netCode) { self_.secondaryWeapon = netCode; }
    const GameState& game() const noexcept { return game_; }
    const TradeState& trade() const noexcept { return trade_; }
    const DialogState& dialog() const noexcept { return dialog_; }
    /// Chat bubbles by speaker net code. Expired ones linger until the next line arrives; check
    /// `expires` against the frame.
    const std::map<std::uint16_t, ChatBubble>& chatBubbles() const noexcept { return bubbles_; }
    /// Damage numbers of the last second or so, oldest first (HealthFloater).
    const std::vector<HealthFloater>& healthFloaters() const noexcept { return floaters_; }
    /// The camera position from the last update stream.
    std::optional<std::pair<int, int>> camera() const noexcept { return camera_; }

    /// thing.bin name for a type id, or "#<id>".
    std::string thingName(std::uint16_t typeId) const;
    /// The thing.bin the type ids are of (null: none given; every name is then "#<id>").
    const things::ThingDb* things() const noexcept { return things_; }

    /// modifier.bin's id for UserColor1 (the clothes' colours are UserColor1 + index), for the
    /// default outfit. Without it the clothes get no colour modifiers.
    void setUserColorBase(std::uint8_t id) { userColorBase_ = id; }
    /// modifier.bin's ids for ArmorQuality1, Material1 and Replenishment1 (the solo and quest
    /// starting weapons' modifiers, Cli_ResetPlayerDefaultOutfit); without them 0xFF.
    void setOutfitModifiers(std::uint8_t armorQuality1, std::uint8_t material1, std::uint8_t replenishment1)
    {
        outfitModifiers_ = {armorQuality1, material1, replenishment1};
    }

    /// FX messages received since the last call, oldest first. Only the newest kMaxEffects are
    /// kept, so a client that never takes them (a headless bot) doesn't grow without bound.
    std::vector<FxEvent> takeEffects();
    static constexpr std::size_t kMaxEffects = 512;
    /// Sounds and music changes since the last call, oldest first (the newest kMaxAudio kept).
    std::vector<AudioCue> takeAudio();
    static constexpr std::size_t kMaxAudio = 256;
    /// Awards and revokes since the last call, oldest first (the newest kMaxBookEvents kept).
    std::vector<BookEvent> takeBookEvents();
    static constexpr std::size_t kMaxBookEvents = 64;

private:
    Drawable& upsert(std::uint16_t netCode, std::uint16_t typeId, std::int32_t x, std::int32_t y, std::uint32_t frame);
    void remove(std::uint16_t netCode, bool keepSelf);
    void inShadows(std::uint16_t netCode, std::uint32_t frame);
    void expire(std::uint32_t frame);
    void nextFrame(std::uint32_t frame);
    void setAnim(Drawable& d, std::uint8_t anim);
    /// A stream object's anim byte, frame and state onto its drawable.
    void animate(Drawable& d, const net::StreamObject& o);
    bool removesItself(const Drawable& d) const;
    std::optional<std::uint32_t> classOf(std::uint16_t typeId) const;
    void team(std::span<const std::uint8_t> m);
    void tradeMessage(std::span<const std::uint8_t> m);
    void dialogMessage(std::span<const std::uint8_t> m);
    void gauntletMessage(std::span<const std::uint8_t> m, std::uint32_t frame);
    void chatText(std::span<const std::uint8_t> m, std::uint32_t frame);
    void queueAudio(const AudioCue& cue);
    /// The message's effect, if it decodes as one (net::decodeFx), onto the effects queue.
    void queueEffect(std::span<const std::uint8_t> m, std::uint32_t frame);
    void gameOverSound(std::span<const std::uint8_t> m, std::uint32_t frame);
    void informSound(std::span<const std::uint8_t> m, std::uint32_t frame);
    void sentryRaySound(std::int32_t x, std::int32_t y, std::uint32_t frame);
    void resetOutfit(PlayerEntry& p, std::uint8_t itemMask);
    void queueBook(const BookEvent& e);
    /// Sets a spell's level, and its linked ones' (Cli_AwardSpell): a general summon,
    /// mark or go-to-mark spell (flags 0x1000 / 0x4000 / 0x10000) sets every spell of its kind
    /// (0x2000 / 0x8000 / 0x20000).
    void setSpellLevel(std::uint8_t spell, std::uint8_t level);

    const things::ThingDb* things_;
    std::optional<std::uint8_t> userColorBase_;
    /// Standing reports (REPORT_X_STATUS, REPORT_ENCHANTMENT) for a drawable not made yet,
    /// applied when it is: our server sends them reliably beside the unreliable update that
    /// makes it (the exe's go in the same packet after the record), so one can come first.
    struct EarlyReport {
        std::optional<std::uint32_t> xStatus;
        std::optional<std::uint32_t> buffs;
        std::optional<std::array<std::uint8_t, 4>> modifiers; ///< REPORT_MODIFIER's (a flag's, an item's)
        std::optional<bool> enabled;                          ///< MSG_ENABLE / DISABLE_OBJECT's
    };
    std::unordered_map<std::uint16_t, EarlyReport> early_;
    EarlyReport& keepEarly(std::uint16_t netCode)
    {
        if (early_.size() > 512) {
            early_.clear(); // (never made: kept no longer)
        }
        return early_[netCode];
    }
    std::array<std::uint8_t, 3> outfitModifiers_{0xFF, 0xFF, 0xFF}; ///< ArmorQuality1, Material1, Replenishment1
    TradeState trade_;
    DialogState dialog_;
    std::map<std::uint16_t, Drawable> drawables_;
    bool fadeObjects_ = false;
    /// Drawables told into shadows with Fade Objects on, to go at their FadeState::expireAt (the
    /// client's expiry list); an entry whose drawable is gone or was updated since is dropped.
    std::vector<std::uint16_t> expiring_;
    /// The frame of the last message or update applied: the expiry list runs for it once a later
    /// frame's comes (nextFrame).
    std::uint32_t expiryFrame_ = 0;
    std::set<std::uint16_t> friends_;
    std::map<std::uint16_t, PlayerEntry> players_;
    std::map<std::uint16_t, NpcEntry> npcs_;
    std::map<std::uint32_t, Team> teams_;
    LocalPlayer self_;
    std::uint32_t pickups_ = 0; ///< pickup reports so far (InventoryItem::pickupOrder)
    GameState game_;
    std::optional<std::pair<int, int>> camera_;
    std::deque<FxEvent> effects_;
    std::deque<AudioCue> audio_;
    std::deque<BookEvent> book_;
    std::uint32_t mapFrame_ = 0;      ///< frame of the last USE_MAP
    std::uint32_t applyFrame_ = 0;    ///< the frame of the message being applied
    bool optionsChanged_ = false;     ///< (the limits changed since the last settings)
    bool settingsSeen_ = false;
    bool settingsAcquired_ = false;   ///< (g_cliGameSettingsAcquired: an 0xB1 since JOIN_DATA / USE_MAP)
    std::array<std::uint32_t, 2> settings2Extra_{};
    std::uint32_t rng_ = 0x4E6F78;    ///< the client's visual random numbers
    std::map<std::uint16_t, ChatBubble> bubbles_;
    std::vector<HealthFloater> floaters_;
    std::uint32_t bubbleOrder_ = 0; ///< the next new bubble's ChatBubble::order
};

} // namespace nox::client
