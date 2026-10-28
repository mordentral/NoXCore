// Material shading for the GPU renderer: the same result as SpriteCache::tinted's CPU path.
//
// The image's place in the texture (all of it, or its place on an atlas page) is twice the
// image's width: the image on the left (material pixels hold a grey intensity), each pixel's
// material byte on the right (0 plain, n = colour n - 1). A material
// pixel becomes grey * colour >> 8 per channel, in integers, as the game's material run
// does. Texels are shaded before filtering, as the CPU path shades the texture before SDL filters
// it: nearest takes the texel under the point, linear blends the four shaded texels round it the
// way the hardware sampler does (straight alpha, clamped at the edges).
//
// u_params.w: a map editor session's pending look (pending_common.hlsli: 1 pending, 2 ghost; 0,
// as always, none), so a recoloured sprite is greyed in the same pass.

#include "pending_common.hlsli"

cbuffer Materials : register(b0, space3)
{
    float4 u_colour[7]; // RGB 0-255 by material colour index
    float4 u_params;    // x: 1 = linear filtering; y, z: the image's width and height in texels; w: pending look
    float4 u_place;     // x, y: the image's corner in the texture; z, w: the texture's size
};

Texture2D u_texture : register(t0, space2);
SamplerState u_sampler : register(s0, space2);

// As SDL's vertex shader (tri_texture.vert, built with shadercross) names its outputs: the
// colour TEXCOORD0, the texture coordinate TEXCOORD1. Direct3D 12 matches them by name.
struct PSInput {
    float4 v_color : TEXCOORD0;
    float2 v_uv : TEXCOORD1;
};

float4 shadedTexel(int2 p)
{
    const int2 last = int2((int)u_params.y - 1, (int)u_params.z - 1);
    p = clamp(p, int2(0, 0), last) + int2(u_place.xy);
    float4 c = u_texture.Load(int3(p, 0));
    const float m = round(u_texture.Load(int3(p.x + (int)u_params.y, p.y, 0)).r * 255.0);
    if (m > 0.5) {
        const float3 grey = round(c.rgb * 255.0);
        const float3 colour = u_colour[(int)min(m - 1.0, 6.0)].rgb;
        c.rgb = floor(grey * colour / 256.0) / 255.0;
    }
    return c;
}

float4 main(PSInput input) : SV_Target0
{
    const float2 pos = input.v_uv * u_place.zw - u_place.xy; // in the image's own texels
    float4 c;
    if (u_params.x < 0.5) {
        c = shadedTexel(int2(floor(pos)));
    } else {
        const float2 p = pos - 0.5;
        const int2 b = int2(floor(p));
        const float2 f = round((p - floor(p)) * 256.0) / 256.0; // the sampler's 8-bit weights
        const float4 top = lerp(shadedTexel(b), shadedTexel(b + int2(1, 0)), f.x);
        const float4 bottom = lerp(shadedTexel(b + int2(0, 1)), shadedTexel(b + int2(1, 1)), f.x);
        c = lerp(top, bottom, f.y);
    }
    c *= input.v_color;
    if (u_params.w > 0.5) {
        c.rgb = pendingGrey(c.rgb, u_params.w);
    }
    return c;
}
