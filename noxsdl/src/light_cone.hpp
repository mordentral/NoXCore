// A light's cone (a directional light) and its animation, as the game's client draws them. No SDL:
// the tests use it too.
//
// The light block (an InvisibleLight's, the drawable's): LightDir and LightCone are 16-bit angles
// (65536 a turn, 0 east, 16384 south), LightCone the cone's HALF width; LightOmni 0xFFFF means all
// round, any other value a cone.
//
// Drawing (cliLight16): a cone lights only the grid points inside the kite of the light P0 and
// three points a radius away, at LightDir (P1), LightDir + LightCone (P2) and LightDir - LightCone
// (P3) (the exe's 4096-entry sine table, in 16.16 grid units 23 world units each), filled as two
// triangles (P0, P2, P1) and (P0, P1, P3) by its scanline filler. Each point inside gets the omni
// light's formula (lighting.hpp) with no radius test, and always adds: the cone path passes "not
// anti" whatever the light's anti flag says. The filler fills only triangles wound one way on the
// screen, so a cone 180 degrees wide or more (or 0) lights nothing.
//
// Animation (ColorLightUpdateDraw, the client's: nothing about it crosses the network; the client
// reads the light block from its own copy of the map, colorLightClientLoad), each frame for a light
// within 100 units of the view, after the colour and intensity keys, and only while the light has at
// least one non-zero colour, intensity or cone key (else the update drops itself at its first run):
//   cone keys   (colorLightAlterRadius) with LightOmni exactly 0: the keys are half widths in
//               degrees, stepped through as the intensity keys are (flag 0x10 loops), the value
//               truncated to whole degrees
//   rotation    (colorLightRotate) flag 0x80, LightOmni exactly 0, LightRotSpeed (signed, degrees a
//               second) not 0: the direction is LightRotStart (LightDir in whole degrees, at load)
//               plus the fraction of the current period times 360, or with flag 0x100 ("sweep")
//               times LightRotMax - LightRotStart: a sawtooth from the start to the most, then back
//               to the start at once
//   aim         (colorLightAimAtTarget) flag 0x40 and a static (client-loaded) drawable with the
//               extent LightTarget: the direction toward it in whole degrees (acos as a float
//               times the float 57.2959); setting a direction clears LightOmni, so an all-round
//               light that aims becomes a cone
// The arithmetic is the x87's as the game runs it, in chop mode (each result cut toward zero), and
// the radius the kite reaches the drawable's whole LightRadius (truncated).
// Directions and widths in degrees become angles as spriteSetLightDir / spriteChangeLightSize do:
// trunc(degrees / 360 * 65536 + 0.5).
#pragma once

#include "noxcore/map/xfer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace noxsdl::lightcone {

inline constexpr std::uint32_t kOmni = 0xFFFF; ///< LightOmni for an all-round light

/// Where an animated light is in its keys at `frame` (colorLightAlterB0ArrayColor,
/// colorLightAlterIntensity, colorLightAlterRadius): n keys `step` frames apart make a cycle of
/// n * step frames; a looping animation runs to the last key then jumps to the first, and one
/// that doesn't runs back down on odd cycles.
struct KeySegment {
    std::size_t from = 0;
    std::size_t to = 0;
    int frames = 0;  ///< whole frames into the step, kept to a byte as the game does
    float along = 0; ///< frames / step
};
std::optional<KeySegment> keySegment(std::size_t n, std::uint16_t step, bool loop, std::uint32_t frame);
/// The value `frames` into a step between keys `from` and `to`, as the three key functions work it:
/// (to - from) / step * frames + from. Whole (__ftol: a colour component, a cone's degrees) or as
/// the float the intensity's is stored to. (Client maths, to the nearest: the exe's chop mode
/// differs only on exact boundaries.)
int keyWhole(int from, int to, std::uint16_t step, int frames);
float keyFloat(int from, int to, std::uint16_t step, int frames);

