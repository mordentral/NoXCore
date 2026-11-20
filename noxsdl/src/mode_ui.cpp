// The modes' UIs and client sides (mode_ui.hpp).
#include "mode_ui.hpp"

#include "app.hpp"
#include "local_host.hpp"

#include "noxcore/io/file.hpp"
#include "noxcore/modes/registry.hpp"
#include "noxcore/modes/script/script_ui.hpp"
#include "noxcore/server/mode_maps.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace noxsdl {

std::vector<ModeUiEntry>& modeUiEntries()
{
    static std::vector<ModeUiEntry> entries;
    return entries;
}

const ModeUiEntry* modeUiEntry(std::string_view modeId)
{
    const auto& entries = modeUiEntries();
    const auto it = std::ranges::find(entries, modeId, &ModeUiEntry::modeId);
    return it != entries.end() ? &*it : nullptr;
}

void makeModeUis(App& app)
{
    app.modeUis.clear();
    for (const ModeUiEntry& e : modeUiEntries()) {
        if (e.make) {
            app.modeUis.emplace_back(e.modeId, e.make(app));
        }
    }
}

void bindModeClients(App& app)
{
    // (The live client's own sides, each idle unless its server plays its mode; one not made with
    // every client, as it acts for our player, made for ours now.)
    app.scriptUi = nullptr;
    for (auto& [id, ui] : app.modeUis) {
        if (!app.live) {
            ui->bind(nullptr);
            continue;
        }
        auto& client = app.live->client();
        const ModeUiEntry* e = modeUiEntry(id);
        nox::modes::ClientMode* side = client.modeSide(id);
        if (!side) {
            // (Made once a client: making one sets its messages' handler.)
            side = e && e->makeSide ? client.adoptModeSide(id, e->makeSide(client)) : client.addModeSide(id);
        }
        ui->bind(side);
        if (auto* script = dynamic_cast<nox::modes::script::ScriptUiClient*>(side)) {
            app.scriptUi = script; // (its weather, for the drawing: weather.hpp)
            // (Client level 14: a script mode's texts, nox.setText's, over nox.csf for this game.)
            if (app.strings) {
                app.strings->clearOverlay();
            }
            script->setStrings(app.strings.get());
        }
    }
}

void unbindModeClients(App& app)
{
    for (auto& [id, ui] : app.modeUis) {
        ui->bind(nullptr);
    }
    if (app.scriptUi) {
        app.scriptUi->setStrings(nullptr); // (the game's texts gone from nox.csf's lookups)
    }
    app.scriptUi = nullptr;
}

bool isModeFakeFlag(std::string_view flag)
{
    return std::ranges::any_of(modeUiEntries(), [&](const ModeUiEntry& e) { return std::ranges::find(e.fakeFlags, flag) != e.fakeFlags.end(); });
}

std::filesystem::path modeSaveRoot(const std::filesystem::path& soloRoot, std::string_view modeId, std::string_view campaign)
{
    const ModeUiEntry* e = modeUiEntry(modeId);
    if (!e || !e->soloScreens) {
        return soloRoot;
    }
    std::filesystem::path root = soloRoot / e->soloScreens->saves;
    if (!campaign.empty()) {
        // A mod's campaign: its players' saves in its own folder (registry.hpp's
        // campaignSavesFolder); where that can't be written (a read-only install), beside the
        // mode's Save\ as before (never one of its slots), said once.
        const nox::modes::Mode* mod = nox::modes::installedCampaign(nox::modes::registry(), campaign);
        if (std::filesystem::path own = mod ? nox::modes::campaignSavesFolder(*mod) : std::filesystem::path(); !own.empty()) {
            root = std::move(own);
        } else {
            root /= std::filesystem::path("Mods") / std::string(campaign);
            static std::set<std::string, std::less<>> told;
            if (told.insert(std::string(campaign)).second) {
                SDL_Log("%.*s: its saves can't go in its mod folder's saves (not there, or not writable): %s instead",
                        static_cast<int>(campaign.size()), campaign.data(), nox::pathUtf8(root).c_str());
            }
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(root / "Save", ec);
    return root;
}

void beginSoloScreensHost(App& app, std::string_view modeId, std::uint8_t playerClass, HostSettings& settings, std::string_view campaign)
{
    if (app.modeKeptRoot.empty()) {
        app.modeKeptRoot = app.saveRoot;
        app.saveRoot = modeSaveRoot(app.saveRoot, modeId, campaign);
    }
    settings.saveRoot = app.saveRoot;
    settings.modeId = std::string(modeId);
    settings.modeOptions["class"] = std::to_string(playerClass);
    app.campaignTitle.clear();
    if (!campaign.empty()) {
        settings.modeOptions["campaign"] = std::string(campaign);
        app.campaignTitle = campaignTitleOf(campaign);
    }
}

std::string campaignTitleOf(std::string_view campaign)
{
    const nox::modes::Mode* mode = campaign.empty() ? nullptr : nox::modes::registry().find(campaign);
    return mode && mode->info.campaign ? mode->info.campaign->title : std::string();
}

std::filesystem::path campaignMapPath(const std::filesystem::path& gameDir, std::string_view campaign, const std::string& name)
{
    // (The maps the mod ships, as its games find them: its package's index, GameHost::findMap's.)
    const nox::modes::Mode* mode = campaign.empty() ? nullptr : nox::modes::registry().find(campaign);
    if (mode && mode->maps) {
        if (const nox::server::ModeMaps::Entry* e = mode->maps->find(name)) {
            return e->map;
        }
    }
    return gameDir / "maps" / name / (name + ".map");
}

void endSoloScreensHost(App& app)
{
    if (!app.modeKeptRoot.empty()) {
        app.saveRoot = std::exchange(app.modeKeptRoot, {});
    }
    app.campaignTitle.clear();
}

std::optional<std::string> hostedCampaign(const App& app)
{
    if (!app.host || app.host->modeId().empty()) {
        return std::nullopt;
    }
    const ModeUiEntry* e = modeUiEntry(app.host->modeId());
    if (!e || !e->soloScreens) {
        return std::nullopt;
    }
    if (!app.campaignTitle.empty()) {
        return app.campaignTitle; // (a mod's campaign: its own name, never the game's own's)
    }
    const nox::modes::Mode* mode = nox::modes::registry().find(app.host->modeId());
    return mode ? mode->info.name : app.host->modeId();
}

} // namespace noxsdl
