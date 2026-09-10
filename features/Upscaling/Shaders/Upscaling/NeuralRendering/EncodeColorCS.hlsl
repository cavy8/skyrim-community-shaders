#include "Upscaling/NeuralRendering/ColorTransfer.hlsli"

cbuffer TransferParams : register(b0)
{
	float2 JitterOffset;  // Sub-pixel projection offset of the source raster, in render pixels.
	float ColorStrength;
	float TransferStrength;
	uint2 ActiveSize;  // Valid region of SourceColor, in source texels.
	uint2 WorkSize;    // Model raster; DestinationColor is allocated at this size.
	uint2 GuideSize;   // Unused here; keeps the layout shared with DecodeColorCS.
	uint DepthAwareResolve;
	uint SkipFrame;
};

Texture2D<float4> SourceColor : register(t0);
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

	// Each destination texel is an unjittered model pixel covering scene position
	// (id + 0.5) * active / work on the source grid. The raster holds that scene
	// position at + JitterOffset, so resample it from there. At native scale with
	// a zero offset this is exactly the source texel itself.
	//
	// The same Catmull-Rom kernel serves every scale. It has a four-texel support
	// on the source grid, so down to half resolution the model pixel's footprint
	// stays inside the kernel; below that the proxy aliases mildly, which the
	// resolve tolerates because only a bounded luminance ratio and chroma ever
	// return to the full-resolution frame.
	float2 scenePosition = (float2(dispatchThreadID.xy) + 0.5) * float2(active) / float2(work);
	float2 position = scenePosition + JitterOffset;
	float3 color = SampleNeuralSourceCatmullRom(SourceColor, LinearClampSampler, position,
		float2(active), float2(sourceWidth, sourceHeight));
	uint2 nearest = min(uint2(scenePosition), active - 1);
	float alpha = SourceColor[nearest].a;
	DestinationColor[dispatchThreadID.xy] = EncodeNeuralColor(float4(color, alpha));
}
