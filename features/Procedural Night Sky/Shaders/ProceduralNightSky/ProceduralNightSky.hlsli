// SPDX-License-Identifier: MIT AND (LicenseRef-GPL-3.0-with-Modding-Exception WITH GPL-3.0-linking-source-exception)
// Noise and star code adapted from WGSL-Generator, Copyright (c) 2026 NeaByteLab (MIT)

#ifndef PROCEDURAL_NIGHT_SKY_HLSLI
#define PROCEDURAL_NIGHT_SKY_HLSLI

namespace ProceduralNightSky
{
	static const float MaxStarRadius = 0.2;
	static const uint AuroraSteps = 8;
	static const float AuroraHeight = 1.1;
	static const float AuroraHorizonBend = 0.26;
	static const float AuroraArcSpacing = 1.7;
	static const float AuroraSheetThickness = 0.075;
	static const float AuroraHazeWidth = 0.5;
	static const float AuroraPeriod = 256.0;

	float Hash12(float2 p)
	{
		float2 q = frac(p * float2(123.34, 456.21));
		q += dot(q, q + 45.32);
		return frac(q.x * q.y);
	}

	float2 Hash22(float2 p)
	{
		float3 q = frac(p.xyx * float3(0.1031, 0.1030, 0.0973));
		q += dot(q, q.yzx + 33.33);
		return frac((q.xx + q.yz) * q.zy);
	}

	float2 WrapCell(float2 cell)
	{
		return cell - AuroraPeriod * floor(cell / AuroraPeriod);
	}

	float GradientNoise(float2 p)
	{
		float2 cell = floor(p);
		float2 offset = p - cell;
		float2 blend = offset * offset * offset * (offset * (offset * 6.0 - 15.0) + 10.0);

		float2 low = WrapCell(cell);
		float2 high = WrapCell(cell + 1.0);
		float bottom = lerp(dot(Hash22(low) * 2.0 - 1.0, offset), dot(Hash22(float2(high.x, low.y)) * 2.0 - 1.0, offset - float2(1.0, 0.0)), blend.x);
		float top = lerp(dot(Hash22(float2(low.x, high.y)) * 2.0 - 1.0, offset - float2(0.0, 1.0)), dot(Hash22(high) * 2.0 - 1.0, offset - 1.0), blend.x);
		return lerp(bottom, top, blend.y);
	}

	float RibbonNoise(float x, float row)
	{
		float cell = floor(x);
		float blend = x - cell;
		blend = blend * blend * blend * (blend * (blend * 6.0 - 15.0) + 10.0);

		float2 wrapped = WrapCell(float2(cell, cell + 1.0));
		return lerp(Hash12(float2(wrapped.x, row)), Hash12(float2(wrapped.y, row)), blend);
	}

	float2 ToOctahedron(float3 direction)
	{
		direction /= abs(direction.x) + abs(direction.y) + abs(direction.z);
		float2 folded = (1.0 - abs(direction.yx)) * float2(direction.x >= 0.0 ? 1.0 : -1.0, direction.y >= 0.0 ? 1.0 : -1.0);
		return direction.z >= 0.0 ? direction.xy : folded;
	}

	float3 StarLayer(float2 uv, float cells, float density, float baseRadius, float pixelAngle, float twinkle, float timer)
	{
		float2 grid = uv * cells;
		float2 cell = floor(grid);
		float pick = Hash12(cell);
		float3 star = 0.0;
		if (pick <= density) {
			float2 seed = Hash22(cell);
			float radius = min(max(baseRadius, pixelAngle * cells * 0.9), MaxStarRadius);
			float2 offset = frac(grid) - 0.5 - (seed - 0.5) * (0.9 - 2.0 * MaxStarRadius);
			float falloff = saturate(1.0 - length(offset) / radius);
			if (falloff > 0.0) {
				float rank = 1.0 - pick / density;
				float magnitude = rank * rank * rank * rank;
				float flicker = 1.0 + twinkle * sin(timer * (1.5 + 4.0 * seed.y) + seed.x * 6.2831853);
				float area = baseRadius * baseRadius / (radius * radius);

				float colorPick = Hash12(cell + float2(123.4, 567.8));
				float3 color = colorPick > 0.8 ? float3(0.6, 0.8, 1.0) : (colorPick < 0.2 ? float3(1.0, 0.9, 0.7) : float3(1.0, 1.0, 1.0));
				star = color * (pow(falloff, 6.0) * 2.5 + falloff * falloff * 0.5) * magnitude * flicker * area;
			}
		}
		return star;
	}

