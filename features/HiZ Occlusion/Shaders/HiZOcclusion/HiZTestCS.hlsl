#include "Common/FrameBuffer.hlsli"

// =============================================================================
// HiZ Occlusion Test Compute Shader
// =============================================================================
// VERSION: 3.0
//
// RESULT CODES (written to VisibilityResults):
//   -3: Visible - depth test passed
//   -2: Visible - undecidable (crosses the near plane or the screen edge)
//   -1: Visible - invalid radius
//    0: Default/unprocessed
//    1: Outside the view (and the guard band for swept entries)
//    2: Occluded
// =============================================================================

struct TestEntry
{
	float4 Bounds;
	float4 Sweep;
};

Texture2D<float> HiZBuffer : register(t0);
StructuredBuffer<TestEntry> Entries : register(t1);
RWStructuredBuffer<uint> VisibilityResults : register(u0);

cbuffer HiZParams : register(b0)
{
	float4 HiZSettings;
	float4 overlaySettings;
	float4 overlayColorToggles;
	float3 CameraWorldPos;
	float MotionMargin;
	float2 BufferDim;
	float2 GuardBand;
};

static const uint RESULT_VISIBLE = 0xFFFFFFFD;
static const uint RESULT_UNDECIDED = 0xFFFFFFFE;
static const uint RESULT_INVALID = 0xFFFFFFFF;
static const uint RESULT_OUTSIDE = 1;
static const uint RESULT_OCCLUDED = 2;

static const uint SEGMENT_HIDDEN = 0;
static const uint SEGMENT_OUTSIDE = 1;
static const uint SEGMENT_VISIBLE = 2;
static const uint SEGMENT_UNDECIDED = 3;

static const uint SWEEP_SEGMENTS = 5;
static const float SWEEP_FRACTIONS[SWEEP_SEGMENTS + 1] = { 0.0, 0.0625, 0.125, 0.25, 0.5, 1.0 };

float LinearizeHiZDepth(float depth)
{
	return FrameBuffer::CameraProj[2][3] / (depth - FrameBuffer::CameraProj[2][2]);
}

float NearPlaneZ()
{
#ifdef REVERSE_Z
	return LinearizeHiZDepth(1.0);
#else
	return LinearizeHiZDepth(0.0);
#endif
}

float FarthestDepth(float a, float b)
{
#ifdef REVERSE_Z
	return min(a, b);
#else
	return max(a, b);
#endif
}

void ExtendRect(float3 centerVS, float radius, inout float2 uvMin, inout float2 uvMax)
{
	[unroll] for (uint i = 0; i < 8; ++i)
	{
		float3 corner = centerVS + float3((i & 1) ? radius : -radius, (i & 2) ? radius : -radius, (i & 4) ? radius : -radius);
		float2 uv = FrameBuffer::ViewToUV(corner, true);
		uvMin = min(uvMin, uv);
		uvMax = max(uvMax, uv);
	}
}

float SampleFarthestLinearDepth(float2 uvMin, float2 uvMax)
{
	uint width, height, levels;
	HiZBuffer.GetDimensions(0, width, height, levels);
	float2 pxMin = uvMin * float2(width, height);
	float2 pxMax = uvMax * float2(width, height);

	float extent = max(max(pxMax.x - pxMin.x, pxMax.y - pxMin.y), 1.0);
	int level = min((int)ceil(log2(extent)), (int)HiZSettings.x - 1);
	if (level > 0) {
		float finer = exp2(-(float)(level - 1));
		int2 span = int2(floor(pxMax * finer)) - int2(floor(pxMin * finer));
		if (all(span <= 1))
			level -= 1;
	}

	uint mipW, mipH, mipLevels;
	HiZBuffer.GetDimensions(level, mipW, mipH, mipLevels);
	int2 lastTexel = int2(mipW, mipH) - 1;
	float scale = exp2(-(float)level);
	int2 t0 = min(int2(floor(pxMin * scale)), lastTexel);
	int2 t1 = min(int2(floor(pxMax * scale)), lastTexel);

	float depth = HiZBuffer.Load(int3(t0, level));
	depth = FarthestDepth(depth, HiZBuffer.Load(int3(t1.x, t0.y, level)));
	depth = FarthestDepth(depth, HiZBuffer.Load(int3(t0.x, t1.y, level)));
	depth = FarthestDepth(depth, HiZBuffer.Load(int3(t1, level)));
	return LinearizeHiZDepth(depth);
}

