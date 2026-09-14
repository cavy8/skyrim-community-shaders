#ifndef COMMON_NEURAL_RENDERING_CATEGORIES
#define COMMON_NEURAL_RENDERING_CATEGORIES

namespace NeuralRenderingCategories
{
	static const uint EverythingElse = 0;
	static const uint Skin = 1;
	static const uint Hair = 2;
	static const uint Eyes = 3;
	static const uint Foliage = 4;
	static const uint Landscape = 5;
	static const uint Equipment = 6;

	static const float StorageScale = 255.0;

	/**
	 * Stores a material category in Masks2's G channel (R keeps upstream's vertex AO).
	 * k / 255 is exact in the R16G16_UNORM target, since 65535 = 255 * 257.
	 */
	float Encode(uint category)
	{
		return min(category, 255u) / StorageScale;
	}

	/** Extracts the material category written by Encode(). */
	uint Decode(float stored)
	{
		return (uint)round(saturate(stored) * StorageScale);
	}

	/**
	 * Fixed, maximally-distinguishable colour for each category, used by the Neural Rendering
	 * "Show Material Categories" debug view (DecodeColorCS) to render the classification itself
	 * rather than its resolved per-category strengths. Unknown categories (7 and above) fall back
	 * to EverythingElse.
	 */
	float3 DebugColor(uint category)
	{
		switch (category) {
		case Skin:
			return float3(1.0, 0.0, 0.0);
		case Hair:
			return float3(1.0, 0.5, 0.0);
		case Eyes:
			return float3(1.0, 1.0, 0.0);
		case Foliage:
			return float3(0.0, 1.0, 0.0);
		case Landscape:
			return float3(0.0, 1.0, 1.0);
		case Equipment:
			return float3(0.5, 0.0, 1.0);
		default:
			return float3(0.05, 0.05, 0.05);
		}
	}
}

#endif
