#ifndef UPSCALING_NEURALRENDERING_COLORTRANSFER
#define UPSCALING_NEURALRENDERING_COLORTRANSFER

// Display-referred proxy encoding and linear-light enhancement transfer. Model/proxy differences are
// applied to the original color without inverse tone mapping. Encode compensates color jitter; guide
// lookups compensate relative guide jitter.

static const float3 kNeuralLuma = float3(0.2126, 0.7152, 0.0722);
static const float kNeuralRatioFloor = 1.0 / 512.0;
// Per-channel guard on the model's chroma change relative to the proxy (see ResolveNeuralColor).
static const float kNeuralChromaRatioMin = 0.25;
static const float kNeuralChromaRatioMax = 4.0;
// Lock near-neutral source hue, releasing smoothly as source chroma increases. Pure gray has zero
// chroma magnitude.
static const float kNeuralHueGuardStart = 0.03;
static const float kNeuralHueGuardEnd = 0.2;
// Display-gamma proxy on an HDR target: linear peak below which NeuralHighlightRolloff is
// the identity. Everything a paper-white SDR frame puts below it reaches the model as-is.
static const float kNeuralHighlightKnee = 0.8;

// TransferParams.ColorDomain values; keep in sync with NeuralRendering::ColorDomain.
static const uint kNeuralColorDomainSceneLinear = 0;   // Linear, open-ended HDR scene colour (pre-tonemap placements).
static const uint kNeuralColorDomainDisplayGamma = 1;  // Finished gamma-2.2 display-referred frame (Finished Image).
// With Linear Lighting off, ISHDR grades gamma-encoded kMAIN; decode proxy and resolve with
// kNeuralSceneGamma.
static const uint kNeuralColorDomainSceneGamma = 2;

// Curve a finished display-referred frame is encoded with (HDR Display uses the same one).
static const float kNeuralDisplayGamma = 2.2;
// Match ISHDR HDR decoding of gamma-domain color; keep the exponent shared by encode and resolve.
static const float kNeuralSceneGamma = 2.2;

// TransferParams.ProxyCurve values; keep in sync with NeuralRendering::ProxyCurve.
static const uint kNeuralProxyDisplayMatched = 0;  // The ISHDR replica, or the ACES fallback.
static const uint kNeuralProxyNeutwo = 1;          // Exposed scene linear through the RenoDX Neutwo curve.
static const uint kNeuralProxyLegacy = 2;          // Per-channel Reinhard, unexposed.

// How the proxy is encoded for the model, and therefore how its answer is read back.
static const uint kNeuralModelSpaceSrgb = 0;     // Piecewise sRGB.
static const uint kNeuralModelSpaceGamma22 = 1;  // Plain 2.2, what the displayed frame actually carries.

// TransferParams.DebugFlags bits.
static const uint kNeuralDebugGuardClamp = 1u << 0;  // Mark the pixels the ratio guard caught.
static const uint kNeuralDebugBroadBand = 1u << 1;   // Show the smooth half of the luminance edit.
static const uint kNeuralDebugDetailBand = 1u << 2;  // Show the remainder.
static const uint kNeuralDebugStats = 1u << 4;       // Accumulate the peak/clamp readback.

/**
 * Model encoding derived from constants: display gamma and vanilla Display-matched use 2.2; other
 * proxies use sRGB.
 *
 * @param vanillaGrading TransferParams.DisplayParam.x - the vanilla tonemap owns the frame.
 */
uint NeuralModelSpace(uint domain, uint proxyCurve, bool vanillaGrading)
{
	if (domain == kNeuralColorDomainDisplayGamma)
		return kNeuralModelSpaceGamma22;
	return (proxyCurve == kNeuralProxyDisplayMatched && vanillaGrading) ? kNeuralModelSpaceGamma22 : kNeuralModelSpaceSrgb;
}

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
 * Uniform proxy display transform built from transfer constants and adaptation inputs.
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
	float highlightWhite;    // Display gamma only: linear peak the display can show (>1 = HDR target); 0 = none.
	uint proxyCurve;         // kNeuralProxy*: how the scene-linear placements build the proxy.
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
	display.highlightWhite = 0.0;
	display.proxyCurve = kNeuralProxyDisplayMatched;
	return display;
}

