#ifndef __SHARED_DATA_DEPENDENCY_HLSL__
#define __SHARED_DATA_DEPENDENCY_HLSL__

#ifdef REVERSE_Z
#	define SCENE_DEPTH_FORMAT float
#else
#	define SCENE_DEPTH_FORMAT unorm float
#endif

#include "Common/FrameBuffer.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

namespace SharedData
{
	static const uint TonemapOwnerVanilla = 0;
	static const uint TonemapOwnerPostProcessing = 1;
	static const uint TonemapOwnerEffects11 = 2;
	static const uint TonemapOwnerPostProcessingFallback = 3;

	cbuffer SharedData : register(b5)
	{
		float4 WaterData[25];
		float4 DirLightDirection;
		float4 DirLightColor;
		float4 SunDirection;
		float4 SunColor;
		float4 MasserDirection;
		float4 MasserColor;
		float4 SecundaDirection;
		float4 SecundaColor;
		float4 CameraData;
		float4 BufferDim;
		float Timer;
		uint FrameCount;
		uint FrameCountAlwaysActive;
		bool InInterior;  // If the current cell is an interior
		bool HasDirectionalShadows;
		bool InMapMenu;           // If the world/local map is open (note that the renderer is still deferred here)
		bool HideSky;             // HideSky flag in WorldSpace, e.g. Blackreach
		float MipBias;            // Offset to mip level for TAA sharpness
		float WaterSystemHeight;  // TES::GetWaterHeight in camera-relative Z; -FLT_MAX when no water body found
		uint PostWaterComposite;
		uint ResetHistory;
		uint TonemapOwner;
		float4 AmbientSHR;
		float4 AmbientSHG;
		float4 AmbientSHB;
		float4 HDRData;
		float PreExposure;
		float RcpPreExposure;
		float PreviousPreExposure;
		float PreExposureHistoryRatio;
	};

	struct GrassLightingSettings
	{
		float Glossiness;
		float SpecularStrength;
		float SubsurfaceScatteringAmount;
		bool OverrideComplexGrassSettings;

		float BasicGrassBrightness;
		float ComplexGrassThreshold;
		// Only read by the GRASS_OPTIMIZATIONS permutation, for grass drawn with an LOD mesh.
		float MidLODBrightness;
		float FarLODBrightness;
		float PBRWrappedLightingAmount;
		float SubsurfaceBalance;
		float CanopyOcclusion;
		float ClumpShading;
	};

	struct CPMSettings
	{
		bool EnableComplexMaterial;
		bool EnableParallax;
		bool EnableTerrainParallax;
		bool EnableHeightBlending;
		bool EnableShadows;
		bool EnableParallaxWarpingFix;
		float ParallaxQuality;
		bool EnableNormalMapShadows;

		float NormalMapShadowHeightScale;
		float NormalMapShadowLength;
		float NormalMapShadowHardness;
		uint HeightMapShadowMode;
	};

	struct CubemapCreatorSettings
	{
		uint Enabled;
		float3 pad0;

		float4 CubemapColor;
	};

	struct TerraOccSettings
	{
		bool EnableTerrainShadow;
		float3 Scale;
		float2 ZRange;
		float2 Offset;
		float ZBlur;
		float3 pad0;
	};

	struct LightLimitFixSettings
	{
		uint EnableLightsVisualisation;
		uint LightsVisualisationMode;
		float2 pad0;
		uint4 ClusterSize;
		uint EnableContactShadows;
		uint ContactShadowMaxSteps;
		float ContactShadowMaxDistance;
		float ContactShadowLength;
		float ContactShadowDepthThickness;
		float ContactShadowStrength;
		uint EnableLocalShadows;
		uint LocalShadowSamples;
		float LocalShadowFilterRadius;
		float LocalShadowTexelSize;
		uint ContactShadowSubSamples;
		float pad1;
	};

	struct WetnessEffectsSettings
	{
		row_major float4x4 OcclusionViewProj;

