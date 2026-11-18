#include "Common/ColorManagement.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/PreExposure.hlsli"
#include "Common/SharedData.hlsli"
#include "Rainbows/Common.hlsli"

#if defined(TERRAIN_BLENDING)
Texture2D<float> SceneDepth : register(t0);
#else
Texture2D<SCENE_DEPTH_FORMAT> SceneDepth : register(t0);
#endif
// Bow radiance by angle from the antisolar point, 0 to 64 degrees, peak luminance 1.
Texture2D<float4> BowLUT : register(t1);
SamplerState LinearSampler : register(s0);

#if defined(EXP_HEIGHT_FOG)
#	define SampColorSampler LinearSampler
#	include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#endif

#if defined(PHYSICAL_SKY)
#	include "PhysicalSky/Common.hlsli"
#endif

#if defined(PHYSICAL_SKY) || defined(EXP_HEIGHT_FOG)
#	include "Common/ViewMedium.hlsli"
#endif

cbuffer RainbowFrame : register(b0)
{
	float4 FogParams;
	float2 RenderSize;
	float Intensity;
	float FogAbsorption;
	float MoonIntensity;
	float AngleToLut;
	float RainHeight;
	float RainExtent;
	float RainDensity;
	float RainStart;
	float2 Padding;
};

// Moonlight reads nearly white to night-adapted eyes.
static const float NightDesaturation = 0.85;
// At night the light must come from a moon (SkySync points it at the brightest lit one, else the zenith).
static const float MoonAlignedLow = 0.990;    // cos 8 degrees
static const float MoonAlignedHigh = 0.9986;  // cos 3 degrees

float GetFogFactor(float distance)
{
	float fog = 0.0;
	if (FogParams.y > 0.0) {
		float distanceFactor = saturate(distance * FogParams.y - FogParams.x);
		fog = distanceFactor > 0.0 ? min(FogParams.w, exp2(FogParams.z * log2(distanceFactor))) : 0.0;
	}
	return fog;
}

float4 main(Rainbows::Interpolants input) :
	SV_Target
{
	float3 direction = normalize(input.ViewPoint.xyz / input.ViewPoint.w - input.Eye);

	float3 toLight = normalize(SharedData::DirLightDirection.xyz);
	float angle = acos(clamp(dot(direction, -toLight), -1.0, 1.0));
	float lutCoord = angle * AngleToLut;
	if (lutCoord >= 1.0)
		discard;

	float night = saturate((0.03 - SharedData::SunDirection.z) / 0.08);
	float moonAligned = max(dot(toLight, SharedData::MasserDirection.xyz), dot(toLight, SharedData::SecundaDirection.xyz));
	float moonbow = MoonIntensity * smoothstep(MoonAlignedLow, MoonAlignedHigh, moonAligned);
	float lightUp = smoothstep(-0.02, 0.04, toLight.z) * lerp(1.0, moonbow, night);
	if (lightUp <= 0.0)
		discard;

	// Distance along this ray to the scene: clip w is view depth, w = dot(row 3, P) for P = eye + t * direction.
	float viewDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0)));
	float4 depthRow = FrameBuffer::CameraViewProj[3];
	float distance = (viewDepth - dot(depthRow.xyz, input.Eye) - depthRow.w) / max(dot(depthRow.xyz, direction), 1e-4);

	// Rain fills the air below the cloud base out to the far side of the shower; the bow is the light its drops
	// send back, so nearby objects with little rain in front of them hide it.
	float rainPath = RainExtent;
	if (direction.z > 1e-4)
		rainPath = min(rainPath, RainHeight / direction.z);
	float path = max(min(distance, rainPath) - RainStart, 0.0);
	float curtain = 1.0 - exp(-path * RainDensity);
	float scatterDistance = RainStart + path * 0.5;

	// The light reaches us from the rain about halfway in, through whatever haze lies in front of it.
	float3 transmittance = 1.0 - FogAbsorption * GetFogFactor(scatterDistance);
#if defined(PHYSICAL_SKY) || defined(EXP_HEIGHT_FOG)
	if (SharedData::PostWaterComposite) {
		float2 uv = input.Position.xy / RenderSize;
		float3 medium = ViewMedium::SampleViewMedium(direction * max(scatterDistance, 1.0), uv, LinearSampler).transmittance;
		transmittance = lerp(1.0, medium, FogAbsorption);
	}
#endif

	float amount = Intensity * curtain * lightUp;
	if (amount <= 0.0)
		discard;

	float3 bow = BowLUT.SampleLevel(LinearSampler, float2(lutCoord, 0.5), 0).rgb;
	bow = lerp(bow, Color::RGBToLuminance(bow), night * NightDesaturation);

	// The LUT is linear light, so the light's color must be too (a no-op under Linear Lighting).
	float3 light = ColorManagement::SceneToLinear(Color::DirectionalLight(SharedData::DirLightColor.xyz)) * Color::BRDFScale;
	return float4(PreExposure::Encode(bow * light * transmittance * amount), 0.0);
}
