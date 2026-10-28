// A stand-in for the server we'll host with, so hosted bots (bot_host.hpp) can play now: a
// deathmatch on a real map's walls, PlayerStart points and items, with players that move, swing,
// charge, harpoon, drink and pick up.
//
// Its rules are rough approximations, NOT the game's (the real ones are the server's, noxcore/server):
// fixed walk and run speeds, a swing every half second hitting the nearest enemy in front within
// reach for a fixed amount, walls as blocked 23-unit cells, no armour, no objects in the way.
// What it does keep from the game: gamedata.bin's warrior numbers, read from the file it's given
// as an arena game reads them (WarriorMaxHealth, the abilities' delays, BerserkerChargeDuration,
// BerserkerDamage, BerserkerStunDuration, BerserkerPainRatio, MaxHarpoonFlightDistance,
// HarpoonDamage: Rules), War Cry's 300 radius, and new players starting as observers who press
// Action to spawn. Good enough to watch brains and the roster work, and to test them.
#pragma once

#include "noxcore/bot/bot_host.hpp"
#include "noxcore/client/navigation.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace nox::map {
struct MapFile;
}
namespace nox::things {
struct GameData;
}

namespace nox::bot {

class EmulatedHost : public BotHost {
public:
    /// The input a player gave, kept until changed (the held controls) or used (the rest).
    struct Controls {
        std::optional<std::uint8_t> move;
        std::uint8_t facing = 0;
        std::pair<int, int> cursor{};
        bool action = false;
        bool press = false;
        bool observe = false;
        bool jump = false;
        std::vector<std::uint8_t> abilities;
        std::vector<std::uint16_t> pickups;
        std::vector<std::uint16_t> uses;
        std::vector<std::uint16_t> equips;
    };

    struct Player {
        std::uint16_t netCode = 0;
        std::string name;
        bool human = false; ///< a stand-in for a person: stands where it spawns
        float x = 0;
        float y = 0;
        std::uint8_t facing = 0;
        bool observer = true; ///< new players watch until they press Action (Plr_CreateFromJoinData)
        bool dead = false;
        std::uint16_t health = 0;
        std::uint16_t maxHealth = 0; ///< Rules::maxHealth (gamedata.bin's WarriorMaxHealth)
        std::array<std::uint32_t, 6> cooldownUntil{}; ///< by ability: the frame it's ready again
        std::uint32_t nextSwing = 0;
        std::uint32_t chargeUntil = 0; ///< charging (Berserker Charge) until this frame
        std::uint32_t stunnedUntil = 0;
        std::uint32_t airUntil = 0; ///< jumping until this frame: over what hurts underfoot
        std::uint32_t hazardFrames = 0; ///< frames in a row standing in a hazard
        std::uint8_t stamina = 100;
        std::uint32_t deadSince = 0;
        int kills = 0;
        int deaths = 0;
        std::vector<OwnItem> inventory;
        Controls controls;
    };

    struct Item {
        std::uint16_t netCode = 0;
        std::uint16_t typeId = 0;
        ItemKind kind = ItemKind::Other;
        float x = 0;
        float y = 0;
        std::uint32_t backAt = 0; ///< taken: lies here again from this frame (0: lying here)
    };

    struct Kill {
        std::uint32_t frame = 0;
        std::uint16_t killer = 0;
        std::uint16_t victim = 0;
        bool charge = false;
        std::uint32_t hazardFrames = 0; ///< how long the victim had stood in a hazard (0: wasn't)
        float x = 0;                    ///< where
        float y = 0;
    };

    /// The gamedata.bin numbers it plays by, read once from the file (an arena game's lines).
    struct Rules {
        std::array<std::uint32_t, 6> abilityDelay{}; ///< by Ability id (0 unused): BerserkerChargeDelay ... EyeOfTheWolfDelay
        std::uint32_t chargeFrames = 0;              ///< BerserkerChargeDuration
        std::uint16_t chargeDamage = 0;              ///< BerserkerDamage
        std::uint32_t chargeStunFrames = 0;          ///< BerserkerStunDuration
        float chargePain = 0;                        ///< BerserkerPainRatio: of the health left
        float harpoonRange = 0;                      ///< MaxHarpoonFlightDistance
        std::uint16_t harpoonDamage = 0;             ///< HarpoonDamage
        std::uint16_t maxHealth = 0;                 ///< WarriorMaxHealth
        static Rules from(const things::GameData& gameData);
    };

