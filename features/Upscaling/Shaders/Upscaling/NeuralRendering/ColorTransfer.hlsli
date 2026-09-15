#ifndef UPSCALING_NEURALRENDERING_COLORTRANSFER
#define UPSCALING_NEURALRENDERING_COLORTRANSFER

// Colour transfer used to move scene colour into and out of the DLSS Neural
// Rendering (NGX Feature 18) shared textures.
//
// The Before/After/Separate Upscaling placements run Neural Rendering *before*
// the game's tonemapper, so the colour handed in is linear and open-ended -
// routinely well above 1.0 on skies, speculars and emissives. Feature 18 is
// created without an HDR flag and was trained on ordinary display-referred
// (tone-mapped, sRGB) SDR frames. Handing it raw linear HDR leaves it re-deciding
// those out-of-range pixels every frame with nothing anchoring them, which reads
// as shimmer and flicker on exactly the bright regions.
//
// EncodeNeuralColor brings the frame into that display-referred domain, and it
// does so through the display transform the frame is actually about to receive
// (NeuralDisplayTransform): the game's eye adaptation (and Post Processing's auto
// exposure when active) and, under the vanilla tonemap, ISHDR's own white point,
// saturation, tint, brightness and contrast stages. A plain unexposed Reinhard of
// the linear scene - what an earlier version handed over - showed the model a
// frame that was far darker (interiors) or flatter (exteriors) than the one the
// user sees, so it pushed local tone and contrast hard to "fix" it, and the
// game's adaptation and contrast then amplified that edit again on the way to
// the screen: neural shading stacked on top of game shading. Matching the proxy
// to the display transform means the model asks for the same edit it would ask
// for on the finished frame. Only the *view* changes: the edit is still a ratio
// against the proxy and is still applied to the untouched linear colour. The
// model's answer is deliberately not inverse-tonemapped.  The derivative of
// inverse Reinhard is 1 / (1 - x)^2, so tiny frame-to-frame changes near white
// used to become enormous scene-linear shading changes.  Instead the resolve
// measures the model's bounded luminance change in proxy space, then carries its
// chroma change - relative to the proxy, and hue-guarded on near-neutral pixels
// so a model colour cast cannot tint renderer-neutral shading - onto that guarded
// scene luminance. The proxy compression uses one RGB scale so it does not
// distort hue before the model sees it. HDR headroom remains renderer-owned and
// every frame is re-anchored to deterministic scene colour rather than to model
// history.
//
// Colour domain. Everything above describes kNeuralColorDomainSceneLinear, the
// pre-tonemap placements. Finished Image runs after the tonemap instead, on a
// frame that is already gamma-2.2 display-referred (0-1 in SDR; HDR Display's
// redirect can carry values above one). Treating that as linear would compress
// and re-encode an already-encoded image, handing the model a washed-out,
// over-bright proxy. kNeuralColorDomainDisplayGamma therefore decodes the frame
// with the same 2.2 curve HDR Display uses, scales only genuinely over-range
// pixels down by one hue-preserving factor, and re-encodes with that curve - an
// exact pass-through for SDR, so the model sees the finished frame as-is. The
// resolve decodes proxy, model and original with that same curve, applies the
// edit in linear light and re-encodes the result.
//
// Jitter. "Before Upscaling" runs on the raw render-resolution raster, which the
// game rendered with the per-frame sub-pixel TAA jitter DLSS later removes. The
// model has no jitter parameter and was trained on unjittered, resolved frames;
// with the framing wobbling by up to a pixel each frame it re-decides its local
// tone and structure every frame, which reads as shadows and detail drifting
// around. The encode therefore resamples the frame onto the unjittered pixel
// grid so the model sees a stable framing, and the resolve samples the model's
// answer back at the jittered position of every original pixel. Only the
// *edit* (a luminance ratio and chroma) is ever resampled - the colour DLSS
// receives is still the original jittered sample scaled by that edit, so DLSS
// keeps the sharp, correctly-jittered input it expects. After the upscaler the
// offset is zero and both samples land exactly on texel centres.
//
// The depth, motion and material-category guides are the game's render-resolution
// targets in every placement, so they always carry that same jitter. After the
// upscaler that makes them jittered relative to the resolved colour, which is the
// one case where a guide lookup needs correcting; NeuralGuidePosition applies it.
//
// JitterOffset follows the Streamline convention: it is the sub-pixel offset
// (in render pixels) the projection applied, so a scene point that projects to
// unjittered pixel position u lands in the raster at u + JitterOffset.

static const float3 kNeuralLuma = float3(0.2126, 0.7152, 0.0722);
static const float kNeuralRatioFloor = 1.0 / 512.0;
// Per-channel guard on the model's chroma change relative to the proxy (see ResolveNeuralColor).
static const float kNeuralChromaRatioMin = 0.25;
static const float kNeuralChromaRatioMax = 4.0;
// Luma-weighted chroma magnitude of the *original* pixel below which the model may not
// rotate its hue: a renderer-neutral pixel stays neutral, and the lock releases smoothly
// as the original carries more chroma of its own. A bluish shadow or pale skin measures
// ~0.1 in this metric, saturated foliage ~0.3, a pure grey exactly 0.
static const float kNeuralHueGuardStart = 0.03;
static const float kNeuralHueGuardEnd = 0.2;

