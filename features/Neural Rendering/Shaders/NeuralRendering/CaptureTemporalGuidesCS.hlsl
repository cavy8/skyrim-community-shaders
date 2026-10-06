#include "Common/NeuralRenderingCategories.hlsli"
#include "NeuralRendering/ColorTransfer.hlsli"
#include "NeuralRendering/TransferParams.hlsli"

Texture2D<float2> MotionVectors : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float> Categories : register(t2);
RWTexture2D<float4> TemporalGuides : register(u0);

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= GuideSize))
		return;
	TemporalGuides[id.xy] = float4(MotionVectors[id.xy], Depth[id.xy],
		float(NeuralRenderingCategories::Unpack(Categories[id.xy])));
}
