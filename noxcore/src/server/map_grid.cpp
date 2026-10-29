// The map grid and the units update's lists (map_grid.hpp). An entry's handle is its pool index + 1
// (0: none); a node's is entry x 5 + its node + 1, node 0 the point list's, 1-4 the extent lists'.
#include "noxcore/server/map_grid.hpp"

#include "noxcore/server/world.hpp"

namespace nox::server {

bool MapGrid::linked(const ServerObject& o) const noexcept
{
    return o.grid.v_ != 0 && pool_[o.grid.v_ - 1].linked;
}

const MapGrid::Rect* MapGrid::box(const ServerObject& o) const noexcept
{
    return o.grid.v_ != 0 ? &pool_[o.grid.v_ - 1].box : nullptr;
}

bool MapGrid::updatable(const ServerObject& o) const noexcept
{
    return o.grid.v_ != 0 && pool_[o.grid.v_ - 1].list == 1;
}

// Its pool entry: a given-back one reused, or a new one, the first time it's linked or listed.
std::uint32_t MapGrid::entryOf(ServerObject& o)
{
    if (o.grid.v_ != 0) {
        return o.grid.v_ - 1;
    }
    std::uint32_t ei = 0;
    if (!free_.empty()) {
        ei = free_.back();
        free_.pop_back();
        pool_[ei] = Entry{};
    } else {
        ei = static_cast<std::uint32_t>(pool_.size());
        pool_.emplace_back();
    }
    pool_[ei].object = &o;
    pool_[ei].live = true;
    o.grid.v_ = ei + 1;
    return ei;
}

void MapGrid::addNode(std::uint32_t ei, int k, int cx, int cy, bool extent)
{
    // At the cell list's head.
    Node& n = pool_[ei].nodes[static_cast<std::size_t>(k)];
    const std::uint32_t h = ei * 5 + static_cast<std::uint32_t>(k) + 1;
    Cell& c = cell(cx, cy);
    std::uint32_t& head = extent ? c.extent : c.point;
    n.cx = static_cast<std::int16_t>(cx);
    n.cy = static_cast<std::int16_t>(cy);
    n.prev = 0;
    n.next = head;
    if (head != 0) {
        node(head).prev = h;
    }
    head = h;
}

void MapGrid::removeNode(std::uint32_t ei, int k, bool extent)
{
    // Its own links are left as they were.
    Node& n = pool_[ei].nodes[static_cast<std::size_t>(k)];
    if (n.prev == 0) {
        Cell& c = cell(n.cx, n.cy);
        (extent ? c.extent : c.point) = n.next;
    } else {
        node(n.prev).next = n.next;
    }
    if (n.next != 0) {
        node(n.next).prev = n.prev;
    }
}

void MapGrid::link(ServerObject& o, const Rect& box, float x, float y, bool missile)
{
    const std::uint32_t ei = entryOf(o);
    Entry& e = pool_[ei];
    e.missile = missile;
    e.box = box;
    if (!missile) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        span(box, x0, y0, x1, y1);
        e.count = 0;
        for (int cy = y0; cy <= y1; ++cy) {
            for (int cx = x0; cx <= x1; ++cx) {
                if (pool_[ei].count < 4) { // (four cells at most, the rest left out)
                    const int k = 1 + pool_[ei].count++;
                    addNode(ei, k, cx, cy, true);
                }
            }
        }
    }
    // Its point (unclamped in the exe: a point off the grid writes past it, which no
    // map's things reach; clamped here).
    int cx = cellOf(x);
    int cy = cellOf(y);
    cx = cx < 0 ? 0 : (kSize <= cx ? kSize - 1 : cx);
    cy = cy < 0 ? 0 : (kSize <= cy ? kSize - 1 : cy);
    addNode(ei, 0, cx, cy, false);
    pool_[ei].linked = true;
}

void MapGrid::setMissileBox(ServerObject& o, const Rect& box) noexcept
{
    if (o.grid.v_ != 0 && pool_[o.grid.v_ - 1].linked && pool_[o.grid.v_ - 1].missile) {
        pool_[o.grid.v_ - 1].box = box;
    }
}

void MapGrid::unlink(ServerObject& o)
{
    if (o.grid.v_ == 0 || !pool_[o.grid.v_ - 1].linked) {
        return;
    }
    const std::uint32_t ei = o.grid.v_ - 1;
    removeNode(ei, 0, false);
    if (!pool_[ei].missile) {
        for (int k = 0; k < pool_[ei].count; ++k) {
            removeNode(ei, k + 1, true);
        }
        pool_[ei].count = 0;
    }
    pool_[ei].linked = false;
}

// Onto the head of a list (`which`: 1 updatable, 2 missiles), unless it's on one already.
void MapGrid::listAdd(ServerObject& o, std::uint32_t& head, std::uint8_t which)
{
    const std::uint32_t ei = entryOf(o);
    Entry& e = pool_[ei];
    if (e.list != 0) {
        return;
    }
    e.listPrev = 0;
    e.listNext = head;
    if (head != 0) {
        pool_[head - 1].listPrev = ei + 1;
    }
    head = ei + 1;
    pool_[ei].list = which;
}

