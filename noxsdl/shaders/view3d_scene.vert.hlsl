// The 3D view's depth-buffered world (view3d_gpu.cpp): positions arrive in clip space, worked out on the
// CPU (the view's shear and depth range are there); this passes them on with the colour and the
// texture coordinate, which the hardware interpolates with perspective.
struct VSInput {
    float4 pos : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 color : TEXCOORD2;
};

struct VSOutput {
    float4 color : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 pos : SV_Position;
};

VSOutput main(VSInput input)
{
    VSOutput o;
    o.pos = input.pos;
    o.uv = input.uv;
    o.color = input.color;
    return o;
}
