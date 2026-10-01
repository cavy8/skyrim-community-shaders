// Encode signed scene-linear NR residuals around neutral gray for private DLSS-SR. Skyrim uses unit
// pre-exposure.

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
