// The note beside the character we play (character_play.hpp).
#include "character_play.hpp"

#include "app.hpp"
#include "local_host.hpp"
#include "menus.hpp"
#include "mode_ui.hpp"

#include "noxcore/client/solo_saves.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/lobby/extended.hpp"
#include "noxcore/modes/registry.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/util/text.hpp"

#include <SDL3/SDL.h>

#include <ctime>
#include <system_error>

namespace noxsdl {

namespace fs = std::filesystem;
namespace charnotes = nox::client::character_notes;

namespace {

/// A line held back at most this long after the last write (a mode that tells one often).
constexpr std::uint64_t kNoteSpacingMs = 5000;

std::int64_t nowSeconds()
{
    return static_cast<std::int64_t>(std::time(nullptr));
}

/// `place` noted beside `plr` (its name and class from its own File Info), played now.
bool notePlace(const fs::path& plr, const charnotes::Place& place)
{
    const nox::client::solo_saves::Slot read = nox::client::solo_saves::readPlr(plr);
    if (!read.exists || !read.infoRead) {
        return false;
    }
    const std::string name = nox::strings::toUtf8(read.info.name);
    const int cls = static_cast<int>(read.info.characterClass);
    charnotes::Notes notes = charnotes::read(plr, name, cls).value_or(charnotes::Notes{});
    notes.name = name;
    notes.characterClass = cls;
    charnotes::played(notes, place, nowSeconds());
    if (!charnotes::write(plr, notes)) {
        SDL_Log("characters: couldn't write %s", nox::pathUtf8(charnotes::fileOf(plr)).c_str());
        return false;
    }
    return true;
}

/// This game's place, with its line, written beside the character we joined with (and the co-op
/// file kept for it).
void writeNow(App& app)
{
    app.characterNoteHeld = false;
    app.characterNoteWrittenMs = SDL_GetTicks();
    if (!app.characterPlace) {
        return;
    }
    notePlace(app.characterPlaceFile, *app.characterPlace);
    if (!app.characterCoopFile.empty()) {
        notePlace(app.characterCoopFile, *app.characterPlace);
    }
}

void begin(App& app, charnotes::Place place)
{
    if (app.characterFile.empty() || !listedCharacter(app, app.characterFile)) {
        return; // (a solo game's slot, a campaign mode's save, none: no note)
    }
    place.id = charnotes::cleanText(place.id, charnotes::kMaxIdBytes);
    place.title = charnotes::cleanText(place.title, charnotes::kMaxTitleBytes);
    if (place.kind == charnotes::Place::Kind::Mode && place.id.empty() && place.title.empty()) {
        return;
    }
    app.characterPlace = std::move(place);
    app.characterPlaceFile = app.characterFile;
    writeNow(app);
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
        return; // (noted already this game)
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
    // (one of our modes by that name: its id, so it's the same place as one hosted here)
    for (const nox::modes::Mode* m : nox::modes::registry().all()) {
        if (m && m->info.name == host.modeName) {
            place.id = m->info.id;
            break;
        }
    }
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
        notePlace(coopFile, *app.characterPlace);
    }
}

void characterSummary(App& app, std::string_view line)
{
    if (!app.characterPlace) {
        return; // (not a character we keep a note for: the line goes nowhere)
    }
    std::string note = charnotes::cleanText(line, charnotes::kMaxNoteBytes);
    if (note == app.characterPlace->note) {
        return;
    }
    app.characterPlace->note = std::move(note);
    if (SDL_GetTicks() - app.characterNoteWrittenMs >= kNoteSpacingMs) {
        writeNow(app);
    } else {
        app.characterNoteHeld = true;
    }
}

void characterNotesDue(App& app)
{
    if (app.characterNoteHeld && SDL_GetTicks() - app.characterNoteWrittenMs >= kNoteSpacingMs) {
        writeNow(app);
    }
}

void characterNotesEnd(App& app)
{
    if (app.characterNoteHeld) {
        writeNow(app);
    }
    app.characterPlace.reset();
    app.characterPlaceFile.clear();
    app.characterCoopFile.clear();
    app.characterNoteHeld = false;
}

} // namespace noxsdl
