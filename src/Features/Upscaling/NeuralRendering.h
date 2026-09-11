#pragma once

#include <cstdint>
#include <memory>

struct ID3D11Resource;
struct ID3D11ShaderResourceView;

/**
 * @brief DLSS Neural Rendering (NGX Feature 18) backend.
 *
 * Feature 18 only ever exposes a D3D12 ABI, so this class does not talk to the
 * Streamline-owned NGX core. It drives a directly loaded nvngx_dlssnr.dll
 * through the transport layer in `NeuralRendering/`, bridging Skyrim's D3D11
 * resources into D3D12 with shared textures. The runtime DLL is proprietary and
 * must be supplied by the user; Community Shaders never ships it.
 */
class NeuralRendering final
{
public:
	/** @brief Settings passed to the Neural Rendering feature. */
	struct Options
	{
		uint32_t style = 3;
		float intensity = 0.8f;
		float colorStrength = 1.0f;
		/// Overall weight of the model's edit on the frame (0..2). Zero leaves the
		/// frame untouched, one applies the model's change exactly, two doubles its
		/// relative luminance change (still inside the resolve's ratio guard).
		float transferStrength = 1.0f;
		/// When the model runs below the colour resolution, fade its edit across
		/// depth silhouettes so a bilinearly upsampled background edit does not
		/// bleed into thin foreground geometry. No effect at native scale.
		bool depthAwareResolve = true;
		/// Experimental: evaluate the model every other frame and re-apply its
		/// previous answer to the fresh frame in between, fading it where the
		/// content under a pixel changed. Halves the neural cost; the model's
		/// own temporal state then sees every second frame.
		bool alternateFrames = false;
		float localToneStrength = 0.75f;
		float localStructureStrength = 0.9f;
		float skinStructureStrength = 0.9f;
		bool automaticMask = true;
		bool reset = false;

		/// Valid region of @p depth and @p motionVectors, i.e. the game's render
		/// (dynamic) resolution. Zero means "same as width/height" - correct only
		/// when Neural Rendering runs before the upscaler, where the colour input is
		/// also at render resolution. After the upscaler the colour input is at
		/// display resolution while the guides are still at render resolution, so
		/// this must be set.
		uint32_t guideWidth = 0;
		uint32_t guideHeight = 0;

		/// Sub-pixel TAA jitter of @p colorIn in render pixels, using the same
		/// convention Streamline receives (a scene point at unjittered position u
		/// lands in the raster at u + offset). Feature 18 has no jitter parameter,
		/// so the backend resamples the frame onto the unjittered grid before the
		/// model sees it and maps the edit back afterwards. Zero when the colour
		/// input is the already-unjittered upscaled frame.
		float jitterOffsetX = 0.0f;
		float jitterOffsetY = 0.0f;

		/// Resolution the model runs at relative to the colour region it
		/// processes, per axis (0.25..2). Below one the model sees a downsampled
		/// proxy and only its bounded luminance/colour edit is applied to the
		/// full-resolution frame, which keeps native detail while cutting the neural
		/// cost roughly with the pixel count. Above one supersamples the model input.
		/// The backend debounces changes so a slider drag does not rebuild the
		/// feature every frame.
		float resolutionScaleX = 1.0f;
		float resolutionScaleY = 1.0f;

		/// DLSS-SR quality/preset selections mirrored from Upscaling settings when
		/// Separate Upscaling is active. They are ignored by the other placements.
		uint32_t superResolutionQualityMode = 1;
		uint32_t superResolutionPreset = 0;
	};

	NeuralRendering();
	~NeuralRendering();

	NeuralRendering(const NeuralRendering&) = delete;
	NeuralRendering& operator=(const NeuralRendering&) = delete;

	/**
	 * @brief Checks whether a usable nvngx_dlssnr.dll can be found and loaded.
	 * @return True when the runtime probe succeeded; the probe runs once and its result is cached.
	 */
	bool IsAvailable() const;

	/**
	 * @brief Checks whether Feature 18 has produced at least one successful frame.
	 * @return True after a successful evaluation; false does not imply the runtime is absent.
	 */
	bool IsFeatureAvailable() const;

	/**
	 * @brief Executes Neural Rendering on the current D3D11 immediate context.
	 *
	 * The shared textures backing the D3D12 bridge are compact allocations at the
	 * active colour and guide extents. This keeps Feature 18's creation raster and
	 * per-frame subrect contract identical.
	 *
	 * @param colorIn Input color resource; must be shader-readable.
	 * @param colorOut Distinct output resource receiving the neural-rendered image; must be UAV-writable.
	 * @param depth Depth resource.
	 * @param depthSRV Shader resource view over @p depth, used by the depth-guide compute pass.
	 * @param motionVectors Motion-vector resource.
	 * @param width Active region width in pixels.
	 * @param height Active region height in pixels.
	 * @param options Neural Rendering settings.
	 * @return True when NGX successfully evaluates the feature and the result reaches @p colorOut.
	 */
	bool Evaluate(ID3D11Resource* colorIn, ID3D11Resource* colorOut,
		ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV,
		ID3D11Resource* motionVectors,
		uint32_t width, uint32_t height, const Options& options);

	/**
	 * @brief Runs NR at render resolution and temporally upscales only its signed contribution.
	 *
	 * The original colour is left untouched for the game's normal DLSS pass. On success,
	 * ResolveSeparateUpscaling() can apply the private full-resolution residual afterwards.
	 */
	bool PrepareSeparateUpscaling(ID3D11Resource* colorIn, ID3D11Resource* editedColor,
		ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV,
		ID3D11Resource* motionVectors, ID3D11Resource* superResolutionMotionVectors,
		uint32_t width, uint32_t height,
		uint32_t outputWidth, uint32_t outputHeight, const Options& options);

	/**
	 * @brief Applies the prepared full-resolution residual to a clean main-DLSS result.
	 * @return True only when a matching private DLSS evaluation completed this frame.
	 */
	bool ResolveSeparateUpscaling(ID3D11Resource* cleanColor, ID3D11Resource* colorOut,
		uint32_t width, uint32_t height);

	/**
	 * @brief Retire private NGX histories and shared textures without shutting down
	 *        the process-wide NGX runtime.
	 *
	 * Placement and upscaler changes call this from the render thread. The private
	 * D3D12 interop/runtime instance is intentionally retained for safe reuse. This
	 * also clears the failure latch so toggling Neural Rendering can retry a failure.
	 */
	void DestroyResources();

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
