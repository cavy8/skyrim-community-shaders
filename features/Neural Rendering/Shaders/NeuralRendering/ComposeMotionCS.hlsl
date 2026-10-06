#include "Common/NeuralRenderingCategories.hlsli"
#include "NeuralRendering/ColorTransfer.hlsli"
#include "NeuralRendering/TemporalReprojection.hlsli"
#include "NeuralRendering/TransferParams.hlsli"

Texture2D<float2> MotionVectors : register(t0);
Texture2D<float4> PreviousGuides : register(t1);
Texture2D<float> Depth : register(t2);
Texture2D<float> Categories : register(t3);
RWTexture2D<float2> ComposedMotion : register(u0);

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= GuideSize))
		return;
	float2 currentJitter = JitterOffset * float2(GuideSize) / float2(ActiveSize) + GuideJitterOffset;
	float2 uv = (float2(id.xy) + 0.5 - currentJitter) / float2(GuideSize);
	float2 motion = MotionVectors[id.xy];
	int2 previousTexel;
	float2 composed = float2(2.0, 2.0);  // Outside history: tell NGX to reject this pixel's temporal sample.
	if (NeuralPreviousGuideTexel(uv + motion, GuideSize, PreviousGuideJitter.xy, previousTexel)) {
		float4 previous = PreviousGuides.Load(int3(previousTexel, 0));
		uint category = NeuralRenderingCategories::Unpack(Categories[id.xy]);
		if (NeuralTemporalSurfaceWeight(Depth[id.xy], category, previous) > 0.5)
			composed = motion + previous.xy;
	}
	ComposedMotion[id.xy] = composed;
}