		float Time;
		float Raining;
		float Wetness;
		float PuddleWetness;

		bool EnableWetnessEffects;
		float MaxRainWetness;
		float MaxPuddleWetness;
		float MaxShoreWetness;

		uint ShoreRange;
		float PuddleRadius;
		float PuddleMaxAngle;
		float PuddleMinWetness;

		float MinRainWetness;
		float SkinWetness;
		float WeatherTransitionSpeed;
		bool EnableRaindropFx;

		bool EnableSplashes;
		bool EnableRipples;
		uint EnableVanillaRipples;
		float RaindropFxRange;

		float RaindropGridSizeRcp;
		float RaindropIntervalRcp;
		float RaindropChance;
		float SplashesLifetime;

		float SplashesStrength;
		float SplashesMinRadius;
		float SplashesMaxRadius;
		float RippleStrength;

		float RippleRadius;
		float RippleBreadth;
		float RippleLifetimeRcp;
		float TopDownVisibility;

		float WallRainDensity;
		float WallRainDropSize;
		float WallRainSpeed;
		float WallRainLayering;

		float WallRainNormalStrength;
		float WallRainWetness;
		float WallRainDarkening;
		bool EnableWallRain;

		float WallRainTrailLength;
		float3 WallRainPad;
	};

	struct SkylightingSettings
	{
		row_major float4x4 OcclusionViewProj;
		row_major float4x4 ZenithViewProj;
		float4 OcclusionSHBasis4Pi;  // SH basis of the occlusion direction, times 4 pi (Monte Carlo weight)

		float4 PosOffset;   // xyz: cell origin in camera model space
		uint4 ArrayOrigin;  // xyz: array origin
		int4 ValidMargin;

		float3 ZenithOffset;  // eye movement since the zenith capture
		uint ZenithValid;

		float MinDiffuseVisibility;
		float MinSpecularVisibility;
		uint AccumulateOcclusion;  // 0 on zenith frames, whose capture is not a sky visibility sample
		uint pad0;
	};

	struct CloudShadowsSettings
	{
		float Opacity;
		float Scattering;
		float2 pad0;
	};

	struct LODBlendingSettings
	{
		float LODTerrainBrightness;
		float LODObjectBrightness;
		float LODObjectSnowBrightness;
		bool DisableTerrainVertexColors;
		float LODTerrainGamma;
		float LODObjectGamma;
		float LODObjectSnowGamma;
		float pad0;
	};

	struct HairSpecularSettings
	{
		uint Enabled;
		float HairGlossiness;
		float SpecularMult;
		float DiffuseMult;
		uint EnableTangentShift;
		float PrimaryTangentShift;
		float SecondaryTangentShift;
		float HairSaturation;
		float SpecularIndirectMult;
		float DiffuseIndirectMult;
		float BaseColorMult;
		float Transmission;
		uint EnableSelfShadow;
		float SelfShadowStrength;
		float SelfShadowExponent;
		float SelfShadowScale;
		uint HairMode;  // 0: Kajiya-Kay, 1: Marschner
		uint3 pad;
	};

	/** @brief Terrain Variation feature settings. */
	struct TerrainVariationSettings
	{
		uint enableLODTerrainTilingFix;  ///< 1 = apply variation to LOD terrain.
		uint enableMeshSupport;          ///< 1 = apply variation to landscape-textured meshes.
		uint2 pad;
	};

	struct IBLSettings
	{
		uint EnableIBL;
		uint PreserveFogLuminance;
		uint UseStaticIBL;
		float DALCAmount;
		float EnvIBLScale;
		float SkyIBLScale;
		float EnvIBLSaturation;
		float SkyIBLSaturation;
		float FogAmount;
		uint DALCMode;       // 0: Luminance Ratio, 1: Color Ratio, 2: DALC + Sky
		uint ENBSkyFalloff;  // Effects 11 owns IBL: darken sky irradiance from below
		float SkySpecularIBLScale;
		uint SkylightingAffectsEnv;
		uint EnableReflectionFallback;
		float ReflectionFallbackDistance;
		float pad0;
	};

