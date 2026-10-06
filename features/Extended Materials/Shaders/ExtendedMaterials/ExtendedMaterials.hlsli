// https://github.com/tgjones/slimshader-cpp/blob/master/src/Shaders/Sdk/Direct3D11/DetailTessellation11/POM.hlsl
// https://github.com/alandtse/SSEShaderTools/blob/main/shaders_vr/ParallaxEffect.h

// https://github.com/marselas/Zombie-Direct3D-Samples/blob/5f53dc2d6f7deb32eb2e5e438d6b6644430fe9ee/Direct3D/ParallaxOcclusionMapping/ParallaxOcclusionMapping.fx
// http://www.diva-portal.org/smash/get/diva2:831762/FULLTEXT01.pdf
// https://bartwronski.files.wordpress.com/2014/03/ac4_gdc.pdf

#ifndef EXTENDED_MATERIALS_HLSLI
#define EXTENDED_MATERIALS_HLSLI

#if defined(TERRAIN_VARIATION)
#	include "TerrainVariation/TerrainVariation.hlsli"
#else
struct StochasticOffsets
{
	float2 offset1;
	float2 offset2;
	float tap1Weight;
};
#endif

struct DisplacementParams
{
	float DisplacementScale;
	float DisplacementOffset;
	float HeightScale;
	float FlattenAmount;
};

namespace ExtendedMaterials
{
	static const float ShadowIntensity = 2.0;
	// Terrain shadow strengths: point lights match the 4/tapCount object scale, directional uses half.
	static const float TerrainPointShadowStrength = 4.0;
	static const float TerrainDirectionalShadowStrength = 2.0;
	static const float ParallaxCheapDistance = 1024.0;
	static const float ParallaxNearShadowQuality = 1.0;
	static const float ParallaxFarShadowQuality = 0.5;
	static const float TerrainParallaxShadowMaxMipLevel = 2.0;
	static const float NormalMapShadowMaxDistance = 4096.0;
	static const float NormalMapShadowBaseSteps = 12.0;
	static const float NormalMapShadowMinNormalZ = 0.35;
	static const float NormalMapShadowPointLightQuality = 0.5;

	inline uint ParallaxShadowTapCount(float quality)
	{
		uint taps = 1;
		if (quality > 0.25)
			taps++;
		if (quality > 0.5)
			taps++;
		if (quality > 0.75)
			taps++;
		return taps;
	}

	float ScaleDisplacement(float displacement, DisplacementParams params)
	{
		return (displacement - 0.5) * params.HeightScale;
	}

	float AdjustDisplacementNormalized(float displacement, DisplacementParams params)
	{
		return (displacement - 0.5) * params.DisplacementScale + 0.5 + params.DisplacementOffset;
	}

	float4 AdjustDisplacementNormalized(float4 displacement, DisplacementParams params)
	{
		return float4(AdjustDisplacementNormalized(displacement.x, params), AdjustDisplacementNormalized(displacement.y, params), AdjustDisplacementNormalized(displacement.z, params), AdjustDisplacementNormalized(displacement.w, params));
	}
	
	float GetMipLevelFromDims(float2 coords, float2 textureDims)
	{
#	if !defined(PARALLAX) && !defined(TRUE_PBR)
		textureDims /= 2.0;
#	endif

		float2 texCoordsPerSize = coords * textureDims;

		// Compute the current gradients:
		float2 dxSize = ddx(texCoordsPerSize);
		float2 dySize = ddy(texCoordsPerSize);

		// Standard mipmapping uses max here
		float minTexCoordDelta = min(dot(dxSize, dxSize), dot(dySize, dySize));

		// Compute the current mip level  (* 0.5 is effectively computing a square root before )
		float mipLevel = max(0.5 * log2(minTexCoordDelta), 0);

#	if !defined(PARALLAX) && !defined(TRUE_PBR)
		mipLevel++;
#	endif

		return max(mipLevel + SharedData::MipBias, 0);
	}

	float GetMipLevel(float2 coords, Texture2D<float4> tex)
	{
		float2 textureDims;
		tex.GetDimensions(textureDims.x, textureDims.y);
		return GetMipLevelFromDims(coords, textureDims);
	}

#	if defined(EMAT)
#		if defined(LANDSCAPE)
#			include "ExtendedMaterials/ExtendedMaterialsTerrain.hlsli"
#		endif
#		include "ExtendedMaterials/ExtendedMaterialsParallaxCore.hlsli"
#	endif

#	if defined(EMAT_NMS)
	float GetNormalMapShadowFootprint(float2 coords)
	{
		float2 dx = ddx(coords);
		float2 dy = ddy(coords);
		return sqrt(min(dot(dx, dx), dot(dy, dy)));
	}

	float GetNormalMapShadowQuality(float viewDepth)
	{
		return viewDepth < ParallaxCheapDistance ? ParallaxNearShadowQuality : ParallaxFarShadowQuality;
	}

