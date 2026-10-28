// The collaborative map editor's session on the host: NoX Core's own, hosted through Host Custom >
// Map Editor; it never exists in any other game.
//
// The session IS a running game of the last published map: stock GAME.EXE clients and noxsdl
// clients join and play in it the whole time; editing never pauses it. The director holds the
// authoritative documents and the op log, and runs between the game's frames (tick, after
// GameHost::tick):
//
//   base      the published version's document (as every member holds it: session_doc.hpp)
//   working   base + the op log: what the editors see
//   the log   every committed op since the last full publish, in the host's order (log index)
//
// noxsdl clients that identified themselves get MSG_NOXCPP_EDIT (session_proto.hpp): a Hello as
// they come in; a Join makes them viewers (the base, the log, the locks, Synced); editors send ops,
// which the host checks (role, play-test, the lock table, then that the op applies) and commits or
// rejects.
//
// What goes live in the running game, while editing goes on:
//   - live objects (creatures, items, players' things, movable things): added through the map
//     load's own reading and placing (World::placeLiveRecords), removed by the game's delayed
//     delete (what they hold deleted with them, not dropped), moved by the game's position set
//     (as a script's MoveObject), changed by a delete and an add at the same place. What can't be
//     changed now (carried by someone, a creature mid-death) waits; what play already used up
//     (killed, destroyed) stays gone until the next publish;
//   - the things every client draws from its own map (mapedit::clientLoadedType: CLASS IMMOBILE
//     or CLIENT_PERSIST; scenery, furniture, fireplaces, trees, doors, the client-only types) and
//     whatever they hold: held for a full publish (mapedit::heldForPublish), the World
//     keeping the published state (a moved fireplace blocks, lights and burns where it was; a
//     deleted one stays; an added one isn't there), as noxsdl draws them pending till then;
//   - walls, floors, edges, waypoints, polygons, groups, map info and the script's bytecode: only
//     in the working copy (noxsdl draws them as pending) until a publish;
//   - the script: SetScriptSource compiled on the host (diagnostics to the editors); an editor's
//     ScriptReload commits the compiled bytecode and reloads the running script (hot: globals kept;
//     or restart): every player's game runs the new script at once (it runs only on the server).
// A full publish writes the working copy under the session's (never a stock) name and changes the
// map to it as the console's "load" does: stock clients download it; noxsdl clients write the same
// file themselves (its CRC matches) and don't. Object-only changes are written automatically for
// late joiners (no map change: the world already has them; held things written as published, as
// the world has them).
//
// The sandbox: players can't die unless the host lets them (World::sandboxPlayersCantDie, every
// player, stock or not); nothing scores (World::sandboxNoScoring); no limits, the same map again at
// a match's end. Play-test rounds run the map's real mode.
//
// Merging: a late joiner past a long log gets a compact snapshot (one op from the base, checked);
// the ids the host moves follow into the sender's ops made before their echo; polygon and group
// sets merge per entry; an object made again keeps its script id (else a session Problem); a
// member's character parks as an observer on the editor's Edit side and comes back at its camera,
// or jumps in at a player (Park, JumpIn).
#pragma once

#include "noxcore/mapedit/document.hpp"
#include "noxcore/mapedit/locks.hpp"
#include "noxcore/mapedit/ops.hpp"
#include "noxcore/mapedit/session_merge.hpp"
#include "noxcore/mapedit/session_proto.hpp"
#include "noxcore/script/compiler.hpp"
#include "noxcore/server/game_host.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace nox::server {

