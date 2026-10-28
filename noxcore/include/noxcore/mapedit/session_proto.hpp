// MSG_NOXCPP_EDIT (0xF6), noxcpp's own: the collaborative map editor's session messages, only
// between a noxcpp server hosting an edit session and the clients that identified themselves
// (lobby/extended.hpp). A stock client never gets one.
//
// Framing as MSG_NOXCPP_MAPVOTE / MODE_F4 / MODE_F5: {u8 0xF6, u8 sub, u16 the rest's length, the rest}.
// A reliable packet holds at most 1024 bytes, so a body over kMaxBody goes as kChunk messages
// ({u8 the sub it carries, u32 transfer, u16 index, u16 count, at most kChunkBytes}) and is put
// back together before it is read (Reassembler). Big byte fields (ops, the session document) are
// "packed": {u8 1 deflated (nxz) / 0 raw, u32 raw size, bytes}.
//
// The flow (S host, C a noxsdl client):
//   S->C Hello      as a noxsdl client comes in: the session (its map, version, rules, our peer id)
//   C->S Join       the client wants the working copy (a viewer from then on)
//   S->C Base       the published version's document (chunked), then Committed (catchUp) for each op
//                   since, the LockTable and Synced{log index}: the client is in sync there
//   S->C Roster     who is in the session (role, colour, stock or noxsdl), on every change
//   C->S RequestEdit / S->C EditRequest (to the owner) / C->S Answer / S->C Role / C->S Revoke
//   C->S Op         {client seq, op}; S->C Committed {log index, author, seq, op} to every viewer
//                   (the author's own is its ack) or S->C Rejected {seq, reason} to the author
//   C->S Lock       the element set wanted (a selection, an open panel); S->C LockAnswer {granted};
//                   S->C LockTable to every viewer on a change; C->S ForceRelease (owner)
//   C<->S Presence  cursor, tool, brush (unreliable, latest wins)
//   C->S Publish (owner) / S->C Published {version, CRC, name, full}
//   C->S ScriptReload / S->C ScriptDiag {compile diagnostics, the reload's result}
//   S->C Live       the running game's players and creatures (the editor's live view)
//   C<->S PlayTest  a play-test round started / ended; C->S Settings (owner)
//   S->C Log        a line for the session log ("<name> can now edit")
//   S->C Snapshot   a late join past a long log: Base, then one op to the working copy
//   C<->S Park      my character parked as an observer (Edit) or back in play
//   C->S JumpIn     my character into the game at a player's spot
//   S->C Problem    a session problem for the editors' Problems panel
//   S->C Editors    who's editing: each editor's character (net code) and state (editing, idle,
//                   playing), on every change; noxsdl marks their characters with a pencil
//
// A session's mod project (the host's mods\<id>), only
// to members whose Join named kFeatureModFiles (a client before it sends Join with no body, and
// never gets one of these):
//   S->C ModProject the mod's id and name and its files: each one's size, published and working
//                   versions, who holds it and who wrote the working version; on join and on every change
//   S->C ModFile    a file's working content (its version; none: deleted in the working copy)
//   C->S ModFile    an editor's save of a file (the working version it was made from), or its delete
//   C->S ModLock    a file held while it's typed in (or let go; the owner may take it over)
//   C->S ModPublish (owner) the working versions written into the host's mods\<id>
#pragma once

#include "noxcore/mapedit/locks.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace nox::mapedit::session {

inline constexpr std::uint8_t kOp = 0xF6;
/// The largest body sent whole; bigger ones are chunked.
inline constexpr std::size_t kMaxBody = 900;
inline constexpr std::size_t kChunkBytes = 880;
/// Packed bytes over this are deflated.
inline constexpr std::size_t kPackAbove = 256;

