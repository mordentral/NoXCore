// The live game's controls (app.hpp): what a bound key's or button's controls do in a game
// (liveControl, liveControls; Cli_ProcessControlEvents and its local actions), and picking up
// what's under the cursor unless there's no room for it (tryPickup). Moved out of main.cpp as they
// were; main.cpp routes the input to them.
#include "app.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace noxsdl {

/// Picks up `netCode` (MSG_TRY_GET) unless there's no room for it (CliNet_SendPickup):
/// gold always goes; anything else needs a cell that can take one more, else the game
/// plays NoCanDo and says pickup.c:CarryingTooMuch instead.
void tryPickup(App& app, std::uint16_t netCode)
{
    auto& client = app.live->client();
    const auto& world = client.world();
    if (const auto* d = world.drawable(netCode); d && app.inventory && world.self().inventoryLoaded) {
        const std::string name = world.thingName(d->typeId);
        const bool gold = name == "Gold" || name == "QuestGoldPile" || name == "QuestGoldChest";
        if (!gold && !app.inventory->canTake(d->typeId, world, gameFlags(app))) {
            uiSound(app, 0x39D); // NoCanDo
            centredMessage(app, gameString(app, "pickup.c:CarryingTooMuch"));
            return;
        }
    }
    client.pickup(netCode);
}

/// Live: what a control does when its key is pressed (Cli_ProcessControlEvents and its
/// local actions). The held ones (MoveForward, Action, the map zoom) act every frame instead, in
/// SDL_AppIterate. Returns whether it did anything.
bool liveControl(App& app, std::string_view control)
{
    auto& client = app.live->client();
    const auto& world = client.world();
    if (control.starts_with("InvokeSlot") && control.size() == 11 && control[10] >= '1' && control[10] <= '5') {  // InvokeSlot1-5: the digit is at index 10
        castSlot(app, static_cast<std::size_t>(control[10] - '1'));
        return true;
    }
    if (control == "SelectSpellSet") {
        // client_spellSetSelect: a tap picks the first set, each tap within half a second the next (to five).
        std::uint64_t& last = app.spellSetTapMs;
        int& chosen = app.spellSetTapped;
        if (app.barsExpanded || isWarrior(app)) {
            return true;
        }
        const std::uint64_t now = SDL_GetTicks();
        if (last != 0 && now - last < 500) {
            if (++chosen > 4) {
                chosen = 4;
                last = now;
                app.bars.row = 4;
                return true;
            }
        } else {
            chosen = 0;
        }
        last = now;
        selectSpellSet(app, chosen);
        return true;
    }
    if (control == "PlaceTrapBomber") {
        buildTrap(app); // T (Cli_ProcessControlEvents' 0x2D)
        return true;
    }
    if (control == "ToggleBook") {
        if (app.book) {
            app.book->toggle(bookContext(app)); // client_toggleSpellbook
        }
        return true;
    }
    if (control == "PreviousSpellSet" || control == "NextSpellSet") {
        turnSpellSet(app, control == "NextSpellSet" ? 1 : -1);
        return true;
    }
    if (control == "QuickHealth" || control == "QuickMana" || control == "QuickCurePoison") {
        drinkPotion(app, control == "QuickHealth" ? 0 : control == "QuickMana" ? 1 : 2);
        return true;
    }
    if (control == "ToggleInventory") {
        if (app.inventory) {
            app.inventory->toggle(); // (with its open / close sounds)
        }
        return true;
    }
    if (control == "ToggleMap") {
        if (app.minimap) {
            app.minimap->toggle();
            uiSound(app, app.minimap->shown() ? 0x310 : 0x311); // MapOpen / MapClose
        }
        return true;
    }
    if (control == "Jump") {
        // Jump; or leave observer mode, or respawn, when not playing (the Action it sends then).
        playing(client) ? client.jump() : client.spawn();
        return true;
    }
    if (control == "IncreaseWindowSize" || control == "DecreaseWindowSize") {
        stepWindowSize(app, control == "IncreaseWindowSize" ? 2 : -2);
        uiSound(app, 0x399); // ShellClick (Cli_ProcessControlEvents)
        return true;
    }
    if (control == "ToggleConsole") {
        if (app.console) {
            app.console->toggle(app.window);
        }
        return true;
    }
    if (control == "Chat" || control == "TeamChat") {
        if (app.chat) {
            app.chat->open(app.window, control == "TeamChat"); // (Enter sends, Escape drops it)
        }
        return true;
    }
    if (control == "IncreaseGamma" || control == "DecreaseGamma") {
        // The gamma level 1-10. Kept in noxcore.cfg, not nox.cfg: the
        // original's Gamma often did nothing on today's Windows, so players' files carry values
        // that would brighten everything here; noxsdl starts at 1 (no change) until it's set.
        app.gamma = std::clamp(app.gamma + (control == "IncreaseGamma" ? 1 : -1), 1, 10);
        uiSound(app, 0x399); // ShellClick
        saveExtras(app, currentExtras(app));
        return true;
    }
    if (control == "ScreenShot") {
        app.screenshotWanted = true; // (taken once the frame's drawn)
        return true;
    }
    if (control == "ToggleNetstat") {
        app.netStat.toggle();
        uiSound(app, 0x399);
        return true;
    }
    if ((control == "AutoSave" || control == "AutoLoad") && soloGame(app) && app.host) {
        // Cli_ProcessControlEvents' 0x36 / 0x37 (a solo game's, out of a widescreen): when
        // the exe's check allows, ShellClick and AUTOSAVE asked for, or the question to load
        // it (cinematics.hpp); else the 0xE7 sound.
        if (!app.host->mayOpenMenu()) {
            uiSound(app, 0xE7);
        } else if (control == "AutoSave") {
            uiSound(app, 0x399);
            app.host->requestSave("AUTOSAVE");
        } else if (app.cinematics) {
            app.cinematics->autoLoadKey();
        }
        return true;
    }
    if (control == "ToggleServerMenu") {
        // Cli_ProcessControlEvents' 0x2D in a network game (flag 0x2000): the server menu
        // (guiServerOptsLoad) opened or closed; the host's (a guest's read-only view
        // and a quest's settings: not yet).
        if ((world.game().gameFlags & 0x2000) && !(world.game().gameFlags & 0x1000) && app.host && !app.host->directed() && app.serverOptions) {
            app.serverOptions->toggle();
        }
        return true;
    }
    if (control == "AutoSave" || control == "AutoLoad") {
        // The solo game's quick save and load (flag 0x800): nothing for a network client.
        return true;
    }
    if (control == "ToggleGUI") {
        app.renderGui = !app.renderGui; // (the game won't under engine flag 0x40000)
        return true;
    }
    if (control == "Taunt" || control == "Laugh" || control == "Point") {
        // MSG_SOCIAL; the server plays the animation and sound. The game allows a taunt or a
        // point a second and a laugh every two (checkKeybTimeout).
        using Social = nox::client::HeadlessClient::Social;
        const Social kind = control == "Taunt" ? Social::Taunt : control == "Laugh" ? Social::Laugh : Social::Point;
        auto& last = app.socialSentMs;
        const std::uint64_t now = SDL_GetTicks();
        const std::uint64_t every = kind == Social::Laugh ? 2000 : 1000;
        if (const auto it = last.find(kind); it == last.end() || now - it->second >= every) {
            last[kind] = now;
            client.social(kind);
        }
        return true;
    }
    if (control == "SwapWeapons") {
        if (client.swapWeapons()) { // client_invAlterWeapon
            uiSound(app, 0x37F);    // NextWeapon
        }
        return true;
    }
    if (control == "ToggleRank") {
        // The scoreboard's next mode; quest games have the quest list. Only in a
        // network game (Cli_ProcessControlEvents makes the event under game flag 0x2000: never
        // in a solo game).
        if (!(world.game().gameFlags & 0x2000)) {
            return true;
        }
        if (app.rankBoard) {
            app.rankBoard->cycle((world.game().gameFlags & 0x1000) != 0, app.online);
        }
        uiSound(app, 0x399); // ShellClick
        return true;
    }
    // Every control is handled (ToggleServerMenu, AutoSave and AutoLoad do nothing for a network
    // client). Held, in SDL_AppIterate: MoveForward, Action, MapZoomIn / Out, InvertSpellTarget.
    return false;
}

