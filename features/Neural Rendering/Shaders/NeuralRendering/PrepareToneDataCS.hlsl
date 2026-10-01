#include "NeuralRendering/ColorTransfer.hlsli"
#include "NeuralRendering/TransferParams.hlsli"

// Prepare proxy log luminance and edit stops at the model raster. Retain this data with its
// proxy/answer pair across skipped evaluations.

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
	float3 model = NeuralModelToLinear(ModelColor[dispatchThreadID.xy].rgb, space);

	float proxyLuma = dot(proxy, kNeuralLuma);
	float modelLuma = dot(model, kNeuralLuma);
	// Use the resolve floor. Black pixels may be valid shadows; empty output is rejected per frame.
	float delta = log2((modelLuma + kNeuralRatioFloor) / (proxyLuma + kNeuralRatioFloor));

	ToneData[dispatchThreadID.xy] = float2(log2(proxyLuma + kNeuralRatioFloor), delta);
}
