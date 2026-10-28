// An anti-aliased rectangle for the effects' lines and square dots (fxdraw, enhanced drawing):
// the game's one-pixel lines and squares, drawn at the same width and length but smooth-edged
// at any scale and angle, in the vertex colour.
//
// The quad's texture coordinates are set so the rectangle's edges are where |uv * 2 - 1| = 1 on
// each axis, the quad reaching a little past them for the smoothed edges; the texture is a white
// pixel.

Texture2D u_texture : register(t0, space2); // the white pixel
SamplerState u_sampler : register(s0, space2);

struct PSInput {
    float4 v_color : TEXCOORD0; // as SDL's vertex shader names them
    float2 v_uv : TEXCOORD1;
};

float4 main(PSInput input) : SV_Target0
{
    const float2 e = abs(input.v_uv * 2.0 - 1.0); // 1 at the edges
    const float2 w = max(fwidth(e), 1e-4);
    const float2 c = saturate((1.0 - e) / w + 0.5);
    const float4 t = u_texture.Sample(u_sampler, saturate(input.v_uv));
    return float4(input.v_color.rgb * t.rgb, input.v_color.a * t.a * c.x * c.y);
}
