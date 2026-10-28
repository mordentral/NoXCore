// The Sprites view (catalog_view.hpp).
#include "gui/catalog_view.hpp"

#include "gui/textures.hpp"
#include "editor/theme_colours.hpp"
#include "image_source.hpp"

#include "noxcore/things/thing_db.hpp"

#include <imgui.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <format>
#include <stdexcept>

namespace fs = std::filesystem;

namespace videotools::gui {

namespace {

constexpr double kGameFps = 30.0;      // Nox game frames per second
constexpr double kStillSeconds = 0.25; // how long playback shows each image of a still set
constexpr float kSpeeds[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
constexpr const char* kSpeedNames[] = {"0.25x", "0.5x", "1x", "2x", "4x"};

std::string describe(const Sequence& s)
{
    std::string d;
    bool even = !s.tracks.empty();
    for (const auto& t : s.tracks) {
        even = even && t.frames.size() == s.tracks.front().frames.size();
    }
    if (s.trackKind == TrackKind::Direction && even) {
        d = std::format("8 dir x {}", s.tracks.front().frames.size());
    } else if (s.tracks.size() > 1 && even) {
        d = std::format("{} x {}", s.tracks.size(), s.tracks.front().frames.size());
    } else {
        d = std::format("{} images", s.imageCount());
    }
    if (!s.kind.empty()) {
        d += "  " + s.kind;
    }
    return d;
}

} // namespace

void CatalogView::load(const nox::things::ThingDb* things, const std::string& thingsError)
{
    catalog_ = {};
    catalogError_.clear();
    list_.clear();
    pick_ = {};
    seq_ = -1;
    if (s_.bag()) {
        try {
            if (!things) {
                throw std::runtime_error(thingsError.empty() ? std::string("thing.bin isn't open") : thingsError);
            }
            catalog_ = buildCatalog(*things, s_.bag()->bag());
        } catch (const std::exception& ex) {
            catalogError_ = ex.what();
        }
    }
    listCat_ = 0;
    filterUsed_ = "\x01";
    refreshList();
}

const Entry* CatalogView::entryForThing(int thingId) const
{
    if (thingId <= 0) {
        return nullptr;
    }
    for (const auto& c : catalog_.categories) {
        for (const auto& e : c.entries) {
            if (e.thingId == thingId) {
                return &e;
            }
        }
    }
    return nullptr;
}

bool CatalogView::selectThing(int thingId)
{
    for (std::size_t c = 0; c < catalog_.categories.size(); ++c) {
        const auto& entries = catalog_.categories[c].entries;
        for (std::size_t e = 0; e < entries.size(); ++e) {
            if (entries[e].thingId == thingId) {
                listCat_ = int(c);
                filterBuf_[0] = 0;
                filterUsed_ = "\x01";
                select({int(c), int(e)});
                scrollToPick_ = true;
                return true;
            }
        }
    }
    return false;
}

bool CatalogView::selectByName(std::string_view name, std::string_view animation, std::string_view track)
{
    const Category* c = nullptr;
    const Entry* e = catalog_.findEntry(name, &c);
    if (!e) {
        return false;
    }
    listCat_ = int(c - catalog_.categories.data());
    filterBuf_[0] = 0;
    filterUsed_ = "\x01";
    select({listCat_, int(e - c->entries.data())});
    scrollToPick_ = true;
    for (std::size_t s = 0; s < e->sequences.size(); ++s) {
        if (lowered(e->sequences[s].path()) == lowered(animation)) {
            selectSequence(int(s));
            for (std::size_t t = 0; t < e->sequences[s].tracks.size(); ++t) {
                if (!track.empty() && lowered(e->sequences[s].tracks[t].name) == lowered(track)) {
                    track_ = int(t);
                }
            }
        }
    }
    return true;
}

void CatalogView::refreshList()
{
    const std::string filter = lowered(filterBuf_);
    if (filter == filterUsed_) {
        return;
    }
    filterUsed_ = filter;
    list_.clear();
    scrollListTop_ = true;
    if (filter.empty()) {
        if (listCat_ < int(catalog_.categories.size())) {
            for (std::size_t e = 0; e < catalog_.categories[std::size_t(listCat_)].entries.size(); ++e) {
                list_.push_back({listCat_, int(e)});
            }
        }
        return;
    }
    // "#123" finds the entries that use video.bag image 123.
    std::optional<std::int32_t> index;
    if (filter.size() > 1 && filter[0] == '#') {
        std::int32_t n = 0;
        if (std::from_chars(filter.data() + 1, filter.data() + filter.size(), n).ec == std::errc{}) {
            index = n;
        }
    }
    for (std::size_t c = 0; c < catalog_.categories.size(); ++c) {
        const auto& entries = catalog_.categories[c].entries;
        for (std::size_t e = 0; e < entries.size(); ++e) {
            bool match = false;
            if (index) {
                for (const auto& s : entries[e].sequences) {
                    for (const auto& t : s.tracks) {
                        match = match || std::find(t.frames.begin(), t.frames.end(), *index) != t.frames.end();
                    }
                }
            } else {
                match = lowered(entries[e].name).find(filter) != std::string::npos;
            }
            if (match) {
                list_.push_back({int(c), int(e)});
            }
        }
    }
}

void CatalogView::select(Pick p)
{
    if (p == pick_) {
        return;
    }
    pick_ = p;
    seq_ = -1;
    track_ = -1; // a new entry starts facing south
    selectSequence(0);
}

void CatalogView::selectSequence(int s)
{
    const Entry* e = entry();
    if (!e || s < 0 || std::size_t(s) >= e->sequences.size()) {
        seq_ = -1;
        return;
    }
    seq_ = s;
    frame_ = 0;
    animTime_ = 0;
    playing_ = e->sequences[std::size_t(s)].frameDelay >= 0; // animations play, still sets wait
    const auto& seq = e->sequences[std::size_t(s)];
    if (track_ < 0 || track_ >= int(seq.tracks.size()) || seq.trackKind != TrackKind::Direction) {
        track_ = 0;
        for (std::size_t t = 0; t < seq.tracks.size(); ++t) {
            if (seq.tracks[t].name == "S") {
                track_ = int(t);
            }
        }
    }
}

void CatalogView::startExport(std::string label, fs::path folder, std::size_t total,
                              std::function<std::size_t(ExportContext&)> run)
{
    s_.settings().outDir = outBuf_;
    s_.startJob(std::move(label), folder, total, [paths = s_.paths(), run = std::move(run)](ExportProgress& progress) {
        ImageSource source(paths); // (video.bag as it is on disk, without unsaved edits)
        ExportContext ctx{source, paths, &progress};
        const std::size_t written = run(ctx);
        std::string msg = std::format("{} {} images", progress.cancel ? "cancelled after" : "wrote", written);
        if (progress.empty) {
            msg += std::format(", {} empty frames", progress.empty.load());
        }
        if (progress.failed) {
            msg += std::format(", {} failed (see animation.json)", progress.failed.load());
        }
        return msg;
    });
}

void CatalogView::exportEverything()
{
    std::size_t total = 0;
    for (const auto& c : catalog_.categories) {
        total += c.imageCount();
    }
    const Catalog* cat = &catalog_;
    startExport("Exporting everything", outDir(), total, [cat, out = outDir()](ExportContext& ctx) {
        std::size_t n = 0;
        for (const auto& c : cat->categories) {
            n += exportCategory(ctx, c, out);
        }
        return n;
    });
}

void CatalogView::leftPane(float height)
{
    ImGui::BeginChild("left", ImVec2(0, height));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search all names, or #index for a bag image", filterBuf_, sizeof filterBuf_);
    const bool searching = filterBuf_[0] != 0;
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const float buttonRow = ImGui::GetFrameHeightWithSpacing();
    if (!searching) {
        const float catH = float(catalog_.categories.size()) * line + ImGui::GetStyle().WindowPadding.y * 2;
        ImGui::BeginChild("cats", ImVec2(0, std::min(catH, height * 0.45f)), ImGuiChildFlags_Borders);
        for (std::size_t c = 0; c < catalog_.categories.size(); ++c) {
            const auto& cat = catalog_.categories[c];
            if (ImGui::Selectable(std::format("{}##cat{}", cat.name, c).c_str(), listCat_ == int(c))) {
                listCat_ = int(c);
                filterUsed_ = "\x01";
            }
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 50 * s_.scale() + ImGui::GetCursorPosX() -
                            ImGui::GetStyle().ItemSpacing.x);
            ImGui::TextDisabled("%5zu", cat.entries.size());
        }
        ImGui::EndChild();
    }
    refreshList();

