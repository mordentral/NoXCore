// nox.setText, nox.clearText, nox.tellStory and what the server keeps of the overlay (mod_texts.hpp).
#include "mod_texts.hpp"

#include "mod_parts.hpp"
#include "toolbox.hpp"

#include "noxcore/lobby/extended.hpp"
#include "noxcore/modes/script/ui_proto.hpp"
#include "noxcore/server/dialog.hpp"
#include "noxcore/server/game_host.hpp"
#include "noxcore/server/server.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/strings/l10n.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/object_class.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace nox::modes::script {

static_assert(Texts::kMaxKeys == uimsg::kMaxTextKeys, "a client keeps as many keys as a mode may set");

namespace {

std::string folded(std::string_view key)
{
    std::string out(key);
    std::ranges::transform(out, out.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return out;
}

/// The player's unit's extent (0: no such player, or no unit).
std::uint32_t extentOf(const server::GameHost& host, std::uint8_t peer)
{
    const auto it = host.players().find(peer);
    return it != host.players().end() ? it->second.extent : 0;
}

/// A player's message, in order with the frame's other messages to it.
void sendTo(server::GameHost& host, std::uint8_t peer, std::vector<std::uint8_t> bytes)
{
    if (const std::uint32_t e = extentOf(host, peer)) {
        host.server().world().broadcasts().push_back({std::move(bytes), true, e});
    }
}

} // namespace

std::size_t Texts::Text::bytes() const
{
    std::size_t n = all.size();
    for (const auto& [code, t] : byLanguage) {
        n += code.size() + t.size();
    }
    return n;
}

const std::string& Texts::Text::in(std::string_view language) const
{
    if (byLanguage.empty()) {
        return all;
    }
    if (const auto it = byLanguage.find(std::string(language)); it != byLanguage.end()) {
        return it->second;
    }
    if (const auto it = byLanguage.find("en"); it != byLanguage.end()) {
        return it->second;
    }
    return byLanguage.begin()->second;
}

bool Texts::reaches(const server::GameHost& host, std::uint8_t peer)
{
    const auto it = host.players().find(peer);
    return it != host.players().end() && it->second.extent != 0 && !host.refused(peer) &&
           (host.clientFeatures(peer) & lobby::ClientFeature::ModeUi) != 0 && host.clientLevel(peer) >= uimsg::kTextsLevel;
}

std::string Texts::languageOf(const server::GameHost& host, std::uint8_t peer)
{
    const std::string& own = host.clientLanguage(peer);
    return own.empty() ? std::string(l10n::language()) : own;
}

bool Texts::set(server::GameHost* host, const std::string& key, const Text* text)
{
    const std::string k = folded(key);
    const auto it = table_.find(k);
    const std::size_t was = it != table_.end() ? it->second.key.size() + it->second.text.bytes() : 0;
    if (!text) {
        if (it == table_.end()) {
            return true;
        }
        bytes_ -= was;
        table_.erase(it);
        // (A key gone: cleared on its clients as a prefix, so the file's text shows again; the
        // mode's keys that go on from it, cleared with it, sent again.)
        if (host) {
            for (const std::uint8_t peer : synced_) {
                if (reaches(*host, peer)) {
                    sendTo(*host, peer, uimsg::textClearMessage(key));
                }
            }
            // (Those that go on from it are sent again.)
            for (const auto& [k2, e] : table_) {
                if (k2.size() > k.size() && k2.starts_with(k)) {
                    for (const std::uint8_t peer : synced_) {
                        if (reaches(*host, peer)) {
                            sendKey(*host, peer, e);
                        }
                    }
                }
            }
        }
        return true;
    }
    const std::size_t now = key.size() + text->bytes();
    if ((it == table_.end() && table_.size() >= kMaxKeys) || bytes_ - was + now > kMaxBytes) {
        return false;
    }
    if (it != table_.end() && it->second.text == *text && it->second.key == key) {
        return true; // (as it is: nothing to send)
    }
    bytes_ = bytes_ - was + now;
    Entry& e = table_[k];
    e.key = key;
    e.text = *text;
    if (host) {
        for (const std::uint8_t peer : synced_) {
            if (reaches(*host, peer)) {
                sendKey(*host, peer, e);
            }
        }
        syncAll(*host); // (a client in the game that hasn't the set: all of it, this one with it)
    }
    return true;
}

void Texts::clear(server::GameHost* host, std::string_view prefix)
{
    const std::string p = folded(prefix);
    bool any = false;
    for (auto it = table_.begin(); it != table_.end();) {
        if (it->first.starts_with(p)) {
            bytes_ -= it->second.key.size() + it->second.text.bytes();
            it = table_.erase(it);
            any = true;
        } else {
            ++it;
        }
    }
    if (any && host) {
        for (const std::uint8_t peer : synced_) {
            if (reaches(*host, peer)) {
                sendTo(*host, peer, uimsg::textClearMessage(prefix));
            }
        }
    }
}

bool Texts::has(std::string_view key) const
{
    return table_.contains(folded(key));
}

void Texts::ready(server::GameHost& host, std::uint8_t peer)
{
    if (!table_.empty() && !synced_.contains(peer) && reaches(host, peer)) {
        sendWhole(host, peer);
    }
}

bool Texts::shows(server::GameHost& host, std::uint8_t peer)
{
    if (!reaches(host, peer)) {
        return false;
    }
    if (!synced_.contains(peer)) {
        sendWhole(host, peer);
    }
    return true;
}

void Texts::reset(server::GameHost* host)
{
    if (host) {
        for (const std::uint8_t peer : synced_) {
            if (reaches(*host, peer)) {
                host->sendToIdentified(peer, uimsg::textClearMessage("")); // (at once: the game may not run another frame)
            }
        }
    }
    *this = Texts{};
}

void Texts::syncAll(server::GameHost& host)
{
    for (const auto& [peer, p] : host.players()) {
        if (!synced_.contains(peer) && reaches(host, peer)) {
            sendWhole(host, peer);
        }
    }
}

void Texts::sendWhole(server::GameHost& host, std::uint8_t peer)
{
    synced_.insert(peer);
    for (const auto& [k, e] : table_) {
        sendKey(host, peer, e);
    }
}

void Texts::sendKey(server::GameHost& host, std::uint8_t peer, const Entry& e)
{
    for (auto& m : uimsg::textSetMessages(e.key, e.text.in(languageOf(host, peer)))) {
        sendTo(host, peer, std::move(m));
    }
}

namespace {

/// A key's text of nox.setText's argument at `idx`: a string, or a table of language code ->
/// string; an argument error for anything else.
Texts::Text textArg(lua_State* L, int idx)
{
    Texts::Text t;
    if (lua_type(L, idx) == LUA_TSTRING) {
        size_t n = 0;
        const char* s = lua_tolstring(L, idx, &n);
        luaL_argcheck(L, n <= uimsg::kMaxTextValueBytes, idx, "a text of 4096 bytes at most");
        t.all.assign(s, n);
        return t;
    }
    luaL_argcheck(L, lua_istable(L, idx), idx, "a text, or a table of texts by language ({ en = ..., ru = ... })");
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TSTRING) {
            luaL_argerror(L, idx, "a table of texts by language code ({ en = ..., ru = ... })");
        }
        const std::string code = lobby::languageCode(lua_tostring(L, -2));
        if (code.empty() || code != lua_tostring(L, -2)) {
            luaL_argerror(L, idx, "its keys are language codes in small letters (en, ru, ko...)");
        }
        size_t n = 0;
        const char* s = lua_tolstring(L, -1, &n);
        if (n > uimsg::kMaxTextValueBytes) {
            luaL_argerror(L, idx, "a text of 4096 bytes at most");
        }
        t.byLanguage[code].assign(s, n);
        lua_pop(L, 1);
    }
    luaL_argcheck(L, !t.byLanguage.empty(), idx, "a table of at least one language's text");
    return t;
}

