// Encodes the render-resolution Neural Rendering contribution into a signed,
// bounded carrier suitable for an independent DLSS Super Resolution history.
// Neutral grey is an exact zero edit. Skyrim's scene colour is not pre-exposed,
// so the reference pipeline's pre-exposure divisor is one here.

Texture2D<float4> OriginalColor : register(t0);
Texture2D<float4> EditedColor : register(t1);
RWTexture2D<float4> ResidualCarrier : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	uint width;
	uint height;
	ResidualCarrier.GetDimensions(width, height);
	if (any(dispatchThreadID.xy >= uint2(width, height)))
		return;

	float3 original = OriginalColor[dispatchThreadID.xy].rgb;
	float3 edited = EditedColor[dispatchThreadID.xy].rgb;
	float3 delta = edited - original;
	if (!all(delta == delta))
		delta = 0.0;
	delta = clamp(delta, -65504.0, 65504.0);

	float3 signedUnit = delta / (1.0 + abs(delta));
	ResidualCarrier[dispatchThreadID.xy] = float4(0.5 + 0.5 * signedUnit, 1.0);
}
