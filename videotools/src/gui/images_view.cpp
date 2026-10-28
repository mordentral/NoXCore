#include "gui/images_view.hpp"

#include "editor/insert.hpp"

#include "noxcore/util/text.hpp"
#include "noxcore/video/sprite_encode.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace fs = std::filesystem;

namespace videotools::gui {

namespace {

constexpr SDL_DialogFileFilter kPngFilter[] = {{"PNG image", "png"}};

const char* const kTypeItems[] = {"All types", "0 floor tile", "1 tile edge", "2 full image", "3 sprite", "4 sprite",
                                  "5 sprite (RGBA4444)", "6 sprite", "7 full image", "8 sprite (RGB565)"};
const char* const kHdItems[] = {"With or without HD", "With an HD image", "Without an HD image"};

std::string lowered(std::string_view s) { return nox::util::lowered(s); }

} // namespace

void ImagesView::show(std::int32_t index)
{
    selected_ = {index};
    focus_ = anchor_ = index;
    if (std::find(list_.begin(), list_.end(), index) == list_.end()) {
        filter_[0] = 0;
        typeFilter_ = -1;
        hdFilter_ = 0;
        editedOnly_ = false;
        refresh();
    }
    scrollToFocus_ = true;
}

std::vector<std::int32_t> ImagesView::selection() const
{
    return {selected_.begin(), selected_.end()};
}

void ImagesView::rebuild()
{
    const std::size_t edits = (s_.bag() ? s_.bag()->editCount() : 0) + (s_.pack() ? s_.pack()->editCount() : 0);
    const bool editsMatter = editedOnly_ || hdFilter_ != 0;
    const std::string key = std::format("{}|{}|{}|{}|{}", filter_, typeFilter_, hdFilter_, editedOnly_,
                                        editsMatter ? edits : 0);
    if (key == filterUsed_ && edits == knownEdits_) {
        return;
    }
    knownEdits_ = edits;
    if (key == filterUsed_) {
        return;
    }
    filterUsed_ = key;
    list_.clear();
    auto* bag = s_.bag();
    auto* pack = s_.pack();
    const std::int32_t count = std::int32_t(s_.imageCount());
    std::vector<std::int32_t> candidates;
    const std::string text = lowered(filter_);
    bool byIndex = false;
    if (!text.empty() && (text[0] == '#' || std::isdigit(static_cast<unsigned char>(text[0])))) {
        try {
            candidates = editor::parseIndexList(text, count);
            byIndex = true;
        } catch (const std::exception&) {
        }
    }
    if (!byIndex) {
        candidates.resize(std::size_t(count));
        for (std::int32_t i = 0; i < count; ++i) {
            candidates[std::size_t(i)] = i;
        }
    }
    for (const auto i : candidates) {
        const bool inBag = bag && std::size_t(i) < bag->size();
        if (!byIndex && !text.empty()) {
            const std::string name = inBag ? bag->bag().images()[std::size_t(i)].name : (pack ? pack->name(std::uint32_t(i)) : "");
            if (lowered(name).find(text) == std::string::npos) {
                continue;
            }
        }
        if (typeFilter_ >= 0) {
            const int type = inBag ? bag->bag().images()[std::size_t(i)].type : -2;
            if (type != typeFilter_) {
                continue;
            }
        }
        const bool hasHd = pack && pack->present(std::uint32_t(i));
        if ((hdFilter_ == 1 && !hasHd) || (hdFilter_ == 2 && hasHd)) {
            continue;
        }
        if (editedOnly_ && !((bag && bag->edited(i)) || (pack && pack->edited(std::uint32_t(i))))) {
            continue;
        }
        list_.push_back(i);
    }
}

void ImagesView::draw(float height)
{
    pollDialogs();
    rebuild();
    if (!s_.bagOpen()) {
        ImGui::TextDisabled("Open a game folder (with video.bag and video.idx) above.");
        return;
    }
    if (ImGui::BeginTable("imagesPanes", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0, height))) {
        ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthFixed, 380 * s_.scale());
        ImGui::TableSetupColumn("detail", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        listPane(height);
        ImGui::TableNextColumn();
        detailPane(height);
        ImGui::EndTable();
    }
}

