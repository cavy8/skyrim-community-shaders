#ifndef UPSCALING_NEURALRENDERING_COLORTRANSFER
#define UPSCALING_NEURALRENDERING_COLORTRANSFER

// Colour transfer used to move scene colour into and out of the DLSS Neural
// Rendering (NGX Feature 18) shared textures.
//
// Both Community Shaders placements run Neural Rendering *before* tonemapping,
// so scene colour routinely exceeds 1.0, while Feature 18 is created without an
// HDR flag. The transfer therefore exists as a dedicated pair of compute passes
// rather than a CopyResource, so a normalisation on the way in and its exact
// inverse on the way out can be introduced without restructuring the pipeline.
//
// The pair below is deliberately the identity today: no colour transform has
// been validated against a working runtime yet. Whenever EncodeNeuralColor is
// changed, DecodeNeuralColor must be changed to its exact inverse.

/**
 * Transform scene colour into the space handed to Feature 18.
 */
float4 EncodeNeuralColor(float4 color)
{
	return color;
}

/**
 * Inverse of EncodeNeuralColor, applied to the Feature 18 output.
 */
float4 DecodeNeuralColor(float4 color)
{
	return color;
}

#endif