	struct ExtendedTranslucencySettings
	{
		uint MaterialModel;  // [0,1,2,3] The MaterialModel
		float Reduction;     // [0, 1.0] The factor to reduce the transparency to matain the average transparency [0,1]
		float Softness;      // [0, 2.0] The soft remap upper limit [0,2]
		float Strength;      // [0, 1.0] The inverse blend weight of the effect
	};

	struct LinearLightingSettings
	{
		uint isMainOrLoadingMenu;
		float diffuseGamma;
		float directionalLightMult;
		float pointLightMult;
		float ambientMult;
		float glowmapMult;
		float effectLightingMult;
		float membraneEffectMult;
		float bloodEffectMult;
		float projectedEffectMult;
		float deferredEffectMult;
		float otherEffectMult;
		float particleEffectMult;
		float lightSpriteEffectMult;
		float fireEffectMult;
		float fireEffectCurve;
		float diffuseCurve;
		float diffuseWhiteReflectance;
		float waterGamma;
		float pad;
	};

	struct ENBSettings
	{
		uint Enable;
		float ColorPow;
		float LightSpriteIntensity;
		float FireIntensity;

		float FireCurve;
		float PBRColorPow;
		float2 pad0;

		float CloudsCurve;
		float CloudsDesaturation;
		float CloudsEdgeIntensity;
		float CloudsEdgeMoonMultiplier;

		uint UseProceduralGradientWeights;
		float ProceduralGradientWeightCurve;
		float LightSpriteCurve;
		uint EnableParticle;

		float ParticleIntensity;
		float ParticleLightingInfluence;
		float ParticleAmbientInfluence;
		float ParticlePointLightingInfluence;

		uint EnableVolumetricRays;
		float VolumetricRaysIntensity;
		float VolumetricRaysDensity;
		float VolumetricRaysSkyColorAmount;

		float VolumetricRaysDesaturation;
		float3 VolumetricRaysColorFilter;

		uint EnableWater;
		float WaterWavesAmplitude;
		float WaterMuddiness;
		float WaterSunLightingMultiplier;

		float WaterSunSpecularMultiplier;
		float WaterFresnelMin;
		float WaterFresnelMax;
		float WaterFresnelMultiplier;

		float WaterReflectionAmount;
		float WaterPad0;
		float WaterPad1;
		float WaterPad2;

		uint EnableCloudsScattering;
		float SkyScatteringIntensity;
		float SkyScatteringShadowAmount;
		float SkyScatteringAmount;

		float3 SkyScatteringColor;
		float SkyScatteringDustDarkening;

		float3 SkyScatteringDustTint;
		float SkyScatteringDustVolume;

		float3 SkyScatteringSunDirection;
		float SkyScatteringSunVisibility;

		float SkyScatteringHorizonRange;
		float SkyScatteringAtmosphereThickness;
		float SkyScatteringAirGlowIntensity;
		float SkyScatteringAirGlowRange;

		float SkyScatteringSunGlowIntensity;
		float SkyScatteringSunGlowRange;
		float SkyScatteringMoonGlowAmount;
		float SkyScatteringMoonGlowRange;

		float SkyScatteringSunIntensity;
		float CloudsLightingSunIntensity;
		float CloudsLightingMoonIntensity;
		uint EnableCloudsLightingFromMoon;

		uint CalculateCloudsEdgeFromScattering;
		float CloudsLightingDesaturation;
		float CloudsLightingForwardScattering;
		float CloudsLightingDensity;

		float3 CloudsColorFilter;
		float CloudsIntensity;

		float CloudsVertexAlphaBoost;
		float CloudsEdgeClamp;
		float CloudsEdgeFadePower;
		float SunBillboardTan;

		float MasserBillboardTan;
		float SecundaBillboardTan;
		float SkyScaleMultiplier;
		float SkyScatteringPad1;

		float3 VolumetricFogColorFilter;
		float VolumetricFogIntensity;

