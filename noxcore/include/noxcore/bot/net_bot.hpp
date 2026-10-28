// A bot that plays over the network: a HeadlessClient for its body, its World for the view.
// It joins any 1.2b server, vanilla included. No sockets here; the caller moves the client's
// datagrams.
#pragma once

#include "noxcore/bot/bot.hpp"
#include "noxcore/client/headless_client.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace nox::bot {

/// The body as a client's input: each call is the HeadlessClient action that sends it.
class ClientBody : public BotBody {
public:
    explicit ClientBody(client::HeadlessClient& client)
        : client_(client)
    {
    }
    void setMove(std::optional<std::uint8_t> direction) override { client_.move(direction); }
    void setFacing(std::uint8_t direction) override { client_.face(direction); }
    void setCursor(int x, int y) override { client_.setCursor(x, y); }
    void setAction(bool held) override { client_.setAttack(held); }
    void pressAction() override { client_.spawn(); }
    void jump() override { client_.jump(); }
    void useAbility(std::uint8_t ability) override { client_.useAbility(ability); }
    void castSpells(std::span<const std::uint32_t> spells, bool onSelf) override { client_.castSpells(spells, onSelf); }
    void pickup(std::uint16_t netCode) override { client_.pickup(netCode); }
    void useItem(std::uint16_t netCode) override { client_.use(netCode); }
    void equip(std::uint16_t netCode) override { client_.equip(netCode); }
    void say(std::string_view text, bool team) override { client_.say(text, team); }
    void taunt() override { client_.social(client::HeadlessClient::Social::Taunt); }
    void joinTeam(std::uint32_t team) override { client_.joinTeam(team); }
    void observe() override { client_.observe(); }

private:
    client::HeadlessClient& client_;
};

/// A network (not quest) game's spells for a class: every spell of its book (thing.bin's class
/// flags: 0x1000000 any caster, 0x2000000 wizards, 0x4000000 conjurers) that the game's settings
/// allow (`allowed`, GAME_SETTINGS_2's bitmap; all zero: every one). The server sends no awards
/// there: it clears a joining player's spells and grants them only in quests
/// (spellAwardAll2), and the client's book shows them all.
std::vector<BotView::KnownSpell> classSpells(const things::ThingDb& db, std::uint8_t playerClass,
                                             const std::array<std::uint8_t, 20>& allowed);

/// classSpells' answer kept for the book and settings it was worked out for (a view's spells, made
/// again only as they change).
struct ClassSpellsCache {
    const things::ThingDb* db = nullptr;
    std::uint8_t playerClass = 0xFF;
    std::array<std::uint8_t, 20> allowed{};
    std::vector<BotView::KnownSpell> spells;
};

/// What a client knows, as a bot's view: us from the World's self and player list, others
/// from the players and their drawables, items from the drawables of item types.
BotView viewOf(const client::HeadlessClient& client, const ItemKinds& kinds);
/// viewOf into `v` (made afresh, its lists' room kept), a network game's spells from `spells`.
void viewInto(BotView& v, const client::HeadlessClient& client, const ItemKinds& kinds, ClassSpellsCache& spells);

class NetBot {
public:
    NetBot(client::PlayerProfile profile, const things::ThingDb& things, const ItemKinds& kinds,
           std::filesystem::path mapsDir, std::filesystem::path mapCache, std::unique_ptr<Brain> brain);
    NetBot(const NetBot&) = delete;
    NetBot& operator=(const NetBot&) = delete;

    client::HeadlessClient& client() noexcept { return client_; }
    const client::HeadlessClient& client() const noexcept { return client_; }
    Brain& brain() noexcept { return *brain_; }
    const Brain& brain() const noexcept { return *brain_; }

    /// Call often (every few ms): the brain thinks once a game frame, then the client runs.
    void tick(std::uint32_t nowMs);
    /// Out of play (BotView::standDown) until cleared.
    void setStandDown(bool value) noexcept { standDown_ = value; }
    /// Its part in a team game (BotView::role, leader), as the squad decides.
    void setRole(BotView::Role role, std::uint16_t leader = 0) noexcept
    {
        role_ = role;
        leader_ = leader;
    }
    BotView::Role role() const noexcept { return role_; }
    /// Carrying their flag: who's escorting us (BotView::escorts).
    void setEscorts(std::vector<std::uint16_t> escorts) { escorts_ = std::move(escorts); }
    /// People having a hard time against the bots (BotView::struggling).
    void setStruggling(std::vector<std::uint16_t> people) { struggling_ = std::move(people); }
    /// For logs: the flags it saw last time it thought (BotView::flags).
    const std::vector<BotView::FlagBase>& flagsSeen() const noexcept { return flagsSeen_; }

private:
    std::vector<BotView::FlagBase> flagsSeen_;
    std::vector<std::uint16_t> escorts_;
    std::vector<std::uint16_t> struggling_;
    bool standDown_ = false;
    client::HeadlessClient client_;
    ClientBody body_;
    std::unique_ptr<Brain> brain_;
    const ItemKinds& kinds_;
    std::uint32_t thoughtAt_ = ~0u; ///< the client's input frame the brain last thought for
    /// The items the map lays out (BotView::mapItems), from the client's map objects; rebuilt
    /// when a map loads.
    std::vector<SeenItem> mapItems_;
    std::vector<BotView::FlagBase> flagBases_;
    /// The map's obelisks (BotView::manaSources) with their map extent, which their charge
    /// comes back under (World's statics).
    std::vector<std::pair<std::uint16_t, BotView::ManaSource>> obelisks_;
    /// The map's elevator cars (BotView::elevatorCars) with their extent, which their height
    /// comes back under (World's statics: MSG_DRAW_FRAME).
    std::vector<std::pair<std::uint16_t, BotView::RideCar>> elevatorCars_;
    std::uint32_t mapItemsLoad_ = 0;
    BotView::Role role_ = BotView::Role::Free;
    std::uint16_t leader_ = 0;
    /// The view the brain thinks over, made again each input frame in the same lists, and the
    /// class's spells as last worked out.
    BotView view_;
    ClassSpellsCache spells_;
};

} // namespace nox::bot
