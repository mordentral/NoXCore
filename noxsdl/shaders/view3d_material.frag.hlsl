// The 3D view's depth-buffered world (view3d_gpu.cpp), for sprites with material pixels (weapons, armour, robes): noxsdl's
// material shader (shaders/material.frag.hlsl, nearest filtering) with scene.frag's alpha cut-off.
//
// The image and its material bytes (0 plain, n = colour n - 1) come from two textures: the same
// one for an image with its material bytes beside it (twice its width), the BC7 page and its
// material page for the image pack's BC7 images (GpuAtlas). A material pixel becomes
// grey * colour >> 8 per channel.
cbuffer Materials : register(b0, space3)
{
    float4 u_colour[7]; // RGB 0-255 by material colour index
    float4 u_params;    // y, z: the image's width and height in texels; w: the alpha cut-off
    float4 u_place;     // x, y: the image's corner in the texture; z, w: the texture's size
    float4 u_labels;    // x, y: the image's corner in the material texture
};

Texture2D u_texture : register(t0, space2);
Texture2D u_labels_texture : register(t1, space2);
SamplerState u_sampler : register(s0, space2);
SamplerState u_labels_sampler : register(s1, space2);

struct PSInput {
    float4 v_color : TEXCOORD0;
    float2 v_uv : TEXCOORD1;
};

float4 main(PSInput input) : SV_Target0
{
    const float2 pos = input.v_uv * u_place.zw - u_place.xy; // in the image's own texels
    const int2 last = int2((int)u_params.y - 1, (int)u_params.z - 1);
    const int2 p = clamp(int2(floor(pos)), int2(0, 0), last);
    float4 c = u_texture.Load(int3(p + int2(u_place.xy), 0));
    const float m = round(u_labels_texture.Load(int3(p + int2(u_labels.xy), 0)).r * 255.0);
    if (m > 0.5) {
        const float3 grey = round(c.rgb * 255.0);
        const float3 colour = u_colour[(int)min(m - 1.0, 6.0)].rgb;
        c.rgb = floor(grey * colour / 256.0) / 255.0;
    }
    c *= input.v_color;
    if (c.a < u_params.w) {
        discard;
    }
    return c;
}
