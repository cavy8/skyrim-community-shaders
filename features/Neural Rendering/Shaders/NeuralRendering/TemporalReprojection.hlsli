#ifndef NEURAL_TEMPORAL_REPROJECTION
#define NEURAL_TEMPORAL_REPROJECTION

/** Reject mismatched surfaces even when foreground and background have the same luminance. */
float NeuralTemporalSurfaceWeight(float currentDepth, uint currentCategory, float4 previousGuide)
{
	if (!all(isfinite(previousGuide)) || !isfinite(currentDepth) || currentCategory != (uint)previousGuide.w)
		return 0.0;
#ifdef REVERSE_Z
	float currentDistance = currentDepth;
	float previousDistance = previousGuide.z;
#else
	float currentDistance = 1.0 - currentDepth;
	float previousDistance = 1.0 - previousGuide.z;
#endif
	// Distance from the far plane is approximately reciprocal view depth. A relative
	// comparison tolerates small camera translations but rejects foreground/background swaps.
	float relativeDifference = abs(currentDistance - previousDistance) / max(max(currentDistance, previousDistance), 1e-6);
	return 1.0 - smoothstep(0.05, 0.15, relativeDifference);
}

/** Model and motion UVs are unjittered; stored guides retain their raster jitter. */
bool NeuralPreviousGuideTexel(float2 previousUV, uint2 guideSize, float2 previousJitter, out int2 texel)
{
	float2 position = previousUV * float2(guideSize) + previousJitter;
	texel = (int2)floor(position);
	return all(isfinite(previousUV)) && all(previousUV >= 0.0) && all(previousUV < 1.0) &&
	       all(position >= 0.0) && all(position < float2(guideSize));
}

#endif