enum Sub : std::uint8_t {
    kHello = 1,
    kJoin = 2,
    kRoster = 3,
    kRequestEdit = 4,
    kEditRequest = 5,
    kAnswer = 6,
    kRole = 7,
    kRevoke = 8,
    kOpMsg = 9,
    kCommitted = 10,
    kRejected = 11,
    kBase = 12,
    kSynced = 13,
    kLock = 14,
    kLockAnswer = 15,
    kLockTable = 16,
    kForceRelease = 17,
    kPresence = 18,
    kPublish = 19,
    kPublished = 20,
    kScriptDiag = 21,
    kScriptReload = 22,
    kLive = 23,
    kPlayTest = 24,
    kSettings = 25,
    kLog = 26,
    kChunk = 27,
    kSnapshot = 28, ///< S->C: a late joiner's working copy in one op (a long log's catch-up)
    kPark = 29,     ///< C->S: park / unpark my character; S->C: its state now
    kJumpIn = 30,   ///< C->S: my character into the game at a player's spot
    kProblem = 31,  ///< S->C (editors): a session problem opened or closed
    // Who's editing (the pencil).
    kEditors = 32,  ///< S->C (viewers): every editor's character and whether they edit, idle or play
    // A mod project (only to members whose Join named kFeatureModFiles).
    kModProject = 33, ///< S->C: the mod's files, their versions and holders
    kModFile = 34,    ///< S->C: a file's working content; C->S: an editor's save or delete
    kModLock = 35,    ///< C->S: hold / let go a file
    kModPublish = 36, ///< C->S (owner): the working files written into the host's mods\<id>
};

/// What a client's Join body names it takes (u32 bits; a client without them sends no body).
enum Feature : std::uint32_t {
    kFeatureModFiles = 1, ///< the mod project's messages (kModProject, kModFile)
};

/// A session's mod project's caps, checked by the host before it keeps anything and by each
/// client on receipt: a file's bytes, the files, all their bytes.
inline constexpr std::size_t kMaxModFile = 4u << 20;
inline constexpr std::size_t kMaxModFiles = 320;
inline constexpr std::size_t kMaxModBytes = 24u << 20;

enum class Role : std::uint8_t {
    Player = 0, ///< in the game only (a stock client, or a noxsdl one that didn't join the session)
    Viewer = 1, ///< receives the ops (a noxsdl client that joined)
    Editor = 2, ///< may send ops
    Owner = 3,  ///< the host: grants and revokes, publishes
};
const char* roleName(Role r);

/// Who may edit: the host alone, or whoever the host approves.
enum class EditMode : std::uint8_t { HostOnly = 0, Approve = 1 };

enum class Reject : std::uint8_t {
    Lock = 1,       ///< touches what another editor holds
    Permission = 2, ///< not an editor (never was, or revoked)
    Invalid = 3,    ///< doesn't apply to the working copy
    PlayTest = 4,   ///< a play-test round is running
};

/// Session flags (Hello, Settings).
enum Flag : std::uint8_t {
    kPlayersCantDie = 1,
    kLiveView = 2,
    kPlayTestRunning = 4,
    kAutoPublish = 8,
    kLiveCreatures = 16,
};

