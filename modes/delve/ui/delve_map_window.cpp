// Delving Mode's map window (delve_map_window.hpp): the MAP button by the meters, the bonus
// objective's line beside it, the run's act map with the fork ahead, and in the lobby the
// records' table behind a TOP button.
#include "delve_map_window.hpp"

#include "draw_box.hpp"

#include "noxcore/mapgen/biome.hpp"
#include "noxcore/server/record_class.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

namespace noxsdl {

namespace {

// In the HUD's game pixels (its 800 x 600, times its scale). The meters' frame is at
// (W - 91, H - 201) (hud.cpp); the button stands just left of it, level with the tubes' top.
constexpr float kMeterW = 91, kMeterH = 201, kTubeTop = 34;
constexpr float kButton = 28;
constexpr float kPad = 14;
constexpr float kNode = 30;
constexpr std::size_t kPastShown = 3;
constexpr int kRooms = 6; ///< an act's rows (server::DelveAct::kLength)
constexpr float kForkScale = 0.7f; ///< a fork's act: its rooms and rows this much smaller

/// Row `row`'s rooms (server::DelveAct::width): one, two ... five, the guardian's one.
int widthOf(int row)
{
    return row >= kRooms - 1 ? 1 : row + 1;
}

struct RoomLook {
    const char* mark;
    const char* name; ///< as the legend and a hover say it
    const char* icon; ///< the HD pack's "Delve/<icon>"
    SDL_Color fill;
};

// By server::DelveRoom's numbers: fight, elite, mystery, shop, campfire, guardian, blessing.
constexpr std::array<RoomLook, 7> kLooks{{
    {"F", "Fight", "Fight", {120, 70, 60, 255}},
    {"E", "Elite", "Elite", {130, 60, 150, 255}},
    {"?", "Mystery", "Mystery", {50, 90, 160, 255}},
    {"$", "Shop", "Shop", {170, 140, 40, 255}},
    {"C", "Campfire", "Campfire", {200, 100, 30, 255}},
    {"B", "Guardian", "Guardian", {170, 30, 30, 255}},
    {"+", "Blessing", "Shrine", {210, 190, 110, 255}},
}};
// A mystery once it's entered (server::DelveMystery's order: stash, purse, shrine, obelisk,
// skirmish, fire, champion), on the HD pack's icons that fit.
constexpr std::array<RoomLook, 7> kMysteries{{
    {"T", "Stash", "Treasure", {190, 160, 50, 255}},
    {"G", "Purse", "Treasure", {200, 170, 40, 255}},
    {"S", "Shrine", "Shrine", {70, 150, 170, 255}},
    {"O", "Obelisk", "Mystery", {60, 90, 200, 255}},
    {"A", "Skirmish", "Ambush", {150, 40, 40, 255}},
    {"C", "Campfire", "Campfire", {200, 100, 30, 255}},
    {"E", "Champion", "Elite", {130, 60, 150, 255}},
}};

const RoomLook& look(std::uint8_t room)
{
    return kLooks[std::min<std::size_t>(room, kLooks.size() - 1)];
}

using box::inside;
using box::outline;

// A filled disc as a fan of 24 triangles.
void disc(SDL_Renderer* renderer, float cx, float cy, float radius, SDL_Color c)
{
    constexpr int kSides = 24;
    const SDL_FColor fc{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
    std::vector<SDL_Vertex> v;
    v.reserve(kSides * 3);
    for (int i = 0; i < kSides; ++i) {
        const float a0 = 2 * std::numbers::pi_v<float> * static_cast<float>(i) / kSides;
        const float a1 = 2 * std::numbers::pi_v<float> * static_cast<float>(i + 1) / kSides;
        v.push_back({{cx, cy}, fc, {}});
        v.push_back({{cx + radius * std::cos(a0), cy + radius * std::sin(a0)}, fc, {}});
        v.push_back({{cx + radius * std::cos(a1), cy + radius * std::sin(a1)}, fc, {}});
    }
    SDL_RenderGeometry(renderer, nullptr, v.data(), static_cast<int>(v.size()), nullptr, 0);
}

/// A path between two rooms: dots (a way ahead; faint, one out of reach) or a solid band (the
/// way walked).
void path(SDL_Renderer* renderer, SDL_FPoint a, SDL_FPoint b, float s, bool walked, bool faint = false)
{
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1) {
        return;
    }
    if (walked) {
        SDL_SetRenderDrawColor(renderer, 230, 180, 60, 255);
        const float nx = -dy / length;
        const float ny = dx / length;
        for (float o = -1.0f * s; o <= 1.0f * s; o += 0.5f) {
            SDL_RenderLine(renderer, a.x + nx * o, a.y + ny * o, b.x + nx * o, b.y + ny * o);
        }
        return;
    }
    if (faint) {
        SDL_SetRenderDrawColor(renderer, 75, 66, 50, 255);
    } else {
        SDL_SetRenderDrawColor(renderer, 165, 145, 105, 255);
    }
    const float gap = 7 * s;
    const float dot = std::max(1.5f, 2.2f * s);
    for (float t = gap / 2; t < length; t += gap) {
        const SDL_FRect d{a.x + dx * t / length - dot / 2, a.y + dy * t / length - dot / 2, dot, dot};
        SDL_RenderFillRect(renderer, &d);
    }
}

// FNV-1a over the biome, act and room, folded to -11 .. 11 units.
/// A small steady sideways wander for a room (the same each time it's drawn).
float wander(const std::string& biome, int act, int room)
{
    std::uint32_t h = 2166136261u;
    for (const char c : biome) {
        h = (h ^ static_cast<std::uint8_t>(c)) * 16777619u;
    }
    h = (h ^ static_cast<std::uint32_t>(act * 31 + room)) * 16777619u;
    return static_cast<float>(static_cast<int>(h % 23u) - 11);
}

// A biome's title as the map names it (its id when unknown).
std::string titleOf(const std::string& biome)
{
    const auto* b = nox::mapgen::findBiome(biome);
    return b ? std::string(b->title) : biome;
}

// The biome's climate note after its title, if it has one.
std::string airOf(const std::string& biome)
{
    const auto* b = nox::mapgen::findBiome(biome);
    if (!b) {
        return {};
    }
    switch (b->climate) {
    case nox::mapgen::Climate::Freezing: return " (bitter cold: slowed)";
    case nox::mapgen::Climate::Scorching: return " (searing heat: burns)";
    case nox::mapgen::Climate::Mild: break;
    }
    return {};
}

} // namespace

// Closes the map when a run starts or ends, and when there's nothing left to show.
void DelveMapWindow::update(const std::optional<Run>& run)
{
    // (The map put away as a run ends; the records' table, with no run, stays its own.)
    if (run.has_value() != hadRun_) {
        open_ = false;
        hadRun_ = run.has_value();
    }
    if (!run && !hasRecords()) {
        open_ = false;
    }
}

void DelveMapWindow::setObjective(const nox::server::delvemap::Objective* objective, std::uint32_t version)
{
    objective_ = objective;
    if (version != objectiveVersion_) {
        objectiveVersion_ = version;
        objectiveAtMs_ = SDL_GetTicks();
    }
}

void DelveMapWindow::drawObjective(const SDL_FRect& button, float scale) const
{
    // "Bonus: Break the seals (touch the orb pools) 1/3", or its clock; gold once done, grey once
    // lost. Right of it the button.
    if (!objective_ || objective_->kind == 0) {
        return;
    }
    const nox::server::delvemap::Objective& o = *objective_;
    std::string line = (o.state == 1 ? "Bonus done: " : o.state == 2 ? "Bonus lost: " : "Bonus: ") + o.text;
    if (o.state == 0 && o.secondsLeft != 0xFFFF) {
        const auto gone = static_cast<std::int64_t>((SDL_GetTicks() - objectiveAtMs_) / 1000);
        const auto left = std::max<std::int64_t>(0, o.secondsLeft - gone);
        line += " " + std::to_string(left / 60) + ":" + (left % 60 < 10 ? "0" : "") + std::to_string(left % 60);
    } else if (o.state == 0 && o.of > 1) {
        line += " " + std::to_string(o.done) + "/" + std::to_string(o.of);
    }
    // 11 game pixels high at the HUD's scale, as a font size (at least 8).
    const int text = font_->lineSize(11, scale, 8);
    const SDL_Color c = o.state == 1 ? SDL_Color{255, 200, 60, 255} : o.state == 2 ? SDL_Color{150, 150, 150, 255} : SDL_Color{235, 235, 235, 255};
    const float tw = font_->measure(line, text);
    const float x = button.x - 8 * scale - tw;
    const float y = button.y + (button.h - font_->lineHeight(text)) / 2;
    font_->draw(line, x + 1, y + 1, text, {0, 0, 0, 200}); // (a shadow: over the floor)
    font_->draw(line, x, y, text, c);
}

SDL_FRect DelveMapWindow::buttonRect(int w, int h, float scale) const
{
    const float s = scale;
    const float W = static_cast<float>(w) / s;
    const float H = static_cast<float>(h) / s;
    return {(W - kMeterW - kButton - 4) * s, (H - kMeterH + kTubeTop) * s, kButton * s, kButton * s};
}

void DelveMapWindow::drawButton(SDL_Renderer* renderer, const SDL_FRect& button, std::string_view label, float scale, SDL_FPoint mouse) const
{
    // A dark square, gold edged, its word.
    const SDL_Color white{235, 235, 235, 255};
    const SDL_Color gold{255, 200, 60, 255};
    const int text = font_->lineSize(11, scale, 8);
    const bool hover = inside(button, mouse.x, mouse.y);
    SDL_SetRenderDrawColor(renderer, 20, 18, 14, 210);
    SDL_RenderFillRect(renderer, &button);
    outline(renderer, button, hover || open_ ? gold : SDL_Color{150, 120, 60, 255});
    const int size = std::max(7, static_cast<int>(std::round(text * 0.8f)));
    const std::string word(label);
    const float tw = font_->measure(word, size);
    font_->draw(word, button.x + (button.w - tw) / 2, button.y + (button.h - font_->lineHeight(size)) / 2, size, hover || open_ ? gold : white);
}

SDL_FRect DelveMapWindow::recordsPanel(int w, int h, float scale) const
{
    const float s = scale;
    const int text = font_->lineSize(11, s, 8);
    const float line = font_->lineHeight(text);
    const std::size_t rows = records_ ? std::min<std::size_t>(records_->size(), 10) : 0;
    const float pw = std::min(static_cast<float>(w) * 0.9f, 620 * s);
    const float ph = kPad * s * 2 + line * (3.6f + static_cast<float>(rows) * 1.1f); // (the title, the rows, the footer)
    return {std::floor((static_cast<float>(w) - pw) / 2), std::floor(std::max(8 * s, (static_cast<float>(h) - ph) / 2 - 40 * s)), pw, ph};
}

void DelveMapWindow::drawRecords(SDL_Renderer* renderer, int w, int h, float scale) const
{
    // The deepest delves: depth, act, the party and its classes (MultiClass for a NoX Core player
    // that played it), the date.
    const float s = scale;
    const int text = font_->lineSize(11, s, 8);
    const float line = font_->lineHeight(text);
    const SDL_FRect p = recordsPanel(w, h, scale);
    SDL_SetRenderDrawColor(renderer, 14, 11, 7, 245);
    SDL_RenderFillRect(renderer, &p);
    outline(renderer, p, {110, 90, 50, 255});
    const SDL_Color gold{255, 200, 60, 255};
    const SDL_Color white{235, 235, 235, 255};
    const SDL_Color grey{150, 150, 150, 255};
    const std::string title = "The deepest delves";
    font_->draw(title, p.x + (p.w - font_->measure(title, text)) / 2, p.y + kPad * s, text, gold);
    float y = p.y + kPad * s + line * 1.6f;
    for (std::size_t i = 0; records_ && i < records_->size() && i < 10; ++i) {
        const auto& r = (*records_)[i];
        std::string party;
        for (const auto& [name, cls] : r.party) {
            party += (party.empty() ? "" : ", ") + name + " (" + std::string(nox::server::recordClassName(cls)) + ")";
        }
        const std::string head = std::to_string(i + 1) + ".  depth " + std::to_string(r.depth) + "  (act " + std::to_string(r.act) + ")";
        font_->draw(head, p.x + kPad * s, y, text, i == 0 ? gold : white);
        const float dw = font_->measure(r.date, text);
        font_->draw(r.date, p.x + p.w - kPad * s - dw, y, text, grey);
        std::string who = party;
        const float px = p.x + kPad * s + 150 * s;
        while (who.size() > 1 && px + font_->measure(who, text) > p.x + p.w - kPad * s - dw - 10 * s) {
            who.pop_back();
        }
        font_->draw(who, px, y, text, i == 0 ? gold : white);
        y += line * 1.1f;
    }
    // The rest online.
    const std::string more(nox::server::kFullLeaderboard);
    font_->draw(more, p.x + (p.w - font_->measure(more, text)) / 2, p.y + p.h - kPad * s - line, text, grey);
}

// Where everything goes for this run on a w x h screen: the panel, the step between rows, the
// spread within a row, the fork's lanes and the middle line.
DelveMapWindow::Layout DelveMapWindow::layout(const Run& run, int w, int h, float scale) const
{
    Layout l;
    const float s = scale;
    l.button = buttonRect(w, h, scale);
    l.text = font_->lineSize(11, s, 8);
    l.line = font_->lineHeight(l.text);
    // Left to right: the act's six rows about the middle, then (a fork ahead) its guardian's two
    // gates' acts about lanes above (the left gate) and below (the right); the title over it all,
    // the acts done under it.
    const std::size_t past = std::min(run.past.size(), kPastShown);
    const float columns = run.fork ? 2.0f * kRooms + 0.5f : static_cast<float>(kRooms); // (a half step to the fork)
    l.node = kNode * s;
    const float pw = std::min(static_cast<float>(w) * 0.96f, 1150 * s);
    l.step = std::clamp((pw - 2 * kPad * s - 3 * l.node) / (columns - 1), 44 * s, 110 * s);
    l.spread = l.node * 1.3f;
    // (The widest row, five, spans four spreads; a fork's two clear of each other.)
    const float actHalf = 2 * l.spread + l.node * 0.6f;
    const float forkHalf = (2 * l.spread + l.node * 0.6f) * kForkScale;
    l.lane = run.fork ? forkHalf + l.node * 0.6f + l.line : 0;
    // From the top: the title; the upper fork's names and act; the middle; the lower fork and its
    // names (the act's own name under its rows); the past.
    const float above = run.fork ? std::max(actHalf, l.lane + forkHalf + l.line * 2.3f) : actHalf;
    l.middle = kPad * s + l.line * 1.6f + above;
    const float below = (run.fork ? std::max(actHalf + l.line * 1.4f, l.lane + forkHalf + l.line * 2.4f) : actHalf + l.line * 1.4f) + l.line * 0.6f;
    const float ph = l.middle + below + l.line * static_cast<float>(past) + kPad * s;
    // (Centred, but clear of the meters at the bottom right when it can be.)
    const float clearOfMeters = static_cast<float>(h) - (kMeterH + 8) * s - ph;
    const float py = std::max(8 * s, std::min((static_cast<float>(h) - ph) / 2, clearOfMeters));
    l.panel = {std::floor((static_cast<float>(w) - pw) / 2), std::floor(py), pw, ph};
    l.middle += l.panel.y;
    l.left = l.panel.x + (pw - (columns - 1) * l.step) / 2;
    return l;
}

void DelveMapWindow::draw(SDL_Renderer* renderer, const std::optional<Run>& run, int w, int h, float scale, SDL_FPoint mouse)
{
    if (!font_) {
        return;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    if (!run) {
        // No run (the lobby): the records' table behind a "TOP" button, when there are any.
        if (hasRecords()) {
            drawButton(renderer, buttonRect(w, h, scale), "TOP", scale, mouse);
            if (open_) {
                drawRecords(renderer, w, h, scale);
            }
        }
        return;
    }
    const Layout l = layout(*run, w, h, scale);
    const SDL_Color white{235, 235, 235, 255};
    const SDL_Color grey{150, 150, 150, 255};
    const SDL_Color gold{255, 200, 60, 255};
    const SDL_Color parchment{200, 190, 160, 255};

    drawButton(renderer, l.button, "MAP", scale, mouse);
    drawObjective(l.button, scale);
    if (!open_) {
        return;
    }

    SDL_SetRenderDrawColor(renderer, 14, 11, 7, 252);
    SDL_RenderFillRect(renderer, &l.panel);
    outline(renderer, l.panel, {110, 90, 50, 255});
    const float s = scale;
    const float x0 = l.panel.x + kPad * s;
    const float cx = l.panel.x + l.panel.w / 2;
    const auto centred = [&](const std::string& text, float x, float y, SDL_Color c) {
        font_->draw(text, x - font_->measure(text, l.text) / 2, y, l.text, c);
    };
    centred("The Delve - depth " + std::to_string(run->depth), cx, l.panel.y + kPad * s, gold);
    if (hasRecords()) { // (the record to beat, at the top right)
        const auto& best = records_->front();
        const std::string line = "Record: depth " + std::to_string(best.depth) + (run->depth > best.depth ? " - beaten!" : "");
        font_->draw(line, l.panel.x + l.panel.w - kPad * s - font_->measure(line, l.text), l.panel.y + kPad * s, l.text,
                    run->depth > best.depth ? gold : grey);
    }

    // Where each room goes: the act's rows about the middle from the left, the fork's about its
    // lanes above (the left gate's) and below (the right's), a half step on, smaller; each room
    // wandering a little.
    const std::size_t past = std::min(run->past.size(), kPastShown);
    const int actNumber = static_cast<int>(run->past.size()) + 1;
    const auto roomAt = [&](int side, int row, int i, const std::string& biome) {
        const float scale = side == 0 ? 1.0f : kForkScale;
        const float column = side == 0 ? static_cast<float>(row) : static_cast<float>(kRooms + row) + 0.5f;
        const float down = (static_cast<float>(i) - static_cast<float>(widthOf(row) - 1) / 2) * l.spread * scale;
        const float lane = l.middle + static_cast<float>(side) * l.lane;
        // (Each act its own wander: ours, the fork's left one act on, its right two.)
        const int wanderAct = side == 0 ? actNumber : actNumber + (side < 0 ? 1 : 2);
        return SDL_FPoint{l.left + column * l.step, lane + down + wander(biome, wanderAct, row * 8 + i) * 0.5f * s * scale};
    };
    struct Room {
        SDL_FPoint at;
        const RoomLook* look;
        float size = 1;      ///< of a room's
        bool passed = false; ///< walked through
        bool ours = false;
        bool out = false;    ///< out of reach: one passed by, or not on a way on from ours
    };
    // Each act's rooms row by row: index[row][i] into `rooms`.
    std::vector<Room> rooms;
    using Index = std::array<std::vector<std::size_t>, kRooms>;
    const auto place = [&](const nox::server::delvemap::Act& a, int side) {
        Index index;
        for (int row = 0; row < kRooms; ++row) {
            const auto& kinds = a.rows[static_cast<std::size_t>(row)];
            for (int i = 0; i < widthOf(row) && i < static_cast<int>(kinds.size()); ++i) {
                index[static_cast<std::size_t>(row)].push_back(rooms.size());
                rooms.push_back({roomAt(side, row, i, a.biome), &look(kinds[static_cast<std::size_t>(i)]), side == 0 ? 1.0f : kForkScale});
            }
        }
        return index;
    };
    const Index act = place(run->act, 0);
    // Our act: `walked` holds the lane taken in each row so far, `here` is our row, `lane` our lane in it.
    const auto& walked = run->act.path;
    const int here = run->room;
    const int lane = here < static_cast<int>(walked.size()) ? walked[static_cast<std::size_t>(here)] : 0;
    for (int row = 0; row < kRooms; ++row) {
        for (std::size_t i = 0; i < act[static_cast<std::size_t>(row)].size(); ++i) {
            Room& r = rooms[act[static_cast<std::size_t>(row)][i]];
            const bool onPath = row < static_cast<int>(walked.size()) && walked[static_cast<std::size_t>(row)] == i;
            r.passed = row < here && onPath;
            r.ours = row == here && onPath;
            // (Ahead, reached from ours: row r's lanes lane .. lane + (r - here); the guardian.)
            const int k = static_cast<int>(i);
            r.out = row == kRooms - 1 ? false : row > here ? k < lane || k > lane + (row - here) : !onPath;
            if (r.ours && run->act.rows[static_cast<std::size_t>(row)][i] == 2 && run->mystery < kMysteries.size()) {  // kind 2: a mystery room
                r.look = &kMysteries[run->mystery]; // (a mystery entered: what it was)
            }
        }
    }
    std::array<Index, 2> forks;
    // Past the guardian, the acts the two gates lead to: the left's lane above (-1), the right's below (+1).
    if (run->fork) {
        forks[0] = place((*run->fork)[0], -1);
        forks[1] = place((*run->fork)[1], 1);
    }

    // The paths, under the rooms: each room to the two it leads on to (the fifth row's to the
    // guardian), the way walked in gold, ways out of reach faint; then from the guardian into
    // each fork.
    const auto paths = [&](const Index& index, bool ours) {
        for (int row = 0; row + 1 < kRooms; ++row) {
            const auto& from = index[static_cast<std::size_t>(row)];
            const auto& to = index[static_cast<std::size_t>(row + 1)];
            for (std::size_t i = 0; i < from.size(); ++i) {
                // A room leads on to the same lane and the next in the row after; the guardian's row has one.
                for (const std::size_t j : {i, i + 1}) {
                    const std::size_t k = row + 1 == kRooms - 1 ? 0 : j;
                    if (k >= to.size() || (row + 1 == kRooms - 1 && j != i)) {
                        continue;
                    }
                    const bool gold = ours && row + 1 < static_cast<int>(walked.size()) && row + 1 <= here && walked[static_cast<std::size_t>(row)] == i &&
                                      walked[static_cast<std::size_t>(row + 1)] == k;
                    const bool faint = ours && !gold && (rooms[from[i]].out || rooms[to[k]].out || row < here);
                    path(renderer, rooms[from[i]].at, rooms[to[k]].at, s * rooms[from[i]].size, gold, faint);
                }
            }
        }
    };
    paths(act, true);
    if (run->fork) {
        for (const Index& f : forks) {
            // From the guardian to the fork's first room.
            if (!f[0].empty() && !act[kRooms - 1].empty()) {
                path(renderer, rooms[act[kRooms - 1][0]].at, rooms[f[0][0]].at, s, false);
            }
            paths(f, false);
        }
    }

    // The rooms: an icon (or a disc and its letter), dimmed once passed or out of reach, ours
    // ringed.
    const Room* hovered = nullptr;
    for (const Room& r : rooms) {
        const float half = l.node * r.size / 2;
        const Sprite* icon = icons_ ? icons_(r.look->icon) : nullptr;
        const float iconSize = l.node * r.size * 1.2f;
        if (r.ours) {
            disc(renderer, r.at.x, r.at.y, (icon ? iconSize / 2 : half) + 4 * s, gold);
        }
        const bool dim = r.passed || r.out;
        if (icon && icon->texture) {
            const SDL_FRect dst{r.at.x - iconSize / 2, r.at.y - iconSize / 2, iconSize, iconSize};
            const Uint8 shade = r.passed ? 110 : r.out ? 80 : 255;
            SDL_SetTextureColorMod(icon->texture, shade, shade, shade);
            drawSprite(renderer, *icon, nullptr, dst);
            SDL_SetTextureColorMod(icon->texture, 255, 255, 255);
        } else {
            SDL_Color fill = r.look->fill;
            if (dim) {
                fill = {static_cast<Uint8>(fill.r / 3), static_cast<Uint8>(fill.g / 3), static_cast<Uint8>(fill.b / 3), 255};
            }
            disc(renderer, r.at.x, r.at.y, half, {20, 16, 10, 255});
            disc(renderer, r.at.x, r.at.y, half - 2 * s, fill);
            const int size = r.size < 1 ? std::max(7, static_cast<int>(std::round(l.text * 0.8f))) : l.text;
            const float mw = font_->measure(r.look->mark, size);
            font_->draw(r.look->mark, r.at.x - mw / 2, r.at.y - font_->lineHeight(size) / 2, size, dim ? grey : white);
        }
        const float dx = mouse.x - r.at.x;
        const float dy = mouse.y - r.at.y;
        if (dx * dx + dy * dy <= half * half * 1.6f) {
            hovered = &r;
        }
    }

    // The names: the act's under its rows; the left gate's over its act, the right's under its.
    {
        const std::string name = "Act " + std::to_string(actNumber) + ": " + titleOf(run->act.biome) + airOf(run->act.biome);
        font_->draw(name, l.left - l.node / 2, l.middle + 2 * l.spread + l.node * 0.75f, l.text, white);
    }
    if (run->fork) {
        const float forkHalf = (2 * l.spread + l.node * 0.6f) * kForkScale;
        for (std::size_t f = 0; f < 2; ++f) {
            const auto& a = (*run->fork)[f];
            if (forks[f][0].empty()) {
                continue;
            }
            const float x = rooms[forks[f][0][0]].at.x - l.node / 2;
            const float laneY = l.middle + (f == 0 ? -l.lane : l.lane);
            const float y0 = f == 0 ? laneY - forkHalf - l.line * 2.2f : laneY + forkHalf + l.line * 0.2f;
            font_->draw((f == 0 ? "Left gate - act " : "Right gate - act ") + std::to_string(actNumber + 1), x, y0, l.text, parchment);
            font_->draw(titleOf(a.biome) + airOf(a.biome), x, y0 + l.line, l.text, parchment);
        }
    }

    // Under it all: the acts done (the last few) with the gate taken out of each.
    float y = l.panel.y + l.panel.h - kPad * s - l.line * static_cast<float>(past);
    const std::size_t first = run->past.size() > kPastShown ? run->past.size() - kPastShown : 0;
    for (std::size_t i = first; i < run->past.size(); ++i) {
        const auto& a = run->past[i];
        const std::string gate = a.taken == 0 ? " - left gate" : a.taken == 1 ? " - right gate" : "";
        font_->draw("Act " + std::to_string(i + 1) + ": " + titleOf(a.biome) + gate, x0, y, l.text, grey);
        y += l.line;
    }

    // A room under the pointer: its name by it.
    if (hovered) {
        const std::string name = hovered->look->name;
        const float tw = font_->measure(name, l.text) + 8 * s;
        const SDL_FRect tip{hovered->at.x - tw / 2, hovered->at.y + l.node / 2 + 4 * s, tw, l.line + 4 * s};
        SDL_SetRenderDrawColor(renderer, 30, 24, 16, 235);
        SDL_RenderFillRect(renderer, &tip);
        outline(renderer, tip, {150, 120, 60, 255});
        font_->draw(name, tip.x + 4 * s, tip.y + 2 * s, l.text, white);
    }
}

// The button toggles the window; Escape closes it; clicks on the open panel are taken.
bool DelveMapWindow::handleEvent(const SDL_Event& event, const std::optional<Run>& run, int w, int h, float scale)
{
    if (!font_ || (!run && !hasRecords())) {
        return false;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && open_) {
        open_ = false;
        return true;
    }
    if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN && event.type != SDL_EVENT_MOUSE_BUTTON_UP) {
        return false;
    }
    const SDL_FRect button = buttonRect(w, h, scale);
    const SDL_FRect panel = run ? layout(*run, w, h, scale).panel : recordsPanel(w, h, scale);
    const float x = event.button.x;
    const float y = event.button.y;
    if (inside(button, x, y)) {
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
            open_ = !open_;
        }
        return true;
    }
    return open_ && inside(panel, x, y);
}

} // namespace noxsdl
