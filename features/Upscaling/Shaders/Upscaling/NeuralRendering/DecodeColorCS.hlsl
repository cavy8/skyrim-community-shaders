#include "Upscaling/NeuralRendering/ColorTransfer.hlsli"

cbuffer ResolveParams : register(b0)
{
	float ColorStrength;
	float3 ResolveParamsPadding;
};

Texture2D<float4> SourceColor : register(t0);
Texture2D<float4> OriginalColor : register(t1);
RWTexture2D<float4> DestinationColor : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	uint width;
	uint height;
	uint sourceWidth;
	uint sourceHeight;
	uint originalWidth;
	uint originalHeight;
	DestinationColor.GetDimensions(width, height);
	SourceColor.GetDimensions(sourceWidth, sourceHeight);
	OriginalColor.GetDimensions(originalWidth, originalHeight);
	if (dispatchThreadID.x < min(width, min(sourceWidth, originalWidth)) &&
		dispatchThreadID.y < min(height, min(sourceHeight, originalHeight)))
		DestinationColor[dispatchThreadID.xy] = ResolveNeuralColor(
			SourceColor[dispatchThreadID.xy], OriginalColor[dispatchThreadID.xy], ColorStrength);
}
