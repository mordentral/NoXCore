// The Reflective Shield's water disc (fxdraw::ripple, HD Spell FX): in place of the game's
// ReflectiveShield* animations, the same disc of water standing in front of the unit, drawn at the
// screen's resolution. Its face (seen from the front: S, SE, SW, and edge on: E, W) has rings of
// ripples running out from a drop in the middle, a ring each loop of the game's eight-frame
// animation, a little uneven as water is; its back (seen from behind: N, NE, NW) the softer,
// shifting light of water seen through. Lit from the upper left as the game's art is, with
// highlights on the ripples' crests and a bright rim, in the art's deep-to-pale cyan; alpha-blended
// nearly opaque, as the art is opaque.
//
// The quad is the disc's ellipse (QuadBatch::corners): the texture coordinates are 0-1 from edge to
// edge, u along the major axis (downward), v along the minor (to the left), a pixel and a half
// beyond for the smoothed edge, so the rings follow the disc's tilt. The vertex colour is the
// scene's light where the shield is (the sprite's colour multiply); its alpha the draw alpha. The
// texture is a white pixel.

Texture2D u_texture : register(t0, space2); // the white pixel
SamplerState u_sampler : register(s0, space2);

cbuffer Params : register(b0, space3)
{
    // x: the rings' loop (0-1, a ring outward each loop), y: seconds (the slow drift), z: 1 seen
    // from behind, w: the disc's width over its height (thin edge on)
    float4 u_params;
};

struct PSInput {
    float4 v_color : TEXCOORD0; // as SDL's vertex shader names them
    float2 v_uv : TEXCOORD1;
};

static const float kTau = 6.2831853;

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

// The front's water height at p (the disc's radius is 1): rings running out from the middle (a
// little wobbly), the drop at the centre, and a fainter set from off the centre crossing them.
float frontHeight(float2 p, float loop, float seconds)
{
    const float wobble = noise(p * 2.6 + float2(seconds * 0.35, -seconds * 0.25)) - 0.5;
    const float r = length(p) + 0.07 * wobble;
    const float rings = sin((r * 3.0 - loop) * kTau) * (1.0 - 0.4 * saturate(r));
    const float drop = exp(-r * r * 30.0) * 0.9;
    const float2 c = p - float2(0.3, -0.35);
    const float rc = length(c);
    const float crossing = 0.15 * sin((rc * 4.0 - loop) * kTau) * exp(-rc * 1.6); // (a whole turn a loop: no jump)
    return 0.5 * rings + drop + crossing;
}

// The back's: the light of water seen through it, soft shifting cells, the rings faint behind.
float backHeight(float2 p, float loop, float seconds)
{
    const float s = seconds * 1.6;
    const float2 q = p * 4.2;
    float v = sin(q.x * 1.7 + sin(q.y * 1.3 + s) + s * 1.1);
    v += sin(q.y * 1.9 + sin(q.x * 1.5 - s * 0.8) - s);
    v += sin((q.x + q.y) * 1.2 + 1.3 * sin(q.x * 0.9 + s * 0.7) + s * 0.6);
    const float r = length(p);
    return 0.1 * v + 0.02 * sin((r * 3.5 - loop) * kTau);
}

float waterHeight(float2 p, float loop, float seconds, bool back)
{
    return back ? backHeight(p, loop, seconds) : frontHeight(p, loop, seconds);
}

float4 main(PSInput input) : SV_Target0
{
    const float2 uv = input.v_uv * 2.0 - 1.0;
    const float2 p = float2(-uv.y, uv.x); // x to the right, y down
    const float r = length(p);
    const float aa = max(fwidth(r), 1e-4) * 1.5;
    const float inside = 1.0 - smoothstep(1.0 - aa, 1.0, r);
    if (inside <= 0.0) {
        return float4(0, 0, 0, 0);
    }
    const float loop = u_params.x;
    const float seconds = u_params.y;
    const bool back = u_params.z > 0.5;
    const float thin = u_params.w;

    // The water's slope (from three samples) on a gently domed disc.
    const float e = 0.012;
    const float h = waterHeight(p, loop, seconds, back);
    const float hx = waterHeight(p + float2(e, 0), loop, seconds, back);
    const float hy = waterHeight(p + float2(0, e), loop, seconds, back);
    const float2 slope = float2(hx - h, hy - h) / e;
    const float strength = back ? 0.25 : 0.12;
    const float3 dome = normalize(float3(p * 0.45, 1.0));
    const float3 n = normalize(float3(p * 0.45 - slope * strength, 1.0));

    // Lit from the upper left.
    const float3 toLight = normalize(float3(-0.55, -0.65, 0.55));
    const float diffuse = saturate(dot(n, toLight));
    const float3 halfway = normalize(toLight + float3(0, 0, 1));
    const float specular = pow(saturate(dot(n, halfway)), 40.0);

    // The art's ramp, navy through cyan to nearly white. The back: by the light on its cells,
    // a little darker. The front: the water's mid blue on the gentle dome, the ripples adding
    // light on the crests that face it and only a little shade in the troughs (light on water,
    // not dark stripes), the troughs a touch clearer.
    float tone;
    float trough = 0.0;
    if (back) {
        tone = saturate((diffuse - 0.2) / 0.8) * 0.85;
    } else {
        const float plain = saturate(dot(dome, toLight)); // (the dome alone)
        const float delta = diffuse - plain;
        tone = saturate(0.32 + 0.45 * saturate((plain - 0.2) / 0.8) + (delta > 0.0 ? delta * 1.4 : delta * 0.4));
        trough = smoothstep(0.0, 0.35, -delta);
    }
    float3 colour = lerp(float3(0.03, 0.09, 0.19), float3(0.0, 0.22, 0.35), saturate(tone / 0.3));
    colour = lerp(colour, float3(0.0, 0.42, 0.61), saturate((tone - 0.3) / 0.2));
    colour = lerp(colour, float3(0.06, 0.71, 0.97), saturate((tone - 0.5) / 0.22));
    colour = lerp(colour, float3(0.65, 1.0, 1.0), saturate((tone - 0.72) / 0.2));
    colour += specular * float3(0.8, 0.97, 1.0) * (back ? 0.5 : 0.9);
    if (back) {
        // Water seen through: the light gathered into soft bright cells.
        colour += smoothstep(0.08, 0.26, h) * float3(0.0, 0.40, 0.55);
    }

    // A bright rim, lit most on the lower right where the art's is.
    const float band = smoothstep(0.78, 0.96, r);
    const float side = saturate(0.45 + 0.55 * dot(p / max(r, 1e-4), float2(0.6, 0.8)));
    colour = lerp(colour, float3(0.38, 0.86, 1.0), band * (0.1 + 0.35 * side));

    // Edge on (E, W) the disc is a thin, bright cyan sliver.
    colour = lerp(colour, float3(0.1, 0.8, 1.0), (1.0 - smoothstep(0.08, 0.35, thin)) * 0.55);

    const float4 tex = u_texture.Sample(u_sampler, float2(0.5, 0.5));
    const float alpha = inside * saturate(0.97 + 0.03 * band - 0.18 * trough * (1.0 - band)) * input.v_color.a;
    return float4(saturate(colour * input.v_color.rgb * tex.rgb), saturate(alpha * tex.a));
}
