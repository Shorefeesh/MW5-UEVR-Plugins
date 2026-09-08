Texture2D    Tex0 : register(t0);
SamplerState Samp0 : register(s0);

struct PSIn {
    float4 SVPosition : SV_Position;
    float2 UV : TEXCOORD0;
};

float4 Main(PSIn i) : SV_Target {
    float4 marker = Tex0.Sample(Samp0, i.UV);

    // TargetBoxBorder is green in the cooked VR widget. Draw it in this composite
    // so it shares the zoom display's geometry and cannot be clipped by the torso
    // WidgetComponent render target.
    float edgeDistance = min(min(i.UV.x, 1.0 - i.UV.x), min(i.UV.y, 1.0 - i.UV.y));
    float border = 1.0 - smoothstep(0.006, 0.012, edgeDistance);
    float4 borderColor = float4(0.0, border, 0.0, border);
    return borderColor + marker * (1.0 - borderColor.a);
}