uint TestSegment(float3 aVS, float3 bVS, float radius, bool swept, out float2 uvMin, out float2 uvMax)
{
	uvMin = 1e30;
	uvMax = -1e30;

	float zMin = min(aVS.z, bVS.z) - radius;
	float zMax = max(aVS.z, bVS.z) + radius;
	float nearZ = NearPlaneZ();
	if (zMax <= nearZ)
		return SEGMENT_OUTSIDE;
	if (zMin <= nearZ)
		return SEGMENT_UNDECIDED;

	ExtendRect(aVS, radius, uvMin, uvMax);
	if (swept)
		ExtendRect(bVS, radius, uvMin, uvMax);

	float2 band = swept ? GuardBand : 0.0;
	if (any(uvMax < -band) || any(uvMin > 1.0 + band))
		return SEGMENT_OUTSIDE;
	if (any(uvMin < 0.0) || any(uvMax > 1.0))
		return SEGMENT_UNDECIDED;
	if (zMin >= LinearizeHiZDepth(FrameBuffer::FarPlaneDepth()))
		return SEGMENT_UNDECIDED;

	float sceneZ = SampleFarthestLinearDepth(uvMin, uvMax);
	return zMin <= sceneZ * (1.0 + HiZSettings.y) ? SEGMENT_VISIBLE : SEGMENT_HIDDEN;
}

#ifdef ENABLE_DEBUG_OVERLAY

RWTexture2D<unorm float4> DebugOverlay : register(u2);

void DrawPixel(int2 p, uint baseW, uint baseH, float4 color)
{
	if ((p.x >= 0) && (p.x < (int)baseW) && (p.y >= 0) && (p.y < (int)baseH)) {
		DebugOverlay[p] = color;
	}
}

void DrawCross(int2 p, uint baseW, uint baseH, float4 color, int thickness)
{
	int halfSize = max(1, thickness * 3);
	[unroll] for (int i = -halfSize; i <= halfSize; ++i)
	{
		DrawPixel(int2(p.x + i, p.y), baseW, baseH, color);
		DrawPixel(int2(p.x, p.y + i), baseW, baseH, color);
	}
}

void DrawRectOutline(int2 minTex0, int2 maxTex0, uint baseW, uint baseH, float4 color, int thickness)
{
	int halfThickness = max(0, thickness >> 1);

	int width = maxTex0.x - minTex0.x;
	int height = maxTex0.y - minTex0.y;

	int horizPixels = width * (1 + 2 * halfThickness);
	int vertPixels = height * (1 + 2 * halfThickness);
	int totalPixels = 2 * horizPixels + 2 * vertPixels;

	int stride = max(1, totalPixels >> 9);

	for (int t = -halfThickness; t <= halfThickness; ++t) {
		for (int x = minTex0.x; x <= maxTex0.x; x += stride) {
			DrawPixel(int2(x, minTex0.y + t), baseW, baseH, color);
			DrawPixel(int2(x, maxTex0.y + t), baseW, baseH, color);
		}
		for (int y = minTex0.y; y <= maxTex0.y; y += stride) {
			DrawPixel(int2(minTex0.x + t, y), baseW, baseH, color);
			DrawPixel(int2(maxTex0.x + t, y), baseW, baseH, color);
		}
	}
}