	float3 Stars(float3 direction, float pixelAngle, float twinkle, float timer)
	{
		float2 uv = ToOctahedron(direction);
		float3 stars = StarLayer(uv, 200.0, 0.5, 0.09, pixelAngle, twinkle, timer) * 0.6;
		stars += StarLayer(uv + float2(5.4, 2.7), 110.0, 0.4, 0.06, pixelAngle, twinkle, timer);
		stars += StarLayer(uv + float2(1.9, 8.4), 55.0, 0.2, 0.035, pixelAngle, twinkle, timer) * 2.0;
		return stars;
	}

	static const float3 RiftNormal = float3(0.75, 0.4330127, 0.5);
	static const float RiftPeriod = 24.0;
	static const float TwoPi = 6.2831853;

	float RiftNoise(float2 p, float period)
	{
		float2 cell = floor(p);
		float2 offset = p - cell;
		float2 blend = offset * offset * offset * (offset * (offset * 6.0 - 15.0) + 10.0);

		float2 low = float2(cell.x - period * floor(cell.x / period), WrapCell(cell).y);
		float2 high = float2(low.x + 1.0 >= period ? 0.0 : low.x + 1.0, WrapCell(cell + 1.0).y);
		float bottom = lerp(dot(Hash22(low) * 2.0 - 1.0, offset), dot(Hash22(float2(high.x, low.y)) * 2.0 - 1.0, offset - float2(1.0, 0.0)), blend.x);
		float top = lerp(dot(Hash22(float2(low.x, high.y)) * 2.0 - 1.0, offset - float2(0.0, 1.0)), dot(Hash22(high) * 2.0 - 1.0, offset - 1.0), blend.x);
		return lerp(bottom, top, blend.y);
	}

	float RiftDetail(float period, float pixelAngle)
	{
		return saturate(TwoPi / (period * max(pixelAngle, 1e-6)) * 0.25 - 0.5);
	}

	float3 RiftBand(float3 direction, float pixelAngle, float twinkle, float timer)
	{
		float3 tangent = normalize(cross(RiftNormal, float3(0.0, 0.0, 1.0)));
		float3 bitangent = cross(RiftNormal, tangent);
		float latitude = asin(clamp(dot(direction, RiftNormal), -1.0, 1.0));
		float3 band = 0.0;

		[branch] if (abs(latitude) < 1.0)
		{
			float longitude = atan2(dot(direction, bitangent), dot(direction, tangent)) / TwoPi + 0.5;
			float2 p = float2(longitude, latitude / TwoPi) * RiftPeriod;

			float centre = 0.07 * RiftNoise(float2(p.x, 3.0), RiftPeriod) + 0.03 * RiftNoise(float2(p.x * 3.0, 11.0), RiftPeriod * 3.0);
			float width = 0.11 * (0.8 + 0.6 * RiftNoise(float2(p.x * 2.0, 17.0), RiftPeriod * 2.0));
			float across = (latitude - centre) / width;
			float core = exp(-across * across);
			float halo = exp(-across * across / 7.0);

			[branch] if (halo > 1e-3)
			{
				float2 warp = float2(RiftNoise(p * 1.5 + 31.0, RiftPeriod * 1.5), RiftNoise(p * 1.5 + 57.0, RiftPeriod * 1.5)) * 0.6;
				float2 q = p * 2.0 + warp;
				float period = RiftPeriod * 2.0;
				float cloud = 0.0;
				float amplitude = 1.0;
				[unroll] for (uint i = 0; i < 4; i++)
				{
					cloud += RiftNoise(q + float(i) * 13.1, period) * amplitude * RiftDetail(period, pixelAngle);
					q *= 2.0;
					period *= 2.0;
					amplitude *= 0.5;
				}
				float clumps = saturate(0.55 + 0.9 * cloud);
				float knots = clumps * clumps * clumps;

				float2 tear = float2(p.x * 2.0, p.y * 7.0) + warp * 1.4;
				float ridge = saturate(1.0 - abs(RiftNoise(tear, RiftPeriod * 2.0)));
				float fineRidge = saturate(1.0 - abs(RiftNoise(tear * 2.0 + 5.3, RiftPeriod * 4.0)));
				float lanes = pow(ridge, 7.0) + 0.6 * pow(fineRidge, 9.0) * RiftDetail(RiftPeriod * 4.0, pixelAngle);
				float splitOffset = across - 0.3 - 0.35 * RiftNoise(float2(p.x * 2.5, 23.0), RiftPeriod * 2.5);
				float splitPresence = smoothstep(-0.15, 0.25, RiftNoise(float2(p.x * 1.25, 29.0), RiftPeriod * 1.25));
				float split = exp(-splitOffset * splitOffset / 0.05) * splitPresence;
				float voids = saturate(1.0 - 0.8 * saturate(lanes) - 0.85 * split);

				float hue = saturate(abs(across) * 0.8 + 0.35 * RiftNoise(p * 3.0 + 41.0, RiftPeriod * 3.0));
				float3 color = lerp(float3(1.0, 0.88, 0.66), float3(0.58, 0.66, 1.0), hue);
				color = lerp(color, float3(0.62, 0.45, 0.92), saturate(abs(across) * 0.5 - 0.4));
				color = lerp(color, float3(1.0, 0.95, 0.85), knots * core * 0.5);

				float glow = core * (0.3 + 0.9 * knots) + halo * 0.3 * (0.5 + 0.5 * clumps);
				band = color * glow * voids * 0.06;

				float2 uv = ToOctahedron(direction);
				band += StarLayer(uv + float2(3.3, 6.1), 420.0, 0.55 * core * clumps, 0.07, pixelAngle, twinkle, timer) * 0.4 * voids;
			}
		}
		return band;
	}

