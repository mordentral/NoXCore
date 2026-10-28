// The map editor: documents, commands and ops (map_editor.hpp). The viewport is in
// map_editor_view.cpp, the panels and dialogs in map_editor_ui.cpp.
#include "editor/map_editor.hpp"
#include "editor/editor_text.hpp"
#include "editor/marker_tools.hpp"
#include "editor/mod_panel.hpp"
#include "editor/object_tools.hpp"
#include "editor/panel_textures.hpp"
#include "editor/play_view.hpp"
#include "editor/property_panel.hpp"
#include "editor/script_panel.hpp"

#include "sprite_cache.hpp"
#include "thing_art.hpp"

#include "noxcore/client/edit_session_client.hpp"
#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/map/map_file.hpp"
#include "noxcore/mapedit/writer.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/text.hpp"
#include "noxcore/video/nxz.hpp"

#include <imgui.h>
#include <imgui_internal.h> // (a test's float: step: the panel's window undocked and placed)
#if NOX_PRIVATE_TOOLS
#include "dev_hooks.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
namespace me = nox::mapedit;

namespace noxsdl {

namespace {

using ObjId = me::ObjId;
using editor::numbersAfter;
using editor::pathText;

/// A map file as a document: a .map (encrypted or not), or a .nxz (the map download's).
me::MapDocument readDocument(const fs::path& path, const nox::things::ThingDb& things)
{
    const std::string ext = nox::util::lowered(path.extension().string());
    if (ext != ".nxz") {
        return me::MapDocument::load(path, things);
    }
    const auto bytes = nox::readFileBytes(path);
    if (bytes.size() < 4) {
        throw nox::FormatError("not a .nxz map");
    }
    const std::uint32_t size = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
    auto plain = nox::video::nxzDecompress(std::span(bytes).subspan(4), size);
    nox::crypto::cipherForKey(nox::crypto::FileKey::Map).decrypt(plain);
    me::MapDocument d = me::MapDocument::fromMapFile(nox::map::parseMapFile(plain), things);
    d.origin.name = path.stem().string();
    d.origin.stock = me::isStockMapName(d.origin.name);
    return d;
}

/// A floor that reads as ground for a new map's default: the first grass, else the first floor.
std::uint8_t defaultFloor(const nox::things::ThingDb& things)
{
    for (std::size_t i = 0; i < things.floors.size(); ++i) {
        if (things.floors[i].name.find("Grass") != std::string::npos) {
            return static_cast<std::uint8_t>(i);
        }
    }
    return 0;
}

} // namespace

MapEditor::MapEditor(Context context)
    : ctx_(std::move(context))
    , things_(*ctx_.things)
{
    settingsPath_ = ctx_.prefDir / "settings.txt";
    if (!ctx_.screenshotRun) {
        settings_.load(settingsPath_); // (a test's run keeps to the defaults, and writes nothing)
    }
    // (A test's run keeps its panels inside the hidden window unless a "floating:on" step asks.)
    imgui_ = std::make_unique<editor::ImGuiHost>(ctx_.window, ctx_.renderer,
                                                 ctx_.screenshotRun ? std::string() : pathText(ctx_.prefDir / "layout.ini"),
                                                 settings_.uiScale, settings_.theme, settings_.floatingPanels && !ctx_.screenshotRun,
                                                 ctx_.screenshotRun);
    initTools();
    registerCommands();
    scriptPanel_ = std::make_unique<ScriptPanel>(*this);
    scriptPanel_->registerCommands();
    modPanel_ = std::make_unique<ModPanel>(*this);
    modPanel_->registerCommands();
    playView_ = std::make_unique<PlayView>(*this);
    playView_->registerCommands();
    objects_ = std::make_unique<ObjectTools>(*this);
    objects_->registerCommands();
    markers_ = std::make_unique<MarkerTools>(*this);
    markers_->registerCommands();
    props_ = std::make_unique<PropertyPanel>(*this);
    props_->registerCommands();
    commands_.loadBindings(settings_.bindings);
    newOptions_.floor = defaultFloor(things_);
    newOptions_.title = "Untitled";
    newDocument(newOptions_);
    SDL_ShowCursor(); // (the game's own pointer is the menus' and the game's)
    SDL_StartTextInput(ctx_.window);
    autosaveAt_ = SDL_GetTicks() + static_cast<std::uint64_t>(std::max(settings_.autosaveMinutes, 1)) * 60000;
}

MapEditor::~MapEditor()
{
    if (validating_.valid()) {
        validating_.wait();
    }
    if (session_) {
        session_->attach(nullptr); // (never our document left with the session's client side)
    }
    saveSettings();
    modPanel_.reset();
    scriptPanel_.reset(); // (ends a play-test it started)
    objects_.reset();
    view_.reset();
    for (auto& [key, t] : thumbs_) {
        if (t) {
            editor::panelTextureForget(t);
            SDL_DestroyTexture(t);
        }
    }
    if (mapTarget_) {
        SDL_DestroyTexture(mapTarget_);
    }
    imgui_.reset();
    SDL_SetWindowTitle(ctx_.window, "NoX Core");
}

void MapEditor::saveSettings()
{
    if (ctx_.screenshotRun) {
        return;
    }
    settings_.bindings = commands_.saveBindings();
    settings_.save(settingsPath_);
}

void MapEditor::setTheme(editor::Theme theme)
{
    if (settings_.theme == theme && imgui_->theme() == theme) {
        return;
    }
    settings_.theme = theme;
    styleDirty_ = true; // (the style made again before the next frame, and saved)
}

void MapEditor::status(std::string text)
{
    SDL_Log("map editor: %s", text.c_str());
    statusText_ = std::move(text);
    statusMs_ = SDL_GetTicks();
}

std::string MapEditor::title() const
{
    if (session_) {
        return "[EDIT] " + docName_ + " (session) - NoX Core Map Editor";
    }
    std::string name = !docName_.empty() ? docName_ : !doc_.origin.name.empty() ? doc_.origin.name + " (copy)" : "Untitled";
    return name + (history_.dirty() ? " *" : "") + " - NoX Core Map Editor";
}

std::string MapEditor::elementName(const me::ElementKey& key) const
{
    return key.text();
}

std::string MapEditor::peerName(me::PeerId peer) const
{
    if (session_) {
        return session_->nameOf(peer);
    }
    return peer == 0 ? std::string("the host") : "editor " + std::to_string(peer);
}

std::optional<me::PeerId> MapEditor::otherHolder(const me::ElementKey& key) const
{
    const auto h = locks_.holder(key);
    if (h && *h != self_) return h;
    return std::nullopt;
}

// --- Documents --------------------------------------------------------------------------------

void MapEditor::resetDocumentState()
{
    history_.clear();
    selection_.clear();
    selectionAt_.clear();
    locks_.releaseAll(self_);
    panelLocks_.clear();
    pick_.reset();
    issues_.clear();
    validatedRevision_ = ~0ull;
    viewBuiltFor_ = ~0ull;
    ++docGeneration_; // (its HD images asked for ahead at the next view: rebuildView)
    cameraSet_ = false;
    drag_ = Drag::None;
    preview_ = {}; // (a preview of the document before: gone with it)
    previewKey_.reset();
    previewSelectionBefore_.reset();
    changedMs_ = SDL_GetTicks();
}

void MapEditor::newDocument(const me::NewMapOptions& options)
{
    doc_ = me::newMap(things_, options);
    resetDocumentState();
    docPath_.clear();
    docName_.clear();
    sidecar_ = {};
    if (scriptPanel_) scriptPanel_->documentOpened();
    loadPaintSettings();
    status("Started a new map (" + (options.size > 0 ? std::to_string(options.size) + " x " + std::to_string(options.size) + " cells of floor" : std::string("no floor")) + ")");
}

bool MapEditor::open(const fs::path& path)
{
    try {
        me::MapDocument d = readDocument(path, things_);
        doc_ = std::move(d);
    } catch (const std::exception& ex) {
        errorText_ = "Can't open " + pathText(path) + ":\n" + ex.what();
        status("Open failed: " + std::string(ex.what()));
        return false;
    }
    resetDocumentState();
    docPath_ = path;
    // A stock map's edits always go under a new name (Save asks for one).
    docName_ = doc_.origin.stock ? std::string() : doc_.origin.name;
    sidecar_ = {};
    sidecar_.origin = doc_.origin.name;
    sidecar_.originStock = doc_.origin.stock;
    const fs::path side = fs::path(path).replace_extension(".nxe");
    std::error_code ec;
    if (fs::exists(side, ec)) {
        try {
            const auto bytes = nox::readFileBytes(side);
            sidecar_ = me::Sidecar::fromJson(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        } catch (const std::exception& ex) {
            status("Sidecar ignored: " + std::string(ex.what()));
        }
    }
    scriptPanel_->documentOpened();
    loadPaintSettings();
    settings_.addRecent(pathText(path));
    saveSettings();
    status("Opened " + pathText(path) + (doc_.origin.stock      ? " (a stock map: Save asks for a new name)"
                                         : saveNeedsName() ? " (its name doesn't fit the game's 8 characters: Save asks for a new one)"
                                                           : ""));
    return true;
}

bool MapEditor::save()
{
    if (session_) {
        sessionPublish(); // (a session's map is saved by its host: the owner's Save publishes)
        return false;
    }
    if (saveNeedsName()) {
        std::snprintf(nameBuf_, sizeof nameBuf_, "%s", saveAsOffer().c_str());
        saveAsOpen_ = true;
        return false;
    }
    return saveAs(docName_);
}

bool MapEditor::saveNeedsName() const
{
    return docName_.empty() || me::mapNameProblem(docName_).has_value();
}

std::string MapEditor::saveAsOffer() const
{
    if (!docName_.empty() && !me::mapNameProblem(docName_)) {
        return docName_;
    }
    // (A copy's name within the game's 8 characters, so its map lists show it.)
    const std::string& from = !docName_.empty() ? docName_ : doc_.origin.name;
    return from.empty() ? std::string() : me::suggestedMapName(from);
}

bool MapEditor::saveAs(const std::string& name)
{
    if (const auto problem = me::mapNameProblem(name)) {
        errorText_ = "Can't save as \"" + name + "\": " + *problem;
        return false;
    }
    try {
        scriptPanel_->beforeSave(sidecar_); // (the source compiled into the map, kept in the sidecar)
        storePaintSettings();
        me::Sidecar side = sidecar_;
        side.revision += 1;
        if (side.origin.empty()) {
            side.origin = doc_.origin.name;
            side.originStock = doc_.origin.stock;
        }
        const auto paths = me::saveDocument(doc_, things_, ctx_.mapsDir, name, &side);
        sidecar_ = side;
        docName_ = name;
        docPath_ = paths.map;
        history_.markSaved();
        settings_.addRecent(pathText(paths.map));
        saveSettings();
        status("Saved " + pathText(paths.map));
    } catch (const std::exception& ex) {
        errorText_ = std::string("Not saved:\n") + ex.what();
        status("Save failed");
        return false;
    }
    if (afterSave_) {
        auto then = std::move(afterSave_);
        afterSave_ = nullptr;
        then();
    }
    return true;
}

void MapEditor::autosave()
{
    const std::uint64_t now = SDL_GetTicks();
    if (settings_.autosaveMinutes <= 0 || now < autosaveAt_ || ctx_.screenshotRun || strokeActive() || wallPreviewUndo_) {
        return; // (a stroke or a drag's live preview in the document: saved once it's done)
    }
    autosaveAt_ = now + static_cast<std::uint64_t>(settings_.autosaveMinutes) * 60000;
    if (!history_.dirty()) {
        return;
    }
    // The recovery copy: the map as it stands, in the user's folder (never over a real map).
    try {
        const auto files = me::encodeFiles(doc_, things_);
        const fs::path dir = ctx_.prefDir / "autosave";
        fs::create_directories(dir);
        std::string name = !docName_.empty() ? docName_ : !doc_.origin.name.empty() ? doc_.origin.name : "untitled";
        if (!nox::isPlainFileName(name)) {
            name = "untitled"; // (a session host's map name isn't ours to make a path of: "..\x")
        }
        std::ofstream(dir / (name + ".map"), std::ios::binary | std::ios::trunc)
            .write(reinterpret_cast<const char*>(files.map.data()), static_cast<std::streamsize>(files.map.size()));
        status("Autosaved a recovery copy");
    } catch (const std::exception& ex) {
        status(std::string("Autosave failed: ") + ex.what());
    }
}

void MapEditor::guarded(Pending action, std::function<void()> then)
{
    if ((action == Pending::Close || action == Pending::Quit) && modPanel_->dirty()) {
        // A mod's unsaved files or form asked about first (as Close Mod asks), then the rest.
        modPanel_->askThen([this, action, then = std::move(then)]() mutable { guarded(action, std::move(then)); });
        return;
    }
    if (session_ && action != Pending::Quit) {
        if (action == Pending::Close) {
            // (close_plan.hpp) Our own session: asked once, as it ends the game for everyone, then
            // main.cpp ends it and goes to the main menu. Someone else's: the editor closes and
            // the session's game is in front (F2 brings the editor back). Never the game put into
            // the Map panel instead of closing.
            if (sessionHooks_.hosting) {
                if (!endSessionAsked_) {
                    endSessionOpen_ = true;
                    endSessionQuits_ = false;
                }
            } else {
                wantsClose_ = true;
            }
        } else {
            status("In a session the map is the session's (leave the game to open another)");
        }
        return;
    }
    if (session_ || !history_.dirty()) {
        then();
        return;
    }
    pending_ = action;
    pendingThen_ = std::move(then);
    unsavedOpen_ = true;
}

bool MapEditor::requestQuit()
{
    if (quitConfirmed_ || ctx_.screenshotRun) {
        return true;
    }
    if (modPanel_->dirty()) {
        // A mod's unsaved files or form first (Save, Don't Save, Cancel), then the quit's own asks.
        modPanel_->askThen([this] {
            if (requestQuit()) {
                quitConfirmed_ = true;
            }
        });
        return false;
    }
    if (session_) {
        // (A session's work is the host's: nothing unsaved here.) Hosting it, the program's end
        // is the session's for everyone: asked first, as Close Editor asks.
        if (!sessionHooks_.hosting) {
            return true;
        }
        if (!endSessionAsked_) {
            endSessionOpen_ = true;
        }
        endSessionQuits_ = true;
        return false;
    }
    if (!history_.dirty()) {
        return true;
    }
    guarded(Pending::Quit, [this] { quitConfirmed_ = true; });
    return false;
}

// --- Ops --------------------------------------------------------------------------------------

bool MapEditor::perform(const me::Op& op)
{
    if (op.empty()) {
        return false;
    }
    // The host's rule: an op touching what another editor holds is refused.
    if (const auto c = locks_.conflict(self_, me::elementsTouched(doc_, op))) {
        status(op.label + " refused: " + elementName(c->first) + " is being edited by " + peerName(c->second));
        return false;
    }
    const std::size_t before = history_.dropped() + history_.position();
    if (session_) {
        // A session: applied as a preview and sent to the host, which commits it in its order or
        // refuses it (then it's rolled back: sessionFrame).
        me::Op inverse;
        std::string why;
        const auto seq = session_->submit(op, &inverse, &why);
        if (!seq) {
            status(op.label + " refused: " + why);
            return false;
        }
        history_.recordApplied(op, std::move(inverse));
        sessionSeqs_[*seq] = before;
        sessionRevision_ = session_->revision();
    } else {
        try {
            history_.perform(doc_, op);
        } catch (const std::exception& ex) {
            status(op.label + " failed: " + ex.what());
            return false;
        }
    }
    viewAfterOp(op);
    selectionAt_.erase(selectionAt_.upper_bound(before), selectionAt_.end()); // (the undone tail is gone)
    selectionAt_[before] = selection_;
    locks_.touch(self_, SDL_GetTicks());
    selection_.prune(doc_);
    changedMs_ = SDL_GetTicks();
    status(op.label);
    return true;
}

void MapEditor::undo()
{
    scriptPanel_->commit(); // (the script's typing first)
    const me::Op* op = history_.nextUndo();
    if (!op) {
        return;
    }
    if (!session_) {
        if (const auto c = locks_.conflict(self_, me::elementsTouched(doc_, *op))) {
            status("Can't undo: " + elementName(c->first) + " is being edited by " + peerName(c->second));
            return;
        }
    }
    const std::size_t at = history_.dropped() + history_.position();
    selectionAt_[at] = selection_;
    const me::Op applied = *op;
    if (session_) {
        // A session's undo is selective: the inverse sent as a new op, cut to what still holds our
        // op's values; what others changed since, or built on, is left as it is.
        const me::Op& done = history_.entries()[history_.position() - 1].op;
        const auto r = session_->submitSelective(applied, done);
        if (!r.seq && !r.nothingLeft) {
            status("Can't undo: " + r.why); // (the entry stays: e.g. a lock let go later)
            return;
        }
        history_.stepBack();
        sessionRevision_ = session_->revision();
        if (r.nothingLeft) {
            status("Nothing left to undo in " + applied.label + ": " + r.note);
        } else {
            viewAfterOp(applied);
            status("Undid " + applied.label + (r.note.empty() ? std::string() : " (" + r.note + ")"));
        }
    } else try {
        status("Undid " + history_.undo(doc_));
        viewAfterOp(applied);
    } catch (const std::exception& ex) {
        status(std::string("Undo failed: ") + ex.what());
    }
    if (const auto it = selectionAt_.find(history_.dropped() + history_.position()); it != selectionAt_.end()) {
        setSelection(it->second, true); // (what was selected before the change)
    } else {
        selection_.prune(doc_);
    }
    changedMs_ = SDL_GetTicks();
}

void MapEditor::redo()
{
    scriptPanel_->commit();
    const me::Op* op = history_.nextRedo();
    if (!op) {
        return;
    }
    if (!session_) {
        if (const auto c = locks_.conflict(self_, me::elementsTouched(doc_, *op))) {
            status("Can't redo: " + elementName(c->first) + " is being edited by " + peerName(c->second));
            return;
        }
    }
    selectionAt_[history_.dropped() + history_.position()] = selection_;
    const me::Op applied = *op;
    if (session_) {
        // Redo is the undo's inverse, as selective: cut to what still holds the undo's values.
        const me::Op& undone = history_.entries()[history_.position()].inverse;
        const auto r = session_->submitSelective(applied, undone);
        if (!r.seq && !r.nothingLeft) {
            status("Can't redo: " + r.why);
            return;
        }
        history_.stepForward();
        sessionRevision_ = session_->revision();
        if (r.nothingLeft) {
            status("Nothing left to redo in " + applied.label + ": " + r.note);
        } else {
            viewAfterOp(applied);
            status("Redid " + applied.label + (r.note.empty() ? std::string() : " (" + r.note + ")"));
        }
    } else try {
        status("Redid " + history_.redo(doc_));
        viewAfterOp(applied);
    } catch (const std::exception& ex) {
        status(std::string("Redo failed: ") + ex.what());
    }
    if (const auto it = selectionAt_.find(history_.dropped() + history_.position()); it != selectionAt_.end()) {
        setSelection(it->second, true); // (what was selected after the change)
    } else {
        selection_.prune(doc_);
    }
    changedMs_ = SDL_GetTicks();
}

void MapEditor::jumpHistory(std::size_t position)
{
    while (history_.position() > position && history_.canUndo()) {
        const std::size_t before = history_.position();
        undo();
        if (history_.position() == before) {
            return; // (refused)
        }
    }
    while (history_.position() < position && history_.canRedo()) {
        const std::size_t before = history_.position();
        redo();
        if (history_.position() == before) {
            return;
        }
    }
}

void MapEditor::setSelection(me::Selection s, bool quiet)
{
    s.prune(doc_);
    std::size_t skipped = 0;
    s = me::withoutLocked(doc_, s, locks_, self_, &skipped);
    // Selection is a lock request: what isn't granted isn't selected. (What open panels hold is
    // asked for with it.)
    std::set<me::ElementKey> wanted = me::elementsOf(doc_, s);
    wanted.insert(panelLocks_.begin(), panelLocks_.end());
    const auto refused = locks_.setHeld(self_, wanted, SDL_GetTicks());
    if (!refused.empty()) {
        me::Selection held = s;
        for (const ObjId id : s.objects) {
            if (refused.contains(me::ElementKey::object(id))) held.objects.erase(id);
        }
        for (const auto c : s.walls) {
            if (refused.contains(me::ElementKey::wall(c))) held.walls.erase(c);
        }
        for (const auto t : s.tiles) {
            if (refused.contains(me::ElementKey::tile(t))) held.tiles.erase(t);
        }
        for (const auto w : s.waypoints) {
            if (refused.contains(me::ElementKey::waypoint(w))) held.waypoints.erase(w);
        }
        for (const auto p : s.polygons) {
            if (refused.contains(me::ElementKey::polygon(p))) held.polygons.erase(p);
        }
        for (const auto& k : refused) panelLocks_.erase(k);
        skipped += refused.size();
        s = std::move(held);
    }
    selection_ = std::move(s);
    if (session_) {
        sendLocks(); // (the host answers with what it grants: sessionFrame)
    }
    if (skipped > 0 && !quiet) {
        status(std::to_string(skipped) + (skipped == 1 ? " item" : " items") + " skipped: being edited by others");
    }
}

bool MapEditor::holdPanel(const me::ElementKey& key, bool on)
{
    if (!on) {
        if (panelLocks_.erase(key)) {
            locks_.release(self_, {key});
            if (session_) sendLocks();
        }
        return true;
    }
    if (panelLocks_.contains(key)) return true;
    if (const auto h = otherHolder(key)) {
        status(elementName(key) + " is being edited by " + peerName(*h));
        return false;
    }
    if (!locks_.request(self_, {key}, SDL_GetTicks()).empty()) return false;
    panelLocks_.insert(key);
    if (session_) sendLocks();
    return true;
}

void MapEditor::takeOverPanel(const me::ElementKey& key)
{
    if (!session_ || !session_->isOwner()) {
        holdPanel(key, true);
        return;
    }
    const auto h = otherHolder(key);
    const std::string from = h ? " from " + peerName(*h) : std::string();
    session_->takeOver(key);
    locks_ = session_->locks(); // (ours already here; the host's table follows)
    panelLocks_.insert(key);
    status("Took over " + elementName(key) + from);
}

void MapEditor::beginPick(PickRequest request)
{
    pick_ = std::move(request);
    status(pick_->prompt + " (right click or Escape cancels)");
}

void MapEditor::cancelPick()
{
    if (pick_) {
        pick_.reset();
        status("Pick cancelled");
    }
}

void MapEditor::copySelection(bool cut)
{
    if (selection_.empty()) {
        status("Nothing selected to copy");
        return;
    }
    clip_ = me::copySelection(doc_, selection_);
    SDL_SetClipboardText(me::clipToText(*clip_, things_).c_str());
    const std::size_t n = clip_->walls.size() + clip_->tiles.size() + clip_->objects.size() + clip_->waypoints.size();
    status((cut ? "Cut " : "Copied ") + std::to_string(n) + (n == 1 ? " item" : " items"));
    if (cut) {
        deleteSelection();
    }
}

void MapEditor::paste(std::optional<me::Cell> at)
{
    std::optional<me::Clip> clip;
    if (char* text = SDL_GetClipboardText()) {
        clip = me::clipFromText(text, things_);
        SDL_free(text);
    }
    if (!clip) {
        clip = clip_;
    }
    if (!clip || clip->empty()) {
        status("Nothing to paste");
        return;
    }
    if (!at) {
        const auto m = mouseWorld();
        const float x = m ? m->x : camera_.x;
        const float y = m ? m->y : camera_.y;
        at = me::wallCellAt(std::clamp(x, 0.0f, me::kWorldSize - 1), std::clamp(y, 0.0f, me::kWorldSize - 1));
    }
    me::Selection pasted;
    if (perform(me::pasteOp(doc_, *clip, at.value_or(me::Cell{128, 128}), &pasted))) {
        setSelection(std::move(pasted), true);
    }
}

void MapEditor::duplicate()
{
    if (selection_.empty()) {
        status("Nothing selected to duplicate");
        return;
    }
    const me::Clip clip = me::copySelection(doc_, selection_);
    const me::Cell at{static_cast<std::uint8_t>(std::min(clip.anchor.x + 2, 254)), static_cast<std::uint8_t>(std::min(clip.anchor.y + 2, 255))};
    me::Selection pasted;
    me::Op op = me::pasteOp(doc_, clip, at, &pasted);
    op.label = "Duplicate " + std::to_string(pasted.size()) + (pasted.size() == 1 ? " item" : " items");
    if (perform(op)) {
        setSelection(std::move(pasted), true);
    }
}

void MapEditor::deleteSelection()
{
    if (selection_.empty()) {
        return;
    }
    // Walls go with their neighbours re-faced to what they still join (walls.hpp).
    me::Selection rest = selection_;
    rest.walls.clear();
    me::Op op = me::deleteOp(doc_, rest);
    if (!selection_.walls.empty()) {
        const me::Op walls = withWallEdges(me::removeWallsOp(doc_, things_, std::vector<me::Cell>(selection_.walls.begin(), selection_.walls.end()), wallReface_));
        op.parts.insert(op.parts.end(), walls.parts.begin(), walls.parts.end());
        op.label = rest.empty() ? walls.label : "Delete " + std::to_string(selection_.size()) + " items";
    }
    if (perform(op)) {
        setSelection({}, true);
    }
}

void MapEditor::nudge(int dx, int dy, bool cell)
{
    if (selection_.empty()) {
        // Nothing to move: the arrows pan (64 screen pixels, a cell's worth more with Shift).
        const float step = (cell ? 256.0f : 64.0f) / camera_.zoom;
        camera_.x = std::clamp(camera_.x + static_cast<float>(dx) * step, 0.0f, me::kWorldSize);
        camera_.y = std::clamp(camera_.y + static_cast<float>(dy) * step, 0.0f, me::kWorldSize);
        return;
    }
    const float unit = cell ? me::kWallCellSize : 1.0f;
    me::Selection moved;
    if (perform(me::moveOp(doc_, selection_, static_cast<float>(dx) * unit, static_cast<float>(dy) * unit, &moved))) {
        setSelection(std::move(moved), true);
    }
}

void MapEditor::frameWorld(float x0, float y0, float x1, float y1)
{
    camera_.x = (x0 + x1) / 2;
    camera_.y = (y0 + y1) / 2;
    const float w = std::max(x1 - x0, 46.0f) * 1.3f;
    const float h = std::max(y1 - y0, 46.0f) * 1.3f;
    if (panelW_ > 0 && panelH_ > 0) {
        camera_.zoom = std::clamp(std::min(panelW_ / w, panelH_ / h), 1.0f / 8, 4.0f);
    }
    flash_ = Flash{{camera_.x, camera_.y}, SDL_GetTicks()};
}

void MapEditor::frameSelection()
{
    if (const auto b = me::bounds(doc_, selection_)) {
        frameWorld(b->first.x, b->first.y, b->second.x, b->second.y);
    } else if (view_) {
        camera_.x = view_->centre().x;
        camera_.y = view_->centre().y;
    }
}

void MapEditor::locate(const me::Issue& issue)
{
    // The validator names where: "object 431 (Door)", "wall 12,40", "tile 3,5", "WayPoints #12".
    const std::string& w = issue.where;
    me::Selection s;
    if (const auto n = numbersAfter(w, "object ", 1)) {
        const auto a = static_cast<std::uint32_t>((*n)[0]);
        for (const auto& [id, o] : doc_.objects) {
            if (o.header.extent == a) {
                ObjId root = id;
                while (doc_.objects.contains(root) && doc_.objects.at(root).parent != 0) {
                    root = doc_.objects.at(root).parent;
                }
                s.objects.insert(root);
                break;
            }
        }
    } else if (const auto wn = numbersAfter(w, "wall ", 2)) {
        s.walls.insert({static_cast<std::uint8_t>((*wn)[0]), static_cast<std::uint8_t>((*wn)[1])});
    } else if (const auto tn = numbersAfter(w, "tile ", 2)) {
        s.tiles.insert({static_cast<std::int16_t>((*tn)[0]), static_cast<std::int16_t>((*tn)[1])});
    } else if (const auto pn = numbersAfter(w, "WayPoints #", 1)) {
        s.waypoints.insert(static_cast<std::uint32_t>((*pn)[0]));
    } else if (const auto en = numbersAfter(w, "extent ", 1)) {
        // (Objects sharing an extent: all of them.)
        for (const auto& [id, o] : doc_.objects) {
            if (o.parent == 0 && o.header.extent == static_cast<std::uint32_t>((*en)[0])) s.objects.insert(id);
        }
    }
    if (const auto bb = me::bounds(doc_, s)) {
        frameWorld(bb->first.x, bb->first.y, bb->second.x, bb->second.y);
        setSelection(std::move(s));
    } else {
        status(issue.where + ": nothing on the map to show");
    }
}

// --- Commands ---------------------------------------------------------------------------------

void MapEditor::registerCommands()
{
    using editor::Command;
    using editor::Shortcut;
    const auto key = [](const char* text) { return Shortcut::parse(text).value_or(Shortcut{}); };
    const auto add = [&](const char* id, const char* label, const char* category, const char* shortcut, const char* help,
                         std::function<void()> run, std::function<bool()> enabled = {}, std::function<bool()> checked = {}) {
        commands_.add(Command{id, label, category, key(shortcut), help, std::move(run), std::move(enabled), std::move(checked)});
    };
    const auto hasSelection = [this] { return !selection_.empty(); };

    // File
    add("file.new", "New Map...", "File", "Ctrl+N", "A new map: its floored size, base floor and game types.",
        [this] { guarded(Pending::New, [this] { newOpen_ = true; }); });
    add("file.open", "Open...", "File", "Ctrl+O", "Open a map from the maps folder, or any .map / .nxz file.",
        [this] { guarded(Pending::Open, [this] { refreshMapList(); openOpen_ = true; }); });
    add("file.save", "Save", "File", "Ctrl+S", "Save the map (.map, .nxz and the editor's .nxe) into the maps folder.", [this] { save(); });
    add("file.saveAs", "Save As...", "File", "Ctrl+Shift+S", "Save under a new name. Stock maps' names are refused.", [this] {
        std::snprintf(nameBuf_, sizeof nameBuf_, "%s", saveAsOffer().c_str());
        saveAsOpen_ = true;
    });
    add("file.validate", "Validate", "File", "F7", "Check the map against what the game accepts (the Problems panel).", [this] {
        validatedRevision_ = ~0ull;
        changedMs_ = 0;
        showProblems_ = true;
    });
    add("file.recover", "Open Autosave...", "File", "", "Open the last recovery copy the autosave wrote (saved under a new name).",
        [this] {
            guarded(Pending::Open, [this] {
                const fs::path dir = ctx_.prefDir / "autosave";
                std::error_code ec;
                fs::path newest;
                fs::file_time_type when{};
                for (const auto& e : fs::directory_iterator(dir, ec)) {
                    if (e.path().extension() == ".map" && (newest.empty() || e.last_write_time() > when)) {
                        newest = e.path();
                        when = e.last_write_time();
                    }
                }
                if (newest.empty()) {
                    status("No autosave to recover");
                } else if (open(newest)) {
                    docName_.clear();
                    docPath_.clear();
                    status("Recovered " + pathText(newest) + ": Save asks for a name");
                }
            });
        });
    add("file.close", "Close Editor", "File", "Ctrl+W",
        "Back to the main menu (asks about unsaved changes). Hosting a session, it ends the session for everyone (asked "
        "first); in someone else's, its game comes in front.",
        [this] { guarded(Pending::Close, [this] { wantsClose_ = true; }); });

    // Edit
    add("edit.undo", "Undo", "Edit", "Ctrl+Z", "Undo the last change.", [this] { undo(); }, [this] { return history_.canUndo(); });
    add("edit.redo", "Redo", "Edit", "Ctrl+Y", "Redo what was undone.", [this] { redo(); }, [this] { return history_.canRedo(); });
    add("edit.cut", "Cut", "Edit", "Ctrl+X", "Copy the selection to the clipboard and delete it.", [this] { copySelection(true); }, hasSelection);
    add("edit.copy", "Copy", "Edit", "Ctrl+C", "Copy the selection (walls, tiles, objects with their items, waypoints) to the clipboard.",
        [this] { copySelection(false); }, hasSelection);
    add("edit.paste", "Paste", "Edit", "Ctrl+V", "Paste at the mouse: new extents and waypoint ids, links inside the copy kept.",
        [this] { paste(std::nullopt); });
    add("edit.duplicate", "Duplicate", "Edit", "Ctrl+D", "Copy the selection two cells down and select the copy.", [this] { duplicate(); },
        hasSelection);
    add("edit.delete", "Delete", "Edit", "Delete", "Delete the selection (undoable).", [this] { deleteSelection(); }, hasSelection);
    add("edit.areaCopy", "Copy Area", "Edit", "Ctrl+Shift+C",
        "Drag a box on the map: everything in it, of the layers ticked in Tool Options, goes to the clipboard.", [this] {
            tool_ = Tool::Select;
            areaCopyArmed_ = true;
            status("Copy area: drag a box on the map (Escape cancels)");
        });
    add("edit.copyCoords", "Copy Coordinates", "Edit", "", "The mouse's (or the selection's) world coordinates as text.", [this] {
        me::WorldPoint p{camera_.x, camera_.y};
        if (const auto b = me::bounds(doc_, selection_)) {
            p = {(b->first.x + b->second.x) / 2, (b->first.y + b->second.y) / 2};
        }
        char text[64];
        std::snprintf(text, sizeof text, "%.1f, %.1f", p.x, p.y);
        SDL_SetClipboardText(text);
        status(std::string("Copied ") + text);
    });
    add("edit.copyExtents", "Copy Extents", "Edit", "", "The selected objects' extents as text.", [this] {
        std::string text;
        for (const ObjId id : selection_.objects) {
            text += (text.empty() ? "" : " ") + std::to_string(doc_.objects.at(id).header.extent);
        }
        SDL_SetClipboardText(text.c_str());
        status("Copied extents: " + text);
    }, [this] { return !selection_.objects.empty(); });
    add("edit.copyWaypoints", "Copy Waypoint Ids", "Edit", "", "The selected waypoints' ids as text.", [this] {
        std::string text;
        for (const auto id : selection_.waypoints) {
            text += (text.empty() ? "" : " ") + std::to_string(id);
        }
        SDL_SetClipboardText(text.c_str());
        status("Copied waypoint ids: " + text);
    }, [this] { return !selection_.waypoints.empty(); });
    const auto nudgeCmd = [&](const char* id, const char* label, const char* k, int dx, int dy, bool cell) {
        add(id, label, "Edit", k, "Move the selection (or pan the view when nothing is selected).", [this, dx, dy, cell] { nudge(dx, dy, cell); });
    };
    nudgeCmd("edit.nudgeLeft", "Nudge Left", "Left", -1, 0, false);
    nudgeCmd("edit.nudgeRight", "Nudge Right", "Right", 1, 0, false);
    nudgeCmd("edit.nudgeUp", "Nudge Up", "Up", 0, -1, false);
    nudgeCmd("edit.nudgeDown", "Nudge Down", "Down", 0, 1, false);
    nudgeCmd("edit.nudgeCellLeft", "Nudge a Cell Left", "Shift+Left", -1, 0, true);
    nudgeCmd("edit.nudgeCellRight", "Nudge a Cell Right", "Shift+Right", 1, 0, true);
    nudgeCmd("edit.nudgeCellUp", "Nudge a Cell Up", "Shift+Up", 0, -1, true);
    nudgeCmd("edit.nudgeCellDown", "Nudge a Cell Down", "Shift+Down", 0, 1, true);

    // Select
    add("select.all", "Select All", "Select", "Ctrl+A", "Everything of the kinds the selection filter allows.",
        [this] { setSelection(me::selectAll(doc_, pickFilter())); });
    add("select.none", "Deselect / Cancel", "Select", "Escape", "Cancel a drag or the area copy, else clear the selection.", [this] {
        if (drag_ != Drag::None) {
            drag_ = Drag::None;
        } else if (areaCopyArmed_) {
            areaCopyArmed_ = false;
            status("Copy area cancelled");
        } else {
            setSelection({}, true);
        }
    });
    add("select.invert", "Invert Selection", "Select", "Ctrl+I", "Select what isn't selected (of the kinds the filter allows).", [this] {
        me::Selection all = me::selectAll(doc_, pickFilter());
        all.remove(selection_);
        setSelection(std::move(all));
    });
    add("select.sameType", "Select Same Type", "Select", "Shift+T", "Every object of the selected objects' types.", [this] {
        me::Selection s;
        for (const ObjId id : selection_.objects) {
            s.add(me::selectType(doc_, doc_.objects.at(id).type));
        }
        setSelection(std::move(s));
    }, [this] { return !selection_.objects.empty(); });
    const auto shapeCmd = [&](const char* id, const char* label, const char* k, int shape, const char* help) {
        add(id, label, "Select", k, help, [this, shape] { settings_.regionShape = shape; }, {},
            [this, shape] { return settings_.regionShape == shape; });
    };
    shapeCmd("select.shapeBox", "Box Select", "", 0, "Dragging on empty map selects a screen-aligned box.");
    shapeCmd("select.shape45", "45-Degree Box Select", "Ctrl+Shift+D", 1, "Dragging selects a box along the floor's diagonals.");
    shapeCmd("select.shapeLasso", "Lasso Select", "", 2, "Dragging draws the outline to select inside.");
    const auto filterCmd = [&](const char* id, const char* label, bool me::KindFilter::*field) {
        add(id, label, "Select", "", "Whether clicks and boxes take this kind.", [this, field] { settings_.pick.*field = !(settings_.pick.*field); },
            {}, [this, field] { return settings_.pick.*field; });
    };
    filterCmd("select.filterObjects", "Pick Objects", &me::KindFilter::objects);
    filterCmd("select.filterWalls", "Pick Walls", &me::KindFilter::walls);
    filterCmd("select.filterTiles", "Pick Floor Tiles", &me::KindFilter::tiles);
    filterCmd("select.filterWaypoints", "Pick Waypoints", &me::KindFilter::waypoints);

    // View
    add("view.frame", "Frame Selection", "View", "F", "Centre and zoom on the selection.", [this] { frameSelection(); });
    add("view.zoomIn", "Zoom In", "View", "=", "", [this] { camera_.zoom = std::min(camera_.zoom * 1.25f, 16.0f); });
    add("view.zoomOut", "Zoom Out", "View", "-", "", [this] { camera_.zoom = std::max(camera_.zoom / 1.25f, 1.0f / 8); });
    add("view.zoomReset", "Actual Size", "View", "0", "One screen pixel a world unit, as the game draws.", [this] { camera_.zoom = 1; });
    add("view.grid", "Grid: Off / Wall Cells / Floor Tiles", "View", "Ctrl+K", "Cycles the grid.", [this] {
        settings_.layers.grid = (settings_.layers.grid + 1) % 3;
    }, {}, [this] { return settings_.layers.grid != 0; });
    const auto layerCmd = [&](const char* id, const char* label, const char* k, bool editor::Layers::*field, bool rebuild) {
        add(id, label, "View", k, "Shows or hides this layer.", [this, field, rebuild] {
            settings_.layers.*field = !(settings_.layers.*field);
            if (rebuild) ++layerRevision_;
        }, {}, [this, field] { return settings_.layers.*field; });
    };
    layerCmd("view.floor", "Floor", "", &editor::Layers::floor, true);
    layerCmd("view.edges", "Floor Edges", "", &editor::Layers::edges, true);
    layerCmd("view.walls", "Walls", "F6", &editor::Layers::walls, true);
    layerCmd("view.objects", "Objects", "", &editor::Layers::objects, true);
    layerCmd("view.waypoints", "Waypoints", "", &editor::Layers::waypoints, false);
    layerCmd("view.polygons", "Polygons", "", &editor::Layers::polygons, false);
    layerCmd("view.lighting", "Lighting Preview", "L", &editor::Layers::lighting, false);
    layerCmd("view.seeThroughWalls", "See-Through Walls Near the View Centre", "", &editor::Layers::seeThroughWalls, false);
    layerCmd("view.labelTypes", "Labels: Type Names", "", &editor::Layers::labelTypes, false);
    layerCmd("view.labelScriptNames", "Labels: Script Names", "", &editor::Layers::labelScriptNames, false);
    layerCmd("view.labelExtents", "Labels: Extents", "", &editor::Layers::labelExtents, false);
    layerCmd("view.facing", "Facing Arrows", "", &editor::Layers::facing, false);
    layerCmd("view.markDisabled", "Mark Disabled Objects", "", &editor::Layers::markDisabled, false);
    layerCmd("view.schematic", "Schematic Overlay", "Ctrl+F", &editor::Layers::schematic, false);
    add("view.extents", "Extents: Hidden / Colliding / All", "View", "F8", "Cycles the collision shapes shown.",
        [this] { settings_.layers.extents = (settings_.layers.extents + 1) % 3; }, {}, [this] { return settings_.layers.extents != 0; });
    add("view.palette", "Command Palette", "View", "Ctrl+P", "Search every command by name.", [this] {
        paletteOpen_ = true;
        paletteFocus_ = true;
        paletteQuery_.clear();
        paletteIndex_ = 0;
    });
    add("help.palette", "Command Palette (F1)", "Help", "F1", "Search every command by name.", [this] { commands_.run("view.palette"); });

    // Tools (the old editor's number keys for the tool groups)
    const auto toolCmd = [&](const char* id, const char* label, const char* k, Tool t, const char* help) {
        add(id, label, "Tools", k, help, [this, t] {
            if (tool_ != t) {
                lastTool_ = tool_;
                tool_ = t;
            }
        }, {}, [this, t] { return tool_ == t; });
    };
    toolCmd("tool.select", "Select", "1", Tool::Select, "Click, Shift-click to add, Ctrl-click to remove; drag to box-select or move.");
    toolCmd("tool.wall", "Wall Tool", "2", Tool::Wall, "Draw, place, line, rectangle, outline and erase walls; wall properties.");
    toolCmd("tool.floor", "Floor Tool", "3", Tool::Floor, "The floor brush: paint, erase, replace, fill, smooth, with auto-edges.");
    toolCmd("tool.object", "Object Tool", "4", Tool::Object, "Objects from the content browser (P2c).");
    toolCmd("tool.waypoint", "Waypoint Tool", "5", Tool::Waypoint, "Place waypoints, link them (one way or both), unlink, move.");
    toolCmd("tool.edge", "Edge Tool", "6", Tool::Edge, "Floor edges by hand: add, remove, or auto-edge where clicked.");
    toolCmd("tool.polygon", "Polygon Tool", "7", Tool::Polygon, "Draw polygons, drag, insert and delete their corners, move them.");
    add("tool.toggle", "Previous Tool", "Tools", "`", "Back to the tool used before.", [this] { std::swap(tool_, lastTool_); });
    add("tool.eyedropper", "Eyedropper", "Tools", "I",
        "Pick what's under the mouse into the tool: a wall's material, a tile's floor, an edge (also Alt+click; Shift for everything).", [this] {
            if (const auto w = mouseWorld()) eyedropper(*w, ImGui::GetIO().KeyShift);
        });
    add("brush.bigger", "Bigger Brush", "Tools", "]", "The floor brush one tile bigger.", [this] { brush_.size = std::min(brush_.size + 1, 16); });
    add("brush.smaller", "Smaller Brush", "Tools", "[", "The floor brush one tile smaller.", [this] { brush_.size = std::max(brush_.size - 1, 1); });
    add("edges.recompute", "Recompute Edges", "Tools", "Ctrl+E",
        "Auto-edge again on the selected tiles (or every tile when none are), with the floor tool's edge options.", [this] {
            std::set<me::TileCell> cells = selection_.tiles;
            if (cells.empty()) {
                for (int gy = -1; gy <= 254; ++gy) {
                    for (int gx = (gy & 1); gx <= 255; gx += 2) {
                        if (doc_.floor.tile(gx, gy)) cells.insert({static_cast<std::int16_t>(gx), static_cast<std::int16_t>(gy)});
                    }
                }
            }
            me::Op op = me::autoEdgeOp(doc_, things_, rules_, edgeOptions(), cells, paint_.customEdges);
            if (op.empty()) status("Edges: nothing to change");
            else perform(op);
        });
    add("window.edgeRules", "Edge Rules...", "Window", "", "Floor priorities and the edge each pair laps with.", [this] { showEdgeRules_ = true; });
    add("view.specialWalls", "Tint Special Walls", "View", "F11", "Secret walls blue, breakable red, windows cyan.",
        [this] { settings_.layers.specialWalls = !settings_.layers.specialWalls; }, {}, [this] { return settings_.layers.specialWalls; });
    // A session's edits not yet published.
    const auto pendingCmd = [&](const char* id, const char* label, int mode, const char* help) {
        add(id, label, "View", "", help, [this, mode] { settings_.layers.pendingView = mode; }, {},
            [this, mode] { return settings_.layers.pendingView == mode; });
    };
    pendingCmd("view.pendingShaded", "Pending Changes: Shaded", 0,
               "In a session: walls, floor and client-only things not yet published greyed; what they remove as ghosts.");
    pendingCmd("view.pendingFinal", "Pending Changes: As Final", 1, "In a session: the working copy as it will look once published.");
    pendingCmd("view.pendingHidden", "Pending Changes: Hidden", 2, "In a session: only what the game has (the published map).");
    add("view.pendingPublished", "Show Published / Working Copy", "View", "G",
        "In a session: flips between the published map (Pending Changes: Hidden) and the working copy as the setting had it.", [this] {
            if (settings_.layers.pendingView != 2) {
                pendingKeptView_ = settings_.layers.pendingView;
                settings_.layers.pendingView = 2;
                status("Showing the published map (G: the working copy)");
            } else {
                settings_.layers.pendingView = pendingKeptView_ == 2 ? 0 : pendingKeptView_;
                status("Showing the working copy (G: the published map)");
            }
        }, {}, [this] { return settings_.layers.pendingView == 2; });

    // Window
    const auto windowCmd = [&](const char* id, const char* label, const char* k, bool MapEditor::*field) {
        add(id, label, "Window", k, "Shows or hides the panel.", [this, field] { this->*field = !(this->*field); }, {},
            [this, field] { return this->*field; });
    };
    windowCmd("window.layers", "Layers", "Ctrl+Shift+L", &MapEditor::showLayers_);
    windowCmd("window.properties", "Properties", "Ctrl+Shift+O", &MapEditor::showProperties_);
    windowCmd("window.tool", "Tool Options", "Ctrl+Shift+T", &MapEditor::showTool_);
    windowCmd("window.history", "History", "Ctrl+Shift+H", &MapEditor::showHistory_);
    windowCmd("window.problems", "Problems", "Ctrl+Shift+P", &MapEditor::showProblems_);
    add("window.chat", "Chat", "Window", "", "Shows or hides the Chat panel: the session's in-game chat, and a box to chat to the players.",
        [this] { showChat_ = !showChat_; }, [this] { return session_ != nullptr; }, [this] { return showChat_; });
    add("window.resetLayout", "Reset Layout", "Window", "", "The panels back where they start.", [this] { resetLayout_ = true; });
    add("window.settings", "Editor Settings...", "Window", "Ctrl+,", "Interface scale, theme, autosave.", [this] { showSettings_ = true; });
    add("help.keys", "Keyboard Shortcuts...", "Help", "Ctrl+Shift+K", "Every command's shortcut; click one to rebind it.",
        [this] { showKeys_ = true; });
    add("help.about", "About the Map Editor", "Help", "", "", [this] { showAbout_ = true; });
}

void MapEditor::runCommands(const std::string& list)
{
    std::istringstream in(list);
    for (std::string step; std::getline(in, step, ';');) {
        if (!step.empty()) {
            queued_.push_back(step);
        }
    }
}

// --- Events and the frame ----------------------------------------------------------------------

void MapEditor::handleEvent(const SDL_Event& event)
{
    imgui_->processEvent(event);
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(ctx_.window)) {
        // The main window's close button with floating panels out: SDL quits on the last
        // top-level window's close only, and a floating window with no parent (where the platform
        // gives it none) is one more. The quit asked for as the last window's close would.
        bool others = false;
        int count = 0;
        if (SDL_Window** windows = SDL_GetWindows(&count)) {
            for (int i = 0; i < count; ++i) {
                if (windows[i] != ctx_.window && !SDL_GetWindowParent(windows[i]) && !(SDL_GetWindowFlags(windows[i]) & SDL_WINDOW_HIDDEN)) others = true;
            }
            SDL_free(windows);
        }
        if (others) {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            quit.quit.timestamp = SDL_GetTicksNS();
            SDL_PushEvent(&quit);
        }
        return;
    }
    if (event.type != SDL_EVENT_KEY_DOWN) {
        return;
    }
    const SDL_Keycode k = event.key.key;
    const bool modifierKey = k == SDLK_LCTRL || k == SDLK_RCTRL || k == SDLK_LSHIFT || k == SDLK_RSHIFT || k == SDLK_LALT ||
                             k == SDLK_RALT || k == SDLK_LGUI || k == SDLK_RGUI;
    editor::Shortcut pressed;
    pressed.ctrl = (event.key.mod & SDL_KMOD_CTRL) != 0;
    pressed.shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
    pressed.alt = (event.key.mod & SDL_KMOD_ALT) != 0;
    pressed.key = modifierKey ? std::string() : std::string(SDL_GetKeyName(k));
    if (rebinding_) {
        // The Keyboard Shortcuts window waits for a key: Escape cancels, Backspace unbinds.
        if (modifierKey) {
            return;
        }
        if (k == SDLK_ESCAPE && !pressed.ctrl && !pressed.shift && !pressed.alt) {
            rebinding_.reset();
            return;
        }
        commands_.bind(*rebinding_, k == SDLK_BACKSPACE ? editor::Shortcut{} : pressed);
        rebinding_.reset();
        saveSettings();
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (pick_ && k == SDLK_ESCAPE && !io.WantTextInput) {
        cancelPick(); // (a field's pick in view: Escape cancels it first)
        return;
    }
    if (io.WantTextInput || modifierKey || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || scriptPanel_->hasKeyboard() ||
        modPanel_->hasKeyboard()) {
        return; // (typing in a field, or a dialog or menu has the keys)
    }
    if (k == SDLK_ESCAPE && !pressed.ctrl && !pressed.shift && !pressed.alt && moveDragging()) {
        cancelDrags(); // (Escape mid-drag: everything back where it was, nothing done)
        return;
    }
    if (strokeActive()) {
        return; // (a stroke being drawn, or a drag shown live: it ends first)
    }
    const editor::Command* c = commands_.match(pressed);
    if (!c) {
        return;
    }
    const bool repeats = c->id.starts_with("edit.nudge") || c->id.starts_with("view.zoom") || c->id == "edit.undo" || c->id == "edit.redo";
    if (event.key.repeat && !repeats) {
        return;
    }
    commands_.run(c->id);
}

bool MapEditor::playKey(const SDL_Event& event)
{
    return playView_->gameKey(event);
}

void MapEditor::runStep(std::string step)
{
    if (step.starts_with("@") && step.find(':') != std::string::npos) {
        // (Tests) "@step:dx,dy,...": its first two points (a camera step's one) from the view's
        // centre, e.g. after session-camera-to:NAME (a player wherever the game put them).
        const std::size_t colon = step.find(':');
        std::string out = step.substr(1, colon);
        const bool camera = out == "camera:";
        int index = 0;
        for (std::size_t p = colon + 1; p < step.size();) {
            const std::size_t comma = std::min(step.find(',', p), step.size());
            const std::string part = step.substr(p, comma - p);
            if (index < (camera ? 2 : 4)) {
                const float v = std::strtof(part.c_str(), nullptr) + (index % 2 == 0 ? camera_.x : camera_.y);
                out += std::to_string(v);
            } else {
                out += part;
            }
            if (comma < step.size()) out += ',';
            ++index;
            p = comma + 1;
        }
        step = out;
    }
    if (step.starts_with("wait:")) {
        queuedWait_ = std::atoi(step.c_str() + 5);
    } else if (const auto cam = numbersAfter(step, "camera:", 3)) {
        camera_.x = (*cam)[0];
        camera_.y = (*cam)[1];
        camera_.zoom = std::clamp((*cam)[2], 1.0f / 8, 16.0f);
        cameraSet_ = true;
    } else if (const auto box = numbersAfter(step, "box:", 4)) {
        me::Region r{static_cast<me::Region::Shape>(settings_.regionShape), {{(*box)[0], (*box)[1]}, {(*box)[2], (*box)[3]}}};
        setSelection(me::selectIn(doc_, r, pickFilter()));
    } else if (const auto mv = numbersAfter(step, "move:", 2)) {
        me::Selection moved;
        if (perform(me::moveOp(doc_, selection_, (*mv)[0], (*mv)[1], &moved))) setSelection(std::move(moved), true);
    } else if (const auto at = numbersAfter(step, "paste-at:", 2)) {
        paste(me::wallCellAt((*at)[0], (*at)[1]));
    } else if (step.starts_with("open:")) {
        open(fs::path(reinterpret_cast<const char8_t*>(step.c_str() + 5)));
    } else if (step.starts_with("save-as:")) {
        saveAs(step.substr(8));
    } else if (step == "new") {
        newDocument(newOptions_);
    } else if (const auto mm = numbersAfter(step, "mouse:", 2)) {
        // Tests: the mouse moved to window pixel (x, y), as SDL would tell it.
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.windowID = SDL_GetWindowID(ctx_.window);
        e.motion.x = (*mm)[0];
        e.motion.y = (*mm)[1];
        testMouseX_ = e.motion.x;
        testMouseY_ = e.motion.y;
        SDL_PushEvent(&e);
    } else if (step == "ldown" || step == "lup" || step == "rdown" || step == "rup" || step == "mdown" || step == "mup") {
        SDL_Event e{};
        e.type = step.ends_with("down") ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.windowID = SDL_GetWindowID(ctx_.window);
        e.button.button = step[0] == 'l' ? SDL_BUTTON_LEFT : step[0] == 'r' ? SDL_BUTTON_RIGHT : SDL_BUTTON_MIDDLE;
        e.button.down = step.ends_with("down");
        e.button.clicks = 1;
        e.button.x = testMouseX_;
        e.button.y = testMouseY_;
        SDL_PushEvent(&e);
    } else if (step.starts_with("text:")) {
        // Tests: text typed (into what has the keyboard), as SDL would bring it.
        testText_.push_back(step.substr(5));
        SDL_Event e{};
        e.type = SDL_EVENT_TEXT_INPUT;
        e.text.windowID = SDL_GetWindowID(ctx_.window);
        e.text.text = testText_.back().c_str();
        SDL_PushEvent(&e);
    } else if (step.starts_with("key:")) {
        // Tests: a key pressed and let go ("key:Return", "key:Tab", "key:Escape": SDL's names).
        SDL_Event e{};
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.windowID = SDL_GetWindowID(ctx_.window);
        e.key.key = SDL_GetKeyFromName(step.c_str() + 4);
        e.key.scancode = SDL_GetScancodeFromKey(e.key.key, nullptr);
        e.key.down = true;
        SDL_PushEvent(&e);
        e.type = SDL_EVENT_KEY_UP;
        e.key.down = false;
        SDL_PushEvent(&e);
    } else if (step == "window-close") {
        // Tests: the window's close button, as SDL tells it (main.cpp asks requestQuit).
        SDL_Event e{};
        e.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&e);
    } else if (step.starts_with("palette:")) {
        commands_.run("view.palette");
        paletteQuery_ = step.substr(8);
    } else if (const auto pk = numbersAfter(step, "pick:", 2)) {
        // Tests: a pick in view answered at a world point, as a click there would.
        if (pick_) {
            PickResult r;
            r.at = {(*pk)[0], (*pk)[1]};
            const me::Selection under = me::pickAt(doc_, r.at.x, r.at.y, 12, me::KindFilter{});
            if (!under.objects.empty()) r.object = *under.objects.begin();
            if (!under.waypoints.empty()) r.waypoint = *under.waypoints.begin();
            if (const auto c = me::wallCellAt(r.at.x, r.at.y); c && doc_.walls.contains(*c)) r.wall = *c;
            PickRequest req = *pick_;
            if (!req.repeat) pick_.reset();
            if (req.done) req.done(r);
        }
    } else if (step.starts_with("live:")) {
        // Tests: a live entity for the live layer ("live:name,x,y,team,stock,player"), as a
        // session feeds them; ",TYPE,dir,state,armour,weapon" after it: drawn as the game draws
        // it (a thing type by name, an AnimDir, its animation state, worn class bits).
        LiveEntity e;
        char name[64] = {};
        char type[64] = {};
        int team = 0, stock = 0, player = 1, dir = 7, state = 0;
        unsigned armor = 0, weapon = 0;
        if (std::sscanf(step.c_str() + 5, "%63[^,],%f,%f,%d,%d,%d,%63[^,],%d,%d,%x,%x", name, &e.x, &e.y, &team, &stock, &player, type, &dir,
                        &state, &armor, &weapon) >= 3) {
            e.name = name;
            e.team = static_cast<std::uint8_t>(team);
            e.stockClient = stock != 0;
            e.player = player != 0;
            e.id = static_cast<std::uint16_t>(0x7000 + live_.size());
            if (const auto* def = type[0] ? things_.findThing(type) : nullptr) {
                e.typeId = static_cast<std::uint16_t>(def - things_.things.data() + 1);
            }
            e.direction = static_cast<std::uint8_t>(dir);
            e.animState = static_cast<std::uint8_t>(state);
            e.armorEquip = armor;
            e.weaponEquip = weapon;
            std::vector<LiveEntity> all = live_;
            all.push_back(std::move(e));
            setLiveEntities(std::move(all));
        }
    } else if (step.starts_with("chat:") || step.starts_with("chat-team:")) {
        // (Tests) A line in the Chat panel as the game would bring it: "chat:NAME,text"
        // ("chat-team:" a team line; no name: the server's own).
        const bool team = step.starts_with("chat-team:");
        const std::string rest = step.substr(team ? 10 : 5);
        const auto comma = rest.find(',');
        editor::ChatLine l;
        l.speaker = comma == std::string::npos ? std::string() : rest.substr(0, comma);
        l.text = comma == std::string::npos ? rest : rest.substr(comma + 1);
        l.kind = l.speaker.empty() ? editor::ChatLine::Kind::System : team ? editor::ChatLine::Kind::Team : editor::ChatLine::Kind::Chat;
        addChat(std::move(l));
    } else if (step.starts_with("chat-say:")) {
        // (Tests) the Chat panel's box sending a line into the game.
        if (sessionHooks_.say) sessionHooks_.say(step.substr(9), false);
    } else if (step == "chat-focus") {
        // (Tests) the Chat panel to the front (its unread count cleared as it's seen).
        showChat_ = true;
        chatFocus_ = true;
    } else if (step == "chat-hide") {
        // (Tests) the Chat panel behind another tab in its dock: new lines mark its tab.
        if (const ImGuiWindow* c = ImGui::FindWindowByName("###Chat"); c && c->DockNode) {
            for (const ImGuiWindow* w : c->DockNode->Windows) {
                if (w != c) {
                    ImGui::SetWindowFocus(w->Name); // (its tab to the front of the dock they share)
                    break;
                }
            }
        }
    } else if (step.starts_with("theme:")) {
        // (Tests) "theme:dark|medium|light": as the Theme drop-down picks it.
        editor::Theme t{};
        if (editor::themeFromKey(step.substr(6), t)) {
            setTheme(t);
        } else {
            status("theme: dark, medium or light");
        }
    } else if (step == "theme-menu") {
        // (Tests) the menu bar's Theme drop-down opened, as a click on it would.
        openThemeMenu_ = true;
    } else if (step == "floating:on" || step == "floating:off") {
        // (Tests) panels may float (a test's run starts with them kept in), or not.
        imgui_->setFloating(step == "floating:on");
    } else if (step == "floating:away") {
        // (Tests) the floating windows put away, as while a full-window play-test has the
        // window (backgroundTick); they come back this frame.
        imgui_->closeFloating();
    } else if (step.starts_with("float:")) {
        // (Tests) "float:WINDOW,dx,dy,w,h": the panel undocked and put at (dx, dy) from the
        // main window's top left, w x h: past its edge, a floating window of its own.
        const std::string rest = step.substr(6);
        std::size_t cut = rest.size();
        for (int n = 0; n < 4 && cut != std::string::npos && cut > 0; ++n) cut = rest.rfind(',', cut - 1);
        const auto nums = cut == std::string::npos ? std::nullopt : numbersAfter(rest.substr(cut), "", 4);
        if (nums) {
            floatRequest_ = FloatRequest{rest.substr(0, cut), (*nums)[0], (*nums)[1], (*nums)[2], (*nums)[3], 30};
            if (ImGuiWindow* w = ImGui::FindWindowByName(floatRequest_->window.c_str()); w && w->DockId != 0) {
                ImGui::DockContextQueueUndockWindow(ImGui::GetCurrentContext(), w); // (done in the next frame's start)
            }
        } else {
            status("float: needs WINDOW,dx,dy,w,h");
        }
    } else if (step.starts_with("float-shot:")) {
        // (Tests) each floating window's next frame saved as BASE-N.png.
        SDL_Log("map editor: %d floating window(s) open", imgui_->floatingWindows());
        imgui_->shootFloating(step.substr(11));
    } else if (step.starts_with("layout-file:") || step.starts_with("layout-save:")) {
        // (Tests) the panels' layout (imgui.ini, the floating windows' places too) read from or
        // written to a file of the test's.
        const fs::path file(reinterpret_cast<const char8_t*>(step.c_str() + 12));
        if (step[7] == 'f') {
            try {
                const auto bytes = nox::readFileBytes(file);
                ImGui::LoadIniSettingsFromMemory(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                status("Layout read from " + pathText(file));
            } catch (const std::exception&) {
                status("Layout " + pathText(file) + " can't be read");
            }
        } else {
            std::size_t n = 0;
            const char* ini = ImGui::SaveIniSettingsToMemory(&n);
            std::ofstream(file, std::ios::binary).write(ini, static_cast<std::streamsize>(n));
            status("Layout written to " + pathText(file));
        }
    } else if (sessionStep(step)) {
        // (session-wait-synced, session-request: ...)
    } else if (markers_->runStep(step)) {
        // (waypoint:, polygon:, group: ...)
    } else if (props_->runStep(step)) {
        // (prop:, select-object: ...)
    } else if (objects_->runStep(step)) {
        // (object:, place:, scatter:, prefab-save: ...)
    } else if (modPanel_->runStep(step)) {
        // (mod-new:, mod-open-file:, mod-type: ...)
    } else if (scriptPanel_->runStep(step)) {
        // (script-goto:, script-type:, script-find:)
    } else if (toolStep(step)) {
        // (a tool step: floor:, wall:, stroke: ...)
    } else if (!commands_.run(step)) {
        status("No command " + step + " (or it's disabled now)");
    }
}

void MapEditor::frame()
{
    // A named layout picked last frame (ImGui reads layouts between frames).
    if (!loadLayout_.empty()) {
        try {
            const auto bytes = nox::readFileBytes(ctx_.prefDir / "layouts" / (loadLayout_ + ".ini"));
            ImGui::LoadIniSettingsFromMemory(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            status("Layout " + loadLayout_);
        } catch (const std::exception&) {
            status("Layout " + loadLayout_ + " can't be read");
        }
        loadLayout_.clear();
    }
#if NOX_PRIVATE_TOOLS
    devhooks::editorFrame([this](const std::string& step) { queued_.push_back(step); }, queued_.empty() && queuedWait_ == 0);
#endif
    // runCommands' steps, one a frame (so what each opens is drawn before the next).
    if (queuedWait_ > 0) {
        --queuedWait_;
    } else if (!queued_.empty()) {
        std::string step = queued_.front();
        queued_.erase(queued_.begin());
        runStep(std::move(step));
    }
    {
        std::lock_guard lock(pickedMutex_);
        if (picked_) {
            const fs::path path(reinterpret_cast<const char8_t*>(picked_->c_str()));
            picked_.reset();
            openOpen_ = false;
            guarded(Pending::Open, [this, path] { open(path); });
        }
    }
    if (styleDirty_) {
        styleDirty_ = false;
        imgui_->setStyle(settings_.uiScale, settings_.theme);
        saveSettings();
    }
    if ((preview_.active() || previewKey_) && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        endPreview(); // (a drag let go where its release wasn't seen: nothing done)
    }
    if (floatingDirty_) {
        floatingDirty_ = false; // ("Allow floating panels": ImGui takes it between frames)
        imgui_->setFloating(settings_.floatingPanels);
        saveSettings();
    }
    if (session_) {
        sessionFrame();
    } else {
        autosave();
    }
    scriptPanel_->tick();
    modPanel_->tick();
    runValidation();
    rebuildView();

    // The map into its panel's texture (last frame's size), then the panels over it.
    renderMap();
    playView_->beforeUi();
    imgui_->newFrame();
    buildUi();
    SDL_SetRenderTarget(ctx_.renderer, nullptr);
    SDL_SetRenderDrawColor(ctx_.renderer, 16, 15, 14, 255);
    SDL_RenderClear(ctx_.renderer);
    imgui_->render();

    if (session_) {
        // A stroke previews in the working copy itself: the session's news wait till it's done.
        session_->setHold(strokeActive() || wallPreviewUndo_.has_value()); // (strokeActive: a drag's live preview too)
    }
    static std::string shownTitle;
    if (const std::string t = title(); t != shownTitle) {
        shownTitle = t;
        SDL_SetWindowTitle(ctx_.window, t.c_str());
    }
}

void MapEditor::backgroundTick()
{
    scriptPanel_->tick();
    modPanel_->tick();
    imgui_->closeFloating(); // (the game has the window: the floating panels come back with the editor)
    chat_.setViewing(false); // (the Chat panel isn't seen behind the game: lines meanwhile are unread)
}

ImVec2 MapEditor::windowOrigin() const
{
    return ImGui::GetMainViewport()->Pos; // ((0, 0) unless panels can float: then the desktop's coordinates)
}

SDL_FRect MapEditor::playViewport() const
{
    const ImVec2 o = windowOrigin();
    return SDL_FRect{panelX_ - o.x, panelY_ - o.y, panelW_, panelH_};
}

void MapEditor::applyFloatRequest()
{
    // (Tests) the "float:" step's panel, once undocked: placed past the main window's edge, where
    // ImGui gives it a viewport (a window) of its own.
    if (!floatRequest_) return;
    ImGuiWindow* w = ImGui::FindWindowByName(floatRequest_->window.c_str());
    if (!w || w->DockId != 0) {
        if (--floatRequest_->frames <= 0) {
            status("float: no undocked window " + floatRequest_->window);
            floatRequest_.reset();
        }
        return;
    }
    const ImVec2 o = windowOrigin();
    ImGui::SetWindowPos(w, ImVec2(o.x + floatRequest_->dx, o.y + floatRequest_->dy), ImGuiCond_Always);
    ImGui::SetWindowSize(w, ImVec2(floatRequest_->w, floatRequest_->h), ImGuiCond_Always);
    if (w->Viewport == ImGui::GetMainViewport()) {
        w->Viewport = nullptr; // (chosen again in its Begin: outside the main window, a viewport of its own)
        w->ViewportId = 0;
    }
    SDL_Log("map editor: %s put at %.0f,%.0f from the window's corner", w->Name, floatRequest_->dx, floatRequest_->dy);
    floatRequest_.reset();
}

void MapEditor::runValidation()
{
    if (validating_.valid() && validating_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        issues_ = validating_.get();
    }
    const std::uint64_t rev = history_.revision();
    if (rev != validatedRevision_ && !validating_.valid() && SDL_GetTicks() - changedMs_ > 400) {
        // Off the drawing thread, on a copy (the editor stays usable on big maps).
        validatedRevision_ = rev;
        validating_ = std::async(std::launch::async, [doc = doc_, things = &things_] { return me::validate(doc, *things); });
    }
}

} // namespace noxsdl
