// HiZDownsampleCS.hlsl
// Downsamples mip N to mip N+1 keeping the farthest depth of each footprint for conservative occlusion culling.

Texture2D<float> SrcMip : register(t0);
RWTexture2D<float> DstMip : register(u0);

float FarthestDepth(float a, float b)
{
#ifdef REVERSE_Z
	return min(a, b);
#else
	return max(a, b);
#endif
}

[numthreads(16, 16, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	uint w, h;
	DstMip.GetDimensions(w, h);

	uint2 dst = dispatchThreadID.xy;
	if (dst.x >= w || dst.y >= h)
		return;

	uint sw, sh;
	SrcMip.GetDimensions(sw, sh);

	uint2 srcBase = dst * 2;
	uint2 srcLast = srcBase + 1;
	if (dst.x == w - 1 && (sw & 1))
		srcLast.x += 1;
	if (dst.y == h - 1 && (sh & 1))
		srcLast.y += 1;
	srcLast = min(srcLast, uint2(sw - 1, sh - 1));

	float farthest = SrcMip.Load(int3(srcBase, 0));
	for (uint y = srcBase.y; y <= srcLast.y; ++y) {
		for (uint x = srcBase.x; x <= srcLast.x; ++x) {
			farthest = FarthestDepth(farthest, SrcMip.Load(int3(x, y, 0)));
		}
	}

	DstMip[dst] = farthest;
}
