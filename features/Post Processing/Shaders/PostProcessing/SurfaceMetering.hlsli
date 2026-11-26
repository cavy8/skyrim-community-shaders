#ifndef POSTPROCESSING_SURFACE_METERING_HLSLI
#define POSTPROCESSING_SURFACE_METERING_HLSLI

#include "Common/Color.hlsli"
#include "Common/ColorManagement.hlsli"
#include "Common/FrameBuffer.hlsli"

namespace SurfaceMetering
{
	/// Stops by which a surface paler than referenceAlbedo out-reflects it; 0 for anything darker and for sky.
	/// referenceAlbedo is the linear albedo of ordinary ground. Metering a pale surface this many stops lower
	/// makes a snow field read like any other ground under the same light.
	float BrightSurfaceStops(Texture2D<unorm float4> texAlbedo, Texture2D<float> texDepth, float2 uv, float2 gbufferScale, float referenceAlbedo, out bool isSky)
	{
		uint2 size;
		texAlbedo.GetDimensions(size.x, size.y);
		uint2 coord = min(uint2(saturate(uv) * gbufferScale * size), size - 1);
		isSky = FrameBuffer::ToStandardDepth(texDepth[coord]) >= 1.0;
		if (isSky)
			return 0.0;
		float albedo = Color::RGBToLuminance(ColorManagement::SceneToLinear(texAlbedo[coord].rgb));
		referenceAlbedo = max(referenceAlbedo, 1e-3);
		return log2(max(albedo, referenceAlbedo) / referenceAlbedo);
	}
}

#endif