/// A key at `idx`: 1 to `most` bytes of printable ASCII.
std::string keyArg(lua_State* L, int idx, std::size_t most, const char* what)
{
    size_t n = 0;
    const char* s = luaL_checklstring(L, idx, &n);
    bool ok = n >= 1 && n <= most;
    for (size_t i = 0; ok && i < n; ++i) {
        ok = s[i] > 0x20 && s[i] < 0x7F;
    }
    luaL_argcheck(L, ok, idx, what);
    return std::string(s, n);
}

int noxSetText(lua_State* L)
{
    // nox.setText(key, text | { [language] = text } | nil): whether it's kept (false: the set is
    // full); bad arguments an error.
    const std::string key = keyArg(L, 1, uimsg::kMaxTextKeyBytes, "a key of 1-64 letters, digits and signs (no spaces)");
    std::optional<Texts::Text> text;
    if (!lua_isnoneornil(L, 2)) {
        text = textArg(L, 2);
    }
    lua_pushboolean(L, toolbox::partsOf(L).texts.set(toolbox::hostOf(L), key, text ? &*text : nullptr));
    return 1;
}

int noxClearText(lua_State* L)
{
    // nox.clearText(prefix): every key that starts with it gone ("" or nil: every one).
    std::string prefix;
    if (!lua_isnoneornil(L, 1)) {
        size_t n = 0;
        const char* s = luaL_checklstring(L, 1, &n);
        prefix.assign(s, std::min<size_t>(n, uimsg::kMaxTextKeyBytes));
    }
    toolbox::partsOf(L).texts.clear(toolbox::hostOf(L), prefix);
    return 0;
}

