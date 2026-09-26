RWTexture2D<float> BlendedDepthTexture : register(u0);
RWTexture2D<unorm float> BlendedDepthTexture16 : register(u1);

#ifdef REVERSE_Z
Texture2D<float> MainDepthTexture : register(t0);
Texture2D<float> TerrainDepthTexture : register(t1);
#else
Texture2D<unorm float> MainDepthTexture : register(t0);
Texture2D<unorm float> TerrainDepthTexture : register(t1);
#endif

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
#ifdef REVERSE_Z
	float mixedDepth = max(MainDepthTexture[DTid.xy], TerrainDepthTexture[DTid.xy]);
#else
	float mixedDepth = min(MainDepthTexture[DTid.xy], TerrainDepthTexture[DTid.xy]);
#endif
	BlendedDepthTexture[DTid.xy] = mixedDepth;
	BlendedDepthTexture16[DTid.xy] = mixedDepth;
}
