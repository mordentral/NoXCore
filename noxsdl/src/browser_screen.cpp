// The server browser screen (browser_screen.hpp): the sorted game list, its input, and its two
// looks (full screen, or framed inside the menus' game browser).
#include "browser_screen.hpp"
#include "client_level_gate.hpp"
#include "pencil_icon.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace noxsdl {

namespace {

// A padlock `s` pixels high centred at (cx, cy): the list's mark for a game with a password, at the
// row's far right. A grey shackle over a gold body with a dark keyhole, a dark edge round both.
void drawLock(SDL_Renderer* renderer, float cx, float cy, float s)
{
    const float bodyTop = cy - 0.06f * s, bodyBottom = cy + 0.46f * s, bodyHalf = 0.36f * s;
    const float arcY = cy - 0.20f * s, rOut = 0.26f * s, thick = std::max(1.5f, 0.09f * s);
    // The shackle: a half ring over arcY, its legs straight down to legEnd, as one strip of quads.
    const auto shackle = [&](float rOuter, float rInner, float legEnd, SDL_FColor c) {
        constexpr int kSteps = 12;
        std::array<SDL_Vertex, 2 * (kSteps + 3)> v{};
        int n = 0;
        const auto put = [&](float x, float y) { v[static_cast<std::size_t>(n++)] = {{x, y}, c, {0, 0}}; };
        put(cx - rOuter, legEnd);
        put(cx - rInner, legEnd);
        for (int i = 0; i <= kSteps; ++i) { // (from the left, over the top, to the right)
            const float a = std::numbers::pi_v<float> * (1.0f + static_cast<float>(i) / kSteps);
            put(cx + rOuter * std::cos(a), arcY + rOuter * std::sin(a));
            put(cx + rInner * std::cos(a), arcY + rInner * std::sin(a));
        }
        put(cx + rOuter, legEnd);
        put(cx + rInner, legEnd);
        std::array<int, 6 * (kSteps + 2)> quads{};
        int k = 0;
        for (int q = 0; q + 3 < n; q += 2) {
            for (const int d : {0, 1, 2, 1, 3, 2}) {
                quads[static_cast<std::size_t>(k++)] = q + d;
            }
        }
        SDL_RenderGeometry(renderer, nullptr, v.data(), n, quads.data(), k);
    };
    shackle(rOut + 1, rOut - thick - 1, bodyTop, {0.04f, 0.04f, 0.05f, 1});
    shackle(rOut, rOut - thick, bodyTop + 1, {0.80f, 0.80f, 0.84f, 1});
    SDL_SetRenderDrawColor(renderer, 10, 10, 12, 255);
    const SDL_FRect edge{cx - bodyHalf - 1, bodyTop - 1, 2 * bodyHalf + 2, bodyBottom - bodyTop + 2};
    SDL_RenderFillRect(renderer, &edge);
    SDL_SetRenderDrawColor(renderer, 240, 180, 42, 255);
    const SDL_FRect body{cx - bodyHalf, bodyTop, 2 * bodyHalf, bodyBottom - bodyTop};
    SDL_RenderFillRect(renderer, &body);
    SDL_SetRenderDrawColor(renderer, 40, 28, 8, 255);
    const float holeW = std::max(1.0f, std::round(0.10f * s));
    const SDL_FRect hole{std::round(cx - holeW / 2), std::round((bodyTop + bodyBottom) / 2 - 0.09f * s), holeW,
                         std::max(2.0f, std::round(0.18f * s))};
    SDL_RenderFillRect(renderer, &hole);
}

// An IPv4 address held most significant byte first, as dotted text.
std::string ipText(std::uint32_t a)
{
    return std::to_string(a >> 24) + "." + std::to_string((a >> 16) & 0xFF) + "." + std::to_string((a >> 8) & 0xFF) +
           "." + std::to_string(a & 0xFF);
}

// Ping colours: grey unknown, green under 100 ms, yellow under 200, else red.
SDL_Color pingColor(const std::optional<std::uint32_t>& ping)
{
    if (!ping) {
        return {140, 140, 140, 255};
    }
    if (*ping < 100) {
        return {120, 230, 120, 255};
    }
    if (*ping < 200) {
        return {235, 220, 110, 255};
    }
    return {235, 120, 110, 255};
}

} // namespace

