// The character (.plr) editor (panels.hpp): its name and colours, with the doll as the game's
// character creator draws it. Body colours are any colour (the game's swatches are there to
// pick from too); clothes are modifier.bin's 32 UserColors, all a .plr can name.
#include "gui/panels.hpp"

#include "editor/theme_colours.hpp"

#include "noxcore/strings/string_table.hpp"

#include <imgui.h>

#include <algorithm>
#include <climits>
#include <format>

namespace fs = std::filesystem;

namespace videotools::gui {

using content::BodyPart;
using content::Garment;
using content::PlrRgb;

namespace {

ImVec4 toImGui(const PlrRgb& c)
{
    return ImVec4(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, 1.0f);
}

PlrRgb fromFloats(const float* f)
{
    const auto b = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return {b(f[0]), b(f[1]), b(f[2])};
}

/// 32 swatches, 8 a row; the one clicked, if any.
std::optional<int> swatches(const char* id, const std::array<PlrRgb, 32>& colours, int current, float size)
{
    std::optional<int> picked;
    ImGui::PushID(id);
    for (int i = 0; i < 32; ++i) {
        if (i % 8 != 0) {
            ImGui::SameLine(0, 2);
        }
        ImGui::PushID(i);
        const bool here = i == current;
        if (here) {
            ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_Text));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
        }
        if (ImGui::ColorButton("##c", toImGui(colours[std::size_t(i)]), ImGuiColorEditFlags_NoTooltip, ImVec2(size, size))) {
            picked = i;
        }
        if (here) {
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    ImGui::PopID();
    return picked;
}

} // namespace

RecordPanel::~RecordPanel()
{
    destroyTexture(doll_);
    dropViewerTextures();
}

void RecordPanel::rebuildDoll()
{
    destroyTexture(doll_);
    doll_ = nullptr;
    dollStale_ = false;
    auto* bag = ws_.session.bag();
    const auto* things = ws_.files.things();
    if (!character_ || !bag || !things) {
        return;
    }
    if (imageByName_.empty()) {
        for (const auto& def : things->images) {
            if (def.image && def.image->index >= 0) {
                imageByName_.emplace(def.name, def.image->index); // (the first of a name, as the game finds it)
            }
        }
    }
    struct Piece {
        nox::video::DecodedImage image;
    };
    std::vector<Piece> pieces;
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
    for (const auto& layer : content::dollLayers(*character_, ws_.files.modifiers())) {
        const auto it = imageByName_.find(layer.image);
        if (it == imageByName_.end()) {
            continue;
        }
        try {
            auto img = bag->decode(it->second);
            content::shadeMaterials(img.rgba, img.material, layer.materials);
            x0 = std::min(x0, img.offsetX);
            y0 = std::min(y0, img.offsetY);
            x1 = std::max(x1, img.offsetX + img.width);
            y1 = std::max(y1, img.offsetY + img.height);
            pieces.push_back({std::move(img)});
        } catch (const std::exception&) {
        }
    }
    if (pieces.empty() || x1 <= x0 || y1 <= y0) {
        return;
    }
    // Each picture over the ones before it, where its offset puts it (as the game draws them).
    dollW_ = x1 - x0;
    dollH_ = y1 - y0;
    std::vector<std::uint8_t> canvas(std::size_t(dollW_) * std::size_t(dollH_) * 4, 0);
    for (const auto& p : pieces) {
        const auto& img = p.image;
        for (int y = 0; y < img.height; ++y) {
            for (int x = 0; x < img.width; ++x) {
                const std::uint8_t* s = &img.rgba[(std::size_t(y) * std::size_t(img.width) + std::size_t(x)) * 4];
                if (s[3] == 0) {
                    continue;
                }
                std::uint8_t* d = &canvas[(std::size_t(y + img.offsetY - y0) * std::size_t(dollW_) + std::size_t(x + img.offsetX - x0)) * 4];
                const unsigned a = s[3];
                for (int ch = 0; ch < 3; ++ch) {
                    d[ch] = static_cast<std::uint8_t>((s[ch] * a + d[ch] * (255 - a)) / 255);
                }
                d[3] = static_cast<std::uint8_t>(a + d[3] * (255 - a) / 255);
            }
        }
    }
    try {
        doll_ = makeTexture(SDL_GetRenderer(ws_.session.window()), canvas, dollW_, dollH_);
        SDL_SetTextureScaleMode(doll_, SDL_SCALEMODE_NEAREST);
    } catch (const std::exception& ex) {
        characterError_ = ex.what();
    }
}

bool RecordPanel::characterForm(const content::Record& r)
{
    if (r.ref.list != content::RecordList::File || r.ref.kind != content::FileKind::Characters) {
        return false;
    }
    const fs::path* file = recordFile(ws_.files, r.ref);
    if (!file) {
        return false;
    }
    const auto& theme = noxsdl::editor::themeColours();
    if (characterFile_ != *file) {
        characterFile_ = *file;
        characterError_.clear();
        character_.reset();
        try {
            character_ = content::Character::load(*file);
        } catch (const std::exception& ex) {
            characterError_ = std::format("Can't read it: {}", ex.what());
        }
        dollStale_ = true;
    }
    if (!character_) {
        ImGui::TextColored(theme.error, "%s", characterError_.c_str());
        return true;
    }
    content::Character& c = *character_;
    if (dollStale_) {
        rebuildDoll();
    }
    const float scale = ws_.session.scale();

    // --- Who it is
    static constexpr const char* kClasses[] = {"Warrior", "Wizard", "Conjurer"};
    const auto cls = std::size_t(c.info().characterClass);
    ImGui::TextDisabled("%s, level %d, %s. %s", cls < 3 ? kClasses[cls] : "?", int(c.info().level), c.female() ? "woman" : "man",
                        c.info().flags & 4 ? "Quest character." : c.info().flags & 1 ? "Solo character." : "Multiplayer character.");
    char name[64] = {};
    {
        const std::string now = nox::strings::toUtf8(c.name());
        std::snprintf(name, sizeof name, "%s", now.c_str());
    }
    ImGui::SetNextItemWidth(260 * scale);
    if (ImGui::InputText("Name", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
        if (!c.setName(nox::strings::fromUtf8(name))) {
            ws_.session.error("A name is 1 to 24 characters.");
        }
    }

    // --- The doll (left) and the colours (right)
    const float box = 200 * scale;
    if (ImGui::BeginTable("character", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("doll", ImGuiTableColumnFlags_WidthFixed, box + 8 * scale);
        ImGui::TableSetupColumn("colours", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + box, p.y + box), IM_COL32(0, 0, 0, 255));
        if (doll_) {
            const float k = std::max(1.0f, std::floor(std::min(box / float(dollW_), box / float(dollH_))));
            const ImVec2 size(dollW_ * k, dollH_ * k);
            const ImVec2 at(p.x + (box - size.x) / 2, p.y + (box - size.y) / 2);
            dl->AddImage(texRef(doll_), at, ImVec2(at.x + size.x, at.y + size.y));
        } else {
            dl->AddText(ImVec2(p.x + 8, p.y + 8), ImGui::GetColorU32(ImGuiCol_TextDisabled), "(the doll's pictures aren't in video.bag)");
        }
        ImGui::Dummy(ImVec2(box, box));
        ImGui::TextDisabled("As the character creator draws it.");

        ImGui::TableNextColumn();
        ImGui::SeparatorText("Body (any colour)");
        struct Part {
            BodyPart part;
            const char* label;
            const std::array<PlrRgb, 32>* table;
        };
        static constexpr Part kParts[] = {
            {BodyPart::Skin, "Skin", &content::kCreatorSkinColors},     {BodyPart::Hair, "Hair", &content::kCreatorHairColors},
            {BodyPart::Mustache, "Mustache", &content::kCreatorHairColors}, {BodyPart::Goatee, "Goatee", &content::kCreatorHairColors},
            {BodyPart::Beard, "Beard", &content::kCreatorHairColors},
        };
        for (const Part& part : kParts) {
            ImGui::PushID(part.label);
            const bool skin = part.part == BodyPart::Skin;
            bool on = c.hasPart(part.part);
            if (!skin) {
                if (ImGui::Checkbox("##on", &on)) {
                    // Off is the skin's colour (as the game saves it); on again, the hair's (or the creator's first).
                    const PlrRgb hair = c.hasPart(BodyPart::Hair) ? c.colour(BodyPart::Hair) : content::kCreatorHairColors[0];
                    c.setColour(part.part, on ? (part.part == BodyPart::Hair ? content::kCreatorHairColors[0] : hair) : c.colour(BodyPart::Skin));
                    dollStale_ = true;
                }
                ImGui::SetItemTooltip(part.part == BodyPart::Hair ? "Off: bald" : "Off: none");
                ImGui::SameLine();
            } else {
                ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
                ImGui::SameLine();
            }
            ImGui::BeginDisabled(!on);
            const PlrRgb now = c.colour(part.part);
            float f[3] = {now[0] / 255.0f, now[1] / 255.0f, now[2] / 255.0f};
            ImGui::SetNextItemWidth(230 * scale);
            if (ImGui::ColorEdit3(part.label, f, ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_InputRGB | ImGuiColorEditFlags_Uint8)) {
                PlrRgb picked = fromFloats(f);
                if (!skin && picked == c.colour(BodyPart::Skin)) {
                    picked[0] = picked[0] < 255 ? std::uint8_t(picked[0] + 1) : std::uint8_t(254); // (the skin's exact colour would read as "off")
                }
                c.setColour(part.part, picked);
                dollStale_ = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Game's...")) {
                ImGui::OpenPopup("swatches");
            }
            ImGui::SetItemTooltip("The character creator's 32 colours for this");
            if (ImGui::BeginPopup("swatches")) {
                const auto at = std::ranges::find(*part.table, now);
                const int current = at == part.table->end() ? -1 : int(at - part.table->begin());
                if (const auto i = swatches(part.label, *part.table, current, 22 * scale)) {
                    c.setColour(part.part, (*part.table)[std::size_t(*i)]);
                    dollStale_ = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }

        ImGui::SeparatorText("Clothes (modifier.bin's UserColor1-32)");
        std::array<PlrRgb, 32> clothes{};
        for (int i = 0; i < 32; ++i) {
            clothes[std::size_t(i)] = content::userColor(ws_.files.modifiers(), i);
        }
        struct Cloth {
            Garment garment;
            const char* label;
        };
        static constexpr Cloth kClothes[] = {
            {Garment::Shirt1, "Shirt"}, {Garment::Shirt2, "Shirt trim"}, {Garment::Pants, "Pants"}, {Garment::Shoes1, "Shoes"}, {Garment::Shoes2, "Shoe trim"},
        };
        for (const Cloth& cloth : kClothes) {
            ImGui::PushID(cloth.label);
            const int index = c.clothing(cloth.garment);
            ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
            ImGui::SameLine();
            if (ImGui::ColorButton("##now", toImGui(clothes[std::size_t(std::min(index, 31))]), ImGuiColorEditFlags_NoTooltip,
                                   ImVec2(ImGui::GetFrameHeight() * 2, ImGui::GetFrameHeight()))) {
                ImGui::OpenPopup("clothes");
            }
            ImGui::SameLine();
            ImGui::Text("%s  (UserColor%d)", cloth.label, index + 1);
            if (ImGui::BeginPopup("clothes")) {
                if (const auto i = swatches(cloth.label, clothes, index, 22 * scale)) {
                    c.setClothing(cloth.garment, std::uint8_t(*i));
                    dollStale_ = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::TextDisabled("A .plr names one of these 32; their colours are modifier.bin's\n(a content pack's modifier.patch can change them, for every player who has it).");
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // --- Saving: always a new file, never in the game's folders
    ImGui::Separator();
    if (c.changed()) {
        ImGui::TextColored(theme.highlightText, "Changed (not saved).");
        ImGui::SameLine();
    }
    if (ImGui::Button("Save as...")) {
        static constexpr SDL_DialogFileFilter kPlr[] = {{"Nox character", "plr"}};
        const std::string start = ws_.session.settings().saveDir.empty() ? std::string() : ws_.session.settings().saveDir + "/" + utf8(file->filename());
        ws_.session.dialogs().request(kDialogSaveCharacter, Dialogs::Save, ws_.session.window(), start, kPlr, 1);
    }
    ImGui::SetItemTooltip("Writes a new .plr (never over this one, never into the game's folder). To play it, put it in\n"
                          "the Save folder of a copy of the game.");
    if (c.changed()) {
        ImGui::SameLine();
        if (ImGui::Button("Revert")) {
            characterFile_.clear(); // (read again next frame)
        }
    }
    if (const auto picked = ws_.session.dialogs().take(kDialogSaveCharacter)) {
        fs::path target = pathFromUtf8(*picked);
        if (target.extension().empty()) {
            target += ".plr";
        }
        if (const std::string why = content::checkSaveOutsideGame(target, *file, ws_.session.gameDir()); !why.empty()) {
            ws_.session.error("Not there: " + why);
        } else {
            const auto save = [this, target] {
                try {
                    character_->save(target); // (the form goes on with what was saved; the record is still the file read)
                    ws_.session.settings().saveDir = utf8(target.parent_path());
                    ws_.session.setStatus("Saved " + utf8(target), target.parent_path());
                } catch (const std::exception& ex) {
                    ws_.session.error(std::string("Couldn't save it: ") + ex.what());
                }
            };
            std::error_code ec;
            if (fs::exists(target, ec)) {
                ws_.session.confirm("Replace it?", utf8(target) + " is there already. Replace it?", save);
            } else {
                save();
            }
        }
    }
    return true;
}

} // namespace videotools::gui
