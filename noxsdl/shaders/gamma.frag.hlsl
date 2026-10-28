// The game's gamma (nox.cfg's Gamma, 1-10; Insert / Delete): its ramp over the whole
// finished frame, as the hardware gamma ramp did: each channel c becomes c ^ (1 / (1 + (g - 1) / 6)),
// so 1 leaves it as it is and higher brightens the darks.

cbuffer Gamma : register(b0, space3)
{
    float4 u_params; // x: the exponent
};

Texture2D u_texture : register(t0, space2);
SamplerState u_sampler : register(s0, space2);

struct PSInput {
    float4 v_color : TEXCOORD0; // as SDL's vertex shader names them
    float2 v_uv : TEXCOORD1;
};

float4 main(PSInput input) : SV_Target0
{
    const float4 c = u_texture.Sample(u_sampler, input.v_uv);
    return float4(pow(saturate(c.rgb), u_params.x), 1.0);
}