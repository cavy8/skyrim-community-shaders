#include "NeuralRendering/ColorTransfer.hlsli"
#include "NeuralRendering/TransferParams.hlsli"

// First of the three passes that split the model's luminance edit into a smooth band and a
// detail band (see ColorTransfer.hlsli, ResolveNeuralColor). It runs once per model evaluation,
// at the model raster, right after the answer has been copied back - so the pair it reads is
// exactly the pair the resolve will use, and an alternating-frame skip keeps the previous
// evaluation's data rather than mixing two frames' answers.
//
// The output carries the proxy's log luminance alongside the edit because the filter that
// follows is edge-aware: it needs to know where the *image* has an edge, not just where the
// edit does, or it blurs the edit straight across a silhouette. Half precision gives about
// 0.01 stops of resolution, which is far finer than anything the edit resolves.

Texture2D<float4> ProxyColor : register(t0);  // The proxy the model was handed.
Texture2D<float4> ModelColor : register(t1);  // Feature 18's answer for it.
RWTexture2D<float2> ToneData : register(u0);  // x: log2 proxy luminance, y: the edit in stops.

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	uint width;
	uint height;
	ToneData.GetDimensions(width, height);
	uint2 work = min(WorkSize, uint2(width, height));
	if (any(dispatchThreadID.xy >= work) || any(work == 0))
		return;

	const uint space = NeuralTransferModelSpace();
	float3 proxy = NeuralModelToLinear(ProxyColor[dispatchThreadID.xy].rgb, space);
	float3 model = NeuralModelToLinear(NeuralTransferModelChannels(ModelColor[dispatchThreadID.xy].rgb), space);

	float proxyLuma = dot(proxy, kNeuralLuma);
	float modelLuma = dot(model, kNeuralLuma);
	// The same floor and the same invalid-answer test the resolve applies, so the band data
	// never disagrees with the edit it is meant to describe.
	float delta = modelLuma > 1e-5 ? log2((modelLuma + kNeuralRatioFloor) / (proxyLuma + kNeuralRatioFloor)) : 0.0;

	ToneData[dispatchThreadID.xy] = float2(log2(proxyLuma + kNeuralRatioFloor), delta);
}
