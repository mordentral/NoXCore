// A pending edit's look: a map editor session's walls and things the working copy has but the game
// doesn't yet, greyed, and those it removed but the game still has, as darker hatched ghosts
// (pending_common.hlsli). Drawn with the sprite's own texture, light and alpha (the caller's alpha
// mod makes it translucent); the CPU's copies (SpriteCache::pendingCopy) give the same look without
// the shaders.

#include "pending_common.hlsli"

cbuffer Pending : register(b0, space3)
{
    float4 u_params; // x: the look (1 pending, 2 ghost)
};

Texture2D u_texture : register(t0, space2);
SamplerState u_sampler : register(s0, space2);

// As SDL's vertex shader names and orders its outputs: the colour TEXCOORD0, the texture
// coordinate TEXCOORD1, then the position (read here as the pixel, for the ghost's hatch).
struct PSInput {
    float4 v_color : TEXCOORD0;
    float2 v_uv : TEXCOORD1;
    float4 pos : SV_Position;
};

float4 main(PSInput input) : SV_Target0
{
    float4 c = u_texture.Sample(u_sampler, input.v_uv) * input.v_color;
    c.rgb = pendingGrey(c.rgb, u_params.x);
    c.a *= pendingHatch(input.pos.xy, u_params.x);
    return c;
}
