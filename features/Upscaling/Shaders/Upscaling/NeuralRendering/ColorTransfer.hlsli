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
// EncodeNeuralColor brings the frame into that display-referred domain and
// DecodeNeuralColor is its exact inverse, so when the model returns its input
// unchanged the round trip is the identity and the frame is unaffected. The
// operator is a per-channel Reinhard curve (x / (1 + x)) followed by the sRGB
// transfer function; both are invertible in closed form. The decode clamps the
// model's answer just below 1.0 before inverting the curve so a blown highlight
// stays a bright highlight instead of resolving to infinity.

static const float kNeuralDecodeCeiling = 0.99987793;  // 1 - 2^-13; caps the reconstructed HDR ratio near 8192x.

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
	float3 linearColor = max(color.rgb, 0.0);
	float3 tonemapped = linearColor / (1.0 + linearColor);  // per-channel Reinhard, maps [0, inf) -> [0, 1)
	return float4(NeuralLinearToSrgb(tonemapped), color.a);
}

/**
 * Inverse of EncodeNeuralColor, applied to the Feature 18 output.
 */
float4 DecodeNeuralColor(float4 color)
{
	float3 tonemapped = min(NeuralSrgbToLinear(color.rgb), kNeuralDecodeCeiling);
	float3 linearColor = tonemapped / (1.0 - tonemapped);  // inverse Reinhard, back to linear HDR
	return float4(linearColor, color.a);
}

#endif
