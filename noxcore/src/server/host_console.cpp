// The host's console on the server (GameHost::command), MSG_SERVER_CMD and MSG_SYSOP_PW, and
// noxcpp's own test commands. GAME.EXE's console table holds commands (as
// entries: the word, its help, flags 1 host / 2 client / 0x10 cheat, the sub-table, the
// function); the host runs its line through server_parseCmdText, and a sysop's
// through SrvNet_OnServerCmd's case 2. What it prints goes to the host's console
// (Gui_Console_Printf): here the lines GameHost::command returns, or takeConsole's.
#include "noxcore/server/game_host.hpp"

#include "noxcore/io/byte_writer.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/map/map_file.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/net/framing.hpp"
#include "noxcore/server/abilities.hpp"
#include "noxcore/server/collide.hpp"
#include "noxcore/server/combat.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/host_client.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/player_save.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/script_host.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/solo_save.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/spell_ids.hpp"
#include "noxcore/util/text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;

/// The words of a line: split at spaces, a "quoted run" kept whole (server_parseCmdText).
std::vector<std::string> wordsOf(std::string_view line)
{
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') {
            ++i;
        }
        if (i >= line.size()) {
            break;
        }
        if (line[i] == '"') {
            const std::size_t end = line.find('"', i + 1);
            out.emplace_back(line.substr(i + 1, end == std::string_view::npos ? std::string_view::npos : end - i - 1));
            i = end == std::string_view::npos ? line.size() : end + 1;
        } else {
            const std::size_t end = line.find(' ', i);
            out.emplace_back(line.substr(i, end == std::string_view::npos ? std::string_view::npos : end - i));
            i = end == std::string_view::npos ? line.size() : end;
        }
    }
    return out;
}

/// _wcsicmp's equality.
using util::equalsNoCase;

/// wcstol(s, 0, 10), 0 for none.
int number(std::string_view s)
{
    int v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

// The words from index `from` on, joined by single spaces.
std::string joinWords(const std::vector<std::string>& w, std::size_t from)
{
    std::string s;
    for (std::size_t i = from; i < w.size(); ++i) {
        s += (s.empty() ? "" : " ") + w[i];
    }
    return s;
}

std::u16string wide(std::string_view s) { return strings::fromUtf8(s); }

using net::bytesOf;

// UTF-16LE bytes up to the first NUL.
using util::utf16Until;

/// A broadcast's chime (SrvNet_OnServerCmd 5): 0x386 at each player's unit, to everyone.
void broadcastSound(World& world)
{
    std::vector<const ServerObject*> units;
    for (const auto& [extent, o] : world.objects()) {
        if (o.player && !o.gone()) {
            units.push_back(&o);
        }
    }
    for (const ServerObject* u : units) {
        playSound(world, 0x386, *u);
    }
}

} // namespace

// A line of console output; with test commands on, also sent in chat to the sysop whose command
// printed it.
void GameHost::printText(std::string text)
{
    console_.push_back(text);
    if (echoTo_ != 0 && options_.testCommands) { // (noxcpp: a sysop sees its command's lines)
        if (const ServerObject* u = server_.world().byExtent(echoTo_)) {
            server_.world().broadcasts().push_back({World::textMessage(wide(text), 0), true, u->extent});
        }
    }
}

std::string GameHost::stringLine(const std::string& id, std::initializer_list<std::string_view> values)
{
    std::string s = strings::toUtf8(server_.world().text(id));
    auto value = values.begin();
    for (std::size_t at = s.find('%'); at != std::string::npos && at + 1 < s.size(); at = s.find('%', at)) {
        const char kind = s[at + 1];
        if ((kind == 's' || kind == 'S' || kind == 'd') && value != values.end()) {
            s.replace(at, 2, *value);
            at += value->size();
            ++value;
        } else {
            at += 2;
        }
    }
    return s;
}

void GameHost::print(std::string_view label, std::initializer_list<std::string_view> values)
{
    // Str_GetString(label) from ParseCmd.c's lines, its %s / %S / %d filled in order.
    std::string id(label);
    if (id.find(':') == std::string::npos) {
        id = "ParseCmd.c:" + id;
    }
    printText(stringLine(id, values));
}

HostedPlayer* GameHost::playerNamed(std::string_view name)
{
    // playerByName: by name, any case.
    for (auto& [peer, p] : players_) {
        if (equalsNoCase(p.name, name)) {
            return &p;
        }
    }
    return nullptr;
}

void GameHost::disconnect(std::uint8_t peer, std::uint8_t reason)
{
    // Plr_DisconnectByIndex: a kick (reason 4) first tells the client (Plr_OnDisconnect's
    // MSG_KICK_NOTIFICATION 0xC5, sent at once); its stream closed, the player out.
    if (reason == 4) {
        transport_.sendReliableNow(peer, std::vector<std::uint8_t>{0xC5}, nowMs_);
    }
    transport_.close(peer);
    leave(peer);
}

