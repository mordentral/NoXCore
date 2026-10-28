// The in-game HUD as the game draws it, from its named images: the health and mana meters in the
// bottom right (guimeter.c, guiHealthManaInit) and the spell quickbar along the
// bottom middle (guispell.c, quickBarCreate). Laid out in the game's pixels
// from the screen's edges, as the exe does at its resolution, and scaled up for bigger windows.
//
//   meters     a 91 x 201 frame (HealthMana1-10, the frames of its flash when hurt) at the
//              bottom right; the tubes over it (HealthManaTubes, a warrior's WarriorHealthTube),
//              the poison tube when poisoned; each tube a 15 x 125 column filled from the bottom
//              in red or blue, the empty part darkened, bubbles rising in it (guiHealthManaTubeDraw); under
//              them the potion slots (cure poison, health, mana) with what each holds, how many
//              (the first stack's), and their keys (guiBottleSlotDrawFn; the health slot a red
//              potion, else meat, else an apple: bottle_slots.hpp). Clicking a tube toggles the small bars beside our
//              player (guiHealthManaTubeProc, drawHealthManaBar); clicking a potion slot drinks one.
//   quickbar   320 wide, centred, 74 from the bottom: QuickBarBase with its five 34 x 34 slots
//              (38, 37, 37, 37 apart), the left end (QuickBarSpellSetBase / QuickBarWarriorLeft
//              with the spellbook button) and right end (QuickBarTrap / QuickBarBomber /
//              QuickBarWarriorRight), the key plates (QuickBarNugget1-5), the slots' keys under
//              them, and the spell set's name. Spell icons dim while the spell can't be cast
//              and in part while mana is short (quickBarWarriorDraw: the top (cost - mana) / cost of it);
//              ability icons darken from the top while they recharge, the clear part rising
//              as the cooldown runs (quickBarDrawFn: 34 - elapsed / (gamedata.bin's *Delay / 30)
//              pixels from when the server said it went unready). Not in the game: a spell
//              the server would refuse while our last one is out dims red (spell_limits.hpp).
//
// What fills the slots: the character's spell bars (spell_bars.hpp), filled from the spellbook
// and kept in its .plr: five sets of five, the one in use paged with W and E, the mouse wheel,
// or the arrows by the spellbook button; a warrior's abilities. Each slot's nugget above it is
// lit while the spell goes on its caster; clicking the nugget's corner turns it. The
// button under the arrows shows all five sets at once, stacked 60 apart above the bar, the last
// in the bar itself. An icon lights for 10 frames when its slot is used.
//
//   traps      a wizard's trap bar, a conjurer's bomber bar (guispell.c): on the bar's right
//              end QuickBarTrap / QuickBarBomber (QuickBarWarriorRight until the Glyph is
//              known in a quest: it slides off and comes back changed when it's learned,
//              quickbarAddTrap, quickbarDrawFn), its button building the trap or bomber from the trap set
//              in use (T too, client_buildTrap), and the small button above it opening the tray
//              (QuickBarTrapTray): three trap sets of three slots with arrows and
//              "Trap Set n", over the bar's middle. A trap slot shades by the mana left once
//              the slots before it are paid at their trap costs (gamedata.bin's *TrapCost).
//   CTF flags  in a CTF game (GUI_CTF.wnd, Gui_Ctf_Show), left of the meters: a 32 x 32 icon
//              per team's flag, red (team 1) over blue (team 2) 40 apart, the lower one's top
//              40 up from the bottom and their left 131 in from the right. Each shows its flag
//              at home, carried or dropped (FlagAtHome / FlagCarried / FlagDroppedInField +
//              colour, by MSG_REPORT_FLAG_STATUS); our own team's has FlagTeamBorder round it
//              (4 out each way). Their tooltips say whose flag is where (gui_ctf.c's *TT).
#pragma once

#include "bottle_slots.hpp"
#include "sprite_cache.hpp"
#include "text.hpp"
#include "thing_art.hpp"

#include "noxcore/client/spell_bars.hpp"
#include "noxcore/client/world.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/game_data.hpp"
#include "noxcore/things/thing_db.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace noxsdl {

/// What a click on the HUD hit.
struct HudHit {
    enum class Kind {
        Slot,
        Target,
        SetUp,
        SetDown,
        SetMax,
        TrapSlot,   ///< a slot of the trap tray
        TrapUp,     ///< the tray's previous trap set (client_trapSetPrev)
        TrapDown,   ///< its next (client_trapSetNext)
        TrapToggle, ///< the small button opening and closing the tray (quickbarTrapProc)
        TrapBuild,  ///< the trap / bomber button (quickbarTrapButtonProc)
        Tube,
        Potion,
        Spellbook,
        Flag,       ///< a CTF flag icon (GUI_CTF.wnd): a tooltip, no click
        Panel
    } kind = Kind::Panel;
    int index = 0; ///< the slot, the potion (0 health, 1 mana, 2 cure poison), or the flag icon (team id - 1)
    int set = 0;   ///< a slot's or nugget's spell set
};

/// The quickbar's state from outside the HUD.
struct QuickbarState {
    const nox::client::SpellBars* bars = nullptr;
    bool expanded = false;      ///< all five sets showing
    int lastSlot = -1;          ///< the slot last used (clientStoreLastButton), its icon lit for 10 frames
    std::uint32_t lastFrame = 0;
    bool bookOpen = false;      ///< the spellbook button lit
    int flash = -1;             ///< an arrow or button just clicked (0 up, 1 down, 2 all sets, 3 / 4 trap sets)
    bool trapAvailable = false; ///< the trap / bomber end shown (else QuickBarWarriorRight)
    bool trapOpen = false;      ///< the tray showing
    bool trapHit = false;       ///< the trap / bomber button lit (just used)
    float trapSlide = 0;        ///< the right end's drop, sliding off and back (game pixels)
    /// noxcpp's multiclass, a mode's: a caster's meters and bars whatever the class, ability
    /// slots (tagged ids, nox::client::kAbilityTag) among the spells.
    bool multiclass = false;
    /// The meters' bottle slots (the inventory's): what each holds and how many it shows.
    const BottleSlots* bottles = nullptr;
};

