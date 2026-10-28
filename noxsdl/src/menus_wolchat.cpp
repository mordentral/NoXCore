// The Westwood Online chat lobby (state 1900, wolchat.c: its screen and its window
// procedure) and the WOL connection the login keeps for it and the game list (lobby/wol_chat.hpp):
//
//   opening   wolchat.wnd from 480 above; the lists' sliders and arrows tied to them, tab stops
//             150 apart; the message of the day in the messages; "Messages -- <nick>" over them;
//             every channel listed (RequestChannelList(-99999), "Downloading channel list..."),
//             then the first lobby with fewer than 25 users joined, else a random one
//             ("Updating channel list...").
//   lobbies   the "Lob_37_<n>" channels by their names (WolChat.c:Nox_Lobby_*), official ones
//             first, with their user counts; a click joins another (leaving ours first).
//   users     ops first (marked "@"), then by name; a click picks a user or lets one go, and
//             what's typed goes to the users picked, privately, instead of the lobby.
//   chat      Enter says the line, Emote acts it; what's said in the lobby, to us, and its comings
//             and goings in the messages, each kind in its colour (0x400D's palette).
//   Refresh   lists the channels and the lobby's users again. Games: the lobby left (PART), on to
//             the game list, which lists XWIS through this connection (state 8's "LIST 37 37").
//             Back and Escape: logged off, back to the login.
//
// Not yet: Find/Page (wolfind.wnd), Options (wolopt.wnd), the server list (servlist.wnd), a user's
// right-click menu (ignore, kick, ban, op); Clans and Ladder are off (Westwood's pages are gone).
#include "menus.hpp"

#include "console.hpp"

#include <algorithm>

