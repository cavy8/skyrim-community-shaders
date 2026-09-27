#include "Common/NeuralRenderingCategories.hlsli"
#include "NeuralRendering/ColorTransfer.hlsli"

cbuffer TransferParams : register(b0)
{
	float2 JitterOffset;  // Sub-pixel projection offset of the original raster, in render pixels.
	float ColorStrength;
	float TransferStrength;  // Overall edit weight (0 = untouched frame, 1 = the model's change, 2 = doubled).
	uint2 ActiveSize;  // Valid region of OriginalColor and DestinationColor, in their texels.
	uint2 WorkSize;    // Model raster; ModelColor and ProxyColor are allocated at this size.
	uint2 GuideSize;   // Valid region of GuideDepth (render resolution), in its texels.
	uint DepthAwareResolve;  // Non-zero: fade the edit across depth silhouettes (see NeuralSilhouetteWeight).
	uint StaleAnswer;        // Non-zero: ModelColor/ProxyColor are the previous frame's; reproject them through MotionVectors.
	uint HueGuardMask;       // Bit i set: category i (NeuralRenderingCategories) hue-guards its chroma change.
	float2 GuideJitterOffset;  // Projection offset of the guide rasters relative to the colour raster, in guide texels.
	uint ColorDomain;          // kNeuralColorDomain* - how OriginalColor and DestinationColor are encoded.
	float4 CategoryColorStrengths[2];
	float4 CategoryTransferStrengths[2];
	float4 CategoryLuminosityStrengths[2];
	float4 DisplayParam;      // x: replicate the vanilla tonemap, y: ISHDR Param.y (white point), z: ISHDR Param.z (Hejl-Burgess-Dawson).
	float4 DisplayCinematic;  // ISHDR Cinematic: x saturation, z contrast, w brightness.
	float4 DisplayTint;       // ISHDR Tint: xyz colour, w amount.
	float4 DisplayExposure;   // x: apply Post Processing auto exposure, y: 0.18 * compensation, zw: adaptation range.
	float LuminosityStrength;  // Overall multiplier on the model's luminance change alone (see ResolveNeuralColor).
	uint DebugCategoryView;    // Non-zero: render the classified category (NeuralRenderingCategories::DebugColor) instead of the model's edit.
	float MaxRatio;            // Two-sided guard on the model/proxy luminance ratio (1/MaxRatio..MaxRatio); see ResolveNeuralColor.
	uint RawModelOutput;       // Non-zero: write Feature 18's answer directly, bypassing the resolve entirely (Finished Image diagnostic).
	float HighlightWhite;      // Display gamma: display peak for the HDR highlight shoulder (NeuralHighlightRolloff); 0 = none.
	float WipePosition;        // Split-screen comparison: split as a fraction of the active width; negative = off.
	uint2 Reserved;
};

