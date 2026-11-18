#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

/**
 * Rainbow optics, free of game types so it can be checked on its own.
 *
 * Each wavelength's bow follows Airy theory, I ~ Ai(-z)^2 with z = (12/h)^(1/3) x^(2/3) dTheta (x = 2 pi a / lambda),
 * scaled by the square root of the z factor so that far from the bow it meets the drop-size-free Cartesian
 * 1/sqrt(dTheta) glow. A spread of drop sizes and the sun's disc blur it. CIE 1931 matching functions (the
 * Wyman, Sloan and Shirley 2013 fit) turn the spectrum into linear Rec.709, white-balanced to an equal-energy
 * light so the scene's light color tints it.
 */
namespace RainbowOptics
{
	struct RGB
	{
		float r = 0.0f;
		float g = 0.0f;
		float b = 0.0f;
	};

	inline constexpr double kPi = std::numbers::pi;
	inline constexpr double kSunRadius = 0.2665 * kPi / 180.0;
	inline constexpr double kAiryLow = -8.0;
	inline constexpr double kAiryHigh = 40.0;
	inline constexpr int kAiryCount = 24001;
	inline constexpr double kAiryStep = (kAiryHigh - kAiryLow) / (kAiryCount - 1);
	inline constexpr float kAiryBlendStart = static_cast<float>(kAiryHigh - 16.0);
	inline constexpr float kAiryBlendWidth = 8.0f;
	inline constexpr int kSunSamples = 12;
	inline constexpr int kWavelengths = 31;
	inline constexpr float kLambdaMin = 400.0f;
	inline constexpr float kLambdaStep = 10.0f;
	inline constexpr std::array<float, 5> kDropScales = { 0.7f, 0.85f, 1.0f, 1.15f, 1.3f };
	inline constexpr std::array<float, 5> kDropWeights = { 0.5f, 1.0f, 1.2f, 1.0f, 0.5f };

	/** Airy function Ai(x): Maclaurin series for |x| <= 6, asymptotic expansions beyond. */
	inline double Airy(double a_x)
	{
		if (a_x > 6.0) {
			const double zeta = 2.0 / 3.0 * std::pow(a_x, 1.5);
			return std::exp(-zeta) / (2.0 * std::sqrt(kPi) * std::pow(a_x, 0.25)) * (1.0 - 5.0 / (72.0 * zeta));
		}
		if (a_x < -6.0) {
			const double a = -a_x;
			const double zeta = 2.0 / 3.0 * std::pow(a, 1.5);
			const double phase = zeta + kPi / 4.0;
			return (std::sin(phase) - 5.0 / (72.0 * zeta) * std::cos(phase)) / (std::sqrt(kPi) * std::pow(a, 0.25));
		}
		constexpr double c1 = 0.355028053887817239;
		constexpr double c2 = 0.258819403792806798;
		const double x3 = a_x * a_x * a_x;
		double f = 1.0, g = a_x, termF = 1.0, termG = a_x;
		for (int k = 1; k < 60; ++k) {
			termF *= x3 / ((3.0 * k - 1.0) * (3.0 * k));
			termG *= x3 / ((3.0 * k) * (3.0 * k + 1.0));
			f += termF;
			g += termG;
			if (std::abs(termF) < 1e-17 && std::abs(termG) < 1e-17)
				break;
		}
		return c1 * f - c2 * g;
	}

	/** Ai(-z)^2, tabulated; past the table the oscillation is finer than the sun's disc, so its mean 1/(2 pi sqrt z). */
	inline float AirySquared(float a_z)
	{
		static const std::vector<float> table = [] {
			std::vector<float> values(kAiryCount);
			for (int i = 0; i < kAiryCount; ++i) {
				const double ai = Airy(-(kAiryLow + i * kAiryStep));
				values[i] = static_cast<float>(ai * ai);
			}
			return values;
		}();

		if (a_z < kAiryLow)
			return 0.0f;
		const float blend = std::clamp((a_z - kAiryBlendStart) / kAiryBlendWidth, 0.0f, 1.0f);
		const float mean = blend > 0.0f ? 1.0f / (2.0f * static_cast<float>(kPi) * std::sqrt(a_z)) : 0.0f;
		if (blend >= 1.0f)
			return mean;
		const float t = static_cast<float>((a_z - kAiryLow) / kAiryStep);
		const int i = std::min(static_cast<int>(t), kAiryCount - 2);
		const float f = t - static_cast<float>(i);
		const float tabulated = table[i] + (table[i + 1] - table[i]) * f;
		return tabulated + (mean - tabulated) * blend;
	}

