// A NoX Core client's side of Undead Tide (server/tide.hpp tidemsg): MSG_NOXCPP_MODE_F5 as it comes
// in, kept for the client's windows (noxsdl's Tide UI), and our picks and answers sent back. Idle on
// any other server (none sends 0xF5). Made with the client (HeadlessClient::setNoxcppHandler).
#pragma once

#include "noxcore/client/headless_client.hpp"
#include "noxcore/modes/mode.hpp"
#include "noxcore/server/tide.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace nox::client {

class TideClient final : public modes::ClientMode {
public:
    /// Takes `client`'s MSG_NOXCPP_MODE_F5; `client` must outlive it.
    explicit TideClient(HeadlessClient& client);
    /// (Its handler stays on the client, idle: the client may go first or after.)
    ~TideClient() override;
    TideClient(const TideClient&) = delete;
    TideClient& operator=(const TideClient&) = delete;

    void onMessage(std::span<const std::uint8_t> m);

    /// Our player is in a run's arena (kState).
    bool run() const noexcept { return run_; }
    /// The run as the server last told it (kHud; none outside a run), and a count of those told (one
    /// more each, so the weapons' waits can be run down from its arrival).
    const std::optional<server::tidemsg::Hud>& hud() const noexcept { return hud_; }
    std::uint32_t hudVersion() const noexcept { return hudVersion_; }
    /// A level-up's offer (kOffer; its level 0 and no picks: none open), a count of those told.
    const server::tidemsg::Offer& offer() const noexcept { return offer_; }
    std::uint32_t offerVersion() const noexcept { return offerVersion_; }
    /// Our pick of the offer (kPick, by its index); the offer put away.
    void pick(std::size_t index);
    /// At the Tide lobby's gate, the server's question (kAsk): the difficulty now, while it's open.
    const std::optional<std::uint8_t>& ask() const noexcept { return ask_; }
    /// Our answer (kChoose): the difficulty, and the run begun if we're still at the gate; the
    /// question put away.
    void chooseDifficulty(std::uint8_t difficulty);
    /// The question put away unanswered (we stay in the lobby; walking onto the gate again asks again).
    void dismissAsk() { ask_.reset(); }
    /// The last run's results (kResults), a count of those told.
    const std::optional<server::tidemsg::Results>& results() const noexcept { return results_; }
    std::uint32_t resultsVersion() const noexcept { return resultsVersion_; }

    // ---- tests (noxsdl's --fake-tide screens)
    void setAskForTest(std::optional<std::uint8_t> current) { ask_ = current; }
    void setForTest(std::optional<server::tidemsg::Hud> hud, server::tidemsg::Offer offer, std::optional<server::tidemsg::Results> results)
    {
        hud_ = std::move(hud);
        offer_ = std::move(offer);
        results_ = std::move(results);
        ++hudVersion_;
        ++offerVersion_;
        ++resultsVersion_;
    }

private:
    HeadlessClient& client_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0); ///< (the handler's check that we're here)
    bool run_ = false;
    std::optional<server::tidemsg::Hud> hud_;
    std::uint32_t hudVersion_ = 0;
    server::tidemsg::Offer offer_;
    std::uint32_t offerVersion_ = 0;
    std::optional<server::tidemsg::Results> results_;
    std::optional<std::uint8_t> ask_;
    std::uint32_t resultsVersion_ = 0;
};

} // namespace nox::client