// TransferParams.ColorDomain values; keep in sync with NeuralRendering::ColorDomain.
static const uint kNeuralColorDomainSceneLinear = 0;   // Linear, open-ended HDR scene colour (pre-tonemap placements).
static const uint kNeuralColorDomainDisplayGamma = 1;  // Finished gamma-2.2 display-referred frame (Finished Image).

float3 NeuralLinearToSrgb(float3 v)
{
	v = saturate(v);
	return lerp(v * 12.92, 1.055 * pow(max(v, 1e-8), 1.0 / 2.4) - 0.055, step(0.0031308, v));
}

float3 NeuralSrgbToLinear(float3 v)
{
	v = saturate(v);
	return lerp(v / 12.92, pow((v + 0.055) / 1.055, 2.4), step(0.04045, v));
}

/**
 * Display transform the pre-tonemap placements approximate when building the
 * scene-linear proxy, so the model sees the frame the way the user will (see
 * the file comment). Built per pixel by MakeNeuralDisplayTransform from the
 * TransferParams constants and the two adaptation inputs; every field is
 * uniform over the frame.
 */
struct NeuralDisplayTransform
{
	float exposure;          // Scalar exposure the frame will receive: vanilla adaptation x Post Processing auto exposure.
	bool vanillaGrading;     // Replicate ISHDR's tonemap, cinematic and contrast stages (vanilla tonemap owner).
	float adaptedLuminance;  // ISHDR's adapted average luminance (AvgTex.x); the pivot of its contrast stage.
	float whitePoint;        // ISHDR Param.y.
	bool hejlBurgessDawson;  // ISHDR Param.z: filmic curve instead of Reinhard.
	float saturation;        // ISHDR Cinematic.x.
	float contrast;          // ISHDR Cinematic.z.
	float brightness;        // ISHDR Cinematic.w.
	float3 tintColor;        // ISHDR Tint.xyz.
	float tintAmount;        // ISHDR Tint.w.
};

/** No exposure and no grading: the plain hue-preserving ACES-filmic proxy (NeuralAcesFilmic). */
NeuralDisplayTransform NeuralIdentityDisplayTransform()
{
	NeuralDisplayTransform display;
	display.exposure = 1.0;
	display.vanillaGrading = false;
	display.adaptedLuminance = 0.0;
	display.whitePoint = 0.0;
	display.hejlBurgessDawson = false;
	display.saturation = 1.0;
	display.contrast = 1.0;
	display.brightness = 1.0;
	display.tintColor = 1.0;
	display.tintAmount = 0.0;
	return display;
}

/**
 * Resolves the display transform from the TransferParams constants and the two
 * adaptation inputs. Either input may be unbound (reads as zero) and then drops
 * out; with neither bound and no vanilla constants this is the identity.
 *
 * @param displayParam x > 0.5: the vanilla tonemap owns the frame and its constants
 *                     were captured; y/z: ISHDR Param.y/.z.
 * @param displayCinematic ISHDR Cinematic (x saturation, z contrast, w brightness).
 * @param displayTint ISHDR Tint (xyz colour, w amount).
 * @param displayExposure x > 0.5: Post Processing auto exposure is active; y its
 *                        0.18 * compensation factor, zw its adaptation range.
 * @param vanillaAdaptation ISHDR AvgTex: x adapted luminance, y target luminance.
 * @param postProcessAdaptedLuminance Post Processing's adapted luminance.
 */
NeuralDisplayTransform MakeNeuralDisplayTransform(float4 displayParam, float4 displayCinematic, float4 displayTint,
	float4 displayExposure, float2 vanillaAdaptation, float postProcessAdaptedLuminance)
{
	NeuralDisplayTransform display = NeuralIdentityDisplayTransform();
	// ISHDR: if (avgValue.x != 0 && avgValue.y != 0) inputColor *= avgValue.y / avgValue.x;
	const bool vanillaValid = displayParam.x > 0.5 && vanillaAdaptation.x > 0.0 && vanillaAdaptation.y > 0.0;
	if (vanillaValid)
		display.exposure *= vanillaAdaptation.y / vanillaAdaptation.x;
	// Post Processing Composite: 0.18 * ExposureCompensation / clamp(avgLuma, AdaptationRange).
	if (displayExposure.x > 0.5 && postProcessAdaptedLuminance > 0.0)
		display.exposure *= displayExposure.y / clamp(postProcessAdaptedLuminance, displayExposure.z, displayExposure.w);
	display.vanillaGrading = vanillaValid;
	display.adaptedLuminance = vanillaAdaptation.x;
	display.whitePoint = displayParam.y;
	display.hejlBurgessDawson = displayParam.z > 0.5;
	display.saturation = displayCinematic.x;
	display.contrast = displayCinematic.z;
	display.brightness = displayCinematic.w;
	display.tintColor = displayTint.xyz;
	display.tintAmount = displayTint.w;
	return display;
}

/** Linear light of a colour stored in @p domain (open-ended; negatives clamp to zero). */
float3 NeuralDomainToLinear(float3 color, uint domain)
{
	color = max(color, 0.0);
	return domain == kNeuralColorDomainDisplayGamma ? pow(color, 2.2) : color;
}

