// The editor's Chat panel's model: the session's in-game chat as
// the editor's own game connection hears it (MSG_TEXT_MESSAGE relayed by the host to every player,
// team lines only to the team; players coming and going; the server's own lines), kept to the
// last `capacity` lines in order, with an unread count for the panel's tab while it isn't seen.
// No SDL or ImGui, so the tests check it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace nox::client {
struct ClientEvent;
}

namespace noxsdl::editor {

struct ChatLine {
    enum class Kind : std::uint8_t {
        Chat,   ///< a player's line to everyone
        Team,   ///< a player's line to their team (the game shows "<team>: text")
        System, ///< the server's own line, a player joining or leaving, the game ending for us
    };
    Kind kind = Kind::Chat;
    std::string speaker;        ///< the player's name (empty for most system lines)
    std::string text;
    std::uint16_t netCode = 0;  ///< the speaker's (0: the server)
    std::uint64_t seq = 0;      ///< its number in the log (1, 2, ...), given by ChatLog::add
    std::uint32_t timeMs = 0;   ///< when it came (the caller's clock)
};

class ChatLog {
public:
    explicit ChatLog(std::size_t capacity = 500) : capacity_(capacity == 0 ? 1 : capacity) {}

    /// A line at the end (the oldest let go past the capacity). Unread unless the panel is being
    /// viewed (setViewing).
    void add(ChatLine line);
    /// Whether the panel is seen now (its tab in front, not collapsed): while it is, new lines are
    /// read as they come; turning it on reads them all.
    void setViewing(bool viewing);
    bool viewing() const { return viewing_; }
    /// Every line read (the panel looked at).
    void markViewed() { unread_ = 0; }
    /// Lines that came since the panel was last seen (at most the capacity).
    std::size_t unread() const { return unread_; }
    const std::deque<ChatLine>& lines() const { return lines_; }
    /// Lines ever added (the panel scrolls to the end when it grows).
    std::uint64_t total() const { return next_ - 1; }
    std::size_t capacity() const { return capacity_; }
    void clear();

private:
    std::size_t capacity_;
    std::deque<ChatLine> lines_;
    std::size_t unread_ = 0;
    std::uint64_t next_ = 1;
    bool viewing_ = false;
};

/// A game event as the Chat panel shows it: a chat line (Team for team chat, System for the
/// server's own: speaker 0, a notice), a player joined or left, the game ending (Disconnected);
/// nothing for anything else. Text as the client has it (a string id still "[id]": the caller
/// resolves it with the game's strings).
std::optional<ChatLine> chatLineFrom(const nox::client::ClientEvent& e);

} // namespace noxsdl::editor