	float GetNormalMapShadowStrength(float viewDepth)
	{
		if (!SharedData::extendedMaterialSettings.EnableNormalMapShadows)
			return 0.0;
		return saturate(4.0 - 4.0 * viewDepth / NormalMapShadowMaxDistance);
	}

#		if defined(LANDSCAPE)
	float3 SampleTerrainShadowNormal(float2 coords, float mipLevel, float4 w1, float2 w2, StochasticOffsets sharedOffset)
	{
		float4 n = 0.0;
		[branch] if (w1.x > 0.01) n += w1.x * float4(TerrainParallaxTexSample(TexNormalSampler, coords, mipLevel, sharedOffset).xyz, 1.0);
		[branch] if (w1.y > 0.01) n += w1.y * float4(TerrainParallaxTexSample(TexLandNormal2Sampler, coords, mipLevel, sharedOffset).xyz, 1.0);
		[branch] if (w1.z > 0.01) n += w1.z * float4(TerrainParallaxTexSample(TexLandNormal3Sampler, coords, mipLevel, sharedOffset).xyz, 1.0);
		[branch] if (w1.w > 0.01) n += w1.w * float4(TerrainParallaxTexSample(TexLandNormal4Sampler, coords, mipLevel, sharedOffset).xyz, 1.0);
		[branch] if (w2.x > 0.01) n += w2.x * float4(TerrainParallaxTexSample(TexLandNormal5Sampler, coords, mipLevel, sharedOffset).xyz, 1.0);
		[branch] if (w2.y > 0.01) n += w2.y * float4(TerrainParallaxTexSample(TexLandNormal6Sampler, coords, mipLevel, sharedOffset).xyz, 1.0);
#			if defined(LANDSCAPE_SEAMS)
		[branch] if (LandscapeSeams::ExtraWeights.x > 0.01) n += LandscapeSeams::ExtraWeights.x * float4(TerrainParallaxTexSample(LandscapeSeams::Normal0, coords, mipLevel, sharedOffset).xyz, 1.0);
		[branch] if (LandscapeSeams::ExtraWeights.y > 0.01) n += LandscapeSeams::ExtraWeights.y * float4(TerrainParallaxTexSample(LandscapeSeams::Normal1, coords, mipLevel, sharedOffset).xyz, 1.0);
		[branch] if (LandscapeSeams::ExtraWeights.z > 0.01) n += LandscapeSeams::ExtraWeights.z * float4(TerrainParallaxTexSample(LandscapeSeams::Normal2, coords, mipLevel, sharedOffset).xyz, 1.0);
		[branch] if (LandscapeSeams::ExtraWeights.w > 0.01) n += LandscapeSeams::ExtraWeights.w * float4(TerrainParallaxTexSample(LandscapeSeams::Normal3, coords, mipLevel, sharedOffset).xyz, 1.0);
#			endif
		return n.xyz * 2.0 - n.w;
	}

	float GetNormalMapShadowMultiplier(float2 coords, float footprint, float3 L, float quality, float strength, float noise, float4 w1, float2 w2, StochasticOffsets sharedOffset)
#		else
	float GetNormalMapShadowMultiplier(float2 coords, float footprint, float3 L, float quality, float strength, float noise, Texture2D<float4> tex, SamplerState texSampler, bool applyMeshTV, StochasticOffsets meshOffset)
#		endif
	{
		const float heightScale = SharedData::extendedMaterialSettings.NormalMapShadowHeightScale;
		float invLenXY = rcp(max(length(L.xy), 1e-5));
		float tanElevation = max(L.z, 0.0) * invLenXY;
		float maxRise = heightScale * rcp(NormalMapShadowMinNormalZ) - tanElevation;
		float visibility = 1.0;

		[branch] if (strength > 0.0 && maxRise > 0.0)
		{
			float2 dir = L.xy * invLenXY;
			float traceLength = SharedData::extendedMaterialSettings.NormalMapShadowLength;
			float hardness = SharedData::extendedMaterialSettings.NormalMapShadowHardness;
			uint numSteps = clamp((uint)(NormalMapShadowBaseSteps * quality * SharedData::extendedMaterialSettings.ParallaxQuality + 0.5), 4u, 32u);
			float invSteps = rcp((float)numSteps);

			float2 texDims;
#		if defined(LANDSCAPE)
			TexNormalSampler.GetDimensions(texDims.x, texDims.y);
#		else
			tex.GetDimensions(texDims.x, texDims.y);
#		endif
			float texDim = max(texDims.x, texDims.y);

			float height = 0.0;
			float occlusion = 0.0;
			float prevEnd = 0.0;
			[loop] for (uint i = 1; i <= numSteps; i++)
			{
				float t = (float)i * invSteps;
				float end = traceLength * t * t;
				float stepLength = end - prevEnd;
				float2 sampleCoords = coords + dir * (prevEnd + stepLength * noise);
				float mipLevel = log2(max(max(footprint, stepLength) * texDim, 1.0));

				float3 n = float3(0.0, 0.0, 1.0);
#		if defined(LANDSCAPE)
				n = SampleTerrainShadowNormal(sampleCoords, mipLevel, w1, w2, sharedOffset);
#		else
#			if defined(DO_ALPHA_TEST)
				if (TexColorSampler.SampleLevel(texSampler, sampleCoords, mipLevel).w < AlphaTestRefRS)
					break;
#			endif
#			if defined(TERRAIN_VARIATION)
				[branch] if (applyMeshTV)
				{
					n = StochasticEffectParallax(tex, texSampler, sampleCoords, mipLevel, meshOffset).xyz * 2.0 - 1.0;
				}
				else
#			endif
				{
					n = tex.SampleLevel(texSampler, sampleCoords, mipLevel).xyz * 2.0 - 1.0;
				}
#		endif

				float slope = -dot(n.xy, dir) * rcp(max(n.z, NormalMapShadowMinNormalZ));
				height += stepLength * (heightScale * slope - tanElevation);
				occlusion = max(occlusion, height * rcp(end) * (1.0 - t * t));

				if (occlusion * hardness >= 1.0 || height + maxRise * (traceLength - end) <= 0.0)
					break;
				prevEnd = end;
			}

			visibility = 1.0 - saturate(occlusion * hardness) * strength;
		}

		return visibility;
	}
#	endif
}

#endif  // EXTENDED_MATERIALS_HLSLI
