// Test automation (app.hpp): the --fake-* screens, --pickup-test, and the scripted keys and
// mouse of --mouse-script.
#include "app.hpp"

#include "noxcore/net/messages.hpp"
#include "noxcore/things/object_class.hpp"

#include <utility>

namespace noxsdl {

/// --pickup-test: walks to the nearest item on the ground with the mouse on it, and clicks once
/// the pickup hand shows, as a player would; logs what's carried afterwards.
void pickupTest(App& app)
{
    auto& client = app.live->client();
    const auto self = app.anchors.find(client.netCode());
    if (self == app.anchors.end()) {
        return;
    }
    if (app.pickupSentMs != 0) {
        if (SDL_GetTicks() - app.pickupSentMs > 2000) {
            std::string carried;
            for (const auto& [nc, item] : client.world().self().inventory) {
                carried += " " + client.world().thingName(item.typeId);
            }
            SDL_Log("pickup test: carrying%s", carried.c_str());
            app.pickupTest = false;
        }
        return;
    }
    if (!app.pickupTarget || !app.anchors.count(*app.pickupTarget)) {
        float best = 0;
        app.pickupTarget.reset();
        for (const auto& [nc, anchor] : app.anchors) {
            const auto* d = client.world().drawable(nc);
            const TypeArt* a = d ? app.art->get(d->typeId) : nullptr;
            if (!a || !(a->classes & nox::things::ObjectClass::Pickup)) {
                continue;
            }
            const float dx = anchor.x - self->second.x;
            const float dy = anchor.y - self->second.y;
            if (!app.pickupTarget || dx * dx + dy * dy < best) {
                best = dx * dx + dy * dy;
                app.pickupTarget = nc;
            }
        }
        if (!app.pickupTarget) {
            return;
        }
        const Anchor& target = app.anchors.at(*app.pickupTarget);
        SDL_Log("pickup test: going for %s at %.0f,%.0f",
                client.world().thingName(client.world().drawable(*app.pickupTarget)->typeId).c_str(), target.x,
                target.y);
        client.walkTo(static_cast<int>(target.x), static_cast<int>(target.y));
    }
    const Anchor& target = app.anchors.at(*app.pickupTarget);
    app.testMouse = SDL_FPoint{(target.x - app.camera.left()) * app.camera.zoom,
                               (target.y - 4 - target.z - app.camera.top()) * app.camera.zoom};
    if ((app.cursorType == CursorType::Pickup || app.cursorType == CursorType::Caution) && app.cursorHovered == app.pickupTarget) {
        SDL_Log("pickup test: the pickup hand is on it; clicking");
        client.walkTo(static_cast<int>(self->second.x), static_cast<int>(self->second.y)); // stop
        client.pickup(*app.cursorHovered);
        app.pickupSentMs = SDL_GetTicks();
    }
}

/// --fake-shop / --fake-dialog: a shop with some stock, or an NPC asking a question, handled as
/// if the server had sent them (the test server's maps have no shopkeepers).
void fakeScreen(App& app, int which)
{
    auto& client = app.live->client();
    // The message of the day closed, out of the screen's way.
    const auto motdAway = [&app] {
        if (app.motdWindow) {
            app.live->takeNewMotd();
            app.motdWindow->close();
        }
    };
    if (which == 7 || which == 8) {
        // --fake-quest-keys: the keys shared (MSG_GAUNTLET 0x18), we hold both keys (0x16 / 0x17),
        // the quest list up. --fake-game-over: a quest's game over (0x02) with some tallies.
        const auto code = static_cast<std::uint16_t>(client.netCode());
        const auto lo = static_cast<std::uint8_t>(code), hi = static_cast<std::uint8_t>(code >> 8);
        if (which == 7) {
            // 0xF0 is MSG_GAUNTLET; the next byte is its sub-message.
            client.injectForTest(std::array<std::uint8_t, 3>{0xF0, 0x18, 1});
            client.injectForTest(std::array<std::uint8_t, 5>{0xF0, 0x16, 1, lo, hi});
            client.injectForTest(std::array<std::uint8_t, 5>{0xF0, 0x17, 1, lo, hi});
            if (app.rankBoard && app.rankBoard->mode() != 1) {
                app.rankBoard->cycle(true, app.online);
            }
        } else {
            client.injectForTest(std::array<std::uint8_t, 14>{0xF0, 0x02, 4, 0, 2, 0, 37, 0, 1, 0, 0, 0, 0, 0});
        }
        return;
    }
    if (which == App::kFakeModeScreen) {
        // A mode's own (its --fake-* flags, each screen of its windows shown locally:
        // mode_ui.hpp).
        for (auto& [id, ui] : app.modeUis) {
            if (ui->fakeScreen(app, app.fakeModeFlag)) {
                break;
            }
        }
        motdAway();
        return;
    }
    // The rest are raw server messages (MSG_TRADE 0xC9, MSG_DIALOG 0xD0), built here and fed to the
    // client as if they had come from the server.
    std::vector<std::uint8_t> m;
    const auto u16 = [&](std::uint16_t v) {
        m.push_back(static_cast<std::uint8_t>(v));
        m.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    const auto u32 = [&](std::uint32_t v) {
        u16(static_cast<std::uint16_t>(v));
        u16(static_cast<std::uint16_t>(v >> 16));
    };
    const auto ascii = [&](const std::string& s, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            m.push_back(i < s.size() ? static_cast<std::uint8_t>(s[i]) : 0);
        }
    };
    const auto wide = [&](const std::string& s, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            u16(i < s.size() ? static_cast<std::uint16_t>(static_cast<unsigned char>(s[i])) : 0);
        }
    };
    // 4: the shop's offer for our first item (a price of 15).
    if (which == 4) {
        if (!client.world().self().inventory.empty()) {
            m = {0xC9, 0x1D};
            // Sub 0x1D: the item's net code, then the price.
            u16(client.world().self().inventory.begin()->first);
            u32(15);
            client.injectForTest(m);
        }
        return;
    }
    // 1 / 3: a shop opens (sub 0x0D: the shopkeeper's type, the shop's name, its greeting), then its
    // stock, an item a message (sub 0x08).
    if (which == 1 || which == 3) {
        m = {0xC9, 0x0D};
        u16(typeIdOf(app.things, "ShopkeeperWarriorsRealm"));
        wide("Test Shop", 25);  // the name: 25 16-bit characters
        ascii("War03b:Shopkeeper", 32);  // the greeting's string id: 32 bytes
        client.injectForTest(m);
        std::uint16_t nc = 0x7F00;
        const std::array<std::pair<const char*, std::uint32_t>, 6> stock{
            {{"Longsword", 120}, {"RedPotion", 25}, {"RedPotion", 25}, {"RedPotion", 25}, {"BluePotion", 30}, {"WarHammer", 9000}}};
        for (const auto& [name, price] : stock) {
            m = {0xC9, 0x08};
            // Type, a net code for the item, price, 100, then four 0xFF modifier bytes.
            u16(typeIdOf(app.things, name));
            u16(nc++);
            u32(price);
            u32(100);
            ascii("\xFF\xFF\xFF\xFF", 4);
            client.injectForTest(m);
        }

    } else {
        // 2: an NPC's dialog (sub 3): its line, name, picture and a Yes / No answer.
        m = {0xD0, 0x03};
        ascii("Con01a:CaptainGreet", 32);
        wide("Airship Captain", 32);
        u32(0);
        ascii("AirshipCaptainPic", 32);
        m.push_back(1); // Yes / No
        client.injectForTest(m);
    }
}