		float VolumetricFogCurve;
		float VolumetricFogOpacity;
		float VolumetricFogShadowAmount;
		uint VolumetricFogEnableLighting;

		float3 VolumetricRaysSkyColor;
		uint VolumetricFogEnableShadows;

		float StarsCurve;
		float StarsIntensity;
		float MoonCurve;
		uint EnableAnimatedStars;

		float StarsAnimationTime;
		float StarsAnimationDensity;
		float StarsAnimationIntensity;
		uint FixBlackCrush;

		uint VolumetricRaysSampleCount;
		uint GameVolumetricRaysDisableDithering;
		float2 GameVolumetricRaysPad0;

		float3 GameVolumetricRaysSunColor;
		float GameVolumetricRaysCustomContribution;

		float3 GameVolumetricRaysCustomColor;
		float GameVolumetricRaysPad1;
	};
	struct TerrainBlendingSettings
	{
		uint Enabled;
		uint3 _padding;
	};

	struct SSGISettings
	{
		uint Enabled;
		uint EnableIL;
		uint ReplaceAmbient;
		float AOPower;
		float ILSaturation;
		float3 pad0;
	};

	struct SSRSettings
	{
		uint Enabled;
		float SpecularMult;
		float SpecCubemapMult;
		uint pad0;
	};

	struct PostProcessingSettings
	{
		uint EnableFire;
		float FireIntensity;
		float FireCurve;
		float LightSpriteIntensity;
		float LightSpriteCurve;
		float FireColorPreservation;
		float2 pad0;
	};

	struct ViewAtmosphereSettings
	{
		uint enabled;
		uint nearPixelSize;
		uint nearSlices;
		uint middlePixelSize;
		uint middleSlices;
		uint distantPixelSize;
		uint distantSlices;
		uint historyMissSamples;
		float nearRange;
		float localRange;
		float historyWeight;
		float sampleJitter;
		float shadowBias;
		float depthDistribution;
		float lookupJitter;
		float padding;
	};

	struct ExponentialHeightFogSettings
	{
		uint enabled;
		uint useDynamicCubemaps;
		float startDistance;
		float fogHeight;
		float fogHeightFalloff;
		float fogDensity;
		float fogHeight2;
		float fogHeightFalloff2;
		float fogDensity2;
		float directionalInscatteringMultiplier;
		float directionalInscatteringAnisotropy;
		uint useSkyIBL;
		float4 inscatteringTint;
		float cubemapMipLevel;
		float sunlightAttenuationAmount;
		uint respectVanillaFogFade;
		uint disableVanillaFog;
		float4 fogInscatteringColor;
		float originalFogColorAmount;
		uint volumetricFogEnabled;
		float volumetricFogStartDistance;
		float volumetricFogNearFadeInDistance;
		float4 volumetricFogAlbedo;
		float4 volumetricFogEmissive;
		float volumetricDirectionalScatteringIntensity;
		float volumetricSkyLightingIntensity;
		float volumetricFogScatteringDistribution;
		float volumetricFogExtinctionScale;
		float volumetricLocalLightScatteringIntensity;
		float volumetricFogNoiseScale;
		float volumetricFogNoiseThreshold;
		float pad3;
		float3 volumetricFogNoiseVelocity;
		float pad0;
		uint useVanillaFogSettings;
		float vanillaFogMaxOpacity;
		float vanillaFogDensity;
		float vanillaFogNear;
		float vanillaFogFar;
		float vanillaFogPower;
		float vanillaFogStrength;
		float pad4;
		float4 vanillaFogNearColor;
		float4 vanillaFogFarColor;
		float fogLightingInfluence;
		float distanceHazeMaxOpacity;
		float distanceHazeStartDistance;
		float distanceHazeFadeDistance;
		float4 originalFogColorOverride;
		float interiorFogStrength;
		float interiorFogBrightness;
		float2 pad5;
		float4 interiorFogTint;
	};

