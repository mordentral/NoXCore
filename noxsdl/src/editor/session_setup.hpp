// Hosting a collaborative map editor session: Host Custom > Map Editor and the editor's File > Host
// a Session show this dialog: the map (one of the user's, a copy of a stock map under a new name,
// or a new blank map) and the session's settings (who may edit, the sandbox's "players can die",
// the play-test mode, the live view, the automatic object publish, the game password and player
// limit). Host writes the map where the session publishes (never under a stock map's name) and
// hands the settings to main.cpp, which hosts it (LocalHost with an EditSessionDirector) and joins.
#pragma once

#include "noxcore/mapedit/new_map.hpp"
#include "noxcore/mapedit/session_proto.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace nox::things {
struct ThingDb;
}

namespace noxsdl::editor {

struct SessionSetup {
    /// The map to start from: a map's name (the maps folder's, else the game's); empty with newMap.
    std::string sourceMap;
    bool newMap = false;
    nox::mapedit::NewMapOptions newOptions;
    /// The session's map name: what is published (never a stock map's; a stock source is copied
    /// under it).
    std::string name;
    nox::mapedit::session::EditMode editMode = nox::mapedit::session::EditMode::Approve;
    bool playersCantDie = true;
    bool liveView = true;
    bool liveCreatures = true;
    bool autoPublish = true;
    std::uint32_t playTestMode = 0x100; ///< game flags' mode bits
    std::string password;               ///< the game's (the stock prompt for stock clients); empty: none
    int maxPlayers = 16;
    /// A mod project the session carries: the editor's open mod's folder (mods\<id>), saved first;
    /// empty: none (EditSessionDirector::Options::modFolder).
    std::filesystem::path modFolder;
};

/// The modes a session's play-test round can run (the game flags' mode bits), as the Server Settings
/// name them (the setup dialog's and the Session panel's lists).
struct SessionMode {
    std::uint32_t bits;
    const char* name;
};
inline constexpr SessionMode kSessionModes[] = {
    {0x100, "Arena"}, {0x400, "Elimination"}, {0x20, "Capture the Flag"}, {0x10, "King of the Realm"}, {0x40, "Flag Ball"}};

/// What the dialog says of where Host lists the session (online on XWIS only with the host
/// logged in there; else the LAN only): main.cpp's host_listing.hpp words it. Empty: nothing said.
struct SessionListingNote {
    std::string text;
    bool warning = false; ///< shown as a warning (not listed online)
};

/// A map the dialog lists.
struct SessionMapChoice {
    std::string name;
    std::filesystem::path path;
    bool stock = false;
};

/// The dialog (Dear ImGui), drawn inside the editor's frame.
class SessionSetupDialog {
public:
    void open(std::vector<SessionMapChoice> maps, SessionSetup defaults, SessionListingNote listing = {});
    void close() { open_ = false; }
    bool isOpen() const { return open_; }
    /// Draws it while open; true on the frame Host was pressed (`out` filled, the dialog closed).
    /// Cancel closes it (wasCancelled then says so, once).
    bool draw(SessionSetup& out);
    bool takeCancelled() { return std::exchange(cancelled_, false); }

private:
    bool open_ = false;
    bool cancelled_ = false;
    bool justOpened_ = false;
    std::vector<SessionMapChoice> maps_;
    SessionSetup s_;
    int selected_ = -1;
    char search_[64] = {};
    char name_[64] = {};
    char password_[16] = {};
    std::string problem_;
    SessionListingNote listing_;
};

/// Writes the session's map into `mapsDir` (<mapsDir>\<name>\<name>.map, .nxz): the source map
/// (found in `mapsDir`, else `gameMapsDir`) under the session's name, or a new map. Returns an
/// error text, empty on success. Refuses a stock map's name; leaves an existing map of the
/// session's name as it is when the source is that map itself.
std::string prepareSessionMap(const SessionSetup& s, const std::filesystem::path& mapsDir, const std::filesystem::path& gameMapsDir,
                              const nox::things::ThingDb& things);

} // namespace noxsdl::editor
