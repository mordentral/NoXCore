// MSG_NOXCPP_COOP (0xF7), noxcpp's own: the co-op campaign's messages between our server and the
// clients that identified themselves (noxsdl). {u8 0xF7, u8 sub, u16 the rest's length, the rest},
// reliable. A stock client never gets one: it's told the stock equivalents instead (coop.hpp's
// presentation layer), or nothing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nox::coop::coopmsg {

inline constexpr std::uint8_t kOp = 0xF7;

// ---- server to client
/// The campaign as the player joins it: {u8 the campaign's class (0 warrior, 1 wizard, 2 conjurer),
/// u8 the most players, u8 flags (1: you are the host, 2: the campaign kept your character: don't
/// send yours), u16 the host's net code}.
inline constexpr std::uint8_t kHello = 1;
/// A line the co-op rules tell a player, to show as a notice: {u8 kind (Notice), u8 length, its
/// UTF-8 text}.
inline constexpr std::uint8_t kNotice = 2;
/// The party: {u8 count, each {u16 net code, u8 class, u8 level, u8 flags (Roster::Member), u16
/// seconds till a respawn (0: none), u8 length, the name's UTF-8}}.
inline constexpr std::uint8_t kRoster = 3;

/// One of the host's own client's calls a guest sees too (Cine): {u8 call, u8 a, u16 b, u16 the
/// actor's net code, u8 length, text}.
inline constexpr std::uint8_t kCine = 4;
/// A chapter screen (StartupScreen / DeathScreen): {u8 chapter (the script's less one), u8 kind
/// (1 a chapter's start, 0 its loss)}. A NoX Core guest shows it as MSG_CHAPTER_END's, and at its
/// end tells the host it's done (kChapterReady); a loss ends on "waiting for the host".
inline constexpr std::uint8_t kChapter = 5;
/// Who is still reading the chapter screen: {u8 still reading, u8 of how many}.
inline constexpr std::uint8_t kReadyState = 7;
/// The host's talk, read-only for the others: {u8 1 open / 0 closed, u16 the talker's net code,
/// u16 the NPC's, u8 length, the text's string-table id}.
inline constexpr std::uint8_t kDialog = 8;
/// A guest's campaign character as the host's save wrote it, for the guest to keep, in parts (a
/// reliable message's room): {u16 the whole's size, u16 this part's place in it, its bytes}; the
/// whole is its server sections as a quest's SAVEDATA carries them.
inline constexpr std::uint8_t kCharacter = 10;
/// The party's creatures: every party member's summons and charmed creatures (those in its summon
/// panel), for the friend dot over them: {u8 count, each u16 net code}, the whole list each time
/// it changes (and as a player comes in). An older client lets an unknown sub go.
inline constexpr std::uint8_t kCreatures = 11;
/// The most net codes kCreatures carries (a party of 4 controls 20 at most: 5 each).
inline constexpr std::size_t kCreaturesMost = 64;
/// What a guest's minimap had seen of the map it just loaded, as the campaign kept it (its own
/// reports, kSeenReport; its saves' Party\<file key>.seen), at its CLIENT_READY, in parts: each a
/// SeenCells body (seenParts), added to what the guest has seen. An older client lets it go.
inline constexpr std::uint8_t kSeen = 13;

// ---- client to server
/// A guest's chapter screen is over (clicked away, or its time ran out): {}.
inline constexpr std::uint8_t kChapterReady = 6;
/// The host's commands from its windows: {u8 HostCmd}.
inline constexpr std::uint8_t kHostCmd = 9;
/// A guest's minimap: the walls it drew and the obelisks it marked for the first time since its
/// last report, about once a second, a SeenCells body each (seenParts). An older host lets it go.
inline constexpr std::uint8_t kSeenReport = 12;

enum class HostCmd : std::uint8_t {
    SkipBriefing = 1, ///< the chapter screen's wait over for everyone
};

/// The host's client's calls (server/host_client.hpp) as a guest is told them.
enum class CineCall : std::uint8_t {
    Blind = 1,
    UnBlind = 2,
    FadeToBlack = 3, ///< a: over the menus, b: frames
    MapDrawn = 4,    ///< a: drawn
    WideScreen = 5,  ///< a: on; the actor's net code (the host's)
    MapIntro = 6,    ///< a: ImmediateBlind's
    EndGame = 7,     ///< a: the ending's kind
    Voice = 8,       ///< a: volume; text: the voice's file
    StopAllFades = 9,
};
struct Cine {
    CineCall call = CineCall::Blind;
    std::uint8_t a = 0;
    std::uint16_t b = 0;
    std::uint16_t actor = 0;
    std::string text;
    bool operator==(const Cine&) const = default;
};
struct Chapter {
    std::uint8_t chapter = 0;
    bool startup = true;
    bool operator==(const Chapter&) const = default;
};
struct DialogMirror {
    bool open = false;
    std::uint16_t talker = 0;
    std::uint16_t npc = 0;
    std::string text;
    bool operator==(const DialogMirror&) const = default;
};

/// What a notice is about (its text comes with it; noxsdl may show each kind its own way).
enum class Notice : std::uint8_t {
    Rules = 1,        ///< the join notice: only the host starts story scenes
    HostOnly = 2,     ///< a guest touched something only the host can set off
    TalkRefused = 3,  ///< a click on an NPC refused: a talk that plays a scene (the host's), or another's talk under way
    Info = 4,         ///< anything else
};

