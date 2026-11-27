// An image of ours over the game's frame (video.cpp, the UI images: the
// achievements' icons): sampled across the rect it's drawn in, greyed for a
// locked one, its alpha scaled.

struct PushConstants
{
    uint TextureIndex;
    uint SamplerIndex;
    float Alpha;
    uint Grey;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD0) : SV_Target
{
    float4 color = g_Texture2DDescriptorHeap[g_PushConstants.TextureIndex].SampleLevel(
        g_SamplerDescriptorHeap[g_PushConstants.SamplerIndex], texCoord, 0);
    if (g_PushConstants.Grey != 0)
    {
        color.rgb = dot(color.rgb, float3(0.299, 0.587, 0.114));
    }
    color.a *= g_PushConstants.Alpha;
    return color;
}
