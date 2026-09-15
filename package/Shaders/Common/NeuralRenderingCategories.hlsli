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

	static const uint CategoryMask = 0x7;
	static const float R16UnormMaximum = 65535.0;

	/**
	 * Stores a material category in the low three bits of the R16_UNORM Masks2
	 * target while preserving the upper thirteen bits of its vertex-AO value.
	 */
	float Pack(float vertexAOStorage, uint category)
	{
		uint packed = (uint)round(saturate(vertexAOStorage) * R16UnormMaximum);
		packed = (packed & ~CategoryMask) | min(category, CategoryMask);
		return packed / R16UnormMaximum;
	}

	/** Extracts the material category written by Pack(). */
	uint Unpack(float packedVertexAO)
	{
		uint packed = (uint)round(saturate(packedVertexAO) * R16UnormMaximum);
		return packed & CategoryMask;
	}

	/**
	 * Fixed, maximally-distinguishable colour for each category, used by the Neural Rendering
	 * "Show Material Categories" debug view (DecodeColorCS) to render the classification itself
	 * rather than its resolved per-category strengths. Categories 7 (never written by Pack, which
	 * clamps to CategoryMask) and above fall back to EverythingElse.
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
