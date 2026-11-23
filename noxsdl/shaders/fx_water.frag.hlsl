// HD Spell FX's water (hd_water.hpp): the Water floors drawn at the floor target's resolution as a
// water surface, in Nox's own murky green and darkness, so it sits with the HD floors round it.
//
// The body under the surface is the floor's own tiles (water_base.hpp):
//  - each frame's picture at a texel a world unit, sampled smoothly (Catmull-Rom), its sample
//    point nudged by a still noise under half a texel so the flecks' outlines come out irregular;
//  - the frames running one into the next, the low-pass (the murk) blended plainly and the detail
//    keeping its contrast (normalised by the frames' correlation); a fine grain below the texel
//    along the detail's own colour, as strong as the original's detail.
// The surface over it:
//  - waves: six directional waves of several lengths and ways, their slopes worked out exactly,
//    nudged by the body's noise so they never line up, as a normal that shades the body (lit
//    from the upper left as the game's art is: a gentle relief, not a colour shift);
//  - a sheen where the game's lights are (up to eight, the widest round the view): a soft
//    highlight off the waves, tinted by the light. The map's ambient and the lights themselves
//    are the floor's light map, multiplied over all of this as over every floor;
//  - ripples: rings running out from where something stands in or moves through the water, a
//    splash's stronger ring with a crown and a burst of droplets (water_ripples.hpp feeds them);
//  - at the shore (the tile's water share falling off where its edges blend the land in): a band
//    lapping in and out, a little broken foam along it, the water a touch clearer there.
// Every channel of the body is kept to the tiles' own darkest and brightest; the foam and the
// sheen go only a little past it.
//
// Positions come from the pixel's place in the floor target (SV_Position) and the target's world
// rectangle (u_view): the water stays put in the world, whatever the tile. The texture SDL binds
// is the tile's water share (MapView::hdTile's wet composite): its alpha the share of each pixel
// (with the tile's own outline), its red the share alone (how near the shore). It's drawn added
// over the tile drawn without the water's colour. The vertex colour multiplies as a sprite's does.

Texture2D u_texture : register(t0, space2); // the water's share
Texture2D u_base : register(t1, space2);    // frames down, each picture then its low-pass across
SamplerState u_sampler : register(s0, space2);
SamplerState u_base_sampler : register(s1, space2); // linear, clamped (the apron keeps reads in a block)

static const int kMaxLights = 8;
static const int kMaxRipples = 24;

cbuffer Water : register(b0, space3)
{
    float4 u_view;  // xy: the world at the target's top-left; z: target texels a world unit; w: the frame (0-frames, continuous)
    float4 u_shape; // xy: the anchor's offset (world units); z: the period (world units); w: frames
    float4 u_mix;   // x: the detail's correlation frame to frame; y: the detail's gain; z: the grain's strength; w: the warp (world units)
    float4 u_grain; // rgb: the detail's colour direction (a unit of luminance)
    float4 u_lo;    // rgb: the tiles' darkest
    float4 u_hi;    // rgb: the tiles' brightest
    float4 u_time;  // x: seconds (looped); y: ripples; z: lights; w: the waves' strength
    float4 u_lights[kMaxLights * 2];  // a light: (x, y, radius, 0), (r, g, b, 0)
    float4 u_ripples[kMaxRipples];    // a ring: x, y, age (s), strength (negative: a splash)
};

struct PSInput {
    float4 v_color : TEXCOORD0; // as SDL's vertex shader names and orders them
    float2 v_uv : TEXCOORD1;
    float4 pos : SV_Position;
};