/// The controls a key or button press fired; whether there were any (a bound key does nothing
/// else). ToggleQuitMenu (Escape) opens the in-game menu, which then has the input.
bool liveControls(App& app, const std::vector<std::string>& controls)
{
    if (app.cinematics && std::ranges::any_of(controls, [&](const std::string& c) { return app.cinematics->refuses(c); })) {
        // What a widescreen or a fade refuses (cinematics.hpp) goes no further.
        std::vector<std::string> kept;
        std::ranges::copy_if(controls, std::back_inserter(kept), [&](const std::string& c) { return !app.cinematics->refuses(c); });
        liveControls(app, kept);
        return true;
    }
    if (std::ranges::find(controls, "ToggleQuitMenu") != controls.end() && app.quitMenu) {
        // Escape (consoleEsc) puts down a spell waiting for its target; else
        // closes the all-sets view and the book; only with neither open the in-game menu.
        if (app.armed || app.barDrag) {
            app.armed.reset();
            app.barDrag.reset();
            return true;
        }
        // An award flying to the bars lands at once, and nothing else.
        if (app.flight && !app.quitMenu->isOpen()) {
            landFlightAtOnce(app);
            return true;
        }
        // The shop's Exit or the dialog's Done (CliNet_SendShopExit, SendDialogDone
        // CliNet_SendDialogDone), before the menu.
        if (app.tradeScreens && !app.quitMenu->isOpen() && app.tradeScreens->escape()) {
            return true;
        }
        bool closed = false;
        if (app.barsExpanded && !app.quitMenu->isOpen()) {
            expandBars(app, false);
            closed = true;
        }
        if (app.book && app.book->isOpen() && !app.quitMenu->isOpen()) {
            app.book->hide();
            closed = true;
        }
        if (app.inventory && app.inventory->isOpen() && !app.quitMenu->isOpen()) {
            app.inventory->toggle(); // (closed, with its sound)
            closed = true;
        }
        if (closed) {
            return true;
        }
        if (!app.quitMenu->isOpen() && app.host) {
            // The menu opens only when the exe's check allows (a solo game's host free to
            // save, solo.hpp hostMayOpenMenu); else the 0xE7 sound.
            if (!app.host->mayOpenMenu()) {
                uiSound(app, 0xE7);
                return true;
            }
        }
        app.quitMenu->toggle();
        app.running = false;
        app.live->client().setAttack(false);
        app.live->client().move(std::nullopt);
        if (app.input) {
            app.input->releaseAll();
        }
        return true;
    }
    for (const auto& control : controls) {
        liveControl(app, control);
    }
    return !controls.empty();
}

} // namespace noxsdl
