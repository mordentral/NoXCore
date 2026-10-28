// NPC dialog on the server: SetDialog / CancelDialog / StoryPic on a monster, the MSG_DIALOG 3
// that opens a dialog on a player's screen, and the start and answer handlers that run the
// NPC's script functions (dialog.hpp has the flow).
#include "noxcore/server/dialog.hpp"

#include "noxcore/server/monsters.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/script_host.hpp"
#include "noxcore/server/script_names.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/object_class.hpp"

#include <array>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;

/// the NPC's name, Str_GetString("NPC:" + its script name (IDPtr) after its last
/// ':', or its type's, with the '_' left out). Missing from the string table: Str_GetString's
/// "MISSING:'<label>'" (World::text).
std::u16string npcName(const World& world, const ServerObject& npc)
{
    std::string_view name = npc.scriptName.empty() ? std::string_view(npc.typeName) : std::string_view(npc.scriptName);
    name = name.substr(0, name.find('\0'));
    if (const auto colon = name.rfind(':'); colon != std::string_view::npos) {
        name = name.substr(colon + 1);
    }
    std::string label = "NPC:";
    for (const char c : name) {
        if (c != '_') {
            label += c;
        }
    }
    return world.text(label);
}

} // namespace

std::uint8_t dialogType(std::string_view name)
{
    // The table: {name, type} pairs to a null name (script_names.hpp's, the editor's too).
    name = name.substr(0, name.find('\0'));
    for (const ScriptDialogType& t : scriptDialogTypes()) {
        if (t.name == name) {
            return t.type;
        }
    }
    return 0;
}

void setDialog(ServerObject& npc, std::uint8_t type, std::int32_t start, std::int32_t end)
{
    if (!(npc.classes & oc::Monster) || !npc.monster) {
        return;
    }
    if (start != -1 && end != -1) {
        npc.monster->dialogStart = start;
        npc.monster->dialogEnd = end;
        npc.monster->dialogType = type;
        setXStatus(npc, 0x10); // (unitSetXStatus)
    }
}

void cancelDialog(ServerObject& npc)
{
    if (!(npc.classes & oc::Monster) || !npc.monster) {
        return;
    }
    npc.monster->dialogStart = -1;
    npc.monster->dialogEnd = -1;
    unsetXStatus(npc, 0x10);
}

void storyPic(ServerObject& npc, std::string_view picture)
{
    if ((npc.classes & oc::Monster) && npc.monster) {
        npc.monster->portrait = std::string(picture.substr(0, picture.find('\0'))); // (a strcpy, as the exe's)
    }
}

std::vector<std::uint8_t> npcDialogMessage(const World& world, const ServerObject& npc, std::uint32_t sound, std::string_view textId)
{
    // SrvNet_OpenNpcDialog's 135 bytes on its stack, written in its order; what runs past the
    // message's end is the exe's stack and isn't sent.
    std::vector<std::uint8_t> m(0x87, 0);
    const auto put = [&](std::size_t at, std::uint8_t b) {
        if (at < m.size()) {
            m[at] = b;
        }
    };
    const auto strcpyAt = [&](std::size_t at, std::string_view s) {
        s = s.substr(0, s.find('\0'));
        for (std::size_t i = 0; i < s.size(); ++i) {
            put(at + i, static_cast<std::uint8_t>(s[i]));
        }
        put(at + s.size(), 0);
    };
    // MSG_DIALOG (0xD0), sub-code 3: open the dialog.
    m[0] = 0xD0;
    m[1] = 3;
    strcpyAt(2, textId);
    // The name: (wcsncpy) of 31 letters, the rest of them zeros; then 0 at +0x60.
    const std::u16string name = npcName(world, npc);
    bool ended = false;
    for (std::size_t i = 0; i < 31; ++i) {
        const char16_t c = !ended && i < name.size() ? name[i] : u'\0';
        ended = ended || c == u'\0';
        put(0x22 + 2 * i, static_cast<std::uint8_t>(c));
        put(0x23 + 2 * i, static_cast<std::uint8_t>(c >> 8));
    }
    put(0x60, 0);
    put(0x61, 0);
    const ServerObject::Monster* mon = npc.monster ? &*npc.monster : nullptr;
    put(0x86, mon ? mon->dialogType : 0); // (the NPC's dialog type)
    for (int i = 0; i < 4; ++i) {
        put(0x62 + static_cast<std::size_t>(i), static_cast<std::uint8_t>(sound >> (8 * i)));
    }
    const std::string_view portrait = mon ? std::string_view(mon->portrait) : std::string_view{};
    strcpyAt(0x66, portrait.empty() ? std::string_view("ShopKeeperPic") : portrait);
    return m;
}

void openNpcDialog(Fight& fight, ServerObject* player, ServerObject* npc, std::uint32_t sound, std::string_view textId)
{
    // (Without an NPC the exe reads through null and fails, an original bug: nothing here.)
    if (!npc || !player || !(player->classes & oc::Player) || !player->player) {
        return;
    }
    World& world = fight.world;
    player->player->dialogWith = npc->extent;
    world.broadcasts().push_back({npcDialogMessage(world, *npc, sound, textId), true, player->extent});
    freezeUnit(fight, *player, 0);
}

void dialogStart(Fight& fight, ServerObject& player, ServerObject& npc)
{
    if (!(player.classes & oc::Player) || !(npc.classes & oc::Monster) || (player.flags & (ObjFlag::Dead | ObjFlag::Destroyed)) ||
        !(npc.xStatus & 0x10) || !npc.monster) {
        return;
    }
    const ServerObject::Monster& m = *npc.monster;
    if (m.dialogStart == -1 || m.dialogEnd == -1) {
        return;
    }
    if (ScriptHost* script = fight.world.script) {
        script->call(static_cast<std::uint32_t>(m.dialogStart), player.extent, npc.extent); // (script_callByIndex)
    }
}

void dialogAnswer(Fight& fight, ServerObject& player, std::uint8_t answer)
{
    World& world = fight.world;
    if (!player.player) {
        return;
    }
    unfreezeUnit(fight, player, 0);
    ServerObject* npc = player.player->dialogWith ? world.byExtent(player.player->dialogWith) : nullptr;
    if (!npc || !npc->monster || npc->monster->dialogStart == -1 || npc->monster->dialogEnd == -1) {
        return;
    }
    ServerObject::Monster& m = *npc->monster;
    world.broadcasts().push_back({{0xD0, 4}, true, player.extent});
    player.player->dialogWith = 0;
    m.dialogAnswer = m.dialogType == 1 ? answer : 0; // (a YESNO's alone; YESNONEXT's too is 0)
    if (ScriptHost* script = world.script) {
        script->call(static_cast<std::uint32_t>(m.dialogEnd), player.extent, npc->extent);
    }
}

std::uint8_t dialogAnswerOf(const ServerObject* o)
{
    return o && (o->classes & oc::Monster) && o->monster ? o->monster->dialogAnswer : 0;
}

} // namespace nox::server
