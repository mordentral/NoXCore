// A NoX Core client's side of the co-op campaign (coop.hpp): MSG_NOXCPP_COOP as it comes in,
// kept for the client's windows (modes/coop/ui), and what the client does about it itself: a
// guest greeted by the host sends its own character (HeadlessClient::sendCharacter); a chapter
// screen's end is told back (chapterDone); a guest's minimap goes to the host to keep and comes back
// at each map's load (reportSeen, takeSeen). Idle on any other server (none sends 0xF7). The mode's
// client side (ClientMode), made only for noxcore.exe's own connection (Mode::everyClient off: a
// bot's mustn't send its character), and by the tests.
#pragma once

#include "noxcore/client/headless_client.hpp"
#include "noxcore/client/spell_bars.hpp"
#include "noxcore/coop/coop_proto.hpp"
#include "noxcore/modes/mode.hpp"
#include "noxcore/player/player_file.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nox::coop {

class CoopClient final : public modes::ClientMode {
public:
    /// Takes `client`'s MSG_NOXCPP_COOP (setNoxcppHandler); `client` must outlive it.
    explicit CoopClient(client::HeadlessClient& client);
    /// (Its handler stays on the client, idle: the client may go first or after.)
    ~CoopClient() override;
    CoopClient(const CoopClient&) = delete;
    CoopClient& operator=(const CoopClient&) = delete;

    void onMessage(std::span<const std::uint8_t> m);

    /// The campaign as the host greeted us (none: not a co-op game, or not yet).
    const std::optional<coopmsg::Hello>& hello() const noexcept { return hello_; }
    bool inCampaign() const noexcept { return hello_.has_value(); }
    bool guest() const noexcept { return hello_ && !hello_->youAreHost; }
    const coopmsg::Roster& roster() const noexcept { return roster_; }
    /// The notices since the last call, oldest first.
    std::vector<std::pair<coopmsg::Notice, std::string>> takeNotices() { return std::exchange(notices_, {}); }
    /// Whether our character went to the host (a guest's, at the greeting).
    bool characterSent() const noexcept { return characterSent_; }
    /// Our campaign character as the host's last save wrote it (its server sections, as a quest's
    /// SAVEDATA), to keep; taken once.
    std::optional<std::vector<std::uint8_t>> takeCharacter() { return std::exchange(character_, std::nullopt); }

    /// The host's client's calls we're to make on our own (a guest's), oldest first.
    std::vector<coopmsg::Cine> takeCines() { return std::exchange(cines_, {}); }
    /// Each of those calls handed on as it comes in (from the client's receive, after its tracer
    /// saw the message: in its turn among the game's own messages), in place of keeping it for
    /// takeCines; none: kept. (noxsdl's coop_ui: into its Cinematics' message queue, issue #14.)
    void setCineSink(std::function<void(const coopmsg::Cine&)> sink) { cineSink_ = std::move(sink); }
    /// The bars are up as the host last said (a scene under way), and its actor's net code.
    bool sceneOn() const noexcept { return scene_; }
    std::uint16_t sceneActor() const noexcept { return actor_; }
    /// A chapter screen to show (taken once).
    std::optional<coopmsg::Chapter> takeChapter() { return std::exchange(chapter_, std::nullopt); }
    /// Ours is over: the host told (kChapterReady).
    void chapterDone();
    /// Who's still reading the chapter screen, of how many (none told yet: 0 of 0).
    std::pair<std::uint8_t, std::uint8_t> readyState() const noexcept { return ready_; }
    /// The host's: the chapter screen's wait over for everyone (kHostCmd).
    void skipBriefing();
    /// The host's talk as last told (open or closed), and a count of those told.
    const coopmsg::DialogMirror& dialog() const noexcept { return dialog_; }
    std::uint32_t dialogVersion() const noexcept { return dialogVersion_; }
    /// The party's creatures as last told (kCreatures: every member's summons and charmed
    /// creatures, by net code, sorted), and a count of the lists told.
    const std::vector<std::uint16_t>& partyCreatures() const noexcept { return partyCreatures_; }
    std::uint32_t partyCreaturesVersion() const noexcept { return partyCreaturesVersion_; }
    /// A guest's minimap: the walls drawn (cell keys) and obelisks marked (extents) for the first
    /// time on `map` (its file's name), told to the host to keep (kSeenReport, in parts).
    void reportSeen(std::string_view map, std::span<const std::uint16_t> walls, std::span<const std::uint16_t> obelisks);
    /// What the campaign kept of our minimap for `map` (kSeen), once our view of that map is up:
    /// what's come in so far, taken (later parts at a later call). Held till then; another map's
    /// coming in drops it.
    std::optional<coopmsg::SeenCells> takeSeen(std::string_view map);

private:
    client::HeadlessClient& client_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0); ///< (the handler's check that we're here)
    std::optional<coopmsg::Hello> hello_;
    coopmsg::Roster roster_;
    std::vector<std::pair<coopmsg::Notice, std::string>> notices_;
    bool characterSent_ = false;
    std::vector<coopmsg::Cine> cines_;
    std::function<void(const coopmsg::Cine&)> cineSink_;
    bool scene_ = false;
    std::uint16_t actor_ = 0;
    std::optional<coopmsg::Chapter> chapter_;
    std::pair<std::uint8_t, std::uint8_t> ready_{0, 0};
    coopmsg::DialogMirror dialog_;
    std::uint32_t dialogVersion_ = 0;
    std::vector<std::uint16_t> partyCreatures_;
    std::uint32_t partyCreaturesVersion_ = 0;
    std::vector<std::uint8_t> partial_;
    std::optional<std::vector<std::uint8_t>> character_;
    std::optional<coopmsg::SeenCells> seen_; ///< (kSeen's parts as they come, till taken)
};

/// A guest's campaign character beside the character it joined with (`characterFile`): "<name>
/// (Co-op).plr" in the same folder, which the host's saves write (noxsdl's coop_ui). That file
/// itself when it's the one joined with; none without one.
std::filesystem::path campaignCharacterFile(const std::filesystem::path& characterFile);

/// A NoX Core guest's spell bars once the host has greeted it (noxsdl's coop_ui). The bars of the
/// file it joined with (`characterFile`) are loaded by then. Returns the bars to use instead, or
/// none to keep those:
/// - a runner the campaign kept (`kept`: Hello's characterKept) plays its campaign character again,
///   so it gets that character's bars, from its "(Co-op).plr" when that file has bars of
///   `characterClass`;
/// - otherwise a character no solo game saved (no GameData: a network character, or no file) gets
///   empty bars, as a new solo character's are, so no spell it doesn't know is on them. A late
///   joiner fills its bars from its book itself.
std::optional<client::SpellBars> guestBars(const std::filesystem::path& characterFile, player::CharacterClass characterClass, bool kept);

} // namespace nox::coop
