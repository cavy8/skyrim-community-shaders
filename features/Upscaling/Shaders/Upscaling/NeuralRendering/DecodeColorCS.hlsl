#include "Upscaling/NeuralRendering/ColorTransfer.hlsli"

cbuffer TransferParams : register(b0)
{
	float2 JitterOffset;  // Sub-pixel projection offset of the original raster, in render pixels.
	float ColorStrength;
	float TransferParamsPadding;
	uint2 ActiveSize;  // Valid region of OriginalColor and DestinationColor, in their texels.
	uint2 WorkSize;    // Model raster; ModelColor and ProxyColor are allocated at this size.
};

Texture2D<float4> ModelColor : register(t0);     // Feature 18 answer, display-referred proxy domain.
Texture2D<float4> OriginalColor : register(t1);  // Untouched linear scene colour, jittered raster.
Texture2D<float4> ProxyColor : register(t2);     // The exact proxy EncodeColorCS handed the model.
RWTexture2D<float4> DestinationColor : register(u0);
SamplerState LinearClampSampler : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	uint width;
	uint height;
	uint originalWidth;
	uint originalHeight;
	DestinationColor.GetDimensions(width, height);
	OriginalColor.GetDimensions(originalWidth, originalHeight);
	uint2 active = min(ActiveSize, min(uint2(width, height), uint2(originalWidth, originalHeight)));
	if (any(dispatchThreadID.xy >= active) || any(ActiveSize == 0))
		return;

	// The original pixel holds scene position (pixel - JitterOffset) on the
	// unjittered grid the model saw. The model and proxy textures span that same
	// active region at the model raster, so normalising by the active size lands
	// on the matching model position whatever the scale. Sample the model's
	// answer and the proxy it was given there so the ratio between them is the
	// edit for this exact scene point; at native scale with a zero offset this is
	// the texel centre.
	float2 uv = (float2(dispatchThreadID.xy) + 0.5 - JitterOffset) / float2(ActiveSize);
	float4 model = ModelColor.SampleLevel(LinearClampSampler, uv, 0);
	float4 proxy = ProxyColor.SampleLevel(LinearClampSampler, uv, 0);
	DestinationColor[dispatchThreadID.xy] = ResolveNeuralColor(model, proxy,
		OriginalColor[dispatchThreadID.xy], ColorStrength);
}
