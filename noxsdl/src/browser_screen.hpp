// The server browser: every game noxcore's ServerBrowser finds (XWIS, LAN, a server list), with
// its ping, to pick one and join. A stopgap until the game's own menus are rebuilt from its .wnd
// files. The menus' Network (LAN play) lists no XWIS (setXwisListing(false)): the LAN's games,
// then the listed servers (servers.txt, --server) after them.
#pragma once

#include "text.hpp"

#include "noxcore/lobby/browser.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace noxsdl {

class BrowserScreen {
public:
    BrowserScreen(Font& font, nox::lobby::ServerBrowser::Options options);

    /// Starts a new search; with `xwisGames`, XWIS's games as a login just listed them.
    void refresh(std::uint64_t nowMs, const std::vector<nox::lobby::GameListing>* xwisGames = nullptr);
    /// Pumps the search.
    void update(std::uint64_t nowMs);
    /// Full screen, with a title and help line.
    void draw(SDL_Renderer* renderer, int width, int height);
    /// Inside `area` (output pixels), as the game list in the menus' game browser: the columns,
    /// the games, and the search messages at the bottom, text at `textSize` pixels.
    void drawFramed(SDL_Renderer* renderer, SDL_FRect area, int textSize);

    /// Handles input; returns the server to join ("a.b.c.d:port") when one is chosen. Mouse
    /// input only counts inside the area last drawn in.
    std::optional<std::string> handleEvent(const SDL_Event& event, std::uint64_t nowMs);
    /// The selected game's address, to join it from a button.
    std::optional<std::string> selectedServer() { return joinSelected(); }
    /// A game clicked in the list since the last call (the list's 0x4010: opens the
    /// description box for it), with where the click was (output pixels).
    struct Clicked {
        nox::lobby::ServerEntry entry;
        SDL_FPoint at{};
    };
    std::optional<Clicked> takeClicked() { return std::exchange(clicked_, std::nullopt); }
    /// "a.b.c.d:port" for a game.
    static std::string address(const nox::lobby::GameListing& game);
    /// The selected game's flags (its mode bits: 0x1000 a quest), 0 with none selected.
    std::uint32_t selectedGameFlags() const
    {
        return selected_ >= 0 && selected_ < static_cast<int>(rows_.size()) ? rows_[static_cast<std::size_t>(selected_)].game.gameFlags : 0u;
    }
    /// The selected game (null with none selected).
    const nox::lobby::ServerEntry* selectedEntry() const
    {
        return selected_ >= 0 && selected_ < static_cast<int>(rows_.size()) ? &rows_[static_cast<std::size_t>(selected_)] : nullptr;
    }
    /// The listed game at `server` ("a.b.c.d:port"; null: not listed).
    const nox::lobby::ServerEntry* entryFor(const std::string& server) const;
    /// Whether the list has `server` ("a.b.c.d:port") as a game that answered its ping but not
    /// the extended query: a stock (GAME.EXE) host. False for one not listed, or not answering.
    bool sawStock(const std::string& server) const;
    /// How many games there are, and whether it's still searching.
    std::size_t gameCount() const { return rows_.size(); }
    bool searching() const { return browser_.searching(); }

    /// A line for the status bar (e.g. why the last game ended).
    void setMessage(std::string message) { message_ = std::move(message); }
    /// List XWIS through the player's own login from now on (ServerBrowser::setXwisLogin).
    void setXwisLogin(std::string nick, std::string apgarPassword)
    {
        browser_.setXwisLogin(std::move(nick), std::move(apgarPassword));
    }
    /// Whether the searches list XWIS (ServerBrowser::setXwisListing): off for Network.
    void setXwisListing(bool on) { browser_.setXwisListing(on); }
    bool listsXwis() const { return browser_.listsXwis(); }
    /// What R / F5 do instead of a new search, when set (the menus list XWIS through the WOL
    /// login's own connection while it lasts).
    void setRefreshHandler(std::function<void(std::uint64_t nowMs)> handler) { refreshHandler_ = std::move(handler); }

private:
    struct Layout {
        int size = 16;        ///< text pixel size
        float rowHeight = 20;
        float top = 0;        ///< first row's top
        int visibleRows = 0;
    };
    Layout layout(int height);
    void drawRows(SDL_Renderer* renderer, SDL_FRect area, const Layout& l, bool framed);
    void sortRows();
    std::optional<std::string> joinSelected();
    /// "Searching ..." naming the places this search asks.
    std::string searchingText() const;

    Font& font_;
    nox::lobby::ServerBrowser browser_;
    std::vector<nox::lobby::ServerEntry> rows_; ///< sorted copy of the browser's list
    int selected_ = 0;
    int scroll_ = 0;
    std::string message_;
    std::vector<std::string> log_;
    std::uint64_t lastClickMs_ = 0;
    int lastClickRow_ = -1;
    int lastHeight_ = 600;
    SDL_FRect area_{0, 0, 800, 600}; ///< where it was last drawn
    Layout lastLayout_;
    std::function<void(std::uint64_t)> refreshHandler_;
    std::optional<Clicked> clicked_;
};

} // namespace noxsdl
