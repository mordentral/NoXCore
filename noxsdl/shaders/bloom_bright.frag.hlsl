// The bright pass of the bloom for light sources' sprites (flames, fireballs, explosions,
// torches; bloom.cpp): what's brighter than the threshold, as the sprite was drawn (the vertex
// colour carries its light tint and alpha), so only the fire and glow bloom, not the torch's
// handle or a missile's dark edge. Added by colour into the bloom target.

Texture2D u_texture : register(t0, space2);
SamplerState u_sampler : register(s0, space2);

struct PSInput {
    float4 v_color : TEXCOORD0; // as SDL's vertex shader names them
    float2 v_uv : TEXCOORD1;
};

// Keyed on the brightest channel: the game's fire, sparks and magic reach full red, blue or green
// while stone and wood beside them (the fireplace's own sprite has both) stay under 0.75; measured
// on Estate's fireplace. Luminance would lose orange fire, which is dark by luminance.
static const float kThreshold = 0.78;

float4 main(PSInput input) : SV_Target0
{
    const float4 c = u_texture.Sample(u_sampler, input.v_uv) * input.v_color;
    const float brightest = max(c.r, max(c.g, c.b));
    const float keep = saturate((brightest - kThreshold) / (1.0 - kThreshold));
    return float4(c.rgb * keep * c.a, 1.0);
}
