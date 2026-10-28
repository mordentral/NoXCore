// The Force Field's barrier (fxdraw::barrier, smooth drawing): in place of the game's
// SphericalShieldAnim, a glassy sphere round the unit. A bright Fresnel rim, a honeycomb of thin
// lines wrapped over the sphere and turning slowly, its cells flaring now and then, a band of
// energy sweeping down it, a soft highlight and a faint, noisy body. Not in the game. Added onto
// the screen; the vertex colour is the barrier's tint, its alpha the strength.
//
// The texture coordinates run 0-1 across the quad, offset by a whole number per unit (its seed),
// so each barrier's pattern runs out of step: frac() is the position, floor() the seed. The
// texture is a white pixel.

Texture2D u_texture : register(t0, space2); // the white pixel
SamplerState u_sampler : register(s0, space2);

cbuffer Params : register(b0, space3)
{
    float4 u_params; // x: seconds
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

float2 wrap(float2 x, float2 y)
{
    return x - y * floor(x / y);
}

// The hexagonal cell p is in: xy the offset from its centre, zw the centre (its id).
float4 hexCell(float2 p)
{
    const float2 r = float2(1.0, 1.7320508);
    const float2 h = r * 0.5;
    const float2 a = wrap(p, r) - h;
    const float2 b = wrap(p - h, r) - h;
    const float2 g = dot(a, a) < dot(b, b) ? a : b;
    return float4(g, p - g);
}

// 0.5 at a cell's edge, 0 at its centre.
float hexDistance(float2 g)
{
    g = abs(g);
    return max(dot(g, float2(0.5, 0.8660254)), g.x);
}

float4 main(PSInput input) : SV_Target0
{
    const float2 seed = floor(input.v_uv);
    const float2 p = (input.v_uv - seed) * 2.0 - 1.0;
    const float r = length(p);
    const float aa = max(fwidth(r), 1e-4) * 1.5;
    const float inside = 1.0 - smoothstep(1.0 - aa, 1.0, r);
    if (inside <= 0.0) {
        return float4(0, 0, 0, 0);
    }
    const float t = u_params.x + seed.x * 7.13 + seed.y * 3.71;
    const float z = sqrt(saturate(1.0 - r * r)); // the sphere's depth there
    const float3 n = float3(p, z);
    const float fresnel = pow(1.0 - z, 3.0);

    // The honeycomb on the sphere: the flat position bulged out toward the rim, turning slowly.
    float2 sp = p / (0.3 + 0.7 * z);
    const float turn = t * 0.25;
    sp = float2(sp.x * cos(turn) - sp.y * sin(turn), sp.x * sin(turn) + sp.y * cos(turn));
    const float4 cell = hexCell(sp * 3.2 + float2(0.0, t * 0.15));
    const float edge = 0.5 - hexDistance(cell.xy);
    const float lineWidth = 0.05 + aa * 3.0;
    const float lines = 1.0 - smoothstep(0.0, lineWidth, edge);
    const float flare = pow(saturate(sin(hash(cell.zw) * 6.2831853 + t * 1.7)), 12.0);

    // A band of energy sweeping down the sphere, and a shimmer through it.
    const float band = pow(0.5 + 0.5 * sin((p.y * 1.6 - t * 1.4) * 3.14159), 8.0);
    const float shimmer = noise(sp * 2.5 + float2(t * 0.6, -t * 0.4));

    // A highlight up and to the left.
    const float3 toLight = normalize(float3(-0.45, -0.6, 0.65));
    const float highlight = pow(saturate(dot(n, toLight)), 28.0);

    const float body = 0.03 + 0.05 * shimmer;
    const float rim = fresnel * (0.7 + 0.4 * shimmer);
    const float grid = lines * (0.08 + 0.45 * fresnel + 0.25 * band) + flare * (1.0 - edge * 1.6) * 0.22;
    const float a = (body + rim + grid + band * 0.04 + highlight * 0.45) * inside * input.v_color.a;
    const float whiten = saturate(highlight * 1.5 + fresnel * 0.3 + flare * 0.35 + band * 0.08);
    const float3 colour = lerp(input.v_color.rgb, float3(1.0, 1.0, 1.0), whiten);
    const float4 tex = u_texture.Sample(u_sampler, float2(0.5, 0.5));
    return float4(saturate(colour * tex.rgb), saturate(a * tex.a));
}