    /// A deathmatch on `map`. `things` names the items' types; `gameData` (the install's
    /// gamedata.bin) gives Rules; `modifiers` (optional) says who may use the items and how good
    /// they are.
    EmulatedHost(const map::MapFile& map, const things::ThingDb& things, const things::GameData& gameData,
                 std::uint32_t seed, std::size_t playerLimit = 16, const things::ModifierDb* modifiers = nullptr);
    ~EmulatedHost() override;

    // BotHost
    std::optional<std::uint16_t> addBot(const client::PlayerProfile& profile) override;
    void removeBot(std::uint16_t netCode) override;
    BotView view(std::uint16_t netCode) const override;
    BotBody& body(std::uint16_t netCode) override;
    std::size_t humans() const override;
    std::size_t playerLimit() const override { return playerLimit_; }
    std::set<std::string> names() const override;

    /// A stand-in for a person joining (the roster makes room); removeBot takes it out again.
    std::optional<std::uint16_t> addHuman(std::string name);

    /// One game frame: everyone's input applied, swings, charges, deaths, items back.
    void step();

    std::uint32_t frame() const noexcept { return frame_; }
    const std::map<std::uint16_t, Player>& players() const noexcept { return players_; }
    const std::vector<Item>& items() const noexcept { return items_; }
    const std::vector<Kill>& kills() const noexcept { return kills_; }
    const client::NavGrid& nav() const noexcept { return nav_; }
    const Rules& rules() const noexcept { return rules_; }
    const std::vector<std::pair<float, float>>& spawns() const noexcept { return spawns_; }

    // The stand-in's rules (world units and frames).
    static constexpr float kRunSpeed = 6;
    static constexpr float kWalkSpeed = 3;
    static constexpr float kChargeSpeed = 20; ///< as WarriorBrain::kChargeSpeed estimates it
    static constexpr float kReach = 40; ///< bare hands (a weapon's is ItemInfo::reach)
    static constexpr std::uint32_t kSwingFrames = 15;
    /// A swing's damage: this plus a part of the weapon's value (ItemInfo::value: a Longsword 20,
    /// a WarHammer 53).
    static constexpr float kSwingDamage = 8;
    static constexpr float kSwingPerValue = 0.3f;
    static constexpr std::uint16_t kPotionHealth = 50;
    static constexpr std::uint32_t kItemRespawnFrames = 900;
    static constexpr std::uint32_t kJumpFrames = 8;   ///< in the air, at twice running speed
    static constexpr std::uint8_t kJumpStamina = 90;  ///< (the game's cost)
    static constexpr std::uint16_t kHazardDamage = 2; ///< every 4 frames standing in a hazard
    /// What a bot is shown: players and items within this much of it each way (about a 640 x 480
    /// client's screen and a margin).
    static constexpr float kViewHalfWidth = 420;
    static constexpr float kViewHalfHeight = 340;

private:
    class Body;
    std::optional<std::uint16_t> join(std::string name, bool human);
    void spawn(Player& p);
    void move(Player& p, std::uint8_t direction, float speed);
    /// Inside the map and not in a blocked cell: somewhere a player can stand.
    bool open(float x, float y) const;
    void hurt(Player& victim, std::uint16_t damage, Player* by, bool charge);
    void swing(Player& p);
    void ability(Player& p, std::uint8_t which);
    Player* nearestInFront(const Player& p, float reach, int halfArc);

    Rules rules_;
    client::NavGrid nav_;
    ItemKinds kinds_;
    std::vector<std::pair<float, float>> spawns_;
    std::vector<Item> items_;
    std::vector<SeenItem> mapItems_; ///< where the map lays its items out (BotView::mapItems)
    std::size_t playerLimit_;
    std::uint16_t swordType_ = 0; ///< Longsword: what everyone spawns holding
    std::mt19937 rng_;
    std::uint32_t frame_ = 0;
    std::uint16_t nextNetCode_ = 1;
    std::uint16_t nextItemCode_ = 0x2000;
    std::map<std::uint16_t, Player> players_;
    std::map<std::uint16_t, std::unique_ptr<Body>> bodies_;
    std::vector<Kill> kills_;
};

} // namespace nox::bot