/// A light's radius for an intensity as the game makes it whole (lightRadiusFromIntensity: its
/// formula on the x87 in chop mode, then Math_DoubleToInt, which truncates): the radius a cone's
/// kite reaches (cliLight16 reads the drawable's LightRadius).
int radiusOf(float intensity);

/// The light grid's first lattice index (lattice point i at 23 i - 11) for a view whose left (or
/// top) edge is at `viewLeft`: the client's grid build puts it at ((view + 11) / 46 - 1) * 46 - 11.
int gridOriginIndex(float viewLeft);
/// The game's light grid: 57 points across, 45 down (where a cone's filler clips: lighting.cpp).
inline constexpr int kGridCols = 57;
inline constexpr int kGridRows = 45;

/// Degrees to a 16-bit angle (spriteSetLightDir, spriteChangeLightSize).
std::uint16_t angleFromDegrees(int degrees);
/// A 16-bit angle in whole degrees (colorLightClientLoad's LightRotStart).
int degreesFromAngle(std::uint16_t angle);
/// The game's sine and cosine of a 16-bit angle, 16.16 (its 4096-entry table, 4.12, shifted).
std::int32_t sinFixed(std::uint16_t angle);
std::int32_t cosFixed(std::uint16_t angle);

/// A cone: where it points and its half width, 16-bit angles.
struct Cone {
    std::uint16_t direction = 0;
    std::uint16_t width = 0;
};

/// The kite in world units: [0] the light, [1] a radius out at the direction, [2] at direction +
/// width, [3] at direction - width.
using Kite = std::array<std::array<float, 2>, 4>;
Kite kite(float x, float y, float radius, Cone cone);
/// Whether the cone lights world point (px, py): inside (P0, P2, P1) or (P0, P1, P3) while those
/// are wound the way the game's filler fills.
bool contains(const Kite& k, float px, float py);

/// The grid points the game lights for a cone (cliLight16's scanline filler, exactly): the light
/// at (relX, relY) world units from grid point (0, 0), points 23 units apart, `cols` x `rows` of
/// them. Like the game's, it never fills the last row and stops spans short of the last column.
/// `out` gets (column, row) pairs (a point may come twice: the game lights it twice).
void rasterize(int relX, int relY, int radius, Cone cone, int cols, int rows, std::vector<std::pair<int, int>>& out);
/// The points of our light grid (`cols` x `rows`, its first point lattice (originI, originJ):
/// gridOriginIndex's) a cone of the light at world (x, y) lights: rasterize on the game's grid
/// from the same first point, 57 x 45 (kGridCols, kGridRows) as the game clips, or past our
/// grid's far edges when a view bigger than the game's needs it; those outside ours left out.
void gridPoints(float x, float y, int radius, Cone cone, int originI, int originJ, int cols, int rows,
                std::vector<std::pair<int, int>>& out);

/// An InvisibleLight's cone animation (ColorLightUpdateDraw's part of it).
struct ConeAnim {
    std::uint32_t omni = kOmni;
    Cone cone;
    std::uint16_t flags = 0;
    std::vector<std::uint8_t> keys; ///< the first n cone keys, n the count of non-zero ones
    std::uint16_t step = 0;
    bool updates = false;           ///< any non-zero colour, intensity or cone key
    std::uint16_t rotStart = 0;     ///< degrees
    std::int16_t rotSpeed = 0;      ///< degrees a second
    std::uint16_t rotMax = 0;       ///< degrees
    std::uint32_t target = 0;       ///< the extent it aims at (flag 0x40)
    std::optional<std::array<float, 2>> targetAt; ///< where that static drawable is, if there is one
};
ConeAnim coneAnim(const nox::map::xfer::InvisibleLight& l);

/// The light's cone at `frame` (game frames, `fps` a second), the light at (x, y); nullopt: all round.
std::optional<Cone> coneAt(const ConeAnim& a, float x, float y, std::uint32_t frame, int fps = 30);

} // namespace noxsdl::lightcone
