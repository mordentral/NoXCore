// The map editor's mod projects: the Mod menu and windows (mod_panel.hpp).
#include "editor/mod_panel.hpp"

#include "editor/editor_text.hpp"
#include "editor/map_editor.hpp"
#include "editor/mod_defs.hpp"
#include "editor/object_tools.hpp"
#include "editor/panel_textures.hpp"
#include "editor/script_panel.hpp"
#include "editor/theme_colours.hpp"
#include "editor/ui_helpers.hpp"

#include "sprite_cache.hpp"

#include "noxcore/client/spell_bars.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/io/zip.hpp"
#include "noxcore/mapedit/mod_files.hpp"
#include "noxcore/modes/registry.hpp"
#include "noxcore/modes/script/manifest.hpp"
#include "noxcore/strings/l10n.hpp"
#include "noxcore/util/text.hpp"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <utility>

namespace fs = std::filesystem;

namespace noxsdl {

namespace {

using editor::help;
using editor::pathText;
using editor::u8path;
using nox::util::lowered;

constexpr std::uint64_t kAnalysisPauseMs = 600; ///< typing paused this long: the files checked
constexpr std::uintmax_t kExportCap = 64u << 20; ///< the most an exported mod holds
constexpr std::uint64_t kFilesCheckMs = 1000;    ///< how often Mod Files looks at the folder again

/// An InputText over a std::string (at most `cap` bytes).
bool inputText(const char* id, std::string& s, std::size_t cap, ImGuiInputTextFlags flags = 0)
{
    char buf[512];
    std::snprintf(buf, sizeof buf, "%s", s.substr(0, std::min(cap, sizeof buf - 1)).c_str());
    if (ImGui::InputText(id, buf, std::min(cap + 1, sizeof buf), flags)) {
        s = buf;
        return true;
    }
    return false;
}

/// The globals Luau scripts read besides the toolbox: coloured as builtins.
bool luauGlobal(std::string_view w)
{
    static const std::set<std::string_view> kGlobals = {"nox",      "math",   "string", "table",    "coroutine", "bit32",  "utf8",
                                                        "buffer",   "vector", "print",  "type",     "typeof",    "tostring", "tonumber",
                                                        "pairs",    "ipairs", "next",   "select",   "error",     "assert", "pcall",
                                                        "xpcall",   "rawget", "rawset", "rawequal", "rawlen",    "setmetatable",
                                                        "getmetatable", "unpack", "require", "self"};
    return kGlobals.contains(w);
}

/// A sample name or a window id from a step ("A choice" -> its index).
std::optional<std::size_t> sampleIndex(const nox::modes::script::UiPreview& p, std::string_view name)
{
    for (std::size_t i = 0; i < p.samples().size(); ++i) {
        if (lowered(p.samples()[i].name) == lowered(std::string(name))) {
            return i;
        }
    }
    return std::nullopt;
}

/// A step's text with its escapes: \n a new line, \c a semicolon (which would end the step), \t a tab.
std::string unescaped(std::string_view s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == 'n' || s[i + 1] == 'c' || s[i + 1] == 't')) {
            out += s[i + 1] == 'n' ? '\n' : s[i + 1] == 't' ? '\t' : ';';
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

} // namespace

ModPanel::ModPanel(MapEditor& editor)
    : ed_(editor)
{
    std::snprintf(newName_, sizeof newName_, "%s", NOX_TRC("editor.mod.new.default_name", "My Mode"));
    std::snprintf(newId_, sizeof newId_, "my_mode");
}

ModPanel::~ModPanel()
{
    freePreviewTextures();
}

void ModPanel::freePreviewTextures()
{
    for (auto& [index, t] : previewTextures_) {
        if (t) {
            editor::panelTextureForget(t);
            SDL_DestroyTexture(t);
        }
    }
    previewTextures_.clear();
    texturesOf_.reset();
}

SDL_Texture* ModPanel::spellIconTexture(int index, float& w, float& h)
{
    if (index <= 0 || !ed_.ctx_.sprites) {
        return nullptr;
    }
    const auto id = static_cast<std::uint32_t>(index);
    std::optional<nox::things::ImageRef> ref;
    if (nox::client::isTaggedAbility(id)) {
        const std::uint32_t a = nox::client::untagged(id);
        if (a >= 1 && a <= ed_.things_.abilities.size()) {
            ref = ed_.things_.abilities[a - 1].icons[0];
        }
    } else if (const auto* s = ed_.things_.findSpell(id)) {
        ref = s->icon;
    }
    if (!ref) {
        return nullptr;
    }
    SDL_Texture* t = ed_.thumb("spellicon" + std::to_string(index), [&](int& tw, int& th) -> std::vector<std::uint8_t> {
        const auto* d = ed_.ctx_.sprites->decoded(ed_.ctx_.sprites->indexOf(*ref));
        if (!d) {
            return {};
        }
        tw = d->width;
        th = d->height;
        return d->rgba;
    });
    if (t) {
        SDL_GetTextureSize(t, &w, &h);
    }
    return t;
}

SDL_Texture* ModPanel::gameImageTexture(std::string_view name, std::int32_t index, float& w, float& h, float& ox, float& oy)
{
    // (API 6) One of the game's pictures (by name or video.bag index), as the original art has it.
    if (!ed_.ctx_.sprites) {
        return nullptr;
    }
    const std::int32_t i = gamePictureIndex(ed_.things_, *ed_.ctx_.sprites, name, 0, index);
    if (i < 0) {
        return nullptr;
    }
    SDL_Texture* t = ed_.thumb("gameimage#" + std::to_string(i), [&](int& tw, int& th) -> std::vector<std::uint8_t> {
        const auto* d = ed_.ctx_.sprites->decoded(i);
        if (!d) {
            return {};
        }
        tw = d->width;
        th = d->height;
        gameImageOffsets_[i] = {static_cast<float>(d->offsetX), static_cast<float>(d->offsetY)};
        return d->rgba;
    });
    if (t) {
        SDL_GetTextureSize(t, &w, &h);
        const auto it = gameImageOffsets_.find(i);
        ox = it != gameImageOffsets_.end() ? it->second.first : 0;
        oy = it != gameImageOffsets_.end() ? it->second.second : 0;
    }
    return t;
}

// --- Commands ----------------------------------------------------------------------------------

void ModPanel::registerCommands()
{
    using editor::Command;
    using editor::Shortcut;
    const auto add = [&](const char* id, const char* label, const char* helpText, std::function<void()> run, std::function<bool()> enabled = {},
                         std::function<bool()> checked = {}, const char* category = nullptr) {
        ed_.commands_.add(Command{id, label, category ? category : NOX_TRC("editor.category.mod", "Mod"), Shortcut{}, helpText, std::move(run),
                                  std::move(enabled), std::move(checked)});
    };
    const auto available = [this] { return !ed_.ctx_.modsDir.empty(); };
    const auto open = [this] { return project_.has_value(); };
    add("mod.new", NOX_TRC("editor.cmd.mod.new.label", "New Mod..."),
        NOX_TRC("editor.cmd.mod.new.help",
                "A new mod from the template (mode.json, a server script, a UI script and its preview) in mods\\ beside noxcore.exe: playable as soon as it's saved."),
        [this] { newOpen_ = true; }, available);
    add("mod.open", NOX_TRC("editor.cmd.mod.open.label", "Open Mod..."),
        NOX_TRC("editor.cmd.mod.open.help", "A mod in mods\\ (a reference mod opens as a copy of its own)."), [this] {
        listing_ = editor::listMods(modsDir());
        openOpen_ = true;
    }, available);
    add("mod.save", NOX_TRC("editor.cmd.mod.save.label", "Save Mod"),
        NOX_TRC("editor.cmd.mod.save.help",
                "Save the mod's open files and its form (mode.json). A running test of the mod reloads its server scripts, keeping their state."),
        [this] { saveAll(); }, open);
    add("mod.play", NOX_TRC("editor.cmd.mod.play.label", "Play Mod"),
        NOX_TRC("editor.cmd.mod.play.help",
                "Host the mod here and play it (on its own map, or the open one: the Play Map setting below), as the map's play-test does."),
        [this] { playMod(playMap_); },
        [this, open] { return open() && ed_.ctx_.playTest.available() && (!ed_.inSession() || (sessionMod_ && ed_.ctx_.playTest.startPrivate)); });
    add("mod.publish", NOX_TRC("editor.cmd.mod.publish.label", "Publish Mod"),
        NOX_TRC("editor.cmd.mod.publish.help",
                "(The session's host) Write the session's mod files as they stand now into your mods\\ folder: the pending changes become the mod."),
        [this] {
            if (auto* s = session()) s->modPublish();
        },
        [this] { return sessionMod_ && session() && session()->isOwner(); });
    add("mod.sessionOpen", NOX_TRC("editor.cmd.mod.session_open.label", "Open the Session's Mod"),
        NOX_TRC("editor.cmd.mod.session_open.help", "The mod the session carries in the Mod windows again (after Close Mod)."), [this] {
        sessionDismissed_ = false;
        sessionModId_.clear(); // (opened afresh at the next frame)
    }, [this] { return session() && session()->mod() && !sessionMod_; });
    add("mod.export", NOX_TRC("editor.cmd.mod.export.label", "Export Mod..."),
        NOX_TRC("editor.cmd.mod.export.help", "Write the mod's folder as a zip for someone to unzip into their own mods\\ (nothing is uploaded)."), [this] {
        static const SDL_DialogFileFilter filters[] = {{NOX_TRC("editor.mod.zip_filter", "Zip archive"), "zip"}};
        static std::string start;
        start = pathText(modsDir().parent_path() / (project_->folderName() + ".zip"));
        editor::saveFileDialog(filePick_, ed_.ctx_.window, filters, 1, start.c_str());
    }, open);
    add("mod.close", NOX_TRC("editor.cmd.mod.close.label", "Close Mod"),
        NOX_TRC("editor.cmd.mod.close.help", "Close the mod's windows (unsaved changes are asked about)."), [this] {
        askThen([this] {
            sessionDismissed_ = sessionMod_; // (the session's: not opened again till asked)
            closeProject();
        });
    }, open);
    add("mod.preview", NOX_TRC("editor.cmd.mod.preview.label", "Preview UI"),
        NOX_TRC("editor.cmd.mod.preview.help", "Run the mod's UI here with no server, fed by ui/preview.luau's test messages."), [this] {
        showPreview_ = true;
        focusPreview_ = 2;
        reloadPreview();
    }, [this, open] { return open() && !form_.uiScript.empty(); });
    const char* window = NOX_TRC("editor.category.window", "Window");
    add("window.modForm", NOX_TRC("editor.cmd.window.mod_form.label", "Mod"),
        NOX_TRC("editor.cmd.window.mod_form.help", "Shows or hides the mod's form (mode.json)."), [this] { showForm_ = !showForm_; }, open,
        [this] { return showForm_; }, window);
    add("window.modFiles", NOX_TRC("editor.cmd.window.mod_files.label", "Mod Files"),
        NOX_TRC("editor.cmd.window.mod_files.help", "Shows or hides the mod's files."), [this] { showFiles_ = !showFiles_; }, open,
        [this] { return showFiles_; }, window);
    add("window.modCode", NOX_TRC("editor.cmd.window.mod_code.label", "Mod Code"),
        NOX_TRC("editor.cmd.window.mod_code.help", "Shows or hides the mod's open files."), [this] { showCode_ = !showCode_; }, open,
        [this] { return showCode_; }, window);
    add("window.modReference", NOX_TRC("editor.cmd.window.mod_reference.label", "Mod Reference"),
        NOX_TRC("editor.cmd.window.mod_reference.help", "Shows or hides the server toolbox and the UI API."), [this] { showReference_ = !showReference_; }, {},
        [this] { return showReference_ && project_.has_value(); }, window);
    add("window.modPreview", NOX_TRC("editor.cmd.window.mod_preview.label", "Mod Preview"),
        NOX_TRC("editor.cmd.window.mod_preview.help", "Shows or hides the UI preview."), [this] { showPreview_ = !showPreview_; }, open,
        [this] { return showPreview_; }, window);
}

void ModPanel::menu()
{
    if (ed_.ctx_.modsDir.empty() || !ImGui::BeginMenu(NOX_TRI("editor.menu.mod", "Mod"))) {
        return;
    }
    for (const char* id : {"mod.new", "mod.open", "mod.save"}) ed_.menuItem(id);
    if (session() && session()->mod()) {
        for (const char* id : {"mod.publish", "mod.sessionOpen"}) ed_.menuItem(id);
    }
    ImGui::Separator();
    ed_.menuItem("mod.play");
    if (ImGui::BeginMenu(NOX_TRI("editor.mod.play_map", "Play Map"), project_.has_value())) {
        if (ImGui::MenuItem(NOX_TRI("editor.mod.play_map.own", "The mode's own"), nullptr, playMap_.empty())) playMap_.clear();
        help(NOX_TRC("editor.mod.play_map.own_help", "The map the mode's configure names (Estate when it names none)."));
        if (ImGui::MenuItem(NOX_TRI("editor.mod.play_map.open", "The open map"), nullptr, playMap_ == "open")) playMap_ = "open";
        help(NOX_TRC("editor.mod.play_map.open_help", "The map open in the editor, written as Play writes it, whatever map the mode names."));
        ImGui::EndMenu();
    }
    ed_.menuItem("mod.preview");
    ImGui::Separator();
    for (const char* id : {"mod.export", "mod.close"}) ed_.menuItem(id);
    ImGui::Separator();
    for (const char* id : {"window.modForm", "window.modFiles", "window.modCode", "window.modReference", "window.modPreview"}) ed_.menuItem(id);
    ImGui::EndMenu();
}

// --- The project -------------------------------------------------------------------------------

fs::path ModPanel::modsDir() const
{
    return ed_.ctx_.modsDir;
}

bool ModPanel::openProject(const fs::path& folder)
{
    std::string error;
    auto p = editor::ModProject::open(folder, error);
    if (!p) {
        ed_.errorText_ = nox::l10n::format(NOX_TR("editor.mod.err.open", "The mod can't be opened:\n{error}"), {{"error", error}});
        return false;
    }
    closeProject();
    project_ = std::move(p);
    form_ = formSaved_ = project_->form();
    prefixFor_.reset();
    refreshFiles();
    showForm_ = showFiles_ = showCode_ = true;
    // (Each shares a dock with the map's panels: brought to the front, the code last, so it has the
    // keyboard. One a frame: a dock shows the tab of the window focused last.)
    focusOpened_ = 4;
    for (const std::string& path : {form_.serverScript, form_.uiScript}) {
        if (!path.empty() && project_->read(path)) {
            openFile(path);
        }
    }
    if (!tabs_.empty()) {
        selectTab_ = 0;
    }
    requestAnalysis();
    previewLoaded_ = false;
    if (showPreview_) {
        reloadPreview();
    }
    ed_.status(nox::l10n::format(NOX_TR("editor.mod.status.opened", "Mod {name} ({path})"), {{"name", project_->folderName()}, {"path", pathText(folder)}}));
    return true;
}

void ModPanel::closeProject()
{
    project_.reset();
    tabs_.clear();
    currentTab_ = -1;
    files_.clear();
    diagnostics_.clear();
    previewLog_.clear();
    previewLoaded_ = false;
    selectedFile_.clear();
    analysed_ = false;
    sessionMod_ = false;
    written_.clear();
}

void ModPanel::newMod(const std::string& id, const std::string& name)
{
    std::string error;
    if (!editor::ModProject::create(modsDir(), id, name, error)) {
        ed_.errorText_ = nox::l10n::format(NOX_TR("editor.mod.err.create", "The mod can't be made:\n{error}"), {{"error", error}});
        return;
    }
    openProject(modsDir() / id);
}

void ModPanel::openMod(const editor::ModListing& listing)
{
    if (!listing.reference) {
        openProject(listing.folder);
        return;
    }
    // A reference mod: never edited in place (a build copies it over again); a copy of its own.
    const std::string id = editor::freeCopyId(modsDir(), listing.id);
    std::string name = listing.name;
    if (const auto at = name.find(" (reference)"); at != std::string::npos) {
        name.erase(at);
    }
    name = (name + " (copy)").substr(0, 32);
    std::string error;
    if (!editor::ModProject::copyOf(listing.folder, modsDir(), id, name, error)) {
        ed_.errorText_ = nox::l10n::format(NOX_TR("editor.mod.err.copy_reference", "The reference mod can't be copied:\n{error}"), {{"error", error}});
        return;
    }
    if (openProject(modsDir() / id)) {
        ed_.status(nox::l10n::format(NOX_TR("editor.mod.status.reference_copy", "{mod} is a reference mod: opened as a copy, mods\\{copy}"),
                                     {{"mod", listing.id}, {"copy", id}}));
    }
}

void ModPanel::refreshFiles()
{
    files_ = project_ ? project_->files() : std::vector<editor::ModProject::Entry>{};
}

ModPanel::Tab* ModPanel::current()
{
    return currentTab_ >= 0 && currentTab_ < static_cast<int>(tabs_.size()) ? tabs_[static_cast<std::size_t>(currentTab_)].get() : nullptr;
}

void ModPanel::classify(editor::CodeEditor& code, const std::string& path)
{
    code.setLanguage(path.ends_with(".luau") || path.ends_with(".lua") ? editor::CodeLanguage::Luau : editor::CodeLanguage::NoxScript);
    const editor::ModHalf half = path.starts_with("ui/") ? editor::ModHalf::Ui : editor::ModHalf::Server;
    code.classify = [](std::string_view w) { return luauGlobal(w) ? editor::Tok::Builtin : editor::Tok::Plain; };
    code.hoverText = [half](std::string_view w) -> std::string {
        for (const editor::ModDef& d : editor::modDefs(half)) {
            if (d.name == w) {
                return (d.kind == editor::ModDef::Kind::Member ? "nox." : "") + d.name + (d.kind == editor::ModDef::Kind::Hook ? "" : ": ") +
                       (d.kind == editor::ModDef::Kind::Hook ? d.signature.substr(d.name.size()) : d.signature) + "\n\n" + d.doc;
            }
        }
        return {};
    };
    code.shortcut = [this](int chord) { return onShortcut(chord); };
}

void ModPanel::openFile(const std::string& path)
{
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i]->path == path) {
            selectTab_ = static_cast<int>(i);
            showCode_ = true;
            return;
        }
    }
    if (path == "mode.json") {
        showForm_ = true; // (mode.json is the form's: edited there, its other fields kept)
        ImGui::SetWindowFocus(NOX_TRI("editor.window.mod", "Mod"));
        return;
    }
    const auto text = project_ ? project_->read(path) : std::nullopt;
    if (!text) {
        ed_.status(nox::l10n::format(NOX_TR("editor.mod.status.cant_read", "{path} can't be read (over 1 MB, or not a file)"), {{"path", path}}));
        return;
    }
    auto tab = std::make_unique<Tab>();
    tab->path = path;
    tab->saved = *text;
    classify(tab->code, path);
    tab->code.setText(*text, false);
    tabs_.push_back(std::move(tab));
    selectTab_ = static_cast<int>(tabs_.size()) - 1;
    showCode_ = true;
    requestAnalysis();
}