class EditSessionDirector {
public:
    struct Options {
        std::filesystem::path mapsDir;      ///< where the session's map lives and is published
        std::string mapName;                ///< the session's map (never a stock map's name)
        mapedit::session::EditMode editMode = mapedit::session::EditMode::Approve;
        bool playersCantDie = true;         ///< the sandbox's default
        bool liveView = true;               ///< the editors' live view of the game
        bool liveCreatures = true;          ///< creatures in it too, not only players
        bool autoPublish = true;            ///< object-only changes written for late joiners
        std::uint32_t playTestMode = 0x100; ///< a play-test round's mode (game flags' mode bits)
        /// The owner: the host's own client (GameHost::isHostPeer), or a player of this name (a
        /// dedicated host's, a test's).
        std::string ownerName;
        /// Names approved "always for this player": editors as they join.
        std::vector<std::string> alwaysEditors;
        /// The list above changed (an "approve always", a revoke): the host keeps it in its own
        /// settings for its next sessions.
        std::function<void(const std::vector<std::string>&)> alwaysEditorsChanged;
        /// A late joiner gets a compact snapshot (Base, then one op to the working copy) instead
        /// of the log once the log holds more than this many bytes or ops.
        std::size_t snapshotLogBytes = 64 * 1024;
        std::size_t snapshotLogOps = 2000;
        std::uint32_t sessionId = 1;
        /// Session log lines ("<name> can now edit"), for the host's log (none: dropped).
        std::function<void(const std::string&)> log;
        /// Timings (ms): idle locks released, requests unanswered, denied ones' wait, the live
        /// view's interval, the auto-publish debounce and its longest wait.
        std::uint32_t lockTimeoutMs = 60'000;
        std::uint32_t requestTimeoutMs = 120'000;
        std::uint32_t deniedWaitMs = 60'000;
        /// (The live view: 10 a second for the players' animations; while creatures go
        /// too, at most 4.)
        std::uint32_t liveIntervalMs = 100;
        std::uint32_t liveCreaturesIntervalMs = 250;
        /// Live is "latest wins": a viewer with more reliable packets than this
        /// still unacknowledged (a slow or stalled link) is skipped that time, so the live view
        /// never fills its queue (net::kMaxQueuedReliable, shared with the game's own reliable
        /// messages and the session's ops: past it they'd be lost).
        std::size_t liveQueueLimit = 32;
        std::uint32_t autoPublishQuietMs = 2'000;
        std::uint32_t autoPublishMaxMs = 10'000;
        /// An editor in the editor with nothing sent for this long (no op, selection, cursor move,
        /// lock, publish, park) is shown idle (Editors: the pencil greyed).
        std::uint32_t idleMs = 30'000;
        /// A mod project the session carries: the host's mods\<id>, read whole at the start
        /// (mapedit/mod_files.hpp's caps; over them, or a bad folder: none, logged). Its files go
        /// to the members that take them (session::kFeatureModFiles), editors' saves are kept as
        /// working versions, and the owner's ModPublish writes them back here (the only place
        /// they're ever written on the host). Empty: none.
        std::filesystem::path modFolder;
        /// A member's file transfers go out while it has at most this many reliable packets
        /// unacknowledged (a mod's files are big: the game's own reliable messages never wait on them).
        std::size_t modQueueLimit = 24;
    };

    /// Reads the session's map (mapsDir\<mapName>\<mapName>.map) as the base. Throws when it
    /// can't be read, or is a stock map's name.
    EditSessionDirector(const things::ThingDb& things, Options options);

    /// The game list's server name.
    static std::string listedName(const std::string& host) { return "[EDIT] " + host; }
    /// The sandbox's mode for a map's MapInfo types: arena where it allows it, else its first
    /// network mode.
    static std::uint32_t sandboxModeFor(std::uint32_t mapTypes);
    /// The sandbox's mode for the session's map.
    std::uint32_t sandboxMode() const { return sandboxMode_; }
    /// GameHost::Options::prepareMap: a map is about to load (each document object's live object
    /// is found again after it).
    void mapLoading() { needRebind_ = true; }

    /// GameHost::Options::onEditMessage: queued, handled at the next tick (between frames).
    void onMessage(GameHost& host, std::uint8_t peer, std::span<const std::uint8_t> m);
    /// Between frames (after GameHost::tick): the queued messages, the roster, the live effects,
    /// the timeouts, the live view, the automatic publish. True when the published version changed.
    bool tick(GameHost& host, std::uint32_t nowMs);