/// A key's press (or with `down` false its release), as SDL sends it: the tests' scripted keys.
SDL_Event keyEvent(SDL_Keycode key, SDL_Scancode scancode, bool down)
{
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = key;
    e.key.scancode = scancode;
    e.key.down = down;
    return e;
}

/// A key pressed and let go.
void pushKeyTap(SDL_Keycode key, SDL_Scancode scancode)
{
    SDL_Event e = keyEvent(key, scancode);
    SDL_PushEvent(&e);
    e = keyEvent(key, scancode, false);
    SDL_PushEvent(&e);
}

/// A --mouse-script "key" step's press: x its SDL keycode.
SDL_Event scriptedKey(const App::ScriptedMouse& m)
{
    const auto key = static_cast<SDL_Keycode>(m.x);
    return keyEvent(key, SDL_GetScancodeFromKey(key, nullptr));
}

/// A --mouse-script step's mouse event: a move (0; windows track the mouse by its motion events),
/// else a button: 1 / 3 press, 2 / 4 let go; 3 and 4 are the right button.
SDL_Event scriptedMouseEvent(const App::ScriptedMouse& m)
{
    SDL_Event e{};
    if (m.action == 0) {
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.x = m.x;
        e.motion.y = m.y;
        return e;
    }
    e.type = m.action % 2 == 1 ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.button = m.action >= 3 ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
    e.button.down = m.action % 2 == 1;
    e.button.x = m.x;
    e.button.y = m.y;
    return e;
}

