// MSG_NOXCPP_MODE_F4 (0xF4), noxcpp's own: a Delving Mode run's map for noxsdl players, drawn
// behind a map button by the health bar as a branching map of rooms (stock players are told it in text
// lines instead). Only between a noxcpp server and the clients that identified themselves
// (lobby/extended.hpp): {u8 0xF4, u8 sub, u16 the rest's length, the rest}.
//
// The server sends kRun at a player's join and with each level made (where the party is now),
// kEnd when the run is over (the lobby again). At a campfire, kCampfire offers the player its
// spells to strengthen one (an empty list puts the offer away); the client's pick comes back
// as kUpgrade (the one message the client sends; a stock player answers by chat instead).
//
// Each message is kept within one reliable packet (net::kMaxReliablePayload, 1024 bytes): a
// longer one would never be sent. A long run's kRun leaves out its oldest acts done; a big
// records table goes as kRecords and then kRecordsMore.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace nox::server::delvemap {

inline constexpr std::uint8_t kOp = 0xF4;
inline constexpr std::uint8_t kEnd = 1; ///< no run any more: the map put away
inline constexpr std::uint8_t kMulticlass = 2; ///< u8 1: we play multiclass (PlayerControl::multiclass)
/// The run as it stands (Run). (0 was the first map's, its acts six rooms in a line: retired, so
/// a client from before the rows ignores this one.) When its oldest acts done were left out (to
/// fit), a u16 after the fork counts them (Run::earlier); a client from before that ignores it.
inline constexpr std::uint8_t kRun = 3;
/// S->C: the campfire's offer: u8 count, then each spell's u8 number and u8 level now (count 0:
/// the offer put away: used, or the player walked off).
inline constexpr std::uint8_t kCampfire = 4;
/// C->S: the spell picked at the campfire: u8 its number.
inline constexpr std::uint8_t kUpgrade = 5;
/// S->C: the server's deepest runs (DelveRecords' top), at a join and as they change, in the
/// lobby as in a run: u8 count, then each {u16 depth, u8 act, u8 date length, its date, u8
/// difficulty length, its name, u8 party size, each {u8 name length, the name, u8 class}}.
inline constexpr std::uint8_t kRecords = 6;
/// S->C: the records that didn't fit in kRecords, as kRecords lays them out, added after them
/// (a client from before it shows the first part only).
inline constexpr std::uint8_t kRecordsMore = 10;
/// S->C: a blessing room's altar's offer: u8 count, then each blessing's u8 number (Blessing)
/// and u8 how many of it the player holds now (count 0: the offer put away).
inline constexpr std::uint8_t kBlessing = 7;
/// C->S: the blessing picked at the altar: u8 its number.
inline constexpr std::uint8_t kBless = 8;

/// A blessing: a boon for the rest of a run (server::DelveDirector keeps it on the player).
/// Vigor, Wisdom and Might stack (a quarter more each time); the rest are once.
enum class Blessing : std::uint8_t { Vigor, Wisdom, Might, Swiftness, Thirst, Warding, Renewal, Fortune };
inline constexpr std::size_t kBlessings = 8;
/// "Vigor", and what it does ("A quarter more health").
const char* blessingName(Blessing b);
const char* blessingText(Blessing b);

struct BlessingOffer {
    Blessing blessing = Blessing::Vigor;
    std::uint8_t held = 0; ///< how many of it the player holds already
    bool operator==(const BlessingOffer&) const = default;
};
std::vector<std::uint8_t> blessingMessage(std::span<const BlessingOffer> offer);
std::optional<std::vector<BlessingOffer>> parseBlessing(std::span<const std::uint8_t> rest);
std::vector<std::uint8_t> blessMessage(Blessing b);

/// S->C: the level's bonus objective as it stands, at a join and as it changes: u8 kind
/// (server::DelveObjective's; 0: none, put away), u8 state (0 open, 1 done, 2 failed), u16
/// done, u16 of, u16 seconds left (0xFFFF: no clock), u8 text length, the text ("Break the
/// seals").
inline constexpr std::uint8_t kObjective = 9;
struct Objective {
    std::uint8_t kind = 0;
    std::uint8_t state = 0;
    std::uint16_t done = 0;
    std::uint16_t of = 0;
    std::uint16_t secondsLeft = 0xFFFF;
    std::string text;
    bool operator==(const Objective&) const = default;
};
std::vector<std::uint8_t> objectiveMessage(const Objective& o);
std::optional<Objective> parseObjective(std::span<const std::uint8_t> rest);

/// A record as the map shows it (server::DelveRecord's).
struct Record {
    std::uint16_t depth = 0;
    std::uint8_t act = 1;
    std::string date;
    std::string difficulty;
    std::vector<std::pair<std::string, std::uint8_t>> party;
    bool operator==(const Record&) const = default;
};
/// The table as kRecords and, past what fits, kRecordsMore messages (one message when it fits;
/// a party too big for a message alone cut short).
std::vector<std::vector<std::uint8_t>> recordsMessages(std::span<const Record> records);
/// A kRecords' or kRecordsMore's rest.
std::optional<std::vector<Record>> parseRecords(std::span<const std::uint8_t> rest);

/// A spell the campfire may strengthen, and its level now.
struct Upgradable {
    std::uint8_t spell = 0;
    std::uint8_t level = 0;
    bool operator==(const Upgradable&) const = default;
};
std::vector<std::uint8_t> campfireMessage(std::span<const Upgradable> spells);
std::optional<std::vector<Upgradable>> parseCampfire(std::span<const std::uint8_t> rest);
std::vector<std::uint8_t> upgradeMessage(std::uint8_t spell);

/// An act on the map: its biome (mapgen/biome.hpp's name) and its six rows of rooms
/// (server::DelveAct's: row r r + 1 rooms, the guardian's one; server::DelveRoom's numbers:
/// fight 0, elite 1, mystery 2, shop 3, campfire 4, guardian 5, blessing 6).
struct Act {
    std::string biome;
    std::array<std::vector<std::uint8_t>, 6> rows;
    /// The way the party took through it: the room taken in each row so far (its index).
    std::vector<std::uint8_t> path;
    /// A finished act's: the gate the party took out of it (0 left, 1 right; 0xFF none).
    std::uint8_t taken = 0xFF;
    bool operator==(const Act&) const = default;
};

struct Run {
    std::uint16_t depth = 0;
    std::uint8_t room = 0;       ///< the row the party is at in `act` (0-5); its room act.path's last
    std::uint8_t mystery = 0xFF; ///< this room's mystery once entered (DelveMystery's), else 0xFF
    std::vector<Act> past;       ///< the acts done, first first (the act's number: earlier + their count + 1)
    Act act;
    std::optional<std::array<Act, 2>> fork; ///< the two next acts its guardian opens onto
    std::uint16_t earlier = 0;   ///< the acts done before past's first, left out
    bool operator==(const Run&) const = default;
};

/// The run as kRun; its oldest acts done left out (counted in earlier) while it wouldn't fit.
std::vector<std::uint8_t> runMessage(const Run& run);
std::vector<std::uint8_t> endMessage();
std::vector<std::uint8_t> multiclassMessage(bool on);
/// The message's sub and the rest, or nullopt when it isn't one (or is cut short).
std::optional<std::pair<std::uint8_t, std::span<const std::uint8_t>>> parse(std::span<const std::uint8_t> m);
/// A kRun's rest, or nullopt when it's malformed.
std::optional<Run> parseRun(std::span<const std::uint8_t> rest);

} // namespace nox::server::delvemap