bool ModPanel::saveTab(Tab& tab)
{
    std::string error;
    const std::string text = tab.code.text();
    if (!project_->write(tab.path, text, error)) {
        ed_.errorText_ = nox::l10n::format(NOX_TR("editor.mod.err.save_file", "The mod's {path} can't be saved:\n{error}"), {{"path", tab.path}, {"error", error}});
        return false;
    }
    tab.saved = text;
    tab.dirty = false;
    if (sessionMod_) {
        sessionSave(tab.path, text); // (to the session: pending till its host publishes)
    }
    return true;
}

const std::string& ModPanel::prefixProblem()
{
    if (!prefixFor_ || *prefixFor_ != form_.mapPrefix) {
        prefixFor_ = form_.mapPrefix;
        // (A session's mod: its copy isn't in mods\; a folder there of its id is the same mod.)
        const fs::path self = sessionMod_ ? modsDir() / project_->folderName() : project_ ? project_->folder() : fs::path();
        prefixProblem_ = project_ ? editor::mapPrefixProblem(form_.mapPrefix, modsDir(), self, form_.id, nox::modes::registry()) : std::string();
    }
    return prefixProblem_;
}

bool ModPanel::saveForm()
{
    if (const std::string& why = prefixProblem(); !why.empty()) {
        ed_.errorText_ = nox::l10n::format(NOX_TR("editor.mod.err.prefix_not_saved", "The mod's mode.json isn't saved: its map prefix {prefix}: {why}."),
                                           {{"prefix", form_.mapPrefix}, {"why", why}});
        return false;
    }
    project_->setForm(form_);
    std::string error;
    if (!project_->saveManifest(error)) {
        ed_.errorText_ = nox::l10n::format(NOX_TR("editor.mod.err.save_manifest", "The mod's mode.json can't be saved:\n{error}"), {{"error", error}});
        return false;
    }
    formSaved_ = form_;
    if (sessionMod_) {
        sessionSave("mode.json", project_->manifestText());
    }
    return true;
}

bool ModPanel::saveAll()
{
    if (!project_) {
        return false;
    }
    bool server = false, ui = false, ok = true;
    if (!(form_ == formSaved_)) {
        ok = saveForm() && ok;
    }
    for (auto& tab : tabs_) {
        if (tab->dirty || tab->code.text() != tab->saved) {
            if (saveTab(*tab)) {
                (tab->path.starts_with("ui/") ? ui : server) = true;
            } else {
                ok = false;
            }
        }
    }
    refreshFiles();
    afterSave(server, ui);
    if (ok) {
        ed_.status(lastReload_.empty()
                       ? nox::l10n::format(NOX_TR("editor.mod.status.saved", "Mod {name} saved"), {{"name", project_->folderName()}})
                       : nox::l10n::format(NOX_TR("editor.mod.status.saved_reload", "Mod {name} saved; {reload}"),
                                           {{"name", project_->folderName()}, {"reload", lastReload_}}));
    }
    return ok;
}

void ModPanel::afterSave(bool server, bool ui)
{
    requestAnalysis();
    analysisDueMs_ = 0;
    auto& hooks = ed_.ctx_.playTest;
    const bool running = playing_ && hooks.running && hooks.running();
    if (running && (server || ui) && hooks.modeCommand) {
        // The running mode's scripts read again, its state kept (save() to restore()); the reload
        // sends its UI again when that changed (ScriptedMode's load).
        lastReload_ = NOX_TR("editor.mod.reloading", "reloading the running mode...");
        hooks.modeCommand("mode reload keep", [this](std::vector<std::string> lines) {
            lastReload_ = lines.empty() ? NOX_TR("editor.mod.reload_no_answer", "the running mode didn't answer the reload") : lines.front();
            ed_.status(lastReload_);
            if (ed_.ctx_.playTest.notify) {
                ed_.ctx_.playTest.notify(lastReload_);
            }
        });
    }
    if (ui && previewLoaded_) {
        reloadPreview();
    }
}

void ModPanel::requestAnalysis()
{
    if (!project_) {
        return;
    }
    editor::ModAnalysis::Files files;
    for (const auto& e : files_) {
        if (!e.folder && e.path.ends_with(".luau") && !e.path.starts_with("assets/")) {
            if (const auto text = project_->read(e.path)) {
                files[e.path] = *text;
            }
        }
    }
    for (const auto& tab : tabs_) {
        if (tab->path.ends_with(".luau")) {
            files[tab->path] = tab->code.text(); // (as it stands, saved or not)
        }
    }
    analysis_.request(std::move(files));
}

void ModPanel::reloadPreview()
{
    if (!project_) {
        return;
    }
    const auto text = [&](const std::string& path) -> std::optional<std::string> {
        for (const auto& tab : tabs_) {
            if (tab->path == path) return tab->code.text();
        }
        return project_->read(path);
    };
    const auto ui = form_.uiScript.empty() ? std::nullopt : text(form_.uiScript);
    const auto samples = text("ui/preview.luau");
    preview_.setTextWidth([](std::string_view s) {
        ImFont* f = ImGui::GetFont();
        return f ? static_cast<int>(f->CalcTextSizeA(10, FLT_MAX, 0, s.data(), s.data() + s.size()).x) : static_cast<int>(s.size()) * 6;
    });
    // The mod's pictures and sounds, decoded as a host decodes them (a bad one: none, and why).
    auto assets = nox::modes::script::loadModAssets(project_->folder() / "assets");
    preview_.setThings(ed_.ctx_.things);
    preview_.setGameImageIndex([this](std::string_view name, int frame) -> std::optional<std::int32_t> {
        const std::int32_t i = ed_.ctx_.sprites ? gamePictureIndex(ed_.things_, *ed_.ctx_.sprites, name, frame) : -1;
        return i >= 0 ? std::optional<std::int32_t>(i) : std::nullopt;
    });
    preview_.setGameImageSize([this](std::int32_t index) -> std::optional<nox::modes::script::UiGameImageSize> {
        float w = 0, h = 0, ox = 0, oy = 0;
        if (index < 0 || !gameImageTexture({}, index, w, h, ox, oy)) {
            return std::nullopt;
        }
        return nox::modes::script::UiGameImageSize{static_cast<int>(w), static_cast<int>(h), static_cast<int>(ox), static_cast<int>(oy)};
    });
    preview_.load(form_.id, form_.name, ui.value_or(""), samples ? std::optional<std::string_view>(*samples) : std::nullopt, form_.uiApi,
                  assets.assets);
    previewLoaded_ = true;
    previewLog_.clear();
    if (!assets.assets) {
        previewLog_.push_back(nox::l10n::format(NOX_TR("editor.mod.preview.assets_error", "assets: {error}"), {{"error", assets.error}}));
    }
    if (!ui) {
        previewLog_.push_back(form_.uiScript.empty()
                                  ? NOX_TR("editor.mod.preview.no_ui", "The mod has no UI script (mode.json's \"ui\").")
                                  : nox::l10n::format(NOX_TR("editor.mod.preview.ui_unreadable", "{path} can't be read."), {{"path", form_.uiScript}}));
    }
}