net::msg::GameSettings2 GameHost::gameSettings2() const
{
    // MSG_GAME_SETTINGS_2: the name, the spells allowed (bit n spell n), the weapons' and
    // armour's (the rules'), the time left.
    const World& world = const_cast<Server&>(server_).world();
    net::msg::GameSettings2 s;
    for (std::size_t i = 0; i < s.gameName.size() - 1 && i < options_.serverName.size(); ++i) {
        s.gameName[i] = static_cast<std::uint8_t>(options_.serverName[i]);
    }
    s.allowedSpells.fill(0xFF);
    for (std::size_t spell = 0; spell < s.allowedSpells.size() * 8; ++spell) {
        if (world.spellDisabled[spell & 0xFF]) {
            s.allowedSpells[spell / 8] &= static_cast<std::uint8_t>(~(1u << (spell % 8)));
        }
    }
    s.gameData44 = rules_.weapons;
    s.gameData48 = rules_.armor;
    return s;
}

void GameHost::broadcastSettings()
{
    // (Two Net_QueueImportantSeq(0x9F, ..): held for a player in the game but not ready, none for
    // one not in yet; here every player's, the host's own client too, a deliberate deviation.)
    sendSettings(true);
}

void GameHost::sendSettings(bool withSecond)
{
    const std::uint32_t frame = server_.clock().frame();
    const auto build = [&](std::optional<std::uint8_t> peer) {
        net::msg::GameSettings s = gameSettings(frame);
        if (peer) {
            s.gameFlags = clientFlags(*peer) & 0x7FFF0u;
        }
        std::vector<std::uint8_t> m = bytesOf(s);
        if (withSecond) {
            const auto two = bytesOf(gameSettings2());
            m.insert(m.end(), two.begin(), two.end());
        }
        return m;
    };
    if (!options_.clientGameFlags) {
        sendImportantToAll(build(std::nullopt));
        return;
    }
    // (noxcpp: a mode tells some clients other flags: each its own, to those in the game.)
    std::vector<std::uint8_t> peers;
    for (const auto& [peer, p] : players_) {
        peers.push_back(peer);
    }
    for (const std::uint8_t peer : peers) {
        sendImportant(peer, build(peer), true);
    }
}

std::uint32_t GameHost::clientFlags(std::uint8_t peer)
{
    const auto it = players_.find(peer);
    return it == players_.end() ? server_.world().gameFlags : clientFlags(peer, it->second);
}

std::uint32_t GameHost::clientFlags(std::uint8_t peer, const HostedPlayer& p)
{
    const std::uint32_t flags = server_.world().gameFlags;
    return options_.clientGameFlags ? options_.clientGameFlags(*this, peer, p, flags) : flags;
}

std::vector<std::string> GameHost::command(std::string_view line, std::uint32_t issuer)
{
    // (What the server printed by itself before it stays for takeConsole.)
    auto earlier = std::exchange(console_, {});
    runConsole(line, issuer);
    return std::exchange(console_, std::move(earlier));
}

void GameHost::onSysopPassword(std::uint8_t peer, const std::u16string& password)
{
    // SrvNet_OnPacket 0xBC: a set password matching (any case) makes it a sysop
    // ("sysopAccessGranted"); MSG_SYSOP_RESULT 0xBD {ok} back.
    const auto lower = [](std::u16string s) {
        for (char16_t& c : s) {
            if (c < 0x80) {
                c = static_cast<char16_t>(std::tolower(static_cast<int>(c)));
            }
        }
        return s;
    };
    const bool ok = !options_.sysopPassword.empty() && lower(password) == lower(options_.sysopPassword);
    if (ok && !sysops_.contains(peer)) {
        sysops_.insert(peer);
        print("Network.c:sysopAccessGranted");
    }
    transport_.sendReliable(peer, std::vector<std::uint8_t>{0xBD, static_cast<std::uint8_t>(ok ? 1 : 0)});
}

void GameHost::onServerCmd(std::uint8_t peer, std::uint8_t cmd, const std::u16string& text)
{
    World& world = server_.world();
    const auto it = players_.find(peer);
    if (it == players_.end()) {
        return;
    }
    ServerObject* unit = world.byExtent(it->second.extent);
    if (!unit || !unit->player) {
        return;
    }
    PlayerControl& p = *unit->player;
    // (Game flags 0xC000, a map switching: only 0, 4 and 5.)
    const bool sysop = sysops_.contains(peer);
    switch (cmd) {
    case 0: { // Plr_GoObserver: not over, not a quest, not observing already
        if ((world.gameFlags & GameFlag::GameOver) || world.quest() || (p.statusFlags & 1u)) {
            break;
        }
        // (0xF00D 0xF0AD: the client's own idle observer request, without its timestamp asked.)
        const bool idle = text.size() >= 2 && text[0] == 0xF00D && text[1] == 0xF0AD;
        Fight f = server_.fight();
        if (goObserver(f, *unit, idle, false)) {
            if (anyonePlaying(world)) {
                if (!idle) {
                    f.setStatus(*unit, 0x100, true);
                }
                observerLeftPlay(f);
            }
            removeTeamMember(world, *unit);
            print("observermode");
        }
        break;
    }
    case 1: { // a script function by name (a sysop's): run with it as caller and trigger
        if (!sysop && !(world.gameFlags & GameFlag::Solo)) {
            break;
        }
        const std::string name = strings::toUtf8(text.substr(0, text.find(u' ')));
        const auto f = world.script ? world.script->function(name) : std::nullopt;
        if (!f) {
            print("InvalidFunction");
            break;
        }
        print("ExecutingFunction");
        world.script->call(*f, unit->extent, unit->extent);
        break;
    }
    case 2: // a console command (a sysop's), run as the host's
        if (!sysop && !(world.gameFlags & GameFlag::Solo)) {
            break;
        }
        print("RemoteSysop");
        echoTo_ = unit->extent;
        runConsole(strings::toUtf8(text), unit->extent);
        echoTo_ = 0;
        break;
    case 3: // print to all
        world.textToAll(text, 0);
        break;
    case 4: { // an observer watches a player (none: its camera free)
        if (!(p.statusFlags & 1u)) {
            print("notinobserver");
            break;
        }
        if (text.empty()) {
            p.cameraFollow = 0; // (playerCameraUnlock)
            break;
        }
        for (auto& [other, q] : players_) {
            if (equalsNoCase(q.name, strings::toUtf8(text))) {
                p.cameraFollow = p.cameraFollow == q.extent ? 0 : q.extent; // (playerCameraFollow)
            }
        }
        break;
    }
    case 5: // broadcast: flag 0x10, 0x386 at each player
        world.textToAll(text, 0x10);
        broadcastSound(world);
        break;
    default:
        print("invalidattempt");
        break;
    }
}