void DrawBounds(uint geometryIndex, uint result, float2 uvMin, float2 uvMax, float distance)
{
	if (overlaySettings.x == 0 || geometryIndex >= (uint)overlaySettings.y || uvMin.x > uvMax.x)
		return;

	uint toggleBits = uint(overlayColorToggles.x);
	float4 color;
	if (result == RESULT_VISIBLE && (toggleBits & 1)) {
		color = float4(0, 1, 0, 1);
	} else if (result == RESULT_UNDECIDED && (toggleBits & 2)) {
		color = float4(0, 1, 0.5, 1);
	} else if (result == RESULT_INVALID && (toggleBits & 4)) {
		color = float4(0, 1, 1, 1);
	} else if (result == RESULT_OUTSIDE && (toggleBits & 8)) {
		color = float4(1, 0, 1, 1);
	} else if (result == RESULT_OCCLUDED && (toggleBits & 16)) {
		color = float4(1, 0, 0, 1);
	} else {
		return;
	}

	uint baseW, baseH, mipCount;
	HiZBuffer.GetDimensions(0, baseW, baseH, mipCount);
	float2 size = float2(baseW, baseH);

	int thickness = clamp(int(500 / max(distance, 1.0)), 1, 5);
	float2 clampedMin = saturate(uvMin);
	float2 clampedMax = saturate(uvMax);
	DrawCross(int2((clampedMin + clampedMax) * 0.5 * size), baseW, baseH, color, thickness);

	int2 minTex0 = clamp(int2(floor(clampedMin * size)), 0, int2(baseW, baseH) - 1);
	int2 maxTex0 = clamp(int2(floor(clampedMax * size)), 0, int2(baseW, baseH) - 1);
	DrawRectOutline(minTex0, maxTex0, baseW, baseH, color, thickness);
}

#endif  // ENABLE_DEBUG_OVERLAY

uint ResolveResult(TestEntry entry, out float2 uvMin, out float2 uvMax, out float distance)
{
	uvMin = 1e30;
	uvMax = -1e30;
	distance = 0.0;

	float radius = entry.Bounds.w;
	if (radius <= 0.0)
		return RESULT_INVALID;
	radius += MotionMargin;

	float3 centerVS = mul(FrameBuffer::CameraView, float4(entry.Bounds.xyz - CameraWorldPos, 1)).xyz;
	distance = length(centerVS);
	if (distance <= radius)
		return RESULT_UNDECIDED;

	float3 sweepVS = mul(FrameBuffer::CameraView, float4(entry.Sweep.xyz, 0)).xyz;
	bool swept = dot(entry.Sweep.xyz, entry.Sweep.xyz) > 0.0;

	if (!swept) {
		uint segment = TestSegment(centerVS, centerVS, radius, false, uvMin, uvMax);
		if (segment == SEGMENT_VISIBLE)
			return RESULT_VISIBLE;
		if (segment == SEGMENT_UNDECIDED)
			return RESULT_UNDECIDED;
		return segment == SEGMENT_HIDDEN ? RESULT_OCCLUDED : RESULT_OUTSIDE;
	}

	bool anyHidden = false;
	[unroll] for (uint i = 0; i < SWEEP_SEGMENTS; ++i)
	{
		float2 segMin, segMax;
		uint segment = TestSegment(centerVS + sweepVS * SWEEP_FRACTIONS[i], centerVS + sweepVS * SWEEP_FRACTIONS[i + 1], radius, true, segMin, segMax);
		if (i == 0) {
			uvMin = segMin;
			uvMax = segMax;
		}
		if (segment == SEGMENT_VISIBLE)
			return RESULT_VISIBLE;
		if (segment == SEGMENT_UNDECIDED)
			return RESULT_UNDECIDED;
		anyHidden = anyHidden || segment == SEGMENT_HIDDEN;
	}
	return anyHidden ? RESULT_OCCLUDED : RESULT_OUTSIDE;
}

[numthreads(256, 1, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	uint geometryIndex = dispatchThreadID.x;
	if (geometryIndex >= (uint)HiZSettings.z)
		return;

	float2 uvMin, uvMax;
	float distance;
	uint result = ResolveResult(Entries[geometryIndex], uvMin, uvMax, distance);
	VisibilityResults[geometryIndex] = result;

#ifdef ENABLE_DEBUG_OVERLAY
	DrawBounds(geometryIndex, result, uvMin, uvMax, distance);
#endif
}
