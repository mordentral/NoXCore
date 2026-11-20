// nox.setText, nox.clearText and nox.tellStory: a script mode's text overlay. The mode
// gives texts by string-table key ("Journal:FindSponsor", "thing.db:SoupPrettyName", a key of its
// own), each one text or one per language; each NoX Core client of client level 14 or above that
// named ModeUi (reaches) is sent each key's text in its own language (ui_proto.hpp's kTextSet and
// kTextClear), and its string lookups find them before its nox.csf's: its journal, story dialogs,
// item names and descriptions, ability titles and tooltips show them as they show the game's own.
// A client gets the whole set at its first ready (or the first change after it's in the game), then
// each change, in order with the game's messages (the world's broadcasts), so a message after a
// change that names its key finds it. Stock clients and older NoX Core ones are sent nothing and
// keep their own nox.csf's texts. A mode that never calls nox.setText keeps and sends nothing;
// nothing runs per frame. nox.tellStory opens a story dialog as TellStory does, with the mode's own
// speaker. A story dialog's speaker name and a shop window's title, which the server looks up
// itself ("NPC:<name>") and sends as text, are the mode's key's text in the player's language first
// (ModeHooks::playerText), for every client. (Internal to the script modes' sources.)
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

struct lua_State;

namespace nox::server {
class GameHost;
}

namespace nox::modes::script {

class Texts {
public:
    /// The most keys a mode keeps, and their texts' bytes together (each language's).
    static constexpr std::size_t kMaxKeys = 512;
    static constexpr std::size_t kMaxBytes = 64 * 1024;

    /// A key's text: `all` for every language, else one per language code with English (or the
    /// first code) for a language it hasn't; with `othersOwn` (nox.setText's `others = "own"`), none
    /// for a language it hasn't: that player keeps its own (its client's nox.csf, the server's string
    /// table), as if the mode had no such key.
    struct Text {
        std::string all;
        std::map<std::string, std::string> byLanguage;
        bool othersOwn = false;
        bool operator==(const Text&) const = default;
        std::size_t bytes() const;
        /// The text in `language`: its own, else English's, else the first code's; null (with
        /// othersOwn) when `language` has none.
        const std::string* in(std::string_view language) const;
    };

    /// Whether `peer` may be sent the overlay: a player in the game with its unit, not a held-out
    /// stock one, whose client named ClientFeature::ModeUi and is at client level 14 or above.
    static bool reaches(const server::GameHost& host, std::uint8_t peer);
    /// A player's language: its client's (client level 14), else the host's own (the program's
    /// language: "en" for a server alone).
    static std::string languageOf(const server::GameHost& host, std::uint8_t peer);

    /// nox.setText's: `key`'s text (none: the key gone) for every client it reaches, now or
    /// later. False (nothing changed) when it would pass kMaxKeys or kMaxBytes.
    bool set(server::GameHost* host, const std::string& key, const Text* text);
    /// nox.clearText's: the keys starting with `prefix` (without case) gone; "" every one.
    void clear(server::GameHost* host, std::string_view prefix);
    /// Whether `key` (any case) is one of the mode's.
    bool has(std::string_view key) const;
    /// `key`'s text (any case), null when it isn't the mode's.
    const Text* find(std::string_view key) const;
    /// The server's own lookup of `key` for the player `peer` (none: one without a client, the host's
    /// language): the mode's text in that player's language (as its client would get it), none when
    /// the key isn't the mode's.
    /// (ModeHooks::playerText: a story dialog's speaker name, a shop window's title.)
    std::optional<std::u16string> textFor(const server::GameHost& host, std::optional<std::uint8_t> peer, std::string_view key) const;
    /// A player's map loaded (CLIENT_READY): a client the overlay reaches and hasn't had it gets
    /// the whole set.
    void ready(server::GameHost& host, std::uint8_t peer);
    /// Whether `peer`'s client has the set now (reaches, and sent it).
    bool shows(server::GameHost& host, std::uint8_t peer);
    /// A player gone.
    void left(std::uint8_t peer) { synced_.erase(peer); }
    /// The mode unloaded: every key gone from each client that had any, everything forgotten.
    void reset(server::GameHost* host);
    std::size_t size() const noexcept { return table_.size(); }

private:
    struct Entry {
        std::string key; ///< as the mode gave it (the map's own is folded to small letters)
        Text text;
    };
    /// Each reached client that hasn't the set sent it now (whole).
    void syncAll(server::GameHost& host);
    void sendWhole(server::GameHost& host, std::uint8_t peer);
    /// `e` to the player in its language; one it has no text in (othersOwn) isn't sent, and when
    /// `was` (the key's text before) gave it one, the key is cleared on it (the keys that go on from
    /// it sent again).
    void sendKey(server::GameHost& host, std::uint8_t peer, const Entry& e, const Text* was = nullptr);

    std::map<std::string, Entry> table_; ///< by key folded to small letters
    std::set<std::uint8_t> synced_;      ///< the players whose clients have the set
    std::size_t bytes_ = 0;
};

/// nox.setText, nox.clearText and nox.tellStory set in the `nox` table on top of L's stack.
void addTextsToolbox(lua_State* L);

} // namespace nox::modes::script
