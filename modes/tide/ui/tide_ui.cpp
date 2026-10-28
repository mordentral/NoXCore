// Undead Tide's UI in noxsdl (mode_ui.hpp): the run's strip, a level-up's cards, the results and the
// lobby gate's difficulty question (tide_window.hpp), from what the Tide's client side was told
// (tide_client.hpp); the game over in the Tide's own words; its --fake-tide screens.
#include "app.hpp"
#include "mode_ui.hpp"
#include "tide_window.hpp"

#include "noxcore/client/tide_client.hpp"
#include "noxcore/server/tide.hpp"

#include <memory>
#include <optional>

namespace noxsdl {

namespace {

namespace ts = nox::server;

class TideUi final : public ModeUi {
public:
    explicit TideUi(App& app)
        : window_(app.font.get())
    {
        // Its icons: the spells' own, a sword's and a bow's for the blade and the arrows; the
        // passives a spell's of the like.
        App* a = &app;
        const auto spellSprite = [a](std::uint32_t spell) -> const Sprite* {
            const auto* s = a->things.findSpell(spell);
            return s && a->sprites ? a->sprites->get(s->icon) : nullptr;
        };
        const auto itemSprite = [a](std::string_view type) -> const Sprite* {
            const auto* t = a->things.findThing(type);
            return t && t->prettyImage && a->sprites ? a->sprites->get(*t->prettyImage) : nullptr;
        };
        window_.setWeaponIcon([a, spellSprite, itemSprite](ts::TideWeapon w) -> const Sprite* {
            // (The weapon's own icon, as the server's table names it: a spell's, an item's, or a
            // warrior ability's.)
            const ts::TideIcon icon = ts::tideWeaponIcon(w);
            if (!icon.item.empty()) {
                return itemSprite(icon.item);
            }
            if (icon.ability > 0 && static_cast<std::size_t>(icon.ability) <= a->things.abilities.size() && a->sprites) {
                return a->sprites->get(a->things.abilities[static_cast<std::size_t>(icon.ability - 1)].icons[0]);
            }
            return spellSprite(icon.spell);
        });
        window_.setPassiveIcon([spellSprite](ts::TidePassive p) -> const Sprite* {
            using P = ts::TidePassive;
            switch (p) {
            case P::Haste: return spellSprite(0x24);     // (Haste)
            case P::Vitality: return spellSprite(0x23);  // (Greater Heal)
            case P::Magnet: return spellSprite(0x41);    // (Pull)
            case P::Recovery: return spellSprite(0x29);  // (Lesser Heal)
            case P::Multishot: return spellSprite(0x35); // (Meteor Shower)
            case P::Wisdom: return spellSprite(0x25);    // (Infravision)
            }
            return nullptr;
        });
    }

    void bind(nox::modes::ClientMode* side) override { tide_ = dynamic_cast<nox::client::TideClient*>(side); }

    // The lobby gate's question (the difficulty; the run begins on the answer), a level-up's cards
    // (a click or the keys 1-3 take one), and the run's results.
    bool handleEvent(App&, const SDL_Event& event, int viewWidth, int viewHeight, float hudScale) override
    {
        if (!tide_) {
            return false;
        }
        nox::client::TideClient& c = *tide_;
        std::optional<std::uint8_t> chosen;
        bool dismissed = false;
        if (window_.handleAskEvent(event, c.ask(), viewWidth, viewHeight, hudScale, chosen, dismissed)) {
            if (chosen) {
                c.chooseDifficulty(*chosen);
            } else if (dismissed) {
                c.dismissAsk();
            }
            return true;
        }
        std::optional<std::size_t> pick;
        if (window_.handleEvent(event, c.offer(), c.results(), viewWidth, viewHeight, hudScale, pick)) {
            if (pick) {
                c.pick(*pick);
            }
            return true;
        }
        return false;
    }