    ImGui::BeginChild("entries", ImVec2(0, -buttonRow), ImGuiChildFlags_Borders);
    const float rowH = std::round(40 * s_.scale());
    if (scrollListTop_) {
        ImGui::SetScrollY(0);
        scrollListTop_ = false;
    }
    if (scrollToPick_) {
        if (const auto it = std::find(list_.begin(), list_.end(), pick_); it != list_.end()) {
            const float y = float(it - list_.begin()) * (rowH + ImGui::GetStyle().ItemSpacing.y);
            ImGui::SetScrollY(std::max(0.0f, y - ImGui::GetWindowHeight() / 3));
        }
        scrollToPick_ = false;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiListClipper clipper;
    clipper.Begin(int(list_.size()), rowH + ImGui::GetStyle().ItemSpacing.y);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const Pick p = list_[std::size_t(i)];
            const auto& cat = catalog_.categories[std::size_t(p.cat)];
            const auto& e = cat.entries[std::size_t(p.entry)];
            ImGui::PushID(i);
            if (ImGui::Selectable("##row", pick_ == p, 0, ImVec2(0, rowH))) {
                select(p);
            }
            const ImVec2 a = ImGui::GetItemRectMin();
            drawFit(dl, s_.sd().get(e.icon), ImVec2(a.x + 2, a.y + 2), ImVec2(rowH - 4, rowH - 4));
            const float tx = a.x + rowH + 6 * s_.scale();
            dl->AddText(ImVec2(tx, a.y + 2), ImGui::GetColorU32(ImGuiCol_Text), e.name.c_str());
            const std::string sub = (searching ? cat.name + "  ·  " : std::string()) +
                                    std::format("{} {}, {} images", e.sequences.size(),
                                                e.sequences.size() == 1 ? "animation" : "animations", e.imageCount());
            dl->AddText(ImVec2(tx, a.y + rowH / 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str());
            ImGui::PopID();
        }
    }
    if (list_.empty()) {
        ImGui::TextDisabled(searching ? "Nothing matches." : "No entries.");
    }
    ImGui::EndChild();