    // ---- the host's own controls (a dedicated host's console, tests; noxsdl's owner goes by the
    // protocol like any editor)
    /// A full publish (true) or an object-only one; false (the reason logged) when it failed.
    bool publish(GameHost& host, bool full);
    void setSettings(GameHost& host, mapedit::session::EditMode mode, std::uint8_t flags);
    bool startPlayTest(GameHost& host, std::uint32_t mode, bool playersCanDie);
    void endPlayTest(GameHost& host);

    // ---- state (tests, the host's panels)
    const mapedit::MapDocument& working() const { return working_; }
    const mapedit::MapDocument& base() const { return base_; }
    std::uint32_t logIndex() const { return logBase_ + static_cast<std::uint32_t>(log_.size()); }
    std::uint32_t version() const { return version_; }
    const mapedit::LockTable& locks() const { return locks_; }
    bool playTestRunning() const { return playTest_; }
    std::uint8_t flags() const;
    mapedit::session::EditMode editMode() const { return options_.editMode; }
    const Options& options() const { return options_; }
    /// The running game's object a document object is (a top-level one), if it's live there.
    std::optional<std::uint32_t> liveExtent(mapedit::ObjId id) const;
    /// Document objects whose change waits till it can be made (carried, mid-death).
    const std::set<mapedit::ObjId>& waiting() const { return waiting_; }
    /// Document objects whose edits are held for a full publish (mapedit::heldForPublish: the
    /// things clients draw from their own map, and what they hold), still differing from the
    /// published copy. Empty after a full publish, and when undo puts one back as published.
    const std::set<mapedit::ObjId>& held() const { return held_; }
    mapedit::session::Role roleOf(std::uint8_t peer) const;
    std::vector<mapedit::session::RosterEntry> roster() const;
    /// Counters (tests, the log): ops committed and rejected, publishes.
    struct Stats {
        std::uint32_t committed = 0;
        std::uint32_t rejected = 0;
        std::uint32_t fullPublishes = 0;
        std::uint32_t objectPublishes = 0;
        std::uint32_t liveAdds = 0;
        std::uint32_t liveRemoves = 0;
        std::uint32_t liveMoves = 0;
        std::uint32_t liveRecreates = 0;
        std::uint32_t heldEdits = 0;     ///< changes to held things kept from the world till a publish
        std::uint32_t snapshots = 0;    ///< late joins caught up by a compact snapshot
        std::uint32_t scriptRepoints = 0; ///< objects made again that kept their script ids
        std::uint32_t merges = 0;        ///< ops merged per entry or with ids moved
        std::uint32_t parks = 0;         ///< characters parked
        std::uint32_t jumps = 0;         ///< characters jumped in at a player
        std::uint32_t liveSkipped = 0;   ///< Live sends passed over for a viewer's full queue
        std::uint32_t modPacks = 0;      ///< mod file bodies made (once a working version)
    };
    const Stats& stats() const { return stats_; }
    /// The log's ops as committed (index logBase+1 on), for replay checks.
    struct Entry {
        std::uint32_t author = 0;
        std::uint32_t seq = 0;
        std::vector<std::uint8_t> op; ///< serializeOp's bytes
    };
    const std::vector<Entry>& log() const { return log_; }
    /// The session's open problems (a script's object made again without its script id...).
    const std::map<std::uint32_t, mapedit::session::Problem>& problems() const { return problems_; }
    /// Whether a peer's character is parked by the session.
    bool parked(std::uint8_t peer) const;
    /// Who's editing (the Editors message, the pencil): every member with edit rights, the net
    /// code of their character, and whether they're editing, idle or playing now.
    std::vector<mapedit::session::EditorState> editorStates(GameHost& host) const;