namespace noxsdl {

namespace st = nox::gui::status;
using Session = nox::lobby::WolChatSession;

namespace {

// wolchat.wnd
constexpr std::int32_t kChatFindPage = 1902, kChatRefresh = 1903, kChatOptions = 1904, kChatGames = 1905,
                       kChatBack = 1906, kChatMessages = 1907, kChatChannels = 1908, kChatUsers = 1909,
                       kChatEntry = 1910, kChatAction = 1913, kChatServer = 1914, kChatTitle = 1916,
                       kChatClans = 1926, kChatLadder = 1927;
/// Each list with its slider, up and down arrows (ties them to it).
struct ListParts {
    std::int32_t list, slider, up, down;
};
constexpr ListParts kChatLists[3] = {
    {kChatMessages, 1917, 1918, 1919}, {kChatChannels, 1920, 1921, 1922}, {kChatUsers, 1923, 1924, 1925}};
// The lines' colours (0x400D's palette indices).
constexpr int kSystemColor = 9;   ///< the joins, leaves and kicks
constexpr int kPublicColor = 6;   ///< what's said in the lobby
constexpr int kPrivateColor = 7;
constexpr int kActionColor = 13;  ///< emotes
constexpr int kListColor = 14;    ///< the lobbies and users
constexpr int kSquelchedColor = 2;

nox::gui::Color palette(int index)
{
    const SDL_Color c = paletteColor(static_cast<std::uint8_t>(index));
    return {c.r, c.g, c.b, false};
}

const nox::gui::ListBoxData* listData(const gui::Window& list)
{
    return std::get_if<nox::gui::ListBoxData>(&list.data);
}

// A line's height plus a pixel's gap: its own (wrapped) height, else the taller of the font's and the list's line height.
int lineStep(const gui::Window& list, const gui::Window::Line& l)
{
    const auto* d = listData(list);
    const int fh = list.largeFont ? 16 : 10;
    return (l.height > 0 ? l.height : std::max(fh, d ? static_cast<int>(d->lineHeight) : 0)) + 1;
}

/// The furthest a list scrolls: its last lines just fill it.
int mostScroll(const gui::Window& list)
{
    // The list's height less its title line, if it has one.
    int room = list.rect.h - (list.text.empty() ? 0 : (list.largeFont ? 16 : 10) + 1) + 1;
    int first = static_cast<int>(list.lines.size());
    // Count back from the end while whole lines still fit; where that stops is the first line shown.
    while (first > 0) {
        const int h = lineStep(list, list.lines[static_cast<std::size_t>(first - 1)]);
        if (h > room) {
            break;
        }
        room -= h;
        --first;
    }
    return first;
}

/// A list line inserted (0x4012's place, then 0x400D), the selection kept on its line.
void insertLine(gui::Window& list, int at, std::string text, int color)
{
    at = std::clamp(at, 0, static_cast<int>(list.lines.size()));
    list.lines.insert(list.lines.begin() + at, {std::move(text), palette(color)});
    if (list.selectedLine >= at) {
        ++list.selectedLine;
    }
}

/// 0x400E: a list line gone, the selection kept on its line (none if it was that one).
void eraseLine(gui::Window& list, int at)
{
    if (at < 0 || at >= static_cast<int>(list.lines.size())) {
        return;
    }
    list.lines.erase(list.lines.begin() + at);
    if (list.selectedLine == at) {
        list.selectedLine = -1;
    } else if (list.selectedLine > at) {
        --list.selectedLine;
    }
}

} // namespace

void Menus::setupChat()
{
    // The screen: wolchat.wnd from 480 above (in at 20 a frame, out at 40); modal, its own
    // Back.
    screen_ = gui_.load(windowDir_ / "wolchat.wnd", [this](gui::Window&, int message, gui::Window& from) {
        return onChat(message, from);
    });
    slides_.push_back(gui_.slideIn(screen_, {0, -480}, {0, 20}, {0, -40}));
    optsBack_->show(false);
    wolState_ = 7;
    for (const auto& parts : kChatLists) {
        if (gui::Window* list = screen_->find(parts.list)) {
            list->tabWidth = 150; // (draw_setTabWidth(0x96))
        }
        // The slider's thumb ShopInventorySlider (lit: ...Selected), 16 by 46.
        if (gui::Window* slider = screen_->find(parts.slider)) {
            slider->enabledImage = "ShopInventorySlider";
            slider->hiliteImage = "ShopInventorySliderSelected";
            slider->status |= st::Image;
            slider->thumbLength = 46;
        }
        scrollList(parts.list, 0);
    }
    for (const auto id : {kChatFindPage, kChatOptions, kChatServer, kChatClans, kChatLadder}) {
        wip(screen_, id);
    }
    // The message of the day: in no lobby yet.
    for (const auto& line : nox::lobby::wolMotdLines(wolMotd_)) {
        chatLine(line, kSystemColor);
    }
    wolLists_.clearLobbies();
    wolLists_.clearUsers();
    chatTitle();
    focusChatEntry();
    if (!wol_ && wolLogin_) {
        // (The connection lost since: logged in again.)
        wol_.emplace(wolLogin_->nick, wolLogin_->password, static_cast<std::uint32_t>(nowMs_));
    }
    if (wol_ && !wol_->session().channel().empty()) {
        wol_->session().requestChannelLeave(); // (back from the game list)
    }
    // The channel list asked for (-99999, 1): every channel, "Downloading channel list..." (0x120: no buttons).
    wolListWanted_ = nox::lobby::kWolAllChannels;
    dialog(gui_.text("wolchat.c:PleaseWait"), gui_.text("wolchat.c:LoadingChannels"), 0x120, {});
}

void Menus::focusChatEntry()
{
    // The entry field has the typing unless a message box is up.
    if (!dialog_ && screen_) {
        gui_.setFocus(screen_->find(kChatEntry));
    }
}

void Menus::closeDialog()
{
    if (dialog_) {
        gui_.destroy(dialog_);
        dialog_ = nullptr;
    }
    dialogKeys_ = nullptr;
    dialogKeysOf_ = nullptr;
}

std::string Menus::lobbyText(int index) const
{
    // Its name, " %d" after it past the 53rd.
    const auto& lobbies = wolLists_.lobbies();
    if (index < 0 || index >= static_cast<int>(lobbies.size())) {
        return {};
    }
    const auto& n = lobbies[static_cast<std::size_t>(index)].name;
    std::string text = gui_.text("WolChat.c:Nox_Lobby_" + std::to_string(n.nameIndex));
    if (n.round > 1) {
        text += " " + std::to_string(n.round);
    }
    return text;
}

void Menus::chatTitle(bool inLobby)
{
    // "Messages -- <nick>, You are in the <lobby> channel." (WolChat.c:messageTitle),
    // or WolChat.c:NoChannel and no lobby picked in the list.
    gui::Window* title = screen_ ? screen_->find(kChatTitle) : nullptr;
    if (!title) {
        return;
    }
    const std::string nick = wolLogin_ ? wolLogin_->nick : std::string();
    const int at = inLobby ? wolLists_.current() : -1;
    if (at < 0) {
        title->text = format("WolChat.c:NoChannel", {nick});
        if (gui::Window* channels = screen_->find(kChatChannels)) {
            channels->selectedLine = -1;
        }
    } else {
        title->text = format("WolChat.c:messageTitle", {nick, lobbyText(at)});
    }
}

void Menus::chatLine(const std::string& text, int color)
{
    // 0x400D on the messages: at the end; a full list (128) drops its first line, and it follows
    // its end (its DATA's first two values). A line too wide wraps.
    gui::Window* list = screen_ ? screen_->find(kChatMessages) : nullptr;
    if (!list || text.empty()) {
        return;
    }
    const auto* d = listData(*list);
    if (d && list->lines.size() >= d->lines) {
        // values[1] 0: a full list takes no more lines.
        if (d->values[1] == 0) {
            return;
        }
        eraseLine(*list, 0);
    }
    gui::Window::Line line{text, palette(color)};
    gui::Painter p(nullptr, gui_.resources(), 640, 480);
    if (p.textWidth(text, list->largeFont) > list->rect.w - 7) {
        line.height = std::max(10, p.textHeight(text, list->largeFont, list->rect.w - 7));
    }
    list->lines.push_back(std::move(line));
    // values[0]: the list scrolls to show the new line.
    if (!d || d->values[0] != 0) {
        scrollList(kChatMessages, mostScroll(*list));
    }
}

void Menus::scrollList(std::int32_t id, int first)
{
    gui::Window* list = screen_ ? screen_->find(id) : nullptr;
    if (!list) {
        return;
    }
    const int most = mostScroll(*list);
    list->scroll = std::clamp(first, 0, most);
    for (const auto& parts : kChatLists) {
        if (parts.list != id) {
            continue;
        }
        // The slider (its maximum at the top) follows.
        if (gui::Window* slider = screen_->find(parts.slider)) {
            if (const auto* sl = std::get_if<nox::gui::SliderData>(&slider->data)) {
                slider->sliderValue = gui::sliderFromScroll(*sl, list->scroll, most);
            }
        }
    }
}

void Menus::fillLobbies()
{
    // State 7's event 10: the list made again, nothing picked.
    gui::Window* list = screen_ ? screen_->find(kChatChannels) : nullptr;
    if (!list) {
        return;
    }
    list->lines.clear();
    list->selectedLine = -1;
    for (int i = 0; i < static_cast<int>(wolLists_.lobbies().size()); ++i) {
        list->lines.push_back({lobbyText(i) + "\t" + std::to_string(wolLists_.lobbies()[static_cast<std::size_t>(i)].users),
                               palette(kListColor)});
    }
    scrollList(kChatChannels, 0);
}

void Menus::lobbyLine(int index)
{
    // "<name>\t<users>" put in at its place, picked if it's ours.
    gui::Window* list = screen_ ? screen_->find(kChatChannels) : nullptr;
    if (!list || index < 0 || index >= static_cast<int>(wolLists_.lobbies().size())) {
        return;
    }
    insertLine(*list, index,
               lobbyText(index) + "\t" + std::to_string(wolLists_.lobbies()[static_cast<std::size_t>(index)].users),
               kListColor);
    if (index == wolLists_.current()) {
        list->selectedLine = index; // (0x4013: and scrolled to)
        if (index < list->scroll) {
            scrollList(kChatChannels, index);
        }
    }
}

void Menus::lobbyUsersChanged(int delta)
{
    // Our lobby's count changed by `delta`; its line taken out and put back,
    // or just taken out if it emptied.
    bool removed = false;
    const int at = wolLists_.changeCurrentUsers(delta, removed);
    if (at < 0) {
        return;
    }
    if (gui::Window* list = screen_ ? screen_->find(kChatChannels) : nullptr) {
        eraseLine(*list, at);
    }
    if (!removed) {
        lobbyLine(at);
    }
}

void Menus::fillUsers()
{
    // State 7's event 0xB: the list made again, no one picked.
    gui::Window* list = screen_ ? screen_->find(kChatUsers) : nullptr;
    if (!list) {
        return;
    }
    list->lines.clear();
    list->selectedLine = -1;
    for (int i = 0; i < static_cast<int>(wolLists_.users().size()); ++i) {
        const auto& u = wolLists_.users()[static_cast<std::size_t>(i)];
        // ("%c%s": '@' an op, else a space.)
        list->lines.push_back({std::string(1, (u.flags & Session::User::kOp) ? '@' : ' ') + u.name,
                               palette((u.flags & Session::User::kSquelched) ? kSquelchedColor : kListColor)});
    }
    scrollList(kChatUsers, 0);
}

void Menus::userLine(int index)
{
    // The user put in at its place.
    gui::Window* list = screen_ ? screen_->find(kChatUsers) : nullptr;
    if (!list || index < 0 || index >= static_cast<int>(wolLists_.users().size())) {
        return;
    }
    const auto& u = wolLists_.users()[static_cast<std::size_t>(index)];
    insertLine(*list, index, std::string(1, (u.flags & Session::User::kOp) ? '@' : ' ') + u.name,
               (u.flags & Session::User::kSquelched) ? kSquelchedColor : kListColor);
}

void Menus::removeUserLine(int index)
{
    // The line taken out (0x400E). (Its "no longer have a user selected" never shows: an original bug.)
    if (gui::Window* list = screen_ ? screen_->find(kChatUsers) : nullptr) {
        eraseLine(*list, index);
    }
}

void Menus::joinLobby(int index)
{
    // No lobby in the title, the join asked for, "Updating channel
    // list..." over the chat (no buttons; closed as we join).
    chatTitle(false);
    const auto& lobbies = wolLists_.lobbies();
    if (wol_ && index >= 0 && index < static_cast<int>(lobbies.size())) {
        wol_->session().requestChannelJoin(lobbies[static_cast<std::size_t>(index)].channel);
    }
    closeDialog();
    dialog(gui_.text("wolchat.c:PleaseWait"), gui_.text("Wolchat.c:UpdatingChannels"), 0x120, {});
}

void Menus::sendChat(bool action)
{
    // Said (or, `action`, emoted): in a lobby, to each user picked privately
    // (echoed "<Private To ...>" with the list's line, its '@' or
    // space and all), else to the lobby (echoed as ours); out of one, WolChat.c:NotJoined.
    gui::Window* entry = screen_ ? screen_->find(kChatEntry) : nullptr;
    if (!entry || entry->entry.empty() || entry->entry[0] == u'\n' || entry->entry[0] == u'\r') {
        return;
    }
    const std::string text = nox::strings::toUtf8(entry->entry);
    const std::string wire = nox::lobby::wolEncodeText(entry->entry);
    const std::string nick = wolLogin_ ? wolLogin_->nick : std::string();
    if (wol_ && wol_->state() == nox::lobby::WolChat::State::Ready && wolLists_.current() >= 0) {
        Session& s = wol_->session();
        gui::Window* users = screen_->find(kChatUsers);
        const bool picked =
            users && std::any_of(users->lines.begin(), users->lines.end(), [](const auto& l) { return l.marked; });
        if (picked) {
            for (const auto& l : users->lines) {
                if (!l.marked) {
                    continue;
                }
                std::string name = l.text;
                if (!name.empty() && (name[0] == '@' || name[0] == ' ')) {
                    name.erase(0, 1);
                }
                // (a user of ours by that very name.)
                const bool known = std::any_of(wolLists_.users().begin(), wolLists_.users().end(),
                                               [&](const auto& u) { return u.name == name; });
                if (!known) {
                    continue;
                }
                if (action ? s.requestPrivateAction({name}, wire) : s.requestPrivateMessage({name}, wire)) {
                    if (action) {
                        chatLine(format("WolChat.c:PrivateToAction", {l.text, nick, text}), kActionColor);
                    } else {
                        chatLine(format("WolChat.c:PrivateTomessage", {l.text, text}), kPrivateColor);
                    }
                }
            }
            return;
        }
        if (action ? s.requestPublicAction(wire) : s.requestPublicMessage(wire)) {
            if (action) {
                chatLine(nick + " " + text, kActionColor); // (L"%S %s")
            } else {
                chatLine("<" + nick + "> " + text, kPublicColor); // (L"<%S> %s")
            }
            return;
        }
    }
    chatLine(gui_.text("WolChat.c:NotJoined"), kSystemColor);
}

bool Menus::onChat(int message, gui::Window& from)
{
    // The screen's window procedure.
    if (message == gui::Hover) {
        gui_.resources().sound(0x398); // (0x4005: ShellSelect)
        return true;
    }
    if (message == gui::SliderChanged) {
        for (const auto& parts : kChatLists) {
            if (from.id != parts.slider) {
                continue;
            }
            gui::Window* list = screen_->find(parts.list);
            const auto* sl = std::get_if<nox::gui::SliderData>(&from.data);
            if (list && sl) {
                const int most = mostScroll(*list);
                list->scroll = gui::scrollFromSlider(*sl, from.sliderValue, most);
            }
        }
        return true;
    }
    if (message == gui::EntryEntered && from.id == kChatEntry) {
        // 0x401F: said, and the field emptied (0x401E).
        sendChat(false);
        from.entry.clear();
        return true;
    }
    if (message != gui::Clicked) {
        return false;
    }
    for (const auto& parts : kChatLists) {
        // The lists' own arrows (the list made them its own: no ShellClick).
        if (from.id == parts.up || from.id == parts.down) {
            if (gui::Window* list = screen_->find(parts.list)) {
                scrollList(parts.list, list->scroll + (from.id == parts.up ? -1 : 1));
            }
            return true;
        }
    }
    switch (from.id) {
    case kChatChannels: {
        // 0x4010: a lobby clicked; out of ours into it. In none, the
        // join goes to joinLobby as 0: nothing joined (an original bug).
        const int i = from.selectedLine;
        if (i >= 0 && i < static_cast<int>(from.lines.size()) && !from.lines[static_cast<std::size_t>(i)].text.empty() &&
            i < static_cast<int>(wolLists_.lobbies().size())) {
            const int current = wolLists_.current();
            if (current < 0) {
                joinLobby(-1);
            } else if (lobbyText(i) != lobbyText(current)) {
                if (wol_) {
                    wol_->session().requestChannelLeave();
                }
                joinLobby(i);
            }
        }
        focusChatEntry();
        return true;
    }
    case kChatUsers:
        // A list that takes several (wndListboxProcWithData10): the user in or out of the ones picked.
        if (from.selectedLine >= 0 && from.selectedLine < static_cast<int>(from.lines.size())) {
            auto& l = from.lines[static_cast<std::size_t>(from.selectedLine)];
            l.marked = !l.marked;
        }
        from.selectedLine = -1;
        return true;
    case kChatMessages:
        return true;
    case kChatRefresh:
        // 0x76F: state 7's 0xC (every channel, "Downloading channel list...") and 0xD (the
        // lobby's users).
        wolListWanted_ = nox::lobby::kWolAllChannels;
        dialog(gui_.text("wolchat.c:PleaseWait"), gui_.text("wolchat.c:LoadingChannels"), 0x120, {});
        if (wol_ && wol_->state() == nox::lobby::WolChat::State::Ready) {
            wol_->session().requestUserList();
        }
        break;
    case kChatGames:
        // 0x771: out of the lobby, state 8, the chat slides out (sub-state 5)
        // and the game list comes in.
        if (wol_ && wol_->state() == nox::lobby::WolChat::State::Ready) {
            wol_->session().requestChannelLeave();
        }
        wolState_ = 8;
        leave([this] { enter(State::Browser); });
        break;
    case kChatBack:
        back(); // (0x772: logged off back to the login)
        break;
    case kChatAction:
        // 0x779: the entry's text emoted, the field emptied.
        sendChat(true);
        if (gui::Window* entry = screen_->find(kChatEntry)) {
            entry->entry.clear();
        }
        break;
    default:
        return false;
    }
    gui_.resources().sound(0x399); // ShellClick
    return true;
}

void Menus::refreshGames()
{
    // The game list's XWIS games through the WOL connection while there is one (state 8's 0xC:
    // RequestChannelList(37), "Downloading game list from server..."), else a search of its own.
    // Network's search lists no XWIS (refreshServerList's network branch: the LAN query only).
    browser_.setXwisListing(!lanOnly_);
    if (wolMode_ && wol_ && !wol_->loggingOff() && wol_->state() != nox::lobby::WolChat::State::Failed &&
        wol_->state() != nox::lobby::WolChat::State::Closed) {
        wolState_ = 8;
        wolListWanted_ = nox::lobby::kWolGameCode;
        dialog(gui_.text("wolchat.c:PleaseWait"), gui_.text("noxworld.c:LoadingGames"), 0x120, {});
        return;
    }
    browser_.refresh(nowMs_);
}

void Menus::logOffWol()
{
    // RequestLogout (state 9's); the connection let go once the server has closed it.
    wolState_ = 0;
    wolListWanted_.reset();
    if (!wol_) {
        return;
    }
    if (wol_->state() == nox::lobby::WolChat::State::Ready) {
        wol_->logOff(static_cast<std::uint32_t>(nowMs_));
    } else {
        wol_.reset();
    }
}

void Menus::pollWol()
{
    using WolState = nox::lobby::WolChat::State;
    if (wol_) {
        wol_->poll(static_cast<std::uint32_t>(nowMs_));
        // Logging in: once Ready or Failed, the login screen goes on (loginDone).
        if (wolLoggingIn_) {
            if (wol_->state() == WolState::Ready) {
                for (const auto& e : wol_->session().takeEvents()) {
                    chatEvent(e); // (the welcome's message of the day, for the chat)
                }
            }
            if (wol_->state() == WolState::Ready || wol_->state() == WolState::Failed) {
                loginDone();
            }
            return;
        }
        if (wol_->state() == WolState::Closed) {
            wol_.reset();
        } else if (wol_->state() == WolState::Failed) {
            SDL_Log("WOL: %s", wol_->error().c_str());
            wol_.reset();
            if (!joinAfterLogOff_ && !stack_.empty() && !leaving_) {
                if (stack_.back() == State::Chat) {
                    // Lost: back to the login, which says so (Wollogin.c:Discon).
                    closeDialog();
                    leave([this] {
                        // The chat and the login under it popped, then the login entered afresh.
                        stack_.pop_back();
                        stack_.pop_back();
                        enter(State::Login);
                        dialog(gui_.text("Wollogin.c:Notice"), gui_.text("Wollogin.c:Discon"), 0x21, {});
                    });
                } else if (stack_.back() == State::Browser && wolListWanted_) {
                    closeDialog();
                    browser_.refresh(nowMs_); // (a search of its own)
                }
            }
            wolListWanted_.reset();
        }
    }
    if (!wol_) {
        if (joinAfterLogOff_) {
            // Logged off: on into the game.
            closeDialog();
            const nox::client::PlayerProfile profile = *joinAfterLogOff_;
            joinAfterLogOff_.reset();
            if (callbacks_.join && !pendingHost_.empty()) {
                callbacks_.join(pendingHost_, profile);
            }
        }
        return;
    }
    if (wol_->state() != WolState::Ready || wol_->loggingOff()) {
        return;
    }
    Session& s = wol_->session();
    // A list asked for earlier goes out once nothing else is being listed.
    if (wolListWanted_ && !s.listing() && s.requestChannelList(*wolListWanted_)) {
        wolListWanted_.reset();
    }
    for (const auto& e : s.takeEvents()) {
        chatEvent(e);
    }
}

void Menus::chatEvent(const Session::Event& e)
{
    // GAME.EXE's IChatEvent (vtable), in the chat (state 7) and the game list (8).
    using Kind = Session::Event::Kind;
    const bool chat = wolState_ == 7 && screen_ && !stack_.empty() && stack_.back() == State::Chat;  // most events only touch the chat screen while it is up
    const auto me = [&](const Session::User& u) { return (u.flags & Session::User::kMe) != 0; };
    const auto decoded = [&](const std::string& wire) {
        return nox::strings::toUtf8(nox::lobby::wolDecodeText(wire));
    };
    switch (e.kind) {
    case Kind::Motd:
        wolMotd_ = e.text; // (its slot 0)
        break;
    case Kind::ChannelList:
        if (wolState_ == 8) {
            // The games in the list.
            closeDialog();
            browser_.refresh(nowMs_, &e.games);
            break;
        }
        if (chat) {
            // The lobbies listed afresh, ours found again by its name;
            // in none, one picked and joined. The list made again
            // (event 10).
            closeDialog();
            const int was = wolLists_.current();
            const std::string ours = was >= 0 ? wolLists_.lobbies()[static_cast<std::size_t>(was)].channel : std::string();
            wolLists_.clearLobbies();
            for (const auto& c : e.channels) {
                wolLists_.addLobby(c);
            }
            fillLobbies();
            if (ours.empty()) {
                const int count = static_cast<int>(wolLists_.lobbies().size());
                // pickLobby takes the first lobby under 25 users, else this random one (a plain LCG).
                wolRandom_ = wolRandom_ * 1103515245u + 12345u;
                const int pick = wolLists_.pickLobby(count > 0 ? static_cast<int>((wolRandom_ >> 16) % count) : -1);
                if (pick >= 0) {
                    wolLists_.setCurrent(wolLists_.lobbies()[static_cast<std::size_t>(pick)].channel);
                    joinLobby(pick);
                }
            } else {
                wolLists_.setCurrent(ours);
            }
        }
        break;
    case Kind::ChannelJoin:
        // Ours (0x8000) in the title and "You have joined...", the box
        // closed; the lobby's count up, the user in the list.
        if (!chat) {
            break;
        }
        if (me(e.user)) {
            wolLists_.setCurrent(e.channel);
            chatTitle();
            if (wolLists_.current() >= 0) {
                chatLine(format("WolChat.c:Joined", {lobbyText(wolLists_.current())}), kSystemColor);
            }
            closeDialog();
            focusChatEntry();
        }
        lobbyUsersChanged(1);
        userLine(wolLists_.addUser(e.user));
        break;
    case Kind::ChannelLeave:
        // The user out of the list; ours: the list emptied, "You have left...", in no
        // lobby.
        if (!chat) {
            break;
        }
        removeUserLine(wolLists_.removeUser(e.user.name));
        if (me(e.user)) {
            wolLists_.clearUsers();
            fillUsers();
            if (wolLists_.current() >= 0) {
                chatLine(format("WolChat.c:Left", {lobbyText(wolLists_.current())}), kSystemColor);
            }
            lobbyUsersChanged(-1);
            wolLists_.setCurrent({});
            chatTitle();
        } else {
            lobbyUsersChanged(-1);
        }
        break;
    case Kind::UserList:
        // The users afresh; the list made again (event 0xB).
        if (chat) {
            wolLists_.clearUsers();
            for (const auto& u : e.users) {
                wolLists_.addUser(u);
            }
            fillUsers();
        }
        break;
    case Kind::PublicMessage:
        if (chat && !(e.user.flags & Session::User::kSquelched)) {
            chatLine("<" + e.user.name + "> " + decoded(e.text), kPublicColor);
        }
        break;
    case Kind::PublicAction:
        if (chat && !(e.user.flags & Session::User::kSquelched)) {
            chatLine(e.user.name + " " + decoded(e.text), kActionColor);
        }
        break;
    case Kind::PrivateMessage:
        if (chat && !me(e.user) && !(e.user.flags & Session::User::kSquelched)) {
            chatLine(format("WolChat.c:Privatemessage", {e.user.name, decoded(e.text)}), kPrivateColor);
        }
        break;
    case Kind::PrivateAction:
        if (chat && !me(e.user) && !(e.user.flags & Session::User::kSquelched)) {
            chatLine(format("WolChat.c:PrivateAction", {e.user.name, decoded(e.text)}), kActionColor);
        }
        break;
    case Kind::UserKick:
        // A user kicked.
        if (!chat) {
            break;
        }
        removeUserLine(wolLists_.removeUser(e.user.name));
        if (me(e.user)) {
            wolLists_.clearUsers();
            fillUsers();
            lobbyUsersChanged(-1);
            wolLists_.setCurrent({});
            chatTitle();
            chatLine(format("WolChat.c:YouWereKicked", {e.other}), kSystemColor);
        } else {
            chatLine(format("WolChat.c:Kicked", {e.other, e.user.name}), kSystemColor);
            lobbyUsersChanged(-1);
        }
        break;
    case Kind::ChannelTopic:
        if (chat) {
            chatLine(decoded(e.text), kSystemColor); // (L"%S")
        }
        break;
    case Kind::JoinFailed:
        // OnChannelJoin's error: the error's line (the box stays up).
        if (chat) {
            // e.text: the server's IRC-style numeric reply.
            const char* why = e.text == "471"   ? "wolstate.c:channelfull"
                              : e.text == "403" ? "wolstate.c:channeldoesnotexist"
                              : e.text == "475" ? "wolstate.c:badpassword"
                              : e.text == "474" ? "wolstate.c:bannedfromchannel"
                                                : "wolstate.c:unknownerror";
            chatLine(gui_.text(why), kSystemColor);
        }
        break;
    }
}

} // namespace noxsdl