int noxTellStory(lua_State* L)
{
    // nox.tellStory(unit, npc, key, speaker?): TellStory's dialog from the npc to that player with
    // the text of `key` (nox.setText's or nox.csf's), the speaker's name `speaker` (nil: the NPC's
    // own, as TellStory gives it); whether it was told.
    toolbox::refuseInsideAsked(L, "nox.tellStory");
    server::ServerObject& unit = toolbox::playerUnitArg(L, 1);
    server::ServerObject& npc = toolbox::objectArg(L, 2);
    // (MSG_DIALOG's text id: 31 letters and its end before the name over it.)
    const std::string key = keyArg(L, 3, 31, "a key of 1-31 letters, digits and signs (no spaces)");
    std::optional<std::u16string> speaker;
    if (!lua_isnoneornil(L, 4)) {
        size_t n = 0;
        const char* s = luaL_checklstring(L, 4, &n);
        speaker = strings::fromUtf8Cut(std::string_view(s, n), 31);
    }
    server::GameHost* host = toolbox::hostOf(L);
    if (!host || !(npc.classes & things::ObjectClass::Monster) || unit.gone() || npc.gone()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    std::optional<std::uint8_t> peer;
    for (const auto& [p, hp] : host->players()) {
        if (hp.extent == unit.extent) {
            peer = p;
        }
    }
    Texts& texts = toolbox::partsOf(L).texts;
    // A key of the mode's own: only to a client that shows the overlay (its set sent first).
    if (!peer || (texts.has(key) && !texts.shows(*host, *peer))) {
        lua_pushboolean(L, 0);
        return 1;
    }
    server::Fight fight = toolbox::fightOf(L);
    server::openNpcDialog(fight, &unit, &npc, 0, key, speaker ? &*speaker : nullptr);
    lua_pushboolean(L, 1);
    return 1;
}

} // namespace

void addTextsToolbox(lua_State* L)
{
    static constexpr std::pair<const char*, lua_CFunction> kFns[] = {
        {"setText", noxSetText}, {"clearText", noxClearText}, {"tellStory", noxTellStory}};
    for (const auto& [name, fn] : kFns) {
        lua_pushcfunction(L, fn, name);
        lua_setfield(L, -2, name);
    }
}

} // namespace nox::modes::script
