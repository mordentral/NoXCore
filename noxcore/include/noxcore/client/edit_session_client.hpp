// A noxsdl client's side of the collaborative map editor session (the host's side:
// server/edit_session.hpp; the messages: mapedit/session_proto.hpp). Headless, so the tests drive
// scripted editors with it and noxsdl's editor uses the same code.
//
// The working copy: the host's committed ops in log order, with this client's own ops not yet
// answered applied on top (an optimistic preview). Every Committed rebases: the pending ops undone,
// the committed one applied, the pending ones applied again (one that no longer applies is dropped,
// as if refused). The author's own op echoes back as its ack; a Rejected one is rolled back. So
// every member's committed state is the host's, op for op, byte for byte.
//
// Locks: the table is the host's copy, sent on every change; a selection is asked for with
// requestLocks (granted at once locally where the copy allows, then cut to what the host's
// LockAnswer grants).
#pragma once

#include "noxcore/mapedit/document.hpp"
#include "noxcore/mapedit/locks.hpp"
#include "noxcore/mapedit/ops.hpp"
#include "noxcore/mapedit/session_proto.hpp"

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

namespace nox::things {
struct ThingDb;
}

namespace nox::client {

class EditSessionClient {
public:
    /// Sends one MSG_NOXCPP_EDIT message (HeadlessClient::sendEdit).
    using Send = std::function<void(std::vector<std::uint8_t> message, bool reliable)>;

    /// `publishCache`: where a full publish's map is written (<cache>\<name>\<name>.map, .nxz:
    /// the same bytes the host wrote, so the map change needs no download); empty: not written.
    EditSessionClient(const things::ThingDb& things, Send send, std::filesystem::path publishCache = {});

    /// A message from the server (HeadlessClient's edit handler).
    void onMessage(std::span<const std::uint8_t> message);
    /// Presence sends (at most 10 a second, only on a change).
    void tick(std::uint32_t nowMs);
    /// While held, what comes waits (the editor's stroke previews the working copy in place:
    /// nothing may be applied under it); let go, it's handled in order.
    void setHold(bool hold);

    // ---- the session
    bool inSession() const { return hello_.has_value(); }
    const mapedit::session::Hello* hello() const { return hello_ ? &*hello_ : nullptr; }
    /// Joined and caught up to the log index Synced named.
    bool synced() const { return synced_; }
    bool joined() const { return joinSent_; }
    mapedit::session::Role role() const { return role_; }
    bool canEdit() const { return synced_ && role_ >= mapedit::session::Role::Editor && !playTestRunning(); }
    bool isOwner() const { return role_ == mapedit::session::Role::Owner; }
    /// Whether the host takes our saves of the mod's files (an editor or the owner; a play-test
    /// round doesn't stop them).
    bool canEditMod() const { return role_ >= mapedit::session::Role::Editor; }
    bool playTestRunning() const { return hello_ && (hello_->flags & mapedit::session::kPlayTestRunning) != 0; }
    mapedit::PeerId self() const { return hello_ ? hello_->you : 0; }
    const std::vector<mapedit::session::RosterEntry>& roster() const { return roster_; }
    /// A member's name ("editor 3" when not known).
    std::string nameOf(mapedit::PeerId peer) const;
    std::uint8_t colourOf(mapedit::PeerId peer) const;
    std::uint32_t logIndex() const { return logIndex_; }
    std::uint32_t version() const { return hello_ ? hello_->version : 0; }

    // ---- the working copy
    bool hasDocument() const { return hasDoc_; }
    /// The working copy (the committed state with our pending ops on top). An editor may hand its
    /// own document to use instead (its contents replaced by the session's at once).
    mapedit::MapDocument& document() { return *doc_; }
    const mapedit::MapDocument& document() const { return *doc_; }
    void attach(mapedit::MapDocument* external);
    /// The published version (what the running game has: the pending shading's reference).
    const mapedit::MapDocument& published() const { return published_; }
    /// Bumped by every change to the working copy.
    std::uint64_t revision() const { return revision_; }
    std::size_t pendingOps() const { return pending_.size(); }
    /// The committed state alone (our pending ops left out): what the host's working copy is.
    mapedit::MapDocument committed() const;
    const mapedit::LockTable& locks() const { return locks_; }

