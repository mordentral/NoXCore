// The map editor in a collaborative session: the session's
// events into the editor (others' ops, our acks and refusals, the lock table), the live view, our
// presence, the Session panel (who's in, edit requests, locks, publish, play-test, settings), the
// edit request toast, other editors' cursors and the pending edits (what differs from the published
// map, given to the view to draw greyed: updatePendingView).
#include "editor/map_editor.hpp"
#include "editor/object_tools.hpp"
#include "editor/pencil_mark.hpp"
#include "editor/script_panel.hpp"
#include "editor/theme_colours.hpp"
#include "editor/ui_helpers.hpp"

#include "noxcore/client/edit_session_client.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/mapedit/pending.hpp"
#include "noxcore/mapedit/validate.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/things/thing_db.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>

namespace fs = std::filesystem;
namespace me = nox::mapedit;
namespace ss = nox::mapedit::session;

namespace noxsdl {

namespace {

using editor::col;

/// An editor's colour (as the lock outlines: map_editor_view.cpp's peerColour; pencil_icon.hpp).
ImU32 peerColour(me::PeerId peer)
{
    return editor::peerColourU32(peer);
}

/// The session panel's help comes up sooner than the other panels'.
void help(const char* text)
{
    editor::help(text, ImGuiHoveredFlags_DelayShort);
}

using editor::kSessionModes;

} // namespace

void MapEditor::attachSession(nox::client::EditSessionClient* session, SessionHooks hooks)
{
    if (!session) {
        if (session_) {
            // The session's client side back on its own copy of the working copy: it must never keep
            // our document, which goes with the editor (closing a joined session's editor while its
            // game runs on crashed in the client's next tick).
            session_->attach(nullptr);
            session_ = nullptr;
            sessionHooks_ = {};
            live_.clear();
            liveTracks_.clear();
            locks_ = {};
            self_ = 0;
            selection_.clear();
            panelLocks_.clear();
            history_.clear();
            history_.touch();
            docPath_.clear();
            sessionSeqs_.clear();
            status("The session ended: the map stays here as an unsaved copy (Save As keeps it)");
        }
        return;
    }
    session_ = session;
    sessionHooks_ = std::move(hooks);
    session_->attach(&doc_);
    resetDocumentState();
    docPath_.clear();
    docName_ = session_->hello() ? session_->hello()->map : std::string();
    sidecar_ = {};
    self_ = session_->self();
    locks_ = session_->locks();
    sessionRevision_ = ~0ull;
    sessionLiveVersion_ = ~0u;
    pendingFor_ = {};
    if (scriptPanel_) scriptPanel_->documentOpened();
    status(session_->synced() ? "In the session: " + docName_ : "Joining the session...");
}

void MapEditor::showSessionSetup(std::function<void(const editor::SessionSetup&)> host, std::function<void()> cancelled)
{
    setupHost_ = std::move(host);
    setupCancelled_ = std::move(cancelled);
    std::vector<editor::SessionMapChoice> maps;
    refreshMapList();
    for (const auto& m : mapList_) {
        maps.push_back({m.name, m.path, m.stock});
    }
    // The game's own maps too (a stock map is copied under a new name), when the editor's maps
    // folder is another.
    std::error_code ec;
    const fs::path gameMaps = ctx_.gameDir / "maps";
    if (!fs::equivalent(gameMaps, ctx_.mapsDir, ec)) {
        for (const auto& dir : fs::directory_iterator(gameMaps, ec)) {
            if (!dir.is_directory(ec)) continue;
            const std::string name = dir.path().filename().string();
            if (std::ranges::any_of(maps, [&](const auto& m) { return m.name == name; })) continue;
            const fs::path p = nox::resolveNoCase(dir.path() / (name + ".map"));
            if (fs::exists(p, ec)) maps.push_back({name, p, me::isStockMapName(name)});
        }
    }
    editor::SessionSetup defaults;
    defaults.newOptions = newOptions_;
    setupDialog_.open(std::move(maps), defaults, setupListing_);
}

void MapEditor::setSessionAlwaysEditors(std::vector<std::string> names)
{
    if (names == settings_.sessionAlwaysEditors) return;
    settings_.sessionAlwaysEditors = std::move(names);
    saveSettings(); // (a test's run writes nothing)
}

// --- The live layer's follow and jump in ---------------------------------------------------------

const MapEditor::LiveEntity* MapEditor::liveAt(const ImVec2& origin, const ImVec2& mouse) const
{
    if (!session_ || !settings_.layers.livePlayers) return nullptr;
    const float z = camera_.zoom;
    const LiveEntity* best = nullptr;
    float bestD = 10.0f * 10.0f; // (screen pixels: the diamond's size and a little)
    for (const auto& e : live_) {
        if (!e.player) continue;
        const auto [x, y] = livePos(e);
        const float sx = origin.x + (x - camera_.left()) * z;
        const float sy = origin.y + (y - camera_.top()) * z;
        float d = (sx - mouse.x) * (sx - mouse.x) + (sy - mouse.y) * (sy - mouse.y);
        // (Drawn as their character: anywhere on the body, its middle half its height up.)
        if (const auto t = liveTracks_.find(e.id); t != liveTracks_.end() && t->second.drawn) {
            const float half = t->second.headY * z / 2;
            const float dx = std::abs(sx - mouse.x), dy = std::abs(sy - half - mouse.y);
            if (dx <= std::max(10.0f, 14 * z) && dy <= half + 4) d = std::min(d, dx * dx * 0.25f);
        }
        if (d <= bestD) {
            bestD = d;
            best = &e;
        }
    }
    return best;
}

bool MapEditor::liveInput(const ImVec2& origin, bool hovered)
{
    if (!session_) return false;
    // Following: the camera on the player, till the view is panned or Escape.
    if (follow_) {
        const auto it = std::ranges::find_if(live_, [&](const LiveEntity& e) { return e.player && e.id == *follow_; });
        if (it == live_.end()) {
            follow_.reset();
            status("The player you followed left the game");
        } else if (drag_ == Drag::Pan || (rightDragging_ && ImGui::IsMouseDown(ImGuiMouseButton_Right)) ||
                   (hovered && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
            follow_.reset();
            status("Stopped following " + it->name);
        } else {
            std::tie(camera_.x, camera_.y) = livePos(*it);
            cameraSet_ = true;
        }
    }
    if (!hovered || pick_) return false;
    const ImVec2 m = ImGui::GetIO().MousePos;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (const LiveEntity* e = liveAt(origin, m)) {
            follow_ = e->id;
            status("Following " + e->name + " (pan or Escape stops; right-click: Jump in here)");
            return true;
        }
    }
    return false;
}

void MapEditor::liveMenu()
{
    if (liveMenu_ && !ImGui::IsPopupOpen("##livemenu")) {
        ImGui::OpenPopup("##livemenu");
    }
    if (!ImGui::BeginPopup("##livemenu")) {
        liveMenu_.reset();
        return;
    }
    if (liveMenu_) {
        const LiveEntity e = *liveMenu_;
        ImGui::TextDisabled("%s%s", e.name.c_str(), e.stockClient ? "  [stock]" : "  [NoX Core]");
        ImGui::Separator();
        const bool following = follow_ && *follow_ == e.id;
        if (ImGui::MenuItem(following ? "Stop following" : "Follow")) {
            if (following) {
                follow_.reset();
            } else {
                follow_ = e.id;
                status("Following " + e.name + " (pan or Escape stops)");
            }
            liveMenu_.reset();
        }
        const bool self = e.name == session_->nameOf(self_);
        if (ImGui::MenuItem("Jump in here", nullptr, false, !self && !session_->playTestRunning())) {
            jumpInAt(e);
            liveMenu_.reset();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", self ? "That's you" : "Your character into the game beside this player, and play (F2 comes back)");
        }
    }
    ImGui::EndPopup();
}

void MapEditor::jumpInAt(const LiveEntity& e)
{
    if (!session_) return;
    session_->jumpIn(e.id);
    follow_.reset();
    status("Jumping in beside " + e.name);
    if (sessionHooks_.toGameAsIs) {
        sessionHooks_.toGameAsIs();
    } else if (sessionHooks_.toGame) {
        sessionHooks_.toGame();
    }
}

void MapEditor::sessionPlayFrom(float x, float y)
{
    if (!session_) return;
    follow_.reset();
    char where[48];
    std::snprintf(where, sizeof where, "%.0f, %.0f", x, y);
    status(std::string("Playing in the session from ") + where);
    if (sessionHooks_.toGameAt) {
        sessionHooks_.toGameAt(x, y);
    } else if (sessionHooks_.toGame) {
        sessionHooks_.toGame();
    }
}

void MapEditor::sendLocks()
{
    if (!session_) return;
    std::set<me::ElementKey> wanted = me::elementsOf(doc_, selection_);
    wanted.insert(panelLocks_.begin(), panelLocks_.end());
    session_->requestLocks(wanted);
}

void MapEditor::sessionPublish()
{
    if (!session_) return;
    if (!session_->isOwner()) {
        status("Only the session's host publishes");
        return;
    }
    scriptPanel_->commit(); // (what's being typed goes first, as an op)
    session_->publish(true);
    status("Publishing: the map changes for everyone (stock clients download it)");
}

void MapEditor::sessionReloadScript(bool hot)
{
    if (!session_) return;
    if (!session_->canEdit()) {
        status("Only editors can reload the session's script");
        return;
    }
    session_->reloadScript(hot);
    status(std::string("Asked the host to ") + (hot ? "hot-reload" : "restart") + " the script for everyone in the game");
}

bool MapEditor::sessionStep(const std::string& step)
{
    if (!step.starts_with("session-")) return false;
    const auto again = [&] {
        queued_.insert(queued_.begin(), step);
        queuedWait_ = 5;
    };
    if (step == "session-wait-synced") {
        if (!session_ || !session_->synced()) again();
    } else if (step == "session-wait-editor") {
        if (!session_ || !session_->canEdit()) again();
    } else if (step == "session-wait-request") {
        if (!session_ || session_->requests().empty()) again();
    } else if (step.starts_with("session-place-near-me:")) {
        // A thing placed beside our own player as the live view shows it (waits till it does).
        const std::string me = session_->nameOf(self_);
        const auto it = std::ranges::find_if(session_->live(), [&](const auto& l) { return l.player && l.name == me; });
        if (it == session_->live().end()) {
            again();
        } else {
            objects_->runStep("object:" + step.substr(22));
            objects_->runStep("place:" + std::to_string(it->x + 60) + "," + std::to_string(it->y - 20));
            camera_.x = it->x;
            camera_.y = it->y;
            cameraSet_ = true;
        }
    } else if (step.starts_with("session-camera-to:")) {
        // (Tests) the view onto a player as the live view shows them (waits till it does).
        const std::string name = step.substr(18);
        const ss::LiveEntry* found = nullptr;
        if (session_) {
            for (const auto& l : session_->live()) {
                if (l.player && l.name == name) found = &l;
            }
        }
        if (!found) {
            again();
        } else {
            camera_.x = static_cast<float>(found->x);
            camera_.y = static_cast<float>(found->y);
            cameraSet_ = true;
        }
    } else if (step == "session-play") {
        if (sessionHooks_.toGame) sessionHooks_.toGame();
    } else if (step.starts_with("session-follow:") || step.starts_with("session-jump:")) {
        // (Tests) a live player by name: the camera follows them, or our character jumps in beside them.
        const bool jump = step.starts_with("session-jump:");
        const std::string name = step.substr(jump ? 13 : 15);
        const auto it = std::ranges::find_if(live_, [&](const LiveEntity& e) { return e.player && e.name == name; });
        if (it == live_.end()) {
            again();
        } else if (jump) {
            jumpInAt(*it);
        } else {
            follow_ = it->id;
            status("Following " + it->name);
        }
    } else if (step == "session-end:confirm" || step == "session-end:cancel") {
        // (Tests) the "End the Session?" prompt answered, as its End Session / Cancel buttons.
        if (!endSessionAsked_ && !endSessionOpen_) {
            status("No End the Session? prompt up: " + step);
        } else {
            endSessionAnswer_ = step.ends_with("confirm") ? 1 : 2; // (endSessionDialog presses it)
        }
    } else if (step == "session-private-test") {
        commands_.run("play.start");
    } else if (step == "session-show") {
        showSession_ = true;
        ImGui::SetWindowFocus("Session");
    } else if (step == "session-setup") {
        showSessionSetup([this](const editor::SessionSetup& s) { status("Would host " + s.name); }); // (tests: the dialog alone)
    } else if (!session_) {
        status("Not in a session: " + step);
    } else if (step.starts_with("session-request:")) {
        session_->requestEdit(step.substr(16));
    } else if (step == "session-approve-all") {
        for (const auto& q : std::vector<ss::EditRequest>(session_->requests())) session_->answer(q.peer, 1);
    } else if (step == "session-publish") {
        sessionPublish();
    } else if (step == "session-playtest:start") {
        session_->playTest(true, kSessionModes[playTestModeIndex_].bits, playTestCanDie_);
    } else if (step == "session-playtest:end") {
        session_->playTest(false);
    } else {
        status("No session step " + step);
    }
    return true;
}

void MapEditor::sessionFrame()
{
    if (!session_) return;
    using Kind = nox::client::EditSessionClient::Event::Kind;
    self_ = session_->self();
    if (const auto* h = session_->hello(); h && docName_.empty()) docName_ = h->map;
    for (auto& e : session_->takeEvents()) {
        switch (e.kind) {
        case Kind::Synced:
            history_.clear();
            selection_.clear();
            selectionAt_.clear();
            sessionSeqs_.clear();
            docName_ = session_->hello() ? session_->hello()->map : docName_;
            cameraSet_ = false;
            if (scriptPanel_) scriptPanel_->documentOpened();
            status("In step with the session: " + docName_);
            break;
        case Kind::Committed:
            if (e.author != 0) {
                status(session_->nameOf(e.author) + ": " + (e.op ? e.op->label : std::string("an edit")));
            }
            break;
        case Kind::Acked:
            if (const auto it = sessionSeqs_.find(e.seq); it != sessionSeqs_.end()) {
                // Our op as the host committed it: new ids put on the way go into our undo.
                const std::size_t done = history_.dropped() + history_.position();
                if (e.op && e.inverse && it->second >= history_.dropped() && it->second < done) {
                    const auto& entry = history_.entries()[it->second - history_.dropped()];
                    if (me::serializeOp(entry.op, things_) != me::serializeOp(*e.op, things_)) {
                        history_.replaceDone(done - it->second, *e.op, *e.inverse);
                    }
                }
                sessionSeqs_.erase(it);
            }
            break;
        case Kind::Rejected:
        case Kind::RolledBack:
            status((e.kind == Kind::Rejected ? "Refused by the host: " : "Undone (another's edit came first): ") + e.text);
            if (const auto it = sessionSeqs_.find(e.seq); it != sessionSeqs_.end()) {
                if (it->second + 1 == history_.dropped() + history_.position()) {
                    history_.stepBack(); // (left to redo once it can: the lock let go)
                }
                sessionSeqs_.erase(it);
            }
            break;
        case Kind::Role:
            sessionRoleText_ = e.text;
            if (!e.text.empty()) status("Session: " + e.text);
            break;
        case Kind::Request:
            if (e.request.open && session_->isOwner()) status(e.request.name + " asks to edit");
            break;
        case Kind::Published:
            status(e.published.full ? "Published version " + std::to_string(e.published.version) + ": the map changes for everyone"
                                    : "Objects published (auto) for players who join later");
            pendingFor_ = {};
            break;
        case Kind::ScriptDiag:
            status("Script: " + e.diag.summary);
            break;
        case Kind::Locks:
        case Kind::LockAnswer: {
            locks_ = session_->locks();
            std::size_t skipped = 0;
            me::Selection held = me::withoutLocked(doc_, selection_, locks_, self_, &skipped);
            if (skipped > 0) {
                selection_ = std::move(held);
                status(std::to_string(skipped) + (skipped == 1 ? " item" : " items") + " let go: being edited by others");
            }
            for (auto it = panelLocks_.begin(); it != panelLocks_.end();) {
                it = otherHolder(*it) ? panelLocks_.erase(it) : std::next(it);
            }
            break;
        }
        case Kind::Log:
            break;
        case Kind::Park:
            // (Our character stood aside or back: the host's reason when it didn't do as asked.)
            if (!e.text.empty()) status("Session: " + e.text);
            break;
        case Kind::Problem:
            if (e.problem.open) {
                status("Session problem: " + e.problem.text);
                showProblems_ = true;
            }
            break;
        }
    }
    // Anything else that changed the working copy (others' ops, a rollback, the catch-up, a
    // publish): the view made again from it.
    if (session_->revision() != sessionRevision_) {
        sessionRevision_ = session_->revision();
        history_.touch();
        selection_.prune(doc_);
        changedMs_ = SDL_GetTicks();
    }
    if (session_->liveVersion() != sessionLiveVersion_) {
        sessionLiveVersion_ = session_->liveVersion();
        std::vector<LiveEntity> live;
        for (const auto& l : session_->live()) {
            LiveEntity e{l.name, static_cast<float>(l.x), static_cast<float>(l.y), l.team, l.player, l.stock, l.id};
            // The update's animation bytes read as the game's client reads them (client
            // World::setAnim: the facing in bits 4-6, skipping the grid's centre; a creature's
            // state in the low nibble, a player's in its own byte).
            const std::uint8_t dir = (l.anim >> 4) & 7;
            e.typeId = l.type;
            e.direction = dir > 3 ? static_cast<std::uint8_t>(dir + 1) : dir;
            e.animState = l.player ? l.state : static_cast<std::uint8_t>(l.anim & 0x0F);
            if (l.frame != 0xFF) e.frame = l.frame;
            e.armorEquip = l.armor;
            e.weaponEquip = l.weapon;
            e.observer = l.observer;
            live.push_back(std::move(e));
        }
        setLiveEntities(std::move(live));
    }
    if (const auto w = mouseWorld()) {
        session_->presence(w->x, w->y, static_cast<std::uint8_t>(tool_), tool_ == Tool::Floor ? brush_.size * 23.0f : 0.0f, true);
    }
}

// --- Pending edits -------------------------------------------------------------------------------

void MapEditor::updatePendingView()
{
    if (!view_) return;
    const editor::Layers& L = settings_.layers;
    const bool session = session_ && session_->synced() && session_->hasDocument();
    if (!session) {
        if (pendingFor_.any && pendingFor_.view == view_->serial()) view_->setPending({});
        if (pendingFor_.any) {
            pendingPlan_ = {};
            ++pendingPlanSerial_;
        }
        pendingFor_ = {};
        return;
    }
    const int layers = (L.floor ? 1 : 0) | (L.edges ? 2 : 0) | (L.walls ? 4 : 0) | (L.objects ? 8 : 0);
    const int mode = std::clamp(L.pendingView, 0, 2);
    if (pendingFor_.view == view_->serial() && pendingFor_.revision == history_.revision() && pendingFor_.version == session_->version() &&
        pendingFor_.mode == mode && pendingFor_.layers == layers) {
        return;
    }
    // The working copy (the view draws it) against the published map: what the game has.
    const me::PendingEdits edits = me::diffPending(doc_, session_->published(), things_);
    me::PendingPlan plan = me::planPending(edits, me::PendingBase::Working, static_cast<me::PendingView>(mode));
    // (The layers hidden stay hidden: their pending parts too.)
    if (!L.floor) {
        plan.tiles.clear();
        plan.hideTiles.clear();
    } else if (!L.edges) {
        for (auto& t : plan.tiles) t.tile.edges.clear();
    }
    if (!L.walls) {
        plan.walls.clear();
        plan.hideWalls.clear();
    }
    if (!L.objects) {
        plan.objects.clear();
        plan.hideObjects.clear();
    }
    view_->setPending(plan);
    pendingFor_ = {view_->serial(), history_.revision(), session_->version(), mode, layers, !plan.empty()};
    pendingPlan_ = std::move(plan);
    ++pendingPlanSerial_;
}

void MapEditor::drawSession(ImDrawList* dl, const ImVec2& origin, const ImVec2& size)
{
    const float z = camera_.zoom;
    const auto S = [&](float x, float y) { return ImVec2(origin.x + (x - camera_.left()) * z, origin.y + (y - camera_.top()) * z); };
    const float x0 = camera_.left(), y0 = camera_.top(), x1 = x0 + size.x / z, y1 = y0 + size.y / z;
    const auto visible = [&](float x, float y) { return x >= x0 - 60 && x <= x1 + 60 && y >= y0 - 60 && y <= y1 + 60; };
    // (The pending edits are drawn with the map: updatePendingView, MapView::setPending.)
    // Other editors' cursors and brushes, in their colours.
    for (const auto& [peer, p] : session_->presences()) {
        if (peer == self_ || !p.inView || !visible(p.x, p.y)) continue;
        const ImVec2 c = S(p.x, p.y);
        const ImU32 k = peerColour(peer);
        dl->AddLine(ImVec2(c.x - 7, c.y), ImVec2(c.x + 7, c.y), k, 2);
        dl->AddLine(ImVec2(c.x, c.y - 7), ImVec2(c.x, c.y + 7), k, 2);
        if (p.radius > 0) dl->AddCircle(c, p.radius * z, k, 0, 1.0f);
        // (Who's editing) Their pencil and name; "(idle)" when they've done nothing for a while
        // (the host's Editors message).
        const auto* ed = session_->editorByPeer(peer);
        const bool idle = ed && ed->activity == ss::Activity::Idle;
        const std::string name = session_->nameOf(peer) + (idle ? "  (idle)" : "");
        const float h = ImGui::GetTextLineHeight();
        const float pencil = std::round(h * 1.2f);
        dl->AddRectFilled(ImVec2(c.x + 8, c.y + 4), ImVec2(c.x + 14 + pencil + ImGui::CalcTextSize(name.c_str()).x, c.y + 6 + h), col(0, 0, 0, 160), 3);
        editor::pencilMark(dl, ImVec2(c.x + 10 + pencil / 2, c.y + 5 + h / 2), pencil, peer, idle);
        dl->AddText(ImVec2(c.x + 12 + pencil, c.y + 5), idle ? col(170, 170, 170) : k, name.c_str());
    }
}

// --- The Session panel and the toast -------------------------------------------------------------

void MapEditor::sessionPanel()
{
    if (!ImGui::Begin("Session", &showSession_)) {
        ImGui::End();
        return;
    }
    const ss::Hello* h = session_->hello();
    const bool owner = session_->isOwner();
    ImGui::Text("Map: %s   version %u", docName_.c_str(), h ? h->version : 0);
    ImGui::Text("You: %s%s", session_->nameOf(self_).c_str(),
                owner ? " (host)" : session_->role() >= ss::Role::Editor ? " (editor)" : " (viewer: ask to edit)");
    const editor::ThemeColours& tc = editor::themeColours();
    if (!session_->synced()) ImGui::TextColored(tc.warning, "Catching up with the session...");
    if (session_->playTestRunning()) ImGui::TextColored(tc.info, "A play-test round is running: editing waits");
    if (h) {
        ImGui::TextDisabled("%s%s", (h->flags & ss::kPlayersCantDie) ? "Sandbox: players can't die" : "Players can die",
                            (h->flags & ss::kAutoPublish) ? "  |  object changes published automatically" : "");
    }
    if (!sessionRoleText_.empty()) ImGui::TextWrapped("Host: %s", sessionRoleText_.c_str());

    if (!owner && session_->role() < ss::Role::Editor) {
        ImGui::SeparatorText("Edit rights");
        ImGui::SetNextItemWidth(-110);
        ImGui::InputTextWithHint("##reason", "why (optional)", requestReason_, sizeof requestReason_);
        ImGui::SameLine();
        if (ImGui::Button("Request edit")) {
            session_->requestEdit(requestReason_);
            status("Asked the host for edit rights");
        }
    }

    ImGui::SeparatorText("People");
    if (ImGui::BeginTable("##roster", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Role");
        ImGui::TableSetupColumn("Doing");
        ImGui::TableSetupColumn("Client");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const auto& r : session_->roster()) {
            ImGui::PushID(static_cast<int>(r.peer));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // (Who's editing) Editors carry their pencil (the mark over their character in the
            // game); everyone else their colour's swatch.
            const auto* ed = session_->editorByPeer(r.peer);
            const float lineH = ImGui::GetTextLineHeight();
            if (ed) {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(lineH, lineH));
                editor::pencilMark(ImGui::GetWindowDrawList(), ImVec2(at.x + lineH / 2, at.y + lineH / 2), lineH, r.peer, ed->activity == ss::Activity::Idle);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Editing the map (the pencil marks their character in the game)");
            } else {
                ImGui::ColorButton("##c", ImGui::ColorConvertU32ToFloat4(peerColour(r.peer)), ImGuiColorEditFlags_NoTooltip, ImVec2(10, 10));
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(ss::roleName(r.role));
            ImGui::TableNextColumn();
            if (ed) {
                // What they're doing, and what they're working on (their locks, as the map shows
                // them by name and colour): "editing: wall 30,40 +2".
                const std::set<me::ElementKey> held = locks_.heldBy(r.peer);
                std::string doing = editor::activityText(ed->activity);
                if (!held.empty()) {
                    doing += ": " + held.begin()->text();
                    if (held.size() > 1) doing += " +" + std::to_string(held.size() - 1);
                }
                const ImVec4 tint = ed->activity == ss::Activity::Idle ? tc.idle
                                    : ed->activity == ss::Activity::Playing ? tc.playing
                                                                            : editor::onPanel(ImGui::ColorConvertU32ToFloat4(peerColour(r.peer)));
                ImGui::TextColored(tint, "%s", doing.c_str());
                if (held.size() > 1 && ImGui::IsItemHovered()) {
                    std::string all;
                    std::size_t n = 0;
                    for (const auto& k : held) {
                        if (++n > 20) {
                            all += "\n... " + std::to_string(held.size() - 20) + " more";
                            break;
                        }
                        all += (all.empty() ? "" : "\n") + k.text();
                    }
                    ImGui::SetTooltip("%s", all.c_str());
                }
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.stock ? "stock" : "NoX Core");
            ImGui::TableNextColumn();
            if (owner && r.peer != self_) {
                if (r.role == ss::Role::Editor && ImGui::SmallButton("Revoke")) session_->revoke(r.peer);
                if (r.role == ss::Role::Viewer && ImGui::SmallButton("Let edit")) session_->answer(r.peer, 1);
                ImGui::SameLine();
            }
            // The camera on them, or our character in beside them.
            if (r.peer != self_) {
                const auto live = std::ranges::find_if(live_, [&](const LiveEntity& e) { return e.player && e.name == r.name; });
                if (live != live_.end()) {
                    const bool following = follow_ && *follow_ == live->id;
                    if (ImGui::SmallButton(following ? "Unfollow" : "Follow")) {
                        if (following) {
                            follow_.reset();
                        } else {
                            follow_ = live->id;
                        }
                    }
                    help("The camera follows this player in the game (pan or Escape stops).");
                    ImGui::SameLine();
                    ImGui::BeginDisabled(session_->playTestRunning());
                    if (ImGui::SmallButton("Jump in")) jumpInAt(*live);
                    ImGui::EndDisabled();
                    help("Your character into the game beside this player, and play (F2 comes back).");
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (owner) {
        if (!session_->requests().empty()) {
            ImGui::SeparatorText("Asking to edit");
            // (A copy: an answer takes its request off the list at once, under the loop.)
            const auto requests = session_->requests();
            for (const auto& q : requests) {
                ImGui::PushID(static_cast<int>(q.peer));
                ImGui::TextWrapped("%s%s%s", q.name.c_str(), q.reason.empty() ? "" : ": ", q.reason.c_str());
                if (ImGui::SmallButton("Approve")) session_->answer(q.peer, 1);
                ImGui::SameLine();
                if (ImGui::SmallButton("Always")) session_->answer(q.peer, 2);
                help("Approve, and let this player edit whenever they join.");
                ImGui::SameLine();
                if (ImGui::SmallButton("Deny")) session_->answer(q.peer, 0);
                ImGui::PopID();
            }
        }
        ImGui::SeparatorText("Publish");
        if (ImGui::Button("Publish (map change)")) sessionPublish();
        ImGui::SameLine();
        if (ImGui::Button("Objects only")) session_->publish(false);
        help("Write the working objects for players who join later, without a map change (the game already has them).");

        ImGui::SeparatorText("Play-test round");
        ImGui::SetNextItemWidth(160);
        if (ImGui::BeginCombo("Mode", kSessionModes[playTestModeIndex_].name)) {
            for (int i = 0; i < static_cast<int>(std::size(kSessionModes)); ++i) {
                if (ImGui::Selectable(kSessionModes[i].name, i == playTestModeIndex_)) playTestModeIndex_ = i;
            }
            ImGui::EndCombo();
        }
        ImGui::Checkbox("Players can die", &playTestCanDie_);
        if (!session_->playTestRunning()) {
            if (ImGui::Button("Start play-test")) session_->playTest(true, kSessionModes[playTestModeIndex_].bits, playTestCanDie_);
            help("Publishes what's pending, then the map's real mode for everyone; editing waits till it ends.");
        } else if (ImGui::Button("End play-test")) {
            session_->playTest(false);
        }

        ImGui::SeparatorText("Settings");
        if (h) {
            std::uint8_t flags = static_cast<std::uint8_t>(h->flags & ~ss::kPlayTestRunning);
            bool changed = false;
            int m = h->editMode == ss::EditMode::Approve ? 1 : 0;
            changed |= ImGui::RadioButton("Only me", &m, 0);
            ImGui::SameLine();
            changed |= ImGui::RadioButton("Others when I approve", &m, 1);
            const auto flag = [&](const char* label, std::uint8_t bit) {
                bool on = (flags & bit) != 0;
                if (ImGui::Checkbox(label, &on)) {
                    flags = static_cast<std::uint8_t>(on ? flags | bit : flags & ~bit);
                    changed = true;
                }
            };
            flag("Players can't die", ss::kPlayersCantDie);
            flag("Live view", ss::kLiveView);
            ImGui::SameLine();
            flag("Creatures too", ss::kLiveCreatures);
            flag("Publish objects automatically", ss::kAutoPublish);
            if (changed) session_->settings(m ? ss::EditMode::Approve : ss::EditMode::HostOnly, flags);
        }

        if (!locks_.all().empty()) {
            ImGui::SeparatorText("Locks");
            for (const auto& [k, holder] : locks_.all()) {
                ImGui::PushID(&k);
                ImGui::TextColored(editor::onPanel(ImGui::ColorConvertU32ToFloat4(peerColour(holder.peer))), "%s", session_->nameOf(holder.peer).c_str());
                ImGui::SameLine();
                ImGui::TextUnformatted(k.text().c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Force release")) session_->forceRelease(k);
                ImGui::PopID();
            }
        }
    }

    if (const auto& d = session_->lastDiag()) {
        ImGui::SeparatorText("Script");
        ImGui::TextWrapped("%s", d->summary.c_str());
        for (std::size_t i = 0; i < d->diagnostics.size() && i < 8; ++i) {
            const auto& x = d->diagnostics[i];
            ImGui::TextColored(x.error ? tc.error : tc.warning, "line %u: %s", x.line, x.text.c_str());
        }
    }
    ImGui::SeparatorText("Session log");
    const auto& lines = session_->logLines();
    const std::size_t from = lines.size() > 12 ? lines.size() - 12 : 0;
    for (std::size_t i = from; i < lines.size(); ++i) ImGui::TextWrapped("%s", lines[i].c_str());
    ImGui::End();
}

void MapEditor::requestToast()
{
    if (!session_->isOwner() || session_->requests().empty()) return;
    // Non-modal: over the map's top right; the Session panel lists them too.
    const auto& q = session_->requests().front();
    ImGui::SetNextWindowPos(ImVec2(panelX_ + panelW_ - 12, panelY_ + 12), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (ImGui::Begin("##editrequest", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        const me::PeerId who = q.peer;
        ImGui::TextColored(editor::onPanel(ImGui::ColorConvertU32ToFloat4(peerColour(who))), "%s", q.name.c_str());
        ImGui::SameLine(0, 4);
        ImGui::Text("asks to edit%s%s", q.reason.empty() ? "" : ": ", q.reason.c_str());
        const std::size_t more = session_->requests().size() - 1;
        if (ImGui::Button("Approve")) session_->answer(who, 1);
        ImGui::SameLine(0, 2);
        if (ImGui::ArrowButton("##more", ImGuiDir_Down)) ImGui::OpenPopup("##approveHow");
        if (ImGui::BeginPopup("##approveHow")) {
            if (ImGui::Selectable("For this session")) session_->answer(who, 1);
            if (ImGui::Selectable("Always for this player")) session_->answer(who, 2);
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Deny")) session_->answer(who, 0);
        if (more > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(+%zu more: Session panel)", more);
        }
    }
    ImGui::End();
}

} // namespace noxsdl
