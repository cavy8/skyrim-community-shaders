#ifndef UPSCALING_NEURALRENDERING_TRANSFERPARAMS
#define UPSCALING_NEURALRENDERING_TRANSFERPARAMS

// Shared b0 layout for transfer passes. Must match TransferParams in Backend.cpp, including fields
// unused by individual passes.

cbuffer TransferParams : register(b0)
{
	float2 JitterOffset;  // Sub-pixel projection offset of the colour raster, in render pixels.
	float ColorStrength;
	float TransferStrength;    // Overall edit weight (0 = untouched frame, 1 = the model's change, 2 = doubled).
	uint2 ActiveSize;          // Valid region of the colour/output rasters, in their texels.
	uint2 WorkSize;            // Model raster; the model, proxy and band textures are allocated at this size.
	uint2 GuideSize;           // Valid region of the depth/motion/category guides (render resolution).
	uint DepthAwareResolve;    // Non-zero: fade the edit across depth silhouettes (see NeuralSilhouetteWeight).
	uint StaleAnswer;          // Non-zero: the model answer is the previous frame's; reproject it through MotionVectors.
	uint HueGuardMask;         // Bit i set: category i (NeuralRenderingCategories) hue-guards its chroma change.
	float2 GuideJitterOffset;  // Projection offset of the guide rasters relative to the colour raster, in guide texels.
	uint ColorDomain;          // kNeuralColorDomain* - how the colour input and output are encoded.
	float4 CategoryColorStrengths[2];
	float4 CategoryTransferStrengths[2];
	float4 CategoryBroadLuminosity[2];
	float4 CategoryDetailLuminosity[2];
	float4 DisplayParam;      // x: replicate the vanilla tonemap, y: ISHDR Param.y (white point), z: ISHDR Param.z (Hejl-Burgess-Dawson).
	float4 DisplayCinematic;  // ISHDR Cinematic: x saturation, z contrast, w brightness.
	float4 DisplayTint;       // ISHDR Tint: xyz colour, w amount.
	float4 DisplayExposure;   // x: apply Post Processing auto exposure, y: 0.18 * compensation, zw: adaptation range.
	float BroadLuminosity;    // Multiplier on the smooth half of the model's luminance change.
	uint DebugCategoryView;   // Non-zero: render the classified category instead of the model's edit.
	float MaxRatio;           // Two-sided guard on the model/proxy luminance ratio (1/MaxRatio..MaxRatio).
	uint RawModelOutput;      // Non-zero: write Feature 18's answer directly, bypassing the resolve (Finished Image diagnostic).
	float HighlightWhite;     // Display gamma: display peak for the HDR highlight shoulder (NeuralHighlightRolloff); 0 = none.
	float WipePosition;       // Split-screen comparison: split as a fraction of the active width; negative = off.
	uint ProxyCurve;          // kNeuralProxy* - how the scene-linear placements build the proxy.
	uint DebugFlags;          // kNeuralDebug* bits.
	// x: Detail Luminosity, y: band radius in model texels, z: non-zero when the band textures
	// hold data for this evaluation, w: spare.
	float4 BandParams;
};

/** The model-space encoding this evaluation uses (see ColorTransfer.hlsli, NeuralModelSpace). */
uint NeuralTransferModelSpace()
{
	return NeuralModelSpace(ColorDomain, ProxyCurve, DisplayParam.x > 0.5);
}

/** Whether the band textures hold a filtered edit for this evaluation. */
bool NeuralTransferHasToneData()
{
	return BandParams.z > 0.5;
}

#endif