    // ---- actions
    void join();
    /// Our op: applied to the working copy now and sent. Its inverse (for our undo) in `inverse`.
    /// Nothing (not applied, nothing sent) when we can't edit or it doesn't apply: `why` says.
    std::optional<std::uint32_t> submit(const mapedit::Op& op, mapedit::Op* inverse = nullptr, std::string* why = nullptr);
    /// Our undo (or redo) in the session, selective: `apply` (the undo's inverse, or the redo's op)
    /// cut to what still holds `expect`'s values (the op undone, or the undo redone): what another
    /// editor changed since, and what their later work depends on, is left as it is.
    struct Selective {
        std::optional<std::uint32_t> seq; ///< sent (nothing when refused or nothing was left)
        bool nothingLeft = false;         ///< everything was changed by others since: the entry is used up
        std::string why;                  ///< refused: why (locks, can't edit...)
        std::string note;                 ///< "2 left as they are: wall 30,40 (changed since by Bob)..."
        std::size_t skipped = 0;
    };
    Selective submitSelective(const mapedit::Op& apply, const mapedit::Op& expect);
    /// Who last changed an element in an op of someone else's (0: nobody seen).
    mapedit::PeerId lastChangedBy(const mapedit::ElementKey& key) const;
    /// Our selection / open panels as one lock request: granted at once where the copy allows;
    /// returns what was refused locally (held by others).
    std::set<mapedit::ElementKey> requestLocks(const std::set<mapedit::ElementKey>& keys);
    /// The host's Take Over: `key` forced free (forceRelease: the owner's alone) and added to
    /// what we hold.
    void takeOver(const mapedit::ElementKey& key);
    void requestEdit(const std::string& reason);
    void answer(mapedit::PeerId peer, std::uint8_t decision);
    void revoke(mapedit::PeerId peer);
    void forceRelease(const mapedit::ElementKey& key);
    void publish(bool full = true);
    void reloadScript(bool hot = true);
    void playTest(bool start, std::uint32_t mode = 0x100, bool playersCanDie = false);
    void settings(mapedit::session::EditMode mode, std::uint8_t flags);
    void presence(float x, float y, std::uint8_t tool, float radius, bool inView);
    /// Our character parked as an observer (the Edit side: not left standing to be killed), or back
    /// in play: at `spot` (the editor's camera) when given, else where it was parked.
    void park(bool on, std::optional<std::pair<float, float>> spot = std::nullopt);
    /// Our character into the game at a player's spot (the live view's "Jump in here"): `target`
    /// is the player's net code (LiveEntry::id).
    void jumpIn(std::uint16_t target);
    /// Whether the host says our character is parked (observing by the session's doing).
    bool parked() const { return parked_; }

    // ---- the session's mod project (the host's mods\<id>, when it carries one)
    /// One of its files: the host's list entry, and its working content when it's come.
    struct ModFileState {
        mapedit::session::ModFileInfo info;
        std::optional<std::vector<std::uint8_t>> bytes;
        std::uint32_t bytesVersion = 0; ///< the working version `bytes` is
        std::uint32_t arrivals = 0;     ///< contents taken for it so far (a copy sent back after a refused save counts too)
        /// Its working content is here (a deleted one has none to wait for).
        bool have() const { return info.working == 0 || (bytes && bytesVersion == info.working); }
        /// Changed in the working copy since the host's last publish of the mod (shown pending).
        bool pending() const { return info.working != info.published; }
    };
    struct ModState {
        std::string id;
        std::string name;
        std::map<std::string, ModFileState> files;
    };
    /// The mod project (none: the session carries none, or it hasn't come yet).
    const std::optional<ModState>& mod() const { return mod_; }
    /// Bumped by every change to it (its list, a file's content).
    std::uint64_t modRevision() const { return modRevision_; }
    /// Every working file's content is here.
    bool modReady() const;
    /// Our save of a file (made from the working version we have; a new file when it has none),
    /// or its delete: the host keeps it as the working version (pending till the owner publishes)
    /// or refuses it with a log line, sending its copy back.
    void modSave(const std::string& path, std::span<const std::uint8_t> bytes);
    void modDelete(const std::string& path);
    /// A file held while we type in it (let go with false); `force`: the owner's take over.
    void modLock(const std::string& path, bool on, bool force = false);
    /// (owner) The working files written into the host's mods\<id>.
    void modPublish();
    /// Whether our Join names the mod project's messages (on by default; off: as a client from
    /// before them, for the tests).
    void setTakesModFiles(bool on) { takesModFiles_ = on; }