	struct TruePBRSettings
	{
		float VertexAOStrength;
		uint EnableMicroShadows;
		float MicroShadowStrength;
		uint EnableMultiScatter;
		uint EnableHorizonOcclusion;
		uint3 pad;
	};


	struct SkinData
	{
		float4 skinParams;
		float4 skinParams2;
		float4 skinDetailParams;
		float4 sssParams;
		float4 fuzzParams;
		float4 physicalParams;
		float4 wetParams;
	};

	struct VanillaFresnelSettings
	{
		uint Enable;
		uint EnableGGX;
		uint EnableDynamicCubemapsConversion;
		uint EnableEyeSpecialHandling;
		float RoughnessMultiplier;
		float SpecularRoughnessBlend;
		float BaseF0Multiplier;
		float MinF0;
		float CubemapToF0Multiplier;
		float ComplexMaterialF0Multiplier;
		float2 pad;
	};

	struct SnowCoverSettings
	{
		float Month;
		float TimeSnowing;
		float SnowingDensity;
		float SeasonalAltitude;

		uint EnableExpensiveFoliage;
		float SnowHeightOffset;
		uint2 pad;

		float RooftopSnow;
		float RooftopHeight;
		float RooftopExposure;
		uint pad1;

		uint EnableSnowCover;
		uint AffectGrassTint;
		uint AffectTreeTint;
		float FoliageHeightOffset;

		float UVScale;
		float peakMainAngle;
		float peakAltAngle;
		float minAngle;

		float maxAngle;
		float mainSpec;
		float altSpec;
		float mapZscale;

		float2 mapScale;
		float2 mapOffset;

		float4 Glint;
		float4 MainTint;
		float4 AltTint;

		float BlendSmoothness;
		float WeatherFadeStart;
		float WeatherFadeEnd;
		float ObjectFadeStart;

		float ObjectFadeEnd;
		float ObjectFadeAmount;
		uint2 pad2;

		uint FireMeltCount;
		float FireMeltStrength;
		float FireMeltRadiusScale;
		uint pad3;
		float4 FireMeltSpheres[8];
	};

	struct VolumetricLightingSettings
	{
		float GodRayGain;
		float GodRayExponent;
		float2 pad0;
	};

	struct HorizonFixSettings
	{
		float farWaterDistance;
		float3 pad;
	};

	struct HairBacklightingSettings
	{
		uint Enable;
		float Strength;
		float ScatterWidth;
		float EdgeFalloff;
		float InteriorGlow;
		float Absorption;
		float DarkBoost;
		float DarkThreshold;
		float HeadOcclusion;
		uint3 pad;
	};

	struct PhysSkyData
	{
		// DYNAMIC
		float2 texDim;
		float2 rcpTexDim;  //
		float2 frameDim;
		float2 rcpFrameDim;  //

		float zCameraPlanet;
		float3 sunDir;  //
		float3 sunlightColor;
		float trMix;  //
		float3 masserDir;
		float apLumMix;  //
		float3 masserColor;
		float apTrMix;  //
		float3 secundaDir;
		float sunDiskCos;  //
		float3 secundaColor;

		// GENERAL
		uint enabled;  //
		float pad;
		float vanillaMix;

		// WORLD
		float zBottom;
		float rPlanet;  //
		float rAtmosphere;
		float3 groundAlbedo;  //

		// ATMOSPHERE
		float2 cloudShadowRemapRange;

		float aerosolFalloff;
		float aerosolPhaseG;  //
		float3 aerosolScatter;
		uint halfResApShadow;  //
		float3 aerosolAbsorption;

		float rayleighFalloff;
		float3 rayleighScatter;  //

		float ozoneAltitude;  //
		float ozoneThickness;
		float3 ozoneAbsorption;  //

		// CLOUDS (VANILLA)
		uint enableVanillaClouds;
		float cloudRelightMix;
		float cloudOriginalMix;
		float silverLiningMix;  //
		float silverLiningSpread;

