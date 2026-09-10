#include "Upscaling/NeuralRendering/ColorTransfer.hlsli"

cbuffer TransferParams : register(b0)
{
	float2 JitterOffset;  // Sub-pixel projection offset of the source raster, in render pixels.
	float ColorStrength;
	float TransferParamsPadding;
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
	// The destination is compact, so its extent is the active region of the
	// (possibly natively allocated) source.
	uint2 active = min(uint2(width, height), uint2(sourceWidth, sourceHeight));
	if (any(dispatchThreadID.xy >= active))
		return;

	// Each destination texel is an unjittered pixel. The raster holds that scene
	// position at + JitterOffset, so resample it from there; with a zero offset
	// this is exactly the texel itself.
	float2 position = float2(dispatchThreadID.xy) + 0.5 + JitterOffset;
	float3 color = SampleNeuralSourceCatmullRom(SourceColor, LinearClampSampler, position,
		float2(active), float2(sourceWidth, sourceHeight));
	float alpha = SourceColor[dispatchThreadID.xy].a;
	DestinationColor[dispatchThreadID.xy] = EncodeNeuralColor(float4(color, alpha));
}
