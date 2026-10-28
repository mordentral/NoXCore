// Weather (weather.cpp, GPU renderer): the shapes nox.weather's rain, snow, storm and wind are
// drawn with, over the world and under the HUD. Not in the game. Each draw is of one shape,
// u_params.x:
//   0 a raindrop's streak: the quad along its path on screen, uv.y 0 at its tail to 1 at its head,
//     uv.x 0-1 across; bright at the head, fading to the tail.
//   1 a soft dot: a snowflake or a mote of dust, uv 0-1 across it; a soft edge and a brighter core.
//   2 a splash: a thin ring at the quad's edge (the quad squashed into the ground's ellipse), uv
//     0-1; its width (u_params.z) thins as it spreads.
//   3 a gust: a long faint streak along the wind, faded at both ends (uv as a raindrop's).
//   4 sheets of rain: a quad over the view whose uv is the world position (in units of 256, so
//     they stay on the ground as the view moves), with falling curtains of faint streaks worked out
//     from noise; u_params.zw the fall's direction on screen, u_params.y the seconds.
// The vertex colour is the tint, its alpha the opacity (the light it's drawn under already in it).
// Drawn alpha-blended on the white pixel.

Texture2D u_texture : register(t0, space2); // the white pixel
SamplerState u_sampler : register(s0, space2);

cbuffer Params : register(b0, space3)
{
    float4 u_params; // x: the shape, y: seconds, zw: the shape's own
};

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

// Across a streak: 1 on its line, 0 at the quad's sides, antialiased.
float across(float u)
{
    const float d = abs(u * 2.0 - 1.0);
    const float aa = max(fwidth(d), 1e-3);
    return 1.0 - smoothstep(0.35 - aa, 1.0, d);
}

float streak(float2 uv)
{
    const float along = saturate(uv.y);
    // The tail fades in, the head is round and bright.
    const float body = pow(along, 1.6);
    const float head = smoothstep(1.0, 0.86, along);
    return across(uv.x) * body * head;
}

float dot1(float2 uv)
{
    const float r = length(uv * 2.0 - 1.0);
    const float aa = max(fwidth(r), 1e-3);
    const float edge = 1.0 - smoothstep(0.55 - aa, 1.0, r);
    const float core = 1.0 - smoothstep(0.0, 0.45, r);
    return saturate(edge * 0.75 + core * 0.35);
}

float ring(float2 uv, float width)
{
    const float r = length(uv * 2.0 - 1.0);
    const float aa = max(fwidth(r), 1e-3);
    const float w = max(width, aa * 1.5);
    const float outer = 1.0 - smoothstep(1.0 - aa, 1.0, r);
    const float inner = smoothstep(1.0 - w - aa, 1.0 - w, r);
    return outer * inner;
}

float gust(float2 uv)
{
    const float along = saturate(uv.y);
    return across(uv.x) * pow(sin(along * 3.14159265), 1.5);
}

// Curtains of rain: thin streaks along the fall, in two layers drifting at their own speeds.
float sheets(float2 world, float t, float2 fall)
{
    const float2 side = float2(-fall.y, fall.x);
    float a = 0.0;
    [unroll] for (int k = 0; k < 2; ++k) {
        const float scale = k == 0 ? 1.0 : 1.7;
        const float speed = k == 0 ? 3.2 : 4.6;
        const float2 p = float2(dot(world, side) * 22.0 * scale, dot(world, fall) * 3.2 * scale - t * speed * 3.0);
        const float n = noise(p) * 0.65 + noise(p * float2(2.03, 1.97) + 17.0) * 0.35;
        const float lines = smoothstep(0.66, 0.97, n);
        // Slow gusts of density through them.
        const float drift = noise(world * 1.3 + float2(t * 0.11, -t * 0.07) + k * 9.0);
        a += lines * (0.45 + 0.55 * drift) * (k == 0 ? 0.65 : 0.35);
    }
    return a;
}

float4 main(PSInput input) : SV_Target0
{
    const int shape = (int)(u_params.x + 0.5);
    float a;
    if (shape == 0) {
        a = streak(input.v_uv);
    } else if (shape == 1) {
        a = dot1(input.v_uv);
    } else if (shape == 2) {
        a = ring(input.v_uv, u_params.z);
    } else if (shape == 3) {
        a = gust(input.v_uv);
    } else {
        a = sheets(input.v_uv, u_params.y, normalize(u_params.zw + float2(0.0, 1e-4)));
    }
    const float4 tex = u_texture.Sample(u_sampler, float2(0.5, 0.5));
    return float4(saturate(input.v_color.rgb * tex.rgb), saturate(a * input.v_color.a * tex.a));
}