uint hash2(int2 c)
{
    uint h = uint(c.x) * 374761393u + uint(c.y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

float h01(int2 c)
{
    return float(hash2(c) & 0xFFFFu) / 65535.0;
}

// Value noise, 0-1, a lattice point a unit.
float vnoise(float2 x)
{
    const float2 i = floor(x);
    const float2 f = x - i;
    const float2 u = f * f * (3.0 - 2.0 * f);
    const int2 c = int2(i);
    return lerp(lerp(h01(c), h01(c + int2(1, 0)), u.x), lerp(h01(c + int2(0, 1)), h01(c + int2(1, 1)), u.x), u.y);
}

int wrap(int v, int period)
{
    const int r = v % period;
    return r < 0 ? r + period : r;
}

static const int kApron = 2; // (water_base.hpp's kApron)

// Where texel q (texel middles at whole + 0.5) of frame f's picture lies in the base, wrapped once
// (its apron holds the texels round it), and how far past that texel q is.
int2 corner(int frame, float2 q, int period, out float2 t)
{
    const float2 i = floor(q);
    t = q - i;
    return int2(wrap(int(i.x), period) + kApron, wrap(int(i.y), period) + kApron + frame * (period + 2 * kApron));
}

// The base filtered at texel-space point `at` (the sampler is linear: a read blends four texels).
float3 filtered(float2 at, float2 size)
{
    return u_base.SampleLevel(u_base_sampler, at / size, 0).rgb;
}

// Frame f's picture at world point p, Catmull-Rom, in nine filtered reads (the middle two taps of
// each axis folded into one read between them, weighted by their sum).
float3 picture(int frame, float2 p, int period, float2 size)
{
    float2 t;
    const float2 c = float2(corner(frame, p - 0.5, period, t)) + 0.5; // (the texel's middle)
    const float2 w0 = t * (-0.5 + t * (1.0 - 0.5 * t));
    const float2 w1 = 1.0 + t * t * (-2.5 + 1.5 * t);
    const float2 w2 = t * (0.5 + t * (2.0 - 1.5 * t));
    const float2 w3 = t * t * (-0.5 + 0.5 * t);
    const float2 w12 = w1 + w2;
    const float2 p0 = c - 1.0;
    const float2 p12 = c + w2 / w12;
    const float2 p3 = c + 2.0;
    float3 sum = (filtered(float2(p0.x, p0.y), size) * w0.x + filtered(float2(p12.x, p0.y), size) * w12.x +
                  filtered(float2(p3.x, p0.y), size) * w3.x) * w0.y;
    sum += (filtered(float2(p0.x, p12.y), size) * w0.x + filtered(float2(p12.x, p12.y), size) * w12.x +
            filtered(float2(p3.x, p12.y), size) * w3.x) * w12.y;
    sum += (filtered(float2(p0.x, p3.y), size) * w0.x + filtered(float2(p12.x, p3.y), size) * w12.x +
            filtered(float2(p3.x, p3.y), size) * w3.x) * w3.y;
    return sum;
}

// Frame f's low-pass at p (a block to the right of its picture), bilinear: one filtered read.
float3 lowPass(int frame, float2 p, int period, float2 size)
{
    float2 t;
    const float2 c = float2(corner(frame, p - 0.5, period, t) + int2(period + 2 * kApron, 0)) + 0.5;
    return filtered(c + t, size);
}

// Frame f at p: its detail (picture less low-pass) at the nudged point, its low-pass, its grain.
// The nudge is one still noise for all frames, turned a frame's own way.
void frameAt(int frame, float2 p, float2 nudge, int period, float2 size, out float3 detail, out float3 low, out float grain)
{
    const float a = 2.3999632 * frame; // (the golden angle: each frame's way well apart)
    const float2 warp = float2(nudge.x * cos(a) - nudge.y * sin(a), nudge.x * sin(a) + nudge.y * cos(a));
    low = lowPass(frame, p, period, size);
    detail = picture(frame, p + warp, period, size) - low;
    grain = vnoise(p * 2.6 + float2(17.0 * frame + 3.1, 29.0 * frame + 11.7)) - 0.5;
}

// The waves' slope at p (world units) at t seconds: six directional waves, 3.5 to 34 units long,
// each moving its own way at a speed that grows with its length, the long swells the strongest.
float2 waveSlope(float2 p, float t)
{
    // (way x, way y, length, phase)
    static const float4 kWaves[6] = {
        float4(0.83, 0.56, 34.0, 0.0), float4(-0.42, 0.91, 21.0, 1.7), float4(0.97, -0.24, 13.0, 4.1),
        float4(-0.71, -0.70, 8.5, 2.3), float4(0.20, 0.98, 5.5, 5.9), float4(-0.95, 0.31, 3.6, 0.8),
    };
    static const float kAmplitude[6] = {1.0, 0.8, 0.5, 0.3, 0.15, 0.08};
    float2 slope = 0;
    [unroll] for (int i = 0; i < 6; ++i) {
        const float k = 6.2831853 / kWaves[i].z;
        const float speed = 1.1 * sqrt(kWaves[i].z);
        const float phase = k * (dot(kWaves[i].xy, p) - speed * t) + kWaves[i].w;
        // A wave's height a share of its length (all as steep), its slope the height's derivative.
        slope += kWaves[i].xy * (0.035 * kWaves[i].z * k * kAmplitude[i]) * cos(phase);
    }
    return slope;
}

float4 main(PSInput input) : SV_Target0
{
    const float4 share = u_texture.Sample(u_sampler, input.v_uv);
    const float weight = share.a * input.v_color.a;
    if (weight <= 0.0) {
        return float4(0, 0, 0, 0);
    }
    const int period = int(u_shape.z + 0.5);
    const int frames = int(u_shape.w + 0.5);
    const float2 size = float2(2 * (period + 2 * kApron), frames * (period + 2 * kApron));
    const float2 world = u_view.xy + input.pos.xy / u_view.z;
    const float2 p = world + u_shape.xy;
    const float seconds = u_time.x;
    const float3 pale = lerp(u_lo.rgb, u_hi.rgb, 0.62); // (foam, droplets, crests: the water's palest, muted)
    const float2 nudge = (float2(vnoise(p * 0.7 + float2(13.1, 7.7)), vnoise(p * 0.7 + float2(41.3, 3.1))) - 0.5) * 2.0;

    // The surface first: the waves (nudged by the body's noise, so their crests wander), the
    // shore's lapping, the ripples.
    float2 slope = waveSlope(world + nudge * 3.0, seconds) * u_time.w;
    const float shore = share.r; // 1 open water, falling to 0 where the land is blended in
    float foam = 0;
    float clearer = 0;
    if (shore < 0.995) {
        // The band laps in and out (each stretch of shore at its own time), its foam broken up.
        const float drift = vnoise(world * 0.06);
        const float swing = seconds * 0.9 + drift * 6.2831853;
        const float tide = 0.22 + 0.32 * (0.5 + 0.5 * sin(swing));
        const float band = exp(-((shore - tide) * (shore - tide)) / 0.008);
        foam = band * smoothstep(0.35, 0.8, vnoise(world * 0.8 + float2(seconds * 0.25, 0))) * 0.5;
        clearer = 0.15 * (1.0 - shore); // (shallower: a touch clearer)
        slope += float2(0.35, 0.3) * band * cos(swing);
    }
    const int ripples = min(int(u_time.y + 0.5), kMaxRipples);
    float drops = 0;
    float crests = 0;
    for (int i = 0; i < ripples; ++i) {
        const float4 r = u_ripples[i];
        const float2 d = world - r.xy;
        const float dist = length(d);
        const float age = r.z;
        const float strength = abs(r.w);
        const bool splash = r.w < 0.0;
        // A ring: a few crests running out at 26 units a second, widening and fading.
        const float front = age * 26.0;
        const float width = 3.0 + age * 4.0;
        const float x = dist - front;
        if (abs(x) < width * 2.5) {
            const float envelope = strength * exp(-age * 1.4) * exp(-(x * x) / (width * width)) * (splash ? 2.4 : 1.0);
            const float wave = cos(x * 1.4);
            slope += (d / max(dist, 1e-3)) * envelope * 3.0 * wave;
            crests += envelope * max(wave, 0.0);
        }
        if (splash && age < 0.8 && dist < 45.0) {
            // The crown: a bright ring close in, quickly gone.
            const float crown = 2.5 + age * 18.0;
            drops += strength * saturate(1.0 - age * 3.0) * exp(-((dist - crown) * (dist - crown)) / 2.5);
            // Droplets flung out and falling back (drawn above where they are: up the screen).
            const uint seed = hash2(int2(r.xy * 8.0));
            [loop] for (int j = 0; j < 10; ++j) {
                const uint h = hash2(int2(int(seed & 0xFFFFu), j));
                const float angle = float(h & 0x3FFu) / 1024.0 * 6.2831853;
                const float flung = 14.0 + float((h >> 10) & 0x3Fu) * 0.4;
                const float up = 40.0 + float((h >> 16) & 0x3Fu) * 0.6;
                const float height = up * age - 120.0 * age * age;
                if (height <= 0.0) {
                    continue;
                }
                const float2 at = r.xy + float2(cos(angle), sin(angle)) * flung * age - float2(0.0, height);
                const float2 e = world - at;
                drops += strength * saturate(1.6 - dot(e, e) / 1.4);
            }
        }
    }

    // The body seen through it: bent by the surface's slope (refraction), so the murk moves
    // under the waves and the rings.
    const float2 seen = p + slope * 1.1;
    const float phase = u_view.w;
    const int f0 = wrap(int(floor(phase)), frames);
    const int f1 = wrap(f0 + 1, frames);
    float b = frac(phase);
    b = b * b * (3.0 - 2.0 * b);
    float3 d0, d1, l0, l1;
    float g0, g1;
    frameAt(f0, seen, nudge * u_mix.w, period, size, d0, l0, g0);
    frameAt(f1, seen, nudge * u_mix.w, period, size, d1, l1, g1);
    const float rho = u_mix.x;
    const float norm = sqrt(max((1.0 - b) * (1.0 - b) + b * b + 2.0 * rho * b * (1.0 - b), 1e-4));
    const float gnorm = sqrt((1.0 - b) * (1.0 - b) + b * b);
    const float3 low = lerp(l0, l1, b);
    const float3 detail = (d0 * (1.0 - b) + d1 * b) / norm;
    const float grain = (g0 * (1.0 - b) + g1 * b) / gnorm;
    float3 colour = clamp(low + detail * u_mix.y + grain * u_mix.z * u_grain.rgb, u_lo.rgb, u_hi.rgb);
    colour *= 1.0 + clearer;

    // The surface over it (lit from the upper left, as the game's art is): a gentle relief, and a
    // faint sheen where the waves turn toward that light, as water catches what's above it.
    const float3 n = normalize(float3(-slope, 1.0));
    const float3 toLight = normalize(float3(-0.55, -0.65, 0.55));
    colour *= 1.0 + 0.45 * (dot(n, toLight) - toLight.z);
    const float3 mirrored = reflect(float3(0, 0, -1), n);
    colour += pale * (pow(saturate(dot(mirrored, normalize(float3(-0.45, -0.55, 0.7)))), 24.0) * 0.07 + crests * 0.22);

    // The lights' sheen off the waves.
    const int lights = min(int(u_time.z + 0.5), kMaxLights);
    float3 sheen = 0;
    for (int k = 0; k < lights; ++k) {
        const float4 at = u_lights[k * 2];
        const float2 d = at.xy - world;
        const float reach = saturate(1.0 - length(d) / max(at.z, 1.0));
        if (reach <= 0.0) {
            continue;
        }
        const float3 l = normalize(float3(d, 35.0));
        const float3 h = normalize(l + float3(0, 0, 1));
        sheen += u_lights[k * 2 + 1].rgb * pow(saturate(dot(n, h)), 70.0) * reach * reach;
    }
    colour += sheen * 0.28;

    // Foam, and a splash's crown and droplets a little brighter (they catch the light).
    colour = lerp(colour, pale, saturate(foam));
    colour = lerp(colour, lerp(pale, u_hi.rgb * 1.1, 0.5), saturate(drops));
    return float4(colour * input.v_color.rgb, weight);
}