// One console line, run as the host: matched by its first word (and the second for list, show and
// set), each command's reply printed through print().
void GameHost::runConsole(std::string_view line, std::uint32_t issuer)
{
    World& world = server_.world();
    const std::vector<std::string> w = wordsOf(line);
    if (w.empty()) {
        return;
    }
    const auto is = [&](std::size_t i, std::string_view word) { return i < w.size() && equalsNoCase(w[i], word); };
    const bool network = (world.gameFlags & GameFlag::Network) != 0;
    const auto onOff = [&](std::size_t i) -> int { return is(i, "on") ? 1 : is(i, "off") ? 0 : -1; };
    const auto settingsChanged = [&] { settingsChanged_ = true; };
    const auto setOption = [&](std::uint32_t bit, bool on) {
        // the server options' bits.
        const std::uint32_t before = options_.serverOptions;
        options_.serverOptions = on ? (options_.serverOptions | bit) : (options_.serverOptions & ~bit);
        world.serverOptions = options_.serverOptions;
        if (before != options_.serverOptions) {
            settingsChanged();
            if (on && (bit & 0x2000u)) {
                camperResetAll(world, server_.clock().frame()); // (the camper alarm on)
            }
        }
    };
    const auto everyPlayer = [&](auto&& fn) {
        for (auto& [extent, o] : world.objects()) {
            if (o.player && !o.gone()) {
                fn(o);
            }
        }
    };
    // A quest's kick: its save to its client and out once the client has it.
    const auto questKick = [&](const HostedPlayer& p) {
        if (ServerObject* unit = server_.world().byExtent(p.extent); unit && unit->player) {
            Fight f = server_.fight();
            questSaveAndKick(f, *unit);
            flushDrops();
        }
    };

#ifndef NOX_NO_TEST_COMMANDS
    if (is(0, "nx")) {
        if (!options_.testCommands) {
            printText("nx: test commands are off");
            return;
        }
        testCommand(w, issuer);
        return;
    }
#endif
    if (is(0, "help") || is(0, "?")) {
#ifndef NOX_NO_TEST_COMMANDS
        printText("host commands: allow ban broadcast cheat exec kick list load set show unset" +
                  std::string(options_.testCommands ? " nx (noxcpp tests: nx help)" : ""));
#else
        // (A release build has no test commands: "nx" is an unknown command there.)
        printText("host commands: allow ban broadcast cheat exec kick list load set show unset");
#endif
        return;
    }
    if (is(0, "kick") && w.size() == 2) {
        // Not the host's own (index 0x1F; a dedicated server has none); in a quest
        // the save-and-kick (with the quests); else Plr_DisconnectByIndex(4), "kicked".
        HostedPlayer* p = playerNamed(w[1]);
        if (!p) {
            return;
        }
        if (p->extent == issuer && issuer != 0) {
            print("cantkickyourself");
            return;
        }
        if (server_.world().quest()) {
            // its save to its client and out once the client has it; else out
            // (nothing printed).
            questKick(*p);
            return;
        }
        const std::string name = p->name;
        disconnect(p->peer, 4); // (Plr_DisconnectByIndex(index, 4) itself)
        print("kicked", {name});
        return;
    }
    if (is(0, "ban") && w.size() == 2) {
        // Present, disconnected (playerDisconnByPlrID; in a quest the save-and-kick)
        // and banned by name ("banned"); absent, its name banned ("banDisallow").
        // (The ban for good.)
        HostedPlayer* p = playerNamed(w[1]);
        if (!p) {
            bans_[wide(w[1])] = 0xFFFFFFFFu;
            syncAccess();
            print("banDisallow", {w[1]});
            return;
        }
        if (p->extent == issuer && issuer != 0) {
            print("cantbanyourself");
            return;
        }
        const std::string name = p->name;
        bans_[utf16Until(p->accept.playerName)] = 0xFFFFFFFFu;
        syncAccess();
        if (server_.world().quest()) {
            // (In a quest as the kick: its save to its client, out once the client
            // has it; still banned and "banned".)
            questKick(*p);
        } else {
            disconnect(p->peer, 4); // (playerDisconnByPlrID)
        }
        print("banned", {name});
        return;
    }
    if (is(0, "broadcast") && 1 < w.size()) { // MSG_SERVER_CMD 5
        world.textToAll(wide(joinWords(w, 1)), 0x10);
        broadcastSound(world);
        return;
    }
    if (is(0, "exec") && w.size() == 2) {
        // MSG_SERVER_CMD 1: the script function, the host's player its caller.
        const auto f = world.script ? world.script->function(w[1]) : std::nullopt;
        if (!f) {
            print("InvalidFunction");
            return;
        }
        print("ExecutingFunction");
        world.script->call(*f, issuer, issuer);
        return;
    }
    if (is(0, "execrul") && w.size() == 2) {
        // The file (".rul" added to a name without a '.'), parsecmd.c:ExecutingRul, then
        // each line of it from the game's folder (up to 255 characters, its '\n'
        // cut) that isn't empty echoed and run as the host's (server_parseCmdText).
        std::string name = w[1];
        if (name.find('.') == std::string::npos) {
            name += ".rul";
        }
        print("ExecutingRul", {name});
        const std::filesystem::path dir = !options_.gameDir.empty()   ? options_.gameDir
                                          : !options_.mapsDir.empty() ? options_.mapsDir.parent_path()
                                                                      : mapPath_.parent_path().parent_path().parent_path();
        std::ifstream in(resolveNoCase(dir / name)); // (in any case, as on Windows: OTQuest.rul)
        for (std::string l; in && std::getline(in, l);) {
            if (l.size() > 255) {
                l.resize(255);
            }
            while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) {
                l.pop_back(); // (the '\r' left by the exe's text read is a space to its words)
            }
            if (!l.empty()) {
                printText(l);
                runConsole(l, issuer);
            }
        }
        return;
    }
    if (is(0, "startSoloQuest")) {
        // The command (OTQuest.rul's line): a quest chosen (isQuest) not yet begun
        // (game flag 0x1000 off): the host's screen to black at once
        // (cliPlayMapIntro(1)) and 30 frames at the
        // tick's end then begins it and loads its first map (Server::tickEnd). (Its client's
        // GUI flag and the settings' 0x10 for those frames, which refuses
        // joins, aren't kept.)
        if (world.questChosen && !(world.gameFlags & GameFlag::Quest)) {
            if (world.hostClient) {
                world.hostClient->playMapIntro(true);
            }
            world.soloQuestCountdown = 30;
        }
        return;
    }
    if (is(0, "load") && w.size() == 2) {
        // not in a quest; a map of this mode (a network game's, not a solo map's;
        // "NoMapLoadNewMode" for another mode's, but in a chat game); then it's loaded
        // ("maploaded").
        if (world.quest()) {
            return;
        }
        namespace fs = std::filesystem;
        std::string name = w[1];
        if (name.size() > 4 && equalsNoCase(std::string_view(name).substr(name.size() - 4), ".map")) {
            name.resize(name.size() - 4);
        }
        const fs::path mapsDir = !options_.mapsDir.empty() ? options_.mapsDir : mapPath_.parent_path().parent_path();
        const fs::path path = mapsDir / name / (name + ".map");
        std::uint32_t types = 0;
        try {
            if (!fs::exists(resolveNoCase(path))) {
                print("CannotAccessMap", {name});
                return;
            }
            const auto file = map::loadMapFile(path);
            types = file.find(map::MapInfo::kName) ? map::decodeSection<map::MapInfo>(file).gameFlags : 0;
        } catch (const std::exception&) {
            print("CannotAccessMap", {name});
            return;
        }
        if (types == 0 || (types & 0x200u)) {
            return;
        }
        // Out of chat, the map must allow the mode being played (0x17F0: the game flags' mode bits).
        if (!(world.gameFlags & GameFlag::Chat) && (mapToGameFlags(types) & world.gameFlags & 0x17F0u) == 0) {
            print("NoMapLoadNewMode");
            return;
        }
        nextMap(name);
        print("maploaded", {name});
        return;
    }
    if (is(0, "list") && is(1, "users")) {
        print("userslist");
        for (const auto& [peer, p] : players_) {
            printText(p.name);
        }
        return;
    }
    if (is(0, "show") && is(1, "game")) { // (in short)
        printText(options_.serverName + "  " + mapFile_ + "  " + std::to_string(players_.size()) + "/" + std::to_string(options_.maxPlayers) +
                  " players");
        return;
    }
    if (is(0, "show") && is(1, "ai")) { // engine flag 8 (the AI's debug lines)
        aiDebug_ = !aiDebug_;
        return;
    }
    if (is(0, "allow")) {
        print("notyetimplemented");
        return;
    }
    // set / unset: the game's switches and limits, and the spells and things allowed.
    if (is(0, "set") || is(0, "unset")) {
        const bool set = is(0, "set");
        if (!set) {
            if (is(1, "god")) { // set_god(0), a solo game's
                Fight f = server_.fight();
                setGod(f, false);
                print("godunset");
            } else if (is(1, "sage")) { // (does nothing)
                print("sageunset");
            }
            return;
        }
        if (is(1, "god")) { // not a quest; a solo game's god mode (engine 0x30)
            if (!world.quest()) {
                Fight f = server_.fight();
                setGod(f, true);
                print("godset");
            }
            return;
        }
        if (is(1, "sage")) {
            if (!world.quest()) {
                print("sageset");
            }
            return;
        }
        if (is(1, "sysop") && w.size() == 3) {
            options_.sysopPassword = wide(w[2]);
            print("sysoppasswordset");
            return;
        }
        if (is(1, "name") && 2 < w.size()) { // the words, the game's name
            options_.serverName = joinWords(w, 2).substr(0, 15);
            settingsChanged();
            print("setgamename");
            return;
        }
        if (is(1, "players") && w.size() == 3) { // 0-999
            const int limit = std::clamp(number(w[2]), 0, 999);
            if (limit != options_.maxPlayers) {
                options_.maxPlayers = limit;
                settingsChanged();
                syncAccess(); // (the lobby's limit)
            }
            print("playersset", {std::to_string(limit)});
            return;
        }
        if (is(1, "lessons") && w.size() == 3) { // this mode's
            world.lessonLimits[static_cast<std::size_t>(modeIndex(world.gameFlags))] = static_cast<std::uint16_t>(std::clamp(number(w[2]), 0, 999));
            settingsChanged();
            return;
        }
        if (is(1, "time") && w.size() == 3) { // this mode's minutes
            world.timeLimits[static_cast<std::size_t>(modeIndex(world.gameFlags))] = static_cast<std::uint8_t>(std::clamp(number(w[2]), 0, 255));
            settingsChanged();
            return;
        }
        if (is(1, "cycle") && w.size() == 3 && onOff(2) >= 0) {
            mapCycleOn_ = onOff(2) == 1;
            print(mapCycleOn_ ? "MapCycleOn" : "MapCycleOff");
            return;
        }
        if (is(1, "weapons") && w.size() == 3 && onOff(2) >= 0) { // option 1
            setOption(1, onOff(2) == 1);
            print("weapons");
            return;
        }
        if (is(1, "staffs") && w.size() == 3 && onOff(2) >= 0) { // option 0x10
            setOption(0x10, onOff(2) == 1);
            print("staffs");
            return;
        }
        if (is(1, "monsters")) { // option 4; "respawn": option 8, on and off alike
            if (w.size() == 4 && is(2, "respawn") && onOff(3) >= 0) {
                setOption(8, true); // (both set it: an original bug)
                print("monsterrespawn");
            } else if (w.size() == 3 && onOff(2) >= 0) {
                setOption(4, onOff(2) == 1);
                print("monsters");
            }
            return;
        }
        if (is(1, "spell") && w.size() == 4 && onOff(3) >= 0) {
            // not in a chat game; by its title or SPELL_ name; 0x84 never turned on
            // in a flagball game.
            if (world.gameFlags & GameFlag::Chat) {
                print("NotInChat");
                return;
            }
            std::uint32_t spell = things::spellId(w[2]).value_or(0);
            if (spell == 0) {
                for (const things::SpellDef& d : things_.spells) {
                    if (equalsNoCase(strings::toUtf8(world.text(d.title)), w[2])) {
                        spell = things::spellId(d.id).value_or(0);
                    }
                }
            }
            if (spell == 0) {
                print("invalidspell");
                return;
            }
            const bool on = onOff(3) == 1;
            if (on && (world.gameFlags & GameFlag::FlagBall) && spell == 0x84) {
                return;
            }
            if (world.spellDisabled[spell & 0xFF] != !on) { // (spellIsEnabled: already so, nothing)
                world.spellDisabled[spell & 0xFF] = !on;
                settingsChanged();
                print(on ? "spellenabled" : "spelldisabled");
            }
            return;
        }
        if ((is(1, "weapon") || is(1, "armor") || is(1, "staff")) && w.size() == 4) {
            // A type allowed or not (its dd10;
            // turned off, those about go). "weapon respawn": option 2, on and
            // off alike.
            if (is(1, "weapon") && is(2, "respawn") && onOff(3) >= 0) {
                setOption(2, true); // (both set it: an original bug)
                print("weaponsrespawn");
                return;
            }
            if (world.gameFlags & GameFlag::Chat) {
                print("NotInChat");
                return;
            }
            // Which: a weapon, armour or a staff (a wand's class).
            const std::size_t which = is(1, "weapon") ? 0 : is(1, "armor") ? 1 : 2;
            static constexpr std::uint32_t kClasses[3] = {oc::Weapon, oc::Armor, oc::Wand};
            static constexpr const char* kKinds[3] = {"weapon", "armor", "staff"};
            const things::ThingDef* def = things_.findThing(w[2]);
            const auto classes = def ? things::thingClasses(*def) : 0u;
            if (!def || !(classes & kClasses[which]) || onOff(3) < 0) {
                print(std::string("invalid") + kKinds[which]);
                return;
            }
            const bool on = onOff(3) == 1;
            const bool disabled = world.disabledTypes.contains(def->name);
            if (on == !disabled) {
                return;
            }
            if (on) {
                world.disabledTypes.erase(def->name);
            } else {
                world.disabledTypes.insert(def->name);
                // (The type's sweep: each thing on the map, what it holds of the type, then itself,
                // by delayedDeleteObject.)
                for (auto& [extent, o] : world.objects()) {
                    if (o.holder != 0) {
                        continue;
                    }
                    for (const std::uint32_t e : keysOf(o.inventory)) {
                        if (ServerObject* i = world.byExtent(e); i && i->typeName == def->name) {
                            deleteObject(world, *i);
                        }
                    }
                    if (o.typeName == def->name) {
                        deleteObject(world, o);
                    }
                }
            }
            settingsChanged();
            print(std::string(kKinds[which]) + (on ? "Enabled" : "Disabled"));
            return;
        }
        if (is(1, "spellpoints")) {
            print("notyetimplemented");
            return;
        }
        return;
    }
    if (is(0, "cheat")) {
        // The cheats: every player's, never in a network game.
        if (network) {
            return;
        }
        Fight f = server_.fight();
        if (is(1, "ability")) {
            everyPlayer([&](ServerObject& o) {
                if (o.player->classId == 0) {
                    cancelAbilities(world, o); // (playerCancelAbils)
                }
            });
        } else if (is(1, "health")) {
            everyPlayer([&](ServerObject& o) { o.health = o.maxHealth; });
        } else if (is(1, "mana")) {
            everyPlayer([&](ServerObject& o) { o.player->mana = static_cast<std::uint16_t>(std::min<int>(o.player->mana + 2000, o.player->maxMana)); });
        } else if (is(1, "level") && w.size() >= 3) {
            const int level = number(w[2]);
            everyPlayer([&](ServerObject& o) {
                o.player->level = static_cast<std::uint8_t>(level);
                readVals(world, o, false);
            });
        } else if (is(1, "spells") && w.size() >= 3) {
            const int level = std::clamp(number(w[2]), 1, 5);
            everyPlayer([&](ServerObject& o) { std::fill(o.player->spellLevels.begin(), o.player->spellLevels.end(), static_cast<std::uint8_t>(level)); });
        } else if (is(1, "goto") && w.size() >= 4) {
            if (ServerObject* me = world.byExtent(issuer)) {
                setPosition(world, *me, static_cast<float>(number(w[2])), static_cast<float>(number(w[3])), f.frame);
            }
        } else if (is(1, "goto") && w.size() == 3) {
            // (the waypoint by the waypoint list's match)
            if (const ServerWaypoint* wp = world.waypointMatching(w[2])) {
                everyPlayer([&](ServerObject& o) { setPosition(world, o, wp->x, wp->y, f.frame); });
            }
        }
        return;
    }
    print("typehelp");
}

