// The game's files as the Content Editor opens them (game_files.hpp).
#include "content/game_files.hpp"

#include "noxcore/io/file.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <exception>
#include <functional>
#include <system_error>

namespace fs = std::filesystem;

namespace videotools::content {

using nox::findNoCase;

namespace {

/// Files in `dir` (not below) whose extension is `ext` (lower case, with the dot), sorted by name.
std::vector<fs::path> filesIn(const fs::path& dir, std::string_view ext)
{
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && nox::util::lowered(it->path().extension().string()) == ext) {
            out.push_back(it->path());
        }
    }
    std::ranges::sort(out, [](const fs::path& a, const fs::path& b) {
        return nox::util::lowered(a.filename().string()) < nox::util::lowered(b.filename().string());
    });
    return out;
}

std::vector<fs::path> subfolders(const fs::path& dir)
{
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) {
            out.push_back(it->path());
        }
    }
    std::ranges::sort(out, [](const fs::path& a, const fs::path& b) {
        return nox::util::lowered(a.filename().string()) < nox::util::lowered(b.filename().string());
    });
    return out;
}

bool present(const fs::path& p)
{
    std::error_code ec;
    return fs::exists(p, ec);
}

} // namespace

const char* fileKindLabel(FileKind kind)
{
    switch (kind) {
    case FileKind::Things: return "Things";
    case FileKind::Modifiers: return "Weapons & armour";
    case FileKind::GameData: return "Tuning";
    case FileKind::Monsters: return "Monsters";
    case FileKind::SoundSets: return "Sound sets";
    case FileKind::Strings: return "Strings";
    case FileKind::Images: return "Images / HD";
    case FileKind::Audio: return "Audio";
    case FileKind::Dialog: return "Dialog";
    case FileKind::Windows: return "Windows (.wnd)";
    case FileKind::Maps: return "Maps";
    case FileKind::Rules: return "Rules (.rul)";
    case FileKind::Characters: return "Characters";
    case FileKind::Mods: return "Mods";
    case FileKind::Text: return "Map cycle / MOTD";
    case FileKind::Fonts: return "Fonts";
    case FileKind::Palette: return "Palette";
    }
    return "?";
}

const char* fileKindFile(FileKind kind)
{
    switch (kind) {
    case FileKind::Things: return "thing.bin";
    case FileKind::Modifiers: return "modifier.bin";
    case FileKind::GameData: return "gamedata.bin";
    case FileKind::Monsters: return "monster.bin";
    case FileKind::SoundSets: return "soundset.bin";
    case FileKind::Strings: return "nox.csf";
    case FileKind::Images: return "video.bag";
    case FileKind::Audio: return "Audio.bag";
    case FileKind::Dialog: return "Dialog";
    case FileKind::Windows: return "window";
    case FileKind::Maps: return "maps";
    case FileKind::Rules: return "*.rul";
    case FileKind::Characters: return "Save";
    case FileKind::Mods: return "mods";
    case FileKind::Text: return "mapcycle.txt, motd.txt";
    case FileKind::Fonts: return "*.fnt";
    case FileKind::Palette: return "default.pal";
    }
    return "?";
}

const FileStatus* GameFiles::statusOf(FileKind kind) const
{
    for (const auto& s : status_) {
        if (s.kind == kind) {
            return &s;
        }
    }
    return nullptr;
}

GameFiles GameFiles::load(const GameFileDirs& dirs)
{
    GameFiles g;
    g.dirs_ = dirs;
    const fs::path& game = dirs.gameDir;

    // One reader each: a failure goes in the file's status and the rest still load.
    const auto read = [&](FileKind kind, std::string_view name, bool optional, const std::function<void(const fs::path&)>& fn) {
        FileStatus s;
        s.kind = kind;
        s.path = findNoCase(game, name);
        s.optional = optional;
        if (!present(s.path)) {
            s.error = "not found";
        } else {
            try {
                fn(s.path);
                s.loaded = true;
            } catch (const std::exception& ex) {
                s.error = ex.what();
            }
        }
        g.status_.push_back(std::move(s));
    };
    read(FileKind::Things, "thing.bin", false, [&](const fs::path& p) { g.things_ = nox::things::loadThingDb(p); });
    read(FileKind::Modifiers, "modifier.bin", false, [&](const fs::path& p) { g.modifiers_ = nox::things::loadModifierDb(p); });
    read(FileKind::GameData, "gamedata.bin", false, [&](const fs::path& p) { g.gameData_ = nox::things::loadGameData(p); });
    read(FileKind::Monsters, "monster.bin", false, [&](const fs::path& p) {
        g.monstersSolo_ = nox::things::loadMonsterDb(p, true);
        g.monstersArena_ = nox::things::loadMonsterDb(p, false);
    });
    read(FileKind::SoundSets, "soundset.bin", false, [&](const fs::path& p) { g.soundSets_ = nox::things::loadSoundSets(p); });
    read(FileKind::Strings, "nox.csf", false, [&](const fs::path& p) { g.strings_ = nox::strings::StringTable::load(p); });
    read(FileKind::Audio, "Audio.bag", false, [&](const fs::path&) {
        g.audio_ = std::make_shared<nox::audio::AudioBag>(nox::audio::AudioBag::openGameDir(game));
    });

    const fs::path dialogDir = findNoCase(game, "Dialog");
    g.dialog_ = filesIn(dialogDir, ".wav");
    g.windows_ = filesIn(findNoCase(game, "window"), ".wnd");
    g.rules_ = filesIn(game, ".rul");
    g.fonts_ = filesIn(game, ".fnt");
    g.palettes_ = filesIn(game, ".pal");
    for (const char* t : {"mapcycle.txt", "motd.txt"}) {
        if (const fs::path p = findNoCase(game, t); present(p)) {
            g.texts_.push_back(p);
        }
    }
    // Maps: maps\<name>\<name>.map (a folder's own map), and each map's .rul beside it.
    for (const fs::path& folder : subfolders(findNoCase(game, "maps"))) {
        const std::string name = folder.filename().string();
        if (const fs::path m = findNoCase(folder, name + ".map"); present(m)) {
            g.maps_.push_back(m);
        } else if (const fs::path z = findNoCase(folder, name + ".nxz"); present(z)) {
            g.maps_.push_back(z);
        }
        for (auto& r : filesIn(folder, ".rul")) {
            g.rules_.push_back(std::move(r));
        }
    }
    const fs::path saveDir = dirs.saveDir.empty() ? findNoCase(game, "Save") : dirs.saveDir;
    g.characters_ = filesIn(saveDir, ".plr");
    const fs::path& modsDir = dirs.modsDir; // (none given: no mods listed; never the game folder's)
    g.mods_ = subfolders(modsDir);
    return g;
}

} // namespace videotools::content
