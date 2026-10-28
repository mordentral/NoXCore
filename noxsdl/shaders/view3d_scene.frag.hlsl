// The 3D view's depth-buffered world (view3d_gpu.cpp): a texel times the vertex colour; under the alpha cut-off the
// pixel is dropped, so a sprite's transparent parts neither show nor hide what's behind them.
cbuffer Params : register(b0, space3)
{
    float4 u_params; // x: the alpha cut-off (0 for blended draws)
};

Texture2D u_texture : register(t0, space2);
SamplerState u_sampler : register(s0, space2);

struct PSInput {
    float4 v_color : TEXCOORD0;
    float2 v_uv : TEXCOORD1;
};

float4 main(PSInput input) : SV_Target0
{
    const float4 c = u_texture.Sample(u_sampler, input.v_uv) * input.v_color;
    if (c.a < u_params.x) {
        discard;
    }
    return c;
}
