// Undead Tide's client side (tide_client.hpp).
#include "noxcore/client/tide_client.hpp"

#include "noxcore/server/tide.hpp"

namespace nox::client {

TideClient::TideClient(HeadlessClient& client)
    : client_(client)
{
    server::loadTideData(); // (the names and words its windows show)
    // (The handler outlives us on the client: it calls in only while we're here, so neither has to
    // go first.)
    client_.setNoxcppHandler(static_cast<std::uint8_t>(net::Op::NoxcppModeF5), [this, alive = std::weak_ptr<int>(alive_)](std::span<const std::uint8_t> m) {
        if (!alive.expired()) {
            onMessage(m);
        }
    });
}

TideClient::~TideClient() = default;

void TideClient::pick(std::size_t index)
{
    if (index < offer_.picks.size()) {
        client_.sendNoxcpp(server::tidemsg::pickMessage(static_cast<std::uint8_t>(index), offer_.level)); // (a stock client says its number)
        offer_ = {};
        ++offerVersion_;
    }
}

void TideClient::chooseDifficulty(std::uint8_t difficulty)
{
    if (difficulty < server::kTideDifficulties) {
        client_.sendNoxcpp(server::tidemsg::chooseMessage(difficulty)); // (a stock client says ?difficulty NAME)
    }
    ask_.reset();
}

// Whether we're in a run's arena, the run, a level-up's offer, the results.
void TideClient::onMessage(std::span<const std::uint8_t> m)
{
    const auto v = server::tidemsg::parse(m);
    if (!v) {
        return;
    }
    const auto rest = v->second;
    switch (v->first) {
    case server::tidemsg::kState:
        if (!rest.empty()) {
            run_ = rest[0] != 0;
            ask_.reset();
            if (!run_) {
                hud_.reset();
                offer_ = {};
                ++offerVersion_;
            } else if (results_) {
                results_.reset(); // (a new run: the last one's results put away)
                ++resultsVersion_;
            }
        }
        break;
    case server::tidemsg::kHud:
        if (auto h = server::tidemsg::parseHud(rest)) {
            hud_ = std::move(*h);
            ++hudVersion_;
        }
        break;
    case server::tidemsg::kOffer:
        if (auto o = server::tidemsg::parseOffer(rest)) {
            offer_ = std::move(*o);
            ++offerVersion_;
        }
        break;
    case server::tidemsg::kAsk:
        if (!rest.empty()) {
            if (rest[0] < server::kTideDifficulties) {
                ask_ = rest[0];
            } else {
                ask_.reset(); // (kAskClosed)
            }
        }
        break;
    case server::tidemsg::kResults:
        if (auto r = server::tidemsg::parseResults(rest)) {
            results_ = std::move(*r);
            ++resultsVersion_;
        }
        break;
    default:
        break;
    }
}

} // namespace nox::client