/**
 * Resolve display transform constants and adaptation inputs. Unbound adaptation inputs read zero;
 * absent grading and exposure yield identity.
 *
 * @param displayParam x > 0.5: the vanilla tonemap owns the frame and its constants
 *                     were captured; y/z: ISHDR Param.y/.z.
 * @param displayCinematic ISHDR Cinematic (x saturation, z contrast, w brightness).
 * @param displayTint ISHDR Tint (xyz colour, w amount).
 * @param displayExposure x > 0.5: Post Processing auto exposure is active; y its
 *                        0.18 * compensation factor, zw its adaptation range.
 * @param vanillaAdaptation ISHDR AvgTex: x adapted luminance, y target luminance.
 * @param postProcessAdaptedLuminance Post Processing's adapted luminance.
 * @param highlightWhite TransferParams.HighlightWhite (see NeuralHighlightRolloff).
 * @param proxyCurve TransferParams.ProxyCurve (see kNeuralProxy*).
 */
NeuralDisplayTransform MakeNeuralDisplayTransform(float4 displayParam, float4 displayCinematic, float4 displayTint,
	float4 displayExposure, float2 vanillaAdaptation, float postProcessAdaptedLuminance, float highlightWhite,
	uint proxyCurve)
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
	display.highlightWhite = highlightWhite;
	display.proxyCurve = proxyCurve;
	return display;
}

/** Linear light of a colour stored in @p domain (open-ended; negatives clamp to zero). */
float3 NeuralDomainToLinear(float3 color, uint domain)
{
	color = max(color, 0.0);
	if (domain == kNeuralColorDomainDisplayGamma)
		return pow(color, kNeuralDisplayGamma);
	if (domain == kNeuralColorDomainSceneGamma)
		return pow(color, kNeuralSceneGamma);
	return color;
}

/** Inverse of NeuralDomainToLinear. */
float3 NeuralLinearToDomain(float3 color, uint domain)
{
	color = max(color, 0.0);
	if (domain == kNeuralColorDomainDisplayGamma)
		return pow(color, 1.0 / kNeuralDisplayGamma);
	if (domain == kNeuralColorDomainSceneGamma)
		return pow(color, 1.0 / kNeuralSceneGamma);
	return color;
}

/**
 * Model-space value to linear light, using @p space's curve (see NeuralModelSpace).
 *
 * Clamped, because the model was handed a 0-1 proxy and anything outside that range is not a
 * value it can have meant.
 */
float3 NeuralModelToLinear(float3 v, uint space)
{
	return space == kNeuralModelSpaceGamma22 ? pow(saturate(v), kNeuralDisplayGamma) : NeuralSrgbToLinear(v);
}

/** Linear light to the model-space encoding for @p space. */
float3 NeuralLinearToModel(float3 v, uint space)
{
	return space == kNeuralModelSpaceGamma22 ? pow(saturate(v), 1.0 / kNeuralDisplayGamma) : NeuralLinearToSrgb(v);
}

/** ISHDR's Hejl-Burgess-Dawson curve on one luminance value, linear out (GetTonemapFactorHejlBurgessDawson). */
float NeuralHejlBurgessDawson(float luminance, float whitePoint)
{
	float tmp = max(0.0, luminance - 0.004);
	float encoded = ((tmp * 6.2 + 0.5) * tmp) / (tmp * (tmp * 6.2 + 1.7) + 0.06);
	return whitePoint * NeuralSrgbToLinear(encoded.xxx).x;
}

/**
 * Krzysztof Narkowicz's ACES filmic fit (2016), applied to luminance as the fallback proxy tonemap.
 */