BrowserScreen::BrowserScreen(Font& font, nox::lobby::ServerBrowser::Options options)
    : font_(font), browser_(std::move(options))
{
}

void BrowserScreen::refresh(std::uint64_t nowMs, const std::vector<nox::lobby::GameListing>* xwisGames)
{
    browser_.refresh(static_cast<std::uint32_t>(nowMs), xwisGames);
    rows_.clear();
    selected_ = 0;
    scroll_ = 0;
}

// Pumps the search, keeping the last three log lines for the bottom of the screen.
void BrowserScreen::update(std::uint64_t nowMs)
{
    browser_.poll(static_cast<std::uint32_t>(nowMs));
    for (auto& line : browser_.takeLog()) {
        SDL_Log("browser: %s", line.c_str());
        log_.push_back(std::move(line));
    }
    if (log_.size() > 3) {
        log_.erase(log_.begin(), log_.end() - 3);
    }
    sortRows();
}

void BrowserScreen::sortRows()
{
    // Keep the selection on the same server as the list changes.
    std::optional<std::pair<std::uint32_t, std::uint16_t>> keep;
    if (selected_ >= 0 && selected_ < static_cast<int>(rows_.size())) {
        keep = std::pair{rows_[static_cast<std::size_t>(selected_)].game.address,
                         rows_[static_cast<std::size_t>(selected_)].game.port};
    }
    rows_ = browser_.servers();
    // noxcpp servers first, then the other pinged ones, fastest first; then the rest by name.
    // Network (no XWIS): the LAN's games before the listed servers (servers.txt, --server).
    const bool lanFirst = !browser_.listsXwis();
    std::stable_sort(rows_.begin(), rows_.end(), [lanFirst](const auto& a, const auto& b) {
        const bool aLan = (a.sources & nox::lobby::kFromLan) != 0, bLan = (b.sources & nox::lobby::kFromLan) != 0;
        if (lanFirst && aLan != bLan) {
            return aLan;
        }
        if (a.extended.has_value() != b.extended.has_value()) {
            return a.extended.has_value();
        }
        if (a.pingMs.has_value() != b.pingMs.has_value()) {
            return a.pingMs.has_value();
        }
        if (a.pingMs && *a.pingMs != *b.pingMs) {
            return *a.pingMs < *b.pingMs;
        }
        return a.game.name < b.game.name;
    });
    if (keep) {
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i].game.address == keep->first && rows_[i].game.port == keep->second) {
                selected_ = static_cast<int>(i);
            }
        }
    }
    selected_ = std::clamp(selected_, 0, std::max(0, static_cast<int>(rows_.size()) - 1));
}

// The full-screen layout: text a 40th of the height (12-40 px), the list under a five-row
// header, four rows kept free below.
BrowserScreen::Layout BrowserScreen::layout(int height)
{
    Layout l;
    l.size = std::clamp(height / 40, 12, 40);
    l.rowHeight = std::round(font_.lineHeight(l.size) * 1.25f);
    l.top = l.rowHeight * 5;
    l.visibleRows = std::max(1, static_cast<int>((static_cast<float>(height) - l.top - l.rowHeight * 4) / l.rowHeight));
    return l;
}

std::optional<std::string> BrowserScreen::joinSelected()
{
    if (selected_ < 0 || selected_ >= static_cast<int>(rows_.size())) {
        return std::nullopt;
    }
    return address(rows_[static_cast<std::size_t>(selected_)].game);
}

std::string BrowserScreen::searchingText() const
{
    std::vector<std::string> places;
    if (browser_.listsXwis()) {
        places.emplace_back("XWIS");
    }
    if (browser_.listsLan()) {
        places.emplace_back("the LAN");
    }
    if (browser_.hasServerList()) {
        places.emplace_back("your server list");
    }
    if (places.empty()) {
        return "Searching...";
    }
    std::string text = "Searching ";
    for (std::size_t i = 0; i < places.size(); ++i) {
        if (i > 0) {
            text += i + 1 == places.size() ? " and " : ", ";
        }
        text += places[i];
    }
    return text + "...";
}

std::string BrowserScreen::address(const nox::lobby::GameListing& game)
{
    return ipText(game.address) + ":" + std::to_string(game.port);
}

const nox::lobby::ServerEntry* BrowserScreen::entryFor(const std::string& server) const
{
    const auto it = std::ranges::find_if(rows_, [&](const nox::lobby::ServerEntry& e) { return address(e.game) == server; });
    return it != rows_.end() ? &*it : nullptr;
}

