// A mod's own game data on noxcore.exe's side (game_data_swap.hpp).
#include "game_data_swap.hpp"

#include "app.hpp"
#include "local_host.hpp"
#include "spell_chant.hpp"

#include "noxcore/audio/sound_names.hpp"
#include "noxcore/bot/bot.hpp"
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
    if (app.inventory) {
        app.inventory->retypeBottles(); // (the meters' potions and food by name: a whole thing.bin's numbers may differ)
    }
    if (!app.localBots.squad) {
        app.localBots.kinds.reset(); // (the bots' item kinds by type id: made again from these when bots are next asked for)
    }
}

/// The install's tables back in the app's place, if a mod's are there (`bases`: and the content
/// packs' added types again, whenever they aren't the process's).
void restoreInstall(App& app, bool bases)
{
    const bool basesOff = bases && nox::things::addedTypeBases() != nox::content::newThingTypes().bases;
    if (!app.installTables && !basesOff) {
        return;
    }
    LocalHost::finishRetired(0); // (a host that left its game may read the app's tables, and the added types, still)
    if (bases) {
        nox::things::setAddedTypeBases(nox::content::newThingTypes().bases); // (the content packs' again, as at start-up)
    }
    if (!app.installTables) {
        return;
    }
    app.things = std::move(app.installTables->things);
    app.modifiers = std::move(app.installTables->modifiers);
    app.gameData = std::move(app.installTables->gameData);
    app.installTables.reset();
    app.gameDataHash = 0;
    remakeFromTables(app);
    SDL_Log("game data: the install's again");
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
    restoreInstall(app, false); // (the added types: this set's, below; a game we host has them already)
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
    nox::things::useAddedTypeBases(tables); // (the mod's new types' stock types, for this game; a whole thing.bin's: none)
    remakeFromTables(app);
    SDL_Log("game data: this game's mod's own (%zu files) in place of the install's", files.size());
    return {};
}

void useInstallData(App& app)
{
    restoreInstall(app, true);
}

} // namespace noxsdl
