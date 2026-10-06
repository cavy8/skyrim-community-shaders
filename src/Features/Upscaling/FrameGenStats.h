#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>

class FrameGenStats
{
public:
	static constexpr size_t kRecentFrames = 240;

	struct Sample
	{
		double frameTimeMs = 0.0;
		double presentMs = 0.0;
		double reflexSleepMs = 0.0;
		double refreshPeriodMs = 0.0;
		uint32_t framesPresented = 0;
		uint32_t syncInterval = 0;
		bool tearing = false;
		bool foreignCamera = false;
		bool worldCamera = false;
		bool hudLess = false;
	};

	struct Totals
	{
		uint64_t frames = 0;
		uint64_t timedFrames = 0;
		uint64_t generated = 0;
		uint64_t presentedFrames = 0;
		uint64_t hitches = 0;
		uint64_t refreshAligned = 0;
		uint64_t vsyncFrames = 0;
		uint64_t tearingFrames = 0;
		uint64_t foreignCamera = 0;
		uint64_t worldCamera = 0;
		uint64_t hudLess = 0;
		double frameTimeSum = 0.0;
		double frameTimeSqSum = 0.0;
		double frameTimeMax = 0.0;
		double presentSum = 0.0;
		double presentMax = 0.0;
		double reflexSleepSum = 0.0;
		double reflexSleepMax = 0.0;
		std::array<float, kRecentFrames> recentFrameTimes{};
		size_t recentCount = 0;
		size_t recentHead = 0;

		[[nodiscard]] double Share(uint64_t a_count) const { return frames ? 100.0 * static_cast<double>(a_count) / static_cast<double>(frames) : 0.0; }
		[[nodiscard]] double TimedShare(uint64_t a_count) const { return timedFrames ? 100.0 * static_cast<double>(a_count) / static_cast<double>(timedFrames) : 0.0; }
		[[nodiscard]] double FrameTimeAverage() const { return timedFrames ? frameTimeSum / static_cast<double>(timedFrames) : 0.0; }
		[[nodiscard]] double FrameTimeDeviation() const
		{
			if (timedFrames < 2)
				return 0.0;
			const double mean = FrameTimeAverage();
			const double variance = frameTimeSqSum / static_cast<double>(timedFrames) - mean * mean;
			return variance > 0.0 ? std::sqrt(variance) : 0.0;
		}
		[[nodiscard]] double PresentAverage() const { return frames ? presentSum / static_cast<double>(frames) : 0.0; }
		[[nodiscard]] double ReflexSleepAverage() const { return frames ? reflexSleepSum / static_cast<double>(frames) : 0.0; }
	};

	void Record(const Sample& a_sample)
	{
		std::scoped_lock lock(mutex);
		auto& t = totals;
		++t.frames;
		if (a_sample.framesPresented > 1)
			++t.generated;
		t.presentedFrames += a_sample.framesPresented;
		if (a_sample.syncInterval != 0)
			++t.vsyncFrames;
		if (a_sample.tearing)
			++t.tearingFrames;
		if (a_sample.foreignCamera)
			++t.foreignCamera;
		if (a_sample.worldCamera)
			++t.worldCamera;
		if (a_sample.hudLess)
			++t.hudLess;
		t.presentSum += a_sample.presentMs;
		t.presentMax = std::max(t.presentMax, a_sample.presentMs);
		t.reflexSleepSum += a_sample.reflexSleepMs;
		t.reflexSleepMax = std::max(t.reflexSleepMax, a_sample.reflexSleepMs);

		if (a_sample.frameTimeMs <= 0.0)
			return;

		++t.timedFrames;
		t.frameTimeSum += a_sample.frameTimeMs;
		t.frameTimeSqSum += a_sample.frameTimeMs * a_sample.frameTimeMs;
		t.frameTimeMax = std::max(t.frameTimeMax, a_sample.frameTimeMs);
		if (smoothedFrameTime > 0.0 && a_sample.frameTimeMs > smoothedFrameTime * kHitchRatio && a_sample.frameTimeMs > smoothedFrameTime + kHitchMinimumMs)
			++t.hitches;
		smoothedFrameTime = smoothedFrameTime > 0.0 ? smoothedFrameTime + kSmoothing * (a_sample.frameTimeMs - smoothedFrameTime) : a_sample.frameTimeMs;

		if (a_sample.refreshPeriodMs > 0.0) {
			const double refreshes = a_sample.frameTimeMs / a_sample.refreshPeriodMs;
			const double nearest = std::round(refreshes);
			if (nearest >= 1.0 && std::abs(refreshes - nearest) < kRefreshAlignTolerance)
				++t.refreshAligned;
		}

		t.recentFrameTimes[t.recentHead] = static_cast<float>(a_sample.frameTimeMs);
		t.recentHead = (t.recentHead + 1) % kRecentFrames;
		t.recentCount = std::min(t.recentCount + 1, kRecentFrames);
	}

	[[nodiscard]] Totals Get() const
	{
		std::scoped_lock lock(mutex);
		return totals;
	}

	void Reset()
	{
		std::scoped_lock lock(mutex);
		totals = {};
		smoothedFrameTime = 0.0;
	}

private:
	static constexpr double kHitchRatio = 1.5;
	static constexpr double kHitchMinimumMs = 4.0;
	static constexpr double kSmoothing = 0.1;
	static constexpr double kRefreshAlignTolerance = 0.12;

	mutable std::mutex mutex;
	Totals totals;
	double smoothedFrameTime = 0.0;
};