	float3 ShootingStar(float3 view, float pixelAngle, float3 head, float3 tail, float width)
	{
		float3 axis = head - tail;
		float along = saturate(dot(view - tail, axis) / max(dot(axis, axis), 1e-8));
		float3 offset = view - lerp(tail, head, along);
		float taperedWidth = width * (0.2 + 0.8 * along);
		float radius = max(taperedWidth, pixelAngle);
		float core = exp(-dot(offset, offset) / (radius * radius)) * taperedWidth / radius;
		return lerp(float3(1.0, 0.62, 0.35), float3(0.85, 0.95, 1.0), along) * core * along * along;
	}

	float AuroraFold(float2 p, float timer)
	{
		float2 drift = float2(timer * 0.021, -timer * 0.009);
		float2 warp = float2(GradientNoise(p * 0.23 + drift.yx + 31.0), GradientNoise(p * 0.23 - drift + 57.0));
		float broad = GradientNoise(p * 0.31 + drift + warp * 0.35);
		float curl = GradientNoise(p * 0.83 + float2(-timer * 0.033, timer * 0.017 + 9.0) + warp * 0.5);
		float ripple = GradientNoise(p * 2.1 + float2(timer * 0.05, 21.0));
		return broad * 3.4 + curl * 1.1 + ripple * 0.3;
	}