/// Each frame in a game: the test switches' steps once in (--spawn, --goto, --pickup-test,
/// --hold-move, --chat-test, --drop-equipped, --award-test, --mouse-script, the --fake-* screens,
/// --cast-test).
void runGameTests(App& app, bool inGame)
{
    auto& client = app.live->client();
    // Test automation: spawn a second in (after any team join), then walk somewhere.
    if (inGame && app.autoSpawn && !app.autoDone && SDL_GetTicks() - app.inGameSinceMs > 1000) {
        client.spawn();
        if (app.autoGoto) {
            client.walkTo(app.autoGoto->x, app.autoGoto->y);
        }
        app.autoDone = true;
    }
    if (inGame && app.pickupTest && app.autoDone) {
        pickupTest(app);
    }
    if (inGame && app.holdMoveTest && app.autoDone && SDL_GetTicks() - app.inGameSinceMs > 1500) {
        // Presses the right mouse button once (MOUSE_BUTTON_RIGHT = MoveForward: through the
        // bindings) and holds it, and logs where we are and our animation once a second.
        if (!std::exchange(app.testMoving, true)) {
            SDL_Event press{};
            press.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            press.button.button = SDL_BUTTON_RIGHT;
            press.button.down = true;
            SDL_PushEvent(&press);
        }
        if (SDL_GetTicks() >= app.holdMoveLogMs) {
            app.holdMoveLogMs = SDL_GetTicks() + 1000;
            if (const auto* self = client.world().drawable(client.netCode())) {
                SDL_Log("hold-move: at %d,%d anim state %u", self->x, self->y, self->animState);
            }
        }
    }
    if (inGame && !app.chatTest.empty() && app.autoDone && SDL_GetTicks() - app.inGameSinceMs > 2500) {
        // Enter, the line typed, Enter: through the Chat binding and the chat entry.
        static std::string typed;
        typed = std::exchange(app.chatTest, {});
        SDL_Event e = keyEvent(SDLK_RETURN, SDL_SCANCODE_RETURN);
        SDL_PushEvent(&e);
        SDL_Event t{};
        t.type = SDL_EVENT_TEXT_INPUT;
        t.text.text = typed.c_str();
        SDL_PushEvent(&t);
        SDL_PushEvent(&e);
    }
    if (inGame && app.dropEquipped && app.autoDone && client.world().self().inventoryLoaded &&
        SDL_GetTicks() - app.inGameSinceMs > 1500) {
        // Empty-handed and bare, for the plain idle animation (the HD pack's): what's worn is
        // taken off (the server won't drop it while it's worn).
        app.dropEquipped = false;
        for (const auto& [nc, item] : client.world().self().inventory) {
            if (item.equipped) {
                client.unequip(nc);
            }
        }
    }
    if (inGame && app.awardTest && app.book && app.autoDone && SDL_GetTicks() - app.inGameSinceMs > 2000) {
        // MSG_REPORT_SPELL_AWARD {spell, level, flags 0x81} as the server sends it, read as one
        // (Cli_AwardSpell: the level known, then the book's event, taken with the others).
        const auto id = static_cast<std::uint8_t>(std::exchange(app.awardTest, 0));
        const auto& levels = client.world().self().spellLevels;
        const auto known = levels.find(id);
        const std::uint8_t level = known != levels.end() && known->second != 0 ? known->second : std::uint8_t{1};
        const std::uint8_t m[] = {static_cast<std::uint8_t>(nox::net::Op::ReportSpellAward), id, level, 0x81};
        client.injectForTest(m);
        SDL_Log("award test: spell %u at level %u (the book's moment from client frame %u)", id, level, clockFrame());
    }
    while (inGame && app.autoDone && !app.mouseScript.empty() &&
           SDL_GetTicks() - app.inGameSinceMs >= app.mouseScript.front().atMs) {
        const auto m = app.mouseScript.front();
        app.mouseScript.erase(app.mouseScript.begin());
        app.testMouse = SDL_FPoint{m.x, m.y};
        if (m.action == 7) {
            if (app.console) {
                app.console->run("nx goto " + std::to_string(static_cast<int>(m.x)) + " " + std::to_string(static_cast<int>(m.y)));
            }
            app.testMouse.reset();
        } else if (m.action == 8) {
            const SDL_Event e = scriptedKey(m);
            pushKeyTap(e.key.key, e.key.scancode); // a key pressed and let go
            app.testMouse.reset();
        } else if (m.action == 5) {
            pushKeyTap(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE); // Escape pressed and let go
        } else {
            SDL_Event e = scriptedMouseEvent(m);
            SDL_PushEvent(&e);
        }
    }
    // (The quest's, 7 and 8: in a quest map, not its chat map (flag 0x80), no briefing up.)
    const bool fakeQuestReady = app.fakeScreen < 7 || app.fakeScreen > 8 || ((gameFlags(app) & 0x1080u) == 0x1000u && app.cinematics &&
                                                        !app.cinematics->screenUp());
    if (inGame && app.fakeScreen && app.autoDone && fakeQuestReady &&
        SDL_GetTicks() - app.inGameSinceMs > (app.fakeScreen == 4 ? 3000u : 2000u)) {
        const int which = std::exchange(app.fakeScreen, 0);
        fakeScreen(app, which);
        if (which == 3) {
            app.fakeScreen = 4; // the offer a second later
        }
    }
    if (inGame && app.castTest && app.autoDone && SDL_GetTicks() - app.inGameSinceMs > 2500) {
        const std::uint32_t spell = std::exchange(app.castTest, 0);
        client.castSpells(std::span(&spell, 1));
        noteCast(app, spell);
    }
}

} // namespace noxsdl
