// RankBoard (rank_board.hpp): the scoreboard's modes and its drawing, laid out in game pixels at
// the HUD's scale.
#include "rank_board.hpp"

#include "console.hpp"
#include "hud.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace noxsdl {

namespace {

constexpr std::uint8_t kHeader = 9;    // the column headings (green)
constexpr std::uint8_t kObserver = 9;  // observers' rows
constexpr std::uint8_t kNoTeam = 3;    // light grey; grey (2) once out
constexpr int kNameWidth = 80;
constexpr int kPerPanel = 16;

/// A team colour's palette entry (by Team colour % 10).
std::uint8_t teamPalette(std::uint8_t color)
{
    static constexpr std::array<std::uint8_t, 10> kTeam{4, 7, 13, 10, 12, 15, 5, 0, 4, 14};
    return kTeam[color % 10];
}

/// The CSF text's printf fields (%d, %02d, %s, %S) filled in order.
std::string format(std::string_view pattern, std::initializer_list<std::string> args)
{
    std::string out;
    auto next = args.begin();
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%' || i + 1 >= pattern.size()) {
            out += pattern[i];
            continue;
        }
        std::size_t j = i + 1;
        std::string width;
        while (j < pattern.size() && std::isdigit(static_cast<unsigned char>(pattern[j]))) {
            width += pattern[j++];
        }
        if (j < pattern.size() && (pattern[j] == 'd' || pattern[j] == 's' || pattern[j] == 'S')) {
            std::string v = next != args.end() ? *next++ : std::string();
            if (!width.empty() && width.front() == '0' && v.size() < static_cast<std::size_t>(std::stoi(width))) {
                v.insert(0, static_cast<std::size_t>(std::stoi(width)) - v.size(), '0');
            }
            out += v;
            i = j;
        } else if (j < pattern.size() && pattern[j] == '%') {
            out += '%';
            i = j;
        } else {
            out += pattern[i];
        }
    }
    return out;
}

} // namespace

std::string RankBoard::text(std::string_view id) const
{
    if (strings_) {
        if (const auto t = strings_->text(id)) {
            return nox::strings::toUtf8(*t);
        }
    }
    const auto colon = id.find(':');
    return std::string(colon == std::string_view::npos ? id : id.substr(colon + 1));
}

void RankBoard::cycle(bool quest, bool online)
{
    // Modes: 0 off, 1 the quest list, 2 the full board, 3 the teams, 4 the top three, 5 the WOL
    // ranks; 1 is skipped out of quests, 5 out of online games.
    mode_ = (mode_ + 1) % 6;
    if (!online && mode_ == 5) {
        mode_ = 0;
        return;
    }
    if (mode_ == 1 && !quest) {
        mode_ = 2; // the quest list only in quest games
    }
}

void RankBoard::gameOver(bool quest, bool online)
{
    if (mode_ != 2) {
        savedMode_ = mode_;
        mode_ = 0;
        cycle(quest, online);
    }
}

void RankBoard::update(bool over, bool quest, bool online)
{
    // no longer over, back to the mode it had (stepping on to it from the one before).
    if (!over && savedMode_ != 6) {
        mode_ = savedMode_ - 1;
        if (mode_ < 0) {
            mode_ = 5;
        }
        cycle(quest, online);
        savedMode_ = 6;
    }
}

