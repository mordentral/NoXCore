// The Westwood Online logins a player has saved. WOLAPI.dll (the game's online login screen,
// wolapi.wnd) keeps them in HKEY_CLASSES_ROOT: keys Wchat\Nick<n>\Nick and Wchat\Nick<n>\Pass,
// each holding its string as the key's default value (WOLAPI's SetNick /
// GetNick), the password as apgar encodes it (lobby/xwis.hpp). Slots 1-128 are the login
// list; slot 129 is the one used for a login that isn't kept. A joining client sends the nick as
// its account name, as the game does when its player is logged in.
//
// Older guesses put them under SOFTWARE\Westwood\WOLAPI\Wchat (and its VirtualStore copies);
// savedOnlineNicks still reads those.
#pragma once

#include <string>
#include <vector>

namespace nox::io {

/// The login list's slots (Wchat\Nick1 to Nick128).
inline constexpr int kSavedLoginSlots = 128;
/// The slot of a login that isn't kept (Wchat\Nick129: the game logs in through it).
inline constexpr int kUnsavedLoginSlot = 128;

struct SavedLogin {
    std::string nick;
    std::string password; ///< apgar-encoded; empty if the password wasn't saved
};

/// The slots 1-129 (index 0 is Wchat\Nick1), empty where nothing is saved; none off Windows.
std::vector<SavedLogin> savedLogins();

/// Writes slot `index` (0-128) for this user (HKEY_CURRENT_USER\Software\Classes, which
/// HKEY_CLASSES_ROOT shows the game too). False if it couldn't, or off Windows.
bool saveLogin(int index, const SavedLogin& login);

/// The saved nicks of the login list, in its order (the first is the one the login screen
/// offers first); empty when there are none, or off Windows.
std::vector<std::string> savedOnlineNicks();

} // namespace nox::io
