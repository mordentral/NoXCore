// The effects' glow line (fxdraw::glowPath, smooth drawing): the game's glow line
// stamps a glow disc every radius / 4 pixels along each segment, each added onto the screen. Here
// one quad per segment adds up those same discs (fx_glow's falloff) for each screen pixel, so a
// lightning bolt is a few dozen quads instead of thousands of overlapping discs: the same sum
// without the blending.
//
// Per segment (the vertex attributes): uv is the pixel's offset from the segment's first disc in
// disc radii (screen axes); the colour's r and g are the step from one disc to the next in radii,
// b the number of discs. Per draw (the uniforms): the glow's colour and its intensity (0-1). The
// texture is the white pixel (unused, but SDL binds one).

Texture2D u_texture : register(t0, space2);
SamplerState u_sampler : register(s0, space2);

cbuffer Params : register(b0, space3)
{
    float4 u_color; // rgb the glow's colour, a its intensity
};

struct PSInput {
    float4 v_color : TEXCOORD0; // as SDL's vertex shader names them
    float2 v_uv : TEXCOORD1;
};

// fx_glow's falloff: (1 - |y|) * (1 - |x| / halfwidth(y)) inside the disc.
float disc(float2 p)
{
    const float halfWidth = sqrt(max(0.0, 1.0 - p.y * p.y));
    if (halfWidth <= 0.0 || abs(p.x) >= halfWidth) {
        return 0.0;
    }
    return (1.0 - abs(p.y)) * (1.0 - abs(p.x) / halfWidth);
}

float4 main(PSInput input) : SV_Target0
{
    const float2 p = input.v_uv;
    const float2 step = input.v_color.rg;
    const int count = (int)round(input.v_color.b);
    // Only the discs within a radius of this pixel: k * step within 1 of p.
    int first = 0;
    int last = count - 1;
    const float len2 = dot(step, step);
    if (len2 > 0.0) {
        const float t = dot(p, step) / len2;
        const float reach = 1.0 / sqrt(len2);
        first = max(first, (int)ceil(t - reach));
        last = min(last, (int)floor(t + reach));
    } else {
        last = min(last, 0);
    }
    float sum = 0.0;
    [loop] for (int k = first; k <= last; ++k) {
        sum += disc(p - step * (float)k);
    }
    return float4(u_color.rgb * (u_color.a * sum), 1.0);
}
