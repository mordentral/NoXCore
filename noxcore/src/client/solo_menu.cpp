// The solo game menu's file work: whether solo maps are installed, counting and listing the
// characters in Save, deleting a slot, and writing a new character's first Player.plr.
#include "noxcore/client/solo_menu.hpp"

#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/server/solo_save.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <system_error>
#include <vector>

namespace nox::client::solo_menu {

namespace fs = std::filesystem;

bool hasSoloMaps(const fs::path& gameDir)
{
    std::error_code ec;
    return fs::exists(resolveNoCase(gameDir / "maps" / "con01a" / "con01a.map"), ec); // (_stat == 0)
}

int countSaveFiles(const fs::path& saveRoot)
{
    std::error_code ec;
    fs::create_directory(saveRoot / "Save", ec); // (CreateDirectoryA)
    int count = fs::exists(saveRoot / "Save" / "AUTOSAVE" / "Player.plr", ec) ? 1 : 0; // (_access(path, 0))
    // (The 13 "%s\Save\SAVE%04d\Player.plr" given only the folder: SAVE<the caller's EDI>, not a
    // slot: nothing more counted.)
    return count;
}

std::string_view firstMap(int characterClass)
{
    switch (characterClass) {
    case 0:
        return "War01a";
    case 1:
        return "Wiz01a";
    case 2:
        return "Con01a";
    default:
        return {};
    }
}

void folderOf(std::string_view path, std::string& out)
{
    const auto slash = path.rfind('\\');
    if (slash == std::string_view::npos) {
        return; // (strrchr found none: left as it was)
    }
    // _strncpy(out, slash - 8, 8): what's before it (a shorter path's start reads before the
    // string in the exe; here from its start), cut at a zero.
    const std::size_t from = slash >= 8 ? slash - 8 : 0;
    std::string s(path.substr(from, slash - from));
    if (const auto zero = s.find('\0'); zero != std::string::npos) {
        s.resize(zero);
    }
    out = s;
}

DeleteAnswer soloDelete(std::string& remembered, const solo_saves::Slot& slot)
{
    // The entry's path: the file's when its read found it, else empty.
    const std::string path = slot.exists ? slot.plr.string() : std::string();
    std::string withBackslashes = path;
    for (char& c : withBackslashes) {
        if (c == '/') {
            c = '\\';
        }
    }
    folderOf(withBackslashes, remembered);
    if (remembered == "AUTOSAVE") {
        return DeleteAnswer::NotAllowed;
    }
    return path.empty() ? DeleteAnswer::NoCanDo : DeleteAnswer::Ask;
}

bool listedForGame(std::uint32_t fileFlags, bool questGame)
{
    // A quest character for a quest game, any other for the rest.
    // File Info flag 4 marks a quest character.
    return ((fileFlags & 4u) != 0) == questGame;
}

namespace {

/// The Save folder made (CreateDirectoryA), then FindFirstFileA("*.plr")'s files, folders
/// skipped (attribute 0x10), in the order NTFS gives them (by name, letters' case ignored: here
/// the same on any file system).
std::vector<fs::path> plrFiles(const fs::path& saveFolder)
{
    std::vector<fs::path> out;
    std::error_code ec;
    fs::create_directory(saveFolder, ec);
    for (const auto& e : fs::directory_iterator(saveFolder, ec)) {
        auto ext = e.path().extension().string();
        for (auto& c : ext) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (ext != ".plr" || e.is_directory(ec)) {
            continue;
        }
        out.push_back(e.path());
    }
    const auto upper = [](const fs::path& p) {
        std::u16string s = p.filename().u16string();
        for (auto& c : s) {
            if (c >= u'a' && c <= u'z') {
                c = static_cast<char16_t>(c - u'a' + u'A');
            }
        }
        return s;
    };
    std::ranges::sort(out, [&](const fs::path& a, const fs::path& b) { return upper(a) < upper(b); });
    return out;
}

/// A record as calloc or the exe's zeroing leaves it: every field 0.
player::FileInfo zeroInfo()
{
    player::FileInfo info{};
    info.flags = 0;
    info.version = 0;
    info.level = 0;
    return info;
}

/// client_countPlayerFiles02 / 04: one record for every file (on the stack, never
/// cleared: its first flags are the stack's, 0 here), each file read into it.
int countFlagged(const fs::path& saveFolder, std::uint32_t flag)
{
    int count = 0;
    std::uint32_t flags = 0;
    for (const auto& plr : plrFiles(saveFolder)) {
        const auto read = solo_saves::readPlr(plr);
        if (read.exists && read.infoRead) {
            flags = read.info.flags; // (else the record as the last file left it)
        }
        if (flags & flag) {
            ++count;
        }
    }
    return count;
}

} // namespace

int countQuestCharacters(const fs::path& saveFolder)
{
    return countFlagged(saveFolder, 4u);
}

int countNetworkCharacters(const fs::path& saveFolder)
{
    return countFlagged(saveFolder, 2u);
}

std::vector<Character> listCharacters(const fs::path& saveFolder, bool questGame)
{
    std::vector<Character> out;
    Character record{{}, zeroInfo()}; // (the array's next record: zeroed)
    for (const auto& plr : plrFiles(saveFolder)) {
        const auto read = solo_saves::readPlr(plr);
        if (read.exists) {
            if (read.infoRead) {
                record.info = read.info; // (as parseFileInfoData read it)
            }
            record.path = plr; // (the path copied in at the end)
        }
        if (listedForGame(record.info.flags, questGame)) {
            out.push_back(std::move(record));
            record = Character{{}, zeroInfo()};
        }
    }
    // Sorted (qsort) with the later time first, field by field (the weekday
    // skipped).
    const auto when = [](const Character& c) {
        const auto& t = c.info.time;
        return std::array<std::uint16_t, 7>{t[0], t[1], t[3], t[4], t[5], t[6], t[7]};
    };
    std::stable_sort(out.begin(), out.end(), [&](const Character& a, const Character& b) { return when(a) > when(b); });
    return out;
}

std::uint32_t fileInfoFlags(std::uint32_t held, bool network, bool questGame)
{
    if (!network) {
        return (held & ~6u) | 1u;
    }
    return (held & ~1u) | (questGame ? 4u : 2u);
}

fs::path writeNewCharacter(const fs::path& saveRoot, const player::FileInfo& held, const player::Gui& gui,
                           const player::Music& music, std::string_view mapName, const std::array<std::uint16_t, 8>& now)
{
    server::saves::remove(saveRoot, "WORKING", false);
    std::error_code ec;
    const fs::path working = saveRoot / "Save" / "WORKING";
    fs::create_directory(saveRoot / "Save", ec);
    fs::create_directory(working, ec);
    const fs::path plr = working / "Player.plr";
    // (The path as the exe builds it: Game_GetWorkDir's, "\Save\", "WORKING\", "Player.plr".)
    std::string path = plr.string();
    for (char& c : path) {
        if (c == '/') {
            c = '\\';
        }
    }
    const player::FileInfo info = solo_saves::saveFileInfo(held, path, now, 1, std::string(mapName), held.questStage);
    std::vector<std::uint8_t> bytes;
    try {
        bytes = solo_saves::clientSectionBytes(gui, info, music);
    } catch (const std::exception&) {
        return {};
    }
    crypto::cipherForKey(crypto::FileKey::PlayerFile).encrypt(bytes);
    std::ofstream f(plr, std::ios::binary | std::ios::trunc); // (cryptOpen(path, 0))
    if (!f) {
        return {};
    }
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return f ? plr : fs::path{};
}

fs::path continueMap(const fs::path& saveRoot, const fs::path& gameDir, std::string_view mapName)
{
    const std::string file = std::string(mapName) + ".map";
    if (const auto copy = server::savedMapCopy(saveRoot, file)) {
        return *copy;
    }
    return gameDir / "maps" / std::string(mapName) / file; // (read in any case: readFileBytes)
}

} // namespace nox::client::solo_menu
