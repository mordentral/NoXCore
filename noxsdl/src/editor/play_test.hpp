// The map editor's play-test: the edited map hosted in this process on the local hosting path (as
// Game Modes': unlisted, no router port, no XWIS post), in its real game mode, with the sandbox's
// "players can't die" unless asked. main.cpp owns the game (LocalHost and the live session); the
// editor reaches it through these hooks.
//
// It plays in the editor's Map panel (play-in-editor: the game's frame drawn into a texture the
// panel shows, the editor working round it), or, as asked ("Play in Full Window"), over the whole
// window with the editor behind it (F2 comes back).
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace nox::server {
class GameHost;
}

namespace noxsdl::editor {

/// What kind of game the test hosts: a network game in `mode`, a solo game (the campaign's: the
/// map hosted as --solo hosts one, its one player the host's), or a quest (Solo Quest's: the chat
/// map first, OTQuest.rul beginning the quest, whose only map is the test's).
enum class PlayKind { Network, Solo, Quest };

struct PlayTestRequest {
    std::filesystem::path map;      ///< the working copy written for the test (<dir>/<name>/<name>.map)
    std::filesystem::path cacheDir; ///< where the client finds it (its map cache: <dir>); a solo or quest test's saves go under it too
    PlayKind kind = PlayKind::Network;
    std::uint32_t mode = 0x100;     ///< a network test's HostSettings::mode (arena 0x100, CTF 0x20, KotR 0x10, flag ball 0x40, elimination 0x400)
    int playerClass = -1;           ///< 0 warrior, 1 wizard, 2 conjurer; -1: the player's profile's
    bool playersCanDie = false;     ///< off: the sandbox's rule, health stops at 1
    bool fullWindow = false;        ///< over the whole window (the editor hidden till F2), not in the Map panel
    /// Jump in at the cursor: the player put here (world units) once it's in the game.
    std::optional<SDL_FPoint> startAt;
    /// A mod project's play-test (the Mod menu's Play Mod): the NoX Core mode hosted by its id (the
    /// mods folder read again first, so its saved mode.json counts); empty: none, `mode` alone.
    std::string modeId;
    /// With modeId: the mode's own map (its configure names one; else `map`, a map of the game's
    /// maps folder); false: `map`, the editor's working copy, over any the mode names.
    bool modeMap = false;
    /// With modeId in a session's private test: the mods folder the test reads (the editor's own
    /// copy of the session's mod); empty: mods\ beside noxcore.exe.
    std::filesystem::path modsDir;
};

/// The running test as the editor shows it (main.cpp's, each frame).
struct PlayTestView {
    SDL_Texture* texture = nullptr; ///< the game's last frame (in-panel play only; null otherwise)
    SDL_FRect area{};               ///< where it goes, in window coordinates (inside the Map panel, letterboxed)
    bool fullWindow = false;        ///< the test has (or had, till F2) the whole window
    bool captured = false;          ///< the game has the keyboard and mouse
    bool paused = false;            ///< the world stands (the toolbar's Pause)
    /// A collaborative session's game in the Map panel (the session bar's Play), not a play-test:
    /// it can't be paused or stopped from here; Stop (and F2) goes back to editing.
    bool session = false;
};

struct PlayTestHooks {
    /// Hosts and joins the test ("" when it's running, else why not).
    std::function<std::string(const PlayTestRequest&)> start;
    /// Back into the running test's game: the keyboard and mouse to it (and, full-window, the
    /// window, the editor hidden till F2 or the test's end).
    std::function<void()> resume;
    /// Ends the test (its host and session gone).
    std::function<void()> stop;
    std::function<bool()> running;
    /// fn on the test's game with its server's lock held, between its frames (hot reload).
    std::function<void(const std::function<void(nox::server::GameHost&)>&)> withGame;
    /// A line on the game's console (the reload's result while playing).
    std::function<void(const std::string&)> notify;
    /// The running test's frame and state for the Map panel and the toolbar.
    std::function<PlayTestView()> view;
    /// The keyboard and mouse to the game in the panel (true) or back to the editor (false).
    std::function<void(bool)> capture;
    /// The world stood still (true) or let go on (false).
    std::function<void(bool)> pause;
    /// Jump in at the cursor while it runs: the test's player put at this world point (as soon
    /// as it's in the game, if it isn't yet).
    std::function<void(SDL_FPoint)> jumpTo;
    /// The world point under a window point on the in-panel game's frame (its camera's); none
    /// off the frame or with no game drawn there.
    std::function<std::optional<SDL_FPoint>(float x, float y)> gameWorldAt;
    /// In a collaborative session: a private local test of the working copy, in a game of its own
    /// (its own process, its own host: unlisted, on the loopback, a free port), the session's game
    /// untouched ("" when it started, else why not).
    std::function<std::string(const PlayTestRequest&)> startPrivate;
    /// A console line to the running test's NoX Core mode (a mod's play-test: "mode reload keep"),
    /// run on the game's thread; what the mode answered handed to `answered` on the main thread a
    /// frame or so later (nothing: no mode, or it didn't take the line).
    std::function<void(const std::string& line, std::function<void(std::vector<std::string>)> answered)> modeCommand;

    bool available() const { return static_cast<bool>(start); }
};

} // namespace noxsdl::editor