    void drawHud(App&, SDL_Renderer* renderer, const HudFrame& hud) override
    {
        if (!tide_) {
            return;
        }
        const nox::client::TideClient& c = *tide_;
        window_.update(c.hud(), c.hudVersion(), c.offer(), c.offerVersion(), c.results(), c.resultsVersion(), hud.nowMs);
        if (hud.renderGui) {
            window_.draw(renderer, c.hud(), c.offer(), c.results(), hud.width, hud.height, hud.scale, hud.mouse, hud.nowMs);
            window_.drawAsk(renderer, c.ask(), hud.width, hud.height, hud.scale, hud.mouse);
        }
    }

    // In a run, the game over speaks of the run (out of it, OK to watch the others till it ends),
    // not of a quest's penalty and continuing. (A stock client keeps the quest's.)
    std::optional<GameOverWords> gameOver() const override
    {
        if (!tide_ || !tide_->run()) {
            return std::nullopt;
        }
        return GameOverWords{"The Tide Takes You", "You're out of this run. OK: watch the others till it ends. Quit: leave the game.", false};
    }

    bool fakeScreen(App&, std::string_view flag) override
    {
        if (!tide_) {
            return false;
        }
        if (flag == "--fake-tide-ask") {
            // The Tide lobby gate's difficulty question (Veteran set now).
            tide_->setAskForTest(1);
            return true;
        }
        if (flag != "--fake-tide" && flag != "--fake-tide-results") {
            return false;
        }
        // --fake-tide: an Undead Tide run's strip and a level-up's cards; --fake-tide-results: the
        // run's end.
        ts::tidemsg::Hud hud;
        hud.level = 9;
        hud.xp = 52;
        hud.need = 85;
        hud.seconds = 734;
        hud.dawn = 1200;
        hud.bossAt = 1080;
        hud.kills = 312;
        hud.stage = 2;
        hud.weapons = {{static_cast<std::uint8_t>(ts::TideWeapon::MagicMissile), 3, 36, 20},
                       {static_cast<std::uint8_t>(ts::TideWeapon::ForceOfNature), 2, 0, 0},
                       {static_cast<std::uint8_t>(ts::TideWeapon::Fireball), 1, 54, 40},
                       {static_cast<std::uint8_t>(ts::TideWeapon::Earthquake), 2, 600, 480}}; // (its wait still swept at the shot)
        hud.passives = {1, 0, 2, 0, 0, 1};
        ts::tidemsg::Offer offer;
        std::optional<ts::tidemsg::Results> results;
        if (flag == "--fake-tide") {
            offer.level = 10;
            offer.seconds = 10;
            offer.picks = {{ts::TidePick::Kind::Weapon, static_cast<std::uint8_t>(ts::TideWeapon::DeathRay), 1},
                           {ts::TidePick::Kind::Weapon, static_cast<std::uint8_t>(ts::TideWeapon::MagicMissile), 4},
                           {ts::TidePick::Kind::Passive, static_cast<std::uint8_t>(ts::TidePassive::Haste), 2}};
        } else {
            hud.boss = 1;
            results = ts::tidemsg::Results{.won = false, .boss = 1, .seconds = 2989, .stage = 3, .elites = 19, .bossName = "Lich Lord"};
            results->players = {{"Jack", 23, 1204, {{1, 5}, {3, 4}, {4, 3}, {9, 2}}},
                                {"Bramble", 18, 811, {{0, 5}, {2, 3}, {8, 2}}}};
        }
        tide_->setForTest(hud, std::move(offer), std::move(results));
        return true;
    }

private:
    TideWindow window_;
    nox::client::TideClient* tide_ = nullptr;
};

} // namespace

void registerModeUi_tide(std::vector<ModeUiEntry>& entries)
{
    entries.push_back({"tide", [](App& app) { return std::make_unique<TideUi>(app); }, {"--fake-tide", "--fake-tide-results", "--fake-tide-ask"}});
}

} // namespace noxsdl
