// One light of the per-pixel light map (lighting.cpp, GPU renderer): the game's light formula
// (cliLight16) worked out for every screen pixel instead of at the
// grid points 23 units apart. For a light of intensity I at distance d:
//     v = (min(I, 31) + 4) / (1 + 0.96789 * d^2 / floor(I)^2)
// and the light adds colour * (v - 4) / 32 where v > 4 (an anti-light subtracts it; its blend
// does that). Drawn additively into a target that starts at the ambient colour and clamps each
// blend to 0-1 as the game clamps each light's sum, which is then multiplied over the floor.
//
// The quad's texture coordinates are set so that uv * 2 - 1 is the offset from the light over
// floor(I) (the quad reaching the light's radius), and the vertex alpha carries min(I, 31) + 4
// over 64; the vertex colour is the light's colour. The texture is a white pixel.

Texture2D u_texture : register(t0, space2); // the white pixel
SamplerState u_sampler : register(s0, space2);

struct PSInput {
    float4 v_color : TEXCOORD0; // as SDL's vertex shader names them
    float2 v_uv : TEXCOORD1;
};

static const float kFalloff = 961.0 * 66.0 / 65536.0; // (the game's constant: 961 x 66 in 16.16)
static const float kThreshold = 4.0;

float4 main(PSInput input) : SV_Target0
{
    const float2 q = input.v_uv * 2.0 - 1.0; // offset / floor(I)
    const float numerator = input.v_color.a * 64.0;
    const float v = numerator / (1.0 + kFalloff * dot(q, q));
    const float amount = max(v - kThreshold, 0.0) / 32.0;
    const float4 t = u_texture.Sample(u_sampler, saturate(input.v_uv));
    return float4(input.v_color.rgb * t.rgb * amount, 1.0);
}
