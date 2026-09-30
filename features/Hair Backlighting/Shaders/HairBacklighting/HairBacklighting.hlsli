#ifndef __HAIR_BACKLIGHTING_DEPENDENCY_HLSL__
#define __HAIR_BACKLIGHTING_DEPENDENCY_HLSL__

#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/LightingCommon.hlsli"

namespace HairBacklighting
{
	// Limits scattering to lights behind the hair and widens the glow with ScatterWidth.
	float GetForwardScatter(float VdotL)
	{
		const float forwardScatter = saturate(-VdotL);
		const float alpha2 = SharedData::hairBacklightingSettings.ScatterWidth * SharedData::hairBacklightingSettings.ScatterWidth;
		const float denominator = forwardScatter * forwardScatter * (alpha2 - 1.0) + 1.0;
		return forwardScatter * (alpha2 * alpha2) / (denominator * denominator);
	}

	// Uses the geometric normal so the glow follows the hair outline; abs() handles double-sided cards.
	float GetThinness(float3 geometricNormal, float3 V)
	{
		const float edge = pow(saturate(1.0 - abs(dot(geometricNormal, V))), SharedData::hairBacklightingSettings.EdgeFalloff);
		return lerp(SharedData::hairBacklightingSettings.InteriorGlow, 1.0, edge);
	}

	// Approximates head shadowing for point lights, which do not cast shadows.
	float GetHeadOcclusion(float3 geometricNormal, float3 V, float3 L)
	{
		const float3 lightOffset = L - V * dot(V, L);
		const float behind = saturate(-dot(geometricNormal, lightOffset));
		return 1.0 - SharedData::hairBacklightingSettings.HeadOcclusion * smoothstep(0.0, 0.4, behind);
	}

	// Uses ambient light only so a fire does not cancel its own boost.
	float GetDarkSurroundingsGain()
	{
		const float3 averageAmbient = max(0.0, float3(DirectionalAmbient._m03, DirectionalAmbient._m13, DirectionalAmbient._m23));
		const float ambientLuminance = Color::RGBToLuminance(Color::Ambient(averageAmbient));
		const float thresholdLuminance = Color::RGBToLuminance(Color::Ambient(SharedData::hairBacklightingSettings.DarkThreshold.xxx));
		return clamp(thresholdLuminance / max(ambientLuminance, 1e-5), 1.0, SharedData::hairBacklightingSettings.DarkBoost);
	}

	/**
	 * @brief Adds backlit hair transmission for one light.
	 * @param lightingOutput Direct lighting output of the current light; transmission is accumulated.
	 * @param context Light context; viewDir, lightDir, lightColor and detailedShadow are used.
	 * @param geometricNormal World-space interpolated vertex normal (tbnTr[2]).
	 * @param baseColor Hair base color; light is absorbed by it on the way through (Absorption).
	 */
	void AddDirectLight(inout DirectLightingOutput lightingOutput, DirectContext context, float3 geometricNormal, float3 baseColor)
	{
		if (SharedData::hairBacklightingSettings.Enable == 0)
			return;

		const float3 V = normalize(context.viewDir);
		const float3 L = normalize(context.lightDir);

		const float scatter = GetForwardScatter(dot(V, L));
		if (scatter <= 0.0)
			return;

		// Approximate pigment absorption as light passes through the hair.
		const float3 tint = pow(max(baseColor, 0.0), SharedData::hairBacklightingSettings.Absorption);

		const float3 N = normalize(geometricNormal);
		float3 transmission = tint * (scatter * GetThinness(N, V) * GetHeadOcclusion(N, V, L) * SharedData::hairBacklightingSettings.Strength * GetDarkSurroundingsGain());
		transmission *= context.lightColor * context.detailedShadow;
#if defined(TRUE_PBR)
		transmission *= BRDF::Diffuse_Lambert();
#else
		transmission *= Color::VanillaNormalization();
#endif
		lightingOutput.transmission += transmission;
	}
}

#endif  //__HAIR_BACKLIGHTING_DEPENDENCY_HLSL__
