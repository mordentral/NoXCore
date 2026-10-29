// A script mode's draw scale on a drawable (nox.setDrawScale, client level 5): where it's scaled
// about, and what that does to its images and to what's drawn over it. No SDL in it, so the tests
// drive it. Every image of one drawable (a player's body, armour and weapon layers; a generator's
// image drawn over its body) is scaled about the one pivot, so they stay together:
//  - a missile about its first image's middle (it flies: "larger" is about its centre);
//  - anything else (creatures, players, NPCs, items, generators) about its ground point, so it
//    stays standing on the floor where it is.
#pragma once

namespace noxsdl::drawscale {

/// A rectangle (x, y its top left).
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};
struct Point {
    float x = 0, y = 0;
};

/// `r` scaled by `k` about `pivot` (all in the same units): its corners' distances from the pivot
/// times k. A rectangle about its own middle: pivot its centre.
inline Rect scaledAbout(Rect r, float k, Point pivot)
{
    return {pivot.x + (r.x - pivot.x) * k, pivot.y + (r.y - pivot.y) * k, r.w * k, r.h * k};
}

/// A drawable's pivot, in world units: a missile's first image's middle (that image drawn at
/// `image`), anything else's ground point (its x, its y less its height off the floor).
inline Point pivotOf(bool missile, Point ground, Rect image)
{
    return missile ? Point{image.x + image.w / 2, image.y + image.h / 2} : ground;
}

/// How far up (world units) a point at world y `top` on the drawable goes when it's scaled by `k`
/// about `pivot`: (k - 1) x its height over the pivot (a label over the thing goes up with its top;
/// about an image's middle, (k - 1) x half the image's height).
inline float rise(float k, Point pivot, float top)
{
    return (k - 1) * (pivot.y - top);
}

} // namespace noxsdl::drawscale