struct Hello {
    std::uint8_t campaignClass = 0;
    std::uint8_t maxPlayers = 4;
    bool youAreHost = false;
    bool characterKept = false; ///< the campaign has the player's character (a returning one's)
    std::uint16_t hostNetCode = 0;
    bool operator==(const Hello&) const = default;
};

struct Roster {
    struct Member {
        std::uint16_t netCode = 0;
        std::uint8_t playerClass = 0;
        std::uint8_t level = 1;
        /// 1 the host, 2 dead, 4 a NoX Core client
        std::uint8_t flags = 0;
        std::uint16_t respawnIn = 0;
        std::string name;
        bool operator==(const Member&) const = default;
    };
    std::vector<Member> members;
    bool operator==(const Roster&) const = default;
};

/// Names and texts at most this many bytes (the message stays well within a reliable packet).
inline constexpr std::size_t kTextMost = 200;
inline constexpr std::size_t kNameMost = 32;
/// A character's file at most this many bytes (none comes near it), sent this many at a time.
inline constexpr std::size_t kCharacterMost = 60000;
inline constexpr std::size_t kCharacterPart = 800;
struct CharacterPart {
    std::uint16_t size = 0;
    std::uint16_t at = 0;
    std::vector<std::uint8_t> bytes;
};

/// Part of what a player's minimap has seen of a map (kSeen, kSeenReport, a .seen file's sections):
/// {u8 length, the map's name (its file's, any case), u16 n, n wall cells as varints (each cell's
/// key, x << 8 | y, less the one before it and 1; the first's less -1: 7 bits a byte, low first,
/// the top bit "more"), u8 m, m u16 obelisks (a map object's extent, 0x7FFF or less)}. The cells
/// in key order, none twice. A part stands alone: parts are added together.
struct SeenCells {
    std::string map;
    std::vector<std::uint16_t> walls;    ///< the cells' keys, in order
    std::vector<std::uint16_t> obelisks; ///< the extents, in order
    bool operator==(const SeenCells&) const = default;
};
/// A part's wall cells at most this many bytes, its obelisks at most this many (a reliable
/// packet's room).
inline constexpr std::size_t kSeenPart = 800;
inline constexpr std::size_t kSeenPartObelisks = 32;
/// The bodies of the parts that carry `walls` and `obelisks` (any order, twice or not; obelisks
/// past 0x7FFF left out) for `map` (its name cut to kNameMost); none when there's nothing.
std::vector<std::vector<std::uint8_t>> seenParts(std::string_view map, std::span<const std::uint16_t> walls,
                                                 std::span<const std::uint16_t> obelisks);
/// The same framed as `sub`'s messages (kSeen, kSeenReport).
std::vector<std::vector<std::uint8_t>> seenMessages(std::uint8_t sub, std::string_view map, std::span<const std::uint16_t> walls,
                                                    std::span<const std::uint16_t> obelisks);
/// None when it's malformed or bigger than a part: no name, more cells or bytes than a part has,
/// a cell out of order or past the grid, an obelisk past 0x7FFF, bytes left over.
std::optional<SeenCells> parseSeen(std::span<const std::uint8_t> rest);

std::vector<std::uint8_t> helloMessage(const Hello& h);
std::vector<std::uint8_t> noticeMessage(Notice kind, std::string_view text);
std::vector<std::uint8_t> rosterMessage(const Roster& r);
std::vector<std::uint8_t> cineMessage(const Cine& c);
std::vector<std::uint8_t> chapterMessage(const Chapter& c);
std::vector<std::uint8_t> readyStateMessage(std::uint8_t reading, std::uint8_t of);
std::vector<std::uint8_t> dialogMessage(const DialogMirror& d);
std::vector<std::uint8_t> creaturesMessage(std::span<const std::uint16_t> netCodes);
std::optional<std::vector<std::uint16_t>> parseCreatures(std::span<const std::uint8_t> rest);
std::vector<std::uint8_t> chapterReadyMessage();
std::vector<std::uint8_t> hostCmdMessage(HostCmd cmd);
/// The file's parts (none when it's empty or too big: kCharacterMost).
std::vector<std::vector<std::uint8_t>> characterMessages(std::span<const std::uint8_t> plr);
std::optional<CharacterPart> parseCharacter(std::span<const std::uint8_t> rest);
std::optional<Cine> parseCine(std::span<const std::uint8_t> rest);
std::optional<Chapter> parseChapter(std::span<const std::uint8_t> rest);
std::optional<std::pair<std::uint8_t, std::uint8_t>> parseReadyState(std::span<const std::uint8_t> rest);
std::optional<DialogMirror> parseDialog(std::span<const std::uint8_t> rest);
std::optional<HostCmd> parseHostCmd(std::span<const std::uint8_t> rest);

/// The sub and the rest, or none when it's cut short (or not a co-op message).
std::optional<std::pair<std::uint8_t, std::span<const std::uint8_t>>> parse(std::span<const std::uint8_t> m);
std::optional<Hello> parseHello(std::span<const std::uint8_t> rest);
std::optional<std::pair<Notice, std::string>> parseNotice(std::span<const std::uint8_t> rest);
std::optional<Roster> parseRoster(std::span<const std::uint8_t> rest);

} // namespace nox::coop::coopmsg