		// VOLUMETRIC CLOUDS
		uint enableVolumetricClouds;
		float shadowVolumeRange;
		float lowestCloudAltitude;  //
		float highestCloudAltitude;
		float3 volCloudScatter;  //
		uint volCloudUseSun;
		float3 volCloudAbsorption;  //
		float volCloudLowBottom;
		float volCloudLowThickness;

		// SETTINGS
		uint lightSkyStatics;
		float skyStaticsBrightness;  //
		float4 lightningPositionRadius;
		float4 lightningEndPosition;
		float4 lightningColorIntensity;
		float4 lightningControl;
		float4 lightningPhotometry;
		float4 lightningPlacement;
		float4 lightningChannelEmitter;
		float4 lightningPreparationPositionRadius;
		float4 lightningPreparationControl;  // base mode, history valid, active mask lane, preparing
		float4 diskAngularRadius;            // Sun, Masser, Secunda, directional light
		float3 dirLightBodyDir;
		uint dirLightDiskIndex;
		float4 dirLightDisc;
		float4 diskBillboardScale;
	};

	struct RainRefractionSettings
	{
		uint Enable;
		float MotionStretch;
		float MotionTransparency;
		uint ScreenRefraction;
		float RefractionFactor;
		float MotionBluriness;
		float Brightness;
		float Visibility;
		float4 TextureBounds;
		float2 TextureRadiusScale;
		float2 pad0;
	};

	struct ProceduralNightSkySettings
	{
		uint enabled;
		float starIntensity;
		float starTwinkle;
		float radianceLimit;

		float3 auroraLowColor;
		float auroraIntensity;

		float3 auroraHighColor;
		float auroraTime;

		float auroraLighting;
		float3 pad0;

		float3 shootingStarHead;
		float shootingStarIntensity;

		float3 shootingStarTail;
		float shootingStarWidth;
	};

	struct NightEyeSettings
	{
		float4 Eye;
		float4 Actor;
		float4 Enemy;
		float4 Ally;
		float4 Neutral;
	};

	cbuffer FeatureData : register(b6)
	{
		GrassLightingSettings grassLightingSettings;
		CPMSettings extendedMaterialSettings;
		CubemapCreatorSettings cubemapCreatorSettings;
		TerraOccSettings terraOccSettings;
		LightLimitFixSettings lightLimitFixSettings;
		WetnessEffectsSettings wetnessEffectsSettings;
		SkylightingSettings skylightingSettings;
		CloudShadowsSettings cloudShadowsSettings;
		LODBlendingSettings lodBlendingSettings;
		HairSpecularSettings hairSpecularSettings;
		TerrainVariationSettings terrainVariationSettings;
		IBLSettings iblSettings;
		ExtendedTranslucencySettings extendedTranslucencySettings;
		LinearLightingSettings linearLightingSettings;
		ENBSettings enbSettings;
		TerrainBlendingSettings terrainBlendingSettings;
		ExponentialHeightFogSettings exponentialHeightFogSettings;
		TruePBRSettings truePBRSettings;
		SkinData skinData;
		VanillaFresnelSettings vanillaFresnelSettings;
		SnowCoverSettings snowCoverSettings;
		VolumetricLightingSettings volumetricLightingSettings;
		HorizonFixSettings horizonFixSettings;
		HairBacklightingSettings hairBacklightingSettings;
		RainRefractionSettings rainRefractionSettings;
		NightEyeSettings nightEyeSettings;
		ProceduralNightSkySettings proceduralNightSkySettings;
		SSGISettings ssgiSettings;
		SSRSettings ssrSettings;
		PostProcessingSettings postProcessingSettings;
		PhysSkyData physSkyData;
		ViewAtmosphereSettings viewAtmosphereSettings;
	};

	Texture2D<float4> DepthTexture : register(t17);

	// Get a int3 to be used as texture sample coord. [0,1] in uv space
	int3 ConvertUVToSampleCoord(float2 uv)
	{
		uv = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(uv);
		return int3(uv * BufferDim.xy, 0);
	}

