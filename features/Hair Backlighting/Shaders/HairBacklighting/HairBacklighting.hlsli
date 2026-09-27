#ifndef __HAIR_BACKLIGHTING_DEPENDENCY_HLSL__
#define __HAIR_BACKLIGHTING_DEPENDENCY_HLSL__

#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/LightingCommon.hlsli"

// Light transmitted through hair when the viewer looks toward a light behind it:
// the glowing halo of backlit hair, the hair counterpart of Foliage Lighting's
// tree foliage transmission. Added to DirectLightingOutput.transmission for every
// light, on top of whichever hair shading model produced the rest of the output.
namespace HairBacklighting
{
	// Forward-scatter lobe around the light direction, GGX-shaped like
	// GetFoliageTransmission() but normalized to 1 when the light sits directly
	// behind the hair, so Strength reads as a multiple of the light's color.
	// ScatterWidth widens the halo as the camera moves off the light axis. The
	// extra forwardScatter factor removes the GGX tail on the viewer's side of
	// the hair, where no light passes through it toward the camera.
	float GetForwardScatter(float VdotL)
	{
		const float forwardScatter = saturate(-VdotL);
		const float alpha2 = SharedData::hairBacklightingSettings.ScatterWidth * SharedData::hairBacklightingSettings.ScatterWidth;
		const float denominator = forwardScatter * forwardScatter * (alpha2 - 1.0) + 1.0;
		return forwardScatter * (alpha2 * alpha2) / (denominator * denominator);
	}

	// Thickness proxy. Strands seen edge-on (silhouettes, flyaways) have the least
	// hair between the light and the camera and glow the most; InteriorGlow keeps
	// some light passing through the body of the hair. Uses the geometric normal,
	// not the normal map or Hair Specular's strand tangent, so the halo follows the
	// hair's outline rather than per-strand detail; abs() covers double-sided cards.
	float GetThinness(float3 geometricNormal, float3 V)
	{
		const float edge = pow(saturate(1.0 - abs(dot(geometricNormal, V))), SharedData::hairBacklightingSettings.EdgeFalloff);
		return lerp(SharedData::hairBacklightingSettings.InteriorGlow, 1.0, edge);
	}

	// Local exposure compensation. Skyrim barely adapts to dark scenes, so a fire
	// behind a character at night produces a glow that is as bright as it should be
	// in absolute terms but reads as muted next to the sun's in daylight. Below
	// DarkThreshold, the glow is scaled up by how much darker the hair's ambient
	// light is than the threshold, capped at DarkBoost; at or above it the gain is 1,
	// so daylight tuning is unaffected. The level is the ambient term only (the
	// constant part of the vanilla directional ambient, i.e. its average over all
	// directions): counting the direct lights would let a fire cancel its own boost.
	// The threshold is given as a vanilla (gamma) ambient brightness and goes through
	// the same Color::Ambient() conversion, so the ratio is taken in the lighting
	// space in use with or without Linear Lighting. DirectionalAmbient is the
	// Lighting.hlsl pixel-shader constant.
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

		// Light crossing several fibers is absorbed by their pigment each time, so the
		// halo is a deeper, more saturated shade of the hair color (blonde turns gold).
		const float3 tint = pow(max(baseColor, 0.0), SharedData::hairBacklightingSettings.Absorption);

		float3 transmission = tint * (scatter * GetThinness(normalize(geometricNormal), V) * SharedData::hairBacklightingSettings.Strength * GetDarkSurroundingsGain());
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
