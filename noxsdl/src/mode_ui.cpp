// The modes' UIs and client sides (mode_ui.hpp).
#include "mode_ui.hpp"

#include "app.hpp"
#include "local_host.hpp"

#include "noxcore/modes/script/script_ui.hpp"

#include <algorithm>
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
        }
    }
}

void unbindModeClients(App& app)
{
    for (auto& [id, ui] : app.modeUis) {
        ui->bind(nullptr);
    }
    app.scriptUi = nullptr;
}

bool isModeFakeFlag(std::string_view flag)
{
    return std::ranges::any_of(modeUiEntries(), [&](const ModeUiEntry& e) { return std::ranges::find(e.fakeFlags, flag) != e.fakeFlags.end(); });
}

std::filesystem::path modeSaveRoot(const std::filesystem::path& soloRoot, std::string_view modeId)
{
    const ModeUiEntry* e = modeUiEntry(modeId);
    if (!e || !e->soloScreens) {
        return soloRoot;
    }
    const std::filesystem::path root = soloRoot / e->soloScreens->saves;
    std::error_code ec;
    std::filesystem::create_directories(root / "Save", ec);
    return root;
}

void beginSoloScreensHost(App& app, std::string_view modeId, std::uint8_t playerClass, HostSettings& settings)
{
    if (app.modeKeptRoot.empty()) {
        app.modeKeptRoot = app.saveRoot;
        app.saveRoot = modeSaveRoot(app.saveRoot, modeId);
    }
    settings.saveRoot = app.saveRoot;
    settings.modeId = std::string(modeId);
    settings.modeOptions["class"] = std::to_string(playerClass);
}

void endSoloScreensHost(App& app)
{
    if (!app.modeKeptRoot.empty()) {
        app.saveRoot = std::exchange(app.modeKeptRoot, {});
    }
}

} // namespace noxsdl