Texture2D<float4> ModelColor : register(t0);     // Feature 18 answer, display-referred proxy domain.
Texture2D<float4> OriginalColor : register(t1);  // Untouched linear scene colour, jittered raster.
Texture2D<float4> ProxyColor : register(t2);     // The exact proxy EncodeColorCS handed the model.
Texture2D<float> GuideDepth : register(t3);      // Game depth at the guide resolution.
Texture2D<float> MaterialCategories : register(t4);  // Masks2: category in the low three R16_UNORM bits.
Texture2D<float2> VanillaAdaptation : register(t5);            // Same inputs EncodeColorCS used for the display transform,
StructuredBuffer<float> PostProcessAdaptation : register(t6);  // so a stale proxy can be compared with a fresh encode.
Texture2D<float2> MotionVectors : register(t7);  // Game motion vectors at the guide resolution (current -> previous, normalised UV).
RWTexture2D<float4> DestinationColor : register(u0);
SamplerState LinearClampSampler : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	uint width;
	uint height;
	uint originalWidth;
	uint originalHeight;
	DestinationColor.GetDimensions(width, height);
	OriginalColor.GetDimensions(originalWidth, originalHeight);
	uint2 active = min(ActiveSize, min(uint2(width, height), uint2(originalWidth, originalHeight)));
	if (any(dispatchThreadID.xy >= active) || any(ActiveSize == 0))
		return;

	// Split-screen comparison ("Compare: Split Screen", runtime only): left of the split the
	// frame passes through exactly as it arrived - no edit, no debug view - and a two-pixel
	// black/white divider marks the split so it reads on both bright and dark content. White
	// is display white: 1.0 in the display-gamma domain, and in scene linear the value the
	// display transform exposes to roughly mid-bright.
	if (WipePosition >= 0.0) {
		float offset = float(dispatchThreadID.x) + 0.5 - WipePosition * float(active.x);
		float4 passthrough = OriginalColor[dispatchThreadID.xy];
		if (abs(offset) < 1.0) {
			float white = 1.0;
			if (ColorDomain != kNeuralColorDomainDisplayGamma) {
				NeuralDisplayTransform display = MakeNeuralDisplayTransform(DisplayParam, DisplayCinematic, DisplayTint, DisplayExposure,
					VanillaAdaptation.SampleLevel(LinearClampSampler, float2(0.5, 0.5), 0), PostProcessAdaptation[0], HighlightWhite);
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

	// "Show Material Categories" debug view: render the classification itself, nearest-neighbour,
	// instead of blending the model's edit. This skips the resolve entirely rather than reusing the
	// tent-filtered strengths below - those are resolved *strengths*, not a category id, and can't be
	// mapped back to one; a nearest lookup also shows the raw per-pixel classification the tent filter
	// exists to soften. Left GuideSize == 0 (guide not yet configured for this placement) as EverythingElse.
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

	// The original pixel holds scene position (pixel - JitterOffset) on the
	// unjittered grid the model saw. The model and proxy textures span that same
	// active region at the model raster, so normalising by the active size lands
	// on the matching model position whatever the scale. Sample the model's
	// answer and the proxy it was given there so the ratio between them is the
	// edit for this exact scene point; at native scale with a zero offset this is
	// the texel centre.
	float2 uv = (float2(dispatchThreadID.xy) + 0.5 - JitterOffset) / float2(ActiveSize);
	// A stale answer (alternating-frame skip) was computed for the previous frame,
	// so this scene point sat elsewhere in it. Follow the game's motion vector back
	// to where it was - Skyrim stores current -> previous as a normalised UV offset
	// over the active region, the same normalisation as uv - and read the answer
	// and its proxy there. Without this the previous frame's edit lands on
	// whatever moved under the pixel, the stale-edit guard below rejects almost
	// the whole frame under any camera motion, and the edit strobes on and off at
	// half the frame rate. A point that came from outside the previous frame has
	// no answer to reuse and shows the clean frame.
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

	// Diagnostic: write Feature 18's answer straight through, preserving the
	// renderer's alpha, bypassing ResolveNeuralColor and every strength/guard
	// below entirely. Restricted to the display-gamma domain (Finished Image) -
	// in the scene-linear domain this would dump a display-referred, roughly
	// 0-1 model answer into a linear HDR buffer the game's own tonemapper still
	// has to process, which is not a meaningful image. Not the normal path; it
	// exists to tell apart a weak model answer from an over-conservative resolve.
	if (RawModelOutput != 0 && ColorDomain == kNeuralColorDomainDisplayGamma) {
		float3 rawLinear = NeuralModelToLinear(model.rgb, ColorDomain);
		DestinationColor[dispatchThreadID.xy] = float4(NeuralLinearToDomain(rawLinear, ColorDomain), original.a);
		return;
	}

	float categoryColorStrength = 1.0;
	float categoryTransferStrength = 1.0;
	float categoryLuminosityStrength = 1.0;
	// Conservative fallback when no guide is available to classify this pixel:
	// guard everywhere rather than silently going unguarded.
	float categoryHueGuardAmount = 1.0;
	if (all(GuideSize > 0)) {
		// Blend a 3x3 neighbourhood of guide texels' resolved category
		// strengths (and hue-guard toggle) with a tent (triangular) filter
		// instead of switching on one nearest-neighbour category. A hard
		// switch flips discretely right at a material boundary; under TAA
		// jitter the boundary pixel picks a different neighbour every frame,
		// and wherever the two categories' sliders differ that reads as
		// shimmer. A 2-texel-wide (radius ~1 texel) bilinear blend still
		// wasn't enough for very thin, high-frequency edges like individual
		// hair strands, which can be only 1-2 guide texels wide and so sit
		// "near a boundary" on both sides at almost every texel along their
		// length; widen the radius to smooth those out too. The blend is
		// done on the resolved strength/toggle values, not the category id
		// itself - an id is a discrete index and can't be meaningfully
		// interpolated; a hue-guard toggle blends into a fractional "amount"
		// the same way, softening the boundary instead of a hard flip.
		// Texel-index space (integer = texel centre) for the tent weights; the
		// jitter the guides carry is already folded in by NeuralGuidePosition.
		float2 guideCoord = NeuralGuidePosition(dispatchThreadID.xy, GuideSize, ActiveSize, GuideJitterOffset) - 0.5;
		int2 guideCenter = (int2)round(guideCoord);
		int2 guideMax = int2(GuideSize) - 1;

		categoryColorStrength = 0.0;
		categoryTransferStrength = 0.0;
		categoryLuminosityStrength = 0.0;
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
				categoryLuminosityStrength += tapWeight * CategoryLuminosityStrengths[tapCategory >> 2][tapCategory & 3];
				categoryHueGuardAmount += tapWeight * float((HueGuardMask >> tapCategory) & 1u);
				totalTapWeight += tapWeight;
			}
		}
		categoryColorStrength /= max(totalTapWeight, 1e-5);
		categoryTransferStrength /= max(totalTapWeight, 1e-5);
		categoryLuminosityStrength /= max(totalTapWeight, 1e-5);
		categoryHueGuardAmount /= max(totalTapWeight, 1e-5);
	}

	// Category controls shape the local result first. The existing global sliders
	// remain a final multiplier over every category.
	float resolvedColorStrength = categoryColorStrength * ColorStrength;
	float editWeight = categoryTransferStrength * TransferStrength;
	float resolvedLuminosityStrength = categoryLuminosityStrength * LuminosityStrength;
	// A stale answer has been reprojected above; fade it out wherever the content
	// under the pixel still differs from what the model saw (disocclusion, a light
	// switching, an animated surface). The fresh frame is encoded with the same
	// display transform the stale proxy received, so only genuine content changes
	// register.
	if (StaleAnswer != 0) {
		NeuralDisplayTransform display = MakeNeuralDisplayTransform(DisplayParam, DisplayCinematic, DisplayTint, DisplayExposure,
			VanillaAdaptation.SampleLevel(LinearClampSampler, float2(0.5, 0.5), 0), PostProcessAdaptation[0], HighlightWhite);
		editWeight *= answerOnScreen ? NeuralStaleEditWeight(proxy, original, ColorDomain, display) : 0.0;
	}
	if (DepthAwareResolve != 0 && all(GuideSize > 0)) {
		// Left fractional (not rounded to a texel) so NeuralSilhouetteWeight can
		// bilinearly blend across the guide/active resolution mismatch instead of
		// aliasing on thin silhouettes.
		float2 guideTexel = NeuralGuidePosition(dispatchThreadID.xy, GuideSize, ActiveSize, GuideJitterOffset);
		editWeight *= NeuralSilhouetteWeight(GuideDepth, LinearClampSampler, guideTexel, GuideSize);
	}

	DestinationColor[dispatchThreadID.xy] = ResolveNeuralColor(model, proxy, original, resolvedColorStrength, editWeight,
		resolvedLuminosityStrength, ColorDomain, categoryHueGuardAmount, MaxRatio);
}
