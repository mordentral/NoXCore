// Delving Mode's UI in noxsdl (mode_ui.hpp): the run's map (its button by the meters), a
// campfire's offer and an altar's (delve_map_window, campfire_window, blessing_window), from what
// the Delve's client side was told (delve_client.hpp); its --fake-delve screens.
#include "app.hpp"
#include "blessing_window.hpp"
#include "campfire_window.hpp"
#include "delve_map_window.hpp"
#include "mode_ui.hpp"

#include "noxcore/client/delve_client.hpp"
#include "noxcore/server/delve_map.hpp"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace noxsdl {

namespace {

namespace dm = nox::server::delvemap;

class DelveUi final : public ModeUi {
public:
    explicit DelveUi(App& app)
        : map_(app.font.get())
        , campfire_(app.font.get())
        , blessing_(app.font.get())
    {
        App* a = &app;
        // The map's rooms' icons: the HD pack's "Delve/<Room>" (with HD images on), else discs.
        map_.setIcons([a](std::string_view room) -> const Sprite* {
            return a->sprites ? a->sprites->packNamed("Delve/" + std::string(room)) : nullptr;
        });
        campfire_.setSpellName([a](std::uint8_t spell) {
            const auto* s = a->things.findSpell(spell);
            return s && !s->title.empty() ? gameString(*a, s->title) : std::string();
        });
        campfire_.setSpellIcon([a](std::uint8_t spell) -> const Sprite* {
            const auto ic = a->hud ? a->hud->icon(spell, false) : std::nullopt;
            return ic && a->sprites ? a->sprites->get(*ic) : nullptr;
        });
    }

    void bind(nox::modes::ClientMode* side) override
    {
        delve_ = dynamic_cast<nox::client::DelveClient*>(side);
        // The map's records and objective are the side's: pointed at the new one's at once (or
        // none), never left on the last game's side, gone with its client (a mouse event before
        // the next update read them: the Load of a saved game after a game crashed in the map's
        // handleEvent).
        refreshMap();
    }

    bool handleEvent(App&, const SDL_Event& event, int viewWidth, int viewHeight, float hudScale) override
    {
        if (!delve_) {
            return false;
        }
        nox::client::DelveClient& c = *delve_;
        // The map: its button by the meters, the map while it's open (Escape closes it).
        if (map_.handleEvent(event, c.run(), viewWidth, viewHeight, hudScale)) {
            return true;
        }
        // A campfire's offer: a click on a spell strengthens it.
        {
            std::optional<std::uint8_t> pick;
            if (campfire_.handleEvent(event, c.campfire(), viewWidth, viewHeight, hudScale, pick)) {
                if (pick) {
                    c.upgradeSpell(*pick);
                }
                return true;
            }
        }
        // An altar's offer: a click on a blessing takes it.
        std::optional<dm::Blessing> pick;
        if (blessing_.handleEvent(event, c.blessingOffer(), viewWidth, viewHeight, hudScale, pick)) {
            if (pick) {
                c.takeBlessing(*pick);
            }
            return true;
        }
        return false;
    }

    void update(App&) override
    {
        if (!delve_) {
            return;
        }
        refreshMap();
        map_.update(delve_->run());
    }

    void drawHud(App&, SDL_Renderer* renderer, const HudFrame& hud) override
    {
        if (!delve_ || !hud.renderGui) {
            return;
        }
        map_.draw(renderer, delve_->run(), hud.width, hud.height, hud.scale, hud.mouse);
        campfire_.draw(renderer, delve_->campfire(), hud.width, hud.height, hud.scale, hud.mouse);
        blessing_.draw(renderer, delve_->blessingOffer(), hud.width, hud.height, hud.scale, hud.mouse);
    }

    bool fakeScreen(App&, std::string_view flag) override
    {
        if (!delve_) {
            return false;
        }
        nox::client::DelveClient& c = *delve_;
        if (flag == "--fake-records") {
            // A delve server's records table (no run: the lobby's), open.
            c.setRecordsForTest({dm::Record{14, 3, "2026-10-11", "Adventurer", {{"Rowan", 1}, {"Tamsin", 0}}},
                                 dm::Record{9, 2, "2026-10-10", "Adventurer", {{"Bramble", 2}}},
                                 dm::Record{6, 1, "2026-10-09", "Adventurer", {{"Nemo", 0}, {"Ivy", 1}, {"Quill", 2}}}});
            map_.setOpen(true);
            return true;
        }
        if (flag == "--fake-blessing") {
            // An altar's three blessings.
            c.setBlessingForTest({dm::BlessingOffer{dm::Blessing::Vigor, 1}, dm::BlessingOffer{dm::Blessing::Warding, 0},
                                  dm::BlessingOffer{dm::Blessing::Renewal, 0}});
            return true;
        }
        if (flag == "--fake-campfire") {
            // A campfire's offer of a few spells.
            c.setCampfireForTest({dm::Upgradable{0x1B, 1}, dm::Upgradable{0x2B, 2}, dm::Upgradable{0x10, 3}, dm::Upgradable{0x38, 1},
                                  dm::Upgradable{0x3C, 4}, dm::Upgradable{0x05, 2}});
            return true;
        }
        if (flag == "--fake-delve") {
            // A run's map, deep in act 2, its fork ahead, the map open.
            dm::Run run;
            run.depth = 9;
            run.room = 2;
            run.mystery = 0xFF;
            // (Rows by server::DelveRoom's numbers: fight 0, elite 1, mystery 2, shop 3, campfire 4,
            // blessing 6.)
            const std::array<std::vector<std::uint8_t>, 6> a = {{{0}, {2, 0}, {6, 0, 2}, {0, 3, 1, 2}, {4, 3, 4, 2, 4}, {5}}};
            c.setObjectiveForTest({.kind = 3, .done = 1, .of = 3, .text = "Break the seals (touch the orb pools)"});
            const std::array<std::vector<std::uint8_t>, 6> b = {{{0}, {0, 1}, {2, 3, 0}, {1, 0, 2, 0}, {3, 4, 3, 4, 3}, {5}}};
            run.past = {dm::Act{.biome = "Forest", .rows = b, .path = {0, 1, 1, 2, 3, 0}, .taken = 1}};
            run.act = dm::Act{.biome = "Lava", .rows = a, .path = {0, 1, 1}};
            run.fork = std::array{dm::Act{.biome = "Desert", .rows = b}, dm::Act{.biome = "Ice", .rows = a}};
            c.setRunForTest(std::move(run));
            map_.setOpen(true);
            return true;
        }
        return false;
    }

private:
    /// The map's records and objective from the bound side (none: cleared).
    void refreshMap()
    {
        map_.setRecords(delve_ ? &delve_->records() : nullptr);
        map_.setObjective(delve_ ? &delve_->objective() : nullptr, delve_ ? delve_->objectiveVersion() : 0u);
    }

    DelveMapWindow map_;
    CampfireWindow campfire_;
    BlessingWindow blessing_;
    nox::client::DelveClient* delve_ = nullptr;
};

} // namespace

void registerModeUi_delve(std::vector<ModeUiEntry>& entries)
{
    entries.push_back({"delve", [](App& app) { return std::make_unique<DelveUi>(app); },
                       {"--fake-delve", "--fake-campfire", "--fake-records", "--fake-blessing"}});
}

} // namespace noxsdl
