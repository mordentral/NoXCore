// The 3D view's particles (view3d.cpp addEffects): quads facing the eye, drawn after the world with
// its depth (hidden behind walls, not hiding each other). No texture: the look is worked out here,
// chosen by the texture coordinate's whole part, its fraction being the place on the quad (0-1):
//   u / 4: 0 glow     the effects' glow disc (fx_glow: the game's falloff), added on;
//                     the vertex alpha its intensity
//          1 core     a solid dot, soft at its edge (the sparks' and particles' centres)
//          2 puff     soft smoke (fx_puff: a round body broken up by value noise), v / 4 its seed
//          3 ribbon   a glowing line (beams, trails): bright down the middle, gone at the edges
struct PSInput {
    float4 v_color : TEXCOORD0;
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
    const float mode = floor(input.v_uv.x / 4.0);
    const float seed = floor(input.v_uv.y / 4.0);
    const float2 uv = float2(input.v_uv.x - mode * 4.0, input.v_uv.y - seed * 4.0);
    const float2 p = uv * 2.0 - 1.0;
    float a = 0.0;
    float3 rgb = input.v_color.rgb;
    if (mode < 0.5) {
        const float halfWidth = sqrt(max(0.0, 1.0 - p.y * p.y));
        if (halfWidth > 0.0 && abs(p.x) < halfWidth) {
            a = (1.0 - abs(p.y)) * (1.0 - abs(p.x) / halfWidth);
        }
    } else if (mode < 1.5) {
        const float r2 = dot(p, p);
        a = saturate((1.0 - r2) * 4.0);
    } else if (mode < 2.5) {
        const float r2 = dot(p, p);
        if (r2 < 1.0) {
            const float body = (1.0 - r2) * (1.0 - r2);
            const float2 q = p * 1.7 + seed * 0.37;
            const float n = 0.6 * noise(q) + 0.4 * noise(q * 2.3 + 5.1);
            a = body * (0.5 + 0.9 * n);
            rgb *= 0.85 + 0.3 * n - 0.1 * p.y;
        }
    } else {
        const float across = 1.0 - abs(p.y);
        a = across * across;
    }
    if (a <= 0.0) {
        discard;
    }
    return float4(saturate(rgb), saturate(input.v_color.a * a));
}
