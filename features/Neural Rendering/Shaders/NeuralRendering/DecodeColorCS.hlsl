#include "Common/NeuralRenderingCategories.hlsli"
#include "NeuralRendering/ColorTransfer.hlsli"
#include "NeuralRendering/TransferParams.hlsli"

Texture2D<float4> ModelColor : register(t0);                   // Feature 18 answer, display-referred proxy domain.
Texture2D<float4> OriginalColor : register(t1);                // Untouched linear scene colour, jittered raster.
Texture2D<float4> ProxyColor : register(t2);                   // The exact proxy EncodeColorCS handed the model.
Texture2D<float> GuideDepth : register(t3);                    // Game depth at the guide resolution.
Texture2D<float> MaterialCategories : register(t4);            // Masks2: category in the low three R16_UNORM bits.
Texture2D<float2> VanillaAdaptation : register(t5);            // Same inputs EncodeColorCS used for the display transform,
StructuredBuffer<float> PostProcessAdaptation : register(t6);  // so a stale proxy can be compared with a fresh encode.
Texture2D<float2> MotionVectors : register(t7);                // Game motion vectors at the guide resolution (current -> previous, normalised UV).
Texture2D<float2> ToneLow : register(t8);                      // y: the edge-aware blur of the edit (FilterToneDataCS); x unused here.
RWTexture2D<float4> DestinationColor : register(u0);
// Debug statistics: clamp count, sample count, and asuint peak luminance. Sampled on an 8x8 grid when
// enabled.
RWStructuredBuffer<uint> DebugStats : register(u1);
SamplerState LinearClampSampler : register(s0);

/**
 * Visualize one luminance band: gray is unchanged; black/white are +/- two stops. Undo proxy exposure
 * for scene-domain output.
 */
float4 NeuralBandDebugColor(float band, float exposure, uint domain, float alpha)
{
	float grey = saturate(0.5 + band * 0.25);
	if (domain != kNeuralColorDomainDisplayGamma)
		grey /= max(exposure, 1e-4);
	return float4(grey.xxx, alpha);
}

groupshared uint gModelFrameValid;

/**
 * Detect an empty model frame using 64 distributed probes per group. Every thread must call before any
 * early return because this uses group barriers.
 */
