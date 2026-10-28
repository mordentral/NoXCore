// The client's line of sight as drawBlack works it out every frame, and what the wall drawing
// (drawWalls) and the soft shadow edge take from it.
//
// drawBlack:
//   occluders  every wall cell in view (the even cells of the wall grid) whose WALL def blocks
//              sight adds the edges from its centre (23x + 11, 23y + 11) toward its corners:
//              directions 0 and 1 one full diagonal each, the others a half-diagonal per arm
//              (1 NE, 2 NW, 4 SE, 8 SW). A window wall is see-through from close by. The screen's
//              four sides and the sight-blocking objects (a door's leaf, a circle, a box: the
//              face turned to the eye, or the diagonal across it when two faces are) are
//              occluders too
//   intervals  each occluder is the angular interval it covers seen from the eye (75000 steps a
//              turn, from an arctangent table), keyed by the squared distance to its middle. The
//              intervals are merged one by one into a sorted list: where two overlap, the one
//              whose middle is nearer keeps the angles (not a true depth test)
//   polygon    every interval left gives two points, where the rays at its ends meet its
//              occluder; clipped to the view, they make the visible polygon. A wall whose edge
//              made it into the list is seen, with the arms it was seen by and whether it faces
//              the eye from below (a front wall). The polygon's edges between two different
//              occluders are its shadow edges
//   spans      the polygon scan-converted into per-row crossings: the floor is drawn inside
//              them, black outside
//
// drawWalls draws only the walls seen: a wall seen by some of its arms only is drawn as the
// direction those arms make (a crossing seen from one side as a T or a straight wall), and only
// in the columns where the drawn direction's base line runs inside the spans. With
// SoftShadowEdge on (nox.cfg, engine flag 0x400), ten black lines fading out are drawn along every
// shadow edge into the seen floor.
//
// The drawable sight test (drawableSeen), which the scene's sort, its list draws and the cursor's
// hover ask of every drawable that isn't a sight blocker (those go by whether the sweep reached
// them) or our own unit:
//   line       the segment from the eye to its position (a gate's: its leaf's middle) crosses
//              none of the polygon's edges: seen
//   box        else its box on the screen: from its last image the generic drawable draw drew
//              (that image's width centred on it, its height up from its shape's bottom), or
//              from its shape and ZSIZE when it has never been drawn so. Wholly on the screen or
//              wholly off it: not seen. Partly: two points inside the box on its side towards the
//              screen (3 in from the sides past 6 wide, 11 from the top and bottom past 21 tall,
//              when it's sized by an image) must both be clear from the eye
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nox::client {

/// The last image the generic drawable draw drew of a drawable: the draw notes its width and
/// height on the drawable each time (never cleared, so it stays from the last draw). The drawable
/// sight test sizes the drawable's box by it once there is one.
struct DrawnImage {
    bool drawn = false;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
};

class SightRegion {
public:
    /// The world rectangle in view (the viewport's World corner and Size; one unit a pixel).
    struct View {
        int left = 0;
        int top = 0;
        int width = 0;
        int height = 0;
        /// Added to the eye's y: the screen shake as the scene draw has swung it for this frame,
        /// while the view (whose top already holds the shake the camera took) stays put. The eye
        /// so stays within a unit of the player through a shake.
        int eyeShiftY = 0;
    };

    /// A wall cell as the sight sees it.
    struct Wall {
        std::uint8_t x = 0;
        std::uint8_t y = 0;
        std::uint8_t direction = 0; ///< the facing (0-10)
        bool blocksSight = true;    ///< WALL def flag 1
        bool window = false;        ///< in the map's WindowWalls (wall flag 0x40)
    };

    /// A sight-blocking object: a door along its leaf from (x0, y0) to (x1, y1), a circle of
    /// `radius` round (x0, y0), or a box round (x0, y0).
    struct Blocker {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        float radius = 0; ///< > 0: a circle
        bool box = false; ///< a box: its corners from (x0, y0) in `corners`
        /// A box's corners from its position, as its shape lays them (a rectangle turned 45
        /// degrees): top, left, right, bottom, each x then y. boxCorners() makes them.
        std::array<float, 8> corners{};
    };
    /// The corners of a box `width` along (1, 1) and `height` along (-1, 1), as Blocker holds them.
    static std::array<float, 8> boxCorners(float width, float height);

    /// What a polygon point was found on (its edge to the next point); kShadowEdge marks a
    /// shadow edge once the polygon is finished.
    enum Kind : std::uint8_t {
        kWall = 0,
        kTop = 1,
        kBottom = 2,
        kLeft = 3,
        kRight = 4,
        kDoor = 6,
        kCircle = 7,
        kBoxTopLeft = 8,      ///< a box seen past its top-left face (top to left corner)
        kBoxTopRight = 9,     ///< past its top-right face (top to right corner)
        kBoxBottomLeft = 10,  ///< past its bottom-left face (bottom to left corner)
        kBoxBottomRight = 11, ///< past its bottom-right face (bottom to right corner)
        kShadowEdge = 12,
        kBoxUpright = 13,     ///< from its left or right: top to bottom corner
        kBoxAcross = 14,      ///< from above or below: left to right corner
    };

    struct Point {
        int x = 0; ///< world
        int y = 0;
        std::uint8_t kind = kWall;
        int id = 0; ///< the wall's y * 256 + x, the blocker's index + 1, 0 for the screen's sides
    };

    /// A wall's state after the sight is built (wall bytes 3 and 4).
    struct WallState {
        std::uint8_t edges = 0; ///< the arms it was seen by (1 NE, 2 NW, 4 SE, 8 SW)
        bool seen = false;      ///< drawn this frame
        bool front = false;     ///< faces the eye from below (drawn last, translucent)
        bool seeOver = false;   ///< its WALL def lets sight through (flag 1 off): never swept
    };

    /// How drawWalls draws a seen wall.
    struct WallDraw {
        int image = 0;     ///< the direction whose image is drawn (11 / 12: a window's; 13 / 14: half walls)
        int direction = 0; ///< the direction drawn (the arms seen, or the wall's own)
        std::optional<int> clipLeft;  ///< world x the image is cut at, on the left
        std::optional<int> clipRight; ///< and on the right (exclusive)
        bool front = false;
    };

    /// A soft shadow edge's lines: from (x0, y0) to (x1, y1) in world units, each next line one
    /// step of (stepX, stepY) further, at kSoftEdgeAlpha's alphas in black.
    struct SoftEdge {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        int stepX = 0;
        int stepY = 0;
    };
    static constexpr std::array<std::uint8_t, 10> kSoftEdgeAlpha{0xD0, 0xBC, 0xA8, 0x94, 0x80,
                                                                 0x6C, 0x58, 0x44, 0x30, 0x1C};

    /// Works the sight out for the view, with the walls standing (broken walls and open secret
    /// walls left out) and the sight-blocking objects in the order the client lists them.
    void build(const View& view, std::span<const Wall> walls, std::span<const Blocker> blockers = {});

    /// noxsdl's improved wall culling, a deliberate deviation, off by
    /// default (the game's). With it, wallDraw's column test doesn't cut a wall where its base is
    /// hidden only by the wall's own arms (a corner turning away behind its front arm), nor a
    /// see-over wall where its base is hidden only by a sight-blocking object (a door's leaf).
    /// Build the region for the walls over wallView(view) to also draw whole the walls whose base
    /// is just past the view's edges but whose image reaches into it.
    void setImprovedWalls(bool on) { improved_ = on; }
    bool improvedWalls() const { return improved_; }
    /// The wall images' reach past the view: a wall's image stands about 4 rows above its base and
    /// spreads about 2 columns either side of its cell (the scene draw lists that far too).
    static constexpr int kWallReachColumns = 2;
    static constexpr int kWallReachRows = 4;
    /// The view grown by that reach on every side (rows above too, so the eye stays where it is).
    static View wallView(const View& view);

    const View& view() const { return view_; }
    int eyeX() const { return view_.left + view_.width / 2; }
    int eyeY() const { return view_.top + view_.height / 2 + view_.eyeShiftY; }

    /// The visible polygon in world units, in angle order (clockwise on the screen).
    std::vector<Point> polygon() const;
    /// Its shadow edges' lines (SoftShadowEdge).
    std::vector<SoftEdge> softEdges() const;

    /// The wall's state, or null for a cell outside the view.
    const WallState* wall(int x, int y) const;
    /// Whether the blocker (its index in build's list) made it into the polygon.
    bool blockerSeen(std::size_t index) const;
    /// Whether the world point is inside the spans (the floor drawn there).
    bool spanContains(int x, int y) const;

    /// A drawable as the drawable sight test sees it.
    struct Target {
        int x = 0; ///< world: its position, or a gate's leaf's middle
        int y = 0;
        int z = 0;           ///< its height (the drawable's z)
        bool circle = false; ///< its shape is a CIRCLE of `radius`; any other goes by `corners`
        float radius = 0;
        std::array<float, 8> corners{}; ///< a BOX's, as boxCorners lays them; zero without one
        float zSizeMin = 0;             ///< ZSIZE
        float zSizeMax = 0;
        DrawnImage image; ///< the generic draw's last image of it
    };
    /// A box on the screen, view-relative (the polygon's units), its sides inclusive.
    struct ScreenBox {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
    };
    /// The box the drawable sight test makes for `t` with its position at (sx, sy) on the screen.
    static ScreenBox screenBox(const Target& t, int sx, int sy);
    /// The drawable sight test (see the top of this file).
    bool drawableSeen(const Target& t) const;
    /// Whether the segment between the screen points crosses none of the polygon's edges.
    bool lineClear(int ax, int ay, int bx, int by) const;
    /// Whether the screen point (view-relative) is inside the polygon by the game's even-odd test
    /// (Turn Undead's orbs go when theirs isn't): an edge counts when the point's row runs from
    /// one end's y (included) to short of the other's, and the point is left of where the edge
    /// crosses that row (worked out in whole numbers, chopped).
    bool insidePolygon(int sx, int sy) const;
    /// The game's segment test: whether segments a and b, each (x0, y0, x1, y1), meet (touching
    /// counts; collinear ones meet when they overlap).
    static bool segmentsMeet(const std::array<int, 4>& a, const std::array<int, 4>& b);
    /// How the wall is drawn, or nullopt when it isn't (not seen, or none of its base line in
    /// sight). `direction` is the wall's own.
    std::optional<WallDraw> wallDraw(int x, int y, std::uint8_t direction, bool window) const;

    /// The sweep's angle of (dx, dy): 0 east, 18750 south, 37500 west, 56250 north.
    static int angle(int dy, int dx);
    /// The direction made of the arms seen, or -1 (one arm or none: the wall's own).
    static int seenDirection(std::uint8_t edges);

private:
    struct Interval {
        int a0 = 0;
        int a1 = 0;
        int distance = 0;
        bool alive = true;
        std::uint8_t kind = kWall;
        std::uint8_t edges = 0; ///< a wall's: which arm(s)
        int cellX = 0;
        int cellY = 0;
        int blocker = -1;
    };
    struct ScreenPoint {
        int x = 0; ///< view-relative (the game's screen coordinates)
        int y = 0;
        std::uint8_t kind = kWall;
        int id = 0;
    };

    void pushInterval(Interval i);
    void addWallEdge(int x, int y, std::uint8_t edge);
    void sweep();
    void emitPoints(std::span<const Blocker> blockers);
    void markShadowEdges();
    void mergePoints();
    void scanConvert();
    /// Who a wall's columns are tested for (the improved culling's excuses).
    struct ColumnsFor {
        int cellX = -1;
        int cellY = -1;
        bool seeOver = false;
    };
    bool columns(int ax, int ay, int bx, int by, int& left, int& right, const ColumnsFor& self) const;
    /// (For no wall in particular. An overload, not a default argument: GCC won't take a nested
    /// class with member initializers as a default argument while the enclosing class is incomplete.)
    bool columns(int ax, int ay, int bx, int by, int& left, int& right) const
    {
        return columns(ax, ay, bx, by, left, right, ColumnsFor{});
    }
    /// Improved culling: whether the screen point, outside the spans, is hidden only by the wall's
    /// own arms or (a see-over wall) only by a sight-blocking object.
    bool excused(float sx, int row, const ColumnsFor& self) const;
    /// The interval of the sweep's list that holds the angle (the nearest occluder there).
    const Interval* occluderAt(int angle) const;
    WallState* wallAt(int x, int y);

    View view_;
    bool improved_ = false;
    int cellX0_ = 0; ///< the wall cells in view
    int cellY0_ = 0;
    int cellsW_ = 0;
    int cellsH_ = 0;
    std::vector<WallState> walls_;
    std::vector<bool> blockersSeen_;
    std::vector<Interval> input_; ///< (a stack, as the game's)
    std::vector<Interval> list_;  ///< sorted by start angle
    std::vector<ScreenPoint> points_;
    std::vector<std::vector<int>> rows_; ///< per screen row, the polygon's crossings in x, sorted
};

} // namespace nox::client