void ImagesView::listPane(float height)
{
    ImGui::BeginChild("imageList", ImVec2(0, height));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", "Name, or #index / 100-200,305", filter_, sizeof filter_);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
    int typeItem = typeFilter_ + 1;
    if (ImGui::Combo("##type", &typeItem, kTypeItems, int(std::size(kTypeItems)))) {
        typeFilter_ = typeItem - 1;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::BeginDisabled(!s_.packOpen());
    ImGui::Combo("##hd", &hdFilter_, kHdItems, int(std::size(kHdItems)));
    ImGui::EndDisabled();
    ImGui::Checkbox("Edited only", &editedOnly_);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu images, %zu selected", list_.size(), selected_.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Select all")) {
        selected_ = {list_.begin(), list_.end()};
    }
    rebuild();

    auto* bag = s_.bag();
    auto* pack = s_.pack();
    const float rowH = std::round(40 * s_.scale());
    const float step = rowH + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("rows", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (scrollToFocus_) {
        if (const auto it = std::find(list_.begin(), list_.end(), focus_); it != list_.end()) {
            ImGui::SetScrollY(std::max(0.0f, float(it - list_.begin()) * step - ImGui::GetWindowHeight() / 3));
        }
        scrollToFocus_ = false;
    }
    // Up / down move through the list.
    if (keys_ && ImGui::IsWindowFocused() && !list_.empty()) {
        const auto it = std::find(list_.begin(), list_.end(), focus_);
        int at = it == list_.end() ? -1 : int(it - list_.begin());
        const int before = at;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            at = std::min(int(list_.size()) - 1, at + 1);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            at = std::max(0, at - 1);
        }
        if (at != before && at >= 0) {
            focus_ = anchor_ = list_[std::size_t(at)];
            selected_ = {focus_};
            const float y = float(at) * step;
            if (y < ImGui::GetScrollY() || y + step > ImGui::GetScrollY() + ImGui::GetWindowHeight()) {
                ImGui::SetScrollY(std::max(0.0f, y - ImGui::GetWindowHeight() / 2));
            }
        }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiListClipper clipper;
    clipper.Begin(int(list_.size()), step);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const std::int32_t i = list_[std::size_t(r)];
            ImGui::PushID(i);
            if (ImGui::Selectable("##row", selected_.count(i) != 0, 0, ImVec2(0, rowH))) {
                const auto& io = ImGui::GetIO();
                if (io.KeyShift && anchor_ >= 0) {
                    const auto a = std::find(list_.begin(), list_.end(), anchor_);
                    if (a != list_.end()) {
                        const auto [lo, hi] = std::minmax(int(a - list_.begin()), r);
                        if (!io.KeyCtrl) {
                            selected_.clear();
                        }
                        for (int k = lo; k <= hi; ++k) {
                            selected_.insert(list_[std::size_t(k)]);
                        }
                    }
                } else if (io.KeyCtrl) {
                    if (!selected_.erase(i)) {
                        selected_.insert(i);
                    }
                    anchor_ = i;
                } else {
                    selected_ = {i};
                    anchor_ = i;
                }
                focus_ = i;
            }
            const ImVec2 a = ImGui::GetItemRectMin();
            const bool inBag = std::size_t(i) < bag->size();
            const Tex* thumb = inBag ? s_.sd().get(i) : s_.hd().get(i);
            drawFit(dl, thumb, ImVec2(a.x + 2, a.y + 2), ImVec2(rowH - 4, rowH - 4));
            const float tx = a.x + rowH + 6 * s_.scale();
            const std::string name = inBag ? bag->bag().images()[std::size_t(i)].name : (pack ? pack->name(std::uint32_t(i)) : "");
            dl->AddText(ImVec2(tx, a.y + 2), ImGui::GetColorU32(ImGuiCol_Text), std::format("#{}  {}", i, name).c_str());
            std::string sub = inBag ? std::format("type {}", int(bag->bag().images()[std::size_t(i)].type)) : "extra pack slot";
            if (thumb && thumb->texture) {
                sub += std::format("  ·  {}x{}", thumb->width, thumb->height);
            }
            if (pack && pack->present(std::uint32_t(i))) {
                const auto* e = pack->entry(std::uint32_t(i));
                sub += e->width > 0 ? std::format("  ·  HD {:.3g}x", double(e->pixelWidth) / e->width) : std::string("  ·  HD");
            }
            if (bag->edited(i) || (pack && pack->edited(std::uint32_t(i)))) {
                sub += "  ·  edited";
            }
            dl->AddText(ImVec2(tx, a.y + rowH / 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str());
            ImGui::PopID();
        }
    }
    if (list_.empty()) {
        ImGui::TextDisabled("Nothing matches.");
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

void ImagesView::factsTable(const char* id, const Tex* t, bool hd, std::int32_t index)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        return;
    }
    const auto row = [](const char* k, const std::string& v) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", k);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(v.c_str());
    };
    auto* bag = s_.bag();
    auto* pack = s_.pack();
    if (!hd && std::size_t(index) < bag->size()) {
        const auto& info = bag->bag().images()[std::size_t(index)];
        row("Name", info.name);
        row("Type", std::format("{} ({}){}", int(info.type), imageTypeName(info.type),
                                nox::video::canEncodeImage(info.type) ? "" : "  read-only"));
        row("Stored", std::format("{} bytes in segment {}", info.size, info.segment));
    } else if (hd && pack && pack->present(std::uint32_t(index))) {
        row("Name", pack->name(std::uint32_t(index)));
    }
    if (t && t->error.empty() && !t->missing) {
        row("Size", std::format("{} x {}", t->width, t->height));
        if (hd) {
            row("Stored at", std::format("{} x {}  ({:.3g}x)", t->pixelWidth, t->pixelHeight,
                                         t->width > 0 ? double(t->pixelWidth) / t->width : 0.0));
        }
        if (t->type != 0 && t->type != 1) {
            row("Draw offset", std::format("{}, {}", t->offsetX, t->offsetY));
            row("Header byte", std::to_string(t->headerByte));
        }
        std::string slots;
        for (const auto m : t->materialSlots) {
            slots += (slots.empty() ? "" : ", ") + std::to_string(m);
        }
        row("Material slots", slots.empty() ? "none" : slots);
        if (t->edgeMask) {
            row("Edge mask", "yes");
        }
        if (!t->extra.empty()) {
            row("Stored as", t->extra);
        }
    }
    const bool edited = hd ? pack && pack->edited(std::uint32_t(index)) : bag->edited(index);
    if (edited) {
        row("Edited", hd && !pack->present(std::uint32_t(index)) ? "removed (saved with the pack)" : "yes (not saved yet)");
    }
    ImGui::EndTable();
}