struct Hello {
    std::uint32_t session = 0;
    std::uint32_t version = 0;     ///< the published version (1 at the start; each full publish +1)
    EditMode editMode = EditMode::Approve;
    std::uint8_t flags = 0;
    std::uint32_t you = 0;         ///< your peer id in the session
    std::string map;               ///< the map's name (what is published)
};
struct RosterEntry {
    std::uint32_t peer = 0;
    Role role = Role::Player;
    std::uint8_t colour = 0;       ///< presence colour index (0-15)
    bool stock = false;            ///< a stock GAME.EXE client (else noxsdl)
    std::string name;
    bool operator==(const RosterEntry&) const = default;
};
struct EditRequest {
    std::uint32_t peer = 0;
    std::string name;
    std::string reason;
    bool open = true;              ///< false: answered, withdrawn or expired (the toast goes)
};
/// Decision of the owner: 0 deny, 1 approve for this session, 2 approve always for this player.
struct Answer {
    std::uint32_t peer = 0;
    std::uint8_t decision = 0;
};
struct RoleMsg {
    Role role = Role::Player;
    std::string text;
};
struct OpMsg {
    std::uint32_t seq = 0;
    std::vector<std::uint8_t> op;  ///< serializeOp's bytes
    /// The log index the sender had reached when it made the op: the host moves the ids it
    /// freshened in the sender's earlier ops in ops made before their echo.
    std::uint32_t seen = 0;
};
struct Committed {
    std::uint32_t index = 0;       ///< the host log's (1-based)
    std::uint32_t author = 0;      ///< peer id (0: the host itself, e.g. a compiled script)
    std::uint32_t seq = 0;         ///< the author's client seq (0: none)
    bool catchUp = false;          ///< part of a late join's catch-up
    std::vector<std::uint8_t> op;
};
struct Rejected {
    std::uint32_t seq = 0;
    Reject reason = Reject::Invalid;
    std::string text;
};
struct Base {
    std::uint32_t version = 0;
    std::vector<std::uint8_t> document; ///< encodeSessionDocument's bytes
};
struct LockMsg {
    std::uint32_t seq = 0;
    std::set<ElementKey> keys;
};
struct Presence {
    std::uint32_t peer = 0;        ///< (S->C: whose)
    float x = 0, y = 0;            ///< the cursor in world units
    std::uint8_t tool = 0;
    float radius = 0;              ///< a brush's footprint (0: none)
    bool inView = true;            ///< the cursor is over the map
};
struct Published {
    std::uint32_t version = 0;
    std::uint32_t crc = 0;
    std::string name;
    bool full = true;              ///< a map change (false: objects only, no reload)
};
struct Diagnostic {
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    bool error = false;
    std::string text;
};
struct ScriptDiag {
    std::uint8_t kind = 0;         ///< 0 a compile, 1 a reload
    bool ok = false;
    std::vector<Diagnostic> diagnostics;
    std::string summary;
};
/// One player or creature of the running game. Besides where it is, what the editor's live layer
/// draws it with: its type and its animation as the game's update stream carries it for the same
/// object (MSG_UPDATE_STREAM's animation bytes: `anim`'s bits 4-6 the facing, its low nibble a
/// creature's state; a frame byte; a player's state byte), and a player's worn armour and weapon
/// class bits (what the game's equip reports say). The player's colours and the worn things'
/// modifiers are the game's own (every client's player list has them).
struct LiveEntry {
    std::uint16_t id = 0;          ///< the object's net code
    bool player = true;
    bool stock = false;            ///< (players) on a stock client
    std::uint8_t team = 0;
    std::int16_t x = 0, y = 0;
    std::string name;
    bool observer = false;         ///< (players) observing: the game draws no body for them
    std::uint16_t type = 0;       ///< thing type (THNG index + 1)
    std::uint8_t anim = 0;         ///< the update's animation byte (see above)
    std::uint8_t frame = 0xFF;     ///< the update's frame byte (0xFF: none)
    std::uint8_t state = 0;        ///< (players) the update's state byte
    std::uint32_t armor = 0;       ///< (players) worn armour class bits
    std::uint32_t weapon = 0;      ///< (players) worn weapon class bits
    bool operator==(const LiveEntry&) const = default;
};
struct PlayTest {
    bool start = true;
    std::uint32_t mode = 0x100;    ///< game flags' mode bits
    bool playersCanDie = false;
};
struct Settings {
    EditMode editMode = EditMode::Approve;
    std::uint8_t flags = 0;
};
/// A late joiner's catch-up when the log is long: instead of every logged op, the one op that takes
/// the base (sent just before) to the working copy at log index `index`, and the document's next
/// ids (which no op sets). Deterministic and id-preserving like the log: the host checks that the
/// op gives its working copy exactly before it sends it.
struct Snapshot {
    std::uint32_t version = 0;
    std::uint32_t index = 0;
    std::uint32_t nextObjId = 0;
    std::uint32_t nextPolygonKey = 0;
    std::uint32_t nextGroupKey = 0;
    std::vector<std::uint8_t> op;  ///< serializeOp's bytes
};
/// Parking: C->S `on` parks my character as an observer (the Edit side), off brings it back (at x,
/// y when `spot`, else where it was parked); S->C my character's state now, with the host's reason
/// when it didn't do what was asked.
struct Park {
    bool on = true;
    bool spot = false;
    float x = 0, y = 0;
    std::string text;
};
/// A session problem (a script holding a recreated object...): `key` names it (the document
/// object's id), closed when `open` is false.
struct Problem {
    std::uint32_t key = 0;
    bool open = true;
    std::string text;
    bool operator==(const Problem&) const = default;
};

