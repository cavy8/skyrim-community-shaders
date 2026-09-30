#include "NeuralRendering/ColorTransfer.hlsli"
#include "NeuralRendering/TransferParams.hlsli"

// One axis of the edge-aware blur that separates the model's luminance edit into its smooth
// and detail halves. Dispatched twice per evaluation, horizontal then vertical; VERTICAL picks
// the axis. Separable bilateral filtering is an approximation - the two passes do not compose
// into a true 2D bilateral kernel - but the thing being filtered is a smooth edit map, not an
// image, and the error it makes is far below what the strengths on either side of the split
// are used to express.
//
// Both passes carry the proxy's log luminance through unchanged in x, because the second pass
// needs it for its own edge weights. Only y is filtered; the resolve reads y.

#ifdef VERTICAL
static const int2 kNeuralBandStep = int2(0, 1);
#else
static const int2 kNeuralBandStep = int2(1, 0);
#endif

// Taps per pass. The stride below stretches (or shrinks) them over the requested radius rather
// than adding more of them, so the cost is fixed whatever Band Radius is set to.
static const int kNeuralBandTaps = 17;
static const int kNeuralBandHalfTaps = kNeuralBandTaps / 2;
// Gaussian falloff across the tap range, in tap units: the outermost tap keeps about 4% of the
// centre's weight, so the kernel is effectively finite without a hard cut-off.
static const float kNeuralBandSigma = 4.0;
// Edge weight is exp(-sharpness * |change in log2 proxy luminance|). Two is Open Shaders'
// value; three to four is tighter, for content where the detail band shows halos.
static const float kNeuralBandEdgeSharpness = 2.0;

Texture2D<float2> SourceToneData : register(t0);
RWTexture2D<float2> DestinationToneData : register(u0);
SamplerState LinearClampSampler : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	uint width;
	uint height;
	DestinationToneData.GetDimensions(width, height);
	uint2 work = min(WorkSize, uint2(width, height));
	if (any(dispatchThreadID.xy >= work) || any(work == 0))
		return;

	int2 centre = int2(dispatchThreadID.xy);
	int2 limit = int2(work) - 1;
	float2 centreData = SourceToneData[centre];

	// Band Radius is in model texels at native scale, so it has to shrink with the model
	// raster to cover the same part of the screen. ActiveSize is the colour region the model
	// raster stands for; their ratio is the scale.
	float2 modelScale = float2(work) / max(float2(ActiveSize), 1.0);
#ifdef VERTICAL
	float axisScale = modelScale.y;
#else
	float axisScale = modelScale.x;
#endif
	float radius = max(BandParams.y * axisScale, 1.0);
	// Fractional, and read through the bilinear sampler: a whole-texel stride would quantise
	// the reach to multiples of kNeuralBandHalfTaps texels, leaving most of the Band Radius
	// range - and the scaling with the model raster - without any effect.
	float stride = radius / (float)kNeuralBandHalfTaps;

	float2 centrePosition = float2(centre) + 0.5;
	float2 inverseSize = 1.0 / float2(width, height);
	float2 lowerPosition = 0.5;
	float2 upperPosition = float2(limit) + 0.5;

	float total = 0.0;
	float weightSum = 0.0;
	[unroll]
	for (int tap = -kNeuralBandHalfTaps; tap <= kNeuralBandHalfTaps; ++tap) {
		float2 position = clamp(centrePosition + float2(kNeuralBandStep) * (float(tap) * stride), lowerPosition, upperPosition);
		float2 data = SourceToneData.SampleLevel(LinearClampSampler, position * inverseSize, 0);
		float spatial = exp(-0.5 * (float(tap) * float(tap)) / (kNeuralBandSigma * kNeuralBandSigma));
		float edge = exp(-kNeuralBandEdgeSharpness * abs(data.x - centreData.x));
		float weight = spatial * edge;
		total += weight * data.y;
		weightSum += weight;
	}

	DestinationToneData[dispatchThreadID.xy] = float2(centreData.x, total / max(weightSum, 1e-5));
}