/** Inverse of NeuralDomainToLinear. */
float3 NeuralLinearToDomain(float3 color, uint domain)
{
	color = max(color, 0.0);
	return domain == kNeuralColorDomainDisplayGamma ? pow(color, 1.0 / 2.2) : color;
}

/** Model-space (display-encoded, 0-1) value to linear light, using @p domain's curve. */
float3 NeuralModelToLinear(float3 v, uint domain)
{
	return domain == kNeuralColorDomainDisplayGamma ? pow(saturate(v), 2.2) : NeuralSrgbToLinear(v);
}

/** Linear light (0-1) to the model-space encoding for @p domain. */
float3 NeuralLinearToModel(float3 v, uint domain)
{
	return domain == kNeuralColorDomainDisplayGamma ? pow(saturate(v), 1.0 / 2.2) : NeuralLinearToSrgb(v);
}

/** ISHDR's Hejl-Burgess-Dawson curve on one luminance value, linear out (GetTonemapFactorHejlBurgessDawson). */
float NeuralHejlBurgessDawson(float luminance, float whitePoint)
{
	float tmp = max(0.0, luminance - 0.004);
	float encoded = ((tmp * 6.2 + 0.5) * tmp) / (tmp * (tmp * 6.2 + 1.7) + 0.06);
	return whitePoint * NeuralSrgbToLinear(encoded.xxx).x;
}

/**
 * Krzysztof Narkowicz's compact fit to the ACES reference tonemap curve
 * ("ACES Filmic Tone Mapping Curve", 2016), on one luminance value.
 *
 * Used as the fallback proxy tonemap below in place of a plain Reinhard
 * (`x / (1 + x)`). Reinhard compresses continuously starting at x = 0, not
 * just the highlights, so even midtones read as flatter and lower-contrast
 * to the model than any actual filmic response - vanilla's own
 * Hejl-Burgess-Dawson/Reinhard-with-white-point curve above, Post Processing's
 * selectable tonemappers (ACES/Frostbite/Melon/...), and typical ReShade-style
 * curves under Effects11 - all keep a near-linear response through shadows and
 * midtones and only roll off toward white. This is a small, widely used,
 * dependency-free approximation of that general shape; it is not an attempt to
 * match any one of those curves exactly; a mismatch there stays a residual
 * `Transfer Strength` corrects.
 */