bool NeuralModelFrameValid(uint groupIndex, uint modelSpace)
{
	if (groupIndex == 0)
		gModelFrameValid = 0;
	GroupMemoryBarrierWithGroupSync();
	uint modelWidth;
	uint modelHeight;
	ModelColor.GetDimensions(modelWidth, modelHeight);
	uint2 probe = uint2(groupIndex & 7u, groupIndex >> 3);
	uint2 texel = min(uint2((float2(probe) + 0.5) * 0.125 * float2(modelWidth, modelHeight)),
		max(uint2(modelWidth, modelHeight), 1u) - 1u);
	float3 answer = NeuralModelToLinear(ModelColor.Load(int3(texel, 0)).rgb, modelSpace);
	if (dot(answer, kNeuralLuma) > 1e-5)
		InterlockedOr(gModelFrameValid, 1u);
	GroupMemoryBarrierWithGroupSync();
	return gModelFrameValid != 0;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
	const bool modelFrameValid = NeuralModelFrameValid(groupIndex, NeuralTransferModelSpace());

	uint width;
	uint height;
	uint originalWidth;
	uint originalHeight;
	DestinationColor.GetDimensions(width, height);
	OriginalColor.GetDimensions(originalWidth, originalHeight);
	uint2 active = min(ActiveSize, min(uint2(width, height), uint2(originalWidth, originalHeight)));
	if (any(dispatchThreadID.xy >= active) || any(ActiveSize == 0))
		return;

	const uint modelSpace = NeuralTransferModelSpace();

	// Split screen: untouched input on the left, enhanced output on the right, with a two-pixel divider in
	// display-white units.
	if (WipePosition >= 0.0) {
		float offset = float(dispatchThreadID.x) + 0.5 - WipePosition * float(active.x);
		float4 passthrough = OriginalColor[dispatchThreadID.xy];
		if (abs(offset) < 1.0) {
			float white = 1.0;
			if (ColorDomain != kNeuralColorDomainDisplayGamma) {
				NeuralDisplayTransform display = MakeNeuralDisplayTransform(DisplayParam, DisplayCinematic, DisplayTint, DisplayExposure,
					VanillaAdaptation.SampleLevel(LinearClampSampler, float2(0.5, 0.5), 0), PostProcessAdaptation[0], HighlightWhite,
					ProxyCurve);
				white = 1.0 / max(display.exposure, 1e-4);
			}
			DestinationColor[dispatchThreadID.xy] = float4((offset < 0.0 ? 0.0 : white).xxx, passthrough.a);
			return;
		}
		if (offset < 0.0) {
			DestinationColor[dispatchThreadID.xy] = passthrough;
			return;
		}
	}

	// Show nearest material category IDs, not filtered strengths. Missing guide extents default to
	// EverythingElse.
	if (DebugCategoryView != 0) {
		uint category = NeuralRenderingCategories::EverythingElse;
		if (all(GuideSize > 0)) {
			float2 guideCoord = NeuralGuidePosition(dispatchThreadID.xy, GuideSize, ActiveSize, GuideJitterOffset) - 0.5;
			int2 guideTexel = clamp((int2)round(guideCoord), int2(0, 0), int2(GuideSize) - 1);
			category = NeuralRenderingCategories::Unpack(MaterialCategories.Load(int3(guideTexel, 0)));
			category = category < 7 ? category : NeuralRenderingCategories::EverythingElse;
		}
		DestinationColor[dispatchThreadID.xy] = float4(NeuralRenderingCategories::DebugColor(category), 1.0);
		return;
	}

	// Map the source pixel to the unjittered model grid; sample proxy and answer at the same position.
	float2 uv = (float2(dispatchThreadID.xy) + 0.5 - JitterOffset) / float2(ActiveSize);
	// Reproject stale answers with current-to-previous normalized UV motion. Out-of-frame positions retain
	// the clean input.
	float2 answerUV = uv;
	bool answerOnScreen = true;
	if (StaleAnswer != 0 && all(GuideSize > 0)) {
		float2 motionCoord = NeuralGuidePosition(dispatchThreadID.xy, GuideSize, ActiveSize, GuideJitterOffset) - 0.5;
		int2 motionTexel = clamp((int2)round(motionCoord), int2(0, 0), int2(GuideSize) - 1);
		answerUV = uv + MotionVectors.Load(int3(motionTexel, 0));
		answerOnScreen = all(answerUV >= 0.0) && all(answerUV <= 1.0);
	}
	float4 model = ModelColor.SampleLevel(LinearClampSampler, answerUV, 0);
	float4 proxy = ProxyColor.SampleLevel(LinearClampSampler, answerUV, 0);

	float4 original = OriginalColor[dispatchThreadID.xy];

	// Display-gamma diagnostic: write raw model output with renderer alpha, bypassing the resolve.
	if (RawModelOutput != 0 && ColorDomain == kNeuralColorDomainDisplayGamma) {
		float3 rawLinear = NeuralModelToLinear(model.rgb, modelSpace);
		DestinationColor[dispatchThreadID.xy] = float4(NeuralLinearToDomain(rawLinear, ColorDomain), original.a);
		return;
	}

	float categoryColorStrength = 1.0;
	float categoryTransferStrength = 1.0;
	float categoryBroadLuminosity = 1.0;
	float categoryDetailLuminosity = 1.0;
	// Conservative fallback when no guide is available to classify this pixel:
	// guard everywhere rather than silently going unguarded.
	float categoryHueGuardAmount = 1.0;
	if (all(GuideSize > 0)) {
		// Tent-filter resolved category strengths and guard amounts across 3x3 guide texels to reduce jitter
		// at thin boundaries. Never interpolate category IDs. NeuralGuidePosition already accounts for guide
		// jitter.
		float2 guideCoord = NeuralGuidePosition(dispatchThreadID.xy, GuideSize, ActiveSize, GuideJitterOffset) - 0.5;
		int2 guideCenter = (int2)round(guideCoord);
		int2 guideMax = int2(GuideSize) - 1;

		categoryColorStrength = 0.0;
		categoryTransferStrength = 0.0;
		categoryBroadLuminosity = 0.0;
		categoryDetailLuminosity = 0.0;
		categoryHueGuardAmount = 0.0;
		float totalTapWeight = 0.0;
		[unroll]
		for (int dy = -1; dy <= 1; ++dy) {
			[unroll]
			for (int dx = -1; dx <= 1; ++dx) {
				int2 tapTexel = clamp(guideCenter + int2(dx, dy), int2(0, 0), guideMax);
				float2 tapOffset = guideCoord - float2(tapTexel);
				float tapWeight = max(0.0, 1.5 - abs(tapOffset.x)) * max(0.0, 1.5 - abs(tapOffset.y));
				if (tapWeight <= 0.0)
					continue;
				uint tapCategory = NeuralRenderingCategories::Unpack(MaterialCategories.Load(int3(tapTexel, 0)));
				tapCategory = tapCategory < 7 ? tapCategory : NeuralRenderingCategories::EverythingElse;
				categoryColorStrength += tapWeight * CategoryColorStrengths[tapCategory >> 2][tapCategory & 3];
				categoryTransferStrength += tapWeight * CategoryTransferStrengths[tapCategory >> 2][tapCategory & 3];
				categoryBroadLuminosity += tapWeight * CategoryBroadLuminosity[tapCategory >> 2][tapCategory & 3];
				categoryDetailLuminosity += tapWeight * CategoryDetailLuminosity[tapCategory >> 2][tapCategory & 3];
				categoryHueGuardAmount += tapWeight * float((HueGuardMask >> tapCategory) & 1u);
				totalTapWeight += tapWeight;
			}
		}
		categoryColorStrength /= max(totalTapWeight, 1e-5);
		categoryTransferStrength /= max(totalTapWeight, 1e-5);
		categoryBroadLuminosity /= max(totalTapWeight, 1e-5);
		categoryDetailLuminosity /= max(totalTapWeight, 1e-5);
		categoryHueGuardAmount /= max(totalTapWeight, 1e-5);
	}

	// Category controls shape the local result first. The existing global sliders
	// remain a final multiplier over every category.
	float editWeight = categoryTransferStrength * TransferStrength;
	// Fade stale edits where reprojected content differs. Match proxy exposure for comparison; undo it for
	// scene-domain debug views.
	float displayExposure = 1.0;
	if (StaleAnswer != 0 ||
		(DebugFlags & (kNeuralDebugBroadBand | kNeuralDebugDetailBand | kNeuralDebugGuardClamp)) != 0) {
		NeuralDisplayTransform display = MakeNeuralDisplayTransform(DisplayParam, DisplayCinematic, DisplayTint, DisplayExposure,
			VanillaAdaptation.SampleLevel(LinearClampSampler, float2(0.5, 0.5), 0), PostProcessAdaptation[0], HighlightWhite,
			ProxyCurve);
		displayExposure = display.exposure;
		if (StaleAnswer != 0)
			editWeight *= answerOnScreen ? NeuralStaleEditWeight(proxy, original, ColorDomain, modelSpace, display) : 0.0;
	}
	if (DepthAwareResolve != 0 && all(GuideSize > 0)) {
		// Left fractional (not rounded to a texel) so NeuralSilhouetteWeight can
		// bilinearly blend across the guide/active resolution mismatch instead of
		// aliasing on thin silhouettes.
		float2 guideTexel = NeuralGuidePosition(dispatchThreadID.xy, GuideSize, ActiveSize, GuideJitterOffset);
		editWeight *= NeuralSilhouetteWeight(GuideDepth, LinearClampSampler, guideTexel, GuideSize);
	}

	// The band split is sampled at the same (possibly reprojected) model position as the
	// answer it belongs to, so a reused answer carries its own band data with it.
	NeuralResolveInputs resolveInputs;
	resolveInputs.modelColor = model;
	resolveInputs.proxyColor = proxy;
	resolveInputs.originalColor = original;
	resolveInputs.colorStrength = categoryColorStrength * ColorStrength;
	resolveInputs.editWeight = editWeight;
	resolveInputs.broadLuminosity = categoryBroadLuminosity * BroadLuminosity;
	resolveInputs.detailLuminosity = categoryDetailLuminosity * BandParams.x;
	resolveInputs.hasToneData = NeuralTransferHasToneData();
	resolveInputs.toneLow = resolveInputs.hasToneData ? ToneLow.SampleLevel(LinearClampSampler, answerUV, 0).y : 0.0;
	resolveInputs.domain = ColorDomain;
	resolveInputs.modelSpace = modelSpace;
	resolveInputs.hueGuardAmount = categoryHueGuardAmount;
	resolveInputs.maxRatio = MaxRatio;
	resolveInputs.modelFrameValid = modelFrameValid;

	NeuralResolveDebug resolveDebug;
	float4 result = ResolveNeuralColor(resolveInputs, resolveDebug);

	// Sample debug statistics once per 8x8 block to limit atomic contention.
	if ((DebugFlags & kNeuralDebugStats) != 0 && (dispatchThreadID.x & 7u) == 0u && (dispatchThreadID.y & 7u) == 0u) {
		uint previous;
		InterlockedAdd(DebugStats[1], 1u, previous);
		if (resolveDebug.clamped != 0)
			InterlockedAdd(DebugStats[0], 1u, previous);
		// asuint is monotonic over non-negative floats, so a max on the bit pattern is a max
		// on the value; the resolve never produces a negative luminance.
		InterlockedMax(DebugStats[2], asuint(max(resolveDebug.modelLuma, 0.0)));
	}

	if ((DebugFlags & kNeuralDebugBroadBand) != 0) {
		DestinationColor[dispatchThreadID.xy] = NeuralBandDebugColor(resolveDebug.lowBand, displayExposure, ColorDomain, original.a);
		return;
	}
	if ((DebugFlags & kNeuralDebugDetailBand) != 0) {
		DestinationColor[dispatchThreadID.xy] = NeuralBandDebugColor(resolveDebug.highBand, displayExposure, ColorDomain, original.a);
		return;
	}
	// "Show Guard Clamping": tint the pixels the guard caught, leaving the rest of the frame
	// readable underneath so it is obvious *what* is being clamped.
	if ((DebugFlags & kNeuralDebugGuardClamp) != 0 && resolveDebug.clamped != 0) {
		float3 marker = resolveDebug.clamped > 0 ? float3(1.0, 0.0, 0.0) : float3(0.0, 0.3, 1.0);
		if (ColorDomain != kNeuralColorDomainDisplayGamma)
			marker /= max(displayExposure, 1e-4);
		result.rgb = lerp(result.rgb, marker, 0.6);
	}

	DestinationColor[dispatchThreadID.xy] = result;
}