    if (!searching && listCat_ < int(catalog_.categories.size())) {
        const auto& cat = catalog_.categories[std::size_t(listCat_)];
        ImGui::BeginDisabled(s_.busy());
        if (ImGui::Button(std::format("Export category ({} images)", cat.imageCount()).c_str(), ImVec2(-FLT_MIN, 0))) {
            startExport("Exporting " + cat.name, outDir() / safeName(cat.name), cat.imageCount(),
                        [c = &cat, out = outDir()](ExportContext& ctx) { return exportCategory(ctx, *c, out); });
        }
        ImGui::EndDisabled();
    } else {
        ImGui::TextDisabled("%zu matches", list_.size());
    }
    ImGui::EndChild();
}

void CatalogView::middlePane(float height)
{
    ImGui::BeginChild("middle", ImVec2(0, height));
    const Category* cat = category();
    const Entry* e = entry();
    if (!e) {
        ImGui::TextDisabled("Pick an entry on the left.");
        ImGui::EndChild();
        return;
    }
    ImGui::TextUnformatted(e->name.c_str());
    std::string facts = cat->name + "  ·  " + e->source;
    if (!e->drawType.empty()) {
        facts += "  ·  " + e->drawType;
    }
    if (e->thingId) {
        facts += std::format("  ·  type id {}", e->thingId);
    }
    ImGui::TextDisabled("%s", facts.c_str());
    ImGui::BeginDisabled(s_.busy());
    if (ImGui::Button(std::format("Export all animations ({} images)", e->imageCount()).c_str(), ImVec2(-FLT_MIN, 0))) {
        startExport("Exporting " + e->name, entryDir(outDir(), *cat, *e), e->imageCount(),
                    [c = cat, en = e, out = outDir()](ExportContext& ctx) { return exportEntry(ctx, *c, *en, out); });
    }
    ImGui::EndDisabled();
    ImGui::Separator();

    ImGui::BeginChild("sequences");
    const auto row = [&](std::size_t i, const std::string& label) {
        const auto& s = e->sequences[i];
        ImGui::PushID(int(i));
        if (ImGui::Selectable(label.c_str(), seq_ == int(i))) {
            selectSequence(int(i));
        }
        if (ImGui::BeginPopupContextItem()) {
            ImGui::BeginDisabled(s_.busy());
            if (ImGui::MenuItem("Export this animation")) {
                startExport("Exporting " + e->name + "/" + s.path(), sequenceDir(outDir(), *cat, *e, s), s.imageCount(),
                            [c = cat, en = e, sq = &s, out = outDir()](ExportContext& ctx) {
                                return exportSequence(ctx, *c, *en, *sq, out);
                            });
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        const std::string d = describe(s);
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(d.c_str()).x);
        ImGui::TextDisabled("%s", d.c_str());
        ImGui::PopID();
    };
    for (std::size_t i = 0; i < e->sequences.size();) {
        const auto& group = e->sequences[i].group;
        std::size_t end = i + 1;
        while (end < e->sequences.size() && e->sequences[end].group == group) {
            ++end;
        }
        if (group.empty()) {
            for (; i < end; ++i) {
                row(i, e->sequences[i].name);
            }
            continue;
        }
        const bool holdsSelection = seq_ >= int(i) && seq_ < int(end);
        ImGui::SetNextItemOpen(holdsSelection, ImGuiCond_Appearing);
        if (ImGui::TreeNodeEx(std::format("{}  ({})", group, end - i).c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
            for (std::size_t k = i; k < end; ++k) {
                row(k, e->sequences[k].name);
            }
            ImGui::TreePop();
        }
        i = end;
    }
    if (!e->attributes.empty()) {
        ImGui::Spacing();
        if (ImGui::TreeNode("Thing attributes")) {
            for (const auto& a : e->attributes) {
                ImGui::TextWrapped("%s", a.c_str());
            }
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

std::optional<Registration> CatalogView::trackRegistration(const Track& track)
{
    std::vector<FrameGeometry> geometry;
    for (const auto f : track.frames) {
        const Tex* t = s_.sd().get(f);
        if (!t) {
            if (f >= 0) {
                return std::nullopt; // still loading
            }
            geometry.push_back({});
            continue;
        }
        geometry.push_back({t->offsetX, t->offsetY, t->texture ? t->width : 0, t->texture ? t->height : 0});
    }
    try {
        return registerFrames(geometry);
    } catch (const std::exception&) {
        return std::nullopt; // every frame empty
    }
}

void CatalogView::canvas(const Track& track, ImVec2 size)
{
    ImGui::BeginChild("canvas", size, 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    // Frames go on the track's registered canvas (common/registration.hpp), so the animation
    // doesn't jump as crop sizes change. Both panes use that canvas, so they show the same frame
    // at the same size; a frame's HD image goes over the same logical rectangle (as noxsdl draws
    // it), moved by any difference between its draw offset and the original's.
    const auto reg = trackRegistration(track);
    const int n = int(track.frames.size());
    const std::int32_t index = frame_ < n ? track.frames[std::size_t(frame_)] : -1;
    const Tex* t = s_.sd().get(index);
    const bool fill = compare_.fillFromBag();
    // A frame's HD image, or (Fill from video.bag) its video.bag image where the pack has none.
    const auto hdFor = [&](std::int32_t i, bool& standIn) -> const Tex* {
        standIn = false;
        const Tex* h = s_.packOpen() && i >= 0 ? s_.hd().get(i) : nullptr;
        if (h && h->missing && fill) {
            standIn = true;
            return s_.sd().get(i);
        }
        return h;
    };

    PaneView bag;
    bag.title = "video.bag";
    bag.image = t;
    PaneView hd;
    hd.title = "HD (image pack)";
    hd.hd = true;
    hd.image = hdFor(index, hd.standIn);
    std::string error;
    if (index < 0 && n > 0) {
        bag.note = hd.note = "This frame's image reference doesn't resolve.";
    } else if (t && !t->error.empty()) {
        error = std::format("Can't decode #{}: {}", index, t->error);
        bag.note = error.c_str();
    }
    const bool placed = t && t->texture && reg;
    if (t && t->texture && !reg) {
        bag.image = hd.image = nullptr; // still loading the track's other frames
    }
    if (placed) {
        const IRect r = reg->canvasRect(std::size_t(frame_));
        bag.canvasWidth = hd.canvasWidth = reg->width;
        bag.canvasHeight = hd.canvasHeight = reg->height;
        bag.x = float(r.x);
        bag.y = float(r.y);
        hd.x = float(r.x);
        hd.y = float(r.y);
        if (hd.image && !hd.standIn) {
            hd.x += float(hd.image->offsetX - t->offsetX);
            hd.y += float(hd.image->offsetY - t->offsetY);
        }
        const auto screen = [](ImVec2 o, float z, const IRect& rc) {
            return std::pair(ImVec2(o.x + float(rc.x) * z, o.y + float(rc.y) * z),
                             ImVec2(o.x + float(rc.x + rc.w) * z, o.y + float(rc.y + rc.h) * z));
        };
        if (onionSkin_ && n > 1) {
            // The frames before and after, faint, each pane from its own images.
            const auto ghosts = [&, screen](bool hdPane) {
                return [&, screen, hdPane](ImDrawList* dl, ImVec2 o, float z) {
                    const std::pair<int, ImU32> frames[] = {{(frame_ + n - 1) % n, IM_COL32(255, 120, 120, 90)},
                                                            {(frame_ + 1) % n, IM_COL32(120, 255, 120, 90)}};
                    for (const auto& [g, colour] : frames) {
                        const std::int32_t gi = track.frames[std::size_t(g)];
                        const Tex* gs = s_.sd().get(gi);
                        if (!gs) {
                            continue;
                        }
                        auto [ga, gb] = screen(o, z, reg->canvasRect(std::size_t(g)));
                        const Tex* gt = gs;
                        if (hdPane) {
                            bool standIn = false;
                            gt = hdFor(gi, standIn);
                            if (gt && gt->texture && !standIn) {
                                ga = ImVec2(ga.x + float(gt->offsetX - gs->offsetX) * z, ga.y + float(gt->offsetY - gs->offsetY) * z);
                                gb = ImVec2(ga.x + float(gt->width) * z, ga.y + float(gt->height) * z);
                            }
                        }
                        if (!gt || !gt->texture) {
                            continue;
                        }
                        const bool nearest = gt == gs;
                        if (nearest) {
                            dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
                        }
                        dl->AddImage(texRef(gt->texture), ga, gb, ImVec2(0, 0), ImVec2(1, 1), colour);
                        if (nearest) {
                            dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);
                        }
                    }
                };
            };
            bag.under = ghosts(false);
            hd.under = ghosts(true);
        }
        if (showRegistration_) {
            bag.over = hd.over = [&, screen, r](ImDrawList* dl, ImVec2 o, float z) {
                const auto [ca, cb] = screen(o, z, {0, 0, reg->width, reg->height});
                dl->AddRect(ca, cb, IM_COL32(200, 200, 210, 200));
                const auto [ra, rb] = screen(o, z, reg->contentRect());
                dl->AddRect(ra, rb, IM_COL32(255, 200, 0, 160));
                const auto [a, b] = screen(o, z, r);
                dl->AddRect(a, b, IM_COL32(0, 220, 230, 255));
            };
        }
    }
    const bool regText = showRegistration_ && placed;
    const float textH = regText ? ImGui::GetTextLineHeightWithSpacing() * 2 : 0.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    compare_.draw(bag, hd, ImVec2(avail.x, std::max(1.0f, avail.y - textH)));
    if (regText) {
        const IRect r = reg->canvasRect(std::size_t(frame_));
        ImGui::TextDisabled("canvas %dx%d  origin (%d, %d)  crop %dx%d at %d,%d (offset %d, %d)", reg->width, reg->height,
                            reg->originX, reg->originY, t->width, t->height, r.x, r.y, t->offsetX, t->offsetY);
        ImGui::TextDisabled("grey: canvas (padding %d)   orange: all frames   cyan: this frame's crop", reg->options.padding);
    }
    ImGui::EndChild();
}

void CatalogView::frameStrip(const Track& track, float height)
{
    ImGui::BeginChild("strip", ImVec2(0, height), ImGuiChildFlags_Borders);
    const float cell = std::round(64 * s_.scale());
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const int cols = std::max(1, int((ImGui::GetContentRegionAvail().x + gap) / (cell + gap)));
    const int n = int(track.frames.size());
    const int rows = (n + cols - 1) / cols;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiListClipper clipper;
    clipper.Begin(rows, cell + ImGui::GetStyle().ItemSpacing.y);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            for (int c = 0; c < cols; ++c) {
                const int f = r * cols + c;
                if (f >= n) {
                    break;
                }
                if (c) {
                    ImGui::SameLine();
                }
                ImGui::PushID(f);
                if (ImGui::InvisibleButton("##f", ImVec2(cell, cell))) {
                    frame_ = f;
                    playing_ = false;
                }
                const ImVec2 a = ImGui::GetItemRectMin();
                const ImVec2 b = ImGui::GetItemRectMax();
                dl->AddRectFilled(a, b, ImGui::GetColorU32(f == frame_ ? ImGuiCol_HeaderActive
                                                                    : ImGui::IsItemHovered() ? ImGuiCol_HeaderHovered
                                                                                             : ImGuiCol_FrameBg));
                drawFit(dl, s_.sd().get(track.frames[std::size_t(f)]), ImVec2(a.x + 3, a.y + 3), ImVec2(cell - 6, cell - 6),
                        compare_.tint());
                dl->AddText(ImVec2(a.x + 3, a.y + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), std::to_string(f).c_str());
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
}

void CatalogView::frameInfo(const Sequence& seq, const Track& track)
{
    ImGui::BeginChild("info", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (frame_ >= int(track.frames.size())) {
        ImGui::EndChild();
        return;
    }
    const std::int32_t index = track.frames[std::size_t(frame_)];
    if (ImGui::BeginTable("meta", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        const auto rowText = [](const char* k, const std::string& v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", k);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(v.c_str());
        };
        const Tex* tex = s_.sd().get(index);
        rowText("Exports as", tex && tex->empty ? "nothing (empty frame, listed in animation.json)"
                                                : frameFileName(seq, track, std::size_t(frame_)));
        rowText("Frame", std::format("{}{} of {}", track.name.empty() ? "" : track.name + " ", frame_, track.frames.size()));
        if (index >= 0 && s_.bag()) {
            const auto& info = s_.bag()->bag().images()[std::size_t(index)];
            rowText("Bag index", std::format("#{}{}", index, s_.bag()->edited(index) ? "  (edited)" : ""));
            rowText("Bag name", info.name);
            rowText("Image type", std::format("{} ({})", info.type, imageTypeName(info.type)));
            if (tex && tex->error.empty()) {
                rowText("Size", std::format("{} x {}", tex->width, tex->height));
                rowText("Draw offset", std::format("{}, {}", tex->offsetX, tex->offsetY));
                rowText("Header byte", std::to_string(tex->headerByte));
                std::string slots;
                for (const auto s : tex->materialSlots) {
                    slots += (slots.empty() ? "" : ", ") + std::to_string(s);
                }
                rowText("Material slots", slots.empty() ? "none" : slots + "  (grey pixels the game tints)");
                if (tex->edgeMask) {
                    rowText("Edge mask", "yes (exported to masks/)");
                }
            } else if (tex) {
                rowText("Error", tex->error);
            }
            if (s_.pack()) {
                const auto* e = s_.pack()->entry(std::uint32_t(index));
                rowText("HD image", e ? std::format("{} x {}", e->pixelWidth, e->pixelHeight) : std::string("none"));
            }
        } else {
            rowText("Bag index", "unresolved");
        }
        if (std::size_t(frame_) < track.drawOffsets.size()) {
            const auto& o = track.drawOffsets[std::size_t(frame_)];
            rowText("Wall offset", std::format("{}, {}", o[0], o[1]));
        }
        ImGui::EndTable();
    }
    if (index >= 0) {
        if (ImGui::SmallButton("Find every use of this image")) {
            std::snprintf(filterBuf_, sizeof filterBuf_, "#%d", index);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Edit in Images")) {
            openInEditor_(index);
        }
    }
    ImGui::EndChild();
}

void CatalogView::previewPane(float height)
{
    ImGui::BeginChild("preview", ImVec2(0, height));
    const Category* cat = category();
    const Entry* e = entry();
    const Sequence* seq = sequence();
    if (!seq || seq->tracks.empty()) {
        ImGui::TextDisabled(e ? "Pick an animation." : "");
        ImGui::EndChild();
        return;
    }
    track_ = std::clamp(track_, 0, std::max(0, int(seq->tracks.size()) - 1));
    const Track& track = seq->tracks[std::size_t(track_)];
    const int frames = int(track.frames.size());
    frame_ = frames ? std::clamp(frame_, 0, frames - 1) : 0;

    ImGui::Text("%s / %s", e->name.c_str(), seq->path().c_str());
    std::string facts;
    if (!seq->kind.empty()) {
        facts = seq->kind + std::format("  ·  {} game frames per frame ({:.1f} fps)", seq->frameDelay + 1,
                                        kGameFps / (seq->frameDelay + 1));
    } else {
        facts = "still images";
    }
    if (seq->state >= 0) {
        facts += std::format("  ·  state {}", seq->state);
    }
    if (!seq->sound.empty()) {
        facts += "  ·  sound " + seq->sound;
    }
    for (const auto& [k, v] : seq->info) {
        facts += "  ·  " + k + " " + v;
    }
    ImGui::TextDisabled("%s", facts.c_str());

    auto& settings = s_.settings();
    if (ImGui::Button(playing_ ? "Pause" : "Play", ImVec2(60 * s_.scale(), 0))) {
        playing_ = !playing_;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220 * s_.scale());
    if (ImGui::SliderInt("##frame", &frame_, 0, std::max(0, frames - 1), "frame %d")) {
        playing_ = false;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70 * s_.scale());
    ImGui::Combo("##speed", &settings.speed, kSpeedNames, int(std::size(kSpeedNames)));
    ImGui::SameLine();
    ImGui::Checkbox("Registration", &showRegistration_);
    ImGui::SameLine();
    ImGui::Checkbox("Onion skin", &onionSkin_);
    ImGui::SameLine();
    ImGui::BeginDisabled(s_.busy());
    if (ImGui::Button("Export this animation")) {
        startExport("Exporting " + e->name + "/" + seq->path(), sequenceDir(outDir(), *cat, *e, *seq), seq->imageCount(),
                    [c = cat, en = e, sq = seq, out = outDir()](ExportContext& ctx) {
                        return exportSequence(ctx, *c, *en, *sq, out);
                    });
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", utf8(sequenceDir(outDir(), *cat, *e, *seq)).c_str());
    compare_.controls(true);

    const float canvasH = std::max(160 * s_.scale(), (ImGui::GetContentRegionAvail().y) * 0.58f);
    if (seq->tracks.size() > 1) {
        ImGui::BeginGroup();
        if (seq->trackKind == TrackKind::Direction && seq->tracks.size() == 8) {
            const float b = 38 * s_.scale();
            for (int cell = 0; cell < 9; ++cell) {
                if (cell % 3) {
                    ImGui::SameLine();
                }
                if (cell == 4) {
                    ImGui::Dummy(ImVec2(b, b));
                    continue;
                }
                const int t = cell < 4 ? cell : cell - 1;
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(t == track_ ? ImGuiCol_ButtonActive : ImGuiCol_Button));
                if (ImGui::Button(seq->tracks[std::size_t(t)].name.c_str(), ImVec2(b, b))) {
                    track_ = t;
                }
                ImGui::PopStyleColor();
            }
        } else {
            ImGui::BeginChild("tracks", ImVec2(110 * s_.scale(), canvasH), ImGuiChildFlags_Borders);
            for (std::size_t t = 0; t < seq->tracks.size(); ++t) {
                if (ImGui::Selectable(seq->tracks[t].name.c_str(), int(t) == track_)) {
                    track_ = int(t);
                }
            }
            ImGui::EndChild();
        }
        ImGui::EndGroup();
        ImGui::SameLine();
    }
    canvas(track, ImVec2(0, canvasH));

    const float lower = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginTable("lower", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0, lower))) {
        ImGui::TableSetupColumn("frames", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableSetupColumn("info", ImGuiTableColumnFlags_WidthStretch, 0.4f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        frameStrip(track, lower - 4);
        ImGui::TableNextColumn();
        frameInfo(*seq, track);
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void CatalogView::handleKeys(const Sequence* seq)
{
    if (!seq || seq->tracks.empty() || ImGui::GetIO().WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        return;
    }
    compare_.keys();
    const int frames = int(seq->tracks[std::size_t(track_)].frames.size());
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        playing_ = !playing_;
    }
    for (const auto [key, dir] : {std::pair(ImGuiKey_RightArrow, 1), std::pair(ImGuiKey_LeftArrow, -1)}) {
        if (frames && ImGui::IsKeyPressed(key)) {
            frame_ = (frame_ + dir + frames) % frames;
            animTime_ = frame_;
            playing_ = false;
        }
    }
}

void CatalogView::advance(double dt)
{
    const Sequence* seq = sequence();
    if (!seq || seq->tracks.empty()) {
        return;
    }
    const int frames = int(seq->tracks[std::size_t(std::clamp(track_, 0, int(seq->tracks.size()) - 1))].frames.size());
    if (frames < 1) {
        return;
    }
    if (frame_ != int(animTime_)) {
        animTime_ = frame_; // moved by hand
    }
    if (playing_ && frames >= 2) {
        const double perFrame = (seq->frameDelay >= 0 ? (seq->frameDelay + 1) / kGameFps : kStillSeconds) /
                                kSpeeds[s_.settings().speed];
        animTime_ = std::fmod(animTime_ + std::min(dt, 0.25) / perFrame, double(frames));
    }
    frame_ = std::clamp(int(animTime_), 0, frames - 1);
}

void CatalogView::draw(float height, double dt)
{
    if (!catalogError_.empty()) {
        ImGui::TextColored(noxsdl::editor::themeColours().error, "No catalog (thing.bin): %s", catalogError_.c_str());
        return;
    }
    if (keys_) {
        handleKeys(sequence());
    }
    advance(dt);
    if (ImGui::BeginTable("panes", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0, height))) {
        ImGui::TableSetupColumn("Browse", ImGuiTableColumnFlags_WidthFixed, 320 * s_.scale());
        ImGui::TableSetupColumn("Animations", ImGuiTableColumnFlags_WidthFixed, 300 * s_.scale());
        ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        leftPane(height);
        ImGui::TableNextColumn();
        middlePane(height);
        ImGui::TableNextColumn();
        previewPane(height);
        ImGui::EndTable();
    }
}

} // namespace videotools::gui
