// HiZBuildLevel0CS.hlsl
// Copies the post z-prepass depth buffer into Hi-Z pyramid mip 0.

#ifdef REVERSE_Z
Texture2D<float> DepthTex : register(t0);
#else
Texture2D<unorm float> DepthTex : register(t0);
#endif
RWTexture2D<float> OutMip0 : register(u0);

[numthreads(16, 16, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	uint w, h;
	OutMip0.GetDimensions(w, h);

	uint2 pix = dispatchThreadID.xy;
	if (pix.x >= w || pix.y >= h)
		return;

	float d = DepthTex.Load(int3(pix, 0));
	OutMip0[pix] = d;
}