	inline float Lobe(float a_lambda, float a_mean, float a_sigmaLow, float a_sigmaHigh)
	{
		const float t = (a_lambda - a_mean) / (a_lambda < a_mean ? a_sigmaLow : a_sigmaHigh);
		return std::exp(-0.5f * t * t);
	}

	/** CIE 1931 2-degree matching functions mapped straight to linear Rec.709. */
	inline RGB SpectralRGB(float a_lambda)
	{
		const float x = 1.056f * Lobe(a_lambda, 599.8f, 37.9f, 31.0f) + 0.362f * Lobe(a_lambda, 442.0f, 16.0f, 26.7f) - 0.065f * Lobe(a_lambda, 501.1f, 20.4f, 26.2f);
		const float y = 0.821f * Lobe(a_lambda, 568.8f, 46.9f, 40.5f) + 0.286f * Lobe(a_lambda, 530.9f, 16.3f, 31.1f);
		const float z = 1.217f * Lobe(a_lambda, 437.0f, 11.8f, 36.0f) + 0.681f * Lobe(a_lambda, 459.0f, 26.0f, 13.8f);
		return {
			3.2406f * x - 1.5372f * y - 0.4986f * z,
			-0.9689f * x + 1.8758f * y + 0.0415f * z,
			0.0557f * x - 0.2040f * y + 1.0570f * z
		};
	}

	inline float Luminance(const RGB& a_rgb)
	{
		return 0.2126f * a_rgb.r + 0.7152f * a_rgb.g + 0.0722f * a_rgb.b;
	}

	/** Refractive index of water (Cauchy fit through 1.3439 at 400 nm and 1.3312 at 700 nm). */
	inline double WaterIndex(double a_lambda)
	{
		return 1.32504 + 3017.0 / (a_lambda * a_lambda);
	}

	struct Bow
	{
		double angle;   // From the antisolar point, radians
		double factor;  // (12/h)^(1/3)
	};

	/** Bow after a_reflections internal reflections: its minimum-deviation angle and Airy's curvature term h. */
	inline Bow GetBow(double a_index, int a_reflections)
	{
		const double p = a_reflections + 1.0;
		const double n2 = a_index * a_index;
		const double incidence = std::acos(std::sqrt((n2 - 1.0) / (p * p - 1.0)));
		const double refraction = std::asin(std::sin(incidence) / a_index);
		const double deviation = 2.0 * incidence - 2.0 * p * refraction + a_reflections * kPi;
		const double h = (p * p - 1.0) * (p * p - 1.0) / (p * p * (n2 - 1.0)) * std::sqrt((p * p - n2) / (n2 - 1.0));
		return { std::abs(kPi - deviation), std::cbrt(12.0 / h) };
	}

