#include "NeuralRendering/ColorTransfer.hlsli"
#include "NeuralRendering/TransferParams.hlsli"

// Separable bilateral blur of edit stops (y), preserving proxy log luminance (x) for edge weights.
// Dispatch horizontal then vertical.

#ifdef VERTICAL
static const int2 kNeuralBandStep = int2(0, 1);
#else
static const int2 kNeuralBandStep = int2(1, 0);
#endif

// Fixed tap count; stride adapts to Band Radius.
static const int kNeuralBandTaps = 17;
static const int kNeuralBandHalfTaps = kNeuralBandTaps / 2;
// Gaussian sigma in tap units; outer taps retain about 4% of the center weight.
static const float kNeuralBandSigma = 4.0;
// Edge weight: exp(-sharpness * abs(delta log luminance)).
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

	// Band Radius is in model texels at native scale; scale it by the WorkSize/ActiveSize ratio to cover
	// the same screen area.
	float2 modelScale = float2(work) / max(float2(ActiveSize), 1.0);
#ifdef VERTICAL
	float axisScale = modelScale.y;
#else
	float axisScale = modelScale.x;
#endif
	float radius = max(BandParams.y * axisScale, 1.0);
	// Use fractional bilinear taps so radius and model-scale changes are not quantized to whole texels.
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
