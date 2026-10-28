// A mod folder in the record editor (panels.hpp): its content pack and its game mode (mode.json,
// edited as text and checked as NoX Core reads it), and a test launch: noxcore.exe hosting a game
// with it on this computer only.
#include "gui/panels.hpp"

#include "editor/theme_colours.hpp"

#include "noxcore/content/content_pack.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/modes/script/manifest.hpp"
#include "noxcore/util/text.hpp"

#include <SDL3/SDL_process.h>
#include <imgui.h>

#include <format>
#include <system_error>

namespace fs = std::filesystem;

namespace videotools::gui {

bool RecordPanel::modForm(const content::Record& r)
{
    if (r.ref.list != content::RecordList::File || r.ref.kind != content::FileKind::Mods) {
        return false;
    }
    const fs::path* folder = recordFile(ws_.files, r.ref);
    if (!folder) {
        return false;
    }
    const auto& theme = noxsdl::editor::themeColours();
    std::error_code ec;
    ImGui::TextDisabled("%s", utf8(*folder).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Show in folder")) {
        SDL_OpenURL(("file:///" + utf8(folder->generic_u8string())).c_str());
    }
    // Its content pack (mods\<id>\content), as NoX Core finds it.
    std::string packFiles;
    for (const auto& p : nox::content::findContentPacks(folder->parent_path())) {
        if (nox::util::equalsNoCase(p.id, folder->filename().string())) {
            const auto add = [&](const std::string& s) { packFiles += (packFiles.empty() ? "" : ", ") + s; };
            for (const auto f : p.tables) add(nox::content::patchFileName(f));
            if (p.things) add("thing.patch");
            if (p.strings) add("strings.patch");
            if (p.audio) add("audio\\");
            if (p.dialog) add("dialog\\");
            if (p.music) add("music\\");
        }
    }
    ImGui::Text("Content pack: %s", packFiles.empty() ? "none" : packFiles.c_str());
    if (!packFiles.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Edit it")) {
            ws_.session.settings().packId = folder->filename().string();
            ws_.session.setStatus("Pick it at the top (Pack) to edit it: " + folder->filename().string());
        }
    }
    const fs::path manifestFile = nox::findNoCase(*folder, "mode.json");
    const bool mode = fs::exists(manifestFile, ec);
    std::string modeId;
    if (mode) {
        try {
            const auto bytes = nox::readFileBytes(manifestFile);
            if (const auto m = nox::modes::script::readManifest(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())).manifest) {
                modeId = m->info.id;
            }
        } catch (const std::exception&) {
        }
    }
    ImGui::Text("Game mode: %s", mode ? (modeId.empty() ? "mode.json (NoX Core won't list it as it is)" : ("\"" + modeId + "\"").c_str()) : "none");

    // The test launch: noxcore.exe from beside this program, hosting on this computer only.
    ImGui::SeparatorText("Try it");
    const char* base = SDL_GetBasePath();
    const fs::path here = base ? fs::path(reinterpret_cast<const char8_t*>(base)) : fs::current_path();
    const fs::path exe = nox::findNoCase(here, "noxcore.exe");
    const bool haveExe = fs::exists(exe, ec);
    const fs::path itsMods = here / "mods";
    const bool sameMods = fs::exists(itsMods, ec) && fs::equivalent(itsMods, folder->parent_path(), ec);
    if (!haveExe) {
        ImGui::TextColored(theme.warning, "noxcore.exe isn't beside this program (%s): put noxcontent.exe beside it to test from here.", utf8(here).c_str());
    } else if (!sameMods) {
        ImGui::TextColored(theme.warning, "noxcore.exe reads the mods folder beside it (%s); this mod is somewhere else: copy it there to test it.",
                           utf8(itsMods).c_str());
    }
    // A map to host: any of the game's (the deathmatch ones suit most modes).
    if (testMap_.empty()) {
        testMap_ = "Estate";
    }
    ImGui::SetNextItemWidth(200 * ws_.session.scale());
    if (ImGui::BeginCombo("Map", testMap_.c_str())) {
        for (const auto& m : ws_.files.maps()) {
            const std::string name = m.stem().string();
            if (ImGui::Selectable(name.c_str(), name == testMap_)) {
                testMap_ = name;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!haveExe || !sameMods || (mode && modeId.empty()));
    if (ImGui::Button("Test launch")) {
        // Hosted with no router port and nothing posted online: a game on this computer only.
        // (Any free port: it's for this computer only, and the usual one may be taken.)
        // --private-test: unlisted, no router port asked for, on the loopback alone (as Play From Here's).
        std::vector<std::string> args = {utf8(exe), "--game-dir", utf8(ws_.session.gameDir()), "--host", testMap_, "--host-port", "0", "--private-test"};
        if (!modeId.empty()) {
            args.insert(args.end(), {"--host-mod", modeId});
        }
        std::vector<const char*> argv;
        for (const auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);
        if (SDL_Process* p = SDL_CreateProcess(argv.data(), false)) {
            SDL_DestroyProcess(p); // (it runs on by itself)
            ws_.session.setStatus(std::format("Started noxcore.exe hosting {}{} (this computer only).", testMap_, modeId.empty() ? "" : " as " + modeId));
        } else {
            ws_.session.error(std::string("Couldn't start noxcore.exe: ") + SDL_GetError());
        }
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("noxcore.exe hosts %s %s, with every installed pack and mod, on this computer only\n"
                          "(no router port, not listed online). Close it when you're done.",
                          testMap_.c_str(), modeId.empty() ? "as a deathmatch" : ("as " + modeId).c_str());
    if (mode) {
        ImGui::SeparatorText("mode.json");
        // (Its own ID scope: the editor's buttons are named like this page's, Show in folder.)
        ImGui::PushID("manifest");
        textEditor(manifestFile);
        ImGui::PopID();
    }
    return true;
}

} // namespace videotools::gui