void ImagesView::detailPane(float height)
{
    ImGui::BeginChild("detail", ImVec2(0, height));
    auto* bag = s_.bag();
    auto* pack = s_.pack();
    if (focus_ < 0 && !list_.empty()) {
        focus_ = anchor_ = list_.front();
        selected_ = {focus_};
    }
    if (focus_ < 0) {
        ImGui::TextDisabled("Pick an image on the left.");
        ImGui::EndChild();
        return;
    }
    const bool inBag = std::size_t(focus_) < bag->size();
    ImGui::Text("#%d  %s", focus_, inBag ? bag->bag().images()[std::size_t(focus_)].name.c_str() : "(extra pack slot)");
    ImGui::SameLine(0, 30 * s_.scale());
    compare_.controls();
    if (keys_) {
        compare_.keys();
    }

    const Tex* sd = inBag ? s_.sd().getNow(focus_) : nullptr;
    const Tex* hd = s_.packOpen() ? s_.hd().getNow(focus_) : nullptr;
    const float actionsH = ImGui::GetFrameHeightWithSpacing() * 4 + ImGui::GetStyle().ItemSpacing.y * 2;
    const float factsH = ImGui::GetTextLineHeightWithSpacing() * 10;
    const float imageH = std::max(120 * s_.scale(), ImGui::GetContentRegionAvail().y - actionsH - factsH);
    PaneView bagPane;
    bagPane.title = "video.bag";
    bagPane.image = sd;
    PaneView hdPane;
    hdPane.title = "HD (image pack)";
    hdPane.image = hd;
    hdPane.hd = true;
    compare_.draw(bagPane, hdPane, ImVec2(ImGui::GetContentRegionAvail().x, imageH));
    ImGui::BeginChild("facts", ImVec2(0, factsH));
    if (ImGui::BeginTable("factsCols", 2, ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("video.bag");
        factsTable("sdFacts", sd, false, focus_);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(pack ? "HD (image pack)" : "HD: no image pack open");
        if (pack) {
            factsTable("hdFacts", hd, true, focus_);
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    actions();
    ImGui::EndChild();
}

void ImagesView::actions()
{
    auto* bag = s_.bag();
    auto* pack = s_.pack();
    const auto sel = selection();
    const bool busy = s_.busy();
    ImGui::Separator();
    ImGui::BeginDisabled(busy || sel.empty());
    if (ImGui::Button(std::format("Extract video.bag ({})", sel.size()).c_str())) {
        extractWhat_ = 1;
        s_.dialogs().request(kDialogExtract, Dialogs::Folder, s_.window(), s_.settings().extractDir);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!pack);
    if (ImGui::Button("Extract HD")) {
        extractWhat_ = 2;
        s_.dialogs().request(kDialogExtract, Dialogs::Folder, s_.window(), s_.settings().extractDir);
    }
    ImGui::SameLine();
    if (ImGui::Button("Extract both")) {
        extractWhat_ = 3;
        s_.dialogs().request(kDialogExtract, Dialogs::Folder, s_.window(), s_.settings().extractDir);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("PNG + .json sidecar (+ masks), to edit and put back");

    const bool inBag = focus_ >= 0 && std::size_t(focus_) < bag->size();
    const bool encodable = inBag && nox::video::canEncodeImage(bag->bag().images()[std::size_t(focus_)].type);
    ImGui::BeginDisabled(!encodable || s_.inserting());
    if (ImGui::Button("Replace video.bag image...")) {
        replaceIndex_ = focus_;
        pendingBagReplace_ = true;
        s_.dialogs().request(kDialogReplaceBag, Dialogs::Open, s_.window(), s_.settings().extractDir, kPngFilter, 1);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("A PNG for #%d, re-encoded in the slot's type (offsets from its .json sidecar if it has one)", focus_);
    ImGui::SameLine();
    ImGui::BeginDisabled(!pack || focus_ < 0 || s_.inserting());
    if (ImGui::Button(pack && pack->present(std::uint32_t(std::max(0, focus_))) ? "Replace HD image..." : "Add HD image...")) {
        replaceIndex_ = focus_;
        s_.dialogs().request(kDialogReplacePack, Dialogs::Open, s_.window(), s_.settings().extractDir, kPngFilter, 1);
    }
    ImGui::SetItemTooltip("A larger PNG for #%d: drawn over the original's size and offsets", focus_);
    ImGui::EndDisabled();
    ImGui::SameLine();
    std::size_t hdSelected = 0, editedSelected = 0;
    for (const auto i : sel) {
        hdSelected += pack && pack->present(std::uint32_t(i));
        editedSelected += bag->edited(i) || (pack && pack->edited(std::uint32_t(i)));
    }
    ImGui::BeginDisabled(hdSelected == 0);
    if (ImGui::Button(std::format("Remove HD ({})", hdSelected).c_str())) {
        s_.confirm("Remove HD images?",
                   std::format("Remove {} HD image{} from the pack? NoX Core then draws the video.bag images.\n"
                               "(Nothing is written until you save the pack.)",
                               hdSelected, hdSelected == 1 ? "" : "s"),
                   [this, sel] { s_.removePack(sel); });
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(editedSelected == 0);
    if (ImGui::Button(std::format("Revert ({})", editedSelected).c_str())) {
        s_.revert(sel);
    }
    ImGui::EndDisabled();

    ImGui::BeginDisabled(s_.inserting());
    if (ImGui::Button("Put in a folder of PNGs...")) {
        s_.dialogs().request(kDialogInsertFolder, Dialogs::Folder, s_.window(), s_.settings().extractDir);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Every PNG with a sidecar (or named <index>_name.png / <index>_name.hd.png) into its slot");
    ImGui::SameLine();
    ImGui::BeginDisabled(!pack);
    ImGui::Checkbox("New HD images as BC7", &s_.encodeOptions().bc7);
    ImGui::SetItemTooltip("BC7 with mips, as NoX Core's HD pack stores them (else lossless)");
    ImGui::EndDisabled();
    if (s_.inserting()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Putting in... %zu left", s_.insertsLeft());
    }
    if (!s_.messages().empty() && ImGui::CollapsingHeader(std::format("Messages ({})###msgs", s_.messages().size()).c_str())) {
        ImGui::BeginChild("msgs", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 6), ImGuiChildFlags_Borders);
        for (const auto& m : s_.messages()) {
            ImGui::TextWrapped("%s", m.c_str());
        }
        ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
}

void ImagesView::pollDialogs()
{
    if (const auto p = s_.dialogs().take(kDialogExtract); p && s_.bag()) {
        const fs::path dir = pathFromUtf8(*p);
        s_.settings().extractDir = *p;
        editor::ExtractRequest req;
        req.paths = s_.paths();
        req.bagEdits = s_.bag()->bagEdits();
        if (auto* pack = s_.pack()) {
            req.packPath = pack->path();
            req.packEdits = pack->packEdits();
        }
        req.indexes = selection();
        req.fromBag = extractWhat_ & 1;
        req.fromPack = (extractWhat_ & 2) && s_.pack();
        req.dir = dir;
        const std::size_t n = req.indexes.size();
        s_.startJob("Extracting", dir, n, [req = std::move(req)](ExportProgress& progress) {
            const auto c = editor::extractImages(
                req,
                [&progress](std::size_t done, std::size_t total) {
                    progress.done = done;
                    progress.total = total;
                },
                &progress.cancel);
            std::string msg = std::format("Extracted {} video.bag and {} HD images to {}{}", c.bag, c.pack, utf8(req.dir),
                                          c.failed ? std::format(" ({} failed)", c.failed) : std::string());
            for (const auto& e : c.errors) {
                msg += "\n  " + e;
            }
            return msg;
        });
    }
    if (const auto p = s_.dialogs().take(kDialogReplaceBag); p && pendingBagReplace_) {
        pendingBagReplace_ = false;
        s_.settings().extractDir = utf8(pathFromUtf8(*p).parent_path());
        s_.replaceBag(replaceIndex_, editor::findImageFile(pathFromUtf8(*p)));
    }
    if (const auto p = s_.dialogs().take(kDialogReplacePack); p && s_.pack()) {
        s_.settings().extractDir = utf8(pathFromUtf8(*p).parent_path());
        try {
            s_.replacePack(replaceIndex_, editor::findImageFile(pathFromUtf8(*p)));
        } catch (const std::exception& ex) {
            s_.error(ex.what());
        }
    }
    if (const auto p = s_.dialogs().take(kDialogInsertFolder)) {
        s_.settings().extractDir = *p;
        try {
            std::vector<InsertTask> tasks;
            std::size_t unknown = 0;
            for (auto& f : editor::findImageFiles(pathFromUtf8(*p))) {
                const auto slot = editor::fileSlot(f);
                if (!slot) {
                    ++unknown;
                    s_.log(utf8(f.png.filename()) + ": no sidecar and no index in its name; skipped");
                    continue;
                }
                tasks.push_back({std::move(f), slot->first, slot->second});
            }
            s_.setStatus(std::format("Putting in {} PNGs from {}{}", tasks.size(), *p,
                                     unknown ? std::format(" ({} skipped: see Messages)", unknown) : std::string()));
            s_.queueInserts(std::move(tasks));
        } catch (const std::exception& ex) {
            s_.error(ex.what());
        }
    }
}

} // namespace videotools::gui
