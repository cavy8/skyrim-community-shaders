#include "Upscaling/NeuralRendering/ColorTransfer.hlsli"

Texture2D<float4> SourceColor : register(t0);
RWTexture2D<float4> DestinationColor : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	uint width;
	uint height;
	uint sourceWidth;
	uint sourceHeight;
	DestinationColor.GetDimensions(width, height);
	SourceColor.GetDimensions(sourceWidth, sourceHeight);
	if (dispatchThreadID.x < min(width, sourceWidth) && dispatchThreadID.y < min(height, sourceHeight))
		DestinationColor[dispatchThreadID.xy] = EncodeNeuralColor(SourceColor[dispatchThreadID.xy]);
}
