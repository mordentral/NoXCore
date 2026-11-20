// A mod's own game data on noxcore.exe's side (game_data_swap.hpp).
#include "game_data_swap.hpp"

#include "app.hpp"
#include "local_host.hpp"
#include "spell_chant.hpp"

#include "noxcore/audio/sound_names.hpp"
#include "noxcore/content/content_pack.hpp"
#include "noxcore/strings/l10n.hpp"

#include <SDL3/SDL.h>

#include <exception>

namespace noxsdl {

namespace {

/// What was made from the app's tables, made afresh from them as they now are.
void remakeFromTables(App& app)
{
    if (app.art) {
        app.art->reset();
    }
    app.newPlayerArt = {};
    app.skullArt = {};
    app.crownArt = {};
    app.flagArt = {};
    app.glyphArt = {};
    app.mimicArt = {};
    if (app.chant) {
        app.chant = std::make_unique<SpellChant>(app.things);
    }
    if (!app.loopSounds.empty()) {
        // Each type's AUDIOLOOP sound by type id (ids count from 1), as at start-up.
        app.loopSounds.assign(app.things.things.size() + 1, 0);
        for (std::size_t i = 0; i < app.things.things.size(); ++i) {
            if (const auto loop = app.things.things[i].attribute("AUDIOLOOP")) {
                app.loopSounds[i + 1] = nox::audio::soundId(*loop).value_or(0);
            }
        }
    }
}

} // namespace

std::string useGameData(App& app, std::uint32_t hash, const std::vector<nox::things::DataFileBytes>& files)
{
    if (app.gameDataHash == hash && app.installTables) {
        return {}; // (this set's already the app's)
    }
    nox::things::DataTables tables;
    try {
        tables = nox::things::readDataTables(files, false, app.gameDir, true); // (its overrides over our install's files: checked against the host's)
    } catch (const std::exception& ex) {
        return ex.what();
    }
    if (app.host && !app.host->modeData()) {
        return NOX_TR("game.data.hosting_other", "another game is hosted here"); // (its server reads the app's tables)
    }
    LocalHost::finishRetired(0); // (a host that left its game may read the app's tables still)
    useInstallData(app);
    auto install = std::make_unique<App::InstallTables>();
    install->things = std::move(app.things);
    install->modifiers = std::move(app.modifiers);
    install->gameData = std::move(app.gameData);
    // Each file the set has in place of the install's; the others a copy of the install's.
    app.things = tables.things ? std::move(*tables.things) : install->things;
    app.modifiers = tables.modifiers ? std::move(*tables.modifiers) : install->modifiers;
    app.gameData = tables.gameData ? std::move(*tables.gameData) : install->gameData;
    app.installTables = std::move(install);
    app.gameDataHash = hash;
    if (!tables.addedBases.empty()) {
        nox::things::setAddedTypeBases(tables.addedBases); // (the mod's new types' stock types, for this game)
    }
    remakeFromTables(app);
    SDL_Log("game data: this game's mod's own (%zu files) in place of the install's", files.size());
    return {};
}

void useInstallData(App& app)
{
    if (!app.installTables) {
        return;
    }
    LocalHost::finishRetired(0);
    app.things = std::move(app.installTables->things);
    app.modifiers = std::move(app.installTables->modifiers);
    app.gameData = std::move(app.installTables->gameData);
    app.installTables.reset();
    app.gameDataHash = 0;
    nox::things::setAddedTypeBases(nox::content::newThingTypes().bases); // (the content packs' again, as at start-up)
    remakeFromTables(app);
    SDL_Log("game data: the install's again");
}

} // namespace noxsdl
