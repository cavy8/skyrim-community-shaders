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
	 * The shared textures backing the D3D12 bridge are allocated at the native
	 * extents of the supplied resources. @p width and @p height describe only the
	 * active dynamic-resolution region and are forwarded to NGX as subrect
	 * extents, so changing dynamic resolution never reallocates anything.
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
	 * @brief Releases the NGX feature, the runtime, the D3D12 interop device and every shared resource.
	 *
	 * Also clears the failure latch, so toggling Neural Rendering off and on is
	 * the supported way to retry after a hard failure.
	 */
	void DestroyResources();

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