class Hud {
public:
    Hud(const nox::things::ThingDb& things, ThingArt& art);

    /// A spell's icon (spellIcon; `lit`, the one shown just after it's used),
    /// or for a warrior an ability's (spellGetAbilityIcon).
    std::optional<nox::things::ImageRef> icon(std::uint32_t id, bool warrior, bool lit = false) const;

    /// Draws a spell's (a warrior: ability's) icon being dragged: at the mouse (x, y), in
    /// output pixels, less 15 game pixels each way (client_drawCursorAndTooltips).
    void drawDragged(SDL_Renderer* renderer, SpriteCache& sprites, std::uint32_t id, bool warrior, float x, float y,
                     int w, int h) const;

    /// Output pixels per game pixel on a w x h screen.
    static float scale(int w, int h);
    /// A slot's top left in game pixels on a w x h screen: set `set`'s box (the bar itself unless
    /// the sets are all showing and it's one of the first four).
    static SDL_FPoint slotPosition(int w, int h, int set, int slot, bool expanded, int currentSet);

    /// Draws the HUD on a w x h screen. `frame` is the game frame (animations, bubbles).
    void draw(SDL_Renderer* renderer, Font* font, SpriteCache& sprites, const nox::client::World& world,
              std::uint16_t us, const QuickbarState& bar, int w, int h, std::uint32_t frame);

    /// What's at output pixel (x, y), if the HUD is there.
    std::optional<HudHit> hit(const nox::client::World& world, std::uint16_t us, const QuickbarState& bar, float x,
                              float y, int w, int h) const;
    /// The slot at output pixel (x, y) (spellBoxPointToWnd over the showing boxes), as (set, slot).
    std::optional<std::pair<int, int>> slotAt(const QuickbarState& bar, float x, float y, int w, int h) const;
    /// The trap tray's slot at output pixel (x, y), if the tray is open.
    std::optional<int> trapSlotAt(const QuickbarState& bar, float x, float y, int w, int h) const;
    /// gamedata.bin, for the trap costs and the abilities' cooldowns (nullptr: none known).
    void setGameData(const nox::things::GameData* data) { gameData_ = data; }
    /// The game's flags, which of gamedata.bin's lines it reads (SOLO in a solo game, ARENA online).
    void setGameFlags(std::uint32_t gameFlags) { gameFlags_ = gameFlags; }
    /// nox.csf, for the tooltips.
    void setStrings(const nox::strings::StringTable* strings) { strings_ = strings; }
    /// The tooltip for what's at output pixel (x, y), if the bars or meters are there: a slot's
    /// spell or ability title, the nuggets' CastOnMe / CastAtOther, the buttons' and arrows'
    /// (guispell.c's ToolTip*, *SpellbookTT), the tubes' and potion slots' (guimeter.c).
    std::string tooltip(const nox::client::World& world, std::uint16_t us, const QuickbarState& bar, float x, float y,
                        int w, int h) const;
    /// What a spell costs in a trap (spellManaCost mode 2): Energy Bolt, Chain
    /// Lightning and Mana Bomb by gamedata.bin's *TrapCost, the rest as cast.
    int trapCost(std::uint32_t spell) const;
    /// Whether output pixel (x, y) is in the bar's area (dropping a slot's spell there
    /// keeps it).
    static bool inBarArea(float x, float y, int w, int h);
    /// Whether the game lets us cast `spell` (spellIsEnabled: the game's spell
    /// settings).
    static bool spellAllowed(const nox::client::World& world, std::uint32_t spell);

    /// A tube was clicked: the bars beside the player on or off.
    void toggleBars() { barsShown_ = !barsShown_; }
    /// The bars beside the player are up (the game's g_guiTubesVisible): then quest games also
    /// show the other players' health bars.
    bool barsShown() const noexcept { return barsShown_; }
    /// Spells the server would refuse because our last one is still out (spell_limits.hpp):
    /// their icons are dimmed red, which the game doesn't do (it fails the cast without a sign).
    void setLimited(std::set<std::uint32_t> spells) { limited_ = std::move(spells); }
    /// A named image (thing.bin's images), for pictures drawn beside the HUD (the game-over
    /// banner's).
    const nox::things::ImageDef* image(const std::string& name) const;

private:
    struct Bubble {
        int x = 0;
        int y16 = 0; ///< from the top, in 16ths
        int speed = 0;
        int size = 0;
        bool live = false;
        SDL_Color color{};
    };

    int manaCost(std::uint32_t spell) const;

    const nox::things::ThingDb& things_;
    ThingArt& art_;
    std::map<std::string, const nox::things::ImageDef*, std::less<>> images_;
    std::array<std::array<Bubble, 64>, 2> bubbles_{};
    std::uint32_t bubbleFrame_ = 0;
    std::uint32_t seed_ = 12345;
    std::uint16_t lastHealth_ = 0;
    int flashTimer_ = 0;
    int flashAcc_ = 0;
    bool barsShown_ = false;
    std::set<std::uint32_t> limited_;
    const nox::things::GameData* gameData_ = nullptr;
    std::uint32_t gameFlags_ = 0; ///< setGameFlags
    const nox::strings::StringTable* strings_ = nullptr;
    std::string text(const std::string& id, const char* file) const;
};

} // namespace noxsdl
