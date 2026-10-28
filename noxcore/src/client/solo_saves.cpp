// The solo save slots as the host's client reads them (File Info only), and the client's
// own sections of a .plr written or appended. See solo_saves.hpp for the formats.
#include "noxcore/client/solo_saves.hpp"

#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/util/bytes.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nox::client::solo_saves {

namespace fs = std::filesystem;

namespace {

// The .plr is enciphered in whole cipher blocks; every section header starts on one.
constexpr std::size_t kBlock = crypto::Blowfish::kBlockSize;

using util::u32At;

/// A SYSTEMTIME's moment without its weekday (SystemTimeToFileTime's order).
std::array<std::uint16_t, 7> moment(const std::array<std::uint16_t, 8>& t)
{
    return {t[0], t[1], t[3], t[4], t[5], t[6], t[7]};
}

} // namespace

std::string slotName(int index)
{
    if (index == 0) {
        return "AUTOSAVE";
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "SAVE%04d", index);
    return buf;
}

Slot read(const fs::path& gameDir, int index)
{
    Slot slot = readPlr(gameDir / "Save" / slotName(index) / "Player.plr");
    slot.name = slotName(index);
    return slot;
}

Slot readPlr(const fs::path& plr)
{
    // The file opened (cryptOpen), its sections walked to the 0 id,
    // File Info read (parseFileInfoData: a version past 12 fails it), the others
    // skipped by their sizes; then the path kept (a slot that's there).
    Slot slot;
    slot.plr = plr;
    slot.info = player::FileInfo{};
    slot.info.flags = 0;
    slot.info.version = 0;
    slot.info.level = 0;
    std::vector<std::uint8_t> bytes;
    {
        std::ifstream f(slot.plr, std::ios::binary);
        if (!f) {
            return slot;
        }
        bytes.assign(std::istreambuf_iterator<char>(f), {});
    }
    bytes.resize(bytes.size() / kBlock * kBlock); // (the binfile reads whole blocks)
    crypto::cipherForKey(crypto::FileKey::PlayerFile).decrypt(bytes);
    std::size_t pos = 0;
    // Each section: u32 id, zeros to the next block, u64 size (its low 32 bits read), then
    // `size` bytes of data. A 0 id ends the file.
    while (pos + 4 <= bytes.size()) {
        const std::uint32_t id = u32At(bytes, pos);
        if (id == 0) {
            break;
        }
        const std::size_t sizeAt = (pos + 4 + kBlock - 1) / kBlock * kBlock;
        if (sizeAt + 8 > bytes.size()) {
            break;
        }
        const std::uint32_t size = u32At(bytes, sizeAt);
        if (size > bytes.size() - (sizeAt + 8)) {
            break;
        }
        if (id == static_cast<std::uint32_t>(player::SectionId::FileInfo)) {
            try {
                slot.info = player::FileInfo::parse(std::span(bytes).subspan(sizeAt + 8, size));
            } catch (const std::exception&) {
                return slot; // (the parse's 0: not there)
            }
            if (slot.info.version > 12) {
                return slot;
            }
            slot.infoRead = true;
        }
        pos = sizeAt + 8 + size;
    }
    slot.exists = true;
    return slot;
}

std::vector<Slot> list(const fs::path& gameDir)
{
    std::vector<Slot> out;
    for (int i = 0; i < kSlots; ++i) {
        out.push_back(read(gameDir, i));
    }
    return out;
}

int latest(const std::vector<Slot>& slots)
{
    // Strictly later replaces (CompareFileTime == 1): the first of equal times stays.
    int best = -1;
    for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
        const Slot& s = slots[static_cast<std::size_t>(i)];
        if (!s.exists) {
            continue;
        }
        // Arrays compare field by field: year, month, day, hour, minute, second, milliseconds.
        if (best < 0 || moment(s.info.time) > moment(slots[static_cast<std::size_t>(best)].info.time)) {
            best = i;
        }
    }
    return best;
}

int lineColor(const Slot& slot)
{
    if (!slot.exists) {
        return 3;
    }
    if (slot.info.flags & 8u) {
        return 10;
    }
    return slot.name == "AUTOSAVE" ? 13 : 14;
}

std::string_view stageLabel(int stage, int cls)
{
    // Stages 1-3 and 11 name a different place per class (0 warrior, 1 wizard, 2 conjurer).
    switch (stage) {
    case 1:
        return cls == 0 ? "WolChat.c:Nox_Lobby_3" : cls == 1 ? "NPC:Horvath" : "Noxworld.wnd:VillageIx";
    case 2:
        return cls == 0 ? "War02a.scr:Sign5" : cls == 1 ? "Noxworld.wnd:Galava" : "NPC:Aldwyn";
    case 3:
        return cls == 0 ? "Noxworld.wnd:VillageIx" : cls == 1 ? "WolChat.c:Nox_Lobby_9" : "Noxworld.wnd:Mines";
    case 4:
        return "Noxworld.wnd:FieldValor";
    case 5:
        return "Noxworld.wnd:HamletBrin";
    case 6:
        return "NPC:Horrendous";
    case 7:
        return "Noxworld.wnd:Galava";
    case 8:
        return "Noxworld.wnd:TempleIx";
    case 9:
        return "Noxworld.wnd:DismalSwamp";
    case 10:
        return "Noxworld.wnd:LandDead";
    case 11:
        return cls == 0 ? "WolChat.c:Nox_Lobby_47" : cls == 1 ? "NPC:Hecubah" : "Noxworld.wnd:GrokkTorr";
    default:
        return "GuiInv.c:IdentifyUnknown";
    }
}

