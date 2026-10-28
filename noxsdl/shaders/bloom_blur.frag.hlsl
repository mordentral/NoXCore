// One direction of the bloom's Gaussian blur (bloom.cpp): nine taps along u_params.xy (one
// texel's step in texture coordinates, across or down), weights for a sigma of about two texels.

cbuffer Blur : register(b0, space3)
{
    float4 u_params; // xy: the step between taps
};

Texture2D u_texture : register(t0, space2);
SamplerState u_sampler : register(s0, space2);

struct PSInput {
    float4 v_color : TEXCOORD0; // as SDL's vertex shader names them
    float2 v_uv : TEXCOORD1;
};

static const float kWeights[5] = {0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162};

float4 main(PSInput input) : SV_Target0
{
    float4 sum = u_texture.Sample(u_sampler, input.v_uv) * kWeights[0];
    [unroll] for (int i = 1; i < 5; ++i) {
        const float2 o = u_params.xy * i;
        sum += u_texture.Sample(u_sampler, input.v_uv + o) * kWeights[i];
        sum += u_texture.Sample(u_sampler, input.v_uv - o) * kWeights[i];
    }
    return sum * input.v_color;
}
