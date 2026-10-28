// A NoX Core client's side of Delving Mode (server/delve_map.hpp): MSG_NOXCPP_MODE_F4 as it comes
// in, kept for the client's windows (noxsdl's Delve UI: the run's map, a campfire's offer, an
// altar's, the level's objective, the records), and our picks sent back. The server's multiclass
// word is the core client's own switch (HeadlessClient::setMulticlass). Idle on any other server
// (none sends 0xF4). Made with the client (HeadlessClient::setNoxcppHandler).
#pragma once

#include "noxcore/client/headless_client.hpp"
#include "noxcore/modes/mode.hpp"
#include "noxcore/server/delve_map.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace nox::client {

class DelveClient final : public modes::ClientMode {
public:
    /// Takes `client`'s MSG_NOXCPP_MODE_F4; `client` must outlive it.
    explicit DelveClient(HeadlessClient& client);
    /// (Its handler stays on the client, idle: the client may go first or after.)
    ~DelveClient() override;
    DelveClient(const DelveClient&) = delete;
    DelveClient& operator=(const DelveClient&) = delete;

    void onMessage(std::span<const std::uint8_t> m);

    /// The run's map as the server last told it (kRun); none outside a run. `version` bumps at each.
    const std::optional<server::delvemap::Run>& run() const noexcept { return run_; }
    std::uint32_t version() const noexcept { return version_; }
    /// A campfire's offer (kCampfire): the spells we may strengthen one of, with their levels now;
    /// empty when none is open.
    const std::vector<server::delvemap::Upgradable>& campfire() const noexcept { return campfire_; }
    /// Our pick at the campfire (kUpgrade): `spell` a level up; the offer put away.
    void upgradeSpell(std::uint8_t spell);
    /// An altar's offer (kBlessing): three blessings to take one of; empty when none is open.
    const std::vector<server::delvemap::BlessingOffer>& blessingOffer() const noexcept { return blessing_; }
    /// Our pick at the altar (kBless); the offer put away.
    void takeBlessing(server::delvemap::Blessing b);
    /// A level's bonus objective (kObjective; kind 0: none), and a count of those told (one more
    /// each, so a clock's seconds left can be counted from its arrival).
    const server::delvemap::Objective& objective() const noexcept { return objective_; }
    std::uint32_t objectiveVersion() const noexcept { return objectiveVersion_; }
    /// The server's deepest runs (kRecords), deepest first; empty: none told (or none yet).
    const std::vector<server::delvemap::Record>& records() const noexcept { return records_; }

    // ---- tests (noxsdl's --fake-delve screens)
    void setCampfireForTest(std::vector<server::delvemap::Upgradable> offer) { campfire_ = std::move(offer); }
    void setBlessingForTest(std::vector<server::delvemap::BlessingOffer> offer) { blessing_ = std::move(offer); }
    void setObjectiveForTest(server::delvemap::Objective o)
    {
        objective_ = std::move(o);
        ++objectiveVersion_;
    }
    void setRecordsForTest(std::vector<server::delvemap::Record> records) { records_ = std::move(records); }
    void setRunForTest(server::delvemap::Run run)
    {
        run_ = std::move(run);
        ++version_;
    }

private:
    HeadlessClient& client_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0); ///< (the handler's check that we're here)
    std::optional<server::delvemap::Run> run_;
    std::uint32_t version_ = 0;
    std::vector<server::delvemap::Upgradable> campfire_;
    std::vector<server::delvemap::BlessingOffer> blessing_;
    server::delvemap::Objective objective_;
    std::uint32_t objectiveVersion_ = 0;
    std::vector<server::delvemap::Record> records_;
};

} // namespace nox::client
