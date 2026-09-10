#ifndef UPSCALING_NEURALRENDERING_COLORTRANSFER
#define UPSCALING_NEURALRENDERING_COLORTRANSFER

// Colour transfer used to move scene colour into and out of the DLSS Neural
// Rendering (NGX Feature 18) shared textures.
//
// Both Community Shaders placements run Neural Rendering *before* the game's
// tonemapper, so the colour handed in is linear and open-ended - routinely well
// above 1.0 on skies, speculars and emissives. Feature 18 is created without an
// HDR flag and was trained on ordinary display-referred (tone-mapped, sRGB) SDR
// frames. Handing it raw linear HDR leaves it re-deciding those out-of-range
// pixels every frame with nothing anchoring them, which reads as shimmer and
// flicker on exactly the bright regions.
//
// EncodeNeuralColor brings the frame into that display-referred domain. The
// model's answer is deliberately not inverse-tonemapped.  The derivative of
// inverse Reinhard is 1 / (1 - x)^2, so tiny frame-to-frame changes near white
// used to become enormous scene-linear shading changes.  Instead the resolve
// measures the model's bounded luminance change in proxy space, then restores
// its full chromaticity at that guarded scene luminance. The proxy compression
// uses one RGB scale so it does not distort hue before the model sees it. HDR
// headroom remains renderer-owned and every frame is re-anchored to deterministic
// scene colour rather than to model history.
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
// JitterOffset follows the Streamline convention: it is the sub-pixel offset
// (in render pixels) the projection applied, so a scene point that projects to
// unjittered pixel position u lands in the raster at u + JitterOffset.

static const float3 kNeuralLuma = float3(0.2126, 0.7152, 0.0722);
static const float kNeuralRatioFloor = 1.0 / 512.0;
static const float kNeuralMaxRatio = 2.0;

float3 EncodeNeuralProxy(float3 color)
{
	color = max(color, 0.0);
	float peak = max(color.r, max(color.g, color.b));
	return color / (1.0 + peak);  // Hue-preserving scalar Reinhard; every channel remains below one.
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
 * Transform linear-HDR scene colour into the display-referred proxy the model sees.
 */
float4 EncodeNeuralColor(float4 color)
{
	return float4(NeuralLinearToSrgb(EncodeNeuralProxy(color.rgb)), color.a);
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
 * Depth-aware silhouette weight for a full-resolution pixel (from
 * DLSSNR-Cost-Scaler's "Depth-Aware Bilateral Silhouette Preservation").
 *
 * When the model runs below the colour resolution its edit is upsampled
 * bilinearly, so at a geometric silhouette the background's edit bleeds a
 * texel or two into the thin foreground and vice versa. This measures the
 * relative depth range of the five-texel cross around the pixel's guide texel
 * and fades the edit towards a quarter across strong discontinuities, leaving
 * flat interiors untouched.
 *
 * @param guideDepth Game depth (the guide the model received), any allocation.
 * @param guideTexel Guide texel this colour pixel maps to.
 * @param guideSize Valid guide region in texels.
 */
float NeuralSilhouetteWeight(Texture2D<float> guideDepth, int2 guideTexel, uint2 guideSize)
{
	uint allocationWidth;
	uint allocationHeight;
	guideDepth.GetDimensions(allocationWidth, allocationHeight);
	int2 maxTexel = int2(min(guideSize, uint2(allocationWidth, allocationHeight))) - 1;
	if (any(maxTexel < 0))
		return 1.0;
	int2 centre = clamp(guideTexel, int2(0, 0), maxTexel);
	float depthCentre = guideDepth.Load(int3(centre, 0));
	float depthEast = guideDepth.Load(int3(min(centre.x + 1, maxTexel.x), centre.y, 0));
	float depthWest = guideDepth.Load(int3(max(centre.x - 1, 0), centre.y, 0));
	float depthSouth = guideDepth.Load(int3(centre.x, min(centre.y + 1, maxTexel.y), 0));
	float depthNorth = guideDepth.Load(int3(centre.x, max(centre.y - 1, 0), 0));

	float minDepth = min(depthCentre, min(min(depthEast, depthWest), min(depthSouth, depthNorth)));
	float maxDepth = max(depthCentre, max(max(depthEast, depthWest), max(depthSouth, depthNorth)));
	float depthRange = (maxDepth - minDepth) / (maxDepth + 1e-4);
	if (depthRange <= 0.02)
		return 1.0;
	float edgeWeight = saturate(1.0 - (depthRange - 0.02) * 20.0);
	return lerp(0.25, 1.0, edgeWeight);
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
 * @p editWeight scales the edit as a whole (the proxy's "transfer strength"):
 * the luminance ratio is raised to it, so zero is the untouched frame, one is
 * exactly the model's relative change and two doubles it in log space, and the
 * two-sided guard clamps after scaling so a weight above one cannot escape it.
 * Chroma follows the same weight, saturated, on top of @p colorStrength.
 */
float4 ResolveNeuralColor(float4 modelColor, float4 proxyColor, float4 originalColor, float colorStrength,
	float editWeight)
{
	float3 original = max(originalColor.rgb, 0.0);
	float3 proxy = NeuralSrgbToLinear(proxyColor.rgb);
	float3 model = NeuralSrgbToLinear(modelColor.rgb);

	float proxyLuma = dot(proxy, kNeuralLuma);
	float modelLuma = dot(model, kNeuralLuma);
	// Some incompatible model/runtime combinations return an empty or invalid
	// frame. Treat that as no edit instead of turning a transient failure into a
	// half-bright flash through the lower ratio guard.
	if (!(modelLuma > 1e-5))
		return float4(original, originalColor.a);

	editWeight = max(editWeight, 0.0);
	float ratio = (modelLuma + kNeuralRatioFloor) / (proxyLuma + kNeuralRatioFloor);
	ratio = clamp(pow(ratio, editWeight), 1.0 / kNeuralMaxRatio, kNeuralMaxRatio);

	float3 luminanceResult = original * ratio;
	float targetLuma = dot(luminanceResult, kNeuralLuma);

	// One positive scale brings the model's complete RGB chromaticity to the
	// guarded scene luminance. Because the proxy is a scalar multiple of the
	// original, model == proxy reconstructs the original exactly.
	float3 fullColorResult = model * (targetLuma / max(modelLuma, 1e-5));

	// Normalized colour is unreliable only near black. Fade the chroma there,
	// while allowing the complete model palette everywhere with meaningful light.
	float shadowConfidence = smoothstep(kNeuralRatioFloor, 4.0 * kNeuralRatioFloor,
		min(proxyLuma, modelLuma));
	float resolvedColorStrength = saturate(colorStrength) * shadowConfidence * saturate(editWeight);

	return float4(lerp(luminanceResult, fullColorResult, resolvedColorStrength), originalColor.a);
}

#endif
