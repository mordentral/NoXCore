// The map editor's mod projects (the Mod menu): a mod's folder (mods\<id>\ beside noxcore.exe, the
// folder the game loads: mod_project.hpp) opened in the editor's dockspace as a window set:
//   - Mod: mode.json's fields as a form (name, description, players, stock clients, levels,
//     options, uiApi...), saved with its other fields kept;
//   - Mod Files: the folder's tree (server/, ui/, assets/), new / rename / delete inside it;
//   - Mod Code: the files open in tabs, in the code editor with Luau's colouring; Luau's analysis
//     (mod_analysis.hpp) checks them on a worker after a pause in typing and on save, into the
//     code's markers and the Problems panel;
//   - Mod Reference: the server toolbox and the UI API with their docs (mod_defs.hpp);
//   - Mod Preview: the UI run with no server (modes/script ui_preview.hpp), fed by ui/preview.luau.
// Play Mod hosts the project's mode here (the play-test's host, PlayTestRequest::modeId), on the
// mode's own map or the open one; saving a server script reloads the running mode keeping its
// state ("mode reload keep"), a UI script is sent again by the reload. Export Mod writes a zip of
// the folder (noxcore io/zip.hpp) for someone to unzip into their own mods\.
//
// In a collaborative session that carries a mod (its host had it open when it hosted), every
// member's Mod windows work on the session's mod: a copy of our own the session's files are
// written into; a save goes to the host as a working version (shown pending till the host's
// Publish Mod writes it into its mods\<id>), a file is held while it's typed in, and Play Mod is a
// private test of the copy in a process of its own.
#pragma once

#include "editor/code_editor.hpp"
#include "editor/mod_analysis.hpp"
#include "editor/mod_project.hpp"

#include "noxcore/client/edit_session_client.hpp"
#include "noxcore/modes/script/ui_preview.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace noxsdl {

class MapEditor;

class ModPanel {
public:
    explicit ModPanel(MapEditor& editor);
    ~ModPanel();
    ModPanel(const ModPanel&) = delete;
    ModPanel& operator=(const ModPanel&) = delete;

    /// Its commands (the Mod menu's), into the editor's registry.
    void registerCommands();
    /// The Mod menu.
    void menu();
    /// Its windows and dialogs.
    void draw();
    /// Every frame, without ImGui: the diagnostics' answers, the debounce, the save's reload.
    void tick();
    /// Its rows in the Problems panel: mode.json's problem, the Luau diagnostics.
    void problems();
    /// A mod file's editor has the keyboard (the map editor's shortcuts wait).
    bool hasKeyboard() const;
    /// Unsaved mod files or form (the editor's quit asks).
    bool dirty() const;
    /// Save, Don't Save or Cancel for the unsaved changes; `then` after either of the first two
    /// (the editor's Close and quit go on).
    void askThen(std::function<void()> then, std::function<void()> cancelled = {});
    /// The open mod's folder for a session about to be hosted (saved first): the session carries
    /// it. Empty: none open (or it's a session's own copy).
    std::filesystem::path folderForSession();
    /// An --editor-run step of its own (tests, the walkthroughs); false for any other. Steps:
    /// "mod-new:ID[,NAME]", "mod-open:ID" (a reference mod opens as a copy), "mod-open-file:PATH",
    /// "mod-type:TEXT" (\n a new line, \c a semicolon, \t a tab, into the current tab), "mod-goto:LINE[,COL]",
    /// "mod-save", "mod-name:TEXT", "mod-play[:MAP]" (MAP "open" the editor's map, else a map's
    /// name; none: the mode's own), "mod-preview", "mod-preview-send:NAME", "mod-preview-click:WINDOW,X,Y",
    /// "mod-export:PATH", "mod-reference:SEARCH", "mod-publish" (a session's host), "mod-close".
    bool runStep(const std::string& step);

private:
    struct Tab {
        std::string path;  ///< the project's ('/' separated)
        editor::CodeEditor code;
        std::string saved; ///< the text as last read or written
        bool dirty = false;
        std::uint64_t typedMs = 0;
    };

    // The project.
    bool openProject(const std::filesystem::path& folder);
    void closeProject();
    std::filesystem::path modsDir() const;
    void newMod(const std::string& id, const std::string& name);
    void openMod(const editor::ModListing& listing);
    bool saveAll();
    bool saveTab(Tab& tab);
    bool saveForm();
    void openFile(const std::string& path);
    Tab* current();
    void refreshFiles();
    void exportTo(const std::filesystem::path& zip);
    void playMod(const std::string& map);
    /// After a save: the running mode reloaded with its state kept (server scripts), the preview
    /// started again (UI scripts).
    void afterSave(bool server, bool ui);
    void requestAnalysis();
    void reloadPreview();

    // The windows.
    void formWindow();
    void filesWindow();
    void codeWindow();
    void referenceWindow();
    void previewWindow();
    void dialogs();
    bool onShortcut(int chord);
    void classify(editor::CodeEditor& code, const std::string& path);

