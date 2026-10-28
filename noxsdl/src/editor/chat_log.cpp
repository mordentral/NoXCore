// The Chat panel's model (chat_log.hpp).
#include "editor/chat_log.hpp"

#include "noxcore/client/headless_client.hpp"

#include <algorithm>

namespace noxsdl::editor {

void ChatLog::add(ChatLine line)
{
    line.seq = next_++;
    lines_.push_back(std::move(line));
    while (lines_.size() > capacity_) {
        lines_.pop_front();
    }
    if (!viewing_) {
        unread_ = std::min(unread_ + 1, capacity_);
    }
}

void ChatLog::setViewing(bool viewing)
{
    viewing_ = viewing;
    if (viewing) {
        unread_ = 0;
    }
}

void ChatLog::clear()
{
    lines_.clear();
    unread_ = 0;
}

std::optional<ChatLine> chatLineFrom(const nox::client::ClientEvent& e)
{
    using K = nox::client::ClientEvent::Kind;
    ChatLine l;
    l.netCode = e.netCode;
    switch (e.kind) {
    case K::Chat:
        // (CliNet_OnPacket 0xA8: speaker 0 or a notice is the server's own line.)
        if (e.netCode == 0 || (e.textFlags & 0x10)) {
            l.kind = ChatLine::Kind::System;
        } else {
            l.kind = e.team ? ChatLine::Kind::Team : ChatLine::Kind::Chat;
            l.speaker = e.name;
        }
        l.text = e.text;
        return l;
    case K::PlayerJoined:
        l.kind = ChatLine::Kind::System;
        l.speaker = e.name;
        l.text = e.name + " joined the game";
        return l;
    case K::PlayerLeft:
        l.kind = ChatLine::Kind::System;
        l.speaker = e.name;
        l.text = e.name.empty() ? std::string("Someone left the game") : e.name + " left the game";
        return l;
    case K::Disconnected:
        l.kind = ChatLine::Kind::System;
        l.text = e.text.empty() ? std::string("Disconnected") : "Disconnected: " + e.text;
        return l;
    default:
        return std::nullopt;
    }
}

} // namespace noxsdl::editor
