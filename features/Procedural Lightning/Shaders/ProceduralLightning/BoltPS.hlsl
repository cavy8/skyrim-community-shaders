#include "Common/ColorManagement.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "ProceduralLightning/Common.hlsli"

#if defined(TERRAIN_BLENDING)
Texture2D<float> SceneDepth : register(t0);
#else
Texture2D<SCENE_DEPTH_FORMAT> SceneDepth : register(t0);
#endif

static const float MaxRadiance = 256.0;
static const float WhiteCore = 0.8;
static const float GlowAmount = 0.055;
static const float FrontSharpness = 48.0;

struct PS_OUTPUT
{
	float4 Color : SV_Target0;
	float4 Mask : SV_Target1;
};

float Erf(float x)
{
	float a = abs(x);
	float p = 1.0 + a * (0.278393 + a * (0.230389 + a * (0.000972 + a * 0.078108)));
	p *= p;
	p *= p;
	return sign(x) * (1.0 - 1.0 / p);
}

float LineKernel(float across, float along, float axisLength, float sigma)
{
	float scale = 0.70710678 / sigma;
	float perpendicular = across * scale;
	return exp(-perpendicular * perpendicular) * 0.5 * (Erf((axisLength - along) * scale) + Erf(along * scale));
}

bool IsOccluded(float2 pixel, float inverseDepth)
{
	return SharedData::GetScreenDepth(SceneDepth.Load(int3(pixel, 0))) * inverseDepth < 1.0;
}

PS_OUTPUT main(ProceduralLightning::VertexOutput input)
{
	PS_OUTPUT output = (PS_OUTPUT)0;

	if (input.Params.y >= ProceduralLightning::KindCloudGlow) {
		float radiusSquared = dot(input.Corner, input.Corner);
		if (radiusSquared >= 1.0 || IsOccluded(input.Position.xy, input.Ends.x))
			discard;

		float falloff = 1.0 - radiusSquared;
		float noise = Random::perlinNoise(float3(input.Corner * 2.6, input.Shape.x)) * 0.65 + Random::perlinNoise(float3(input.Corner * 6.1, input.Shape.y)) * 0.35;
		output.Color = float4(PreExposure::Encode(min(input.Tint.rgb * input.Tint.a * falloff * falloff * saturate(0.55 + noise), MaxRadiance)), 0.0);
		return output;
	}

	float2 axis = input.Line.zw - input.Line.xy;
	float axisLength = length(axis);
	if (axisLength <= 1e-3)
		discard;

	float2 direction = axis / axisLength;
	float2 offset = input.Position.xy - input.Line.xy;
	float along = dot(offset, direction);
	float across = dot(offset, float2(-direction.y, direction.x));

	float core = LineKernel(across, along, axisLength, input.Shape.z);
	float glow = input.Shape.w > 0.0 ? LineKernel(across, along, axisLength, input.Shape.w) : 0.0;
	if (core + glow <= 0.002)
		discard;

	float t = saturate(along / axisLength);
	if (IsOccluded(input.Position.xy, lerp(input.Ends.x, input.Ends.y, t)))
		discard;

	float energy = lerp(input.Ends.z, input.Ends.w, t);
	float revealed = saturate((input.Params.x - lerp(input.Shape.x, input.Shape.y, t)) * FrontSharpness);
	float amount = energy * revealed * input.Tint.a;
	if (amount <= 0.0)
		discard;

	float3 color = lerp(input.Tint.rgb, 1.0, WhiteCore) * core + input.Tint.rgb * glow * GlowAmount;
	output.Color = float4(PreExposure::Encode(min(color * amount, MaxRadiance)), 0.0);
	output.Mask = float4(0.0, saturate(core * energy * revealed * 3.0), 0.0, 0.0);
	return output;
}