void ModPanel::playMod(const std::string& map)
{
    if (!project_) {
        return;
    }
    auto& hooks = ed_.ctx_.playTest;
    if (!hooks.available()) {
        ed_.status(NOX_TR("editor.mod.status.no_playtest", "Play-test isn't available here"));
        return;
    }
    if (ed_.inSession()) {
        // In a session: a private test of the session's mod as our copy has it (our unsaved
        // changes saved first), in a process of its own; the session's game untouched.
        if (!sessionMod_ || !hooks.startPrivate) {
            ed_.status(NOX_TR("editor.mod.status.session_no_mod", "In a session, Play Mod plays the session's mod (it carries none here)"));
            return;
        }
        if (!saveAll()) {
            return;
        }
        if (const std::string problem = project_->manifestProblem(); !problem.empty()) {
            ed_.errorText_ = nox::l10n::format(NOX_TR("editor.mod.err.doesnt_load", "The mod doesn't load:\n{problem}"), {{"problem", problem}});
            return;
        }
        if (map == "open") {
            ed_.scriptPanel_->playMod(form_.id, sessionRoot());
            return;
        }
        const std::string name = map.empty() ? "Estate" : map;
        editor::PlayTestRequest req;
        req.map = nox::resolveNoCase(ed_.ctx_.gameDir / "maps" / u8path(name) / u8path(name + ".map"));
        req.cacheDir = !ed_.ctx_.playTestDir.empty() ? ed_.ctx_.playTestDir : ed_.ctx_.prefDir / "playtest";
        req.modeId = form_.id;
        req.modeMap = true;
        req.modsDir = sessionRoot();
        req.playersCanDie = true;
        if (const std::string error = hooks.startPrivate(req); !error.empty()) {
            ed_.errorText_ = nox::l10n::format(NOX_TR("editor.mod.err.private_test", "The mod's private test didn't start:\n{error}"), {{"error", error}});
            return;
        }
        ed_.status(nox::l10n::format(NOX_TR("editor.mod.status.private_test", "A private test of the session's mod {id} in its own window (the session goes on here)"),
                                     {{"id", form_.id}}));
        return;
    }
    if (hooks.running && hooks.running()) {
        if (playing_) {
            if (hooks.resume) hooks.resume();
            return;
        }
        ed_.status(NOX_TR("editor.mod.status.playtest_running", "A play-test is running: stop it first"));
        return;
    }
    if (!saveAll()) {
        return;
    }
    if (const std::string problem = project_->manifestProblem(); !problem.empty()) {
        ed_.errorText_ = nox::l10n::format(NOX_TR("editor.mod.err.doesnt_load", "The mod doesn't load:\n{problem}"), {{"problem", problem}});
        return;
    }
    const std::string id = form_.id;
    if (map == "open") {
        ed_.scriptPanel_->playMod(id);
    } else {
        // The mode's own map (its configure's), else a map of the game's by name (Estate).
        const std::string name = map.empty() ? "Estate" : map;
        editor::PlayTestRequest req;
        req.map = nox::resolveNoCase(ed_.ctx_.gameDir / "maps" / u8path(name) / u8path(name + ".map"));
        req.cacheDir = !ed_.ctx_.playTestDir.empty() ? ed_.ctx_.playTestDir : ed_.ctx_.prefDir / "playtest";
        req.modeId = id;
        req.modeMap = true;
        req.playersCanDie = true; // (the mod's own rules: no sandbox)
        req.fullWindow = ed_.settings_.playFullWindow;
        if (const std::string error = hooks.start(req); !error.empty()) {
            ed_.errorText_ = nox::l10n::format(NOX_TR("editor.mod.err.playtest", "The mod's play-test didn't start:\n{error}"), {{"error", error}});
            return;
        }
    }
    playing_ = hooks.running && hooks.running();
    lastReload_.clear();
    if (playing_) {
        focusMap_ = 2; // (the game is drawn in the Map panel: in front of a tab sharing its dock, the preview's)
        ed_.status(map == "open"   ? nox::l10n::format(NOX_TR("editor.mod.status.playing_open", "Playing mod {id} on the open map: saving a server script reloads it, its state kept"),
                                                       {{"id", id}})
                   : map.empty() ? nox::l10n::format(NOX_TR("editor.mod.status.playing", "Playing mod {id}: saving a server script reloads it, its state kept"),
                                                     {{"id", id}})
                                 : nox::l10n::format(NOX_TR("editor.mod.status.playing_on", "Playing mod {id} on {map}: saving a server script reloads it, its state kept"),
                                                     {{"id", id}, {"map", map}}));
    }
}

void ModPanel::exportTo(const fs::path& zipPath)
{
    if (!project_) {
        return;
    }
    nox::io::ZipWriter zip;
    const std::string root = project_->folderName() + "/";
    std::uintmax_t total = 0;
    try {
        zip.add(root, std::span<const std::uint8_t>{});
        for (const auto& e : project_->files()) {
            if (e.folder) {
                zip.add(root + e.path + "/", std::span<const std::uint8_t>{});
                continue;
            }
            total += e.size;
            if (total > kExportCap) {
                ed_.errorText_ = NOX_TR("editor.mod.err.export_too_big", "The mod is over 64 MB: not exported.");
                return;
            }
            const auto at = project_->pathOf(e.path);
            const std::vector<std::uint8_t> bytes = at ? nox::readFileBytes(*at) : std::vector<std::uint8_t>{};
            zip.add(root + e.path, bytes);
        }
        const std::vector<std::uint8_t> out = zip.finish();
        std::ofstream f(zipPath, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!f) {
            throw std::runtime_error(nox::l10n::format(NOX_TR("editor.mod.err.cant_write", "can't write {path}"), {{"path", pathText(zipPath)}}));
        }
    } catch (const std::exception& ex) {
        ed_.errorText_ = nox::l10n::format(NOX_TR("editor.mod.err.export", "The mod can't be exported:\n{error}"), {{"error", ex.what()}});
        return;
    }
    ed_.status(nox::l10n::format(NOX_TR("editor.mod.status.exported", "Exported {name} ({n} entries) to {path}: unzip it into a mods\\ folder beside noxcore.exe"),
                                 {{"name", project_->folderName()}, {"n", zip.entries()}, {"path", pathText(zipPath)}}));
}

bool ModPanel::dirty() const
{
    if (!project_) {
        return false;
    }
    if (!(form_ == formSaved_)) {
        return true;
    }
    return std::ranges::any_of(tabs_, [](const auto& t) { return t->dirty; });
}

bool ModPanel::hasKeyboard() const
{
    return showCode_ && std::ranges::any_of(tabs_, [](const auto& t) { return t->code.focused(); });
}

bool ModPanel::onShortcut(int chord)
{
    const int key = chord & ~ImGuiMod_Mask_;
    const bool ctrl = (chord & ImGuiMod_Ctrl) != 0;
    const bool shift = (chord & ImGuiMod_Shift) != 0;
    const bool alt = (chord & ImGuiMod_Alt) != 0;
    if (ctrl && !alt && !shift && key == ImGuiKey_S) {
        saveAll();
        return true;
    }
    if (ctrl && !alt && !shift && key == ImGuiKey_W) {
        if (Tab* t = current(); t && !t->dirty) {
            if (sessionMod_ && session()) {
                session()->modLock(t->path, false); // (a session's file let go with its tab)
            }
            tabs_.erase(tabs_.begin() + currentTab_);
            currentTab_ = -1;
        }
        return true;
    }
    if (!ctrl && !alt && !shift && key == ImGuiKey_F5) {
        ed_.commands_.run("mod.play");
        return true;
    }
    if (!ctrl && !alt && !shift && key == ImGuiKey_F12) {
        return false; // (the editor's)
    }
    // Else the registry's command on that key, for the categories that make sense while typing (as
    // the Script panel's: Shift+F5 stops the test, Shift+F1 frees the mouse...).
    editor::Shortcut s;
    s.ctrl = ctrl;
    s.shift = shift;
    s.alt = alt;
    s.key = ImGui::GetKeyName(static_cast<ImGuiKey>(key));
    if (const editor::Command* c = ed_.commands_.match(s)) {
        const std::string& cat = c->category;
        // (A category as the registry holds it: in English, or in the language in use.)
        const auto is = [&](std::string_view en, const std::string& shown) { return cat == en || cat == shown; };
        if (is("File", NOX_TR("editor.category.file", "File")) || is("Window", NOX_TR("editor.category.window", "Window")) ||
            is("Help", NOX_TR("editor.category.help", "Help")) || is("Play", NOX_TR("editor.category.play", "Play")) ||
            is("Mod", NOX_TR("editor.category.mod", "Mod")) || c->id == "view.palette") {
            ed_.commands_.run(c->id);
            return true;
        }
    }
    return false; // (the text's own chords stay the code editor's)
}

// --- A session's mod -----------------------------------------------------------------------------

nox::client::EditSessionClient* ModPanel::session() const
{
    return ed_.session_;
}

fs::path ModPanel::sessionRoot() const
{
    // (Our own folder for it, never mods\: a private test reads it with --mods-dir.)
    const fs::path base = !ed_.ctx_.playTestDir.empty() ? ed_.ctx_.playTestDir : ed_.ctx_.prefDir / "playtest";
    return base / "session_mod" / "mods";
}

const nox::client::EditSessionClient::ModFileState* ModPanel::sessionFile(const std::string& path) const
{
    const auto* s = session();
    if (!sessionMod_ || !s || !s->mod()) {
        return nullptr;
    }
    const auto it = s->mod()->files.find(path);
    return it == s->mod()->files.end() ? nullptr : &it->second;
}

std::string ModPanel::heldByOther(const std::string& path) const
{
    const auto* f = sessionFile(path);
    if (!f || f->info.holder == 0 || f->info.holder == session()->self()) {
        return {};
    }
    return session()->nameOf(f->info.holder);
}

void ModPanel::sessionSave(const std::string& path, const std::optional<std::string>& text)
{
    auto* s = session();
    if (!s || !s->mod()) {
        return;
    }
    if (text) {
        s->modSave(path, std::span(reinterpret_cast<const std::uint8_t*>(text->data()), text->size()));
        // (Noted till the session sends the file again: ours taken, or its own back.)
        const auto* f = sessionFile(path);
        sent_[path] = SentSave{*text, f ? f->arrivals : 0};
    } else {
        s->modDelete(path);
        sent_.erase(path);
    }
}

void ModPanel::sessionTick()
{
    auto* s = session();
    if (sessionMod_ && (!s || !s->mod())) {
        // The session (or its mod) gone: its copy closed (what was saved stays in it); what wasn't
        // is named, as it's dropped.
        std::string dropped;
        for (const auto& tab : tabs_) {
            if (tab->dirty) dropped += (dropped.empty() ? "" : ", ") + tab->path;
        }
        if (!(form_ == formSaved_)) dropped += std::string(dropped.empty() ? "" : ", ") + NOX_TR("editor.mod.the_form", "the Mod form");
        closeProject();
        sessionModId_.clear();
        ed_.status(dropped.empty() ? NOX_TR("editor.mod.status.session_closed", "The session's mod is closed")
                                   : nox::l10n::format(NOX_TR("editor.mod.status.session_closed_dropped",
                                                              "The session's mod is closed; not saved, so dropped: {files} (what was saved is in its copy)"),
                                                       {{"files", dropped}}));
        return;
    }
    if (!s || !s->mod() || !s->modReady()) {
        return;
    }
    if (!sessionMod_ || sessionModId_ != s->mod()->id) {
        if (sessionDismissed_ && sessionModId_ == s->mod()->id) {
            return; // (closed by Close Mod: Open the Session's Mod brings it back)
        }
        if (project_ && !sessionMod_ && dirty()) {
            // Our own mod with unsaved changes: asked first (Save, Don't Save, Cancel); on Cancel
            // ours stays open and the session's waits for "Open the Session's Mod".
            if (!askingForSession_) {
                askingForSession_ = true;
                const std::string id = s->mod()->id;
                askThen([this] {
                    askingForSession_ = false;
                    attachSessionMod();
                },
                        [this, id] {
                            askingForSession_ = false;
                            sessionDismissed_ = true;
                            sessionModId_ = id;
                        });
            }
            return;
        }
        attachSessionMod();
    } else if (s->modRevision() != sessionModRev_) {
        syncSessionMod();
    }
}

void ModPanel::attachSessionMod()
{
    auto* s = session();
    const std::string id = s->mod()->id;
    sessionModId_ = id;
    sessionDismissed_ = false;
    if (!nox::modes::script::validModeId(id)) {
        ed_.status(NOX_TR("editor.mod.status.session_bad_id", "The session's mod has a bad id: not opened"));
        return;
    }
    closeProject(); // (asked about first when it had unsaved changes: sessionTick)
    // The copy made afresh from the session's files (only inside our own folder for it).
    const fs::path root = sessionRoot();
    const fs::path dir = root / id;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    for (const auto& [path, f] : s->mod()->files) {
        std::string error;
        if (f.info.working != 0 && f.bytes && nox::mapedit::writeModFile(dir, path, *f.bytes, error)) {
            written_[path] = f.info.working;
        }
    }
    if (!fs::exists(dir / "mode.json", ec) || !openProject(dir)) {
        ed_.status(nox::l10n::format(NOX_TR("editor.mod.status.session_no_manifest", "The session's mod {id} has no mode.json to open"), {{"id", id}}));
        return;
    }
    // (openProject's closeProject cleared what was written: noted again.)
    for (const auto& [path, f] : s->mod()->files) {
        if (f.info.working != 0 && f.bytes) written_[path] = f.info.working;
    }
    sessionMod_ = true;
    sessionModRev_ = s->modRevision();
    ed_.status(nox::l10n::format(NOX_TR("editor.mod.status.session_opened", "The session's mod {id}: saves go to the session, pending till its host publishes them"),
                                 {{"id", id}}));
}