	inline float Smooth(float a_x)
	{
		const float t = std::clamp(a_x, 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	/**
	 * Fills a_out with linear RGB for a_size angles evenly covering [0, a_maxDegrees] from the antisolar point
	 * (texel centers). Returns the peak luminance.
	 * @param a_dropSize Mean drop radius in micrometers.
	 * @param a_secondary Secondary bow amplitude relative to the primary.
	 * @param a_glow Brightness kept on each bow's bright side more than a few degrees from it (1 = Airy/Cartesian).
	 */
	inline float BuildProfile(float a_dropSize, float a_secondary, float a_glow, uint32_t a_size, float a_maxDegrees, std::vector<RGB>& a_out)
	{
		const double maxAngle = a_maxDegrees * kPi / 180.0;
		a_out.assign(a_size, RGB{});

		std::array<float, kSunSamples> sunOffsets{};
		std::array<float, kSunSamples> sunWeights{};
		float sunTotal = 0.0f;
		for (int s = 0; s < kSunSamples; ++s) {
			const float u = (s + 0.5f) / kSunSamples * 2.0f - 1.0f;
			sunOffsets[s] = static_cast<float>(u * kSunRadius);
			sunWeights[s] = std::sqrt(1.0f - u * u);
			sunTotal += sunWeights[s];
		}
		for (auto& weight : sunWeights)
			weight /= sunTotal;

		std::array<float, kDropScales.size()> dropWeights{};
		float dropTotal = 0.0f;
		for (size_t d = 0; d < kDropScales.size(); ++d) {
			dropWeights[d] = kDropWeights[d] * kDropScales[d] * kDropScales[d];
			dropTotal += dropWeights[d];
		}
		for (auto& weight : dropWeights)
			weight /= dropTotal;

		RGB white{};
		for (int w = 0; w < kWavelengths; ++w) {
			const RGB rgb = SpectralRGB(kLambdaMin + w * kLambdaStep);
			white.r += rgb.r;
			white.g += rgb.g;
			white.b += rgb.b;
		}

		std::vector<float> thetas(a_size);
		for (uint32_t i = 0; i < a_size; ++i)
			thetas[i] = static_cast<float>((i + 0.5) / a_size * maxAngle);

		std::vector<float> spectral(a_size);
		for (int w = 0; w < kWavelengths; ++w) {
			const float lambda = kLambdaMin + w * kLambdaStep;
			const double index = WaterIndex(lambda);
			std::fill(spectral.begin(), spectral.end(), 0.0f);

			for (int reflections = 1; reflections <= 2; ++reflections) {
				const float amplitude = reflections == 1 ? 1.0f : a_secondary;
				if (amplitude <= 0.0f)
					continue;
				const Bow bow = GetBow(index, reflections);
				const float bowAngle = static_cast<float>(bow.angle);

				std::array<float, kDropScales.size()> scales{};
				std::array<float, kDropScales.size()> norms{};
				for (size_t d = 0; d < kDropScales.size(); ++d) {
					const double x = 2.0 * kPi * a_dropSize * kDropScales[d] * 1000.0 / lambda;
					scales[d] = static_cast<float>(bow.factor * std::pow(x, 2.0 / 3.0));
					norms[d] = dropWeights[d] * std::sqrt(scales[d]);
				}

				for (uint32_t i = 0; i < a_size; ++i) {
					float sum = 0.0f;
					for (int s = 0; s < kSunSamples; ++s) {
						// Positive on the bow's bright side: inside the primary, outside the secondary.
						const float angle = thetas[i] + sunOffsets[s];
						const float delta = reflections == 1 ? bowAngle - angle : angle - bowAngle;
						// Dark side: Ai(-z) has fallen below 1e-6 of its peak by z = -8, which every drop reaches here.
						if (delta * scales[0] < static_cast<float>(kAiryLow))
							continue;
						const float degrees = delta * static_cast<float>(180.0 / kPi);
						float taper = 1.0f + (a_glow - 1.0f) * Smooth((degrees - 0.75f) / 3.25f);
						if (reflections == 2)
							taper *= 1.0f - Smooth((degrees - 4.0f) / 8.0f);
						if (taper <= 0.0f)
							continue;
						float airy = 0.0f;
						for (size_t d = 0; d < kDropScales.size(); ++d)
							airy += norms[d] * AirySquared(scales[d] * delta);
						sum += sunWeights[s] * taper * airy;
					}
					spectral[i] += amplitude * sum;
				}
			}

			const RGB rgb = SpectralRGB(lambda);
			const RGB weight{ rgb.r / white.r, rgb.g / white.g, rgb.b / white.b };
			for (uint32_t i = 0; i < a_size; ++i) {
				a_out[i].r += spectral[i] * weight.r;
				a_out[i].g += spectral[i] * weight.g;
				a_out[i].b += spectral[i] * weight.b;
			}
		}

		float peak = 0.0f;
		for (auto& texel : a_out) {
			const float luminance = Luminance(texel);
			const float lowest = std::min({ texel.r, texel.g, texel.b });
			// Spectral colors fall outside Rec.709: desaturate toward their luminance until no channel is negative.
			if (lowest < 0.0f) {
				const float scale = luminance > 0.0f ? luminance / (luminance - lowest) : 0.0f;
				texel.r = luminance + (texel.r - luminance) * scale;
				texel.g = luminance + (texel.g - luminance) * scale;
				texel.b = luminance + (texel.b - luminance) * scale;
			}
			texel.r = std::max(texel.r, 0.0f);
			texel.g = std::max(texel.g, 0.0f);
			texel.b = std::max(texel.b, 0.0f);
			peak = std::max(peak, Luminance(texel));
		}
		return peak;
	}
}