    MapEditor& ed_;
    std::optional<editor::ModProject> project_;
    editor::ModManifest form_;      ///< the form as edited
    editor::ModManifest formSaved_; ///< as mode.json last had it
    /// The form's map prefix's problem (mapPrefixProblem), worked out when the prefix changes.
    const std::string& prefixProblem();
    std::optional<std::string> prefixFor_;
    std::string prefixProblem_;
    std::vector<editor::ModProject::Entry> files_;
    std::vector<std::unique_ptr<Tab>> tabs_;
    int currentTab_ = -1;
    int selectTab_ = -1;            ///< a tab brought to the front next frame
    std::string selectedFile_;      ///< the tree's selection

    // Diagnostics.
    editor::ModAnalysis analysis_;
    std::vector<editor::ModDiagnostic> diagnostics_;
    std::uint64_t analysisDueMs_ = 0;
    double analysisMs_ = 0;
    bool analysed_ = false;

    // The preview.
    nox::modes::script::UiPreview preview_;
    bool previewLoaded_ = false;
    std::vector<std::string> previewLog_;
    /// The mod's pictures as textures (by asset index), of the set `texturesOf_`.
    std::map<int, SDL_Texture*> previewTextures_;
    std::shared_ptr<const nox::modes::script::ModAssets> texturesOf_;
    void freePreviewTextures();
    /// (UI API 4) The game's icon for spell `index` (or an ability, tagged as the bars keep it) as
    /// a texture made once (the map editor's thumbnails), and its size; null: none.
    SDL_Texture* spellIconTexture(int index, float& w, float& h);
    /// (UI API 2) A key of the fixed set pressed over the preview's screen: to the window under the mouse.
    void previewKeys(float ux, float uy);
    /// (UI API 3) A press in one of the preview's windows (a drag's), as the game tells it.
    struct PreviewPress {
        std::string window;
        int button = 1;
        float fromX = 0, fromY = 0, toldX = 0, toldY = 0, wx = 0, wy = 0;
        bool dragging = false;
    };
    std::optional<PreviewPress> previewPress_;
    float previewWheel_ = 0; ///< (a fine wheel's part of a step)
    /// (UI API 3) The mouse over the preview's screen: drags, buttons let go, the wheel.
    void previewMouse(bool hovered, int clicked, float ux, float uy);

    // Play.
    bool playing_ = false;          ///< a test of this mod we started (the hooks say if it's still up)
    std::string playMap_;           ///< "" the mode's own, "open" the editor's map, else a map's name
    std::string lastReload_;

    // A collaborative session's mod: the session's files written into a copy of our own (sessionRoot()/<id>), opened as the project; saves go to the
    // host as working versions, pending till its owner publishes; a file held while typed in.
    nox::client::EditSessionClient* session() const;
    std::filesystem::path sessionRoot() const;
    void sessionTick();
    void attachSessionMod();
    void syncSessionMod();
    /// Our save of a file (or a delete: no text) to the session, after the copy's.
    void sessionSave(const std::string& path, const std::optional<std::string>& text);
    const nox::client::EditSessionClient::ModFileState* sessionFile(const std::string& path) const;
    /// Who holds the file in the session when it's another than us ("" for nobody or us).
    std::string heldByOther(const std::string& path) const;
    bool sessionMod_ = false;          ///< the open project is the session's mod's copy
    bool sessionDismissed_ = false;    ///< its Close Mod: not opened again till asked
    std::string sessionModId_;
    std::uint64_t sessionModRev_ = ~0ull;
    std::map<std::string, std::uint32_t> written_; ///< what the copy has: path -> working version
    /// Our saves the session hasn't answered yet: what we sent, and how many contents the file had
    /// taken then (the next one is the answer: ours, or its own back after a refusal).
    struct SentSave {
        std::string text;
        std::uint32_t arrivals = 0;
    };
    std::map<std::string, SentSave> sent_;
    bool askingForSession_ = false; ///< our unsaved mod asked about before the session's opens
    std::function<void()> askThen_;
    std::function<void()> askCancelled_;

    // The windows' state.
    bool showForm_ = true, showFiles_ = true, showCode_ = true, showReference_ = true, showPreview_ = false;
    int focusCode_ = 0;
    int focusPreview_ = 0;
    int focusOpened_ = 0;               ///< (a mod opened: its windows' tabs to the front of their docks, one a frame)
    int focusMap_ = 0;                  ///< (Play Mod: the Map panel, where the game is, to the front)
    std::uint64_t filesCheckMs_ = 0;    ///< when Mod Files next looks at the folder
    bool newOpen_ = false, openOpen_ = false, closeAsk_ = false;
    char newId_[32] = {};
    char newName_[40] = {};
    char fileName_[128] = {};
    std::string fileAction_;        ///< "new", "folder", "rename": the file dialog's
    bool fileDialog_ = false;
    std::string confirmDelete_;
    std::vector<editor::ModListing> listing_;
    char refSearch_[64] = {};
    int refHalf_ = 0;               ///< 0 server, 1 UI
    std::string status_;
    // A file picked in the system's dialog (Export: its callback may come from another thread).
    std::mutex pickedMutex_;
    std::optional<std::string> picked_;
};

} // namespace noxsdl
