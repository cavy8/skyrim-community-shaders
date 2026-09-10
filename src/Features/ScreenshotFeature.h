#pragma once

#include "Feature.h"
#include "Utils/Subrect.h"
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

struct ScreenshotFeature : public Feature
{
	/** @brief Stops the background screenshot worker thread on destruction. */
	virtual ~ScreenshotFeature();
	virtual std::string GetName() override { return "Screenshot"; }
	virtual std::string GetDisplayName() override { return T("feature.screenshot.name", "Screenshot"); }
	virtual std::string GetShortName() override { return "Screenshot"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kUtility; }

	/** @brief Returns true, indicating this feature's settings are always visible in the menu. */
	virtual bool IsInMenu() const override;

	/** @brief Draws the ImGui settings UI for screenshot path, format, crop, and hotkey configuration. */
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& a_json) override;
	virtual void SaveSettings(json& a_json) override;
	/** @brief Resets transient state (no-op for this feature). */
	virtual void Reset() override;
	/** @brief Called after all features are loaded (no-op for this feature). */
	virtual void PostPostLoad() override;

	/**
	 * @brief Captures a screenshot from the current back buffer and enqueues it for async encoding and save.
	 * @param overridePath When non-empty, the screenshot is saved here (extension chosen from the
	 *        capture format) instead of the configured Screenshots folder. Used by the Neural
	 *        Rendering comparison capture to redirect its pair into Data/DLSS 5 Screenshots/.
	 */
	void Capture(std::filesystem::path overridePath = {});
	/** @brief Checks for pending capture requests and executes Capture() for each. Called after HDR Present processing. */
	void ProcessCaptureRequest();

	/**
	 * @brief Queues a screenshot to be taken at the next Present into an explicit path (no crop, no clipboard).
	 *
	 * Drained by ProcessCaptureRequest() alongside the normal hotkey capture. Upscaling's Neural
	 * Rendering comparison state machine calls this once with Neural Rendering forced off and once
	 * forced on to produce the _NR-off / _NR-on pair.
	 *
	 * @param timestamp Shared timestamp string so both halves of a pair sort together.
	 * @param suffix Filename suffix, e.g. "_NR-off" / "_NR-on".
	 */
	void QueueNeuralRenderingComparisonShot(const std::string& timestamp, const char* suffix);
	bool applyCropToScreenshot = true;

	// Settings
	std::string screenshotPath = "Screenshots";
	// HDR PNG quantization (7-16); used when HDR Display captures the back buffer.
	unsigned int hdrPngBitDepth = 11;
	// SDR output (HDR captures always use PNG).
	bool sdrUsePng = true;
	// After save, put the file path on the clipboard (CF_HDROP).
	bool copyToClipboard = false;

	std::atomic<bool> captureRequested{ false };

private:
	struct PendingScreenshot
	{
		winrt::com_ptr<ID3D11Texture2D> stagingTexture;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		uint32_t width = 0;
		uint32_t height = 0;
		std::filesystem::path outputPath;
		bool saveAsHdrPng = false;
		bool saveAsSdrPng = false;
		int hdrPngBitDepth = 11;
		bool copyToClipboard = false;
	};

	std::mutex screenshotQueueMutex;
	std::condition_variable screenshotQueueCV;
	std::queue<PendingScreenshot> screenshotQueue;
	std::thread screenshotWorker;
	bool screenshotWorkerRunning = false;
	Util::Subrect::Controller subrect;

	// SRV-readable copy used when the capture source's own SRV can't be sampled
	// directly (kFRAMEBUFFER on flat aliases the swap-chain backbuffer).
	winrt::com_ptr<ID3D11Texture2D> previewCacheTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> previewCacheSRV;

	void EnsureWorkerThread();
	void StopWorkerThread();
	void EnqueueScreenshot(PendingScreenshot&& screenshot);

	// Explicit-path captures queued by the Neural Rendering comparison state machine,
	// drained in ProcessCaptureRequest() so they run through the normal capture path.
	std::mutex overrideCaptureMutex;
	std::vector<std::filesystem::path> pendingOverrideCaptures;
	void ScreenshotWorkerLoop();
	void EnsurePreviewCache(ID3D11Texture2D* sourceTexture);
	static void ShowInGameNotification(std::string message);
};