    // ---- the mod project (Options::modFolder)
    /// One of its files: as published (in the host's folder) and as worked on (none: deleted).
    struct ModEntry {
        std::optional<std::vector<std::uint8_t>> published;
        std::uint32_t publishedVersion = 0;
        std::optional<std::vector<std::uint8_t>> working;
        std::uint32_t workingVersion = 0;
        std::uint8_t holder = 0;     ///< the member holding it (0: nobody)
        std::uint32_t heldAt = 0;    ///< when the holder last saved or took it (the idle release)
        std::uint32_t author = 0;    ///< who wrote the working version (0: the host's folder, or one who left)
        std::vector<std::uint8_t> body; ///< the working version's ModFile body (packed once a version)
    };
    /// Whether the session carries a mod, its id (the folder's name), its files, and why it doesn't
    /// when the folder couldn't be taken.
    bool hasMod() const { return !modId_.empty(); }
    const std::string& modId() const { return modId_; }
    const std::map<std::string, ModEntry>& modFiles() const { return mod_; }
    const std::string& modError() const { return modError_; }
    /// The owner's publish of the mod: every working version written into Options::modFolder
    /// (a deleted one removed); false (logged) when one couldn't be.
    bool publishMod(GameHost& host);
    /// (Tests) A member's mod files waiting to go out: the paths wanted, and the transfer under way.
    std::size_t modWanted(std::uint8_t peer) const;

private:
    /// A member's mod transfers: the list owed (latest wins), the files wanted (each path once), and
    /// the frames of the one transfer under way.
    struct ModOut {
        bool sent = false;      ///< the project was sent to it (a Join again wants the list alone)
        bool listDirty = false;
        std::deque<std::string> want;
        std::set<std::string> wanted;
        std::vector<std::vector<std::uint8_t>> frames;
        std::size_t frameAt = 0;
    };
    struct Peer {
        mapedit::session::Role role = mapedit::session::Role::Player;
        bool greeted = false;   ///< sent Hello
        bool joined = false;    ///< a viewer: gets the ops
        bool stock = true;
        std::uint8_t colour = 0;
        std::string name;
        std::optional<std::string> request; ///< a pending edit request's reason
        std::uint32_t requestAt = 0;
        std::uint32_t deniedAt = 0;
        bool denied = false;
        bool always = false;
        /// Parked by the session (the Edit side): observing, with where it stood.
        bool parked = false;
        bool parkSpot = false;
        float parkX = 0, parkY = 0;
        /// When the member last sent anything (the idle state's clock).
        std::uint32_t activeAt = 0;
        /// Ids the host moved in this peer's ops (freshenNewIds), for its ops made before it saw
        /// the echo: {the log index of the op that moved them, the moves}.
        struct Moved {
            std::uint32_t index = 0;
            mapedit::IdMoves moves;
        };
        std::vector<Moved> moved;
        mapedit::session::Reassembler in;
        /// Its Join named kFeatureModFiles: it gets the mod project's messages.
        bool modFiles = false;
        /// What of the mod waits for room in its reliable queue (Options::modQueueLimit).
        ModOut mod;
    };

