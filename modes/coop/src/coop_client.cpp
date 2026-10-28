// The co-op campaign's client side (coop_client.hpp).
#include "noxcore/coop/coop_client.hpp"

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

} // namespace nox::coop