	float3 Aurora(float3 view, float timer, float3 lowColor, float3 highColor)
	{
		float2 plane = view.xy / (view.z + AuroraHorizonBend);
		float foldLow = AuroraFold(plane, timer);
		float foldHigh = AuroraFold(plane * (1.0 + AuroraHeight), timer);

		// Each step stands for a slice of altitude: the sheet is widened by how far it moves across the slice and the
		// height profile is averaged over it, so the sum needs no dither and has no grain
		float stepSize = 1.0 / float(AuroraSteps);
		float stepLength = abs(plane.y * AuroraHeight + foldHigh - foldLow) * stepSize;
		float spread = sqrt(AuroraSheetThickness * AuroraSheetThickness + stepLength * stepLength);
		// Rays finer than the distance a slice travels along the curtain would alias into stripes, so they fade to their mean
		float alongStep = (plane.x * AuroraHeight + 0.6 * (foldHigh - foldLow)) * stepSize;
		alongStep = sqrt(alongStep * alongStep + 0.04 * stepLength * stepLength);
		float3 detail = saturate(1.8 - alongStep * float3(6.5, 13.7, 29.0) * 2.4);

		float3 glow = 0.0;
		for (uint i = 0; i < AuroraSteps; i++) {
			float sliceLow = float(i) * stepSize;
			float altitude = sliceLow + 0.5 * stepSize;
			float2 p = plane * (1.0 + altitude * AuroraHeight);
			float fold = lerp(foldLow, foldHigh, altitude);

			float phase = (p.y + fold + timer * 0.012) / AuroraArcSpacing;
			float arc = floor(phase + 0.5);
			float offset = (phase - arc) * AuroraArcSpacing;
			float sheet = exp(-offset * offset / (spread * spread)) * 2.2 * AuroraSheetThickness / spread;
			float haze = exp(-offset * offset / (AuroraHazeWidth * AuroraHazeWidth)) * 0.2;
			// Each arc has its own pattern, so it has to reach zero where the nearest arc changes
			float window = smoothstep(0.5, 0.36, abs(phase - arc));

			float along = p.x + fold * 0.6 + arc * 17.3;
			float presence = 0.42 + 0.58 * smoothstep(-0.12, 0.2, GradientNoise(float2(along * 0.17 + timer * 0.013, arc * 3.7 + 40.0)));
			float pulse = 0.8 + 0.4 * RibbonNoise(along * 0.45 - timer * 0.09, arc + 50.0);
			float base = 0.03 + 0.16 * RibbonNoise(along * 0.9 + timer * 0.04, arc + 60.0);
			float rays = lerp(0.5, RibbonNoise(along * 6.5 + timer * 0.21, arc + 70.0), detail.x);
			float strands = lerp(0.5, RibbonNoise(along * 13.7 - timer * 0.33, arc + 75.0), detail.y);
			float fine = lerp(0.5, RibbonNoise(along * 29.0 - timer * 0.47, arc + 80.0), detail.z);
			float streak = 0.12 + 1.5 * rays * rays * rays + 0.5 * strands * strands * (0.25 + rays) + 0.7 * fine * fine * fine * (0.3 + rays);

			// Brighter rays reach higher
			float falloff = 3.6 / (0.7 + 0.6 * rays);
			float from = max(sliceLow - base, 0.0);
			float to = max(sliceLow + stepSize - base, 0.0);
			float profile = (exp(-from * falloff) - exp(-to * falloff)) / (falloff * stepSize);
			float lift = 0.5 * (from + to);
			glow += lerp(lowColor, highColor * 0.75, saturate(lift * 1.9)) * (sheet + haze) * window * presence * pulse * streak * profile;
		}

		float slant = sqrt(1.0 + dot(plane, plane));
		return glow * slant * smoothstep(0.0, 0.2, view.z) / float(AuroraSteps);
	}

	float4 ToAdditiveBlend(float3 emitted, float radianceLimit)
	{
		emitted = max(emitted, 0.0);
		float peak = max(emitted.x, max(emitted.y, emitted.z));
		float4 result = 0.0;
		if (peak > 0.0) {
			float alpha = clamp(peak / max(radianceLimit, 1.0), 1.0 / 256.0, 1.0);
			result = float4(emitted / alpha, alpha);
		}
		return result;
	}

	float3 EvaluateAurora(float3 view)
	{
		const SharedData::ProceduralNightSkySettings settings = SharedData::proceduralNightSkySettings;

		float3 lowColor = ColorManagement::SRGBToWorking(settings.auroraLowColor);
		float3 highColor = ColorManagement::SRGBToWorking(settings.auroraHighColor);
		return Aurora(view, settings.auroraTime, lowColor, highColor) * settings.auroraIntensity;
	}

	float4 Shade(float3 starDirection, float3 view, float pixelAngle, float3 starTint, float horizon)
	{
		const SharedData::ProceduralNightSkySettings settings = SharedData::proceduralNightSkySettings;

		float3 starLight = 0.0;
		[branch] if (settings.starIntensity > 0.0)
			starLight = Stars(starDirection, pixelAngle, settings.starTwinkle, SharedData::Timer) * settings.starIntensity;
		[branch] if (settings.shootingStarIntensity > 0.0)
			starLight += ShootingStar(view, pixelAngle, settings.shootingStarHead, settings.shootingStarTail, settings.shootingStarWidth) * settings.shootingStarIntensity;

		float3 emitted = ColorManagement::SRGBToWorking(starLight) * starTint;
		[branch] if (settings.riftIntensity > 0.0)
			emitted += ColorManagement::SRGBToWorking(RiftBand(starDirection, pixelAngle, settings.starTwinkle, SharedData::Timer)) * settings.riftIntensity * starTint;
		return ToAdditiveBlend(emitted * horizon, settings.radianceLimit);
	}
}

#endif
