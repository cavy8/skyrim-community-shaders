#include "Upscaling/NeuralRendering/ColorTransfer.hlsli"

cbuffer TransferParams : register(b0)
{
	float2 JitterOffset;  // Sub-pixel projection offset of the source raster, in render pixels.
	float ColorStrength;
	float TransferStrength;
	uint2 ActiveSize;  // Valid region of SourceColor, in source texels.
	uint2 WorkSize;    // Model raster; DestinationColor is allocated at this size.
	uint2 GuideSize;   // Unused here; keeps the layout shared with DecodeColorCS.
	uint DepthAwareResolve;
	uint SkipFrame;
	uint PerCategoryStrengths;  // Unused here; layout shared with DecodeColorCS.
	float2 GuideJitterOffset;   // Unused here; layout shared with DecodeColorCS.
	uint ColorDomain;           // kNeuralColorDomain* - how SourceColor is encoded.
	float4 CategoryColorStrengths[2];     // Unused here; layout shared with DecodeColorCS.
	float4 CategoryTransferStrengths[2];  // Unused here; layout shared with DecodeColorCS.
	float4 DisplayParam;      // x: replicate the vanilla tonemap, y: ISHDR Param.y (white point), z: ISHDR Param.z (Hejl-Burgess-Dawson).
	float4 DisplayCinematic;  // ISHDR Cinematic: x saturation, z contrast, w brightness.
	float4 DisplayTint;       // ISHDR Tint: xyz colour, w amount.
	float4 DisplayExposure;   // x: apply Post Processing auto exposure, y: 0.18 * compensation, zw: adaptation range.
};

Texture2D<float4> SourceColor : register(t0);
// ISHDR's AvgTex from the previous frame's tonemap pass: x adapted luminance, y target luminance.
Texture2D<float2> VanillaAdaptation : register(t1);
// Post Processing's Histogram Auto Exposure adapted luminance (a single float).
StructuredBuffer<float> PostProcessAdaptation : register(t2);
RWTexture2D<float4> DestinationColor : register(u0);
SamplerState LinearClampSampler : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	uint width;
	uint height;
	uint sourceWidth;
	uint sourceHeight;
	DestinationColor.GetDimensions(width, height);
	SourceColor.GetDimensions(sourceWidth, sourceHeight);
	// The source may be a natively allocated game target whose valid region is
	// the top-left ActiveSize; the destination is compact at the model raster.
	uint2 active = min(ActiveSize, uint2(sourceWidth, sourceHeight));
	uint2 work = min(WorkSize, uint2(width, height));
	if (any(dispatchThreadID.xy >= work) || any(active == 0))
		return;

	// Each destination texel is an unjittered model pixel covering scene position
	// (id + 0.5) * active / work on the source grid. The raster holds that scene
	// position at + JitterOffset, so resample it from there. At native scale with
	// a zero offset this is exactly the source texel itself.
	//
	// footprint is the source-texel extent, per axis, that one destination texel
	// represents. An axis at or above source resolution (footprint <= 1: native
	// scale, or that axis is being supersampled) keeps the Catmull-Rom
	// reconstruction the jitter compensation already needed. An axis below
	// source resolution (footprint > 1) switches to an exact-area box average
	// instead, because reconstructing a single point there leaves source
	// frequencies above the model's new Nyquist limit free to alias into the
	// proxy as neural shimmer - see SampleNeuralSourceAreaMinify. Filtering is
	// per axis so an anisotropic scale (e.g. 0.65 x 0.85) only boxes the axis
	// that is actually shrinking.
	float2 footprint = float2(active) / float2(work);
	float2 scenePosition = (float2(dispatchThreadID.xy) + 0.5) * footprint;
	float2 position = scenePosition + JitterOffset;
	float3 color = any(footprint > 1.0)
		? SampleNeuralSourceAreaMinify(SourceColor, position, footprint, float2(active))
		: SampleNeuralSourceCatmullRom(SourceColor, LinearClampSampler, position, float2(active), float2(sourceWidth, sourceHeight));
	uint2 nearest = min(uint2(scenePosition), active - 1);
	float alpha = SourceColor[nearest].a;

	// The display transform the frame will go through after this placement, so
	// the scene-linear proxy the model sees is exposed and graded like the frame
	// the user will see (see ApplyNeuralDisplayTransform). The adaptation values
	// are uniform, so the texture centre stands for the whole target; an unbound
	// input reads zero and drops out of the transform.
	NeuralDisplayTransform display = MakeNeuralDisplayTransform(DisplayParam, DisplayCinematic, DisplayTint, DisplayExposure,
		VanillaAdaptation.SampleLevel(LinearClampSampler, float2(0.5, 0.5), 0), PostProcessAdaptation[0]);
	DestinationColor[dispatchThreadID.xy] = EncodeNeuralColor(float4(color, alpha), ColorDomain, display);
}