void MapGrid::listRemove(ServerObject& o, std::uint32_t& head)
{
    const std::uint32_t ei = o.grid.v_ - 1;
    Entry& e = pool_[ei];
    if (e.listPrev == 0) {
        head = e.listNext;
    } else {
        pool_[e.listPrev - 1].listNext = e.listNext;
    }
    if (e.listNext != 0) {
        pool_[e.listNext - 1].listPrev = e.listPrev;
    }
    e.list = 0;
}

void MapGrid::listAppend(std::span<ServerObject* const> objects, std::uint32_t& head, std::uint8_t which)
{
    std::uint32_t tail = 0;
    for (std::uint32_t h = head; h != 0; h = pool_[h - 1].listNext) {
        tail = h;
    }
    for (ServerObject* o : objects) {
        const std::uint32_t ei = entryOf(*o);
        Entry& e = pool_[ei];
        if (e.list != 0) {
            continue;
        }
        e.listPrev = tail;
        e.listNext = 0;
        if (tail != 0) {
            pool_[tail - 1].listNext = ei + 1;
        } else {
            head = ei + 1;
        }
        tail = ei + 1;
        e.list = which;
    }
}

void MapGrid::appendUpdatable(std::span<ServerObject* const> objects)
{
    listAppend(objects, updHead_, 1);
}

void MapGrid::appendMissiles(std::span<ServerObject* const> objects)
{
    listAppend(objects, missileHead_, 2);
}

void MapGrid::addUpdatable(ServerObject& o)
{
    // (Its Obj130 cleared as it joins.)
    if (o.grid.v_ == 0 || pool_[o.grid.v_ - 1].list == 0) {
        listAdd(o, updHead_, 1);
        o.hurtBy = 0;
    }
}

void MapGrid::removeUpdatable(ServerObject& o)
{
    // (Its Obj130 cleared as it leaves.)
    if (updatable(o)) {
        listRemove(o, updHead_);
        o.hurtBy = 0;
    }
}

void MapGrid::addMissile(ServerObject& o)
{
    listAdd(o, missileHead_, 2);
}

void MapGrid::removeMissile(ServerObject& o)
{
    if (o.grid.v_ != 0 && pool_[o.grid.v_ - 1].list == 2) {
        listRemove(o, missileHead_);
    }
}

void MapGrid::release(ServerObject& o)
{
    if (o.grid.v_ == 0) {
        return;
    }
    unlink(o);
    removeUpdatable(o);
    removeMissile(o);
    // (Not reused till no walk can be on it: its links stay as they were meanwhile.)
    pool_[o.grid.v_ - 1].live = false;
    freed_.push_back(o.grid.v_ - 1);
    o.grid.v_ = 0;
}

void MapGrid::recycle()
{
    free_.insert(free_.end(), freed_.begin(), freed_.end());
    freed_.clear();
}

void MapGrid::clear()
{
    for (Entry& e : pool_) {
        if (e.live) {
            e.object->grid.v_ = 0;
        }
    }
    pool_.clear();
    free_.clear();
    freed_.clear();
    cells_.assign(static_cast<std::size_t>(kSize) * kSize, Cell{});
    updHead_ = 0;
    missileHead_ = 0;
    wpNext_.clear();
}

void MapGrid::registerWaypoint(std::uint32_t id, float x, float y)
{
    // waypointMapRegister: at its cell's head (the cell of a point, as for an object's, not clamped).
    if (id == 0) {
        return;
    }
    if (wpNext_.size() < id) {
        wpNext_.resize(id, 0);
    }
    const int cx = cellOf(x);
    const int cy = cellOf(y);
    if (cx < 0 || kSize <= cx || cy < 0 || kSize <= cy) {
        return;
    }
    Cell& c = cell(cx, cy);
    wpNext_[id - 1] = c.waypoint;
    c.waypoint = id;
}

std::vector<const ServerObject*> MapGrid::cellList(int cx, int cy, bool extent) const
{
    std::vector<const ServerObject*> out;
    const Cell& c = cell(cx, cy);
    for (std::uint32_t h = extent ? c.extent : c.point; h != 0; h = pool_[(h - 1) / 5].nodes[(h - 1) % 5].next) {
        out.push_back(pool_[(h - 1) / 5].object);
    }
    return out;
}

std::vector<std::pair<int, int>> MapGrid::cellsOf(const ServerObject& o) const
{
    std::vector<std::pair<int, int>> out;
    if (!linked(o)) {
        return out;
    }
    const Entry& e = pool_[o.grid.v_ - 1];
    out.emplace_back(e.nodes[0].cx, e.nodes[0].cy);
    for (int k = 0; k < e.count; ++k) {
        out.emplace_back(e.nodes[static_cast<std::size_t>(k + 1)].cx, e.nodes[static_cast<std::size_t>(k + 1)].cy);
    }
    return out;
}

} // namespace nox::server
