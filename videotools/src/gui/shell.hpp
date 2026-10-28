// The NoX Content Editor's window: the menus and top bar, and the docked panels (laid out like the
// map editor's, any of them able to float onto another monitor):
//
//   Content (left)       every file the game reads, as records, with one search over them all
//   Record (centre)      the selected record's fields, beside the Images and Sprites views
//   Preview, References  (right) what it looks or sounds like; what it names and what names it
//   Changes, Problems,   (bottom) every change against the game's files; what's missing or
//   Log                  broken; the session's messages
#pragma once

#include "content/game_files.hpp"
#include "content/map_usage.hpp"
#include "content/pack_edit.hpp"
#include "content/records.hpp"
#include "gui/audio_player.hpp"
#include "gui/catalog_view.hpp"
#include "gui/images_view.hpp"
#include "gui/session.hpp"

#include <SDL3/SDL.h>

#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace noxsdl::editor {
class ImGuiHost;
}

namespace videotools::gui {

/// The panels' window names (what the View menu lists).
namespace panel {
inline constexpr const char* kContent = "Content";
inline constexpr const char* kRecord = "Record";
inline constexpr const char* kImages = "Images";
inline constexpr const char* kSprites = "Sprites";
inline constexpr const char* kPreview = "Preview";
inline constexpr const char* kReferences = "References";
inline constexpr const char* kChanges = "Changes";
inline constexpr const char* kProblems = "Problems";
inline constexpr const char* kLog = "Log";
} // namespace panel

/// One change against the game's files, as the Changes panel lists it.
struct ChangeRow {
    std::string what;   ///< "video.bag #123 BLAH0001.pcx"
    std::string detail; ///< "replaced"
    std::string reach;  ///< "client-visible"
    std::function<void()> revert;
    std::function<void()> show;
};

/// A problem with the open files (the Problems panel).
struct ProblemRow {
    bool error = false; ///< else a warning
    std::string text;
    std::function<void()> show;
};

/// What the panels share: the open game's files and their records, the selection, the sound
/// player, the image views.
struct Workspace {
    Workspace(Session& s, CatalogView& c, ImagesView& i) : session(s), catalog(c), images(i) {}
    Session& session;
    CatalogView& catalog;
    ImagesView& images;
    content::GameFiles files;
    content::ContentIndex index;
    /// The content pack being edited (the text tables' changes).
    content::PackEditor pack;
    /// Which maps use which types (read in the background when a game opens).
    std::shared_future<content::MapUsage> mapUsage;
    std::optional<content::RecordRef> selected;
    AudioPlayer audio;
    /// Brings a panel to the front (by its window name), opening it if it was closed.
    std::function<void(const char*)> focus;
    /// The records the pack changes (the Content panel's "Changed only").
    std::function<std::vector<content::RecordRef>()> changedRecords;

    /// Selects a record (shown in Record, Preview and References; the Content browser scrolls to it).
    void select(const content::RecordRef& ref);
    bool scrollContentToSelection = false;
    /// The pack's new types listed in Content (Things) when they've changed since: once a frame,
    /// before the panels (which hold records of the index while they draw).
    void syncNewThings();
    std::vector<content::NewThing> newThingsListed;
    /// A type to select once it's listed (one just added).
    std::string selectAfterSync;
};

class Shell {
public:
    Shell(SDL_Window* window, SDL_Renderer* renderer, noxsdl::editor::ImGuiHost& host, std::filesystem::path settingsFile, Settings settings,
          bool screenshotRun);
    ~Shell();

    void openGame();
    void openPack(const std::filesystem::path& path);

    Session& session() { return session_; }
    CatalogView& catalog() { return catalog_; }
    ImagesView& images() { return images_; }
    Workspace& workspace() { return *ws_; }
    /// Brings a panel to the front.
    void focus(const char* panelName);
    /// Selects a record by "List:Name" (thing:GruntAxe, monster:GruntAxe, string:QuitMenu.wnd:Quit,
    /// tuning:FireballDelay ...) or a browser section by name; false if there's none.
    bool selectByText(std::string_view text);
    /// Fills the Content panel's search box.
    void setSearch(std::string_view text);

    /// Whether quitting may go ahead now (else it asks first).
    bool mayQuit();
    bool quitConfirmed() const { return quitConfirmed_; }

    void frame(double dt);

private:
    std::size_t edits() const;
    void menuBar();
    void topBar();
    void statusBar();
    void dockSpace();
    void buildDefaultLayout(unsigned int dockId);
    void askThenOpenGame();
    void catalogToolbar();
    void drawPanels(double dt);
    void collectChanges(std::vector<ChangeRow>& out);
    void collectProblems(std::vector<ProblemRow>& out);
    void applyTheme();
    // The content pack (mods\<id>\content): pick, make, save, export.
    void packBar();
    void openContentPack(const std::string& id);
    void savePack();
    void exportFull(const std::filesystem::path& dir);
    void packPopups();
    std::optional<content::RecordRef> recordOf(const content::FieldEdit& e) const;
    std::optional<content::RecordRef> soundRecord(const content::SoundEdit& e) const;

    SDL_Window* window_;
    noxsdl::editor::ImGuiHost& host_;
    std::filesystem::path settingsFile_;
    Settings settings_;
    Session session_;
    CatalogView catalog_;
    ImagesView images_;
    std::unique_ptr<Workspace> ws_;
    struct Panels;
    std::unique_ptr<Panels> panels_;
    char gameBuf_[1024] = {};
    bool quitConfirmed_ = false;
    bool screenshotRun_ = false;
    bool resetLayout_ = false;
    std::vector<std::string> wantFocus_;
    std::filesystem::path modsDir_;
    std::vector<std::string> packProblems_; ///< reading the open pack's patch files
    bool newPackOpen_ = false;
    bool saveAfterNewPack_ = false;
    char newPackBuf_[64] = {};
};

} // namespace videotools::gui
