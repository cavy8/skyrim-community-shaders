// =============================================================================
// NeckSeamFixCS.hlsl
//
// Detects and fills the 1-2 pixel seam that sometimes appears between Skyrim's
// head and body meshes.
//
// Algorithm
// ---------
// For each screen pixel that is NOT identified as skin (MASKS.x == 0):
//   1. Search a configurable square neighbourhood for skin pixels.
//   2. If any skin pixels are found, check whether the current pixel's depth
//      is close enough to the neighbours to be a genuine gap (not air).
//   3. If so, compute the average colour from the skin neighbours and blend it
//      into the main colour buffer.
//
// This effectively "inpaints" the thin gap with skin colour sampled from the
// geometry on either side, making the seam visually invisible.
//
// Register layout (must match NeckSeamFix.cpp)
//   t0  = DepthTexture       (current scene linear depth SRV, DXGI_FORMAT_R32_FLOAT or similar)
//   t1  = MaskTexture        (MASKS render target;  .x > 0 = SSS skin pixel)
//   u0  = MainRW             (MAIN render target UAV, read-modify-write)
//   b1  = NeckSeamCB         (SearchRadius, DepthThreshold, BlendStrength)
// =============================================================================

#include "Common/SharedData.hlsli"
#include "Common/Color.hlsli"

// ---------------------------------------------------------------------------
// Inputs / Outputs
// ---------------------------------------------------------------------------

Texture2D<float>  DepthTexture : register(t0);
Texture2D<float4> MaskTexture  : register(t1);

RWTexture2D<float4> MainRW : register(u0);

// ---------------------------------------------------------------------------
// Constant buffer  (b0 is reserved for per-frame global data)
// ---------------------------------------------------------------------------

cbuffer NeckSeamCB : register(b1)
{
	float SearchRadius;    // Maximum pixel offset to search (e.g. 2.0)
	float DepthThreshold;  // Max |depth_gap - depth_skin| in view-space units
	float BlendStrength;   // [0, 1]  how hard to blend the skin colour in
	float pad;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Returns the linearised view-space depth for pixel integer coords.
float GetLinearDepth(int2 coord)
{
	float rawDepth = DepthTexture.Load(int3(coord, 0)).x;
	return SharedData::GetScreenDepth(rawDepth);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	uint2 bufDim = uint2(SharedData::BufferDim.xy);

	if (any(DTid.xy >= bufDim))
		return;

	int2 pixCoord = int2(DTid.xy);

	// -----------------------------------------------------------------------
	// Early-out: if this pixel is already skin, there is nothing to fill.
	// -----------------------------------------------------------------------
	float centerSkinMask = MaskTexture.Load(int3(pixCoord, 0)).x;
	if (centerSkinMask > 1e-4f)
		return;

	// -----------------------------------------------------------------------
	// Similarly skip sky / background pixels (depth == 1.0 means no geometry).
	// Using the raw depth buffer: 1.0 (or very close to the far plane) = no hit.
	// -----------------------------------------------------------------------
	float rawCenter = DepthTexture.Load(int3(pixCoord, 0)).x;
	if (rawCenter >= 0.9999f)
		return;

	float linearCenter = SharedData::GetScreenDepth(rawCenter);

	// -----------------------------------------------------------------------
	// Search neighbourhood for skin pixels.
	// We accumulate colour from all skin neighbours whose depth is close to
	// our own depth, then blend the average in.
	// -----------------------------------------------------------------------

	int radius = max(1, (int)round(SearchRadius));

	float3 accumulatedColor = 0.0f;
	float  accumWeight      = 0.0f;

	for (int dy = -radius; dy <= radius; ++dy)
	{
		for (int dx = -radius; dx <= radius; ++dx)
		{
			if (dx == 0 && dy == 0)
				continue;

			int2 sampleCoord = pixCoord + int2(dx, dy);

			// Clamp to screen bounds
			sampleCoord = clamp(sampleCoord, int2(0, 0), int2(bufDim) - 1);

			float neighbourSkin = MaskTexture.Load(int3(sampleCoord, 0)).x;
			if (neighbourSkin <= 1e-4f)
				continue;

			// Depth proximity test — rejects neighbours on the far side of a wall etc.
			float linearNeighbour = GetLinearDepth(sampleCoord);
			float depthDiff = abs(linearNeighbour - linearCenter);
			if (depthDiff > DepthThreshold)
				continue;

			// Weight: closer neighbours contribute more.
			float dist   = length(float2(dx, dy));
			float weight = 1.0f / max(dist, 0.001f);

			float3 neighbourColor = MainRW[sampleCoord].rgb;
			accumulatedColor += neighbourColor * weight;
			accumWeight      += weight;
		}
	}

	// -----------------------------------------------------------------------
	// If we found at least one eligible skin neighbour, fill the gap.
	// -----------------------------------------------------------------------
	if (accumWeight > 0.0f)
	{
		float3 fillColor    = accumulatedColor / accumWeight;
		float4 currentPixel = MainRW[pixCoord];

		// Preserve the original alpha (motion vector alpha stores geometry flag)
		MainRW[pixCoord] = float4(lerp(currentPixel.rgb, fillColor, BlendStrength), currentPixel.a);
	}
}
