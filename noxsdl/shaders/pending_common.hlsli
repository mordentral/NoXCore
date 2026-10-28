// The pending edits' looks, shared by pending.frag.hlsl and the material shaders. The same numbers
// as src/pending_look.hpp (the CPU's copies and the floor's composites): keep them together.
//
//   look 1, pending: grey, a little blue, slightly brightened (the caller's alpha makes it
//           translucent);
//   look 2, ghost:   removed but still in the game: a darker grey, more see-through, hatched.

float3 pendingGrey(float3 rgb, float look)
{
    const float l = dot(rgb, float3(0.299, 0.587, 0.114));
    if (look < 1.5) {
        return saturate(l * float3(0.80, 0.87, 1.00) * 1.12);
    }
    return saturate(l * float3(0.74, 0.76, 0.86) * 0.78);
}

// A ghost's hatch: every other 4-pixel diagonal band at 35% (screen pixels, so it stays put as
// sprites of any resolution are drawn); 1 for anything else.
float pendingHatch(float2 pixel, float look)
{
    if (look < 1.5) {
        return 1.0;
    }
    return frac((pixel.x + pixel.y) / 8.0) < 0.5 ? 0.35 : 1.0;
}
