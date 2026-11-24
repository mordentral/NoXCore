// The labels of the character we play (character_play.hpp).
#include "character_play.hpp"

#include "app.hpp"
#include "local_host.hpp"
#include "menus.hpp"
#include "mode_ui.hpp"

#include "noxcore/client/solo_saves.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/lobby/extended.hpp"
#include "noxcore/modes/registry.hpp"
#include "noxcore/player/character_store.hpp"
#include "noxcore/util/text.hpp"

#include <SDL3/SDL.h>

#include <ctime>
#include <system_error>

namespace noxsdl {

namespace fs = std::filesystem;
namespace charnotes = nox::client::character_notes;
namespace store = nox::player::character_store;

namespace {

std::int64_t nowSeconds()
{
    return static_cast<std::int64_t>(std::time(nullptr));
}

/// `plr`'s labels as its file has them (none: no file, none there, or ones that don't read).
charnotes::Notes labelsOf(const fs::path& plr)
{
    const nox::client::solo_saves::Slot read = nox::client::solo_saves::readPlr(plr);
    if (!read.exists || read.labels.empty()) {
        return {};
    }
    return charnotes::decode(read.labels).value_or(charnotes::Notes{});
}

/// This game's place put in `labels` (played now) and kept for `plr`'s next write.
void record(const fs::path& plr, charnotes::Notes& labels, const charnotes::Place& place)
{
    charnotes::played(labels, place, nowSeconds());
    store::setLabels(plr, charnotes::encode(labels));
}

void begin(App& app, charnotes::Place place)
{
    if (app.characterFile.empty() || !listedCharacter(app, app.characterFile)) {
        return; // (a solo game's slot, a campaign mode's save, none: no labels)
    }
    place.id = charnotes::cleanText(place.id, charnotes::kMaxIdBytes);
    place.title = charnotes::cleanText(place.title, charnotes::kMaxTitleBytes);
    if (place.kind == charnotes::Place::Kind::Mode && place.id.empty() && place.title.empty()) {
        return;
    }
    app.characterPlace = std::move(place);
    app.characterPlaceFile = app.characterFile;
    app.characterGameFlags = app.live ? gameFlags(app) : 0u; // (its end writes the labels or drops them: characterNotesEnd)
    app.characterLabels = labelsOf(app.characterPlaceFile);
    record(app.characterPlaceFile, app.characterLabels, *app.characterPlace);
    const charnotes::Place& p = *app.characterPlace;
    SDL_Log("characters: %s played at %s%s", nox::pathUtf8(app.characterPlaceFile.filename()).c_str(),
            p.kind == charnotes::Place::Kind::Coop ? "co-op, " : "",
            !p.title.empty() ? p.title.c_str() : !p.id.empty() ? p.id.c_str() : "the game's own campaign");
}

} // namespace

bool listedCharacter(const App& app, const fs::path& plr)
{
    if (!nox::util::equalsNoCase(plr.extension().string(), ".plr")) {
        return false;
    }
    const fs::path folder = app.menus ? app.menus->saveDir() : app.saveRoot / "Save";
    std::error_code ec;
    return !folder.empty() && fs::equivalent(plr.parent_path(), folder, ec) && !ec;
}

void characterPlayedHere(App& app)
{
    if (app.characterPlace) {
        return; // (labelled already this game)
    }
    charnotes::Place place;
    if (app.host) {
        const std::string& id = app.host->modeId();
        if (id.empty()) {
            return; // (a stock game or the arena: nothing)
        }
        if (const ModeUiEntry* e = modeUiEntry(id); e && e->soloScreens) {
            return; // (a campaign mode hosted through the solo screens: a slot, never listed; its Hello names the campaign to the guests)
        }
        const nox::modes::Mode* mode = nox::modes::registry().find(id);
        place.id = id;
        place.title = mode ? mode->info.name : id;
        begin(app, std::move(place));
        return;
    }
    if (!app.live || !app.live->extended()) {
        return; // (a stock host)
    }
    const nox::lobby::ExtendedServer& host = *app.live->extended();
    if ((host.features & nox::lobby::ServerFeature::CoopCampaign) != 0 || host.modeName.empty()) {
        return; // (the co-op campaign: its Hello says which; no mode: nothing)
    }
    place.title = host.modeName;
    place.id = modeIdByName(host.modeName); // (one of our modes by that name: its id, so it's the same place as one hosted here)
    begin(app, std::move(place));
}

void characterPlayedCoop(App& app, std::string_view campaignId, std::string_view title, const fs::path& coopFile)
{
    if (!app.characterPlace) {
        charnotes::Place place;
        place.kind = charnotes::Place::Kind::Coop;
        place.id = std::string(campaignId);
        place.title = std::string(title);
        begin(app, std::move(place));
    }
    if (app.characterPlace && !coopFile.empty() && coopFile != app.characterPlaceFile && app.characterCoopFile != coopFile &&
        listedCharacter(app, coopFile)) {
        app.characterCoopFile = coopFile;
        app.characterCoopLabels = labelsOf(coopFile); // (before its next write: kept by it)
        record(coopFile, app.characterCoopLabels, *app.characterPlace);
    }
}

void characterSummary(App& app, std::string_view line)
{
    if (!app.characterPlace) {
        return; // (not a character we keep labels for: the line goes nowhere)
    }
    std::string note = charnotes::cleanText(line, charnotes::kMaxNoteBytes);
    if (note == app.characterPlace->note) {
        return;
    }
    app.characterPlace->note = std::move(note);
    record(app.characterPlaceFile, app.characterLabels, *app.characterPlace);
    if (!app.characterCoopFile.empty()) {
        record(app.characterCoopFile, app.characterCoopLabels, *app.characterPlace);
    }
}

void characterNotesEnd(App& app)
{
    for (const fs::path& plr : {app.characterPlaceFile, app.characterCoopFile}) {
        if (plr.empty()) {
            continue;
        }
        // (Only where the original game writes the character at its end, a network game that isn't
        // a quest; elsewhere labels no write took are dropped.)
        if (store::flushLabels(plr, app.characterGameFlags)) {
            SDL_Log("characters: labels written into %s", nox::pathUtf8(plr.filename()).c_str());
        }
    }
    app.characterGameFlags = 0;
    app.characterPlace.reset();
    app.characterPlaceFile.clear();
    app.characterLabels = {};
    app.characterCoopFile.clear();
    app.characterCoopLabels = {};
}

} // namespace noxsdl
