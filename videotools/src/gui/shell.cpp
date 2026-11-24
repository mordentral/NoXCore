// The NoX Content Editor's window (shell.hpp).
#include "gui/shell.hpp"

#include "editor/documents.hpp" // (checkSaveTarget)
#include "gui/panels.hpp"
#include "image_source.hpp"

#include "editor/imgui_host.hpp"
#include "editor/theme.hpp"
#include "editor/theme_colours.hpp"

#include "noxcore/strings/l10n.hpp"
#include "noxcore/util/text.hpp"

#include <imgui.h>
#include <imgui_internal.h> // (DockBuilder: the default layout)

#include <algorithm>
#include <cstdio>
#include <format>

namespace fs = std::filesystem;

namespace videotools::gui {

using content::FileKind;
using content::RecordList;
using content::RecordRef;

namespace {

fs::path exeDir()
{
    const char* base = SDL_GetBasePath();
    return base ? pathFromUtf8(base) : fs::path();
}

/// The mods folder: as set, else mods beside this program (where noxcore.exe keeps them when the
/// editor sits beside it).
fs::path modsFolder(const Settings& s)
{
    // The one noxcore.exe reads: mods beside it, so beside this program (put beside it). Never the
    // game folder's (a pack saved there would be written into the game's install, and read by
    // nothing).
    if (!s.modsDir.empty()) {
        return pathFromUtf8(s.modsDir);
    }
    return exeDir() / "mods";
}

noxsdl::editor::Theme themeOf(const Settings& s)
{
    noxsdl::editor::Theme t = noxsdl::editor::Theme::Dark;
    noxsdl::editor::themeFromKey(s.theme, t);
    return t;
}

/// A panel's window title, in the editor's language (its window known by the English name: every
/// Begin, dock and focus of it goes through here).
const char* panelTitle(std::string_view name)
{
    if (name == panel::kContent) return NOX_TRI("content.panel.content", "Content");
    if (name == panel::kRecord) return NOX_TRI("content.panel.record", "Record");
    if (name == panel::kImages) return NOX_TRI("content.panel.images", "Images");
    if (name == panel::kSprites) return NOX_TRI("content.panel.sprites", "Sprites");
    if (name == panel::kPreview) return NOX_TRI("content.panel.preview", "Preview");
    if (name == panel::kReferences) return NOX_TRI("content.panel.references", "References");
    if (name == panel::kChanges) return NOX_TRI("content.panel.changes", "Changes");
    if (name == panel::kProblems) return NOX_TRI("content.panel.problems", "Problems");
    if (name == panel::kLog) return NOX_TRI("content.panel.log", "Log");
    return "?";
}

/// "thing:GruntAxe" -> the list and the name.
std::optional<RecordList> listNamed(std::string_view word)
{
    static constexpr std::pair<const char*, RecordList> kLists[] = {
        {"thing", RecordList::Thing},       {"spell", RecordList::Spell},     {"ability", RecordList::Ability},
        {"sound", RecordList::SoundEvent},  {"floor", RecordList::Floor},     {"edge", RecordList::Edge},
        {"wall", RecordList::Wall},         {"image", RecordList::NamedImage}, {"weapon", RecordList::Weapon},
        {"armor", RecordList::Armor},       {"armour", RecordList::Armor},    {"modifier", RecordList::Modifier},
        {"tuning", RecordList::Tuning},     {"monster", RecordList::Monster}, {"soundset", RecordList::SoundSet},
        {"string", RecordList::String},     {"sample", RecordList::Sample},    {"file", RecordList::File},
    };
    const std::string w = nox::util::lowered(word);
    for (const auto& [name, list] : kLists) {
        if (w == name) {
            return list;
        }
    }
    return std::nullopt;
}

} // namespace

void Workspace::select(const RecordRef& ref)
{
    selected = ref;
    scrollContentToSelection = true;
    focus(panel::kRecord);
}

void Workspace::syncNewThings()
{
    const auto* base = pack.baseThings();
    const auto* work = pack.workingThings();
    if (pack.newThings() != newThingsListed && base && work) {
        newThingsListed = pack.newThings();
        // (a new type taken out moves those after it: what was selected may be another one now)
        if (selected && selected->list == RecordList::Thing && std::size_t(selected->index) >= base->things.size()) {
            selected.reset();
        }
        index.setNewThings(*work, base->things.size());
    }
    if (!selectAfterSync.empty()) {
        if (const auto ref = index.findByName(RecordList::Thing, selectAfterSync)) {
            select(*ref);
        }
        selectAfterSync.clear();
    }
}

struct Shell::Panels {
    explicit Panels(Workspace& ws) : browser(ws), record(ws), preview(ws), references(ws) {}
    ContentBrowser browser;
    RecordPanel record;
    PreviewPanel preview;
    ReferencesPanel references;
    bool showContent = true, showRecord = true, showImages = true, showSprites = true, showPreview = true, showReferences = true,
         showChanges = true, showProblems = true, showLog = true;
    bool* flag(std::string_view name)
    {
        if (name == panel::kContent) return &showContent;
        if (name == panel::kRecord) return &showRecord;
        if (name == panel::kImages) return &showImages;
        if (name == panel::kSprites) return &showSprites;
        if (name == panel::kPreview) return &showPreview;
        if (name == panel::kReferences) return &showReferences;
        if (name == panel::kChanges) return &showChanges;
        if (name == panel::kProblems) return &showProblems;
        if (name == panel::kLog) return &showLog;
        return nullptr;
    }
};

Shell::Shell(SDL_Window* window, SDL_Renderer* renderer, noxsdl::editor::ImGuiHost& host, fs::path settingsFile, Settings settings,
             bool screenshotRun)
    : window_(window)
    , host_(host)
    , settingsFile_(std::move(settingsFile))
    , settings_(std::move(settings))
    , session_(window, renderer, host.scale(), settings_)
    , catalog_(session_,
               [this](std::int32_t i) {
                   images_.show(i);
                   focus(panel::kImages);
               })
    , images_(session_)
    , screenshotRun_(screenshotRun)
{
    ws_.reset(new Workspace(session_, catalog_, images_));
    ws_->focus = [this](const char* name) { focus(name); };
    ws_->changedRecords = [this] {
        std::vector<RecordRef> out;
        for (const auto& e : ws_->pack.edits()) {
            if (const auto ref = recordOf(e); ref && std::ranges::find(out, *ref) == out.end()) {
                out.push_back(*ref);
            }
        }
        for (const auto& n : ws_->pack.newThings()) {
            if (const auto ref = ws_->index.findByName(RecordList::Thing, n.name); ref && std::ranges::find(out, *ref) == out.end()) {
                out.push_back(*ref);
            }
        }
        for (const auto& e : ws_->pack.thingEdits()) {
            if (const auto ref = ws_->index.findByName(RecordList::Thing, e.type); ref && std::ranges::find(out, *ref) == out.end()) {
                out.push_back(*ref);
            }
        }
        for (const auto& e : ws_->pack.stringEdits()) {
            if (const auto ref = ws_->index.findByName(RecordList::String, e.label); ref && std::ranges::find(out, *ref) == out.end()) {
                out.push_back(*ref);
            }
        }
        for (const auto& e : ws_->pack.soundEdits()) {
            if (const auto ref = soundRecord(e); ref && std::ranges::find(out, *ref) == out.end()) {
                out.push_back(*ref);
            }
        }
        return out;
    };
    panels_ = std::make_unique<Panels>(*ws_);
    std::snprintf(gameBuf_, sizeof gameBuf_, "%s", settings_.gameDir.c_str());
    std::snprintf(catalog_.outBuf(), catalog_.outBufSize(), "%s", settings_.outDir.c_str());
}

Shell::~Shell()
{
    settings_.outDir = catalog_.outBuf();
    if (!settingsFile_.empty()) {
        saveSettings(settingsFile_, settings_);
    }
}

void Shell::openGame()
{
    settings_.gameDir = gameBuf_;
    const fs::path game = pathFromUtf8(gameBuf_);
    session_.openGame(game);
    content::GameFileDirs dirs;
    dirs.gameDir = game;
    dirs.saveDir = settings_.charactersDir.empty() ? fs::path() : pathFromUtf8(settings_.charactersDir);
    dirs.modsDir = modsFolder(settings_);
    modsDir_ = dirs.modsDir;
    ws_->files = content::GameFiles::load(dirs);
    ws_->index = content::ContentIndex::build(ws_->files);
    ws_->newThingsListed.clear(); // (the new index lists none)
    ws_->selected.reset();
    ws_->pack.openBase(game);
    ws_->pack.closePack();
    ws_->mapUsage = std::async(std::launch::async, [maps = ws_->files.maps()] { return content::MapUsage::scan(maps); }).share();
    if (!settings_.packId.empty()) {
        openContentPack(settings_.packId); // (the pack edited last)
    }
    const content::FileStatus* things = ws_->files.statusOf(FileKind::Things);
    catalog_.load(ws_->files.things(), things ? things->error : std::string());
    images_.refresh();
    std::size_t failed = 0;
    for (const auto& s : ws_->files.status()) {
        if (!s.loaded && !s.optional) {
            ++failed;
            session_.log(std::format("{}: {}", utf8(s.path), s.error));
        }
    }
    session_.setStatus(nox::l10n::format(NOX_TR("content.shell.opened", "Opened {folder}: {n} records"), {{"folder", gameBuf_}, {"n", ws_->index.recordCount()}}) +
                       (failed ? nox::l10n::format(NOX_TR("content.shell.opened_failed", ", {n} files missing or broken (see Problems)"), {{"n", failed}})
                               : std::string()));
}

void Shell::openPack(const fs::path& p)
{
    session_.openPack(p);
    images_.refresh();
}

void Shell::focus(const char* name)
{
    if (bool* f = panels_->flag(name)) {
        *f = true;
    }
    wantFocus_.emplace_back(name);
}

bool Shell::selectByText(std::string_view text)
{
    const auto colon = text.find(':');
    if (colon != std::string_view::npos) {
        if (const auto list = listNamed(text.substr(0, colon))) {
            if (const auto ref = ws_->index.findByName(*list, text.substr(colon + 1))) {
                ws_->select(*ref);
                return true;
            }
            return false;
        }
    }
    // A record by name anywhere, the first found.
    const auto found = ws_->index.search(text, 1);
    if (!found.empty()) {
        ws_->select(found.front());
        return true;
    }
    return false;
}

void Shell::setSearch(std::string_view text)
{
    panels_->browser.setSearch(text);
    focus(panel::kContent);
}

std::size_t Shell::edits() const
{
    auto& s = const_cast<Session&>(session_);
    return (s.bag() ? s.bag()->editCount() : 0) + (s.pack() ? s.pack()->editCount() : 0);
}

bool Shell::mayQuit()
{
    const bool packUnsaved = ws_->pack.dirty() && ws_->pack.changeCount() > 0;
    if ((edits() == 0 && !packUnsaved) || quitConfirmed_) {
        return true;
    }
    session_.confirm(NOX_TR("content.shell.quit_title", "Quit without saving?"),
                     packUnsaved ? NOX_TR("content.shell.quit_pack", "The content pack's changes aren't saved. Quit anyway?")
                                 : nox::l10n::format(NOX_TR("content.shell.quit_edits", "{n} edits aren't saved. Quit anyway?"), {{"n", edits()}}),
                     [this] { quitConfirmed_ = true; });
    return false;
}

// ---------------------------------------------------------------------------------------------
// The content pack

void Shell::openContentPack(const std::string& id)
{
    if (id.empty()) {
        ws_->pack.closePack();
        ws_->syncNewThings(); // (its new types out of Content; never from inside a panel)
        settings_.packId.clear();
        return;
    }
    const fs::path dir = modsDir_ / id / "content";
    packProblems_.clear();
    for (const std::string& p : ws_->pack.openPack(dir, id)) {
        const std::string line = nox::l10n::format(NOX_TR("content.shell.pack_problem", "content pack {id}: {problem}"), {{"id", id}, {"problem", p}});
        session_.log(line);
        packProblems_.push_back(line);
    }
    ws_->syncNewThings(); // (its new types listed in Content now: --show finds them)
    settings_.packId = id;
    session_.setStatus(nox::l10n::format(NOX_TR("content.shell.pack_opened", "Content pack {id}: {n} changes ({folder})"),
                                         {{"id", id}, {"n", ws_->pack.changeCount()}, {"folder", utf8(dir)}}));
}

void Shell::savePack()
{
    if (!ws_->pack.packOpen()) {
        newPackOpen_ = true; // (a pack to save into first)
        saveAfterNewPack_ = true;
        return;
    }
    try {
        ws_->pack.save();
        session_.setStatus(nox::l10n::format(NOX_TR("content.shell.pack_saved", "Saved content pack {id}: {n} changes in {folder}"),
                                             {{"id", ws_->pack.packId()}, {"n", ws_->pack.changeCount()}, {"folder", utf8(ws_->pack.contentDir())}}),
                           ws_->pack.contentDir());
    } catch (const std::exception& ex) {
        session_.error(nox::l10n::format(NOX_TR("content.shell.cant_save_pack", "Can't save the content pack: {error}"), {{"error", ex.what()}}));
    }
}

void Shell::exportFull(const fs::path& dir)
{
    // Never into the game's folder (or any holding GAME.EXE): whole files go to a folder of their own.
    const std::string refused = editor::checkSaveTarget(dir / "gamedata.bin", {}, {pathFromUtf8(gameBuf_)});
    if (!refused.empty()) {
        session_.error(nox::l10n::format(NOX_TR("content.common.not_there", "Not there: {why}"), {{"why", refused}}));
        return;
    }
    try {
        const auto written = ws_->pack.exportFull(dir);
        session_.setStatus(nox::l10n::format(NOX_TR("content.shell.exported", "Exported {n} whole files to {folder}"), {{"n", written.size()}, {"folder", utf8(dir)}}), dir);
    } catch (const std::exception& ex) {
        session_.error(nox::l10n::format(NOX_TR("content.shell.cant_export", "Can't export: {error}"), {{"error", ex.what()}}));
    }
}

std::optional<RecordRef> Shell::recordOf(const content::FieldEdit& e) const
{
    switch (e.format) {
    case content::TableFormat::Monster: return ws_->index.findByName(RecordList::Monster, e.record);
    case content::TableFormat::SoundSet: return ws_->index.findByName(RecordList::SoundSet, e.record);
    case content::TableFormat::GameData: return ws_->index.findByName(RecordList::Tuning, e.key);
    case content::TableFormat::Modifier:
        return ws_->index.findByName(e.section == "WEAPON_DEFINITIONS"  ? RecordList::Weapon
                                     : e.section == "ARMOR_DEFINITIONS" ? RecordList::Armor
                                                                        : RecordList::Modifier,
                                     e.record);
    }
    return std::nullopt;
}

void Shell::packPopups()
{
    if (newPackOpen_) {
        ImGui::OpenPopup(NOX_TRI("content.shell.new_pack_title", "New content pack"));
        newPackOpen_ = false;
        newPackBuf_[0] = 0;
    }
    if (ImGui::BeginPopupModal(NOX_TRI("content.shell.new_pack_title", "New content pack"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(NOX_TRC("content.shell.new_pack_where", "A content pack is a folder in the mods folder:"));
        ImGui::TextDisabled("%s", utf8(modsDir_ / "<name>" / "content").c_str());
        ImGui::TextUnformatted(NOX_TRC("content.shell.new_pack_name", "Its name (letters, digits, - and _):"));
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        const bool enter = ImGui::InputText("##id", newPackBuf_, sizeof newPackBuf_, ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string id = newPackBuf_;
        const bool valid = nox::util::plainName(id, sizeof newPackBuf_); // (the box holds fewer)
        std::error_code ec;
        const bool exists = valid && fs::exists(modsDir_ / id, ec);
        if (exists) {
            ImGui::TextColored(noxsdl::editor::themeColours().warning, "%s",
                               nox::l10n::format(NOX_TR("content.shell.new_pack_exists", "mods\\{id} is there already: its pack is opened (its changes join yours)."), {{"id", id}}).c_str());
        }
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button(exists ? NOX_TRI("content.shell.new_pack_open", "Open") : NOX_TRI("content.shell.new_pack_make", "Make")) || (enter && valid)) {
            // The edits made so far go along into the pack.
            const auto carried = ws_->pack.edits();
            const auto carriedThings = ws_->pack.thingEdits();
            openContentPack(id);
            for (const auto& e : carriedThings) {
                if (e.value) {
                    ws_->pack.setThing(e.type, e.key, *e.value);
                } else {
                    ws_->pack.removeThing(e.type, e.key);
                }
            }
            for (const auto& e : carried) {
                if (e.value) {
                    ws_->pack.set(e.format, e.section, e.record, e.key, e.kind, *e.value);
                } else {
                    ws_->pack.remove(e.format, e.section, e.record, e.key, e.kind);
                }
            }
            ImGui::CloseCurrentPopup();
            if (saveAfterNewPack_) {
                saveAfterNewPack_ = false;
                savePack();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(NOX_TRI("content.common.cancel", "Cancel"))) {
            saveAfterNewPack_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Shell::packBar()
{
    const float s = host_.scale();
    auto& pack = ws_->pack;
    ImGui::SameLine(0, 20 * s);
    ImGui::TextUnformatted(NOX_TRC("content.shell.pack_label", "Pack"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170 * s);
    const std::string current = pack.packOpen() ? pack.packId() : NOX_TR("content.shell.pack_none", "(none)");
    if (ImGui::BeginCombo("##pack", current.c_str())) {
        if (ImGui::Selectable(NOX_TRI("content.shell.pack_none_item", "(none: no pack open)"), !pack.packOpen())) {
            openContentPack({});
        }
        for (const fs::path& m : ws_->files.mods()) {
            const std::string id = m.filename().string();
            if (ImGui::Selectable(id.c_str(), pack.packOpen() && pack.packId() == id)) {
                openContentPack(id);
            }
        }
        ImGui::Separator();
        if (ImGui::Selectable(NOX_TRI("content.shell.pack_new_item", "New pack..."))) {
            newPackOpen_ = true;
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", NOX_TRC("content.shell.pack_tip", "The content pack being edited: mods\\<name>\\content\\ (patch files of the changes; installed by hand, never downloaded)"));
    ImGui::SameLine();
    ImGui::BeginDisabled(pack.changeCount() == 0 && !pack.dirty());
    if (ImGui::Button(pack.dirty() ? NOX_TRI("content.shell.save_pack_dirty", "Save pack *") : NOX_TRI("content.shell.save_pack", "Save pack"))) {
        savePack();
    }
    ImGui::EndDisabled();
}

void Shell::askThenOpenGame()
{
    if (edits() || (ws_->pack.dirty() && ws_->pack.changeCount() > 0)) {
        session_.confirm(NOX_TR("content.shell.drop_edits_title", "Drop the edits?"),
                         NOX_TR("content.shell.drop_edits", "Opening a game folder drops the edits that aren't saved. Go on?"), [this] { openGame(); });
    } else {
        openGame();
    }
}

void Shell::applyTheme()
{
    host_.setStyle(float(settings_.uiScalePercent) / 100.0f, themeOf(settings_));
}

// ---------------------------------------------------------------------------------------------
// Menus and bars

void Shell::menuBar()
{
    if (!ImGui::BeginMenuBar()) {
        return;
    }
    auto* bag = session_.bag();
    auto* pack = session_.pack();
    const bool busy = session_.busy() || session_.inserting();
    if (ImGui::BeginMenu(NOX_TRI("content.menu.file", "File"))) {
        if (ImGui::MenuItem(NOX_TRI("content.menu.open_game", "Open game folder..."), nullptr, false, !busy)) {
            session_.dialogs().request(kDialogGameDir, Dialogs::Folder, session_.window(), gameBuf_);
        }
        if (ImGui::MenuItem(NOX_TRI("content.menu.reload_game", "Reload game folder"), nullptr, false, !busy)) {
            askThenOpenGame();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(NOX_TRI("content.menu.open_hd_pack", "Open HD pack..."), nullptr, false, !busy)) {
            const std::string start = pack && !pack->path().empty() ? utf8(pack->path()) : settings_.packPath;
            session_.dialogs().request(kDialogPack, Dialogs::Open, session_.window(), start, packFileFilter(), 1);
        }
        if (ImGui::MenuItem(NOX_TRI("content.menu.new_hd_pack", "New HD pack"), nullptr, false, !busy && bag)) {
            session_.newPack();
            images_.refresh();
        }
        if (ImGui::MenuItem(NOX_TRI("content.menu.close_hd_pack", "Close HD pack"), nullptr, false, !busy && pack)) {
            session_.closePack();
            images_.refresh();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(NOX_TRI("content.menu.save_bag_as", "Save video.bag as..."), nullptr, false, !busy && bag)) {
            session_.saveBagAs();
        }
        if (ImGui::MenuItem(NOX_TRI("content.menu.save_pack_as", "Save pack as..."), nullptr, false, !busy && pack)) {
            session_.savePackAs();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(NOX_TRI("content.menu.new_content_pack", "New content pack..."))) {
            newPackOpen_ = true;
        }
        if (ImGui::MenuItem(NOX_TRI("content.menu.save_content_pack", "Save content pack"), "Ctrl+S", false, ws_->pack.changeCount() > 0 || ws_->pack.dirty())) {
            savePack();
        }
        if (ImGui::MenuItem(NOX_TRI("content.menu.export_whole", "Export whole files..."), nullptr, false, ws_->pack.changeCount() > 0)) {
            session_.dialogs().request(kDialogExportFull, Dialogs::Folder, session_.window(), settings_.saveDir);
        }
        ImGui::SetItemTooltip("%s", NOX_TRC("content.menu.export_whole_tip", "The changed files written whole, in the game's own format (encrypted), into a folder you pick: for use outside NoX Core."));
        ImGui::Separator();
        if (ImGui::MenuItem(NOX_TRI("content.menu.quit", "Quit"))) {
            SDL_Event e{};
            e.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&e);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(NOX_TRI("content.menu.edit", "Edit"))) {
        if (ImGui::MenuItem(NOX_TRI("content.menu.revert_pack", "Revert every pack change"), nullptr, false, ws_->pack.changeCount() > 0)) {
            session_.confirm(NOX_TR("content.menu.revert_pack_title", "Revert the pack?"),
                             nox::l10n::format(NOX_TR("content.menu.revert_pack_ask", "Drop all {n} changes in the content pack (back to the game's own values)?"),
                                               {{"n", ws_->pack.changeCount()}}),
                             [this] { ws_->pack.revertAll(); });
        }
        if (ImGui::MenuItem(NOX_TRI("content.menu.revert_images", "Revert every image edit"), nullptr, false, edits() > 0)) {
            session_.confirm(NOX_TR("content.menu.revert_images_title", "Revert everything?"),
                             nox::l10n::format(NOX_TR("content.menu.revert_images_ask", "Drop all {n} image edits?"), {{"n", edits()}}), [this] {
                std::vector<std::int32_t> all;
                if (auto* b = session_.bag()) {
                    for (const auto& [i, _] : b->bagEdits()) all.push_back(std::int32_t(i));
                }
                if (auto* p = session_.pack()) {
                    for (const auto& [i, _] : p->packEdits()) all.push_back(std::int32_t(i));
                }
                session_.revert(all);
                images_.refresh();
            });
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(NOX_TRI("content.menu.view", "View"))) {
        for (const char* name : {panel::kContent, panel::kRecord, panel::kImages, panel::kSprites, panel::kPreview, panel::kReferences,
                                 panel::kChanges, panel::kProblems, panel::kLog}) {
            ImGui::MenuItem(panelTitle(name), nullptr, panels_->flag(name));
        }
        ImGui::Separator();
        if (ImGui::BeginMenu(NOX_TRI("content.menu.theme", "Theme"))) {
            for (const auto t : noxsdl::editor::kThemeMenuOrder) {
                const bool on = themeOf(settings_) == t;
                if (ImGui::MenuItem(noxsdl::editor::themeLabel(t), nullptr, on) && !on) {
                    settings_.theme = noxsdl::editor::themeKey(t);
                    applyTheme();
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(NOX_TRI("content.menu.interface_size", "Interface size"))) {
            for (int pct : {75, 90, 100, 110, 125, 150, 175, 200}) {
                if (ImGui::MenuItem(std::format("{}%", pct).c_str(), nullptr, settings_.uiScalePercent == pct)) {
                    settings_.uiScalePercent = pct;
                    applyTheme();
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(NOX_TRI("content.menu.floating", "Floating panels"), nullptr, settings_.floating, host_.floatingSupported())) {
            settings_.floating = !settings_.floating;
            host_.setFloating(settings_.floating);
        }
        ImGui::SetItemTooltip("%s", NOX_TRC("content.menu.floating_tip", "Panels dragged out of the window become windows of their own (onto another monitor)."));
        if (ImGui::MenuItem(NOX_TRI("content.menu.reset_layout", "Reset layout"))) {
            resetLayout_ = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(NOX_TRI("content.menu.help", "Help"))) {
        ImGui::TextDisabled("NoX Content Editor");
        ImGui::TextDisabled("%s", NOX_TRC("content.menu.help_opens", "Opens the game's files: thing.bin, modifier.bin, gamedata.bin,"));
        ImGui::TextDisabled("monster.bin, soundset.bin, nox.csf, video.bag, Audio.bag...");
        ImGui::TextDisabled("%s", NOX_TRC("content.menu.help_never_written", "Original files are never written: saves go to new files."));
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void Shell::topBar()
{
    if (const auto p = session_.dialogs().take(kDialogGameDir)) {
        std::snprintf(gameBuf_, sizeof gameBuf_, "%s", p->c_str());
        askThenOpenGame();
    }
    if (const auto p = session_.dialogs().take(kDialogExportFull)) {
        settings_.saveDir = *p;
        exportFull(pathFromUtf8(*p));
    }
    if (const auto p = session_.dialogs().take(kDialogPack)) {
        const fs::path path = pathFromUtf8(*p);
        if (session_.pack() && session_.pack()->editCount()) {
            session_.confirm(NOX_TR("content.shell.drop_pack_title", "Drop the pack's edits?"),
                             NOX_TR("content.shell.drop_pack_open", "The open pack has edits that aren't saved. Open another anyway?"), [this, path] { openPack(path); });
        } else {
            openPack(path);
        }
    }
    const auto& theme = noxsdl::editor::themeColours();
    const float s = host_.scale();
    const bool busy = session_.busy() || session_.inserting();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetStyle().WindowPadding.x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(NOX_TRC("content.shell.game_folder", "Game folder"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(280 * s);
    const bool enter = ImGui::InputText("##game", gameBuf_, sizeof gameBuf_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(NOX_TRI("content.shell.browse_game", "Browse##game"))) {
        session_.dialogs().request(kDialogGameDir, Dialogs::Folder, session_.window(), gameBuf_);
    }
    ImGui::SameLine();
    if (ImGui::Button(NOX_TRI("content.shell.open_game", "Open##game")) || (enter && !busy)) {
        askThenOpenGame();
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0, 20 * s);
    ImGui::TextUnformatted(NOX_TRC("content.shell.hd_pack", "HD pack"));
    ImGui::SameLine();
    auto* pack = session_.pack();
    const std::string packName = !pack                  ? NOX_TR("content.shell.hd_pack_none", "none")
                                 : pack->path().empty() ? NOX_TR("content.shell.hd_pack_new", "new (not saved yet)")
                                                        : utf8(pack->path().filename());
    ImGui::TextColored(pack ? theme.info : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s", packName.c_str());
    if (pack && !pack->path().empty()) {
        ImGui::SetItemTooltip("%s", utf8(pack->path()).c_str());
    }
    if (pack && !session_.packMatchesBag()) {
        ImGui::SameLine();
        ImGui::TextColored(theme.caution, "%s", NOX_TRC("content.shell.hd_pack_other_idx", "(made for another video.idx)"));
        ImGui::SetItemTooltip("%s", NOX_TRC("content.shell.hd_pack_other_idx_tip", "NoX Core uses a pack only with the video.idx it was made for. Stamp it when you save it."));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(NOX_TRI("content.shell.open_pack", "Open...##pack"))) {
        const std::string start = pack && !pack->path().empty() ? utf8(pack->path()) : settings_.packPath;
        session_.dialogs().request(kDialogPack, Dialogs::Open, session_.window(), start, packFileFilter(), 1);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!session_.bagOpen());
    if (ImGui::Button(NOX_TRI("content.shell.new_pack", "New##pack"))) {
        const auto make = [this] {
            session_.newPack();
            images_.refresh();
        };
        if (pack && pack->editCount()) {
            session_.confirm(NOX_TR("content.shell.drop_pack_title", "Drop the pack's edits?"),
                             NOX_TR("content.shell.drop_pack_new", "The open pack has edits that aren't saved. Start a new one anyway?"), make);
        } else {
            make();
        }
    }
    ImGui::SetItemTooltip("%s", NOX_TRC("content.shell.new_pack_tip", "An empty pack for this video.bag, to add HD images to"));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!pack);
    if (ImGui::Button(NOX_TRI("content.shell.close_pack", "Close##pack"))) {
        const auto close = [this] {
            session_.closePack();
            images_.refresh();
        };
        if (pack && pack->editCount()) {
            session_.confirm(NOX_TR("content.shell.drop_pack_title", "Drop the pack's edits?"),
                             NOX_TR("content.shell.drop_pack_close", "The open pack has edits that aren't saved. Close it anyway?"), close);
        } else {
            close();
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0, 20 * s);
    auto* bag = session_.bag();
    ImGui::BeginDisabled(!bag);
    if (ImGui::Button(std::format("{}{}###saveBag", NOX_TR("content.shell.save_bag_as", "Save video.bag as..."),
                                  bag && bag->editCount() ? std::format(" ({})", bag->editCount()) : "")
                          .c_str())) {
        session_.saveBagAs();
    }
    ImGui::SetItemTooltip("%s", NOX_TRC("content.shell.save_bag_tip", "Writes a new video.bag and video.idx (never the game's own)"));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!pack);
    if (ImGui::Button(std::format("{}{}###savePack", NOX_TR("content.shell.save_hd_pack_as", "Save pack as..."),
                                  pack && pack->editCount() ? std::format(" ({})", pack->editCount()) : "")
                          .c_str())) {
        session_.savePackAs();
    }
    ImGui::SetItemTooltip("%s", NOX_TRC("content.shell.save_hd_pack_tip", "Writes a new .nxp (never over the open one)"));
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    packBar();

    // The changes' reach: the strongest of them (image edits are seen only by those who install them).
    ImGui::SameLine(0, 20 * s);
    ImGui::TextUnformatted(NOX_TRC("content.shell.reach", "Reach:"));
    ImGui::SameLine();
    std::optional<content::Reach> reach = ws_->pack.reach();
    if (edits() > 0) {
        reach = reach ? content::stronger(*reach, content::Reach::ClientVisible) : content::Reach::ClientVisible;
    }
    if (!reach) {
        ImGui::TextDisabled("%s", NOX_TRC("content.shell.no_changes", "no changes"));
    } else {
        ImGui::TextColored(*reach == content::Reach::ClientVisible ? theme.ok : *reach == content::Reach::ServerOnly ? theme.info : theme.caution, "%s",
                           content::reachLabel(*reach));
        ImGui::SetItemTooltip("%s", content::reachDocs(*reach));
    }
}

void Shell::statusBar()
{
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetStyle().WindowPadding.x);
    if (auto* job = session_.job()) {
        const auto& p = job->progress;
        const std::size_t total = std::max<std::size_t>(1, p.total.load());
        const std::string overlay = std::format("{}  {} / {}  {}", job->label, p.done.load(), p.total.load(), p.current());
        ImGui::ProgressBar(float(p.done.load()) / float(total), ImVec2(-100 * host_.scale(), 0), overlay.c_str());
        ImGui::SameLine();
        if (ImGui::Button(NOX_TRI("content.common.cancel", "Cancel"), ImVec2(-FLT_MIN, 0))) {
            job->progress.cancel = true;
        }
        return;
    }
    ImGui::AlignTextToFramePadding();
    const std::string& status = session_.status();
    const auto nl = status.find('\n');
    ImGui::TextUnformatted(status.substr(0, nl).c_str());
    if (!session_.statusFolder().empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton(NOX_TRI("content.shell.open_folder", "Open folder"))) {
            SDL_OpenURL(("file:///" + utf8(session_.statusFolder().generic_u8string())).c_str());
        }
    }
}

void Shell::catalogToolbar()
{
    if (const auto p = session_.dialogs().take(kDialogExportDir)) {
        std::snprintf(catalog_.outBuf(), catalog_.outBufSize(), "%s", p->c_str());
    }
    const float s = host_.scale();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(NOX_TRC("content.shell.export_to", "Export to"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(300 * s);
    ImGui::InputText("##out", catalog_.outBuf(), catalog_.outBufSize());
    ImGui::SameLine();
    if (ImGui::Button(NOX_TRI("content.shell.browse_out", "Browse##out"))) {
        session_.dialogs().request(kDialogExportDir, Dialogs::Folder, session_.window(), catalog_.outBuf());
    }
    ImGui::SameLine();
    if (ImGui::Button(NOX_TRI("content.shell.open_out", "Open##out"))) {
        std::error_code ec;
        fs::create_directories(pathFromUtf8(catalog_.outBuf()), ec);
        SDL_OpenURL(("file:///" + utf8(pathFromUtf8(catalog_.outBuf()).generic_u8string())).c_str());
    }
    ImGui::SameLine(0, 24 * s);
    ImGui::BeginDisabled(session_.busy() || !session_.bagOpen());
    if (ImGui::Button(NOX_TRI("content.shell.export_everything_button", "Export everything..."))) {
        ImGui::OpenPopup(NOX_TRI("content.shell.export_everything_title", "Export everything"));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", NOX_TRC("content.shell.export_everything_what", "PNG frames + animation.json per animation, from video.bag as saved on disk"));
    if (ImGui::BeginPopupModal(NOX_TRI("content.shell.export_everything_title", "Export everything"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::size_t total = 0;
        for (const auto& c : catalog_.catalog().categories) {
            total += c.imageCount();
        }
        ImGui::TextUnformatted(
            nox::l10n::format(NOX_TR("content.shell.export_everything_ask", "Export every category ({n} images, counting images used more than once) to"), {{"n", total}})
                .c_str());
        ImGui::TextUnformatted(catalog_.outBuf());
        ImGui::TextDisabled("%s", NOX_TRC("content.shell.export_everything_cost", "This takes a few minutes and a few gigabytes."));
        if (ImGui::Button(NOX_TRI("content.shell.export", "Export"))) {
            catalog_.exportEverything();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(NOX_TRI("content.common.cancel", "Cancel"))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------------------------
// The docked panels

void Shell::buildDefaultLayout(unsigned int dockspace)
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImVec2(std::max(avail.x, 400.0f), std::max(avail.y - 30, 300.0f)));
    ImGuiID centre = dockspace;
    ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.18f, nullptr, &centre);
    ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.22f, nullptr, &centre);
    ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.28f, nullptr, &centre);
    ImGuiID rightLow = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.40f, nullptr, &right);
    ImGui::DockBuilderDockWindow(panelTitle(panel::kContent), left);
    ImGui::DockBuilderDockWindow(panelTitle(panel::kRecord), centre);
    ImGui::DockBuilderDockWindow(panelTitle(panel::kImages), centre);
    ImGui::DockBuilderDockWindow(panelTitle(panel::kSprites), centre);
    ImGui::DockBuilderDockWindow(panelTitle(panel::kPreview), right);
    ImGui::DockBuilderDockWindow(panelTitle(panel::kReferences), rightLow);
    ImGui::DockBuilderDockWindow("###Changes", bottom); // (their titles carry counts: known by their ids)
    ImGui::DockBuilderDockWindow("###Problems", bottom);
    ImGui::DockBuilderDockWindow(panelTitle(panel::kLog), bottom);
    ImGui::DockBuilderFinish(dockspace);
    // The tabs shown first, before any asked for already (they come after, so they win).
    wantFocus_.insert(wantFocus_.begin(), {panel::kRecord, panel::kChanges});
}

void Shell::dockSpace()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##contentEditor", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
    menuBar();
    ImGui::Dummy(ImVec2(0, 2));
    topBar();
    const float statusH = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
    const ImGuiID dock = ImGui::GetID("ContentDock");
    if (resetLayout_ || !ImGui::DockBuilderGetNode(dock)) {
        resetLayout_ = false;
        buildDefaultLayout(dock);
    }
    ImGui::DockSpace(dock, ImVec2(0, -statusH));
    statusBar();
    ImGui::End();
}

std::optional<RecordRef> Shell::soundRecord(const content::SoundEdit& e) const
{
    if (e.folder == content::SoundEdit::Audio) {
        return ws_->index.findByName(RecordList::Sample, e.name);
    }
    for (std::size_t i = 0; i < ws_->files.dialog().size(); ++i) {
        if (nox::util::equalsNoCase(ws_->files.dialog()[i].stem().string(), e.name)) {
            return RecordRef{FileKind::Dialog, RecordList::File, std::int32_t(i)};
        }
    }
    return std::nullopt;
}

void Shell::collectChanges(std::vector<ChangeRow>& out)
{
    auto& pack = ws_->pack;
    for (std::size_t i = 0; i < pack.stringEdits().size(); ++i) {
        const content::StringEdit e = pack.stringEdits()[i];
        ChangeRow r;
        r.what = "nox.csf  " + e.label;
        r.detail = e.variants.empty() ? NOX_TR("content.changes.removed", "removed")
                                      : std::format("\"{}\"{}", nox::content::escapeStringsText(e.variants.front()).substr(0, 60),
                                                    e.variants.size() > 1 ? nox::l10n::format(NOX_TR("content.changes.more_variants", " (+{n} more)"), {{"n", e.variants.size() - 1}})
                                                                          : std::string());
        r.reach = content::reachLabel(content::Reach::ServerShown); // (the host's own text lines; the rest each player's)
        r.revert = [this, e] {
            auto& p = ws_->pack;
            for (std::size_t k = 0; k < p.stringEdits().size(); ++k) {
                if (p.stringEdits()[k].label == e.label) {
                    p.revertString(k);
                    break;
                }
            }
        };
        r.show = [this, e] {
            if (const auto ref = ws_->index.findByName(RecordList::String, e.label)) {
                ws_->select(*ref);
            }
        };
        out.push_back(std::move(r));
    }
    for (std::size_t i = 0; i < pack.soundEdits().size(); ++i) {
        const content::SoundEdit e = pack.soundEdits()[i];
        ChangeRow r;
        r.what = e.label();
        r.detail = e.source.empty() ? NOX_TR("content.changes.sound_taken_out", "taken out of the pack")
                   : e.saved      ? NOX_TR("content.changes.sound_in_pack", "in the pack")
                                  : nox::l10n::format(NOX_TR("content.changes.sound_from", "from {file}"), {{"file", utf8(e.source)}});
        r.reach = content::reachLabel(content::Reach::ClientVisible);
        r.revert = [this, e] {
            auto& p = ws_->pack;
            for (std::size_t k = 0; k < p.soundEdits().size(); ++k) {
                if (p.soundEdits()[k].folder == e.folder && p.soundEdits()[k].name == e.name) {
                    p.revertSound(k);
                    break;
                }
            }
        };
        r.show = [this, e] {
            if (const auto ref = soundRecord(e)) {
                ws_->select(*ref);
            }
        };
        out.push_back(std::move(r));
    }
    for (const content::NewThing& n : pack.newThings()) {
        ChangeRow r;
        r.what = nox::l10n::format(NOX_TR("content.changes.new_type", "thing.bin  new type {name}"), {{"name", n.name}});
        r.detail = nox::l10n::format(NOX_TR("content.changes.made_like", "made like {name}"), {{"name", n.like}});
        r.reach = content::reachLabel(content::Reach::Structural);
        r.revert = [this, name = n.name] {
            if (const std::string why = ws_->pack.removeNewThing(name); !why.empty()) {
                session_.error(why);
            }
        };
        r.show = [this, name = n.name] {
            if (const auto ref = ws_->index.findByName(RecordList::Thing, name)) {
                ws_->select(*ref);
            }
        };
        out.push_back(std::move(r));
    }
    for (std::size_t i = 0; i < pack.thingEdits().size(); ++i) {
        const content::ThingEdit& e = pack.thingEdits()[i];
        ChangeRow r;
        r.what = "thing.bin  " + e.label();
        std::string stock = pack.isNewThing(e.type) ? NOX_TR("content.changes.stock_new_type", "(new type)") : NOX_TR("content.changes.stock_none", "(none)");
        if (const auto* db = pack.baseThings()) {
            if (const auto* t = db->findThing(e.type)) {
                stock = nox::content::thingValue(*t, e.key).value_or(NOX_TR("content.changes.stock_none", "(none)"));
            }
        }
        r.detail = e.value ? std::format("{} -> {}", stock, *e.value)
                           : nox::l10n::format(NOX_TR("content.changes.value_removed", "{stock} -> removed"), {{"stock", stock}});
        r.reach = content::reachLabel(pack.reachOf(e));
        const content::ThingEdit copy = e;
        r.revert = [this, copy] {
            auto& p = ws_->pack;
            for (std::size_t k = 0; k < p.thingEdits().size(); ++k) {
                if (p.thingEdits()[k].sameTarget(copy)) {
                    p.revertThing(k);
                    break;
                }
            }
        };
        r.show = [this, copy] {
            if (const auto ref = ws_->index.findByName(RecordList::Thing, copy.type)) {
                ws_->select(*ref);
            }
        };
        out.push_back(std::move(r));
    }
    for (std::size_t i = 0; i < pack.edits().size(); ++i) {
        const content::FieldEdit& e = pack.edits()[i];
        ChangeRow r;
        r.what = std::string(nox::content::tableFileName(e.format)) + "  " + e.label();
        std::string stock = NOX_TR("content.changes.stock_none", "(none)");
        if (const auto* b = pack.base(e.format)) {
            if (const auto rec = b->find(e.record, e.section)) {
                stock = b->records()[*rec].effective(e.key, e.kind).value_or(NOX_TR("content.changes.stock_none", "(none)"));
            }
        }
        r.detail = e.value ? std::format("{} -> {}", stock, *e.value)
                           : nox::l10n::format(NOX_TR("content.changes.value_removed", "{stock} -> removed"), {{"stock", stock}});
        r.reach = content::reachLabel(content::PackEditor::reachOf(e));
        const content::FieldEdit copy = e;
        r.revert = [this, copy] {
            auto& p = ws_->pack;
            for (std::size_t k = 0; k < p.edits().size(); ++k) {
                if (p.edits()[k].sameTarget(copy)) {
                    p.revert(k);
                    break;
                }
            }
        };
        r.show = [this, copy] {
            if (const auto ref = recordOf(copy)) {
                ws_->select(*ref);
            }
        };
        out.push_back(std::move(r));
    }
    if (auto* bag = session_.bag()) {
        const auto& images = bag->bag().images();
        for (const auto& [index, _] : bag->bagEdits()) {
            const std::int32_t i = std::int32_t(index);
            ChangeRow r;
            r.what = std::format("video.bag #{}  {}", i, index < images.size() ? images[index].name : std::string());
            r.detail = NOX_TR("content.changes.image_replaced", "image replaced");
            r.reach = content::reachLabel(content::Reach::ClientVisible);
            r.revert = [this, i] {
                session_.revert({i});
                images_.refresh();
            };
            r.show = [this, i] {
                images_.show(i);
                focus(panel::kImages);
            };
            out.push_back(std::move(r));
        }
    }
    if (auto* hd = session_.pack()) {
        for (const auto& [index, edit] : hd->packEdits()) {
            const std::int32_t i = std::int32_t(index);
            ChangeRow r;
            r.what = nox::l10n::format(NOX_TR("content.changes.hd_pack_image", "HD pack #{index}  {name}"), {{"index", i}, {"name", hd->name(index)}});
            r.detail = edit.encoded ? NOX_TR("content.changes.hd_put_in", "HD image put in") : NOX_TR("content.changes.hd_removed", "HD image removed");
            r.reach = content::reachLabel(content::Reach::ClientVisible);
            r.revert = [this, i] {
                session_.revert({i});
                images_.refresh();
            };
            r.show = [this, i] {
                images_.show(i);
                focus(panel::kImages);
            };
            out.push_back(std::move(r));
        }
    }
}

void Shell::collectProblems(std::vector<ProblemRow>& out)
{
    if (!session_.loadError().empty()) {
        out.push_back({true,
                       nox::l10n::format(NOX_TR("content.problems.no_bag", "Can't open the game's video.bag: {error} (set the game folder at the top and press Open)"),
                                         {{"error", session_.loadError()}}),
                       {}});
    }
    for (const auto& s : ws_->files.status()) {
        if (!s.loaded && !s.optional) {
            out.push_back({true, std::format("{}: {}", utf8(s.path), s.error), {}});
        }
    }
    for (const std::string& e : ws_->pack.errors()) {
        out.push_back({true, e, {}});
    }
    for (const std::string& e : packProblems_) {
        out.push_back({true, e, {}});
    }
    // What thing.patch's loader would refuse (it would leave the whole file out).
    for (const std::string& p : ws_->pack.thingProblems()) {
        out.push_back({true, nox::l10n::format(NOX_TR("content.problems.thing_patch", "thing.patch: {problem} (NoX Core would leave the whole file out)"), {{"problem", p}}), {}});
    }
    // A pack that adds types: who can play with it.
    if (!ws_->pack.newThings().empty()) {
        out.push_back({false,
                       nox::l10n::format(NOX_TR("content.problems.new_types", "The pack adds {n} object types: a game hosted with it lets in only NoX Core games with the same pack "
                                                                              "(stock players can't join it), and a game with it joins only hosts with the same pack or none that adds types"),
                                         {{"n", ws_->pack.newThings().size()}}),
                       {}});
    }
    // The pack's values, checked against what the game reads.
    for (const content::ThingEdit& e : ws_->pack.thingEdits()) {
        const auto show = [this, e] {
            if (const auto ref = ws_->index.findByName(RecordList::Thing, e.type)) {
                ws_->select(*ref);
            }
        };
        const bool fresh = ws_->pack.isNewThing(e.type);
        if (ws_->pack.baseThings() && !ws_->pack.baseThings()->findThing(e.type) && !fresh) {
            out.push_back({true, nox::l10n::format(NOX_TR("content.problems.no_type", "thing.bin has no type {name} (a new one is added with New type like this...)"), {{"name", e.type}}), show});
            continue;
        }
        const content::FieldSpec* spec = content::thingField(e.key);
        if (!spec) {
            out.push_back({false, nox::l10n::format(NOX_TR("content.problems.key_unread", "{field}: NoX Core doesn't read this key"), {{"field", e.label()}}), show});
        } else if (spec->structural && !fresh) { // (a new type's warning is its own, below)
            out.push_back({false, nox::l10n::format(NOX_TR("content.problems.structural", "{field}: every player needs this pack: {docs}"), {{"field", e.label()}, {"docs", nox::l10n::tr(spec->docs)}}),
                           show});
        }
        if (spec && e.value) {
            if (const std::string why = content::checkValue(*spec, *e.value); !why.empty()) {
                out.push_back({false, std::format("{}: {}", e.label(), why), show});
            }
        }
    }
    for (const content::FieldEdit& e : ws_->pack.edits()) {
        const content::FieldSpec* spec = content::tableField(e.format, e.key, e.section);
        const auto show = [this, e] {
            if (const auto ref = recordOf(e)) {
                ws_->select(*ref);
            }
        };
        if (!spec) {
            out.push_back({e.format != content::TableFormat::GameData,
                           e.format == content::TableFormat::GameData
                               ? nox::l10n::format(NOX_TR("content.problems.name_unread", "{field}: nothing in the game reads this name (only a game-mode mod could)"), {{"field", e.label()}})
                           : e.format == content::TableFormat::SoundSet
                               ? nox::l10n::format(NOX_TR("content.problems.no_such_key_stops", "{field}: {file} has no such key (the game stops reading the file there)"),
                                                   {{"field", e.label()}, {"file", nox::content::tableFileName(e.format)}})
                               : nox::l10n::format(NOX_TR("content.problems.no_such_key", "{field}: {file} has no such key"), {{"field", e.label()}, {"file", nox::content::tableFileName(e.format)}}),
                           show});
            continue;
        }
        if (e.value) {
            if (const std::string why = content::checkValue(*spec, *e.value); !why.empty()) {
                out.push_back({false, std::format("{}: {}", e.label(), why), show});
            }
        }
    }
    if (session_.pack() && !session_.packMatchesBag()) {
        out.push_back({false, NOX_TR("content.problems.pack_other_idx", "The HD pack was made for another video.idx: NoX Core won't use it with this one until it's stamped for it (Save pack as...)."),
                       [this] { focus(panel::kImages); }});
    }
}

void Shell::drawPanels(double dt)
{
    Panels& p = *panels_;
    const auto begin = [](const char* name, bool* open) {
        ImGui::SetNextWindowSize(ImVec2(420, 360), ImGuiCond_FirstUseEver);
        return ImGui::Begin(name, open);
    };
    const auto beginPanel = [&](const char* name, bool* open) { return begin(panelTitle(name), open); };
    const auto focused = [] { return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows); };
    if (p.showContent) {
        if (beginPanel(panel::kContent, &p.showContent)) {
            p.browser.draw();
        }
        ImGui::End();
    }
    if (p.showRecord) {
        if (beginPanel(panel::kRecord, &p.showRecord)) {
            p.record.draw();
        }
        ImGui::End();
    }
    if (p.showImages) {
        if (beginPanel(panel::kImages, &p.showImages)) {
            images_.setKeys(focused());
            if (session_.bagOpen()) {
                images_.draw(ImGui::GetContentRegionAvail().y);
            } else {
                ImGui::TextDisabled("%s", NOX_TRC("content.shell.no_bag_open", "No video.bag is open."));
            }
        }
        ImGui::End();
    }
    if (p.showSprites) {
        if (beginPanel(panel::kSprites, &p.showSprites)) {
            catalog_.setKeys(focused());
            catalogToolbar();
            catalog_.draw(ImGui::GetContentRegionAvail().y, dt);
        }
        ImGui::End();
    }
    if (p.showPreview) {
        if (beginPanel(panel::kPreview, &p.showPreview)) {
            p.preview.draw(dt);
        }
        ImGui::End();
    }
    if (p.showReferences) {
        if (beginPanel(panel::kReferences, &p.showReferences)) {
            p.references.draw();
        }
        ImGui::End();
    }
    std::vector<ChangeRow> changes;
    collectChanges(changes);
    if (p.showChanges) {
        if (begin((nox::l10n::format(NOX_TR("content.panel.changes_count", "Changes ({n})"), {{"n", changes.size()}}) + "###" + panel::kChanges).c_str(), &p.showChanges)) {
            drawChanges(*ws_, changes);
        }
        ImGui::End();
    }
    std::vector<ProblemRow> problems;
    collectProblems(problems);
    if (p.showProblems) {
        if (begin((nox::l10n::format(NOX_TR("content.panel.problems_count", "Problems ({n})"), {{"n", problems.size()}}) + "###" + panel::kProblems).c_str(), &p.showProblems)) {
            drawProblems(*ws_, problems);
        }
        ImGui::End();
    }
    if (p.showLog) {
        if (beginPanel(panel::kLog, &p.showLog)) {
            drawLog(*ws_);
        }
        ImGui::End();
    }
}

void Shell::frame(double dt)
{
    session_.frame();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal) && (ws_->pack.changeCount() > 0 || ws_->pack.dirty())) {
        savePack();
    }
    ws_->syncNewThings();
    dockSpace();
    drawPanels(dt);
    packPopups();
    for (const auto& name : wantFocus_) {
        // (Changes and Problems carry counts in their titles: their windows are known by their ids.)
        const std::string id = name == panel::kChanges || name == panel::kProblems ? std::format("###{}", name) : std::string(panelTitle(name));
        ImGui::SetWindowFocus(id.c_str());
    }
    wantFocus_.clear();
}

} // namespace videotools::gui