void RankBoard::draw(SDL_Renderer* renderer, const nox::client::World& world, std::uint16_t us, int w, int h)
{
    if (mode_ == 0 || !font_) {
        return;
    }
    const auto& game = world.game();
    const bool elimination = (game.gameFlags & 0x400) != 0;  // game flag 0x400: Highlander
    const float s = Hud::scale(w, h);
    // A pixel size giving 9-game-pixel text at this scale.
    const int size = font_->lineSize(9.0f, s);
    const auto width = [&](const std::string& t) { return static_cast<int>(std::ceil(font_->measure(t, size) / s)); };
    const int fh = 10;          // the default font's height
    const int lineH = fh + 1;   // the lists' line height (guiDrawRank)

    // The columns' widths (guiDrawRank): name 80; marker; class; score; ping.
    const std::array<std::string, 3> classNames{text("guirank.c:Warrior"), text("guirank.c:Wizard"),
                                                text("guirank.c:Conjurer")};
    // In the quest list columns 2-4 hold lives, health and class instead, hence the wider headings.
    std::array<int, 5> col{};
    col[0] = kNameWidth;
    col[1] = std::max(width(text("guirank.c:Flag")), 18) + 14;
    col[2] = std::max({width(text("Guirank.c:class")), width(classNames[0]), width(classNames[1]),
                       width(classNames[2]), width(text("GUIRank.c:LivesHeading"))}) + 7;
    col[3] = std::max(width(text("Guirank.c:score")), width(text("GUIRank.c:HealthHeading"))) + 7;
    col[4] = std::max(width(text("Guirank.c:ping")), width("8888888888")) + 7;
    int panelWidth = 0;
    for (const int c : col) {
        panelWidth += c;
    }

    // Each panel's five columns, filled a row at a time.
    std::array<Panel, 2> panels{};
    const auto row = [&](int p, std::array<Line, 5> cells) {
        for (std::size_t c = 0; c < 5; ++c) {
            panels[static_cast<std::size_t>(p)][c].push_back(std::move(cells[c]));
        }
    };

    // The teams, by score (fewest first in elimination games).
    std::vector<const nox::client::Team*> teams;
    for (const auto& [id, t] : world.teams()) {
        teams.push_back(&t);
    }
    std::ranges::stable_sort(teams, [&](const auto* a, const auto* b) {
        return elimination ? a->score < b->score : a->score > b->score;
    });
    if (!teams.empty() && (mode_ == 2 || mode_ == 3)) {
        row(0, {Line{text("Guirank.c:team"), kHeader}, Line{"", kHeader}, Line{"", kHeader},
                Line{text("Guirank.c:score"), kHeader}, Line{"", kHeader}});
        std::uint8_t last = 4;
        for (const auto* t : teams) {
            last = teamPalette(t->color);
            row(0, {Line{t->name, last}, Line{"", last}, Line{"", last}, Line{std::to_string(t->score), last}, Line{"", last}});
        }
        row(0, {Line{"", last}, Line{"", last}, Line{"", last}, Line{"", last}, Line{"", last}});
    }

    // The players: those playing first, by lessons (deaths, fewest first, in elimination games),
    // then the observers.
    struct Entry {
        const nox::client::PlayerEntry* p;
        bool observing;
        std::int32_t score;
    };
    std::vector<Entry> players;
    int active = 0;
    for (const auto& [nc, p] : world.players()) {
        // Status 0x20: eliminated (out of lives in elimination, made to watch); still counted in the game.
        const bool observing = (p.status & 1) && !(p.status & 0x20);  // status 1: observing
        players.push_back({&p, observing, elimination ? p.kills : p.lessons});
        active += observing ? 0 : 1;
    }
    std::ranges::stable_sort(players, [&](const Entry& a, const Entry& b) {
        if (a.observing != b.observing) {
            return !a.observing;
        }
        return elimination ? a.score < b.score : a.score > b.score;
    });

    int ourRow = -1;
    int ourPanel = 0;
    int ourRank = 0;
    struct KeyMarks {
        int row = 0;
        std::uint8_t silver = 0;
        std::uint8_t gold = 0;
    };
    std::vector<KeyMarks> keys; // the quest list's rows with a key held
    if (!players.empty() && mode_ == 1) {
        // The quest list: the players, their lives ("%d", each player's extra lives: MSG_GAUNTLET
        // 4), health ("%d%%", each player's reported health percentage; ours from our own meter, its value / its most x 100
        // chopped; under 26 colour 6, under 51 colour 0xF, else 4) and class.
        row(0, {Line{text("Guirank.c:player"), kHeader}, Line{"", kHeader}, Line{text("GUIRank.c:LivesHeading"), kHeader},
                Line{text("GUIRank.c:HealthHeading"), kHeader}, Line{text("Guirank.c:class"), kHeader}});
        for (const Entry& e : players) {
            if (e.observing) {
                continue;
            }
            const auto team = world.teams().find(e.p->team);
            const std::uint8_t color =
                e.p->team == 0 || team == world.teams().end() ? std::uint8_t{4} : teamPalette(team->second.color);
            if (e.p->netCode == us) {
                ourRow = static_cast<int>(panels[0][0].size());
            }
            int health = e.p->healthPct;
            if (e.p->netCode == us && world.self().maxHealth != 0) {
                const auto& self = world.self();
                health = static_cast<int>(static_cast<float>(self.health) / static_cast<float>(self.maxHealth) * 100.0f);
            }
            const std::uint8_t healthColor = health < 0x1A ? std::uint8_t{6} : health < 0x33 ? std::uint8_t{0xF} : std::uint8_t{4};
            if (e.p->silverKey != 0 || e.p->goldKey != 0) {
                keys.push_back({static_cast<int>(panels[0][0].size()), e.p->silverKey, e.p->goldKey});
            }
            row(0, {Line{e.p->name, color}, Line{"", color}, Line{std::to_string(e.p->questLives), color},
                    Line{std::to_string(health) + "%", healthColor},
                    Line{classNames[std::min<std::size_t>(e.p->playerClass, 2)], color}});
        }
    }
    if (!players.empty() && (mode_ == 2 || mode_ == 4 || mode_ == 5)) {
        const auto header = [&](int p) {
            row(p, {Line{text("Guirank.c:player"), kHeader}, Line{"", kHeader}, Line{text("Guirank.c:class"), kHeader},
                    Line{text("Guirank.c:score"), kHeader}, Line{text("Guirank.c:ping"), kHeader}});
        };
        const int teamRows = static_cast<int>(panels[0][0].size());
        header(0);
        const std::size_t count = mode_ == 4 ? std::min<std::size_t>(players.size(), 3) : players.size();
        const auto carrying = [&](const nox::client::PlayerEntry& p) -> std::pair<std::string, std::uint8_t> {
            // the flag carriers in CTF (0x20), the ball's in flagball (0x40), the crown's
            // wearer in King of the Realm (0x10).
            if (game.gameFlags & 0x20) {
                int slot = 0;
                for (const auto& [i, f] : game.flags) {
                    if (f.state == 1 && f.carrier == p.netCode) {
                        return {"<" + text("guirank.c:Flag"), slot == 0 ? std::uint8_t{7} : std::uint8_t{13}};
                    }
                    ++slot;
                }
            } else if (game.gameFlags & 0x40) {
                if (game.ballCarrier == p.netCode) {
                    return {"<" + text("guirank.c:Ball"), 4};
                }
            } else if (game.gameFlags & 0x10) {
                if (const auto* d = world.drawable(p.netCode); d && (d->buffs & (1u << 30))) {
                    return {"<" + text("guirank.c:King"), 4};
                }
            }
            return {"", 4};
        };
        for (std::size_t i = 0; i < count; ++i) {
            const Entry& e = players[i];
            const nox::client::PlayerEntry& p = *e.p;
            // Players past the 16th go into the second panel.
            const int panel = static_cast<int>(i / kPerPanel) > 0 ? 1 : 0;
            if (i == kPerPanel) {
                // The second panel's rows line up with the first's players.
                for (int k = 0; k < teamRows; ++k) {
                    row(1, {Line{}, Line{}, Line{}, Line{}, Line{}});
                }
                header(1);
            }
            // Out of an elimination game: the limit reached.
            const bool out = elimination && game.fragLimit != 0 && game.fragLimit <= e.score;
            std::uint8_t color = kObserver;
            if (!e.observing || out) {
                const auto team = world.teams().find(p.team);
                if (p.team == 0 || team == world.teams().end()) {
                    color = out || (p.status & 0x20) ? 2 : kNoTeam;  // 0x20: eliminated
                } else {
                    color = teamPalette(team->second.color);
                    if (out || (p.status & 0x20)) {  // 0x20: eliminated
                        color = static_cast<std::uint8_t>(color - 2);
                    }
                }
            }
            std::string name = p.name;
            while (!name.empty() && width(name) + 5 > kNameWidth) {
                name.pop_back();
            }
            if (p.netCode == us) {
                ourRow = static_cast<int>(panels[static_cast<std::size_t>(panel)][0].size());
                ourPanel = panel;
            }
            auto [marker, markerColor] = carrying(p);
            std::string score = std::to_string(e.score);
            if (mode_ == 5) {
                // The WOL ranks: the account name where the marker goes, "--" for no score.
                marker = "(" + p.account + ")";
                markerColor = color;
                if (e.score < 1) {
                    score = "--";
                }
            }
            row(panel, {Line{name, color}, Line{marker, markerColor},
                        Line{classNames[std::min<std::size_t>(p.playerClass, 2)], color}, Line{score, color},
                        Line{std::to_string(p.latency), color}});
        }
    }
    for (std::size_t i = 0; i < players.size(); ++i) {
        if (players[i].p->netCode == us) {
            // one more than those ahead.
            ourRank = 1;
            for (const Entry& e : players) {
                if (!e.observing && (elimination ? e.score < players[i].score : e.score > players[i].score)) {
                    ++ourRank;
                }
            }
        }
    }

    // The lines above the lists (guiDrawRank's static texts): title, your rank, lesson limit, time.
    const float x0 = 2;
    const float y0 = static_cast<float>(fh + 40);
    const auto put = [&](const std::string& t, float gx, float gy, std::uint8_t color) {
        if (!t.empty()) {
            font_->draw(t, gx * s, gy * s, size, paletteColor(color));
        }
    };
    const std::string title = mode_ == 1   ? text("Noxworld.c:Quest")
                              : mode_ == 2 ? text("guirank.c:TeamPlayerRank")
                              : mode_ == 3 ? text("guirank.c:Teams")
                              : mode_ == 4 ? text("guirank.c:Top3")
                                           : text("guirank.c:WolRank");
    put(title, x0, y0, Green);
    const auto me = world.players().find(us);
    const bool playing = me != world.players().end() && (!(me->second.status & 1) || (me->second.status & 0x20));  // 0x20: eliminated, still in the game
    if (mode_ == 3) {
        if (me != world.players().end() && me->second.team != 0) {
            int teamRank = 0;
            for (std::size_t i = 0; i < teams.size(); ++i) {
                if (teams[i]->id == me->second.team) {
                    teamRank = static_cast<int>(i) + 1;
                }
            }
            put(format("%s %d / %d", {text("Guirank.c:yourteamrank"), std::to_string(teamRank), std::to_string(teams.size())}),
                x0, y0 + fh, Yellow);
        }
    } else if (playing && mode_ != 1) {
        put(format("%s %d / %d", {text("Guirank.c:yourrank"), std::to_string(ourRank), std::to_string(active)}), x0,
            y0 + fh, Yellow);
    }
    if (!(game.gameFlags & 0x1080)) {  // none for quests (0x1000) or chat games (0x80)
        put(format(text("guirank.c:LessonLimit"), {std::to_string(game.fragLimit)}), x0, y0 + 2 * fh, White);
    }
    if (game.timerRunning) {
        const std::uint32_t ms = game.timeRemainingMs;
        put(format(text("guirank.c:TimeRemaining"), {std::to_string(ms / 60000), std::to_string(ms % 60000 / 1000)}), x0,
            y0 + 3 * fh, Red);
    }

    // The two panels of five columns.
    const float listTop = y0 + 4 * fh + 2;
    for (std::size_t p = 0; p < 2; ++p) {
        float cx = x0 + static_cast<float>(p) * static_cast<float>(panelWidth);
        // A column at a time, top to bottom, then on by its width.
        for (std::size_t c = 0; c < 5; ++c) {
            float y = listTop;
            for (const Line& l : panels[p][c]) {
                put(l.text, cx + 2, y, l.color);
                y += lineH;
            }
            cx += static_cast<float>(col[c]);
        }
    }
    if (ourRow >= 0) {
        // a yellow dot by our row.
        const float px = (x0 + static_cast<float>(ourPanel * panelWidth) + 1) * s;
        const float py = (listTop + static_cast<float>(ourRow) * lineH + lineH / 2.0f) * s;
        const SDL_Color y = paletteColor(Yellow);
        SDL_SetRenderDrawColor(renderer, y.r, y.g, y.b, 255);
        const SDL_FRect dot{px - 1.5f * s, py - 1.5f * s, 3 * s, 3 * s};
        SDL_RenderFillRect(renderer, &dot);
    }
    // The quest list's keys: in the marker column, at its x + 5 and the row's middle
    // less 1 (the line height / 2 - 1 + row x the line height), a key's shape: a ring of radius
    // 2 (video_drawCircle), its shaft (x + 2 to x + 9) and two teeth (x + 9 down 3,
    // x + 7 down 2). A SilverKey white (the player holds one), a GoldKey yellow, the gold one
    // at x + 0x14 beside a silver one.
    const auto pixel = [&](int gx, int gy) {
        const SDL_FRect p{static_cast<float>(gx) * s, static_cast<float>(gy) * s, s, s};
        SDL_RenderFillRect(renderer, &p);
    };
    const auto key = [&](int kx, int ky, SDL_Color c) {
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 255);
        static constexpr std::array<SDL_Point, 12> kRing{{{0, 2}, {0, -2}, {2, 0}, {-2, 0}, {1, 2}, {-1, 2}, {1, -2}, {-1, -2},
                                                          {2, 1}, {-2, 1}, {2, -1}, {-2, -1}}};
        for (const SDL_Point& d : kRing) {
            pixel(kx + d.x, ky + d.y);
        }
        for (int i = 2; i <= 9; ++i) {
            pixel(kx + i, ky);
        }
        for (int i = 1; i <= 3; ++i) {
            pixel(kx + 9, ky + i);
        }
        for (int i = 1; i <= 2; ++i) {
            pixel(kx + 7, ky + i);
        }
    };
    const int markerX = static_cast<int>(x0) + col[0];
    for (const KeyMarks& k : keys) {
        const int ky = static_cast<int>(listTop) + lineH / 2 - 1 + k.row * lineH;
        if (k.silver != 0) {
            key(markerX + 5, ky, paletteColor(White));
        }
        if (k.gold != 0) {
            key(markerX + (k.silver == 1 ? 0x14 : 5), ky, paletteColor(Yellow));
        }
    }
}

} // namespace noxsdl
