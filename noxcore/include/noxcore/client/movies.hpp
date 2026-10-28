// When and how GAME.EXE plays its movies (the client's side; the decoding is video/vqa.hpp).
//
//   start-up   game_switchStates's state 10: game_rollLogoAndStart plays
//              WWLogo.vqa, then game_rollIntroAndStart Intro.vqa (only while nox.cfg's
//              TrackData lacks 0x80, which it then sets: the intro is seen once), then
//              game_rollNoxLogoAndStart NoxLogo.vqa and the legal screen
//              (game_showLegal), then the main menu. Each is skipped under game flag
//              0x2000000 or when its file isn't there (and the chain goes on).
//   main menu  its Intro button (windowMainMenuProc's 0x79): the menu slides out, Intro.vqa plays
//              (client_drawGeneralCallback: the player with 0), and the menu comes back
//              (game_switchStates).
//   endings    EndGame keeps the class; the main menu plays
//              <Class>.vqa, then the credits.
//   playing    the file queued, then the callback, the player (with `skip`) plays it
//              unless engine flag 0x200 (-safe) or Shift is held, then clears the
//              screen and runs the callback. The game's sound stops meanwhile (the
//              music held, the speech stopped).
//   skipping   the window procedure on WM_KEYDOWN (auto-repeats too; not
//              WM_SYSKEYDOWN, Alt's and F10's): Escape is the stop check with 1, any
//              other key with 0; WM_LBUTTONDOWN with 0. The check with n stops the
//              movie if it was played with 0 (the logos, and Intro from the main menu) or n is 1:
//              the start-up's Intro and the endings stop only for Escape.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace nox::client::movies {

/// The player's argument.
enum class Skip : unsigned char {
    AnyInput = 0,   ///< a key or the left button stops it
    EscapeOnly = 1, ///< only Escape does
};

enum class Input : unsigned char { Escape, OtherKey, LeftButton };

/// The stop check, as the window procedure calls it.
bool skips(Input input, Skip how);

/// A key press as the window procedure sees it: Windows sends WM_KEYDOWN for it (Escape or another key),
/// auto-repeats included, unless it's a WM_SYSKEYDOWN (`systemKey`: F10, Alt itself, or a key
/// pressed while Alt is held without Ctrl), which the procedure passes on unseen.
std::optional<Input> keyInput(bool escape, bool systemKey);

/// game_rollIntroAndStart: the intro plays at start-up (TrackData's 0x80 not yet set,
/// game flag 0x2000000 off).
bool introAtStartup(int trackData, bool gameFlag2000000 = false);
/// TrackData once the intro has played (set to its value | 0x80), a byte.
int trackDataAfterIntro(int trackData);
/// TrackData as the exe keeps it, a byte: nox.cfg's number read as atoi's low
/// byte and written back & 0xFF.
int trackDataByte(int value);

/// the ending movie for EndGame's `kind` (Warrior, Wizard, Conjurer, plus
/// ".vqa"); empty for none (-1, or out of the table).
std::string endingMovie(int kind);

/// game_setMovieFile: movies\<name> in the game folder (here matched without regard
/// to case; the exe's CD fallback, <CD>:\Install\movies\, isn't looked at).
std::optional<std::filesystem::path> findMovie(const std::filesystem::path& gameDir, std::string_view name);

/// The HD version (noxsdl's, not the exe's): <hdDir>\<name's stem>.mkv, matched without regard
/// to case.
std::optional<std::filesystem::path> findHdMovie(const std::filesystem::path& hdDir, std::string_view name);

} // namespace nox::client::movies
