// The co-op campaign's client side (coop_client.hpp).
#include "noxcore/coop/coop_client.hpp"

#include "noxcore/util/text.hpp"

#include <exception>
#include <string>

namespace nox::coop {

CoopClient::CoopClient(client::HeadlessClient& client)
    : client_(client)
{
    // (The handler outlives us on the client: it calls in only while we're here, so neither has to
    // go first.)
    client_.setNoxcppHandler(coopmsg::kOp, [this, alive = std::weak_ptr<int>(alive_)](std::span<const std::uint8_t> m) {
        if (!alive.expired()) {
            onMessage(m);
        }
    });
}

CoopClient::~CoopClient() = default;

void CoopClient::onMessage(std::span<const std::uint8_t> m)
{
    const auto p = coopmsg::parse(m);
    if (!p) {
        return;
    }
    const auto& [sub, rest] = *p;
    switch (sub) {
    case coopmsg::kHello:
        if (auto h = coopmsg::parseHello(rest)) {
            hello_ = *h;
            // A guest brings its own character: its file's server sections to the host, once
            // (unless the campaign has kept ours: a returning player's).
            if (!h->youAreHost && !h->characterKept && !characterSent_) {
                characterSent_ = client_.sendCharacter();
            }
        }
        break;
    case coopmsg::kNotice:
        if (auto n = coopmsg::parseNotice(rest)) {
            notices_.push_back(std::move(*n));
            if (notices_.size() > 64) {
                notices_.erase(notices_.begin());
            }
        }
        break;
    case coopmsg::kRoster:
        if (auto r = coopmsg::parseRoster(rest)) {
            roster_ = std::move(*r);
        }
        break;
    case coopmsg::kCine:
        if (auto c = coopmsg::parseCine(rest)) {
            if (c->call == coopmsg::CineCall::WideScreen) {
                scene_ = c->a != 0;
                actor_ = c->actor;
            }
            cines_.push_back(std::move(*c));
            if (cines_.size() > 256) {
                cines_.erase(cines_.begin());
            }
        }
        break;
    case coopmsg::kChapter:
        if (auto c = coopmsg::parseChapter(rest)) {
            chapter_ = *c;
        }
        break;
    case coopmsg::kReadyState:
        if (auto r = coopmsg::parseReadyState(rest)) {
            ready_ = *r;
        }
        break;
    case coopmsg::kDialog:
        if (auto d = coopmsg::parseDialog(rest)) {
            dialog_ = std::move(*d);
            ++dialogVersion_;
        }
        break;
    case coopmsg::kCreatures:
        if (auto c = coopmsg::parseCreatures(rest)) {
            partyCreatures_ = std::move(*c);
            ++partyCreaturesVersion_;
        }
        break;
    case coopmsg::kSeen:
        // (Parts of one map's add up; another map's starts again: ours moved on.)
        if (auto s = coopmsg::parseSeen(rest)) {
            if (!seen_ || !util::equalsNoCase(seen_->map, s->map)) {
                seen_ = std::move(*s);
            } else {
                seen_->walls.insert(seen_->walls.end(), s->walls.begin(), s->walls.end());
                seen_->obelisks.insert(seen_->obelisks.end(), s->obelisks.begin(), s->obelisks.end());
            }
        }
        break;
    case coopmsg::kCharacter:
        // Our campaign character in parts, in order (reliable): whole once the last is in.
        if (auto c = coopmsg::parseCharacter(rest)) {
            if (c->at == 0) {
                partial_.clear();
            }
            if (c->at == partial_.size()) {
                partial_.insert(partial_.end(), c->bytes.begin(), c->bytes.end());
                if (partial_.size() == c->size) {
                    character_ = std::exchange(partial_, {});
                }
            } else {
                partial_.clear(); // (out of step: wait for the next whole one)
            }
        }
        break;
    default:
        break;
    }
}

void CoopClient::chapterDone()
{
    client_.sendNoxcpp(coopmsg::chapterReadyMessage());
}

void CoopClient::skipBriefing()
{
    client_.sendNoxcpp(coopmsg::hostCmdMessage(coopmsg::HostCmd::SkipBriefing));
}

void CoopClient::reportSeen(std::string_view map, std::span<const std::uint16_t> walls, std::span<const std::uint16_t> obelisks)
{
    if (map.empty()) {
        return;
    }
    for (auto& part : coopmsg::seenMessages(coopmsg::kSeenReport, map, walls, obelisks)) {
        client_.sendNoxcpp(std::move(part));
    }
}

std::optional<coopmsg::SeenCells> CoopClient::takeSeen(std::string_view map)
{
    if (!seen_ || !util::equalsNoCase(seen_->map, map)) {
        return std::nullopt;
    }
    return std::exchange(seen_, std::nullopt);
}

std::filesystem::path campaignCharacterFile(const std::filesystem::path& characterFile)
{
    if (characterFile.empty()) {
        return {};
    }
    std::u16string stem = characterFile.stem().u16string();
    const std::u16string mark = u" (Co-op)";
    if (!stem.ends_with(mark)) {
        stem += mark;
    }
    return characterFile.parent_path() / std::filesystem::path(stem + u".plr");
}

std::optional<client::SpellBars> guestBars(const std::filesystem::path& characterFile, player::CharacterClass characterClass, bool kept)
{
    if (kept) {
        const std::filesystem::path campaign = campaignCharacterFile(characterFile);
        if (!campaign.empty() && campaign != characterFile) {
            if (auto bars = client::SpellBars::read(campaign, characterClass)) {
                return bars;
            }
        }
    }
    bool savedBySolo = false; // (no file at all: a network game's starting set, emptied too)
    try {
        if (!characterFile.empty()) {
            const auto file = player::loadPlayerFile(characterFile);
            const auto* data = file.find(player::SectionId::GameData);
            savedBySolo = data && !data->data.empty();
        }
    } catch (const std::exception&) {
    }
    if (savedBySolo) {
        return std::nullopt;
    }
    return client::SpellBars{};
}

} // namespace nox::coop