bool BrowserScreen::sawStock(const std::string& server) const
{
    return std::ranges::any_of(rows_, [&](const nox::lobby::ServerEntry& e) {
        return e.answered && !e.extended && address(e.game) == server;
    });
}

std::optional<std::string> BrowserScreen::handleEvent(const SDL_Event& event, std::uint64_t nowMs)
{
    // Uses the layout of the last draw, so clicks map onto the rows as they were shown.
    const Layout l = lastLayout_;
    const auto inArea = [&](float x, float y) {
        return x >= area_.x && x < area_.x + area_.w && y >= area_.y && y < area_.y + area_.h;
    };
    // Scrolls so the selected row is on screen.
    const auto keepVisible = [&] {
        if (selected_ < scroll_) {
            scroll_ = selected_;
        } else if (selected_ >= scroll_ + l.visibleRows) {
            scroll_ = selected_ - l.visibleRows + 1;
        }
    };
    switch (event.type) {
    // Keys: arrows move the selection, R/F5 search again, Enter joins.
    case SDL_EVENT_KEY_DOWN:
        switch (event.key.key) {
        case SDLK_UP:
            selected_ = std::max(0, selected_ - 1);
            keepVisible();
            break;
        case SDLK_DOWN:
            selected_ = std::min(static_cast<int>(rows_.size()) - 1, selected_ + 1);
            keepVisible();
            break;
        case SDLK_R:
        case SDLK_F5:
            // The menus install their own refresh (it asks the WOL connection for the list while logged in).
            if (refreshHandler_) {
                refreshHandler_(nowMs);
            } else {
                refresh(nowMs);
            }
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            return joinSelected();
        default:
            break;
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!inArea(event.wheel.mouse_x, event.wheel.mouse_y)) {
            break;
        }
        // One wheel notch scrolls one row; never past the last full page.
        scroll_ = std::clamp(scroll_ - static_cast<int>(event.wheel.y), 0,
                             std::max(0, static_cast<int>(rows_.size()) - l.visibleRows));
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.button == SDL_BUTTON_LEFT && inArea(event.button.x, event.button.y)) {
            // Which row was hit, counted from the top of the list below the header.
            const float y = event.button.y - l.top;
            if (y >= 0 && y < l.rowHeight * static_cast<float>(l.visibleRows)) {
                const int row = scroll_ + static_cast<int>(y / l.rowHeight);
                if (row < static_cast<int>(rows_.size())) {
                    // A second click on the same row within 400 ms joins it.
                    const bool again = row == lastClickRow_ && nowMs - lastClickMs_ < 400;
                    selected_ = row;
                    lastClickRow_ = row;
                    lastClickMs_ = nowMs;
                    clicked_ = Clicked{rows_[static_cast<std::size_t>(row)], {event.button.x, event.button.y}};
                    if (again) {
                        return joinSelected();
                    }
                }
            }
        }
        break;
    default:
        break;
    }
    return std::nullopt;
}

void BrowserScreen::draw(SDL_Renderer* renderer, int width, int height)
{
    lastHeight_ = height;
    area_ = {0, 0, static_cast<float>(width), static_cast<float>(height)};
    const Layout l = layout(height);
    lastLayout_ = l;
    const float margin = std::round(static_cast<float>(width) * 0.03f);
    const SDL_Color grey{150, 150, 150, 255};
    const SDL_Color gold{240, 200, 110, 255};

    font_.draw("Nox games", margin, l.rowHeight * 0.6f, static_cast<int>(l.size * 1.5), gold);
    std::string status = browser_.searching() ? searchingText() : std::to_string(rows_.size()) + " game(s)";
    status += "     Enter / double-click: join     R: refresh     Esc: quit";
    font_.draw(status, margin, l.rowHeight * 2.2f, l.size, grey);
    if (!message_.empty()) {
        font_.draw(message_, margin, l.rowHeight * 3.2f, l.size, {235, 160, 110, 255});
    }
    drawRows(renderer, {margin, 0, static_cast<float>(width) - 2 * margin, static_cast<float>(height)}, l, false);

    // The last few search messages along the bottom.
    float y = static_cast<float>(height) - l.rowHeight * (static_cast<float>(log_.size()) + 0.5f);
    for (const auto& line : log_) {
        font_.draw(line, margin, y, std::max(10, l.size - 2), grey);
        y += l.rowHeight;
    }
}

