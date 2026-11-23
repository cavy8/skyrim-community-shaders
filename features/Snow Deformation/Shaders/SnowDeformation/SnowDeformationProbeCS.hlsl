#include "SnowDeformation/Surface.hlsli"
#include "SnowDeformation/UpdateParams.hlsli"

Texture2D<float> Field : register(t0);
Texture2D<float> BlanketRaise : register(t1);
SamplerState FieldSampler : register(s0);
RWBuffer<float2> Depths : register(u0);

float RaiseAt(float2 windowPosition)
{
	float raise = 0.0;
	[branch] if (BlanketShape.z > 0.5)
	{
		const float2 cell = (windowPosition + float2(OriginCell.xy) * Control.z) * (1.0 / BLANKET_CELL);
		const int2 inside = (int2)floor(cell) - BlanketCell.xy;
		[branch] if (all(inside >= 0) && all(inside < BLANKET_TEXELS - 1))
			raise = BlanketRaise.SampleLevel(FieldSampler, (cell + 0.5) * (1.0 / BLANKET_TEXELS), 0);
	}
	return raise;
}

float SurfaceAt(float2 windowPosition)
{
	float2 texel = windowPosition / Control.z + float2(OriginCell.xy & (FIELD_TEXELS - 1)) + 0.5;
	float2 fromCentre = abs(windowPosition - Probe.x);
	float fade = 1.0 - saturate((max(fromCentre.x, fromCentre.y) - Probe.y) * Probe.z);
	float field = Field.SampleLevel(FieldSampler, texel / FIELD_TEXELS, 0) * fade;
	uint kind = SurfaceOf(Field.Load(int3((int2)floor(texel) & (FIELD_TEXELS - 1), 0)));
	float raise = RaiseAt(windowPosition);
	float surface = raise + field;
	return BlanketShape.z > 0.5 && kind == SURFACE_SNOW ? max(surface, 0.0) : surface;
}

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	const uint i = dispatchID.x;
	if (i >= min((uint)Control.y, (uint)MAX_STAMPS))
		return;

	const float4 stamp = Stamps[i];
	const float4 shape = StampShape[i];
	const float halfLength = max(stamp.z, 1e-3);
	const float2 forward = shape.z > 0.0 ? normalize(shape.xy) : float2(1.0, 0.0);
	const float2 right = float2(-forward.y, forward.x);
	const float halfWidth = shape.z > 0.0 ? max(shape.z, 1e-3) : halfLength;

	float height = 0.0;
	[unroll] for (int y = 0; y < 4; y++)
	{
		[unroll] for (int x = 0; x < 4; x++)
		{
			const float2 offset = (float2(x, y) - 1.5) * (Probe.w / 1.5);
			height += SurfaceAt(stamp.xy + forward * (offset.x * halfLength) + right * (offset.y * halfWidth));
		}
	}
	Depths[i] = float2(max(-height / 16.0, 0.0), RaiseAt(stamp.xy));
}
