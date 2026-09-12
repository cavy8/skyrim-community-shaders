#include "Common/NeuralRenderingCategories.hlsli"
#include "Upscaling/NeuralRendering/ColorTransfer.hlsli"

cbuffer TransferParams : register(b0)
{
	float2 JitterOffset;  // Sub-pixel projection offset of the original raster, in render pixels.
	float ColorStrength;
	float TransferStrength;  // Overall edit weight (0 = untouched frame, 1 = the model's change, 2 = doubled).
	uint2 ActiveSize;  // Valid region of OriginalColor and DestinationColor, in their texels.
	uint2 WorkSize;    // Model raster; ModelColor and ProxyColor are allocated at this size.
	uint2 GuideSize;   // Valid region of GuideDepth (render resolution), in its texels.
	uint DepthAwareResolve;  // Non-zero: fade the edit across depth silhouettes (see NeuralSilhouetteWeight).
	uint SkipFrame;          // Non-zero: the model was not run this frame; ModelColor/ProxyColor are stale.
	uint PerCategoryStrengths;
	float2 GuideJitterOffset;  // Projection offset of the guide rasters relative to the colour raster, in guide texels.
	uint ColorDomain;          // kNeuralColorDomain* - how OriginalColor and DestinationColor are encoded.
	float4 CategoryColorStrengths[2];
	float4 CategoryTransferStrengths[2];
	float4 DisplayParam;      // x: replicate the vanilla tonemap, y: ISHDR Param.y (white point), z: ISHDR Param.z (Hejl-Burgess-Dawson).
	float4 DisplayCinematic;  // ISHDR Cinematic: x saturation, z contrast, w brightness.
	float4 DisplayTint;       // ISHDR Tint: xyz colour, w amount.
	float4 DisplayExposure;   // x: apply Post Processing auto exposure, y: 0.18 * compensation, zw: adaptation range.
};

Texture2D<float4> ModelColor : register(t0);     // Feature 18 answer, display-referred proxy domain.
Texture2D<float4> OriginalColor : register(t1);  // Untouched linear scene colour, jittered raster.
Texture2D<float4> ProxyColor : register(t2);     // The exact proxy EncodeColorCS handed the model.
Texture2D<float> GuideDepth : register(t3);      // Game depth at the guide resolution.
Texture2D<float> MaterialCategories : register(t4);  // Masks2: category in the low three R16_UNORM bits.
Texture2D<float2> VanillaAdaptation : register(t5);            // Same inputs EncodeColorCS used for the display transform,
StructuredBuffer<float> PostProcessAdaptation : register(t6);  // so a stale proxy can be compared with a fresh encode.
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

	// The original pixel holds scene position (pixel - JitterOffset) on the
	// unjittered grid the model saw. The model and proxy textures span that same
	// active region at the model raster, so normalising by the active size lands
	// on the matching model position whatever the scale. Sample the model's
	// answer and the proxy it was given there so the ratio between them is the
	// edit for this exact scene point; at native scale with a zero offset this is
	// the texel centre.
	float2 uv = (float2(dispatchThreadID.xy) + 0.5 - JitterOffset) / float2(ActiveSize);
	float4 model = ModelColor.SampleLevel(LinearClampSampler, uv, 0);
	float4 proxy = ProxyColor.SampleLevel(LinearClampSampler, uv, 0);

	float4 original = OriginalColor[dispatchThreadID.xy];
	float categoryColorStrength = 1.0;
	float categoryTransferStrength = 1.0;
	if (PerCategoryStrengths != 0 && all(GuideSize > 0)) {
		// Blend a 3x3 neighbourhood of guide texels' resolved category
		// strengths with a tent (triangular) filter instead of switching on
		// one nearest-neighbour category. A hard switch flips discretely
		// right at a material boundary; under TAA jitter the boundary pixel
		// picks a different neighbour every frame, and wherever the two
		// categories' sliders differ that reads as shimmer. A 2-texel-wide
		// (radius ~1 texel) bilinear blend still wasn't enough for very
		// thin, high-frequency edges like individual hair strands, which can
		// be only 1-2 guide texels wide and so sit "near a boundary" on both
		// sides at almost every texel along their length; widen the radius
		// to smooth those out too. The blend is done on the resolved
		// strength values, not the category id itself - an id is a discrete
		// index and can't be meaningfully interpolated.
		// Texel-index space (integer = texel centre) for the tent weights; the
		// jitter the guides carry is already folded in by NeuralGuidePosition.
		float2 guideCoord = NeuralGuidePosition(dispatchThreadID.xy, GuideSize, ActiveSize, GuideJitterOffset) - 0.5;
		int2 guideCenter = (int2)round(guideCoord);
		int2 guideMax = int2(GuideSize) - 1;

		categoryColorStrength = 0.0;
		categoryTransferStrength = 0.0;
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
				totalTapWeight += tapWeight;
			}
		}
		categoryColorStrength /= max(totalTapWeight, 1e-5);
		categoryTransferStrength /= max(totalTapWeight, 1e-5);
	}

	// Category controls shape the local result first. The existing global sliders
	// remain a final multiplier over every category.
	float resolvedColorStrength = categoryColorStrength * ColorStrength;
	float editWeight = categoryTransferStrength * TransferStrength;
	// On an alternating skip frame the model's previous answer is re-applied to
	// the fresh frame; fade it out wherever the content under the pixel changed.
	// The fresh frame is encoded with the same display transform the stale proxy
	// received, so only genuine content changes register.
	if (SkipFrame != 0) {
		NeuralDisplayTransform display = MakeNeuralDisplayTransform(DisplayParam, DisplayCinematic, DisplayTint, DisplayExposure,
			VanillaAdaptation.SampleLevel(LinearClampSampler, float2(0.5, 0.5), 0), PostProcessAdaptation[0]);
		editWeight *= NeuralStaleEditWeight(proxy, original, ColorDomain, display);
	}
	if (DepthAwareResolve != 0 && all(GuideSize > 0)) {
		// Left fractional (not rounded to a texel) so NeuralSilhouetteWeight can
		// bilinearly blend across the guide/active resolution mismatch instead of
		// aliasing on thin silhouettes.
		float2 guideTexel = NeuralGuidePosition(dispatchThreadID.xy, GuideSize, ActiveSize, GuideJitterOffset);
		editWeight *= NeuralSilhouetteWeight(GuideDepth, LinearClampSampler, guideTexel, GuideSize);
	}

	DestinationColor[dispatchThreadID.xy] = ResolveNeuralColor(model, proxy, original, resolvedColorStrength, editWeight, ColorDomain);
}
