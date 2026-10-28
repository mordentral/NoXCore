// Delving Mode's client side (delve_client.hpp).
#include "noxcore/client/delve_client.hpp"

#include <algorithm>

namespace nox::client {

DelveClient::DelveClient(HeadlessClient& client)
    : client_(client)
{
    // (The handler outlives us on the client: it calls in only while we're here, so neither has to
    // go first.)
    client_.setNoxcppHandler(static_cast<std::uint8_t>(net::Op::NoxcppModeF4), [this, alive = std::weak_ptr<int>(alive_)](std::span<const std::uint8_t> m) {
        if (!alive.expired()) {
            onMessage(m);
        }
    });
}

DelveClient::~DelveClient() = default;

void DelveClient::upgradeSpell(std::uint8_t spell)
{
    if (std::ranges::any_of(campfire_, [&](const server::delvemap::Upgradable& u) { return u.spell == spell; })) {
        client_.sendNoxcpp(server::delvemap::upgradeMessage(spell)); // (a stock client says the spell's number)
        campfire_.clear();
    }
}

void DelveClient::takeBlessing(server::delvemap::Blessing b)
{
    if (std::ranges::any_of(blessing_, [&](const server::delvemap::BlessingOffer& o) { return o.blessing == b; })) {
        client_.sendNoxcpp(server::delvemap::blessMessage(b)); // (a stock client says its number)
        blessing_.clear();
    }
}

// The run as it stands, or over; a campfire's or an altar's offer, the objective, the records; and
// the multiclass word, the client's own.
void DelveClient::onMessage(std::span<const std::uint8_t> m)
{
    const auto v = server::delvemap::parse(m);
    if (!v) {
        return;
    }
    const auto rest = v->second;
    switch (v->first) {
    case server::delvemap::kRun:
        if (auto run = server::delvemap::parseRun(rest)) {
            run_ = std::move(*run);
            ++version_;
        }
        break;
    case server::delvemap::kEnd:
        run_.reset();
        campfire_.clear();
        blessing_.clear();
        objective_ = {};
        ++version_;
        break;
    case server::delvemap::kMulticlass:
        if (!rest.empty()) {
            client_.setMulticlass(rest[0] != 0);
        }
        break;
    case server::delvemap::kCampfire:
        if (auto offer = server::delvemap::parseCampfire(rest)) {
            campfire_ = std::move(*offer);
        }
        break;
    case server::delvemap::kObjective:
        if (auto o = server::delvemap::parseObjective(rest)) {
            objective_ = std::move(*o);
            ++objectiveVersion_;
        }
        break;
    case server::delvemap::kBlessing:
        if (auto offer = server::delvemap::parseBlessing(rest)) {
            blessing_ = std::move(*offer);
        }
        break;
    case server::delvemap::kRecords:
        if (auto records = server::delvemap::parseRecords(rest)) {
            records_ = std::move(*records);
        }
        break;
    default:
        break;
    }
}

} // namespace nox::client