void ModPanel::syncSessionMod()
{
    auto* s = session();
    sessionModRev_ = s->modRevision();
    const fs::path dir = project_->folder();
    bool manifest = false;
    std::vector<std::string> changedBy;
    std::vector<std::string> notTaken;
    // Our saves the session has answered: the file sent again since. Not ours (refused: held,
    // changed since): the copy made the session's again, our text kept in its tab, unsaved.
    for (auto it = sent_.begin(); it != sent_.end();) {
        const auto f = s->mod()->files.find(it->first);
        if (f == s->mod()->files.end()) {
            it = sent_.erase(it);
            continue;
        }
        if (f->second.arrivals <= it->second.arrivals || !f->second.have()) {
            ++it;
            continue;
        }
        const std::string theirs = f->second.bytes ? std::string(f->second.bytes->begin(), f->second.bytes->end()) : std::string();
        if (theirs != it->second.text) {
            std::string error;
            if (f->second.info.working != 0 && nox::mapedit::writeModFile(dir, it->first, std::span(reinterpret_cast<const std::uint8_t*>(theirs.data()), theirs.size()), error)) {
                written_[it->first] = f->second.info.working;
            }
            for (auto& tab : tabs_) {
                if (tab->path == it->first) {
                    tab->saved = theirs;
                    tab->dirty = tab->code.text() != theirs;
                }
            }
            notTaken.push_back(it->first);
        }
        it = sent_.erase(it);
    }
    // Each file whose working version our copy hasn't: written (or removed), an open tab without
    // our own changes following it.
    for (const auto& [path, f] : s->mod()->files) {
        const auto w = written_.find(path);
        const std::uint32_t had = w == written_.end() ? 0 : w->second;
        if (!f.have() || had == f.info.working) {
            continue;
        }
        std::string error;
        if (f.info.working == 0) {
            nox::mapedit::removeModFile(dir, path, error);
            written_.erase(path);
        } else if (f.bytes && nox::mapedit::writeModFile(dir, path, *f.bytes, error)) {
            written_[path] = f.info.working;
        } else {
            continue;
        }
        manifest = manifest || path == "mode.json";
        for (auto& tab : tabs_) {
            if (tab->path != path) continue;
            const std::string now = f.bytes ? std::string(f.bytes->begin(), f.bytes->end()) : std::string();
            if (!tab->dirty) {
                tab->code.setText(now, true);
                tab->saved = now;
            } else if (now != tab->code.text()) {
                changedBy.push_back(path + " (" + s->nameOf(f.info.author) + ")");
            }
        }
    }
    // Files the session no longer has (deleted and published, or never published).
    for (auto it = written_.begin(); it != written_.end();) {
        if (!s->mod()->files.contains(it->first)) {
            std::string error;
            nox::mapedit::removeModFile(dir, it->first, error);
            it = written_.erase(it);
        } else {
            ++it;
        }
    }
    if (manifest && form_ == formSaved_) {
        // mode.json changed by someone else: the form follows (our unsaved form is kept).
        std::string error;
        if (auto p = editor::ModProject::open(dir, error)) {
            project_ = std::move(p);
            form_ = formSaved_ = project_->form();
            prefixFor_.reset();
        }
    }
    if (!notTaken.empty()) {
        std::string list;
        for (const auto& c : notTaken) list += (list.empty() ? "" : ", ") + c;
        ed_.status(nox::l10n::format(NOX_TR("editor.mod.status.not_taken", "{files}: not saved in the session (see the session log); your text is kept, unsaved"),
                                     {{"files", list}}));
    } else if (!changedBy.empty()) {
        std::string list;
        for (const auto& c : changedBy) list += (list.empty() ? "" : ", ") + c;
        ed_.status(nox::l10n::format(NOX_TR("editor.mod.status.changed_by", "Changed in the session meanwhile: {files}: your edits are kept (saving them replaces theirs)"),
                                     {{"files", list}}));
    }
    refreshFiles();
    requestAnalysis();
}

std::filesystem::path ModPanel::folderForSession()
{
    if (!project_ || sessionMod_) {
        return {};
    }
    saveAll();
    return project_->folder();
}

void ModPanel::askThen(std::function<void()> then, std::function<void()> cancelled)
{
    if (!dirty()) {
        then();
        return;
    }
    askThen_ = std::move(then);
    askCancelled_ = std::move(cancelled);
    closeAsk_ = true;
}

// --- Every frame -------------------------------------------------------------------------------

void ModPanel::tick()
{
    sessionTick();
    const std::uint64_t now = SDL_GetTicks();
    if (auto r = analysis_.take()) {
        diagnostics_ = std::move(r->diagnostics);
        analysisMs_ = r->ms;
        analysed_ = true;
        for (auto& tab : tabs_) {
            std::vector<editor::CodeEditor::Marker> markers;
            for (const editor::ModDiagnostic& d : diagnostics_) {
                if (d.file == tab->path) markers.push_back({d.line, d.col, d.error, d.message});
            }
            tab->code.setMarkers(std::move(markers));
        }
    }
    if (analysisDueMs_ != 0 && now >= analysisDueMs_) {
        analysisDueMs_ = 0;
        requestAnalysis();
    }
    if (playing_ && ed_.ctx_.playTest.running && !ed_.ctx_.playTest.running()) {
        playing_ = false;
    }
    if (project_ && showFiles_ && now >= filesCheckMs_) {
        // Files put in from outside (a picture copied into assets\ with the system's file browser):
        // the tree follows, looked at once a second.
        filesCheckMs_ = now + kFilesCheckMs;
        if (auto files = project_->files(); files != files_) {
            files_ = std::move(files);
            requestAnalysis();
        }
    }
    const std::optional<std::string> picked = filePick_.take();
    if (picked && project_) {
        fs::path p = u8path(*picked);
        if (p.extension().empty()) {
            p += ".zip";
        }
        exportTo(p);
    }
}

void ModPanel::problems()
{
    if (!project_) {
        return;
    }
    editor::json::Value doc = project_->manifestDoc();
    editor::applyManifestForm(doc, form_);
    const auto manifest = nox::modes::script::readManifest(editor::json::write(doc));
    if (manifest.manifest && diagnostics_.empty() && prefixProblem().empty()) {
        return;
    }
    ImGui::SeparatorText(nox::l10n::format(NOX_TR("editor.mod.problems.heading", "Mod {name}"), {{"name", project_->folderName()}}).c_str());
    if (ImGui::BeginTable("##modissues", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
        const float s = ed_.imgui_->scale();
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60 * s);
        ImGui::TableSetupColumn(NOX_TRI("editor.mod.problems.where", "Where"), ImGuiTableColumnFlags_WidthFixed, 200 * s);
        ImGui::TableSetupColumn(NOX_TRI("editor.mod.problems.problem", "Problem"), ImGuiTableColumnFlags_WidthStretch);
        int row = 0;
        const auto line = [&](bool error, const std::string& where, const std::string& message, const std::string& file, int goLine, int goCol) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(error ? editor::themeColours().error : editor::themeColours().warning, "%s",
                               error ? NOX_TRC("editor.mod.problems.error", "Error") : NOX_TRC("editor.mod.problems.warning", "Warning"));
            ImGui::TableNextColumn();
            if (ImGui::Selectable((where + "##m" + std::to_string(row++)).c_str(), false, ImGuiSelectableFlags_SpanAllColumns) && !file.empty()) {
                openFile(file);
                if (goLine > 0) {
                    for (auto& tab : tabs_) {
                        if (tab->path == file) tab->code.gotoLine(goLine, goCol);
                    }
                }
            }
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", message.c_str());
        };
        if (!manifest.manifest) {
            line(true, "mode.json", manifest.error, "mode.json", 0, 0);
        }
        if (!prefixProblem().empty()) {
            line(true, "mode.json",
                 nox::l10n::format(NOX_TR("editor.mod.problems.map_prefix", "mapPrefix \"{prefix}\": {why}"), {{"prefix", form_.mapPrefix}, {"why", prefixProblem()}}),
                 "mode.json", 0, 0);
        }
        for (const editor::ModDiagnostic& d : diagnostics_) {
            line(d.error, d.file + " " + std::to_string(d.line) + ":" + std::to_string(d.col), d.message, d.file, d.line, d.col);
        }
        ImGui::EndTable();
    }
}

// --- The windows -------------------------------------------------------------------------------

void ModPanel::draw()
{
    dialogs();
    if (!project_) {
        return;
    }
    if (focusMap_ > 0 && --focusMap_ == 0) {
        ImGui::SetWindowFocus(NOX_TRI("editor.window.map", "Map"));
    }
    if (focusOpened_ > 0) {
        // (The first frame makes the windows; then the form, the files, the code.)
        if (--focusOpened_ < 3) {
            ImGui::SetWindowFocus(focusOpened_ == 0   ? NOX_TRI("editor.window.mod_code", "Mod Code")
                                  : focusOpened_ == 1 ? NOX_TRI("editor.window.mod_files", "Mod Files")
                                                      : NOX_TRI("editor.window.mod", "Mod"));
        }
    }
    if (showForm_) formWindow();
    if (showFiles_) filesWindow();
    if (showCode_) codeWindow();
    if (showReference_) referenceWindow();
    if (showPreview_) previewWindow();
}