#ifndef NOX_NO_TEST_COMMANDS
void GameHost::testCommand(const std::vector<std::string>& w, std::uint32_t issuer)
{
    // noxcpp's (not GAME.EXE's): for testing a game on this server. Each acts on the issuer's
    // player, or on a dedicated server's console on every player.
    World& world = server_.world();
    Fight f = server_.fight();
    const auto is = [&](std::size_t i, std::string_view word) { return i < w.size() && equalsNoCase(w[i], word); };
    ServerObject* me = issuer ? world.byExtent(issuer) : nullptr;
    // Who the command acts on: the issuer's own player, or every player when typed at the console.
    std::vector<ServerObject*> targets;
    if (me) {
        targets.push_back(me);
    } else {
        for (auto& [extent, o] : world.objects()) {
            if (o.player && !o.gone()) {
                targets.push_back(&o);
            }
        }
    }
    const auto anchor = [&]() -> ServerObject* { return me ? me : (targets.empty() ? nullptr : targets.front()); };
    if (w.size() < 2 || is(1, "help")) {
        printText("nx spawn TYPE [N] | give TYPE | god [off] | enchant NAME [SECONDS|off] | heal | mana [N] | charge N | level N | xp N | spells N | goto X Y | goto WAYPOINT | "
                  "kill NAME|monsters | touch X Y | where | players | obelisks CHARGE | screen blind|unblind|wide [off]|intro [now]|endgame K | save NAME [FRAMES] | loadgame NAME");
        return;
    }
    if (is(1, "save") && (w.size() == 3 || w.size() == 4)) {
        // A solo game saved as the save menu asks it (setSaveFileName(1)),
        // or after FRAMES as the AutoSave builtin's delay.
        if (!(world.gameFlags & GameFlag::Solo)) {
            printText("nx: save: not a solo game");
            return;
        }
        world.saveName = w[2];
        const int delay = w.size() == 4 ? std::max(0, number(w[3])) : 0;
        requestSave(world, true, 0, static_cast<std::uint32_t>(delay), server_.clock().frame());
        printText("nx: save " + w[2] + " asked");
        return;
    }
    if (is(1, "loadgame") && w.size() == 3) {
        // A solo game's saved game loaded, as the save menu's Load.
        if (!(world.gameFlags & GameFlag::Solo) || options_.saveDir.empty()) {
            printText("nx: loadgame: not a solo game with a save folder");
            return;
        }
        printText(loadSoloGame(server_, {.gameDir = options_.saveDir, .clientSections = options_.clientSections}, w[2])
                      ? "nx: loadgame " + w[2]
                      : "nx: loadgame " + w[2] + " failed");
        return;
    }
    if (is(1, "screen") && 3 <= w.size()) {
        // The host's own client's cinematics (host_client.hpp) as the builtins call them.
        HostClient* hc = world.hostClient;
        if (!hc) {
            printText("nx: no host client here");
            return;
        }
        if (is(2, "blind")) {
            hc->blind();
        } else if (is(2, "unblind")) {
            hc->unblind();
        } else if (is(2, "wide")) {
            hc->wideScreen(!is(3, "off"));
        } else if (is(2, "intro")) {
            hc->playMapIntro(is(3, "now"));
        } else if (is(2, "endgame")) {
            hc->endGame(w.size() >= 4 ? number(w[3]) : 0);
        } else {
            printText("nx: screen blind|unblind|wide [off]|intro [now]|endgame K");
            return;
        }
        printText("nx: screen " + w[2]);
        return;
    }
    if (is(1, "obelisks") && 3 <= w.size()) {
        // Every obelisk's mana (0-50) set, the clients told as a drain tells them.
        const int charge = std::clamp(number(w[2]), 0, 50);
        int n = 0;
        for (auto& [extent, o] : world.objects()) {
            if ((o.classes & oc::Immobile) && (o.subclasses & 0x18u) && !o.gone()) {
                o.value = charge;
                o.touch();
                ++n;
            }
        }
        printText("nx: " + std::to_string(n) + " obelisks at " + std::to_string(charge));
        return;
    }
    if (is(1, "spawn") && 3 <= w.size()) {
        const ServerObject* a = anchor();
        const int n = std::clamp(w.size() >= 4 ? number(w[3]) : 1, 1, 50);  // 1 to 50 at a time
        // With nobody in to anchor on, a fixed point of the map.
        float x = a ? a->x : 2944.0f;
        float y = a ? a->y : 2944.0f;
        if (a) { // (a little ahead of it)
            const auto [fx, fy] = directionVector(static_cast<std::uint8_t>(a->direction));
            x += fx * 60.0f;
            y += fy * 60.0f;
        }
        if (!things_.findThing(w[2])) {
            printText("nx: no thing " + w[2]);
            return;
        }
        for (int i = 0; i < n; ++i) {
            // (Five to a row, 24 apart.)
            const float ox = static_cast<float>((i % 5) * 24);
            const float oy = static_cast<float>((i / 5) * 24);
            if (ServerObject* o = world.createAt(things_, w[2], x + ox, y + oy, f.frame)) {
                o->flags |= ObjFlag::Enabled;
            }
        }
        printText("nx: spawned " + std::to_string(n) + " " + w[2]);
        return;
    }
    if (is(1, "give") && 3 <= w.size()) {
        for (ServerObject* t : targets) {
            if (!server_.items().give(*t, w[2])) {
                printText("nx: can't give " + w[2]);
            }
        }
        return;
    }
    if (is(1, "god")) {
        const bool off = is(2, "off");
        for (ServerObject* t : targets) {
            if (off) {
                disenchant(world, *t, Enchant::Invulnerable);
            } else {
                enchant(world, *t, Enchant::Invulnerable, 30 * 60 * 60 * 24, 5);  // a day of frames at 30 a second
            }
        }
        printText(off ? "nx: god off" : "nx: god on");
        return;
    }
    if (is(1, "enchant") && 3 <= w.size()) {
        // An enchantment by the game's name (ENCHANT_SHIELD, with or without ENCHANT_) or bit, for
        // a minute (or N seconds), or "off".
        static constexpr std::array<std::string_view, 32> kNames{
            "INVISIBLE", "MOONGLOW", "BLINDED", "CONFUSED", "SLOWED", "HELD", "DETECTING", "ETHEREAL",
            "RUN", "HASTED", "VILLAIN", "AFRAID", "BURNING", "VAMPIRISM", "ANCHORED", "LIGHT", "DEATH",
            "PROTECT_FROM_FIRE", "PROTECT_FROM_POISON", "PROTECT_FROM_MAGIC", "PROTECT_FROM_ELECTRICITY",
            "INFRAVISION", "SHOCK", "INVULNERABLE", "TELEKINESIS", "FREEZE", "SHIELD", "REFLECTIVE_SHIELD",
            "CHARMING", "ANTI_MAGIC", "CROWN", "SNEAK"};
        std::string_view name = w[2];
        if (name.size() > 8 && equalsNoCase(name.substr(0, 8), "ENCHANT_")) {
            name.remove_prefix(8);
        }
        // The name's index in kNames is the enchantment's bit; a plain number is taken as the bit itself.
        int e = -1;
        for (std::size_t i = 0; i < kNames.size(); ++i) {
            if (equalsNoCase(name, kNames[i])) {
                e = static_cast<int>(i);
            }
        }
        if (e < 0 && !name.empty() && std::isdigit(static_cast<unsigned char>(name[0]))) {
            e = std::clamp(number(w[2]), 0, 31);
        }
        if (e < 0) {
            printText("nx: no enchantment " + w[2]);
            return;
        }
        const bool off = is(3, "off");
        const int seconds = !off && w.size() >= 4 ? std::max(number(w[3]), 1) : 60;
        for (ServerObject* t : targets) {
            if (off) {
                disenchant(world, *t, e);
            } else {
                enchant(world, *t, e, 30 * seconds, 3);  // 30 frames a second
            }
        }
        printText("nx: ENCHANT_" + std::string(kNames[static_cast<std::size_t>(e)]) + (off ? " off" : " on"));
        return;
    }
    if (is(1, "heal")) {
        for (ServerObject* t : targets) {
            t->health = t->maxHealth;
            t->poison = 0;
            t->touch();
        }
        return;
    }
    if (is(1, "mana")) {
        // Full, or N (nx mana N).
        for (ServerObject* t : targets) {
            t->player->mana = 3 <= w.size() ? static_cast<std::uint16_t>(std::clamp(number(w[2]), 0, static_cast<int>(t->player->maxMana)))
                                            : t->player->maxMana;
        }
        return;
    }
    if (is(1, "charge") && 3 <= w.size()) {
        // The weapon in hand's charge (0-100%), its charges with it (a staff to recharge).
        for (ServerObject* t : targets) {
            if (ServerObject* weapon = t->player->currentWeapon ? world.byExtent(t->player->currentWeapon) : nullptr) {
                weapon->chargePercent = std::clamp(number(w[2]), 0, 100);
                weapon->charges = static_cast<std::uint8_t>(weapon->maxCharges * weapon->chargePercent / 100);
            }
        }
        return;
    }
    if (is(1, "level") && 3 <= w.size()) {
        for (ServerObject* t : targets) {
            t->player->level = static_cast<std::uint8_t>(std::clamp(number(w[2]), 1, 10));  // 1 to 10, its numbers then set again by class and level (readVals)
            readVals(world, *t, true);
        }
        return;
    }
    if (is(1, "xp") && 3 <= w.size()) {
        for (ServerObject* t : targets) {
            giveExperience(f, *t, static_cast<float>(number(w[2])));
        }
        return;
    }
    if (is(1, "spells") && 3 <= w.size()) {
        const auto level = static_cast<std::uint8_t>(std::clamp(number(w[2]), 0, 5));
        for (ServerObject* t : targets) {
            std::fill(t->player->spellLevels.begin() + 1, t->player->spellLevels.end(), level);  // index 0 is SPELL_INVALID: left as it is
        }
        return;
    }
    if (is(1, "goto") && 4 <= w.size()) {
        for (ServerObject* t : targets) {
            setPosition(world, *t, static_cast<float>(number(w[2])), static_cast<float>(number(w[3])), f.frame);
        }
        return;
    }
    if (is(1, "goto") && 3 <= w.size()) {
        const ServerWaypoint* wp = world.waypointNamed(w[2]);
        if (!wp) {
            printText("nx: no waypoint " + w[2]);
            return;
        }
        for (ServerObject* t : targets) {
            setPosition(world, *t, wp->x, wp->y, f.frame);
        }
        return;
    }
    if (is(1, "kill") && 3 <= w.size()) {
        if (is(2, "monsters")) {
            // Every living monster on the map, killed by taking all its health.
            for (auto& [extent, o] : world.objects()) {
                if ((o.classes & oc::Monster) && !o.gone() && !(o.flags & ObjFlag::Dead)) {
                    f.takeHealth(o, o.health);
                }
            }
            return;
        }
        if (HostedPlayer* p = playerNamed(w[2])) {
            if (ServerObject* o = world.byExtent(p->extent)) {
                f.takeHealth(*o, o->health);
            }
        }
        return;
    }
    if (is(1, "touch") && 4 <= w.size()) {
        // The trigger nearest (X, Y) (a lever, switch, button: within 50) touched by the player, as
        // its client's MSG_TRY_COLLIDE does.
        const float x = static_cast<float>(number(w[2]));
        const float y = static_cast<float>(number(w[3]));
        ServerObject* best = nullptr;
        float bestD = 50.0f * 50.0f;
        for (auto& [extent, o] : world.objects()) {
            const float d = (o.x - x) * (o.x - x) + (o.y - y) * (o.y - y);
            if ((o.classes & oc::Trigger) && !o.gone() && d <= bestD) {
                bestD = d;
                best = &o;
            }
        }
        ServerObject* by = anchor();
        if (!best || !by) {
            printText("nx: no trigger there");
            return;
        }
        collideFunction(f, *best, by);
        printText("nx: touched " + best->typeName);
        return;
    }
    if (is(1, "where")) {
        for (ServerObject* t : targets) {
            printText(std::to_string(static_cast<int>(t->x)) + " " + std::to_string(static_cast<int>(t->y)));
        }
        return;
    }
    if (is(1, "players")) {
        for (const auto& [peer, p] : players_) {
            const ServerObject* o = world.byExtent(p.extent);
            printText(p.name + (o ? "  " + std::to_string(o->health) + "/" + std::to_string(o->maxHealth) + " hp" : "") +
                      (sysops_.contains(peer) ? "  (sysop)" : ""));
        }
        return;
    }
    printText("nx: unknown (nx help)");
}
#endif // NOX_NO_TEST_COMMANDS

} // namespace nox::server