	// Get a raw depth from the depth buffer. [0,1] in uv space
	float GetDepth(float2 uv)
	{
		return DepthTexture.Load(ConvertUVToSampleCoord(uv)).x;
	}

	float GetScreenDepth(float depth)
	{
#ifdef REVERSE_Z
#	if defined(PSHADER) || defined(VSHADER)
		if (!FrameBuffer::IsReverseProjection())
			return (CameraData.w / (-depth * CameraData.z + CameraData.x));
#	endif
		return (CameraData.w / (depth * CameraData.z + CameraData.y));
#else
		return (CameraData.w / (-depth * CameraData.z + CameraData.x));
#endif
	}

	float4 GetScreenDepths(float4 depths)
	{
#ifdef REVERSE_Z
#	if defined(PSHADER) || defined(VSHADER)
		if (!FrameBuffer::IsReverseProjection())
			return (CameraData.w / (-depths * CameraData.z + CameraData.x));
#	endif
		return (CameraData.w / (depths * CameraData.z + CameraData.y));
#else
		return (CameraData.w / (-depths * CameraData.z + CameraData.x));
#endif
	}

	float GetScreenDepth(float2 uv)
	{
		float depth = GetDepth(uv);
		return GetScreenDepth(depth);
	}

	static const float WaterBlendWidth = 0.25;
	static const float WaterBlendRange = 128.0;

	float4 GetWaterTile(int2 cellInt)
	{
		float4 waterData = float4(1.0, 1.0, 1.0, -2147483648);
		[flatten] if (all(cellInt >= 0) && all(cellInt < 5))
			waterData = WaterData[cellInt.x + cellInt.y * 5];
		return waterData;
	}

	float4 BlendWaterData(int2 cellInt, float2 cellFrac, float z)
	{
		const int2 side = int2(cellFrac.x < 0.5 ? -1 : 1, cellFrac.y < 0.5 ? -1 : 1);
		const float2 edge = 0.5 - 0.5 * smoothstep(0.0, WaterBlendWidth, min(cellFrac, 1.0 - cellFrac));
		const float4 weights = float4((1.0 - edge.x) * (1.0 - edge.y), edge.x * (1.0 - edge.y), (1.0 - edge.x) * edge.y, edge.x * edge.y);

		const float4 own = GetWaterTile(cellInt);
		const float4 acrossX = GetWaterTile(cellInt + int2(side.x, 0));
		const float4 acrossY = GetWaterTile(cellInt + int2(0, side.y));
		const float4 acrossXY = GetWaterTile(cellInt + side);

		const float4 above = clamp(z - float4(own.w, acrossX.w, acrossY.w, acrossXY.w), -WaterBlendRange, WaterBlendRange);
		const float ownAbove = z - own.w;
		const float blendedAbove = dot(weights, above) + saturate(2.0 * weights.x - 1.0) * (ownAbove - above.x);
		const float3 color = own.xyz * weights.x + acrossX.xyz * weights.y + acrossY.xyz * weights.z + acrossXY.xyz * weights.w;
		return float4(color, z - blendedAbove);
	}

	// Returns water data for the tile containing worldPosition (camera-relative XY).
	float4 GetWaterData(float3 worldPosition)
	{
		// floor keeps negative coordinates and cell borders exact
		float2 cellPosition = (worldPosition.xy + FrameBuffer::CameraPosAdjust.xy) / 4096.0;
		float2 cellFloor = floor(cellPosition);
		float2 cellFrac = cellPosition - cellFloor;
		int2 cellInt = (int2)(cellFloor - floor(FrameBuffer::CameraPosAdjust.xy / 4096.0)) + 2;  // 5x5 cell grid

		return BlendWaterData(cellInt, cellFrac, worldPosition.z);
	}

	float3 GetAmbient(float3 normal)
	{
		return SphericalHarmonics::Unproject(AmbientSHR, AmbientSHG, AmbientSHB, normal);
	}
}
#endif  // __SHARED_DATA_DEPENDENCY_HLSL__