    void handle(GameHost& host, std::uint8_t peer, const mapedit::session::Message& m);
    void join(GameHost& host, std::uint8_t peer);
    void onOp(GameHost& host, std::uint8_t peer, const mapedit::session::OpMsg& m);
    /// Commits `op` (already checked) as `author`'s: applied, logged, sent, made live.
    void commit(GameHost& host, mapedit::Op op, std::uint32_t author, std::uint32_t seq);
    void reject(GameHost& host, std::uint8_t peer, std::uint32_t seq, mapedit::session::Reject reason, const std::string& text);
    void onLock(GameHost& host, std::uint8_t peer, const mapedit::session::LockMsg& m);
    void onRequestEdit(GameHost& host, std::uint8_t peer, const std::string& reason);
    void onAnswer(GameHost& host, std::uint8_t peer, const mapedit::session::Answer& a);
    void revoke(GameHost& host, std::uint8_t peer, const std::string& why);
    void grant(GameHost& host, std::uint8_t peer, bool always);
    void scriptCompiled(GameHost& host);
    void reloadScript(GameHost& host, std::uint8_t peer, std::uint32_t mode);
    void refreshRoster(GameHost& host);
    void sendLive(GameHost& host);
    void broadcastLocks(GameHost& host);
    void broadcastHello(GameHost& host);
    void hello(GameHost& host, std::uint8_t peer);
    void logLine(GameHost& host, const std::string& line);
    bool isOwner(GameHost& host, std::uint8_t peer) const;
    /// The catch-up a late joiner gets after Base: the log, or (long) a compact snapshot.
    void catchUp(GameHost& host, std::uint8_t peer);
    /// Parking and jumping in at a player.
    void onPark(GameHost& host, std::uint8_t peer, const mapedit::session::Park& m);
    void onJumpIn(GameHost& host, std::uint8_t peer, std::uint32_t target);
    /// The peer's character back in play at x, y (from observing); false with `why` when it can't be.
    bool unpark(GameHost& host, std::uint8_t peer, float x, float y, std::string& why);
    void sendPark(GameHost& host, std::uint8_t peer, const std::string& text);
    /// A session problem opened (text) or closed (empty text), to the editors and the log.
    void problem(GameHost& host, std::uint32_t key, const std::string& text);
    void alwaysChanged();

    // The mod project.
    void loadMod();
    mapedit::session::ModProject modProject() const;
    /// A file's message body made for its working version (ModEntry::body).
    void packModEntry(const std::string& path, ModEntry& e);
    /// The list owed to every member that takes it (or one): sent by pumpMod, latest wins.
    void modListDirty(std::optional<std::uint8_t> only = std::nullopt);
    /// A file's working content wanted by every member that takes it (or one), each path once.
    void queueModFile(const std::string& path, std::optional<std::uint8_t> only = std::nullopt);
    /// A file's holder set (modHeld_ kept), and every file a member holds let go (a leave, a revoke).
    void setModHolder(ModEntry& e, std::uint8_t holder);
    void releaseModHolds(std::uint8_t peer);
    /// A line to the host's log and to the members that take the mod.
    void modLog(GameHost& host, const std::string& line);
    void onModFile(GameHost& host, std::uint8_t peer, const mapedit::session::ModFile& m);
    void onModLock(GameHost& host, std::uint8_t peer, const mapedit::session::ModLock& m);
    /// Each member's list and transfers out, as its reliable queue has room; idle mod holds let go.
    void pumpMod(GameHost& host);

    // Sending.
    void send(GameHost& host, std::uint8_t peer, std::uint8_t sub, std::span<const std::uint8_t> body);
    void sendJoined(GameHost& host, std::uint8_t sub, std::span<const std::uint8_t> body, std::optional<std::uint8_t> except = std::nullopt,
                    bool editorsOnly = false);
    /// The peer's role as it is, with a line for it (Role).
    void sendRole(GameHost& host, std::uint8_t peer, const std::string& text);
    /// A peer's edit request closed, to the editors (EditRequest, not open).
    void requestClosed(GameHost& host, std::uint8_t peer, const std::string& name, const std::string& reason);
    /// The peer's pending edit request dropped unanswered: it told `text`, the editors the request closed.
    void dropRequest(GameHost& host, std::uint8_t peer, const std::string& text);