void BrowserScreen::drawFramed(SDL_Renderer* renderer, SDL_FRect area, int textSize)
{
    // The game's list colours: gold text, the headers in the frame's green.
    area_ = area;
    Layout l;
    l.size = std::max(8, textSize);
    l.rowHeight = std::round(font_.lineHeight(l.size) * 1.2f);
    const float pad = std::round(l.rowHeight * 0.4f);
    l.top = area.y + pad + l.rowHeight * 1.3f;
    const float footer = l.rowHeight * 2.5f; // status and the latest search message
    l.visibleRows = std::max(1, static_cast<int>((area.y + area.h - footer - l.top) / l.rowHeight));
    lastLayout_ = l;
    lastHeight_ = static_cast<int>(area.h);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
    SDL_RenderFillRect(renderer, &area);
    drawRows(renderer, {area.x + pad, area.y, area.w - 2 * pad, area.h}, l, true);

    const SDL_Color grey{150, 150, 150, 255};
    float y = area.y + area.h - footer + l.rowHeight * 0.3f;
    std::string status = browser_.searching() ? searchingText() : std::to_string(rows_.size()) + " game(s)";
    if (!message_.empty()) {
        status = message_;
    }
    font_.draw(status, area.x + pad, y, l.size, {240, 180, 42, 255});
    if (!log_.empty()) {
        font_.draw(log_.back(), area.x + pad, y + l.rowHeight, std::max(8, l.size - 1), grey);
    }
}

