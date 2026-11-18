#ifndef __FUR_SHELLS_HLSLI__
#define __FUR_SHELLS_HLSLI__

namespace FurShells
{
	cbuffer FurShellData : register(b13)
	{
		float Length;
		float ShellCount;
		float Droop;
		float RootThreshold;
		float TipThreshold;
		float RootDarkening;
		float ShellColor;
		float RootTest;
	};

#if defined(PSHADER)
	Texture2D<float4> TexShell : register(t122);
	Texture2D<float4> TexDepth : register(t123);

	bool IsRootCovered(float4 rootPosition, float rootSlope)
	{
		float2 uv = rootPosition.xy / rootPosition.w * float2(0.5, -0.5) + 0.5;
		float sceneDepth = SharedData::GetScreenDepth(TexDepth.Load(SharedData::ConvertUVToSampleCoord(uv)).x);
		float bias = 0.03 + rootSlope + 2e-8 * rootPosition.w * rootPosition.w;
		return RootTest > 0.0 && rootPosition.w > 0.0 && all(uv == saturate(uv)) && sceneDepth < rootPosition.w - bias;
	}

	static const float CoverageSpread = 0.75;
	static const float CoverageLod = 3.0;
	static const float StrandRadius = 0.75;    // Strand radius at the root, in texels: neighbours overlap, so roots close up
	static const float StrandJitter = 0.2;     // How far a strand may sit from its texel's centre, in texels
	static const float StrandRoundness = 0.6;  // How far a magnified strand's normal follows its rounded surface

	/**
	 * Shell alpha a texel needs to reach this shell. The lowest shell takes RootThreshold, so wherever fur grows it
	 * hides the base surface like an undercoat instead of leaving bright gaps at the roots; the outermost shell takes
	 * TipThreshold.
	 */
	float GetThreshold(float shell)
	{
		const float height = ShellCount > 1.0 ? saturate((shell * ShellCount - 1.0) / (ShellCount - 1.0)) : 1.0;
		return lerp(RootThreshold, TipThreshold, height);
	}

	struct Coverage
	{
		float Value;          // 0 to 1, tested against per-pixel noise
		float2 StrandOffset;  // Pixel's offset from its magnified strand's axis over the strand's radius; 0 when minified
	};

	/**
	 * Minified, a texel's alpha is a strand height and coverage widens with the footprint, so fur thins smoothly with
	 * distance. Magnified, each texel holds one round strand that tapers to a point at its height, so fur seen up close
	 * shows strands instead of filtered squares. Call in uniform control flow.
	 */
	Coverage GetCoverage(float alpha, float threshold, float2 uv)
	{
		uint width, height, levels;
		TexShell.GetDimensions(0, width, height, levels);
		const float2 texel = uv * float2(width, height);
		const float2 dx = ddx(texel);
		const float2 dy = ddy(texel);
		const float footprint = max(max(dot(dx, dx), dot(dy, dy)), 1e-8);
		const float pixelLod = 0.5 * log2(footprint);
		const float lod = pixelLod + SharedData::MipBias;

		Coverage coverage;
		const float spread = CoverageSpread * saturate(lod / CoverageLod) * alpha;
		coverage.Value = saturate((alpha + spread - threshold) / max(2.0 * spread, 1e-4));
		coverage.StrandOffset = 0.0;

		// Strands only once a texel truly spans a pixel: an upscaler's negative mip bias would make them sub-pixel and shimmer.
		const float magnified = saturate(-pixelLod);
		[branch] if (magnified > 0.0)
		{
			const float pixel = max(sqrt(footprint), 1e-3);                  // Pixel footprint in texels, for antialiased strand edges
			const float2 position = frac(uv) * float2(width, height) - 0.5;  // Texel centres at integer coordinates
			const int2 size = int2(width, height);
			const int2 base = int2(floor(position));  // -1 to size - 1
			float strand = 0.0;
			// Strands reach past their own texel, so the four nearest are tested.
			[unroll] for (uint i = 0; i < 4; ++i)
			{
				const int2 cell = base + int2(i & 1, i >> 1);
				const uint2 wrapped = uint2(cell + size) % uint2(size);
				const float strandAlpha = TexShell.Load(int3(wrapped, 0)).a;
				const float2 jitter = (float2(Random::pcg2d(wrapped) & 0xFFFF) / 65535.0 - 0.5) * (2.0 * StrandJitter);
				const float radius = strandAlpha > threshold ? StrandRadius * saturate((strandAlpha - threshold) / max(strandAlpha - RootThreshold, 1e-3)) : 0.0;
				const float2 offset = position - (float2(cell) + jitter);
				const float covered = radius > 0.0 ? saturate((radius - length(offset)) / pixel + 0.5) : 0.0;
				if (covered > strand) {
					strand = covered;
					coverage.StrandOffset = offset / radius;
				}
			}
			coverage.Value = lerp(coverage.Value, strand, magnified);
			coverage.StrandOffset *= magnified;
		}
		return coverage;
	}

	/**
	 * Tilts a shell's normal toward the pixel's side of its magnified strand, so close strands shade as rounded.
	 * Call where the shell is drawn (quad-uniform): it takes screen derivatives.
	 */
	float3 GetStrandNormal(float3 worldNormal, float3 worldPosition, float2 uv, float2 strandOffset)
	{
		// Cotangent frame: dP/du and dP/dv from screen derivatives, independent of the mesh's tangent convention.
		const float3 dp1 = ddx(worldPosition);
		const float3 dp2 = ddy(worldPosition);
		const float2 duv1 = ddx(uv);
		const float2 duv2 = ddy(uv);
		const float3 dp2perp = cross(dp2, worldNormal);
		const float3 dp1perp = cross(worldNormal, dp1);
		const float3 tangent = dp2perp * duv1.x + dp1perp * duv2.x;
		const float3 bitangent = dp2perp * duv1.y + dp1perp * duv2.y;

		float3 result = worldNormal;
		[branch] if (any(strandOffset != 0.0))
		{
			const float2 offset = strandOffset * rsqrt(max(dot(strandOffset, strandOffset), 1.0));
			const float3 rounded = tangent * (offset.x * rsqrt(max(dot(tangent, tangent), 1e-20))) +
			                       bitangent * (offset.y * rsqrt(max(dot(bitangent, bitangent), 1e-20))) +
			                       worldNormal * sqrt(saturate(1.0 - dot(offset, offset)));
			result = normalize(lerp(worldNormal, normalize(rounded), StrandRoundness));
		}
		return result;
	}
#endif

#if defined(VSHADER) && defined(MODELSPACENORMALS)
	Texture2D<float4> TexModelNormal : register(t122);

	float3 GetModelNormal(float2 uv)
	{
		uint width, height, levels;
		TexModelNormal.GetDimensions(0, width, height, levels);
		uint mip = min(uint(max(int(firstbithigh(max(width, height))), 9) - 9), max(levels, 1u) - 1u);
		uint2 size = max(uint2(width, height) >> mip, 1u);
		uint2 texel = min(uint2(frac(uv) * float2(size)), size - 1u);
		float3 normal = TexModelNormal.Load(int3(texel, mip)).xzy * 2.0 - 1.0;
		return normal * rsqrt(max(dot(normal, normal), 1e-6));
	}
#endif

	float GetShell(uint instanceID)
	{
		return saturate((ShellCount - float(instanceID)) / max(ShellCount, 1.0));
	}

	float GetDarkening(float shell, float facing)
	{
		return lerp(1.0, lerp(RootDarkening, 1.0, shell), smoothstep(0.0, 0.5, facing));
	}
}

#endif  // __FUR_SHELLS_HLSLI__
