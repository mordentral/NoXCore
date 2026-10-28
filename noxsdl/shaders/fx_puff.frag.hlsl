// A soft puff of smoke or gas (fxdraw::puff): round, densest in the middle and fading smoothly to
// nothing at the edge, its density broken up by a little value noise so a crowd of them reads as
// cloud rather than as discs. Not in the game (its toxic clouds and smoke blasts are sprites with
// hard edges); drawn with alpha blending in the vertex colour, the vertex alpha the puff's
// opacity.
//
// The texture coordinates run 0-1 across the quad, offset by a whole number per puff (its seed),
// so each puff's noise differs: frac() is the position, floor() the seed. The texture is a white
// pixel.

Texture2D u_texture : register(t0, space2); // the white pixel
SamplerState u_sampler : register(s0, space2);

struct PSInput {
    float4 v_color : TEXCOORD0; // as SDL's vertex shader names them
    float2 v_uv : TEXCOORD1;
};

float hash(float2 p)
{
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

float noise(float2 p)
{
    const float2 i = floor(p);
    const float2 f = frac(p);
    const float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash(i), hash(i + float2(1, 0)), u.x), lerp(hash(i + float2(0, 1)), hash(i + float2(1, 1)), u.x),
                u.y);
}

float4 main(PSInput input) : SV_Target0
{
    const float2 seed = floor(input.v_uv);
    const float2 p = (input.v_uv - seed) * 2.0 - 1.0;
    const float r2 = dot(p, p);
    if (r2 >= 1.0) {
        return float4(0, 0, 0, 0);
    }
    const float body = (1.0 - r2) * (1.0 - r2); // smooth to the edge
    const float2 q = p * 1.7 + seed * 0.37;
    const float n = 0.6 * noise(q) + 0.4 * noise(q * 2.3 + 5.1);
    const float density = body * (0.5 + 0.9 * n);
    // Billows: the thick parts a little lighter, as lit from above.
    const float shade = 0.85 + 0.3 * n - 0.1 * p.y;
    const float4 t = u_texture.Sample(u_sampler, float2(0.5, 0.5));
    return float4(saturate(input.v_color.rgb * t.rgb * shade), saturate(input.v_color.a * t.a * density));
}
