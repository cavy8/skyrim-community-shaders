#include "NeuralRendering/ColorTransfer.hlsli"
#include "NeuralRendering/TransferParams.hlsli"

Texture2D<float4> SourceColor : register(t0);
// ISHDR's AvgTex from the previous frame's tonemap pass: x adapted luminance, y target luminance.
Texture2D<float2> VanillaAdaptation : register(t1);
// Post Processing's Histogram Auto Exposure adapted luminance (a single float).
StructuredBuffer<float> PostProcessAdaptation : register(t2);
RWTexture2D<float4> DestinationColor : register(u0);
SamplerState LinearClampSampler : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	uint width;
	uint height;
	uint sourceWidth;
	uint sourceHeight;
	DestinationColor.GetDimensions(width, height);
	SourceColor.GetDimensions(sourceWidth, sourceHeight);
	// The source may be a natively allocated game target whose valid region is
	// the top-left ActiveSize; the destination is compact at the model raster.
	uint2 active = min(ActiveSize, uint2(sourceWidth, sourceHeight));
	uint2 work = min(WorkSize, uint2(width, height));
	if (any(dispatchThreadID.xy >= work) || any(active == 0))
		return;

	// Map unjittered model pixels into the source raster. Use area filtering on minified axes and Catmull-
	// Rom on native axes to suppress aliasing.
	float2 footprint = float2(active) / float2(work);
	float2 scenePosition = (float2(dispatchThreadID.xy) + 0.5) * footprint;
	float2 position = scenePosition + JitterOffset;
	float3 color = any(footprint > 1.0) ? SampleNeuralSourceAreaMinify(SourceColor, position, footprint, float2(active)) : SampleNeuralSourceCatmullRom(SourceColor, LinearClampSampler, position, float2(active), float2(sourceWidth, sourceHeight));
	uint2 nearest = min(uint2(scenePosition), active - 1);
	float alpha = SourceColor[nearest].a;

	// Adaptation textures are uniform; sample their centers. Unbound inputs read zero and disable their
	// contribution.
	NeuralDisplayTransform display = MakeNeuralDisplayTransform(DisplayParam, DisplayCinematic, DisplayTint, DisplayExposure,
		VanillaAdaptation.SampleLevel(LinearClampSampler, float2(0.5, 0.5), 0), PostProcessAdaptation[0], HighlightWhite,
		ProxyCurve);
	DestinationColor[dispatchThreadID.xy] = EncodeNeuralColor(float4(color, alpha), ColorDomain, NeuralTransferModelSpace(), display);
}