void ModPanel::formWindow()
{
    ImGui::SetNextWindowSize(ImVec2(360, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(NOX_TRI("editor.window.mod", "Mod"), &showForm_)) {
        ImGui::End();
        return;
    }
    const float s = ed_.imgui_->scale();
    const float fieldX = 120 * s;
    ImGui::TextDisabled("mods\\%s", project_->folderName().c_str());
    ImGui::SameLine();
    const bool changed = !(form_ == formSaved_);
    ImGui::BeginDisabled(!changed && !dirty());
    if (ImGui::SmallButton(NOX_TRI("editor.common.save", "Save"))) saveAll();
    ImGui::EndDisabled();
    help(NOX_TRC("editor.mod.form.save_help", "Save mode.json and the open files (Ctrl+S in a file)."));
    ImGui::SameLine();
    ImGui::BeginDisabled(!ed_.commands_.enabled("mod.play"));
    if (ImGui::SmallButton(playing_ ? NOX_TRI("editor.mod.form.back_to_game", "Back to Game") : NOX_TRI("editor.cmd.mod.play.label", "Play Mod")))
        ed_.commands_.run("mod.play");
    ImGui::EndDisabled();
    help(ed_.commands_.find("mod.play") ? ed_.commands_.find("mod.play")->help : std::string());
    if (playing_ && !lastReload_.empty()) {
        ImGui::TextColored(editor::themeColours().info, "%s", lastReload_.c_str());
    }
    if (sessionMod_ && session()) {
        // The session's mod: saves go to its host, pending till the host publishes them.
        const auto pending = std::ranges::count_if(session()->mod()->files, [](const auto& f) { return f.second.pending(); });
        ImGui::TextColored(editor::themeColours().info, "%s",
                           nox::l10n::format(NOX_TR("editor.mod.form.session_pending", "The session's mod: saves go to the session ({n} pending)"),
                                             {{"n", pending}})
                               .c_str());
        if (session()->isOwner()) {
            ImGui::SameLine();
            ImGui::BeginDisabled(pending == 0);
            if (ImGui::SmallButton(NOX_TRI("editor.cmd.mod.publish.label", "Publish Mod"))) ed_.commands_.run("mod.publish");
            ImGui::EndDisabled();
            help(ed_.commands_.find("mod.publish") ? ed_.commands_.find("mod.publish")->help : std::string());
        }
    }
    ImGui::Separator();

    editor::labelRow(NOX_TRC("editor.mod.form.id", "Id"), fieldX,
                     NOX_TRC("editor.mod.form.id_help", "The mode's key (a-z, 0-9 and _, at most 24): the game registers it under this. Its folder's name should match."));
    inputText("##id", form_.id, 24, ImGuiInputTextFlags_CharsNoBlank);
    if (form_.id != project_->folderName()) {
        ImGui::TextColored(editor::themeColours().warning, "%s",
                           nox::l10n::format(NOX_TR("editor.mod.form.folder_mismatch", "The folder is mods\\{folder}: the game lists the mode as \"{id}\"."),
                                             {{"folder", project_->folderName()}, {"id", form_.id}})
                               .c_str());
    }
    editor::labelRow(NOX_TRC("editor.mod.form.name", "Name"), fieldX, NOX_TRC("editor.mod.form.name_help", "As the mode lists show it (at most 32)."));
    inputText("##name", form_.name, 32);
    editor::labelRow(NOX_TRC("editor.mod.form.description", "Description"), fieldX,
                     NOX_TRC("editor.mod.form.description_help", "One line for the list (at most 120)."));
    inputText("##desc", form_.description, 120);
    editor::labelRow(NOX_TRC("editor.mod.form.players", "Players"), fieldX, NOX_TRC("editor.mod.form.players_help", "The fewest and the most players (1-32)."));
    int players[2] = {form_.minPlayers, form_.maxPlayers};
    if (ImGui::InputInt2("##players", players)) {
        form_.minPlayers = std::clamp(players[0], 1, 32);
        form_.maxPlayers = std::clamp(players[1], form_.minPlayers, 32);
    }
    editor::labelRow(NOX_TRC("editor.mod.form.stock_clients", "Stock clients"), fieldX,
                     NOX_TRC("editor.mod.form.stock_clients_help",
                             "Whether GAME.EXE players can join (off: NoX Core players only; stock ones are told why and turned away)."));
    ImGui::Checkbox("##stock", &form_.stockClients);
    editor::labelRow(NOX_TRC("editor.mod.form.levels", "Levels"), fieldX,
                     NOX_TRC("editor.mod.form.levels_help", "Map files (the host's maps), or levels the mode generates itself."));
    int levels = form_.generatedLevels ? 1 : 0;
    const char* levelItems[] = {NOX_TRC("editor.mod.form.levels.map_files", "Map files"), NOX_TRC("editor.mod.form.levels.generated", "Generated")};
    if (ImGui::Combo("##levels", &levels, levelItems, 2)) form_.generatedLevels = levels == 1;
    editor::labelRow(NOX_TRC("editor.mod.form.map_prefix", "Map prefix"), fieldX,
                     NOX_TRC("editor.mod.form.map_prefix_help",
                             "2-3 capital letters its generated maps' names start with (\"DSC\": DSC_...), none another mode or mod has. Empty: none."));
    if (inputText("##prefix", form_.mapPrefix, 3, ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank)) {
        prefixFor_.reset();
    }
    if (const std::string& why = prefixProblem(); !why.empty()) {
        ImGui::TextColored(editor::themeColours().error, "%s",
                           nox::l10n::format(NOX_TR("editor.mod.form.map_prefix_problem", "Map prefix: {why}."), {{"why", why}}).c_str());
    }
    editor::labelRow(NOX_TRC("editor.mod.form.plays_as", "Plays as"), fieldX,
                     NOX_TRC("editor.mod.form.plays_as_help", "A stock mode it looks like to the game and the clients (its own rules the script's)."));
    int looks = form_.looksLike == "arena" ? 1 : form_.looksLike == "ctf" ? 2 : 0;
    const char* looksItems[] = {NOX_TRC("editor.mod.form.plays_as.own", "Its own"), NOX_TRC("editor.mod.form.plays_as.arena", "Arena"),
                                NOX_TRC("editor.mod.form.plays_as.ctf", "Capture the Flag")};
    if (ImGui::Combo("##looks", &looks, looksItems, 3)) form_.looksLike = looks == 1 ? "arena" : looks == 2 ? "ctf" : "";
    editor::labelRow(NOX_TRC("editor.mod.form.server_script", "Server script"), fieldX,
                     NOX_TRC("editor.mod.form.server_script_help", "The script the host runs ('/' separated, inside the folder)."));
    inputText("##server", form_.serverScript, 128);
    editor::labelRow(NOX_TRC("editor.mod.form.ui_script", "UI script"), fieldX,
                     NOX_TRC("editor.mod.form.ui_script_help", "The UI sent to NoX Core players (empty: none; everyone gets the views as text)."));
    inputText("##ui", form_.uiScript, 128);
    editor::labelRow(NOX_TRC("editor.mod.form.ui_api", "UI API"), fieldX,
                     NOX_TRC("editor.mod.form.ui_api_help",
                             "1: boxes, outlines, text, clicks. 2: images, sounds, keys and more. 3: lines, tooltips, labels over things, HUD strips, "
                             "drags and the wheel. 4: the game's spell icons, hit areas, a tick, kept labels. 5: the game's top-left icons kept "
                             "out of a box. 6: the game's own pictures and window look, clipping, the whole view, covering the HUD (each sent "
                             "only to clients that run it; the rest get text)."));
    int api = std::clamp(form_.uiApi, 1, 6) - 1;
    if (ImGui::Combo("##uiapi", &api, "1\0" "2\0" "3\0" "4\0" "5\0" "6\0")) form_.uiApi = api + 1;

    ImGui::SeparatorText(NOX_TRC("editor.mod.form.options", "Options"));
    help(NOX_TRC("editor.mod.form.options_help", "Settings shown under the mode's row in Host Custom; a script reads them with nox.option(key)."));
    int remove = -1;
    for (std::size_t i = 0; i < form_.options.size(); ++i) {
        editor::ModOption& o = form_.options[i];
        ImGui::PushID(static_cast<int>(i));
        const std::string head = (o.label.empty() ? o.key : o.label) + "##opt";
        if (ImGui::TreeNodeEx(head.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
            editor::labelRow(NOX_TRC("editor.mod.option.key", "Key"), fieldX, NOX_TRC("editor.mod.option.key_help", "a-z, 0-9 and _ (nox.option's key)."));
            inputText("##key", o.key, 32, ImGuiInputTextFlags_CharsNoBlank);
            editor::labelRow(NOX_TRC("editor.mod.option.label", "Label"), fieldX);
            inputText("##label", o.label, 48);
            editor::labelRow(NOX_TRC("editor.mod.option.kind", "Kind"), fieldX);
            int kind = static_cast<int>(o.kind);
            const char* kindItems[] = {NOX_TRC("editor.mod.option.kind.toggle", "Toggle"), NOX_TRC("editor.mod.option.kind.choice", "Choice"),
                                       NOX_TRC("editor.mod.option.kind.number", "Number"), NOX_TRC("editor.mod.option.kind.text", "Text")};
            if (ImGui::Combo("##kind", &kind, kindItems, 4)) o.kind = static_cast<editor::ModOption::Kind>(kind);
            if (o.kind == editor::ModOption::Kind::Choice) {
                std::string list;
                for (const auto& c : o.choices) list += (list.empty() ? "" : ", ") + c;
                editor::labelRow(NOX_TRC("editor.mod.option.choices", "Choices"), fieldX, NOX_TRC("editor.mod.option.choices_help", "Comma-separated, 1-16 of them."));
                if (inputText("##choices", list, 400)) {
                    o.choices.clear();
                    for (std::size_t at = 0; at <= list.size();) {
                        const std::size_t comma = std::min(list.find(',', at), list.size());
                        std::string c = list.substr(at, comma - at);
                        c.erase(0, c.find_first_not_of(' '));
                        c.erase(c.find_last_not_of(' ') + 1);
                        if (!c.empty()) o.choices.push_back(c);
                        at = comma + 1;
                    }
                }
            }
            if (o.kind == editor::ModOption::Kind::Number) {
                editor::labelRow(NOX_TRC("editor.mod.option.min_max", "Min, max"), fieldX);
                int range[2] = {o.min, o.max};
                if (ImGui::InputInt2("##range", range)) {
                    o.min = range[0];
                    o.max = std::max(range[0], range[1]);
                }
            }
            if (o.kind != editor::ModOption::Kind::Text) {
                editor::labelRow(NOX_TRC("editor.mod.option.default", "Default"), fieldX);
                ImGui::InputInt("##default", &o.defaultValue);
            }
            if (ImGui::SmallButton(NOX_TRI("editor.common.remove", "Remove"))) remove = static_cast<int>(i);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (remove >= 0) {
        form_.options.erase(form_.options.begin() + remove);
    }
    if (ImGui::SmallButton(NOX_TRI("editor.mod.option.add", "Add Option")) && form_.options.size() < 32) {
        editor::ModOption o;
        o.key = "option" + std::to_string(form_.options.size() + 1);
        o.label = nox::l10n::format(NOX_TR("editor.mod.option.new_label", "Option {n}"), {{"n", form_.options.size() + 1}});
        form_.options.push_back(std::move(o));
    }
    ImGui::Separator();
    editor::json::Value doc = project_->manifestDoc();
    editor::applyManifestForm(doc, form_);
    if (const auto m = nox::modes::script::readManifest(editor::json::write(doc)); !m.manifest) {
        ImGui::TextColored(editor::themeColours().error, "%s", m.error.c_str());
    } else if (changed) {
        ImGui::TextColored(editor::themeColours().info, "%s",
                           NOX_TRC("editor.mod.form.changed", "Changed: Save writes mode.json (a running test takes it at its next start)."));
    }
    ImGui::End();
}

void ModPanel::filesWindow()
{
    ImGui::SetNextWindowSize(ImVec2(260, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(NOX_TRI("editor.window.mod_files", "Mod Files"), &showFiles_)) {
        ImGui::End();
        return;
    }
    const auto action = [&](const char* what) {
        fileAction_ = what;
        fileDialog_ = true;
        std::string start;
        const std::string folder = selectedFile_.empty() ? std::string() : selectedFile_.substr(0, selectedFile_.rfind('/') + 1);
        if (fileAction_ == "rename") start = selectedFile_;
        else if (fileAction_ == "new") start = (folder.empty() ? std::string("server/") : folder) + "new.luau";
        else start = folder + "folder";
        std::snprintf(fileName_, sizeof fileName_, "%s", start.c_str());
    };
    if (ImGui::SmallButton(NOX_TRI("editor.mod.files.new_file", "New File"))) action("new");
    ImGui::SameLine();
    if (ImGui::SmallButton(NOX_TRI("editor.mod.files.new_folder", "New Folder"))) action("folder");
    ImGui::SameLine();
    ImGui::BeginDisabled(selectedFile_.empty() || selectedFile_ == "mode.json");
    if (ImGui::SmallButton(NOX_TRI("editor.common.rename", "Rename"))) action("rename");
    ImGui::SameLine();
    if (ImGui::SmallButton(NOX_TRI("editor.common.delete", "Delete"))) confirmDelete_ = selectedFile_;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton(NOX_TRI("editor.common.refresh", "Refresh"))) refreshFiles();
    ImGui::Separator();
    // The tree: each entry under its folder (the list is in a tree's order).
    std::vector<std::string> open; // (the folders whose nodes are open, innermost last)
    for (const auto& e : files_) {
        const std::size_t slash = e.path.rfind('/');
        const std::string parent = slash == std::string::npos ? std::string() : e.path.substr(0, slash);
        const std::string leaf = slash == std::string::npos ? e.path : e.path.substr(slash + 1);
        // Close the nodes this entry isn't inside; skip it when its folder is shut.
        while (!open.empty() && open.back() != parent && !parent.starts_with(open.back() + "/")) {
            ImGui::TreePop();
            open.pop_back();
        }
        const std::size_t depth = parent.empty() ? 0 : static_cast<std::size_t>(std::ranges::count(parent, '/')) + 1;
        if (depth != open.size()) {
            continue; // (inside a shut folder)
        }
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (selectedFile_ == e.path) flags |= ImGuiTreeNodeFlags_Selected;
        if (e.folder) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            const bool isOpen = ImGui::TreeNodeEx(e.path.c_str(), flags, "%s", leaf.c_str());
            if (ImGui::IsItemClicked()) selectedFile_ = e.path;
            if (isOpen) open.push_back(e.path);
        } else {
            const bool dirtyTab = std::ranges::any_of(tabs_, [&](const auto& t) { return t->path == e.path && t->dirty; });
            // A session's file: greyed while pending (changed since its host's publish), its holder named.
            const auto* sf = sessionFile(e.path);
            const bool pending = sf && sf->pending();
            const std::string holder = heldByOther(e.path);
            if (pending) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TreeNodeEx(e.path.c_str(), flags | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen, "%s%s%s%s", leaf.c_str(), dirtyTab ? " *" : "",
                              pending ? (sf->info.published == 0 ? NOX_TRC("editor.mod.files.new_pending", " (new, pending)") : NOX_TRC("editor.mod.files.pending", " (pending)")) : "",
                              holder.empty() ? "" : (" [" + holder + "]").c_str());
            if (pending) ImGui::PopStyleColor();
            if (ImGui::IsItemClicked()) selectedFile_ = e.path;
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openFile(e.path);
            const auto issues = std::ranges::count_if(diagnostics_, [&](const auto& d) { return d.file == e.path && d.error; });
            if (issues > 0) {
                ImGui::SameLine();
                ImGui::TextColored(editor::themeColours().error, "(%td)", issues);
            }
        }
    }
    while (!open.empty()) {
        ImGui::TreePop();
        open.pop_back();
    }
    // The session's files deleted in its working copy: gone here, kept by the host till it publishes.
    if (sessionMod_ && session() && session()->mod()) {
        for (const auto& [path, f] : session()->mod()->files) {
            if (f.info.working == 0 && f.info.published != 0) {
                ImGui::TextDisabled("%s", nox::l10n::format(NOX_TR("editor.mod.files.deleted_pending", "{path} (deleted, pending)"), {{"path", path}}).c_str());
            }
        }
    }
    ImGui::End();
}

void ModPanel::codeWindow()
{
    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
    if (focusCode_ > 0) {
        --focusCode_;
        ImGui::SetNextWindowFocus();
    }
    if (!ImGui::Begin(NOX_TRI("editor.window.mod_code", "Mod Code"), &showCode_)) {
        ImGui::End();
        return;
    }
    // The bar: the checks' state.
    const auto errors = std::ranges::count_if(diagnostics_, [](const auto& d) { return d.error; });
    const auto warnings = static_cast<std::ptrdiff_t>(diagnostics_.size()) - errors;
    if (analysis_.busy()) {
        ImGui::TextDisabled("%s", NOX_TRC("editor.mod.code.checking", "checking..."));
    } else if (!analysed_) {
        ImGui::TextDisabled("%s", NOX_TRC("editor.mod.code.not_checked", "not checked yet"));
    } else if (errors > 0) {
        ImGui::TextColored(editor::themeColours().error, "%s",
                           nox::l10n::format(NOX_TRP("editor.mod.code.errors", "{n} error, {warnings} warnings (Problems)",
                                                     "{n} errors, {warnings} warnings (Problems)", errors),
                                             {{"n", errors}, {"warnings", warnings}})
                               .c_str());
    } else {
        ImGui::TextColored(editor::themeColours().ok, "%s",
                           warnings ? nox::l10n::format(NOX_TR("editor.mod.code.no_errors_warnings", "no errors ({warnings} warnings)"), {{"warnings", warnings}}).c_str()
                                    : NOX_TRC("editor.mod.code.no_errors", "no errors"));
    }
    if (analysed_) {
        ImGui::SameLine();
        char ms[32];
        std::snprintf(ms, sizeof ms, "%.0f", analysisMs_);
        ImGui::TextDisabled("%s", nox::l10n::format(NOX_TR("editor.mod.code.analysis_ms", "Luau analysis, {ms} ms"), {{"ms", ms}}).c_str());
    }
    if (tabs_.empty()) {
        ImGui::TextDisabled("%s", NOX_TRC("editor.mod.code.empty", "Double-click a file in Mod Files to open it."));
    }
    int close = -1;
    if (ImGui::BeginTabBar("##modtabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (std::size_t i = 0; i < tabs_.size(); ++i) {
            Tab& tab = *tabs_[i];
            bool keep = true;
            ImGuiTabItemFlags flags = tab.dirty ? ImGuiTabItemFlags_UnsavedDocument : 0;
            if (selectTab_ == static_cast<int>(i)) {
                flags |= ImGuiTabItemFlags_SetSelected;
            }
            // A session's file changed since its host's last publish: its tab greyed (pending).
            const auto* sf = sessionFile(tab.path);
            const bool pending = sf && sf->pending();
            if (pending) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            const bool shown =
                ImGui::BeginTabItem((tab.path + (pending ? NOX_TR("editor.mod.files.pending", " (pending)") : std::string()) + "###" + tab.path).c_str(), &keep, flags);
            if (pending) ImGui::PopStyleColor();
            if (pending) help(NOX_TRC("editor.mod.code.pending_help", "Changed in the session since its host last published the mod (the host's Publish Mod writes it)."));
            if (shown) {
                currentTab_ = static_cast<int>(i);
                const std::string holder = heldByOther(tab.path);
                // (A session's viewer edits nothing: its tabs are read-only, and it holds nothing.)
                const bool viewer = sessionMod_ && session() && !session()->canEditMod();
                if (viewer) {
                    ImGui::TextColored(editor::themeColours().info, "%s",
                                       NOX_TRC("editor.mod.code.viewer", "You view the session's mod: ask the host to edit (Session panel)."));
                } else if (!holder.empty()) {
                    ImGui::TextColored(editor::themeColours().caution, "%s",
                                       nox::l10n::format(NOX_TR("editor.mod.code.held", "{name} is editing this file: read-only till they let it go."),
                                                         {{"name", holder}})
                                           .c_str());
                }
                tab.code.setReadOnly(viewer || !holder.empty());
                ImGui::PushFont(ed_.imgui_->monoFont(), ImGui::GetStyle().FontSizeBase * ed_.settings_.scriptFontScale);
                tab.code.render("##modsource", ImVec2(0, 0), nullptr);
                ImGui::PopFont();
                if (tab.code.takeChanged()) {
                    tab.dirty = tab.code.text() != tab.saved;
                    tab.typedMs = SDL_GetTicks();
                    analysisDueMs_ = tab.typedMs + kAnalysisPauseMs; // (after a pause in typing)
                    if (sf && !viewer && sf->info.holder != session()->self()) {
                        session()->modLock(tab.path, true); // (held while we type in it)
                    }
                }
                ImGui::EndTabItem();
            }
            if (!keep) {
                close = static_cast<int>(i);
            }
        }
        selectTab_ = -1;
        ImGui::EndTabBar();
    }
    if (close >= 0) {
        if (tabs_[static_cast<std::size_t>(close)]->dirty) {
            ed_.status(nox::l10n::format(NOX_TR("editor.mod.status.tab_unsaved", "{path} has unsaved changes: save it (Ctrl+S) first"),
                                         {{"path", tabs_[static_cast<std::size_t>(close)]->path}}));
        } else {
            if (sessionFile(tabs_[static_cast<std::size_t>(close)]->path)) {
                session()->modLock(tabs_[static_cast<std::size_t>(close)]->path, false); // (let go with its tab)
            }
            tabs_.erase(tabs_.begin() + close);
            currentTab_ = -1;
        }
    }
    ImGui::End();
}

void ModPanel::referenceWindow()
{
    ImGui::SetNextWindowSize(ImVec2(360, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(NOX_TRI("editor.window.mod_reference", "Mod Reference"), &showReference_)) {
        ImGui::End();
        return;
    }
    ImGui::RadioButton(NOX_TRI("editor.mod.ref.server", "Server toolbox"), &refHalf_, 0);
    help(NOX_TRC("editor.mod.ref.server_help", "What a server script (server/...) has: the global nox, require, and the hooks its table may have."));
    ImGui::SameLine();
    ImGui::RadioButton(NOX_TRI("editor.mod.ref.ui", "UI API"), &refHalf_, 1);
    help(NOX_TRC("editor.mod.ref.ui_help", "What a UI script (ui/...) has on a NoX Core client."));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##modrefsearch", NOX_TRC("editor.common.search", "Search"), refSearch_, sizeof refSearch_);
    const std::string query = lowered(refSearch_);
    ImGui::BeginChild("##modref");
    std::string section;
    ImFont* mono = ed_.imgui_->monoFont();
    for (const editor::ModDef& d : editor::modDefs(refHalf_ == 0 ? editor::ModHalf::Server : editor::ModHalf::Ui)) {
        if (!query.empty() && lowered(d.name).find(query) == std::string::npos && lowered(d.doc).find(query) == std::string::npos) {
            continue;
        }
        if (d.section != section) {
            section = d.section;
            // (Hooks and Globals are named by the reader, the rest by the declarations' own headings.)
            ImGui::SeparatorText(section == "Hooks"     ? NOX_TRC("editor.mod.ref.hooks", "Hooks")
                                 : section == "Globals" ? NOX_TRC("editor.mod.ref.globals", "Globals")
                                                        : section.c_str());
        }
        const std::string call = d.kind == editor::ModDef::Kind::Member ? "nox." + d.name : d.kind == editor::ModDef::Kind::Hook ? "function mode." + d.signature : d.name;
        ImGui::PushFont(mono, 0.0f);
        const bool clicked = ImGui::Selectable((call + "##" + d.name + std::to_string(static_cast<int>(d.kind))).c_str());
        ImGui::PopFont();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s%s\n\n%s\n\n%s", call.c_str(), d.kind == editor::ModDef::Kind::Hook ? "" : (": " + d.signature).c_str(), d.doc.c_str(),
                              NOX_TRC("editor.mod.ref.click_inserts", "(click: insert it in the open file)"));
        }
        if (clicked) {
            if (Tab* t = current()) {
                t->code.insertText(d.kind == editor::ModDef::Kind::Member && d.signature.starts_with("(") ? call + "(" : call);
                t->code.focus();
                t->dirty = t->code.text() != t->saved;
            }
        }
        ImGui::Indent();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", d.doc.c_str());
        ImGui::PopStyleColor();
        ImGui::Unindent();
    }
    ImGui::EndChild();
    ImGui::End();
}

void ModPanel::previewWindow()
{
    ImGui::SetNextWindowSize(ImVec2(700, 480), ImGuiCond_FirstUseEver);
    if (focusPreview_ > 0) {
        --focusPreview_;
        ImGui::SetNextWindowFocus();
    }
    if (!ImGui::Begin(NOX_TRI("editor.window.mod_preview", "Mod Preview"), &showPreview_)) {
        ImGui::End();
        return;
    }
    if (!previewLoaded_) {
        reloadPreview();
    }
    const float s = ed_.imgui_->scale();
    // The left column: reload, the test messages, what the UI sent.
    ImGui::BeginChild("##previewside", ImVec2(200 * s, 0), ImGuiChildFlags_ResizeX);
    if (ImGui::Button(NOX_TRI("editor.mod.preview.reload", "Reload"))) reloadPreview();
    help(NOX_TRC("editor.mod.preview.reload_help", "Run the UI afresh (it reloads by itself when a UI file is saved)."));
    if (!preview_.error().empty()) {
        ImGui::TextColored(editor::themeColours().error, "%s", NOX_TRC("editor.mod.preview.stopped", "The UI stopped:"));
        ImGui::TextWrapped("%s", preview_.error().c_str());
    } else if (preview_.running()) {
        ImGui::TextColored(editor::themeColours().ok, "%s", NOX_TRC("editor.mod.preview.running", "Running"));
    }
    ImGui::SeparatorText(NOX_TRC("editor.mod.preview.test_messages", "Test messages"));
    help(NOX_TRC("editor.mod.preview.test_messages_help", "ui/preview.luau's: each sent to the UI as the server would send it."));
    if (!preview_.previewError().empty()) {
        ImGui::TextColored(editor::themeColours().error, "%s", preview_.previewError().c_str());
    } else if (preview_.samples().empty()) {
        ImGui::TextDisabled("%s", NOX_TRC("editor.mod.preview.no_samples", "None: ui/preview.luau returns { { name = ..., message = ... } }"));
    }
    for (std::size_t i = 0; i < preview_.samples().size(); ++i) {
        const auto& sample = preview_.samples()[i];
        ImGui::BeginDisabled(sample.value.empty());
        if (ImGui::Button((sample.name + (sample.view ? NOX_TR("editor.mod.preview.view_suffix", " (view)") : std::string()) + "##sample" + std::to_string(i)).c_str(),
                          ImVec2(-FLT_MIN, 0))) {
            preview_.send(i);
            previewLog_.push_back(nox::l10n::format(NOX_TR("editor.mod.preview.sent", "sent {name}"), {{"name", sample.name}}));
        }
        ImGui::EndDisabled();
        if (!sample.problem.empty()) help(sample.problem, ImGuiHoveredFlags_AllowWhenDisabled);
    }
    ImGui::SeparatorText(NOX_TRC("editor.mod.preview.ui_sent", "The UI sent"));
    for (std::string& line : preview_.takeSent()) {
        previewLog_.push_back(std::move(line));
    }
    if (previewLog_.size() > 200) {
        previewLog_.erase(previewLog_.begin(), previewLog_.end() - 200);
    }
    for (const std::string& line : previewLog_) {
        ImGui::TextWrapped("%s", line.c_str());
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // The screen: the UI's 640 x 480 fitted in, its windows framed as the game frames them.
    ImGui::BeginChild("##previewscreen", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float scale = std::max(0.25f, std::min(avail.x / 640.0f, avail.y / 480.0f));
    const ImVec2 origin(at.x + (avail.x - 640 * scale) / 2, at.y + (avail.y - 480 * scale) / 2);
    ImGui::InvisibleButton("##screen", ImVec2(std::max(avail.x, 1.0f), std::max(avail.y, 1.0f)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const int clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left) ? 1 : ImGui::IsItemClicked(ImGuiMouseButton_Right) ? 2 : 0;
    const ImVec2 m = ImGui::GetIO().MousePos;
    const float ux = (m.x - origin.x) / scale, uy = (m.y - origin.y) / scale;
    preview_.setMouse(hovered ? std::optional(std::pair{ux, uy}) : std::nullopt);
    if (hovered && preview_.api() >= 2) {
        previewKeys(ux, uy);
    }
    preview_.frame(SDL_GetTicks());
    if (preview_.assets() != texturesOf_) {
        freePreviewTextures(); // (a new set: its pictures made as they're drawn)
        texturesOf_ = preview_.assets();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto pt = [&](float x, float y) { return ImVec2(origin.x + x * scale, origin.y + y * scale); };
    dl->AddRectFilled(pt(0, 0), pt(640, 480), IM_COL32(28, 34, 26, 255)); // (where the game would be)
    dl->AddRect(pt(0, 0), pt(640, 480), IM_COL32(90, 90, 80, 255));
    ImFont* font = ImGui::GetFont();
    constexpr float kTitleH = 13;
    const float textPx = 10 * scale;
    // (UI API 3) The labels, at stand-in spots along the screen's lower part (there's no world):
    // a dot for the object, its label over it; "alt" ones while Alt is held.
    {
        const auto& labels = preview_.labels();
        for (std::size_t i = 0; i < labels.size(); ++i) {
            const auto& l = labels[i];
            if (l.show == nox::modes::script::UiLabel::Show::Alt && !ImGui::GetIO().KeyAlt) {
                continue;
            }
            const float sx = 60 + static_cast<float>(i % 6) * 100;
            const float sy = 330 + static_cast<float>(i / 6 % 6) * 24;
            dl->AddCircleFilled(pt(sx, sy), 2 * scale, IM_COL32(200, 200, 200, 255));
            const ImVec2 size = font->CalcTextSizeA(textPx, FLT_MAX, 0, l.text.c_str());
            const ImVec2 t0(pt(sx, sy).x - size.x / 2 - 3, pt(sx, sy - l.offset).y - size.y - 2);
            dl->AddRectFilled(t0, ImVec2(t0.x + size.x + 6, t0.y + size.y + 2), IM_COL32(0, 0, 0, 160));
            dl->AddText(font, textPx, ImVec2(t0.x + 3, t0.y + 1), IM_COL32(l.color.r, l.color.g, l.color.b, l.color.a), l.text.c_str());
        }
    }
    // The HUD pieces (UI API 3) under the other windows.
    std::vector<const nox::modes::script::UiWindow*> order;
    for (const auto& w : preview_.windows()) {
        if (w.hud) order.push_back(&w);
    }
    for (const auto& w : preview_.windows()) {
        if (!w.hud) order.push_back(&w);
    }
    for (const auto* wp : order) {
        const auto& w = *wp;
        if (w.frame) {
            const ImVec2 o0 = pt(static_cast<float>(w.x) - 2, static_cast<float>(w.y) - kTitleH - 2);
            const ImVec2 o1 = pt(static_cast<float>(w.x + w.w) + 2, static_cast<float>(w.y + w.h) + 2);
            dl->AddRectFilled(o0, o1, IM_COL32(10, 8, 6, 225));
            dl->AddRect(o0, o1, IM_COL32(160, 120, 40, 255));
            const std::string label = preview_.modeName() + ": " + w.title;
            dl->PushClipRect(o0, ImVec2(o1.x - 2 * scale, o1.y), true);
            dl->AddText(font, textPx, ImVec2(o0.x + 4 * scale, o0.y + 2 * scale), IM_COL32(240, 180, 42, 255), label.c_str());
            dl->PopClipRect();
        }
        const ImVec2 w0 = pt(static_cast<float>(w.x), static_cast<float>(w.y));
        const ImVec2 w1 = pt(static_cast<float>(w.x + w.w), static_cast<float>(w.y + w.h));
        dl->PushClipRect(w0, w1, true);
        bool clippedAway = false; // (UI API 6: a clip box off the window, nothing till the next)
        for (const auto& d : w.draws) {
            const ImU32 c = IM_COL32(d.color.r, d.color.g, d.color.b, d.color.a);
            const ImVec2 a = pt(static_cast<float>(w.x) + d.x, static_cast<float>(w.y) + d.y);
            if (d.kind == nox::modes::script::UiDraw::Kind::Clip) {
                // (UI API 6) The later draws inside a box of the window (or the whole window again).
                dl->PopClipRect();
                ImVec2 c0 = w0, c1 = w1;
                if (d.w >= 0) {
                    c0 = ImVec2(std::max(w0.x, a.x), std::max(w0.y, a.y));
                    c1 = ImVec2(std::min(w1.x, a.x + d.w * scale), std::min(w1.y, a.y + d.h * scale));
                }
                clippedAway = c1.x <= c0.x || c1.y <= c0.y;
                dl->PushClipRect(c0, clippedAway ? c0 : c1, true);
                continue;
            }
            if (clippedAway) continue;
            switch (d.kind) {
            case nox::modes::script::UiDraw::Kind::Clip: break;
            case nox::modes::script::UiDraw::Kind::GameImage: {
                // (UI API 6) One of the game's pictures, its own offset kept, its size or w x h.
                float iw = 0, ih = 0, ox = 0, oy = 0;
                if (SDL_Texture* t = gameImageTexture(d.text, d.index, iw, ih, ox, oy); t && iw > 0 && ih > 0) {
                    const float kx = d.w > 0 ? d.w / iw : 1, ky = d.h > 0 ? d.h / ih : 1;
                    const ImVec2 p0(a.x + ox * kx * scale, a.y + oy * ky * scale);
                    dl->AddImage(editor::texId(t), p0, ImVec2(p0.x + iw * kx * scale, p0.y + ih * ky * scale));
                }
                break;
            }
            case nox::modes::script::UiDraw::Kind::Box: dl->AddRectFilled(a, ImVec2(a.x + d.w * scale, a.y + d.h * scale), c); break;
            case nox::modes::script::UiDraw::Kind::Outline: dl->AddRect(a, ImVec2(a.x + d.w * scale, a.y + d.h * scale), c); break;
            case nox::modes::script::UiDraw::Kind::Text: dl->AddText(font, textPx * d.size, a, c, d.text.c_str()); break;
            case nox::modes::script::UiDraw::Kind::Line:
                dl->AddLine(a, pt(static_cast<float>(w.x) + d.w, static_cast<float>(w.y) + d.h), c, std::max(1.0f, d.width * scale));
                break;
            case nox::modes::script::UiDraw::Kind::Image: {
                // A mod picture, as a texture made once per set.
                const auto* set = texturesOf_.get();
                if (!set || d.index < 0 || static_cast<std::size_t>(d.index) >= set->list.size() || !(d.w > 0) || !(d.h > 0)) break;
                const auto& asset = set->list[static_cast<std::size_t>(d.index)];
                if (asset.kind != nox::modes::script::ModAsset::Kind::Image) break;
                auto it = previewTextures_.find(d.index);
                if (it == previewTextures_.end()) {
                    SDL_Texture* t = SDL_CreateTexture(ed_.ctx_.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, asset.width, asset.height);
                    if (t) {
                        editor::panelTextureSet(t, asset.rgba.data(), asset.width, asset.height, asset.width * 4); // (floating panels' copies too)
                        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
                    }
                    it = previewTextures_.emplace(d.index, t).first;
                }
                if (it->second) dl->AddImage(editor::texId(it->second), a, ImVec2(a.x + d.w * scale, a.y + d.h * scale));
                break;
            }
            case nox::modes::script::UiDraw::Kind::Thing: {
                // A thing type's picture (the content browser's thumbnail), centred on the point.
                if (d.index <= 0 || static_cast<std::size_t>(d.index) > ed_.things_.things.size()) break;
                const float size = (d.w > 0 && d.h > 0 ? std::min(d.w, d.h) : 32) * scale; // (UI API 6: fitted to its box)
                ed_.objects_->drawThumbnailAt(dl, ed_.things_.things[static_cast<std::size_t>(d.index) - 1].name, ImVec2(a.x - size / 2, a.y - size / 2), size);
                break;
            }
            case nox::modes::script::UiDraw::Kind::SpellIcon: {
                // (UI API 4) The game's own icon for a spell or an ability (the book's), its size
                // or w x h.
                float iw = 0, ih = 0;
                if (SDL_Texture* t = spellIconTexture(d.index, iw, ih)) {
                    const float dw = d.w > 0 ? d.w : iw, dh = d.h > 0 ? d.h : ih;
                    dl->AddImage(editor::texId(t), a, ImVec2(a.x + dw * scale, a.y + dh * scale));
                }
                break;
            }
            }
        }
        // (UI API 4) A frameless window's hit areas, faintly: where it takes the mouse.
        if (!w.frame) {
            for (const auto& r : w.hitAreas) {
                dl->AddRect(pt(static_cast<float>(w.x) + r.x, static_cast<float>(w.y) + r.y),
                            pt(static_cast<float>(w.x) + r.x + r.w, static_cast<float>(w.y) + r.y + r.h), IM_COL32(120, 200, 255, 90));
            }
        }
        dl->PopClipRect();
    }
    // (UI API 3) This frame's tooltip at the mouse, whole on the screen.
    if (const auto& tip = preview_.tooltip(); hovered && !tip.empty()) {
        float widest = 0;
        for (const auto& l : tip) {
            widest = std::max(widest, font->CalcTextSizeA(textPx, FLT_MAX, 0, l.text.c_str()).x / scale);
        }
        const float lineH = 11;
        const float w = widest + 8, h = lineH * static_cast<float>(tip.size()) + 6;
        const auto [tx, ty] = nox::modes::script::placeTooltip(ux, uy, w, h);
        dl->AddRectFilled(pt(tx, ty), pt(tx + w, ty + h), IM_COL32(10, 8, 6, 235));
        dl->AddRect(pt(tx, ty), pt(tx + w, ty + h), IM_COL32(160, 120, 40, 255));
        for (std::size_t i = 0; i < tip.size(); ++i) {
            const auto& c = tip[i].color;
            dl->AddText(font, textPx, pt(tx + 4, ty + 3 + lineH * static_cast<float>(i)), IM_COL32(c.r, c.g, c.b, c.a), tip[i].text.c_str());
        }
    }
    if (clicked == 1 || (clicked == 2 && preview_.api() >= 2)) {
        // A click in a window goes to the UI, in its own units (topmost first, never a
        // click-through one); a right one to an API 2 UI alone, as in the game.
        if (const auto* w = preview_.windowAt(ux, uy)) {
            const std::string id = w->id;
            const float wx = static_cast<float>(w->x), wy = static_cast<float>(w->y);
            if (preview_.api() >= 3) {
                previewPress_ = PreviewPress{id, clicked, ux, uy, ux, uy, wx, wy};
            }
            preview_.click(id, ux - wx, uy - wy, clicked);
        }
    }
    if (preview_.api() >= 3) {
        previewMouse(hovered, clicked, ux, uy);
    }
    ImGui::EndChild();
    ImGui::End();
}

void ModPanel::previewMouse(bool hovered, int clicked, float ux, float uy)
{
    using nox::modes::script::UiDrag;
    ImGuiIO& io = ImGui::GetIO();
    // The wheel over a window, in whole steps (a fine wheel's parts add up, as in the game).
    if (hovered && io.MouseWheel != 0) {
        if (const auto* w = preview_.windowAt(ux, uy)) {
            previewWheel_ += io.MouseWheel;
            const int steps = static_cast<int>(std::trunc(previewWheel_));
            previewWheel_ -= static_cast<float>(steps);
            if (steps != 0) {
                const std::string id = w->id;
                preview_.wheel(id, ux - static_cast<float>(w->x), uy - static_cast<float>(w->y), steps);
            }
        } else {
            previewWheel_ = 0;
        }
    }
    if (!previewPress_ || clicked != 0) {
        return;
    }
    PreviewPress& p = *previewPress_;
    const ImGuiMouseButton button = p.button == 2 ? ImGuiMouseButton_Right : ImGuiMouseButton_Left;
    if (ImGui::IsMouseReleased(button)) {
        const PreviewPress done = p;
        previewPress_.reset();
        if (done.dragging) {
            preview_.drag(done.window, UiDrag::End, ux - done.wx, uy - done.wy, done.button);
        }
        if (const auto* w = preview_.windowAt(ux, uy)) {
            const std::string id = w->id;
            preview_.mouseUp(id, ux - static_cast<float>(w->x), uy - static_cast<float>(w->y), done.button);
        }
        return;
    }
    if (ux == p.toldX && uy == p.toldY) {
        return;
    }
    if (!p.dragging) {
        if (std::hypot(ux - p.fromX, uy - p.fromY) < 3) {
            return;
        }
        p.dragging = true;
        preview_.drag(p.window, UiDrag::Start, p.fromX - p.wx, p.fromY - p.wy, p.button);
    }
    if (previewPress_) {
        previewPress_->toldX = ux;
        previewPress_->toldY = uy;
        preview_.drag(previewPress_->window, UiDrag::Move, ux - previewPress_->wx, uy - previewPress_->wy, previewPress_->button);
    }
}

void ModPanel::previewKeys(float ux, float uy)
{
    // The fixed set script_ui.hpp names (never typed text).
    static const std::pair<ImGuiKey, const char*> kKeys[] = {
        {ImGuiKey_0, "0"},     {ImGuiKey_1, "1"},         {ImGuiKey_2, "2"},       {ImGuiKey_3, "3"},           {ImGuiKey_4, "4"},
        {ImGuiKey_5, "5"},     {ImGuiKey_6, "6"},         {ImGuiKey_7, "7"},       {ImGuiKey_8, "8"},           {ImGuiKey_9, "9"},
        {ImGuiKey_UpArrow, "up"}, {ImGuiKey_DownArrow, "down"}, {ImGuiKey_LeftArrow, "left"}, {ImGuiKey_RightArrow, "right"},
        {ImGuiKey_Enter, "enter"}, {ImGuiKey_Escape, "escape"}, {ImGuiKey_Tab, "tab"}, {ImGuiKey_Space, "space"},
        {ImGuiKey_F1, "f1"},   {ImGuiKey_F2, "f2"},       {ImGuiKey_F3, "f3"},     {ImGuiKey_F4, "f4"},         {ImGuiKey_F5, "f5"},
        {ImGuiKey_F6, "f6"},   {ImGuiKey_F7, "f7"},       {ImGuiKey_F8, "f8"},     {ImGuiKey_F9, "f9"},         {ImGuiKey_F10, "f10"},
        {ImGuiKey_F11, "f11"}, {ImGuiKey_F12, "f12"},
    };
    if (ImGui::GetIO().WantTextInput) {
        return;
    }
    for (const auto& [key, name] : kKeys) {
        if (!ImGui::IsKeyPressed(key, false)) {
            continue;
        }
        const auto windows = preview_.windows();
        for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
            if (ux >= it->x && ux < it->x + it->w && uy >= it->y && uy < it->y + it->h) {
                preview_.key(it->id, name);
                break;
            }
        }
    }
}

void ModPanel::dialogs()
{
    const float s = ed_.imgui_->scale();
    if (newOpen_) {
        ImGui::OpenPopup(NOX_TRI("editor.mod.new.title", "New Mod"));
        newOpen_ = false;
    }
    if (ImGui::BeginPopupModal(NOX_TRI("editor.mod.new.title", "New Mod"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", nox::l10n::format(NOX_TR("editor.mod.new.text",
                                                          "A new mod in {path}, from the template: mode.json, server/main.luau, ui/main.luau and ui/preview.luau."),
                                                   {{"path", pathText(modsDir())}})
                                     .c_str());
        ImGui::SetNextItemWidth(260 * s);
        ImGui::InputText(NOX_TRI("editor.mod.new.name", "Name"), newName_, sizeof newName_);
        ImGui::SetNextItemWidth(260 * s);
        ImGui::InputText(NOX_TRI("editor.mod.new.id", "Id"), newId_, sizeof newId_, ImGuiInputTextFlags_CharsNoBlank);
        help(NOX_TRC("editor.mod.new.id_help", "Its folder and the mode's key: a-z, 0-9 and _, at most 24."));
        const bool good = nox::modes::script::validModeId(newId_) && std::string_view(newId_).size() <= 24;
        std::error_code ec;
        const bool taken = good && fs::exists(modsDir() / newId_, ec);
        if (!good) ImGui::TextColored(editor::themeColours().warning, "%s", NOX_TRC("editor.mod.new.bad_id", "An id is 1-24 of a-z, 0-9 and _."));
        if (taken)
            ImGui::TextColored(editor::themeColours().warning, "%s",
                               nox::l10n::format(NOX_TR("editor.mod.new.taken", "mods\\{id} is there already."), {{"id", newId_}}).c_str());
        ImGui::BeginDisabled(!good || taken);
        if (ImGui::Button(NOX_TRI("editor.mod.new.create", "Create"), ImVec2(120 * s, 0))) {
            newMod(newId_, newName_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(NOX_TRI("editor.common.cancel", "Cancel"), ImVec2(120 * s, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (openOpen_) {
        ImGui::OpenPopup(NOX_TRI("editor.mod.open.title", "Open Mod"));
        openOpen_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(480 * s, 360 * s), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(NOX_TRI("editor.mod.open.title", "Open Mod"))) {
        ImGui::TextDisabled("%s", pathText(modsDir()).c_str());
        ImGui::BeginChild("##mods", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
        if (listing_.empty()) ImGui::TextDisabled("%s", NOX_TRC("editor.mod.open.none", "No mods yet: New Mod makes one."));
        for (const auto& l : listing_) {
            const std::string label = l.name + "  (" + l.folder.filename().string() + ")" +
                                      (l.reference ? NOX_TR("editor.mod.open.reference", "  reference: opens as a copy") : std::string());
            if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                openMod(l);
                ImGui::CloseCurrentPopup();
            }
            if (!l.problem.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(editor::themeColours().warning, "%s", NOX_TRC("editor.mod.open.doesnt_load", "(doesn't load)"));
                help(l.problem);
            }
        }
        ImGui::EndChild();
        if (ImGui::Button(NOX_TRI("editor.common.cancel", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", NOX_TRC("editor.mod.open.hint", "Double-click a mod to open it."));
        ImGui::EndPopup();
    }
    if (fileDialog_) {
        ImGui::OpenPopup(NOX_TRI("editor.mod.file.title", "Mod File"));
        fileDialog_ = false;
    }
    if (ImGui::BeginPopupModal(NOX_TRI("editor.mod.file.title", "Mod File"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(fileAction_ == "rename"
                                   ? nox::l10n::format(NOX_TR("editor.mod.file.rename", "Rename {path} to:"), {{"path", selectedFile_}}).c_str()
                               : fileAction_ == "new" ? NOX_TRC("editor.mod.file.new_file", "A new file ('/' separated, inside the mod):")
                                                      : NOX_TRC("editor.mod.file.new_folder", "A new folder:"));
        ImGui::SetNextItemWidth(320 * s);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##filename", fileName_, sizeof fileName_, ImGuiInputTextFlags_EnterReturnsTrue);
        const bool good = editor::ModProject::validPath(fileName_);
        if (!good)
            ImGui::TextColored(editor::themeColours().warning, "%s",
                               NOX_TRC("editor.mod.file.bad_path", "Parts '/' separated, none empty, \"..\" or with < > : \" \\ | ? *"));
        ImGui::BeginDisabled(!good);
        if (ImGui::Button(NOX_TRI("editor.common.ok", "OK"), ImVec2(120 * s, 0)) || (enter && good)) {
            std::string error;
            bool ok = false;
            if (fileAction_ == "new") {
                const std::string text = std::string(fileName_).ends_with(".luau") ? "-- " + std::string(fileName_) + "\nreturn {}\n" : "";
                ok = project_->newFile(fileName_, text, error);
                if (ok && sessionMod_) sessionSave(fileName_, text); // (a new file in the session: pending)
                if (ok) openFile(fileName_);
            } else if (fileAction_ == "folder") {
                ok = project_->newFolder(fileName_, error);
            } else {
                // A session's files it moves: saved there under their new paths, the old ones deleted.
                std::vector<std::string> moving;
                for (const auto& e : files_) {
                    if (!e.folder && (e.path == selectedFile_ || e.path.starts_with(selectedFile_ + "/")) && sessionFile(e.path)) moving.push_back(e.path);
                }
                const std::string manifestBefore = project_->manifestText();
                ok = project_->rename(selectedFile_, fileName_, error);
                if (ok && sessionMod_) {
                    for (const auto& from : moving) {
                        const std::string to = std::string(fileName_) + from.substr(selectedFile_.size());
                        if (const auto text = project_->read(to)) sessionSave(to, *text);
                        sessionSave(from, std::nullopt);
                        written_.erase(from);
                    }
                    if (project_->manifestText() != manifestBefore) sessionSave("mode.json", project_->manifestText());
                }
                if (ok) {
                    for (auto& tab : tabs_) {
                        if (tab->path == selectedFile_) tab->path = fileName_;
                        else if (tab->path.starts_with(selectedFile_ + "/")) tab->path = fileName_ + tab->path.substr(selectedFile_.size());
                    }
                    form_.serverScript = project_->form().serverScript; // (mode.json followed a script it names)
                    form_.uiScript = project_->form().uiScript;
                    formSaved_.serverScript = form_.serverScript;
                    formSaved_.uiScript = form_.uiScript;
                    selectedFile_ = fileName_;
                }
            }
            if (!ok) ed_.errorText_ = error;
            refreshFiles();
            requestAnalysis();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(NOX_TRI("editor.common.cancel", "Cancel"), ImVec2(120 * s, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (!confirmDelete_.empty()) {
        ImGui::OpenPopup(NOX_TRI("editor.mod.delete.title", "Delete?##mod"));
    }
    if (ImGui::BeginPopupModal(NOX_TRI("editor.mod.delete.title", "Delete?##mod"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool folder = std::ranges::any_of(files_, [&](const auto& e) { return e.path == confirmDelete_ && e.folder; });
        ImGui::TextUnformatted(
            (folder ? nox::l10n::format(NOX_TR("editor.mod.delete.folder", "Delete {path} and everything in it? This can't be undone."), {{"path", confirmDelete_}})
                    : nox::l10n::format(NOX_TR("editor.mod.delete.file", "Delete {path}? This can't be undone."), {{"path", confirmDelete_}}))
                .c_str());
        if (ImGui::Button(NOX_TRI("editor.common.delete", "Delete"), ImVec2(120 * s, 0))) {
            std::string error;
            std::vector<std::string> gone; // (a session's files it takes: deleted there too)
            for (const auto& e : files_) {
                if (!e.folder && (e.path == confirmDelete_ || e.path.starts_with(confirmDelete_ + "/")) && sessionFile(e.path)) gone.push_back(e.path);
            }
            if (project_->remove(confirmDelete_, error)) {
                for (const auto& path : gone) {
                    sessionSave(path, std::nullopt);
                    written_.erase(path);
                }
                std::erase_if(tabs_, [&](const auto& t) { return t->path == confirmDelete_ || t->path.starts_with(confirmDelete_ + "/"); });
                currentTab_ = -1;
                selectedFile_.clear();
            } else {
                ed_.errorText_ = error;
            }
            confirmDelete_.clear();
            refreshFiles();
            requestAnalysis();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(NOX_TRI("editor.common.cancel", "Cancel"), ImVec2(120 * s, 0))) {
            confirmDelete_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (closeAsk_) {
        ImGui::OpenPopup(NOX_TRI("editor.mod.close.title", "Close Mod?"));
        closeAsk_ = false;
    }
    if (ImGui::BeginPopupModal(NOX_TRI("editor.mod.close.title", "Close Mod?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(NOX_TRC("editor.mod.close.text", "The mod has unsaved changes."));
        if (ImGui::Button(NOX_TRI("editor.common.save", "Save"), ImVec2(100 * s, 0))) {
            if (saveAll()) {
                askCancelled_ = {};
                if (auto then = std::exchange(askThen_, {})) then(); // (what asked: Close Mod, the editor's Close or quit)
            } else {
                askThen_ = {}; // (not saved: as a Cancel)
                if (auto cancelled = std::exchange(askCancelled_, {})) cancelled();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(NOX_TRI("editor.common.dont_save", "Don't Save"), ImVec2(100 * s, 0))) {
            const bool session = sessionMod_;
            closeProject(); // (the changes dropped with it)
            sessionDismissed_ = session;
            askCancelled_ = {};
            if (auto then = std::exchange(askThen_, {})) then();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(NOX_TRI("editor.common.cancel", "Cancel"), ImVec2(100 * s, 0))) {
            askThen_ = {};
            if (auto cancelled = std::exchange(askCancelled_, {})) cancelled();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// --- Test steps --------------------------------------------------------------------------------

bool ModPanel::runStep(const std::string& step)
{
    if (!step.starts_with("mod-")) {
        return false;
    }
    if (step.starts_with("mod-new:")) {
        const std::string rest = step.substr(8);
        const auto comma = rest.find(',');
        const std::string id = rest.substr(0, comma);
        newMod(id, comma == std::string::npos ? id : rest.substr(comma + 1));
    } else if (step.starts_with("mod-open:")) {
        const std::string id = step.substr(9);
        for (const auto& l : editor::listMods(modsDir())) {
            if (l.folder.filename().string() == id) {
                openMod(l);
                return true;
            }
        }
        ed_.status("No mod " + id + " in " + pathText(modsDir()));
    } else if (!project_ && step != "mod-close") {
        ed_.status("No mod is open for " + step);
    } else if (step.starts_with("mod-open-file:")) {
        openFile(step.substr(14));
        focusCode_ = 2;
    } else if (step.starts_with("mod-type:")) {
        if (Tab* t = selectTab_ >= 0 ? tabs_[static_cast<std::size_t>(selectTab_)].get() : current()) {
            t->code.insertText(unescaped(step.substr(9)));
            t->dirty = t->code.text() != t->saved;
            analysisDueMs_ = SDL_GetTicks() + kAnalysisPauseMs;
        }
    } else if (step.starts_with("mod-goto:")) {
        if (Tab* t = selectTab_ >= 0 ? tabs_[static_cast<std::size_t>(selectTab_)].get() : current()) {
            const auto comma = step.find(',', 9);
            t->code.gotoLine(std::max(1, std::atoi(step.c_str() + 9)), comma == std::string::npos ? 1 : std::max(1, std::atoi(step.c_str() + comma + 1)));
        }
    } else if (step == "mod-save") {
        saveAll();
    } else if (step == "mod-publish") {
        ed_.commands_.run("mod.publish"); // (a session's host: the pending files written)
    } else if (step.starts_with("mod-name:")) {
        form_.name = step.substr(9);
        showForm_ = true;
    } else if (step == "mod-play" || step.starts_with("mod-play:")) {
        playMod(step.size() > 9 ? step.substr(9) : std::string());
    } else if (step == "mod-preview") {
        ed_.commands_.run("mod.preview");
    } else if (step.starts_with("mod-preview-send:")) {
        if (const auto i = sampleIndex(preview_, step.substr(17))) {
            preview_.send(*i);
            previewLog_.push_back(nox::l10n::format(NOX_TR("editor.mod.preview.sent", "sent {name}"), {{"name", preview_.samples()[*i].name}}));
        } else {
            ed_.status("No test message " + step.substr(17));
        }
    } else if (step.starts_with("mod-preview-click:")) {
        char window[64] = {};
        float x = 0, y = 0;
        if (std::sscanf(step.c_str() + 18, "%63[^,],%f,%f", window, &x, &y) == 3) {
            preview_.click(window, x, y);
        }
    } else if (step.starts_with("mod-export:")) {
        exportTo(u8path(step.substr(11)));
    } else if (step.starts_with("mod-reference:")) {
        showReference_ = true;
        std::snprintf(refSearch_, sizeof refSearch_, "%s", step.c_str() + 14);
    } else if (step == "mod-reference-ui" || step == "mod-reference-server") {
        refHalf_ = step.ends_with("ui") ? 1 : 0;
    } else if (step == "mod-close") {
        closeProject();
    } else {
        ed_.status("No mod step " + step);
    }
    return true;
}

} // namespace noxsdl
