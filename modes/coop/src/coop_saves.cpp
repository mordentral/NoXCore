// The co-op campaign's saves and returning players (coop.hpp): the party's characters in each save's
// Party folder beside the solo game's own files, given back to the runners the campaign knows.
#include "noxcore/coop/coop.hpp"

#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/player/player_file.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/player_save.hpp"
#include "noxcore/server/server.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/things/object_class.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <locale>
#include <string>
#include <vector>

namespace nox::coop {

namespace {

namespace fs = std::filesystem;

/// A runner key as a file's name: letters and digits kept, the rest '_', and the key's hash (two
/// keys alike once cut stay apart).
std::string fileKey(std::string_view key)
{
    std::string out;
    for (const char c : key.substr(0, 40)) {
        out += (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : '_';
    }
    std::uint32_t h = 2166136261u;
    for (const char c : key) {
        h = (h ^ static_cast<std::uint8_t>(c)) * 16777619u;
    }
    char hex[10];
    std::snprintf(hex, sizeof hex, "-%08x", h);
    return out + hex;
}

/// A field of party.txt's lines (tab-separated): no tabs or line ends in it.
std::string field(std::string_view s)
{
    std::string out(s);
    std::ranges::replace_if(out, [](char c) { return c == '\t' || c == '\r' || c == '\n'; }, ' ');
    return out;
}

std::vector<std::string> split(const std::string& line, char by)
{
    std::vector<std::string> out;
    std::string cur;
    for (const char c : line) {
        if (c == by) {
            out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(std::move(cur));
    return out;
}

std::vector<std::uint8_t> readAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool writeAll(const fs::path& p, std::span<const std::uint8_t> bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

} // namespace

fs::path CoopDirector::partyFolder() const
{
    if (!host_ || host_->options().saveDir.empty()) {
        return {};
    }
    return host_->options().saveDir / "Save" / "WORKING" / "Party";
}

void CoopDirector::readRecords()
{
    records_.clear();
    const fs::path dir = partyFolder();
    if (dir.empty()) {
        return;
    }
    std::ifstream f(dir / "party.txt");
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto v = split(line, '\t');
        if (v.size() < 7 || v[0].empty()) {
            continue;
        }
        Record r;
        r.key = v[0];
        r.name = v[1];
        try {
            r.playerClass = static_cast<std::uint8_t>(std::stoi(v[2]));
            r.map = v[3];
            r.x = std::stof(v[4]);
            r.y = std::stof(v[5]);
            for (const std::string& id : split(v[6], ',')) {
                if (!id.empty()) {
                    r.owned.push_back(std::stoi(id));
                }
            }
        } catch (const std::exception&) {
            continue; // (a line that doesn't read: that runner comes back as itself)
        }
        records_[r.key] = std::move(r);
    }
}

void CoopDirector::writeRecords()
{
    const fs::path dir = partyFolder();
    if (dir.empty()) {
        return;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f(dir / "party.txt", std::ios::trunc);
    f.imbue(std::locale::classic());
    f << std::setprecision(9); // (a float's place, whole)
    for (const auto& [key, r] : records_) {
        f << field(r.key) << '\t' << field(r.name) << '\t' << int(r.playerClass) << '\t' << field(r.map) << '\t' << r.x << '\t' << r.y << '\t';
        for (std::size_t i = 0; i < r.owned.size(); ++i) {
            f << (i ? "," : "") << r.owned[i];
        }
        f << '\n';
    }
}

std::vector<std::uint8_t> CoopDirector::keepGuest(const Member& m, server::ServerObject& unit)
{
    const fs::path dir = partyFolder();
    if (dir.empty() || m.host || !unit.player) {
        return {};
    }
    // Only a character that is the runner's own (given back, its file loaded, or settled without
    // one: the catch-up's moment) and of the campaign's class: never the stand-in made at its join,
    // nor one about to be let go for its class.
    // (A stock guest always sends its own: kept only once it's in, or given back.)
    const bool settled = m.identified ? (m.restored || m.characterIn || m.caughtUp) : (m.restored || m.characterIn);
    if (!settled || unit.player->classId != options_.campaignClass) {
        return {};
    }
    server::Fight f = host_->server().fight();
    // A player down is kept on its feet (its file's health is put on it at the load: none would
    // kill it again there).
    const bool down = (unit.flags & server::ObjFlag::Dead) != 0;
    const std::uint16_t health = unit.health;
    const auto mana = unit.player->mana;
    if (down) {
        unit.health = unit.maxHealth;
        unit.player->mana = unit.player->maxMana;
    }
    const auto built = server::buildPlayerFile(f, unit);
    if (down) {
        unit.health = health;
        unit.player->mana = mana;
    }
    if (!built) {
        return {}; // (the exe's save would fail too: its last kept stays)
    }
    std::vector<std::uint8_t> bytes = player::serializePlayerFile(*built);
    crypto::cipherForKey(crypto::FileKey::PlayerFile).encrypt(bytes);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!writeAll(dir / (fileKey(m.key) + ".plr"), bytes)) {
        return {};
    }
    Record r;
    r.key = m.key;
    r.name = m.name;
    r.playerClass = unit.player->classId;
    r.map = world().mapName;
    r.x = unit.x;
    r.y = unit.y;
    for (const auto& [e, o] : world().objects()) {
        if (o.owner == unit.extent && &o != &unit && o.scriptId != 0 && !o.gone() && (o.classes & things::ObjectClass::Monster)) {
            r.owned.push_back(o.scriptId);
        }
    }
    records_[m.key] = std::move(r);
    return server::saveDataSent(*built); // (what the guest keeps: as a quest's SAVEDATA)
}

bool CoopDirector::save(const std::string& name, server::ServerObject* exit)
{
    // The party first, into WORKING (a save other than WORKING copies it along with the rest).
    std::vector<std::pair<const Member*, std::vector<std::uint8_t>>> kept;
    if (host_) {
        for (const Member& m : party_) {
            if (server::ServerObject* u = host_->unitOf(m.peer); u && u->player && !m.host) {
                if (auto bytes = keepGuest(m, *u); !bytes.empty()) {
                    kept.emplace_back(&m, std::move(bytes));
                }
            }
        }
        writeRecords();
    }
    // The map's copy with its monsters' own health (the party's scale put back after): a load or a
    // revisit scales them from the game's numbers again, never on top of a scale.
    const float scale = scaledHealth_;
    if (host_ && scale != 1.0f) {
        applyScale(1.0f);
    }
    const bool ok = coreSave_ ? coreSave_(name, exit) : true;
    if (host_ && scale != 1.0f) {
        applyScale(scale);
    }
    // Each NoX Core guest its campaign character to keep (the game saved: what it holds now).
    if (ok) {
        for (const auto& [m, bytes] : kept) {
            sendCharacter(*m, bytes);
        }
    }
    return ok;
}

void CoopDirector::sendCharacter(const Member& m, std::span<const std::uint8_t> plr)
{
    if (!m.identified) {
        // A stock guest's: the quest's save flow (MSG_GAUNTLET 0x1C {1}, SAVEDATA written as a
        // quest's: its client keeps it as the quest character it came with, and answers 0x1B).
        if (host_) {
            host_->sendSaveTo(m.peer);
        }
        return;
    }
    for (std::vector<std::uint8_t>& part : coopmsg::characterMessages(plr)) {
        tell(m, std::move(part));
    }
}

bool CoopDirector::restoreGuest(Member& m, server::ServerObject& unit, bool place)
{
    const auto it = records_.find(m.key);
    const fs::path dir = partyFolder();
    if (it == records_.end() || dir.empty() || !unit.player || it->second.playerClass != options_.campaignClass) {
        return false;
    }
    const std::vector<std::uint8_t> bytes = readAll(dir / (fileKey(m.key) + ".plr"));
    if (bytes.empty()) {
        return false;
    }
    server::Fight f = host_->server().fight();
    if (unit.flags & server::ObjFlag::Dead) {
        f.respawn(unit, true); // (on its feet first: the file's numbers then)
    }
    if (!server::loadPlayerData(f, unit, bytes, nullptr, true)) {
        return false;
    }
    m.restored = true;
    server::World& w = world();
    const std::uint32_t frame = host_->server().clock().frame();
    if (place && it->second.map == w.mapName) {
        const auto [x, y] = server::clearPointNear(w, rng_, it->second.x, it->second.y, 50.0f);
        server::setPosition(w, unit, x, y, frame);
    }
    // Its creatures on this map (script ids are a map's own), saved with it but owned by nobody
    // after the load (the save's location owns the host's alone), its own again.
    if (it->second.map == w.mapName) {
        giveCreatures(it->second.owned, unit);
    }
    rosterDirty_ = true;
    return true;
}

void CoopDirector::giveCreatures(const std::vector<std::int32_t>& owned, server::ServerObject& to)
{
    // (As restoreFromSaveLocation gives the host its own: a summoned one acquired again, its
    // minimap mark.)
    server::World& w = world();
    for (const std::int32_t id : owned) {
        server::ServerObject* o = w.byScriptId(id);
        if (!o || o->gone() || o->owner != 0 || !(o->classes & things::ObjectClass::Monster)) {
            continue;
        }
        server::setOwner(w, &to, *o);
        if (o->monster && (o->monster->status & things::MonsterStatus::Summoned)) {
            o->subclasses |= 0x80u;
            server::reportAcquired(w, to, *o);
            server::markMinimap(w, to, *o, 1);
        }
    }
}

void CoopDirector::gameLoaded()
{
    loadPending_ = true; // (the saved map's load comes next: restoreParty at its mapLoaded)
}

void CoopDirector::restoreParty()
{
    readRecords();
    for (Member& m : party_) {
        server::ServerObject* u = host_->unitOf(m.peer);
        if (!u || !u->player || m.host) {
            continue;
        }
        const bool restored = restoreGuest(m, *u, true);
        if (restored && !(u->flags & server::ObjFlag::Dead)) {
            downs_.erase(m.peer); // (on its feet with it)
            continue;
        }
        // (None kept for it: as it was, by the host; back on its feet at the checkpoint if down.)
        if (u->flags & server::ObjFlag::Dead) {
            downs_[m.peer] = 0;
        }
        if (restored) {
            continue; // (placed by its record)
        }
        if (const server::ServerObject* h = server::hostPlayer(world())) {
            const auto [x, y] = server::clearPointNear(world(), rng_, h->x, h->y, 50.0f);
            server::setPosition(world(), *u, x, y, host_->server().clock().frame());
        }
    }
    // The creatures of runners who aren't here: the host's (not wild meanwhile).
    if (server::ServerObject* h = server::hostPlayer(world())) {
        for (const auto& [key, r] : records_) {
            const bool here = std::ranges::any_of(party_, [&](const Member& m) { return m.key == key; });
            if (!here && r.map == world().mapName) {
                giveCreatures(r.owned, *h);
            }
        }
    }
}

void CoopDirector::characterIn(server::GameHost& host, std::uint8_t peer, bool loaded)
{
    host_ = &host;
    for (Member& m : party_) {
        if (m.peer != peer) {
            continue;
        }
        m.characterIn = loaded;
        m.caughtUp = false; // (its own numbers now: looked at again)
        // A runner the campaign kept: its campaign character wins over the file it came with (a
        // stock client sends its own whatever it's told; an older one, or one refused and its
        // things gone, would be the character from now on).
        if (m.restored) {
            if (server::ServerObject* u = host.unitOf(peer); u && u->player) {
                m.restored = false;
                restoreGuest(m, *u, false);
            }
        }
    }
}

void CoopDirector::catchUp(Member& m, server::ServerObject& unit)
{
    m.caughtUp = true;
    if (!options_.catchUp || m.restored || !unit.player) {
        return;
    }
    // The party's lowest level but this player's (the host's among them).
    int lowest = 0;
    for (const Member& o : party_) {
        const server::ServerObject* u = o.peer == m.peer ? nullptr : host_->unitOf(o.peer);
        if (u && u->player && (lowest == 0 || u->player->level < lowest)) {
            lowest = u->player->level;
        }
    }
    const auto known = [&] {
        return std::ranges::count_if(unit.player->spellLevels, [](std::uint8_t l) { return l != 0; }) +
               std::ranges::count(unit.player->fieldGuide, true);
    };
    const auto knownBefore = known();
    const int levelBefore = unit.player->level;
    if (lowest > unit.player->level) {
        // (Plr_CheckLevelUp takes one level a gain: the experience given a level at a time.)
        server::Fight f = host_->server().fight();
        for (int level = unit.player->level + 1; level <= lowest; ++level) {
            const float want = world().number("XPTable", static_cast<std::size_t>(level));
            if (want > unit.experience) {
                server::giveExperience(f, unit, want - unit.experience);
            }
        }
    }
    // And the party's book (deliberately: a late joiner comes without the items, not without
    // the spells): every spell, creature and ability another player knows, at the best level.
    // A party that started together has nothing in it yet.
    partyBookTo(unit);
    const bool spells = known() != knownBefore;
    if (unit.player->level != levelBefore || spells) {
        notice(m, coopmsg::Notice::Info, "Caught up with the party: level " + std::to_string(unit.player->level) +
                                             (spells ? ", and the party's spells." : "."));
    }
}

} // namespace nox::coop
