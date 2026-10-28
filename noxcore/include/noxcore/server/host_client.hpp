// The host's own client, as GAME.EXE's server calls it directly (no message) in a game it hosts
// itself: the solo game, and a listen server's host (game flag 2). Everything else between a
// server and its clients is the protocol, the host's own client included (GAME.EXE's player 0x1F
// talks to its server through in-memory message lists: netlist_receiveCli each client
// frame, netlist in the server's tick); these calls are the exceptions. noxsdl's
// in-process host implements them; with none attached (a dedicated server, the tests) they do
// nothing and a fade ends at once.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nox::server {

// What the in-process client implements; the server reaches it through World::hostClient
// (Server::setHostClient).
class HostClient {
public:
    virtual ~HostClient() = default;

    /// Blind: Draw_FadeInScreen(25, 0): to black, then the map no longer
    /// drawn (gameDisableMapDraw) unless a fade back has begun meanwhile.
    virtual void blind() = 0;
    /// UnBlind: the map drawn again, Draw_FadeOutScreen(25, 0, 0).
    virtual void unblind() = 0;
    /// Draw_FadeInScreen(frames, overMenu, done): to black over `frames` of the client's (`overMenu`:
    /// the fade's flags 5, over the GUI too); `done` run when black, or at once when the map isn't
    /// drawn. (`done` runs on the server's side, between its ticks.)
    virtual void fadeToBlack(int frames, bool overMenu, std::function<void()> done) = 0;
    /// gameDisableMapDraw: the world not drawn (a cinematic's black) or drawn again.
    virtual void setMapDrawn(bool drawn) = 0;
    /// The widescreen bars moving now (a WideScreen under way).
    virtual bool wideScreenMoving() const = 0;
    /// The bars' move in (0x10, 10) / out (0x10, 0x1E) and the HUD switch: the bars in (the HUD
    /// and its windows hidden) or out (back).
    virtual void wideScreen(bool on) = 0;
    /// cliPlayMapIntro: the map's intro (`immediate`: ImmediateBlind's).
    virtual void playMapIntro(bool immediate) = 0;
    /// EndGame: the ending (by `kind`), then back to the menu.
    virtual void endGame(int kind) = 0;
    /// Snd_MusicPush / Snd_MusicPop, and a solo game's MusicEvent (its host branch).
    virtual void musicPush() = 0;
    virtual void musicPop() = 0;
    virtual void music(int track, int volume) = 0;
    /// playDialogFile: a line's recorded voice (a Chat / ChatTimer string's).
    virtual void playVoice(std::string_view file, int volume) = 0;
    /// video_stopAllFades's client part (builtin 196): the fades from and
    /// to black ended (their callbacks run), the map intro's black ended, the map drawn. (Its
    /// Game_SetPaused(0) is the server's.)
    virtual void stopAllFades() = 0;

    // The client's state GAME.EXE's server reads.
    /// guiCursor: the widescreen's HUD hidden (the HUD switch's 1).
    virtual bool wideScreenOn() const = 0;
    /// A briefing shown (Gui_Brief_ShowLockScreen sets it, the fade back from it clears it).
    virtual bool briefingShown() const = 0;
    /// An award flying from the book to the bars (a solo game's: bookFillAll sets it, its flight
    /// clears it as it lands).
    virtual bool bookFlying() const { return false; }
    /// The inventory grid's room for (type, count): the cells of the client's inventory grid that could take `count`
    /// more of thing type `typeId` (empty, or a stack of it with room). No grid: kNoGrid.
    virtual int inventoryRoom(std::uint16_t typeId, int count) const
    {
        (void)typeId;
        (void)count;
        return kNoGrid;
    }
    static constexpr int kNoGrid = 0x7FFFFFFF;
    /// What plr_rwInventory reads of the host's inventory window: its things' net codes
    /// in cell order (rows 0-19, each row's columns 0-3, each cell's stack in order),
    /// the secondary weapon's (g_guiInvAltWeaponCell's) and the
    /// last quiver's (g_guiInvLastQuiverNetCode). 0: none.
    struct InventoryWindow {
        std::vector<std::uint16_t> cells;
        std::uint16_t secondaryWeapon = 0;
        std::uint16_t lastQuiver = 0;
    };
    /// Not overridden: no window (the cells empty, no secondary weapon nor quiver).
    virtual InventoryWindow inventoryWindow() const { return {}; }
    /// plr_rwInventory's read in a solo game, after the things are worn again: the
    /// window's secondary weapon and last quiver cleared directly (Gui_Inv_SetSecondaryWeapon(0,
    /// 0): the cell and the previous one forgotten, nothing sent; Gui_Inv_SetLastQuiver(0)),
    /// before the file's are told (MSG_REPORT_SECONDARY_WEAPON / MSG_REPORT_LAST_QUIVER).
    virtual void clearInventoryWeapons() {}

    // The minimap's memory, which a solo save keeps (the exe's server reads its client's).
    /// drawWalls's mark: the wall at grid (x, y) drawn by the client on the map the
    /// server has loaded (the exe stores dword 1 in the shared wall cell's field where WallMap's
    /// read put the map's byte; server_mapRWWallMap writes its low byte back).
    virtual bool wallDrawn(int x, int y) const
    {
        (void)x;
        (void)y;
        return false;
    }
    /// ObeliskXfer's test (XFerObelisk): the static drawable of `netCode` (a map thing's Extent |
    /// 0x8000, Cli_GetStaticDrawableByNetCode's) on the client's minimap list
    /// (cliFirstMinimapObj, g_cliMinimapMarkerList).
    virtual bool minimapMarked(std::uint16_t netCode) const
    {
        (void)netCode;
        return false;
    }
    // A solo game's save and load (solo_save.hpp): the host's client's own parts.
    /// What the server's save hands its client's sections (the Player record GAME.EXE's
    /// client and server share).
    struct ClientSave {
        std::uint8_t stage = 0;       ///< the briefing byte (World::briefingByte)
        std::uint8_t level = 1;       ///< g_cliLocalPlayer->Level
        std::uint8_t summonOrder = 4; ///< the player's SummonOrderAll (the GUI section's)
        std::string mapName;          ///< Map_GetCurrentName (the File Info's)
    };
    /// The save's start: Gui_CancelDragAndDrop and quickBarClose.
    virtual void saveStarting() {}
    /// The client's own sections (GUI 7, File Info 1, Music 12) and the closing 0
    /// id appended to `plr`; false when that fails (the save fails). Not overridden: nullopt,
    /// nothing written (solo_save.cpp then writes the closing id alone).
    virtual std::optional<bool> saveClientSections(const std::filesystem::path& plr, const ClientSave& info)
    {
        (void)plr;
        (void)info;
        return std::nullopt;
    }
    /// What Plr_LoadClientPlayerData read that's the server's to apply.
    struct ClientLoad {
        /// The GUI section's summon order: orderUnitLocal in a solo game, as soon as
        /// its section's read (set even when a later section then fails the load).
        std::optional<std::uint8_t> summonOrder;
    };
    /// Plr_LoadClientPlayerData (after the map intro): the bars cleared,
    /// then `plr`'s client sections read (spell bars, file info, music); false
    /// when the file can't be read or a section fails (the load fails). Not overridden: true.
    virtual bool loadClientSections(const std::filesystem::path& plr, ClientLoad& out)
    {
        (void)plr;
        (void)out;
        return true;
    }
    /// The solo load's end: the console's lines cleared and
    /// Gui_ClearCenteredMessages.
    virtual void gameLoaded() {}
};

} // namespace nox::server
