// A NoX Core client's side of the co-op campaign (coop.hpp): MSG_NOXCPP_COOP as it comes in,
// kept for the client's windows (modes/coop/ui), and what the client does about it itself: a
// guest greeted by the host sends its own character (HeadlessClient::sendCharacter); a chapter
// screen's end is told back (chapterDone). Idle on any other server (none sends 0xF7). The mode's
// client side (ClientMode), made only for noxcore.exe's own connection (Mode::everyClient off: a
// bot's mustn't send its character), and by the tests.
#pragma once

#include "noxcore/client/headless_client.hpp"
#include "noxcore/coop/coop_proto.hpp"
#include "noxcore/modes/mode.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
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

private:
    client::HeadlessClient& client_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0); ///< (the handler's check that we're here)
    std::optional<coopmsg::Hello> hello_;
    coopmsg::Roster roster_;
    std::vector<std::pair<coopmsg::Notice, std::string>> notices_;
    bool characterSent_ = false;
    std::vector<coopmsg::Cine> cines_;
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
};

} // namespace nox::coop
