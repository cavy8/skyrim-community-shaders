// Decodes the independently DLSS-upscaled signed residual and applies it to the
// clean result from the game's normal DLSS history. The inverse is kept away
// from its poles and invalid carrier samples become a no-op. Alpha remains
// renderer-owned.

Texture2D<float4> CleanColor : register(t0);
Texture2D<float4> ResidualCarrier : register(t1);
RWTexture2D<float4> DestinationColor : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	uint width;
	uint height;
	DestinationColor.GetDimensions(width, height);
	if (any(dispatchThreadID.xy >= uint2(width, height)))
		return;

	float4 clean = CleanColor[dispatchThreadID.xy];
	float3 carrier = ResidualCarrier[dispatchThreadID.xy].rgb;
	float3 signedUnit = clamp((carrier - 0.5) * 2.0, -0.999, 0.999);
	float3 delta = signedUnit / max(1.0 - abs(signedUnit), 0.001);
	if (!all(carrier == carrier) || !all(delta == delta))
		delta = 0.0;

	DestinationColor[dispatchThreadID.xy] = float4(max(clean.rgb + delta, 0.0), clean.a);
}
