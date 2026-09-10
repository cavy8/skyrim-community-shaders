#include "Upscaling/NeuralRendering/ColorTransfer.hlsli"

cbuffer TransferParams : register(b0)
{
	float2 JitterOffset;  // Sub-pixel projection offset of the original raster, in render pixels.
	float ColorStrength;
	float TransferParamsPadding;
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
	uint modelWidth;
	uint modelHeight;
	uint originalWidth;
	uint originalHeight;
	uint proxyWidth;
	uint proxyHeight;
	DestinationColor.GetDimensions(width, height);
	ModelColor.GetDimensions(modelWidth, modelHeight);
	OriginalColor.GetDimensions(originalWidth, originalHeight);
	ProxyColor.GetDimensions(proxyWidth, proxyHeight);
	uint2 active = min(min(uint2(width, height), uint2(modelWidth, modelHeight)),
		min(uint2(originalWidth, originalHeight), uint2(proxyWidth, proxyHeight)));
	if (any(dispatchThreadID.xy >= active))
		return;

	// The original pixel holds scene position (pixel - JitterOffset) on the
	// unjittered grid the model saw. Sample the model's answer and the proxy it
	// was given at that same position so the ratio between them is the edit for
	// this exact scene point; the model and proxy textures are compact, so their
	// size is the active region. With a zero offset this lands on the texel.
	float2 uv = (float2(dispatchThreadID.xy) + 0.5 - JitterOffset) / float2(modelWidth, modelHeight);
	float4 model = ModelColor.SampleLevel(LinearClampSampler, uv, 0);
	float4 proxy = ProxyColor.SampleLevel(LinearClampSampler, uv, 0);
	DestinationColor[dispatchThreadID.xy] = ResolveNeuralColor(model, proxy,
		OriginalColor[dispatchThreadID.xy], ColorStrength);
}