/// What an editor is doing (Editors): in the editor and working (an op, a selection, a cursor
/// move, a panel lock within the host's idle time), in the editor with nothing done for that long,
/// or with their character in play (the game in front: not parked, not observing).
enum class Activity : std::uint8_t { Editing = 0, Idle = 1, Playing = 2 };
const char* activityName(Activity a);
/// One editor (role Editor or Owner) of the session, for the pencil over their character: the
/// net code of their character in the game (0: none), and what they're doing.
struct EditorState {
    std::uint32_t peer = 0;
    std::uint16_t netCode = 0;
    Activity activity = Activity::Editing;
    bool operator==(const EditorState&) const = default;
};

/// One file of a session's mod project (ModProject).
struct ModFileInfo {
    std::string path;               ///< '/' separated, inside the mod's folder (mapedit::modPathValid)
    std::uint32_t size = 0;         ///< the working content's bytes (0: deleted, or empty)
    std::uint32_t published = 0;    ///< the version in the host's mods\<id> (0: not there)
    std::uint32_t working = 0;      ///< the working version (0: deleted in the working copy)
    std::uint32_t holder = 0;       ///< the member holding it (0: nobody)
    std::uint32_t author = 0;       ///< who wrote the working version (0: the host's folder)
    bool operator==(const ModFileInfo&) const = default;
};
struct ModProject {
    std::string id;                 ///< the mod's id (its folder's name)
    std::string name;
    std::vector<ModFileInfo> files;
};
/// S->C: a file's working content at `version` (`deleted`: none). C->S: a save made from the
/// working version `version` (0: a new file), or a delete.
struct ModFile {
    std::string path;
    std::uint32_t version = 0;
    bool deleted = false;
    std::vector<std::uint8_t> bytes;
};
struct ModLock {
    std::string path;
    bool on = true;
    bool force = false;             ///< (owner) taken from whoever holds it
};

/// One message's sub and body.
struct Message {
    std::uint8_t sub = 0;
    std::vector<std::uint8_t> body;
};

// --- Framing ----------------------------------------------------------------------------------

/// The whole messages for (sub, body): one when it fits, else kChunk messages (`transfer` is
/// the sender's counter, bumped).
std::vector<std::vector<std::uint8_t>> frame(std::uint8_t sub, std::span<const std::uint8_t> body, std::uint32_t& transfer);
/// A message's sub and body ({0xF6, sub, u16 len, body}); nothing when it isn't one.
std::optional<Message> unframe(std::span<const std::uint8_t> message);

/// Puts chunked messages back together (per transfer); whole messages pass straight through.
class Reassembler {
public:
    /// The message ready (whole, or a transfer's last chunk), or nothing yet / malformed.
    std::optional<Message> feed(std::span<const std::uint8_t> message);
    std::size_t pending() const { return parts_.size(); }
    /// Transfers held unfinished at most (a sender's chunks come in order, one
    /// transfer after another, so one is the most a good sender leaves): past it the oldest is let
    /// go, so a client that never finishes one can't grow the host's memory without end.
    static constexpr std::size_t kMaxPending = 4;

private:
    struct Parts {
        std::uint8_t sub = 0;
        std::uint16_t count = 0;
        std::map<std::uint16_t, std::vector<std::uint8_t>> chunks;
    };
    std::map<std::uint32_t, Parts> parts_;
};

/// {u8 deflated, u32 raw size, bytes}.
std::vector<std::uint8_t> pack(std::span<const std::uint8_t> raw);
std::vector<std::uint8_t> unpack(std::span<const std::uint8_t> packed);

// --- Bodies (encode / decode; decode throws FormatError) ----------------------------------------