// The column headers, then the visible rows from `scroll_`, the selected one on a bar.
void BrowserScreen::drawRows(SDL_Renderer* renderer, SDL_FRect area, const Layout& l, bool framed)
{
    const SDL_Color text = framed ? SDL_Color{240, 180, 42, 255} : SDL_Color{235, 235, 235, 255};
    const SDL_Color header = framed ? SDL_Color{0, 200, 0, 255} : SDL_Color{150, 150, 150, 255};
    const SDL_Color grey{150, 150, 150, 255};
    const float x0 = area.x;
    const float w = area.w;

    // Columns: name, map, mode, players, ping, where it was found.
    // (From has the most room after the name's: "NoX Core XWIS LAN" and the padlock beside it.)
    const float cols[6] = {x0, x0 + w * 0.35f, x0 + w * 0.49f, x0 + w * 0.61f, x0 + w * 0.71f, x0 + w * 0.80f};
    const char* headers[6] = {"Name", "Map", "Mode", "Players", "Ping", "From"};
    const float headerY = l.top - l.rowHeight;
    for (int i = 0; i < 6; ++i) {
        font_.draw(headers[i], cols[i], headerY, l.size, header);
    }
    SDL_SetRenderDrawColor(renderer, framed ? 0 : 80, framed ? 120 : 80, framed ? 0 : 80, 255);
    SDL_RenderLine(renderer, x0, l.top - 2, x0 + w, l.top - 2);

    // The padlock's place at each row's far right, kept clear whether or not a row has one.
    const float lockSize = std::round(font_.lineHeight(l.size) * 0.95f);
    const float lockRoom = lockSize + 4;

    // A column's text, cut to fit before the next one (with `after`, cut to leave room for it,
    // drawn after it in `afterColor`).
    const auto cell = [&](int column, const std::string& s, float ty, SDL_Color c, const std::string& after = {},
                          SDL_Color afterColor = {}) {
        const float room = (column < 5 ? cols[column + 1] : x0 + w - lockRoom) - cols[column] - 4;
        const float afterWidth = after.empty() ? 0.0f : font_.measure(after, l.size);
        std::string shown = s;
        while (!shown.empty() && font_.measure(shown, l.size) > room - afterWidth) {
            shown.pop_back();
        }
        font_.draw(shown, cols[column], ty, l.size, c);
        if (!after.empty()) {
            font_.draw(after, cols[column] + font_.measure(shown, l.size), ty, l.size, afterColor);
        }
    };
    // A game that wants a newer NoX Core (client_level_gate.hpp): "(Update needed)" after its name,
    // as a closed game's, in the messages' orange, kept whole however long the name.
    const SDL_Color updateColor{235, 160, 110, 255};
    for (int r = 0; r < l.visibleRows && scroll_ + r < static_cast<int>(rows_.size()); ++r) {
        const int index = scroll_ + r;
        const auto& e = rows_[static_cast<std::size_t>(index)];
        const auto& g = e.game;
        const float y = l.top + static_cast<float>(r) * l.rowHeight;
        // The selection bar: the frame's dark green when framed, blue full screen.
        if (index == selected_) {
            if (framed) {
                SDL_SetRenderDrawColor(renderer, 0, 90, 0, 255);
            } else {
                SDL_SetRenderDrawColor(renderer, 40, 60, 105, 255);
            }
            const SDL_FRect bar{x0 - 3, y, w + 6, l.rowHeight};
            SDL_RenderFillRect(renderer, &bar);
        }
        const float ty = y + (l.rowHeight - font_.lineHeight(l.size)) / 2;
        // Unnamed listings show their address instead.
        std::string name = g.name.empty() ? ipText(g.address) : g.name;
        // (A password's game: the padlock at the row's far right, drawn below.)
        if (g.access == nox::lobby::GameAccess::Closed) {
            name += " (closed)";
        }
        if (e.extended && (e.extended->features & nox::lobby::ServerFeature::NoxCoreOnly)) {
            name += " (NoX Core only)"; // (a mod that turns GAME.EXE players away)
        }
        const std::string updateMark = client_level::updateNeeded(e) ? " (Update needed)" : "";
        if (e.extended && (e.extended->features & nox::lobby::ServerFeature::EditSession)) {
            // A map editor session (the NXSV edit-session bit): a pencil before its name
            // ("[EDIT] <host>"), the name in the noxcpp colour.
            const SDL_Color mark{120, 210, 255, 255};
            // (pencil_icon.hpp: the same pencil marks editors in the game.)
            const float s = std::round(font_.lineHeight(l.size) * 0.95f);
            drawPencil(renderer, x0 - s / 2 - 2, y + l.rowHeight / 2, s, PeerRgb{242, 199, 77});
            cell(0, name, ty, mark, updateMark, updateColor);
        } else if (e.extended) {
            // A noxcpp server (it answered the extended query): a light-blue diamond before
            // its name, the name in that colour.
            const SDL_Color mark{120, 210, 255, 255};
            const float d = std::round(font_.lineHeight(l.size) * 0.22f);
            const float cx = x0 - d - 3;
            const float cy = y + l.rowHeight / 2;
            const SDL_Vertex v[4] = {{{cx, cy - d}, {mark.r / 255.0f, mark.g / 255.0f, mark.b / 255.0f, 1}, {0, 0}},
                                     {{cx + d, cy}, {mark.r / 255.0f, mark.g / 255.0f, mark.b / 255.0f, 1}, {0, 0}},
                                     {{cx, cy + d}, {mark.r / 255.0f, mark.g / 255.0f, mark.b / 255.0f, 1}, {0, 0}},
                                     {{cx - d, cy}, {mark.r / 255.0f, mark.g / 255.0f, mark.b / 255.0f, 1}, {0, 0}}};
            const int quad[6] = {0, 1, 2, 0, 2, 3};
            SDL_RenderGeometry(renderer, nullptr, v, 4, quad, 6);
            cell(0, name, ty, mark, updateMark, updateColor);
        } else {
            cell(0, name, ty, text);
        }
        cell(1, g.map, ty, text);
        cell(2, std::string(nox::lobby::gameModeName(g.gameFlags)), ty, text);
        cell(3, std::to_string(g.players) + "/" + std::to_string(g.maxPlayers), ty, text);
        cell(4, e.pingMs ? std::to_string(*e.pingMs) + " ms" : "-", ty, pingColor(e.pingMs));
        // `sources` is a bitmask of every place the game was found (it can be several).
        std::string from;
        if (e.extended) {
            from += "NoX Core ";
        }
        if (e.sources & nox::lobby::kFromXwis) {
            from += "XWIS ";
        }
        if (e.sources & nox::lobby::kFromLan) {
            from += "LAN ";
        }
        if (e.sources & nox::lobby::kFromList) {
            from += "list";
        }
        cell(5, from, ty, grey);
        if (g.access == nox::lobby::GameAccess::Password) {
            drawLock(renderer, x0 + w - lockSize / 2 - 1, y + l.rowHeight / 2, lockSize);
        }
    }
}

} // namespace noxsdl
