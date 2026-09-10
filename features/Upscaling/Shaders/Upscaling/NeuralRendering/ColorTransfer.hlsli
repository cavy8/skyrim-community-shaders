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
 * Compose the Feature 18 answer onto the untouched scene colour.
 *
 * A model no-op is an exact no-op apart from normal texture precision: its
 * luminance matches the reconstructed proxy, making the ratio one.  The common
 * floor makes the ratio converge smoothly to one in deep shadow, where a tiny
 * absolute model change would otherwise become an unbounded relative change.
 * A two-sided guard limits both flashes and sudden collapses without clipping
 * individual RGB channels.
 */
float4 ResolveNeuralColor(float4 modelColor, float4 originalColor, float colorStrength)
{
	float3 original = max(originalColor.rgb, 0.0);
	float3 proxy = EncodeNeuralProxy(original);
	float3 model = NeuralSrgbToLinear(modelColor.rgb);

	float proxyLuma = dot(proxy, kNeuralLuma);
	float modelLuma = dot(model, kNeuralLuma);
	// Some incompatible model/runtime combinations return an empty or invalid
	// frame. Treat that as no edit instead of turning a transient failure into a
	// half-bright flash through the lower ratio guard.
	if (!(modelLuma > 1e-5))
		return float4(original, originalColor.a);

	float ratio = (modelLuma + kNeuralRatioFloor) / (proxyLuma + kNeuralRatioFloor);
	ratio = clamp(ratio, 1.0 / kNeuralMaxRatio, kNeuralMaxRatio);

	float3 luminanceResult = original * ratio;
	float targetLuma = dot(luminanceResult, kNeuralLuma);

	// One positive scale brings the model's complete RGB chromaticity to the
	// guarded scene luminance. Because the encoded proxy is a scalar multiple of
	// the original, model == proxy reconstructs the original exactly.
	float3 fullColorResult = model * (targetLuma / max(modelLuma, 1e-5));

	// Normalized colour is unreliable only near black. Fade the chroma there,
	// while allowing the complete model palette everywhere with meaningful light.
	float shadowConfidence = smoothstep(kNeuralRatioFloor, 4.0 * kNeuralRatioFloor,
		min(proxyLuma, modelLuma));
	float resolvedColorStrength = saturate(colorStrength) * shadowConfidence;

	return float4(lerp(luminanceResult, fullColorResult, resolvedColorStrength), originalColor.a);
}

#endif
