// The effects' glow disc (fxdraw::glow, smooth drawing), worked out for each screen pixel: the
// game's falloff (brightest in the middle, falling linearly to nothing at the edge,
// first down the rows and then along each row: (1 - |y|/R) * (1 - |x|/halfwidth(y))), in the
// vertex colour with the vertex alpha as the glow's intensity, added onto the screen. The same
// glow as the stock one's texture, exact at any size instead of a stretched 128-pixel image.
//
// The quad's texture coordinates put the disc's edge at |uv * 2 - 1| = 1; the texture is a white
// pixel.

Texture2D u_texture : register(t0, space2); // the white pixel
SamplerState u_sampler : register(s0, space2);

struct PSInput {
    float4 v_color : TEXCOORD0; // as SDL's vertex shader names them
    float2 v_uv : TEXCOORD1;
};

float4 main(PSInput input) : SV_Target0
{
    const float2 p = input.v_uv * 2.0 - 1.0;
    const float halfWidth = sqrt(max(0.0, 1.0 - p.y * p.y));
    float f = 0.0;
    if (halfWidth > 0.0 && abs(p.x) < halfWidth) {
        f = (1.0 - abs(p.y)) * (1.0 - abs(p.x) / halfWidth);
    }
    const float4 t = u_texture.Sample(u_sampler, saturate(input.v_uv));
    return float4(input.v_color.rgb * t.rgb, input.v_color.a * t.a * f);
}