float NeuralAcesFilmic(float x)
{
	const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
	return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

/**
 * Display-linear proxy of scene-linear @p linearColor under @p display.
 *
 * Without vanilla grading this is the exposed colour through NeuralAcesFilmic,
 * applied hue-preservingly on luminance and rescaled back onto colour exactly
 * like the vanilla-grading tonemap stage below. With vanilla grading it
 * instead replicates the SDR path of ISHDR.hlsl's BLEND pass stage for stage -
 * exposure, the luminance-driven Reinhard (white point) or Hejl-Burgess-Dawson
 * curve, saturation / tint / brightness, and the shadow-aware contrast around
 * the adapted luminance - omitting only bloom, the fade overlay and the HDR
 * display mapping. The output is clamped to 0..1 like the frame it stands for.
 */
float3 ApplyNeuralDisplayTransform(float3 linearColor, NeuralDisplayTransform display)
{
	float3 color = max(linearColor, 0.0) * display.exposure;
	if (!display.vanillaGrading) {
		float luminance = dot(color, kNeuralLuma);
		float mapped = NeuralAcesFilmic(luminance);
		return color * (mapped / max(luminance, 1e-5));
	}

	float luminance = dot(color, kNeuralLuma);
	float mapped = display.hejlBurgessDawson ?
	                   NeuralHejlBurgessDawson(luminance, display.whitePoint) :
	                   (luminance * (luminance * display.whitePoint + 1.0)) / (luminance + 1.0);
	color *= mapped / max(luminance, 1e-5);

	float blendedLuminance = dot(color, kNeuralLuma);
	float3 tinted = display.brightness *
	                lerp(lerp(blendedLuminance.xxx, color, display.saturation), blendedLuminance * display.tintColor, display.tintAmount);

	float3 contrasted = lerp(display.adaptedLuminance.xxx, tinted, display.contrast);
	float safeAverage = max(display.adaptedLuminance, 1e-5);
	float3 contrastedModified = pow(max(0.0, abs(tinted) / safeAverage), display.contrast) * safeAverage * sign(tinted);
	contrasted = lerp(contrastedModified, contrasted, saturate(contrastedModified / 0.1));
	return saturate(contrasted);
}

/**
 * Linear-light proxy of @p color with every channel at or below one.
 *
 * Display gamma: the frame is already tonemapped, so only genuinely over-range
 * (HDR) pixels are scaled down by one hue-preserving factor and an SDR frame
 * passes through unchanged. Scene linear: the colour goes through @p display
 * (see ApplyNeuralDisplayTransform); with the identity transform that is the
 * hue-preserving scalar ACES-filmic curve, a single positive scale of the
 * linear light.
 */
float3 EncodeNeuralProxy(float3 color, uint domain, NeuralDisplayTransform display)
{
	float3 linearColor = NeuralDomainToLinear(color, domain);
	if (domain == kNeuralColorDomainDisplayGamma) {
		float peak = max(linearColor.r, max(linearColor.g, linearColor.b));
		return linearColor / max(peak, 1.0);
	}
	return ApplyNeuralDisplayTransform(linearColor, display);
}

/**
 * Transform colour stored in @p domain into the display-referred proxy the model sees.
 */
float4 EncodeNeuralColor(float4 color, uint domain, NeuralDisplayTransform display)
{
	return float4(NeuralLinearToModel(EncodeNeuralProxy(color.rgb, domain, display), domain), color.a);
}

/**
 * Catmull-Rom resample of linear scene colour at a fractional texel position.
 *
 * Nine bilinear fetches reproduce the sixteen-tap kernel. A plain bilinear
 * shift would blur by an amount that changes with the jitter phase, so the model
 * would see sharpness pulsing frame to frame; Catmull-Rom keeps that nearly
 * constant. Taps are clamped to the active region, never the allocation, because
 * the game renders into the top-left of natively sized targets and the margin
 * holds stale frames. The result is clamped to the 2x2 neighbourhood so the
 * negative lobes cannot ring on HDR speculars.
 *
 * @param position Texel-space sample position (pixel centres sit at n + 0.5).
 * @param activeSize Valid region of @p source in texels.
 * @param allocationSize Allocated size of @p source in texels.
 */
float3 SampleNeuralSourceCatmullRom(Texture2D<float4> source, SamplerState linearClamp,
	float2 position, float2 activeSize, float2 allocationSize)
{
	float2 texPos1 = floor(position - 0.5) + 0.5;
	float2 f = position - texPos1;

	float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	float2 w3 = f * f * (-0.5 + 0.5 * f);
	float2 w12 = w1 + w2;

	float2 lo = 0.5;
	float2 hi = activeSize - 0.5;
	float2 texPos0 = clamp(texPos1 - 1.0, lo, hi) / allocationSize;
	float2 texPos3 = clamp(texPos1 + 2.0, lo, hi) / allocationSize;
	float2 texPos12 = clamp(texPos1 + w2 / w12, lo, hi) / allocationSize;

	float3 result = 0.0;
	result += source.SampleLevel(linearClamp, float2(texPos0.x, texPos0.y), 0).rgb * w0.x * w0.y;
	result += source.SampleLevel(linearClamp, float2(texPos12.x, texPos0.y), 0).rgb * w12.x * w0.y;
	result += source.SampleLevel(linearClamp, float2(texPos3.x, texPos0.y), 0).rgb * w3.x * w0.y;
	result += source.SampleLevel(linearClamp, float2(texPos0.x, texPos12.y), 0).rgb * w0.x * w12.y;
	result += source.SampleLevel(linearClamp, float2(texPos12.x, texPos12.y), 0).rgb * w12.x * w12.y;
	result += source.SampleLevel(linearClamp, float2(texPos3.x, texPos12.y), 0).rgb * w3.x * w12.y;
	result += source.SampleLevel(linearClamp, float2(texPos0.x, texPos3.y), 0).rgb * w0.x * w3.y;
	result += source.SampleLevel(linearClamp, float2(texPos12.x, texPos3.y), 0).rgb * w12.x * w3.y;
	result += source.SampleLevel(linearClamp, float2(texPos3.x, texPos3.y), 0).rgb * w3.x * w3.y;

	// Neighbourhood clamp against the 2x2 texels the position falls between.
	int2 maxIndex = int2(activeSize) - 1;
	int2 base = clamp(int2(floor(position - 0.5)), 0, maxIndex);
	int2 next = min(base + 1, maxIndex);
	float3 c00 = source.Load(int3(base.x, base.y, 0)).rgb;
	float3 c10 = source.Load(int3(next.x, base.y, 0)).rgb;
	float3 c01 = source.Load(int3(base.x, next.y, 0)).rgb;
	float3 c11 = source.Load(int3(next.x, next.y, 0)).rgb;
	float3 neighbourhoodMin = min(min(c00, c10), min(c01, c11));
	float3 neighbourhoodMax = max(max(c00, c10), max(c01, c11));
	return clamp(result, neighbourhoodMin, neighbourhoodMax);
}

/**
 * Catmull-Rom weights/indices for one axis, as four raw taps (no hardware
 * bilinear collapsing) so this axis can be combined with a differently-shaped
 * filter on the other axis. Matches the weights in SampleNeuralSourceCatmullRom
 * exactly; only the tap layout differs. Slots 4 and 5 are unused zero-weight
 * pads so the array shares a size with NeuralBoxAxis.
 *
 * @param coord Texel-space sample position on this axis (centres at n + 0.5).
 */
void NeuralCubicAxis(float coord, out float weight[6], out int index[6])
{
	float base = floor(coord - 0.5);
	float f = coord - 0.5 - base;

	weight[0] = f * (-0.5 + f * (1.0 - 0.5 * f));
	weight[1] = 1.0 + f * f * (-2.5 + 1.5 * f);
	weight[2] = f * (0.5 + f * (2.0 - 1.5 * f));
	weight[3] = f * f * (-0.5 + 0.5 * f);
	weight[4] = 0.0;
	weight[5] = 0.0;

	int b = (int)base;
	index[0] = b - 1;
	index[1] = b;
	index[2] = b + 1;
	index[3] = b + 2;
	index[4] = b + 2;
	index[5] = b + 2;
}

/**
 * Exact-area box weights/indices for one axis: the fraction of each source
 * texel covered by the destination texel's footprint, so the axis is
 * integrated rather than reconstructed at one point. Six taps comfortably
 * covers the largest footprint the model resolution slider allows (4 source
 * texels at the 0.25x minimum) plus the fractional slop `ScaledExtent`'s
 * round-to-even can introduce.
 *
 * @param coord Texel-space centre of the footprint on this axis.
 * @param footprint Source texels this destination texel covers on this axis (> 1).
 */
void NeuralBoxAxis(float coord, float footprint, out float weight[6], out int index[6])
{
	float lo = coord - footprint * 0.5;
	float hi = coord + footprint * 0.5;
	int i0 = (int)floor(lo);
	float invFootprint = 1.0 / footprint;

	[unroll]
	for (int k = 0; k < 6; ++k) {
		int i = i0 + k;
		float overlap = max(0.0, min(hi, (float)(i + 1)) - max(lo, (float)i));
		weight[k] = overlap * invFootprint;
		index[k] = i;
	}
}

/**
 * Resamples the source at a fractional position where at least one axis is
 * shrinking (a footprint of more than one source texel per destination
 * texel), integrating that axis with an exact-area box instead of
 * reconstructing it with Catmull-Rom.
 *
 * Catmull-Rom (and any other point-sample reconstruction filter) answers "what
 * is the signal at this one point", which is the right question when the
 * destination is at or above source resolution. When the destination is
 * coarser, the question a model texel actually needs answered is "what is the
 * average of the source over the region this texel represents" - the two only
 * coincide at native scale. Left unanswered, source frequencies above the
 * model's new, lower Nyquist limit alias into the proxy; as the camera moves
 * the aliasing changes phase and the resolve reads it as neural shimmer. This
 * is the box downsample OptiScaler's DLSSNR fork uses below native
 * (https://github.com/Dagherbou/OptiScaler_DLSSNR/discussions/2).
 *
 * Each axis is filtered independently: an axis whose footprint is still one
 * texel or less (native scale, or that axis is being supersampled) keeps
 * Catmull-Rom reconstruction instead, so an anisotropic scale like 0.65 x 0.85
 * only integrates the axis that is actually shrinking. The combined result is
 * clamped to the range of every texel actually sampled, which is a no-op for
 * the (always non-negative) box weights and only bites on a Catmull-Rom axis's
 * negative lobes - the same HDR ringing guard SampleNeuralSourceCatmullRom
 * applies, generalised to whichever taps this call used.
 *
 * @param position Texel-space sample position (pixel centres sit at n + 0.5).
 * @param footprint Source texels one destination texel covers, per axis.
 * @param activeSize Valid region of @p source in texels.
 */
float3 SampleNeuralSourceAreaMinify(Texture2D<float4> source, float2 position, float2 footprint, float2 activeSize)
{
	float weightX[6], weightY[6];
	int indexX[6], indexY[6];
	if (footprint.x > 1.0)
		NeuralBoxAxis(position.x, footprint.x, weightX, indexX);
	else
		NeuralCubicAxis(position.x, weightX, indexX);
	if (footprint.y > 1.0)
		NeuralBoxAxis(position.y, footprint.y, weightY, indexY);
	else
		NeuralCubicAxis(position.y, weightY, indexY);

	int2 maxIndex = int2(activeSize) - 1;
	// First tap seeds the neighbourhood range; every tap (including the
	// zero-weight pads, which only duplicate an already-sampled texel) folds
	// into it unconditionally below, so no sentinel infinity literal is needed.
	float3 result = 0.0;
	float3 neighbourhoodMin = source.Load(int3(clamp(indexX[0], 0, maxIndex.x), clamp(indexY[0], 0, maxIndex.y), 0)).rgb;
	float3 neighbourhoodMax = neighbourhoodMin;
	[unroll]
	for (int j = 0; j < 6; ++j) {
		int y = clamp(indexY[j], 0, maxIndex.y);
		[unroll]
		for (int i = 0; i < 6; ++i) {
			int x = clamp(indexX[i], 0, maxIndex.x);
			float3 tap = source.Load(int3(x, y, 0)).rgb;
			result += tap * weightX[i] * weightY[j];
			neighbourhoodMin = min(neighbourhoodMin, tap);
			neighbourhoodMax = max(neighbourhoodMax, tap);
		}
	}
	return clamp(result, neighbourhoodMin, neighbourhoodMax);
}

/**
 * Guide-raster position a colour pixel's scene point occupies.
 *
 * The depth, motion and material-category guides are always the game's render-
 * resolution targets, rendered with the frame's sub-pixel TAA jitter. Before
 * the upscaler the colour is that same jittered raster at that same resolution,
 * so the mapping is the identity and @p guideJitterOffset is zero. After the
 * upscaler the colour is display resolution and already resolved onto the
 * unjittered grid, so the pixel's scene point is first scaled into guide space
 * and then shifted by the jitter the guides still carry.
 *
 * Leaving that shift out does not blur a guide lookup, it misplaces it by up to
 * half a guide texel in a direction that changes every frame with the jitter
 * phase - so a boundary pixel reads the wrong side of the boundary on some
 * frames and the right side on others. The result is kept fractional; callers
 * filter around it rather than snapping to one texel.
 *
 * @param colorPixel Colour/output pixel index.
 * @param guideSize Valid guide region in texels.
 * @param activeSize Valid colour region in texels.
 * @param guideJitterOffset Projection offset of the guides, in guide texels.
 * @return Guide-space position, texel centres at integer + 0.5.
 */
float2 NeuralGuidePosition(uint2 colorPixel, uint2 guideSize, uint2 activeSize, float2 guideJitterOffset)
{
	return (float2(colorPixel) + 0.5) * float2(guideSize) / float2(activeSize) + guideJitterOffset;
}

/**
 * Depth-aware silhouette weight for a full-resolution pixel (from
 * DLSSNR-Cost-Scaler's "Depth-Aware Bilateral Silhouette Preservation").
 *
 * When the model runs below the colour resolution its edit is upsampled
 * bilinearly, so at a geometric silhouette the background's edit bleeds a
 * texel or two into the thin foreground and vice versa. This measures the
 * relative depth range of the five-texel cross around the pixel's guide
 * position and fades the edit towards a quarter across strong
 * discontinuities, leaving flat interiors untouched.
 *
 * The cross is bilinearly sampled rather than loaded at one nearest guide
 * texel. After the upscaler the guide is at render resolution while this
 * runs at display resolution (see DecodeColorCS.hlsl), so several adjacent
 * display pixels share the same nearest guide texel; a hard nearest lookup
 * then holds one discontinuity reading over that whole block and flips it
 * wholesale between guide texels as TAA jitter moves the silhouette, which
 * reads as chunky shimmer on thin, high-frequency edges like individual hair
 * strands - the same aliasing the per-category blend above widens a kernel
 * for. Bilinear sampling instead varies continuously across that block, and
 * lands exactly on the old nearest-texel reads when guide and colour share a
 * resolution (Before/Separate Upscaling, where this never mattered).
 *
 * @param guideDepth Game depth (the guide the model received), any allocation.
 * @param linearClamp Bilinear, clamp-to-edge sampler.
 * @param guideTexel Guide-space position this colour pixel maps to, texel
 *                    centres at integer + 0.5 (may be fractional).
 * @param guideSize Valid guide region in texels.
 */
float NeuralSilhouetteWeight(Texture2D<float> guideDepth, SamplerState linearClamp, float2 guideTexel, uint2 guideSize)
{
	uint allocationWidth;
	uint allocationHeight;
	guideDepth.GetDimensions(allocationWidth, allocationHeight);
	float2 validSize = min(float2(guideSize), float2(allocationWidth, allocationHeight));
	if (any(validSize < 1.0))
		return 1.0;
	float2 allocationSize = float2(allocationWidth, allocationHeight);

	float2 lo = 0.5;
	float2 hi = validSize - 0.5;
	float2 centre = clamp(guideTexel, lo, hi);
	float2 east = clamp(float2(centre.x + 1.0, centre.y), lo, hi);
	float2 west = clamp(float2(centre.x - 1.0, centre.y), lo, hi);
	float2 south = clamp(float2(centre.x, centre.y + 1.0), lo, hi);
	float2 north = clamp(float2(centre.x, centre.y - 1.0), lo, hi);

	float depthCentre = guideDepth.SampleLevel(linearClamp, centre / allocationSize, 0);
	float depthEast = guideDepth.SampleLevel(linearClamp, east / allocationSize, 0);
	float depthWest = guideDepth.SampleLevel(linearClamp, west / allocationSize, 0);
	float depthSouth = guideDepth.SampleLevel(linearClamp, south / allocationSize, 0);
	float depthNorth = guideDepth.SampleLevel(linearClamp, north / allocationSize, 0);

	float minDepth = min(depthCentre, min(min(depthEast, depthWest), min(depthSouth, depthNorth)));
	float maxDepth = max(depthCentre, max(max(depthEast, depthWest), max(depthSouth, depthNorth)));
	float depthRange = (maxDepth - minDepth) / (maxDepth + 1e-4);
	if (depthRange <= 0.02)
		return 1.0;
	float edgeWeight = saturate(1.0 - (depthRange - 0.02) * 20.0);
	return lerp(0.25, 1.0, edgeWeight);
}

/**
 * Confidence that a model answer from the previous evaluated frame still
 * belongs to this pixel (alternating-frame mode, the proxy's "VRNR").
 *
 * Compares the luminance of the stale proxy the model actually saw against the
 * fresh frame encoded into the same domain through the same display transform
 * (a changed exposure alone would otherwise register as motion). Where they
 * differ the scene moved
 * under this pixel and the stale edit fades towards no edit, so the pixel shows
 * the clean current frame rather than a misplaced ratio. Constants match the
 * proxy's skip-frame guard.
 */
float NeuralStaleEditWeight(float4 proxyColor, float4 originalColor, uint domain, NeuralDisplayTransform display)
{
	float staleLuma = dot(NeuralModelToLinear(proxyColor.rgb, domain), kNeuralLuma);
	float freshLuma = dot(EncodeNeuralProxy(originalColor.rgb, domain, display), kNeuralLuma);
	float difference = abs(staleLuma - freshLuma);
	return saturate(1.0 - (difference * 2.5) / (staleLuma + freshLuma + 0.05));
}

/**
 * Luma-weighted magnitude of a chroma offset from neutral (see NeuralChromaOffset).
 *
 * The weighting uses the same Rec. 709 coefficients as the luminance, so a
 * deviation in a channel that carries little luminance (blue) is not counted as
 * a large colour just because it is numerically large once luma-normalised.
 */
float NeuralChromaMagnitude(float3 chroma)
{
	return sqrt(dot(chroma * chroma, kNeuralLuma));
}

/**
 * Chroma of @p color as an offset from neutral: the colour divided by its own
 * luminance, minus one. A grey is exactly zero, and the offset is orthogonal to
 * kNeuralLuma by construction, so adding it back to one never changes luminance.
 */
float3 NeuralChromaOffset(float3 color)
{
	float luma = dot(color, kNeuralLuma);
	return luma > 1e-5 ? color / luma - 1.0 : 0.0;
}

/**
 * Compose the Feature 18 answer onto the untouched scene colour.
 *
 * @p modelColor and @p proxyColor are the model's answer and the exact proxy it
 * was handed, both in the display-referred domain and both sampled at the same
 * (possibly fractional) position. A model no-op is therefore an exact no-op: its
 * luminance matches the proxy, making the ratio one. The common floor makes the
 * ratio converge smoothly to one in deep shadow, where a tiny absolute model
 * change would otherwise become an unbounded relative change. A two-sided guard
 * limits both flashes and sudden collapses without clipping individual RGB
 * channels.
 *
 * Chroma is transferred the same way: as the model's change *relative to the
 * proxy*, applied to the original. Both are expressed as luma-normalised colour,
 * so the edit is a per-channel ratio (guarded to kNeuralChromaRatioMin..Max) and
 * a model no-op reproduces the original's chroma exactly whatever the proxy's
 * own colour was. While the proxy is a plain scalar multiple of the original the
 * result is the model's complete palette, as before; a proxy that carries its
 * own grading (saturation, tint) no longer has that grading read back as a model
 * edit and applied a second time.
 *
 * The transferred chroma is then hue-guarded against the original. Where the
 * original is near neutral (kNeuralHueGuardStart..End on its luma-weighted chroma
 * magnitude) any hue the model emits is arbitrary - there is no renderer hue for
 * it to be a change *of* - and a small, consistent bias there reads as a colour
 * cast over whole shaded surfaces. So on such pixels the model may only move the
 * chroma along the original's own hue axis: more or less saturated, never rotated,
 * and never past neutral onto the complementary hue. A pure grey therefore stays
 * grey however the model recolours it. Pixels with clear chroma of their own
 * take the model's full chroma change, so intentional recolouring of skin,
 * foliage and materials survives.
 *
 * @p editWeight scales the edit as a whole (the proxy's "transfer strength"):
 * the luminance ratio is raised to it, so zero is the untouched frame, one is
 * exactly the model's relative change and two doubles it in log space, and the
 * two-sided guard clamps after scaling so a weight above one cannot escape it.
 * Chroma is gated by the same weight, saturated (see @p colorStrength below for
 * how much of it is transferred in the first place).
 *
 * @p colorStrength raises the model's per-channel chroma ratio (relative to the
 * proxy, guarded to kNeuralChromaRatioMin..Max) to itself: zero collapses that
 * ratio to one in every channel, which reproduces the original's own chroma
 * exactly and so is indistinguishable from no colour transfer at all; one is
 * the model's transferred chroma unchanged; above one - up to 2 in the UI -
 * extrapolates the same relative colour change further, re-guarded to the same
 * bound afterwards so a strength above one cannot escape it either.
 *
 * @p luminosityStrength further scales only the luminance exponent, on top of
 * @p editWeight: one reproduces the plain @p editWeight behaviour above exactly,
 * below one damps the light/dark change (useful when a strong transfer reads as
 * overly contrasty) without touching the chroma edit, and above one exaggerates
 * it further. Zero freezes luminance at the original regardless of @p editWeight.
 *
 * @p hueGuardAmount blends the hue guard above in (1) or out (0); a fractional
 * value - as produced by blending several categories' toggles across a material
 * boundary - partially releases the lock rather than switching it discretely.
 *
 * @p originalColor is stored in @p domain, and so is the result: the edit itself is
 * always applied in linear light, decoded with that domain's curve.
 *
 * @p maxRatio is the two-sided guard (1/maxRatio..maxRatio) on the model/proxy
 * luminance ratio after @p editWeight and @p luminosityStrength have scaled it;
 * one disables any luminance change, and the previous hardcoded behaviour is
 * exactly two. Values below one are treated as one - a guard cannot be tighter
 * than the floor it exists to raise.
 */
float4 ResolveNeuralColor(float4 modelColor, float4 proxyColor, float4 originalColor, float colorStrength,
	float editWeight, float luminosityStrength, uint domain, float hueGuardAmount, float maxRatio)
{
	maxRatio = max(maxRatio, 1.0);
	float3 original = NeuralDomainToLinear(originalColor.rgb, domain);
	float3 proxy = NeuralModelToLinear(proxyColor.rgb, domain);
	float3 model = NeuralModelToLinear(modelColor.rgb, domain);

	float proxyLuma = dot(proxy, kNeuralLuma);
	float modelLuma = dot(model, kNeuralLuma);
	// Some incompatible model/runtime combinations return an empty or invalid
	// frame. Treat that as no edit instead of turning a transient failure into a
	// half-bright flash through the lower ratio guard.
	if (!(modelLuma > 1e-5))
		return float4(NeuralLinearToDomain(original, domain), originalColor.a);

	editWeight = max(editWeight, 0.0);
	float lumaExponent = max(editWeight * max(luminosityStrength, 0.0), 0.0);
	float ratio = (modelLuma + kNeuralRatioFloor) / (proxyLuma + kNeuralRatioFloor);
	ratio = clamp(pow(ratio, lumaExponent), 1.0 / maxRatio, maxRatio);

	float3 luminanceResult = original * ratio;
	float targetLuma = dot(luminanceResult, kNeuralLuma);

	// Luma-normalised colour: a neutral is exactly one in every channel.
	float3 originalChroma = NeuralChromaOffset(original);
	float3 normalizedOriginal = 1.0 + originalChroma;
	float3 normalizedProxy = 1.0 + NeuralChromaOffset(proxy);
	float3 normalizedModel = 1.0 + NeuralChromaOffset(model);

	// The model's chroma change relative to the proxy it actually saw, carried
	// onto the original. Equal to the model's own chroma whenever the proxy is a
	// scalar multiple of the original; a no-op reproduces the original exactly.
	float3 chromaRatio = clamp(normalizedModel / max(normalizedProxy, 1e-3), kNeuralChromaRatioMin, kNeuralChromaRatioMax);
	// @p colorStrength raises that ratio to itself, the same log-space
	// extrapolation @p editWeight already applies to the luminance ratio above:
	// zero collapses it to 1 (the original's own chroma - see the @p colorStrength
	// doc below for why that coincides exactly with leaving chroma alone), one is
	// the model's transferred chroma unchanged, and above one extrapolates the
	// same relative colour change further, re-guarded to kNeuralChromaRatioMin..Max
	// afterwards so a strength above one cannot escape the per-channel bound.
	float3 scaledChromaRatio = clamp(pow(chromaRatio, max(colorStrength, 0.0)), kNeuralChromaRatioMin, kNeuralChromaRatioMax);
	float3 normalizedTarget = normalizedOriginal * scaledChromaRatio;
	normalizedTarget /= max(dot(normalizedTarget, kNeuralLuma), 1e-5);
	float3 targetChroma = normalizedTarget - 1.0;

	// Hue guard: on a near-neutral original, keep only the component of the
	// model's chroma that lies along the original's own hue axis (a saturation
	// change), and not past neutral. Released smoothly as the original's own
	// chroma grows, so genuinely coloured pixels take the model's full palette.
	// @p hueGuardAmount at 0 forces the lock fully open, applying the transferred
	// chroma everywhere unguarded; a fractional amount partially releases it.
	float originalChromaMagnitude = NeuralChromaMagnitude(originalChroma);
	float3 lockedChroma = 0.0;
	if (hueGuardAmount > 0.0 && originalChromaMagnitude > 1e-4) {
		float3 axis = originalChroma / originalChromaMagnitude;
		float along = max(dot(targetChroma * axis, kNeuralLuma), 0.0);
		lockedChroma = axis * along;
	}
	float hueLock = hueGuardAmount * (1.0 - smoothstep(kNeuralHueGuardStart, kNeuralHueGuardEnd, originalChromaMagnitude));
	float3 normalizedResult = max(1.0 + lerp(targetChroma, lockedChroma, hueLock), 0.0);
	normalizedResult /= max(dot(normalizedResult, kNeuralLuma), 1e-5);

	// One positive scale brings the resolved chromaticity to the guarded scene luminance.
	float3 fullColorResult = normalizedResult * targetLuma;

	// Normalized colour is unreliable only near black. Fade the chroma there,
	// while allowing the complete model palette everywhere with meaningful light.
	// @p colorStrength no longer gates this blend directly - it is already baked
	// into fullColorResult's chroma above - which is exact at zero: chromaRatio^0
	// is 1 in every channel, so normalizedTarget reduces to normalizedOriginal and
	// fullColorResult's chroma matches luminanceResult's chroma precisely (both
	// are the untouched original chroma), making the two lerp endpoints coincide
	// regardless of this weight.
	float shadowConfidence = smoothstep(kNeuralRatioFloor, 4.0 * kNeuralRatioFloor,
		min(proxyLuma, modelLuma));
	float resolvedColorStrength = shadowConfidence * saturate(editWeight);

	return float4(NeuralLinearToDomain(lerp(luminanceResult, fullColorResult, resolvedColorStrength), domain), originalColor.a);
}

#endif
