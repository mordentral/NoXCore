// The preview and the references (panels.hpp).
#include "gui/panels.hpp"

#include "gui/textures.hpp"

#include "editor/theme_colours.hpp"

#include "noxcore/audio/pcm.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/util/text.hpp"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

namespace fs = std::filesystem;

namespace videotools::gui {

using content::RecordList;

namespace {

constexpr double kGameFps = 30.0;

/// The sequence a thing's preview starts with: IDLE if it has one, else the first.
int startSequence(const Entry& e)
{
    for (std::size_t i = 0; i < e.sequences.size(); ++i) {
        if (e.sequences[i].name == "IDLE" || e.sequences[i].path() == "IDLE/NAKED") {
            return int(i);
        }
    }
    return 0;
}

int southTrack(const Sequence& s)
{
    for (std::size_t t = 0; t < s.tracks.size(); ++t) {
        if (s.tracks[t].name == "S") {
            return int(t);
        }
    }
    return 0;
}

} // namespace

void PreviewPanel::refresh()
{
    shown_ = ws_.selected;
    info_ = {};
    entry_ = nullptr;
    thingIndex_ = -1;
    sequence_ = track_ = 0;
    time_ = 0;
    playing_ = true;
    if (!shown_) {
        return;
    }
    info_ = content::previewOf(ws_.files, *shown_);
    // A thing's animations; a monster's, sound set's or weapon's, its thing's of the same name.
    std::optional<content::RecordRef> thing;
    if (shown_->list == RecordList::Thing) {
        thing = shown_;
    } else if (shown_->list == RecordList::Monster || shown_->list == RecordList::SoundSet || shown_->list == RecordList::Weapon ||
               shown_->list == RecordList::Armor) {
        if (const content::Record* r = ws_.index.find(*shown_)) {
            thing = ws_.index.findByName(RecordList::Thing, r->name);
        }
    }
    // A creature's voice: its sound set's sounds (the set named as its type).
    events_.clear();
    if (const content::Record* self = ws_.index.find(*shown_); self && ws_.files.soundSets() &&
                                                                (shown_->list == RecordList::SoundSet || shown_->list == RecordList::Monster ||
                                                                 shown_->list == RecordList::Thing)) {
        if (const auto* set = ws_.files.soundSets()->find(self->name)) {
            for (std::size_t slot = 1; slot < set->sounds.size(); ++slot) {
                if (const std::uint32_t id = set->sounds[slot]) {
                    const std::string n = content::soundLabel(id);
                    if (std::ranges::find(events_, n) == events_.end()) {
                        events_.push_back(n);
                    }
                }
            }
        }
    }
    if (thing) {
        thingIndex_ = thing->index;
        // A pack's new type: drawn as the type of thing.bin's it's made like (its DRAW is that one's).
        if (const auto* base = ws_.pack.baseThings(); base && std::size_t(thingIndex_) >= base->things.size()) {
            const auto* work = ws_.pack.workingThings();
            std::string like = work && std::size_t(thingIndex_) < work->things.size() ? work->things[std::size_t(thingIndex_)].name : std::string();
            for (std::size_t hops = 0; hops <= ws_.pack.newThings().size() && !like.empty() && !base->findThing(like); ++hops) {
                const auto it = std::ranges::find_if(ws_.pack.newThings(), [&](const content::NewThing& n) { return n.name == like; });
                like = it != ws_.pack.newThings().end() ? it->like : std::string();
            }
            const auto* t = like.empty() ? nullptr : base->findThing(like);
            thingIndex_ = t ? static_cast<std::int32_t>(t - base->things.data()) : -1;
        }
        entry_ = thingIndex_ >= 0 ? ws_.catalog.entryForThing(thingIndex_ + 1) : nullptr;
        if (entry_ && !entry_->sequences.empty()) {
            sequence_ = startSequence(*entry_);
            track_ = southTrack(entry_->sequences[std::size_t(sequence_)]);
        }
    }
}

void playSample(Workspace& ws, const std::string& sample)
{
    auto* bag = ws.files.audio();
    if (!bag) {
        ws.session.setStatus("No Audio.bag is open.");
        return;
    }
    const auto i = bag->find(sample);
    if (!i) {
        ws.session.setStatus(std::format("Audio.bag has no sample named {}.", sample));
        return;
    }
    try {
        if (!ws.audio.play(bag->decode(*i), sample)) {
            ws.session.setStatus("Can't play it: " + ws.audio.error());
        }
    } catch (const std::exception& ex) {
        ws.session.setStatus(std::format("Can't decode {}: {}", sample, ex.what()));
    }
}

void playSoundEvent(Workspace& ws, std::string_view name)
{
    const auto* db = ws.files.things();
    auto* bag = ws.files.audio();
    if (!db || !bag) {
        ws.session.setStatus("No thing.bin or Audio.bag to play it from.");
        return;
    }
    const std::vector<std::string>* files = nullptr;
    for (const auto& a : db->audio) {
        if (nox::util::equalsNoCase(a.name, name)) {
            files = &a.files;
            break;
        }
    }
    if (!files) {
        for (const auto& a : db->avnts) {
            if (nox::util::equalsNoCase(a.name, name) && a.files) {
                files = &*a.files;
                break;
            }
        }
    }
    if (!files || files->empty()) {
        ws.session.setStatus(std::format("thing.bin has no sound files for {}.", name));
        return;
    }
    // (the game picks one at random each time; the preview plays them in turn)
    static std::size_t turn = 0;
    const std::string file = fs::path((*files)[turn++ % files->size()]).stem().string();
    const auto i = bag->find(file);
    if (!i) {
        ws.session.setStatus(std::format("Audio.bag has no sample named {}.", file));
        return;
    }
    try {
        if (!ws.audio.play(bag->decode(*i), std::string(name) + ": " + file)) {
            ws.session.setStatus("Can't play it: " + ws.audio.error());
        }
    } catch (const std::exception& ex) {
        ws.session.setStatus(std::format("Can't decode {}: {}", file, ex.what()));
    }
}

void playSoundFile(Workspace& ws, const fs::path& file)
{
    try {
        if (!ws.audio.play(nox::audio::decodeFile(nox::readFileBytes(file)), file.filename().string())) {
            ws.session.setStatus("Can't play it: " + ws.audio.error());
        }
    } catch (const std::exception& ex) {
        ws.session.setStatus(std::format("Can't decode {}: {}", file.filename().string(), ex.what()));
    }
}

void playingBar(Workspace& ws)
{
    if (!ws.audio.playing()) {
        return;
    }
    ImGui::PushID("playing");
    const float stopW = ImGui::CalcTextSize("Stop").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
    ImGui::ProgressBar(float(ws.audio.position() / std::max(0.001, ws.audio.length())), ImVec2(-stopW, 0), ws.audio.label().c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Stop")) {
        ws.audio.stop();
    }
    ImGui::PopID();
}

void PreviewPanel::draw(double dt)
{
    if (shown_ != ws_.selected) {
        refresh();
    }
    if (!shown_) {
        ImGui::TextDisabled("Nothing selected.");
        return;
    }
    const float scale = ImGui::GetFontSize() / 15.0f;
    // The frames: a thing's animation from the Sprites view, else the record's own images.
    const std::vector<std::int32_t>* frames = &info_.frames;
    int delay = info_.frameDelay;
    if (entry_ && !entry_->sequences.empty()) {
        const Sequence& seq = entry_->sequences[std::size_t(std::clamp(sequence_, 0, int(entry_->sequences.size()) - 1))];
        if (!seq.tracks.empty()) {
            frames = &seq.tracks[std::size_t(std::clamp(track_, 0, int(seq.tracks.size()) - 1))].frames;
        }
        delay = std::max(0, seq.frameDelay);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##seq", seq.path().c_str())) {
            for (std::size_t i = 0; i < entry_->sequences.size(); ++i) {
                if (ImGui::Selectable(entry_->sequences[i].path().c_str(), int(i) == sequence_)) {
                    sequence_ = int(i);
                    track_ = southTrack(entry_->sequences[i]);
                    time_ = 0;
                }
            }
            ImGui::EndCombo();
        }
        if (seq.tracks.size() > 1) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            const std::string& tn = seq.tracks[std::size_t(std::clamp(track_, 0, int(seq.tracks.size()) - 1))].name;
            if (ImGui::BeginCombo("##track", tn.c_str())) {
                for (std::size_t t = 0; t < seq.tracks.size(); ++t) {
                    if (ImGui::Selectable(seq.tracks[t].name.c_str(), int(t) == track_)) {
                        track_ = int(t);
                    }
                }
                ImGui::EndCombo();
            }
        }
    }
    if (!frames->empty()) {
        const int n = int(frames->size());
        if (playing_ && n > 1) {
            time_ += std::min(dt, 0.25) * kGameFps / double(delay + 1);
        }
        const int f = int(std::fmod(time_, double(n)));
        const float side = std::max(64.0f * scale, std::min(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y * 0.6f));
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        drawBackground(dl, p0, ImVec2(p0.x + side, p0.y + side), ws_.session.settings().background, scale);
        drawFit(dl, ws_.session.sd().get((*frames)[std::size_t(f)]), ImVec2(p0.x + 4, p0.y + 4), ImVec2(side - 8, side - 8));
        ImGui::Dummy(ImVec2(side, side));
        if (n > 1) {
            if (ImGui::SmallButton(playing_ ? "Pause" : "Play")) {
                playing_ = !playing_;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("frame %d of %d  ·  #%d", f + 1, n, (*frames)[std::size_t(f)]);
        } else {
            ImGui::TextDisabled("video.bag #%d", (*frames)[0]);
        }
        if (entry_ && ImGui::SmallButton("Open in Sprites")) {
            ws_.catalog.selectThing(thingIndex_ + 1);
            ws_.focus(panel::kSprites);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Edit image")) {
            ws_.images.show((*frames)[std::size_t(f)]);
            ws_.focus(panel::kImages);
        }
    }
    // Sounds.
    if (!info_.sounds.empty()) {
        ImGui::SeparatorText("Sounds");
        for (const auto& s : info_.sounds) {
            ImGui::PushID(s.c_str());
            if (ImGui::SmallButton("Play")) {
                playSample(ws_, s);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(s.c_str());
            ImGui::PopID();
        }
    }
    if (!events_.empty()) {
        ImGui::SeparatorText("Its sound set");
        for (const auto& e : events_) {
            ImGui::PushID(e.c_str());
            if (ImGui::SmallButton("Play")) {
                playSoundEvent(ws_, e);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(e.c_str());
            ImGui::PopID();
        }
    }
    if (shown_->list == RecordList::File && shown_->kind == content::FileKind::Dialog) {
        if (const fs::path* p = recordFile(ws_.files, *shown_)) {
            if (ImGui::Button("Play")) {
                playSoundFile(ws_, *p);
            }
        }
    }
    playingBar(ws_);
    if (!info_.text.empty() || shown_->list == RecordList::String) {
        // As the game shows it, with the pack's change (each variant: the game picks one).
        std::vector<std::string> texts;
        const content::Record* self = ws_.index.find(*shown_);
        if (const auto* doc = ws_.pack.workingStrings(); doc && self && shown_->list == RecordList::String) {
            if (const auto* e = doc->find(self->name)) {
                for (const auto& v : e->variants) {
                    texts.push_back(nox::strings::toUtf8(v.shown()));
                }
            }
        } else {
            texts.push_back(info_.text);
        }
        for (std::size_t i = 0; i < texts.size(); ++i) {
            ImGui::SeparatorText(texts.size() == 1 ? "Text" : std::format("Variant {}", i + 1).c_str());
            ImGui::TextWrapped("%s", texts[i].c_str());
        }
    }
}

void ReferencesPanel::draw()
{
    if (shown_ != ws_.selected) {
        shown_ = ws_.selected;
        refs_ = shown_ ? content::references(ws_.files, ws_.index, *shown_) : std::vector<content::Reference>{};
    }
    if (!shown_) {
        return;
    }
    const auto& theme = noxsdl::editor::themeColours();
    // The maps a type (or a monster's type) is used in.
    if (shown_->list == RecordList::Thing || shown_->list == RecordList::Monster) {
        const content::Record* self = ws_.index.find(*shown_);
        if (!ws_.mapUsage.valid()) {
        } else if (ws_.mapUsage.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ImGui::TextDisabled("maps: looking through them...");
        } else if (self) {
            const content::MapUsage& usage = ws_.mapUsage.get();
            const auto* maps = usage.mapsUsing(self->name);
            if (!maps) {
                ImGui::TextDisabled("used in no map");
            } else if (ImGui::TreeNodeEx("maps", ImGuiTreeNodeFlags_SpanAvailWidth, "used in %zu map%s", maps->size(), maps->size() == 1 ? "" : "s")) {
                for (const std::size_t m : *maps) {
                    ImGui::BulletText("%s", usage.maps()[m].c_str());
                }
                ImGui::TreePop();
            }
        }
    }
    if (refs_.empty()) {
        ImGui::TextDisabled("No links found.");
        return;
    }
    for (std::size_t i = 0; i < refs_.size(); ++i) {
        const auto& r = refs_[i];
        ImGui::PushID(int(i));
        if (r.target) {
            if (ImGui::Selectable(r.text.c_str())) {
                ws_.select(*r.target);
            }
        } else {
            ImGui::TextColored(theme.unknown, "%s", r.text.c_str());
            ImGui::SetItemTooltip("Not found in the open files.");
        }
        ImGui::PopID();
    }
}

} // namespace videotools::gui