    // ---- what came
    struct Event {
        enum class Kind : std::uint8_t {
            Synced,     ///< caught up
            Committed,  ///< another's op (or the host's) applied: `op`
            Acked,      ///< our op (seq) committed
            Rejected,   ///< our op (seq) refused and rolled back: `text`
            RolledBack, ///< our pending op (seq) no longer applied after another's and was dropped
            Role,       ///< our role changed or the host said something about it: `text`
            Request,    ///< (owner) an edit request opened or closed: `request`
            Published,  ///< a publish: `published`
            ScriptDiag, ///< `diag`
            Log,        ///< a session log line: `text`
            Locks,      ///< the lock table changed
            LockAnswer, ///< the host granted `keys` of our request
            Park,       ///< our character parked or back (parked()); `text`: the host's reason when it didn't
            Problem,    ///< a session problem opened or closed: `problem`
            ModProject, ///< the mod project's list changed (versions, holders, files added or gone)
            ModFile,    ///< a mod file's working content came: `text` its path
        };
        Kind kind = Kind::Log;
        std::uint32_t seq = 0;
        mapedit::PeerId author = 0;
        std::string text;
        std::optional<mapedit::Op> op;
        std::optional<mapedit::Op> inverse; ///< (Acked) the committed op's inverse, for our undo
        mapedit::session::EditRequest request;
        mapedit::session::Published published;
        mapedit::session::ScriptDiag diag;
        std::set<mapedit::ElementKey> keys;
        mapedit::session::Problem problem;
    };
    std::vector<Event> takeEvents() { return std::exchange(events_, {}); }
    const std::map<mapedit::PeerId, mapedit::session::Presence>& presences() const { return presences_; }
    const std::vector<mapedit::session::LiveEntry>& live() const { return live_; }
    std::uint32_t liveVersion() const { return liveVersion_; }
    /// Who's editing (the host's Editors message): every editor with their character's net code
    /// and what they're doing, for the pencil over them.
    const std::vector<mapedit::session::EditorState>& editors() const { return editors_; }
    /// The editor whose character is `netCode`, or null (not an editor, or no character).
    const mapedit::session::EditorState* editorByNetCode(std::uint16_t netCode) const
    {
        for (const auto& e : editors_) {
            if (netCode != 0 && e.netCode == netCode) return &e;
        }
        return nullptr;
    }
    /// The editor `peer`'s state, or null (not an editor).
    const mapedit::session::EditorState* editorByPeer(mapedit::PeerId peer) const
    {
        for (const auto& e : editors_) {
            if (e.peer == peer) return &e;
        }
        return nullptr;
    }
    /// (owner) Edit requests still open.
    const std::vector<mapedit::session::EditRequest>& requests() const { return requests_; }
    const std::optional<mapedit::session::ScriptDiag>& lastDiag() const { return lastDiag_; }
    /// Session log lines, newest last (the last 200).
    const std::deque<std::string>& logLines() const { return log_; }
    /// The maps a full publish wrote here (tests).
    std::uint32_t publishesWritten() const { return publishesWritten_; }
    /// Whether the last catch-up came as a compact snapshot (tests).
    bool caughtUpBySnapshot() const { return bySnapshot_; }
    /// The session's open problems (the Problems panel), by key.
    const std::map<std::uint32_t, mapedit::session::Problem>& problems() const { return problems_; }

private:
    struct PendingOp {
        std::uint32_t seq = 0;
        mapedit::Op op;
        mapedit::Op inverse;
        mapedit::Op sent;                ///< as sent (with the host's notes)
        std::vector<std::uint8_t> bytes; ///< the op as the host will echo it (without the notes)
    };
    void handle(const mapedit::session::Message& m);
    void onCommitted(const mapedit::session::Committed& c);
    void onRejected(const mapedit::session::Rejected& r);
    void onPublished(const mapedit::session::Published& p);
    /// Out of step with the host (`why`, logged): the pending ops dropped and a join sent again.
    void catchUpAgain(const std::string& why);
    /// The pending ops undone (the working copy back to the committed state)...
    void undoPending();
    /// ...and applied again (those that no longer apply dropped, reported).
    void redoPending();
    void sendBody(std::uint8_t sub, std::span<const std::uint8_t> body);
    void push(Event e) { events_.push_back(std::move(e)); }
    void logText(std::string line);

    const things::ThingDb& things_;
    Send send_;
    std::filesystem::path cache_;
    mapedit::session::Reassembler in_;
    bool hold_ = false;
    std::vector<std::vector<std::uint8_t>> held_;
    std::uint32_t transfer_ = 0;
    std::optional<mapedit::session::Hello> hello_;
    mapedit::session::Role role_ = mapedit::session::Role::Player;
    bool joinSent_ = false;
    bool synced_ = false;
    bool hasDoc_ = false;
    mapedit::MapDocument own_;
    mapedit::MapDocument* doc_ = &own_;
    mapedit::MapDocument published_;
    std::uint64_t revision_ = 0;
    std::uint32_t logIndex_ = 0;
    std::uint32_t nextSeq_ = 1;
    std::uint32_t lockSeq_ = 0;
    std::deque<PendingOp> pending_;
    mapedit::LockTable locks_;
    std::vector<mapedit::session::RosterEntry> roster_;
    std::map<mapedit::PeerId, mapedit::session::Presence> presences_;
    std::vector<mapedit::session::LiveEntry> live_;
    std::uint32_t liveVersion_ = 0;
    std::vector<mapedit::session::EditorState> editors_;
    std::vector<mapedit::session::EditRequest> requests_;
    std::optional<mapedit::session::ScriptDiag> lastDiag_;
    std::deque<std::string> log_;
    std::vector<Event> events_;
    std::optional<mapedit::session::Presence> presenceWanted_;
    std::optional<mapedit::session::Presence> presenceSent_;
    std::uint32_t presenceAt_ = 0;
    std::uint32_t nowMs_ = 0;
    std::uint32_t publishesWritten_ = 0;
    bool bySnapshot_ = false;
    bool parked_ = false;
    std::map<std::uint32_t, mapedit::session::Problem> problems_;
    std::map<mapedit::ElementKey, mapedit::PeerId> lastAuthor_;
    // The mod project.
    void onModProject(mapedit::session::ModProject m);
    void onModFile(mapedit::session::ModFile m);
    std::optional<ModState> mod_;
    std::uint64_t modRevision_ = 0;
    bool takesModFiles_ = true;
    std::vector<std::uint8_t> joinBody() const;
};

} // namespace nox::client