std::string_view className(int cls)
{
    // (a byte past 2 reads past the table: the exe's own)
    static constexpr std::string_view kNames[3] = {"Warrior", "Wizard", "Conjurer"};
    return cls >= 0 && cls < 3 ? kNames[cls] : std::string_view{};
}

std::string whenText(const std::array<std::uint16_t, 8>& t)
{
#ifdef _WIN32
    SYSTEMTIME st{t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]};
    char date[256] = {};
    char time[256] = {};
    GetDateFormatA(LOCALE_SYSTEM_DEFAULT, DATE_SHORTDATE, &st, nullptr, date, sizeof date);
    GetTimeFormatA(LOCALE_SYSTEM_DEFAULT, TIME_NOSECONDS | TIME_NOTIMEMARKER | TIME_FORCE24HOURFORMAT, &st, nullptr, time,
                   sizeof time);
    return std::string(date) + ", " + time + " ";
#else
    char buf[64];
    std::snprintf(buf, sizeof buf, "%u/%u/%u, %u:%02u ", t[1], t[3], t[0], t[4], t[5]);
    return buf;
#endif
}

std::array<std::uint16_t, 8> localTime()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    return {static_cast<std::uint16_t>(tm.tm_year + 1900), static_cast<std::uint16_t>(tm.tm_mon + 1),
            static_cast<std::uint16_t>(tm.tm_wday),        static_cast<std::uint16_t>(tm.tm_mday),
            static_cast<std::uint16_t>(tm.tm_hour),        static_cast<std::uint16_t>(tm.tm_min),
            static_cast<std::uint16_t>(tm.tm_sec),         static_cast<std::uint16_t>(ms)};
}

player::FileInfo saveFileInfo(player::FileInfo held, const std::string& plrPath, const std::array<std::uint16_t, 8>& now,
                              std::uint8_t level, const std::string& mapName, std::uint8_t stage, std::uint32_t gameFlags)
{
    player::FileInfo f = std::move(held);
    f.version = 12;
    // Game flag 0x2000 is a network game, 0x1000 a quest; File Info flags 1 solo, 2 network, 4 quest.
    if (gameFlags & 0x2000u) {
        f.flags = (f.flags & ~1u) | ((gameFlags & 0x1000u) ? 4u : 2u); // (parseFileInfoData in the network)
    } else {
        f.flags = (f.flags & ~8u & ~6u) | 1u; // (the save's & ~8; parseFileInfoData out of the network)
    }
    f.savePath = plrPath;                 // (clientCurSave: the client sections' file)
    f.time = now;                         // (GetLocalTime)
    f.isFemale = 0;                       // (cleared)
    f.level = level;
    f.mapName = mapName;                  // (Map_GetCurrentName)
    f.questStage = stage;
    return f;
}

std::vector<std::uint8_t> clientSectionBytes(const player::Gui& gui, const player::FileInfo& info, const player::Music& music)
{
    ByteWriter w;
    const auto section = [&](player::SectionId id, const std::vector<std::uint8_t>& data) {
        w.u32(static_cast<std::uint32_t>(id));
        w.align(kBlock);
        w.u64(data.size());
        w.bytes(data);
    };
    section(player::SectionId::Gui, gui.serialize());
    section(player::SectionId::FileInfo, info.serialize());
    section(player::SectionId::Music, music.serialize());
    w.u32(0);
    w.align(kBlock);
    return std::move(w).take();
}

KeptSave keepSaveData(const fs::path& saveDir, const fs::path& plr, std::span<const std::uint8_t> data, bool host)
{
    if (!host && data.size() < kMinSaveData) {
        return KeptSave::TooShort;
    }
    std::error_code ec;
    fs::create_directory(saveDir, ec); // (CreateDirectoryA: its failure not looked at)
    // Written a byte at a time: the low 16 bits of the size (a short), each whole
    // block enciphered as it fills; the close fills the last with zeros.
    std::vector<std::uint8_t> bytes(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(data.size() & 0xFFFFu));
    bytes.resize((bytes.size() + kBlock - 1) / kBlock * kBlock, 0);
    crypto::cipherForKey(crypto::FileKey::PlayerFile).encrypt(bytes);
    std::ofstream f(plr, std::ios::binary | std::ios::trunc);
    if (!f) {
        return KeptSave::Unwritten;
    }
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return f ? KeptSave::Written : KeptSave::Unwritten;
}

bool appendClientSections(const fs::path& plr, const player::Gui& gui, const player::FileInfo& info, const player::Music& music)
{
    std::vector<std::uint8_t> bytes;
    try {
        bytes = clientSectionBytes(gui, info, music);
    } catch (const std::exception&) {
        return false;
    }
    crypto::cipherForKey(crypto::FileKey::PlayerFile).encrypt(bytes);
    std::error_code ec;
    if (!fs::exists(plr, ec)) {
        return false; // (cryptOpen's append: the file must be there)
    }
    std::ofstream f(plr, std::ios::binary | std::ios::app);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

} // namespace nox::client::solo_saves