    // The running game.
    /// Each document object's live object anew (after a map load: its records' extents).
    void rebind(GameHost& host);
    /// What the op touched (the world owners), made live: `before` the owners of what it changed
    /// as they were, `moved` those only moved. Held things (heldForPublish) are left as the World
    /// has them and noted in held_. True when any owner wasn't held.
    bool applyLive(GameHost& host, const std::set<mapedit::ObjId>& owners, const std::set<mapedit::ObjId>& moved);
    /// A document object (with its inventory) made in the running game; false when it can't be.
    bool createLive(GameHost& host, mapedit::ObjId id);
    /// The world owner (top-level ancestor) of a document object, or 0.
    mapedit::ObjId ownerOf(const mapedit::MapDocument& doc, mapedit::ObjId id) const;
    void retryWaiting(GameHost& host);
    /// A live object's items (depth first, inventory order): type and script id.
    static std::vector<std::pair<std::string, std::int32_t>> itemScriptIds(World& w, const ServerObject& holder);
    /// A document object made again keeps the script ids its old live self (and items) had, so
    /// scripts holding them find it; what can't be kept goes to the session's problems.
    void repointScriptIds(GameHost& host, mapedit::ObjId id, std::int32_t oldScriptId,
                          const std::vector<std::pair<std::string, std::int32_t>>& oldItems);

    const things::ThingDb& things_;
    Options options_;
    mapedit::MapDocument base_;
    mapedit::MapDocument working_;
    std::vector<std::uint8_t> baseBytes_; ///< encodeSessionDocument(base_), for late joiners
    std::vector<std::uint8_t> baseBody_;  ///< the Base message's body made of it (made once a version)
    std::vector<Entry> log_;
    std::size_t logBytes_ = 0;  ///< the log's ops' bytes (the compact snapshot's threshold)
    std::uint32_t logBase_ = 0; ///< the log index before the log's first entry
    std::uint32_t version_ = 1;
    mapedit::LockTable locks_;
    std::map<std::uint8_t, Peer> peers_;
    std::deque<std::pair<std::uint8_t, std::vector<std::uint8_t>>> inbox_;
    std::uint32_t transfer_ = 0;
    std::uint32_t nowMs_ = 0;
    bool playTest_ = false;
    std::uint32_t sandboxMode_ = 0x100;
    std::map<mapedit::ObjId, std::uint32_t> live_; ///< document object -> its extent in the world
    std::set<mapedit::ObjId> waiting_;
    std::set<mapedit::ObjId> held_;
    /// The rule (mapedit::heldForPublish) for a document object, the base against the working copy.
    bool holds(mapedit::ObjId id) const;
    /// held_ without what's back as published (an undo, an edit put back by hand).
    void pruneHeld();
    bool needRebind_ = true;
    std::uint32_t lastLiveMs_ = 0;
    std::vector<mapedit::session::RosterEntry> rosterSent_;
    std::vector<mapedit::session::EditorState> editorsSent_;
    void refreshEditors(GameHost& host);
    // The automatic object publish.
    bool objectsDirty_ = false;
    std::uint32_t objectsDirtySince_ = 0;
    std::uint32_t objectsChangedAt_ = 0;
    // The running script's debug info (for hot reload by name).
    std::optional<script::DebugInfo> runningDebug_;
    bool runningDebugTried_ = false;
    Stats stats_;
    std::map<std::uint32_t, mapedit::session::Problem> problems_;
    // The compact snapshot made for the log as it stands (made once per log index).
    std::uint32_t snapshotAt_ = 0;
    std::vector<std::uint8_t> snapshotBody_;
    // The mod project.
    std::string modId_;
    std::string modError_;
    std::map<std::string, ModEntry> mod_;
    std::uint32_t modVersions_ = 0; ///< (the last version given: each new working version the next)
    std::string modName_;           ///< mode.json's "name" at the start (else the id)
    std::size_t modHeld_ = 0;       ///< files held now (none: the idle check skipped)
    std::vector<std::uint8_t> modListBody_; ///< the list as last encoded (empty: made again when sent)
};

/// The host's options for an edit session with `director` (which must outlive the host): the
/// "[EDIT] <host>" name, the sandbox's mode and rules (no limits, no map cycle or vote, players
/// can't die as the director says), its messages. Returns the map to host.
std::filesystem::path setUpEditSession(EditSessionDirector& director, GameHost::Options& o, const std::string& hostName);

} // namespace nox::server