float NeuralAcesFilmic(float x)
{
	const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
	return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

/**
  * RenoDX Neutwo: c / sqrt(peak^2 + 1), a hue-preserving curve for exposed scene linear.
 */
float3 NeuralNeutwo(float3 c)
{
	float peak = max(c.r, max(c.g, c.b));
	return c * rsqrt(peak * peak + 1.0);
}

/**
 * Build a scene-linear proxy using captured ISHDR exposure, tonemap, and grading, or the hue-
 * preserving ACES fallback. Excludes bloom, fade, and HDR mapping; clamps to 0..1.
 */
float3 ApplyNeuralDisplayTransformExposed(float3 exposedColor, NeuralDisplayTransform display)
{
	float3 color = max(exposedColor, 0.0);
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

/** ApplyNeuralDisplayTransformExposed with the transform's own exposure applied first. */
float3 ApplyNeuralDisplayTransform(float3 linearColor, NeuralDisplayTransform display)
{
	return ApplyNeuralDisplayTransformExposed(max(linearColor, 0.0) * display.exposure, display);
}

/**
 * HDR proxy shoulder adapted from RenoDX/OptiScaler. Identity below the knee; extended Reinhard above
 * it, reaching one at the display peak with unit slope at the knee.
 *
 * @param peak Largest linear channel of the pixel.
 * @param white Display peak in the same units; must exceed one.
 * @return The rolled-off peak, in 0..1.
 */
float NeuralHighlightRolloff(float peak, float white)
{
	if (peak <= kNeuralHighlightKnee)
		return peak;
	const float headroom = 1.0 - kNeuralHighlightKnee;
	float t = (peak - kNeuralHighlightKnee) / headroom;
	float tWhite = (white - kNeuralHighlightKnee) / headroom;
	float shoulder = t * (1.0 + t / (tWhite * tWhite)) / (1.0 + t);
	return kNeuralHighlightKnee + headroom * min(shoulder, 1.0);
}

/**
 * Apply the selected scene curve. Exposure is explicit because scene-gamma input applies it before
 * decoding.
 */
float3 EncodeNeuralSceneCurve(float3 linearColor, float exposure, NeuralDisplayTransform display)
{
	// Per-channel Reinhard on the raw buffer: no exposure, no grading.
	if (display.proxyCurve == kNeuralProxyLegacy)
		return linearColor / (1.0 + linearColor);
	// RenoDX Neutwo: one hue-preserving scale driven by the peak channel.
	if (display.proxyCurve == kNeuralProxyNeutwo)
		return NeuralNeutwo(linearColor * exposure);
	return ApplyNeuralDisplayTransformExposed(max(linearColor, 0.0) * exposure, display);
}

/**
 * Decode finished gamma color to linear proxy space. Preserve SDR; compress HDR highlights with a hue-
 * preserving shoulder or peak normalization.
 */
float3 EncodeNeuralDisplayProxy(float3 color, NeuralDisplayTransform display)
{
	float3 linearColor = pow(color, kNeuralDisplayGamma);
	float peak = max(linearColor.r, max(linearColor.g, linearColor.b));
	float scale = display.highlightWhite > 1.0 ?
	                  NeuralHighlightRolloff(peak, display.highlightWhite) / max(peak, 1e-5) :
	                  1.0 / max(peak, 1.0);
	return linearColor * scale;
}

/**
 * Build the 0-1 linear-light proxy for the selected color domain and curve.
 */
float3 EncodeNeuralProxy(float3 color, uint domain, NeuralDisplayTransform display)
{
	color = max(color, 0.0);

	// Use one return: fxc emits X4000 for branch-local returns in this dispatch.
	float3 proxy = color;
	if (domain == kNeuralColorDomainDisplayGamma) {
		proxy = EncodeNeuralDisplayProxy(color, display);
	} else if (domain == kNeuralColorDomainSceneGamma) {
		// Match vanilla grading on gamma-encoded values; the result is display-encoded, not linear.
		if (display.proxyCurve == kNeuralProxyDisplayMatched && display.vanillaGrading) {
			proxy = pow(max(ApplyNeuralDisplayTransform(color, display), 0.0), kNeuralSceneGamma);
		} else {
			// Every other curve takes linear light. Exposure still belongs where the game
			// applies it, on the encoded values, so it goes on before the decode, not after.
			proxy = EncodeNeuralSceneCurve(pow(max(color * display.exposure, 0.0), kNeuralSceneGamma), 1.0, display);
		}
	} else {
		proxy = EncodeNeuralSceneCurve(color, display.exposure, display);
	}
	return proxy;
}

/**
 * Transform colour stored in @p domain into the proxy the model sees, encoded for @p space.
 */
float4 EncodeNeuralColor(float4 color, uint domain, uint space, NeuralDisplayTransform display)
{
	return float4(NeuralLinearToModel(EncodeNeuralProxy(color.rgb, domain, display), space), color.a);
}

/**
 * Nine-fetch Catmull-Rom reconstruction. Clamp taps to the active region and the result to the 2x2
 * neighborhood to prevent stale-margin reads and HDR ringing.
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
 * Raw four-tap Catmull-Rom axis weights, padded to six entries for combination with NeuralBoxAxis.
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
 * Exact-area box weights for one axis. Six taps cover the 0.25x minimum scale plus fractional extent
 * rounding.
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
 * Per-axis area filtering when minifying, following OptiScaler's DLSSNR fork
 * (https://github.com/Dagherbou/OptiScaler_DLSSNR/discussions/2). Native axes retain Catmull-Rom.
 * Clamp to the sampled range to prevent ringing.
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
	// The first tap seeds the neighbourhood range; every tap, including zero-weight pads, folds into it.
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
 * Map color pixels into the render-resolution guide raster and apply relative guide jitter. Keep the
 * position fractional for filtered lookups.
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
 * Depth-aware silhouette weighting from xenmods/DLSSNR-Cost-Scaler (MIT, see
 * DLSSNR-Cost-Scaler.MIT.LICENSE). Bilinear cross samples avoid guide-resolution
 * stepping; convert Reverse Z to conventional depth before measuring relative discontinuities. Strong
 * edges fade the edit toward one quarter.
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
#ifdef REVERSE_Z
	depthCentre = 1.0 - depthCentre;
	depthEast = 1.0 - depthEast;
	depthWest = 1.0 - depthWest;
	depthSouth = 1.0 - depthSouth;
	depthNorth = 1.0 - depthNorth;
#endif

	float minDepth = min(depthCentre, min(min(depthEast, depthWest), min(depthSouth, depthNorth)));
	float maxDepth = max(depthCentre, max(max(depthEast, depthWest), max(depthSouth, depthNorth)));
	float depthRange = (maxDepth - minDepth) / (maxDepth + 1e-4);
	if (depthRange <= 0.02)
		return 1.0;
	float edgeWeight = saturate(1.0 - (depthRange - 0.02) * 20.0);
	return lerp(0.25, 1.0, edgeWeight);
}

/**
 * Fade stale enhancements by comparing reprojected proxy color with fresh color under the same
 * display transform.
 */
float NeuralStaleEditWeight(float4 proxyColor, float4 originalColor, uint domain, uint space, NeuralDisplayTransform display)
{
	float3 stale = NeuralModelToLinear(proxyColor.rgb, space);
	float3 fresh = EncodeNeuralProxy(originalColor.rgb, domain, display);
	float staleLuma = dot(stale, kNeuralLuma);
	float freshLuma = dot(fresh, kNeuralLuma);
	float difference = dot(abs(stale - fresh), kNeuralLuma);
	return saturate(1.0 - (difference * 2.5) / (staleLuma + freshLuma + 0.05));
}

/**
 * Rec.709-weighted chroma magnitude; discounts offsets in low-luminance channels.
 */
float NeuralChromaMagnitude(float3 chroma)
{
	return sqrt(dot(chroma * chroma, kNeuralLuma));
}

/**
 * Luma-normalized color minus one. Gray is zero; adding this chroma offset preserves luminance.
 */
float3 NeuralChromaOffset(float3 color)
{
	float luma = dot(color, kNeuralLuma);
	return luma > 1e-5 ? color / luma - 1.0 : 0.0;
}

/** Everything ResolveNeuralColor needs for one pixel; see its documentation below. */
struct NeuralResolveInputs
{
	float4 modelColor;     // Feature 18's answer, in model space.
	float4 proxyColor;     // The exact proxy it was handed, same position, same space.
	float4 originalColor;  // The untouched frame pixel, stored in `domain`.
	float colorStrength;
	float editWeight;
	// Global x this pixel's per-category value, for each half of the luminance edit.
	float broadLuminosity;
	float detailLuminosity;
	float toneLow;     // Edge-aware blur of the log2 luminance edit, in stops.
	bool hasToneData;  // False: `toneLow` is unset and the edit is not split.
	uint domain;       // kNeuralColorDomain*: how originalColor and the result are stored.
	uint modelSpace;   // kNeuralModelSpace*: how modelColor and proxyColor are encoded.
	float hueGuardAmount;
	float maxRatio;
	bool modelFrameValid;  // False: the answer is empty this frame (NeuralModelFrameValid); pass the original through.
};

/** What the resolve measured on the way, for the debug views and the readback. */
struct NeuralResolveDebug
{
	float lowBand;    // Smooth half of the model's luminance edit, in stops.
	float highBand;   // The remainder.
	int clamped;      // 1 the guard capped a brighten, -1 a darken, 0 it did not bind.
	float modelLuma;  // Luminance of the model's answer, in linear model-space units.
};

/**
 * Transfer model/proxy luminance and chroma ratios to the original in linear light. Scale broad/detail
 * edit stops independently; equal strengths bypass band data. Clamp luminance after scaling when
 * enabled, and bound chroma ratios before and after extrapolation. Near-neutral hue guards preserve
 * the original hue axis. Encode the result in the original domain.
 */
float4 ResolveNeuralColor(NeuralResolveInputs inputs, out NeuralResolveDebug o_debug)
{
	o_debug = (NeuralResolveDebug)0;

	const uint domain = inputs.domain;
	const uint space = inputs.modelSpace;
	float maxRatio = max(inputs.maxRatio, 1.0);
	float colorStrength = inputs.colorStrength;
	float hueGuardAmount = inputs.hueGuardAmount;
	float3 original = NeuralDomainToLinear(inputs.originalColor.rgb, domain);
	float3 proxy = NeuralModelToLinear(inputs.proxyColor.rgb, space);
	float3 model = NeuralModelToLinear(inputs.modelColor.rgb, space);

	float proxyLuma = dot(proxy, kNeuralLuma);
	float modelLuma = dot(model, kNeuralLuma);
	o_debug.modelLuma = modelLuma;
	// Reject empty output per frame, not per pixel: black model pixels can represent valid shadows.
	if (!inputs.modelFrameValid)
		return float4(NeuralLinearToDomain(original, domain), inputs.originalColor.a);

	// Display gamma uses the same floor pedestal for original, proxy, and model, reproducing the model
	// exactly when original equals proxy. Scene domains retain a ratio floor because original and proxy
	// have different units.
	const float pedestal = domain == kNeuralColorDomainDisplayGamma ? kNeuralRatioFloor : 0.0;
	const float ratioFloor = kNeuralRatioFloor - pedestal;
	float shadowConfidence = pedestal > 0.0 ? 1.0 : smoothstep(kNeuralRatioFloor, 4.0 * kNeuralRatioFloor, min(proxyLuma, modelLuma));
	original += pedestal;
	proxy += pedestal;
	model += pedestal;
	proxyLuma += pedestal;
	modelLuma += pedestal;

	float editWeight = max(inputs.editWeight, 0.0);
	// Scale broad/detail edit stops separately. Equal strengths reduce to a single exponent without band
	// data.
	float delta = log2((modelLuma + ratioFloor) / (proxyLuma + ratioFloor));
	float lowBand = inputs.hasToneData ? inputs.toneLow : delta;
	float highBand = delta - lowBand;
	o_debug.lowBand = lowBand;
	o_debug.highBand = highBand;
	float tone = editWeight * (lowBand * max(inputs.broadLuminosity, 0.0) + highBand * max(inputs.detailLuminosity, 0.0));
	float unclampedRatio = exp2(tone);
	float ratio = clamp(unclampedRatio, 1.0 / maxRatio, maxRatio);
	o_debug.clamped = unclampedRatio > maxRatio ? 1 : (unclampedRatio < 1.0 / maxRatio ? -1 : 0);

	float3 luminanceResult = original * ratio;
	float targetLuma = dot(luminanceResult, kNeuralLuma);

	// Luma-normalised colour: a neutral is exactly one in every channel.
	float3 originalChroma = NeuralChromaOffset(original);
	float3 normalizedOriginal = 1.0 + originalChroma;
	float3 normalizedProxy = 1.0 + NeuralChromaOffset(proxy);
	float3 normalizedModel = 1.0 + NeuralChromaOffset(model);

	// Transfer chroma relative to the proxy so a model no-op preserves original colors.
	float3 chromaRatio = clamp(normalizedModel / max(normalizedProxy, 1e-3), kNeuralChromaRatioMin, kNeuralChromaRatioMax);
	// Exponentiate the chroma ratio by colorStrength, then re-clamp. Zero preserves original chroma; one
	// applies the model change.
	float3 scaledChromaRatio = clamp(pow(chromaRatio, max(colorStrength, 0.0)), kNeuralChromaRatioMin, kNeuralChromaRatioMax);
	float3 normalizedTarget = normalizedOriginal * scaledChromaRatio;
	normalizedTarget /= max(dot(normalizedTarget, kNeuralLuma), 1e-5);
	float3 targetChroma = normalizedTarget - 1.0;

	// For near-neutral sources, project model chroma onto the original hue axis without crossing neutral.
	// Release smoothly with source chroma and hueGuardAmount.
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

	// Fade unreliable normalized chroma near black. colorStrength is already applied to the target chroma;
	// zero makes both blend endpoints equal.
	float resolvedColorStrength = shadowConfidence * saturate(editWeight);

	// NeuralLinearToDomain clamps at zero: a darkening edit on a pixel darker than the proxy
	// it was measured against can land below the pedestal.
	float3 resolved = lerp(luminanceResult, fullColorResult, resolvedColorStrength) - pedestal;
	return float4(NeuralLinearToDomain(resolved, domain), inputs.originalColor.a);
}

#endif
