// Material shading for the GPU renderer, for the image pack's BC7 images: material.frag.hlsl's
// result, with the image (BC7) and its material bytes (R8, lossless) in textures of their own,
// as GpuAtlas keeps them (the BC7 page SDL binds, the material page the render state binds).
//
// A material pixel becomes grey * colour >> 8 per channel, in integers, as the game's material
// run does. Texels are shaded before filtering: nearest takes the texel under the point, linear
// blends the four shaded texels round it the way the hardware sampler does (straight alpha,
// clamped at the image's edges). u_params.w: a pending look, as material.frag.hlsl's.

#include "pending_common.hlsli"

cbuffer Materials : register(b0, space3)
{
    float4 u_colour[7]; // RGB 0-255 by material colour index
    float4 u_params;    // x: 1 = linear filtering; y, z: the image's width and height in texels; w: pending look
    float4 u_place;     // x, y: the image's corner in its texture; z, w: that texture's size
    float4 u_labels;    // x, y: the image's corner in the material texture
};

Texture2D u_texture : register(t0, space2);
Texture2D u_labels_texture : register(t1, space2);
SamplerState u_sampler : register(s0, space2);
SamplerState u_labels_sampler : register(s1, space2);

// As SDL's vertex shader (tri_texture.vert, built with shadercross) names its outputs: the
// colour TEXCOORD0, the texture coordinate TEXCOORD1. Direct3D 12 matches them by name.
struct PSInput {
    float4 v_color : TEXCOORD0;
    float2 v_uv : TEXCOORD1;
};

float4 shadedTexel(int2 p)
{
    const int2 last = int2((int)u_params.y - 1, (int)u_params.z - 1);
    p = clamp(p, int2(0, 0), last);
    float4 c = u_texture.Load(int3(p + int2(u_place.xy), 0));
    const float m = round(u_labels_texture.Load(int3(p + int2(u_labels.xy), 0)).r * 255.0);
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
