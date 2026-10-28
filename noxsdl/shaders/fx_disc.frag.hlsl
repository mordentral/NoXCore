// An anti-aliased disc for the effects' points (fxdraw, enhanced drawing): the game draws its
// sparks and particle blobs as little pixel shapes (drawPointMB), which turn to
// blocks when the view is scaled up; this draws the same size of dot, round and smooth at any
// scale, in the vertex colour.
//
// The quad's texture coordinates are set so the disc's edge is where |uv * 2 - 1| = 1, the quad
// reaching a little past it for the smoothed edge; the texture is a white pixel.

Texture2D u_texture : register(t0, space2); // the white pixel
SamplerState u_sampler : register(s0, space2);

struct PSInput {
    float4 v_color : TEXCOORD0; // as SDL's vertex shader names them
    float2 v_uv : TEXCOORD1;
};

float4 main(PSInput input) : SV_Target0
{
    const float d = length(input.v_uv * 2.0 - 1.0); // 1 at the edge
    const float w = max(fwidth(d), 1e-4);
    const float coverage = saturate((1.0 - d) / w + 0.5);
    const float4 t = u_texture.Sample(u_sampler, saturate(input.v_uv));
    return float4(input.v_color.rgb * t.rgb, input.v_color.a * t.a * coverage);
}