std::vector<std::uint8_t> encode(const Hello& m);
std::vector<std::uint8_t> encode(const std::vector<RosterEntry>& m);
std::vector<std::uint8_t> encode(const EditRequest& m);
std::vector<std::uint8_t> encode(const Answer& m);
std::vector<std::uint8_t> encode(const RoleMsg& m);
std::vector<std::uint8_t> encode(const OpMsg& m);
std::vector<std::uint8_t> encode(const Committed& m);
std::vector<std::uint8_t> encode(const Rejected& m);
std::vector<std::uint8_t> encode(const Base& m);
std::vector<std::uint8_t> encode(const LockMsg& m);
std::vector<std::uint8_t> encode(const Presence& m);
std::vector<std::uint8_t> encode(const Published& m);
std::vector<std::uint8_t> encode(const ScriptDiag& m);
std::vector<std::uint8_t> encode(const std::vector<LiveEntry>& m);
std::vector<std::uint8_t> encode(const PlayTest& m);
std::vector<std::uint8_t> encode(const Settings& m);
std::vector<std::uint8_t> encode(const Snapshot& m);
std::vector<std::uint8_t> encode(const Park& m);
std::vector<std::uint8_t> encode(const Problem& m);
std::vector<std::uint8_t> encode(const std::vector<EditorState>& m);
std::vector<std::uint8_t> encode(const ModProject& m);
std::vector<std::uint8_t> encode(const ModFile& m);
std::vector<std::uint8_t> encode(const ModLock& m);
/// A string body (RequestEdit's reason, Log's line).
std::vector<std::uint8_t> encodeText(const std::string& text);
/// A u32 body (Synced's index, Revoke's peer, ScriptReload's mode, Publish's kind).
std::vector<std::uint8_t> encodeU32(std::uint32_t v);
std::vector<std::uint8_t> encodeKey(const ElementKey& key);

Hello decodeHello(std::span<const std::uint8_t> b);
std::vector<RosterEntry> decodeRoster(std::span<const std::uint8_t> b);
EditRequest decodeEditRequest(std::span<const std::uint8_t> b);
Answer decodeAnswer(std::span<const std::uint8_t> b);
RoleMsg decodeRole(std::span<const std::uint8_t> b);
OpMsg decodeOp(std::span<const std::uint8_t> b);
Committed decodeCommitted(std::span<const std::uint8_t> b);
Rejected decodeRejected(std::span<const std::uint8_t> b);
Base decodeBase(std::span<const std::uint8_t> b);
LockMsg decodeLock(std::span<const std::uint8_t> b);
Presence decodePresence(std::span<const std::uint8_t> b);
Published decodePublished(std::span<const std::uint8_t> b);
ScriptDiag decodeScriptDiag(std::span<const std::uint8_t> b);
std::vector<LiveEntry> decodeLive(std::span<const std::uint8_t> b);
PlayTest decodePlayTest(std::span<const std::uint8_t> b);
Settings decodeSettings(std::span<const std::uint8_t> b);
Snapshot decodeSnapshot(std::span<const std::uint8_t> b);
Park decodePark(std::span<const std::uint8_t> b);
Problem decodeProblem(std::span<const std::uint8_t> b);
std::vector<EditorState> decodeEditors(std::span<const std::uint8_t> b);
/// (Untrusted input, both ways: every path checked (modPathValid), at most kMaxModFiles files of at
/// most kMaxModFile bytes, kMaxModBytes in all; a content's size checked before it's unpacked.)
ModProject decodeModProject(std::span<const std::uint8_t> b);
ModFile decodeModFile(std::span<const std::uint8_t> b);
ModLock decodeModLock(std::span<const std::uint8_t> b);
/// Join's body: the features a client takes (an empty body: none).
std::vector<std::uint8_t> encodeJoin(std::uint32_t features);
std::uint32_t decodeJoin(std::span<const std::uint8_t> b);
std::string decodeText(std::span<const std::uint8_t> b);
std::uint32_t decodeU32(std::span<const std::uint8_t> b);
ElementKey decodeKey(std::span<const std::uint8_t> b);

} // namespace nox::mapedit::session
