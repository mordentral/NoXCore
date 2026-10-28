// A Delving Mode run's map (noxcore server/delve_map.hpp, MSG_NOXCPP_MODE_F4): a map button
// left of the health and mana meters while a run is on, which opens the
// run drawn left to right. The act under way is its rows of rooms (fight, elite, ?, shop,
// campfire; one, then two, three, four and five side by side, then the guardian) along the
// middle, paths from each room to the two it leads on to, the way walked gold and ours ringed,
// the rooms it can't reach any more dimmed; at its guardian the way forks, the left gate's act
// smaller on a lane above and the right's below, each named with where it is ("the Lava
// Fields") and its air. The acts already done are lines under it,
// with the gate taken out of each. Each room an icon (the HD pack's "Delve/<Room>" images, when
// it has them; else a coloured disc with its letter). Stock players are told the same in text
// lines by the server. Not in the original game.
#pragma once

#include "sprite_cache.hpp"
#include "text.hpp"

#include "noxcore/server/delve_map.hpp"

#include <SDL3/SDL.h>

#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace noxsdl {

class DelveMapWindow {
public:
    explicit DelveMapWindow(Font* font) : font_(font) {}

    using Run = nox::server::delvemap::Run;

    /// A room's icon by its name ("Fight", "Elite", "Mystery", "Shop", "Campfire", "Guardian",
    /// "Treasure", "Ambush", "Shrine"); null: none (the disc and letter instead).
    using Icons = std::function<const Sprite*(std::string_view room)>;
    void setIcons(Icons icons) { icons_ = std::move(icons); }
    /// The server's deepest delves (HeadlessClient::delveRecords): with no run (the lobby) the
    /// button reads "TOP" and opens their table; in a run the map says the record to beat.
    using Records = std::vector<nox::server::delvemap::Record>;
    void setRecords(const Records* records) { records_ = records; }
    /// The level's bonus objective (HeadlessClient::objective, and its version: a new one's clock
    /// counts from its arrival), drawn as a line left of the button while there's a run.
    void setObjective(const nox::server::delvemap::Objective* objective, std::uint32_t version);

    /// Follows the server's map (each frame): the button while there's a run, the map put away
    /// when it ends.
    void update(const std::optional<Run>& run);
    bool open() const { return open_; }
    void setOpen(bool open) { open_ = open; }

    /// Draws the button (and the map, open) on a w x h view whose HUD scale is `scale`.
    void draw(SDL_Renderer* renderer, const std::optional<Run>& run, int w, int h, float scale, SDL_FPoint mouse);
    /// A click on the button opens or closes the map; one on the open map, or Escape while it's
    /// open, is the map's. Returns whether the event was taken (so goes no further).
    bool handleEvent(const SDL_Event& event, const std::optional<Run>& run, int w, int h, float scale);

private:
    struct Layout {
        SDL_FRect button{};
        SDL_FRect panel{};
        float node = 0;   ///< a room's diameter
        float step = 0;   ///< row to row, left to right
        float spread = 0; ///< room to room down a row of the act under way (a fork's: kForkScale of it)
        float lane = 0;   ///< the middle to a fork's middle
        float middle = 0; ///< the act's lane (output pixels)
        float left = 0;   ///< the first room's x
        int text = 0;   ///< the text's pixel size
        float line = 0;
    };
    Layout layout(const Run& run, int w, int h, float scale) const;
    SDL_FRect buttonRect(int w, int h, float scale) const;
    /// The records' table (no run): its panel.
    SDL_FRect recordsPanel(int w, int h, float scale) const;
    void drawButton(SDL_Renderer* renderer, const SDL_FRect& button, std::string_view label, float scale, SDL_FPoint mouse) const;
    void drawRecords(SDL_Renderer* renderer, int w, int h, float scale) const;
    bool hasRecords() const { return records_ && !records_->empty(); }

    Font* font_;
    Icons icons_;
    const Records* records_ = nullptr;
    const nox::server::delvemap::Objective* objective_ = nullptr;
    std::uint32_t objectiveVersion_ = 0;
    std::uint64_t objectiveAtMs_ = 0;
    void drawObjective(const SDL_FRect& button, float scale) const;
    bool hadRun_ = false;
    bool open_ = false;
};

} // namespace noxsdl
