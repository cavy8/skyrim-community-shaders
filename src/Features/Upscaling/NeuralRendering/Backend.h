#pragma once

#include <array>
#include <cstdint>
#include <memory>

struct ID3D11Resource;
struct ID3D11ShaderResourceView;

/**
 * @brief D3D11-facing driver for the DLSS Neural Rendering (NGX Feature 18) transport layer.
 *
 * This façade exists for a hard language reason, not for layering taste: the
 * transport layer declares a `NeuralRendering` **namespace**
 * (`NeuralRendering::Runtime`, `NeuralRendering::D3D12Interop`) while the public
 * feature API declares a `NeuralRendering` **class**. C++ forbids a namespace
 * and a class of the same name in one scope, so no single translation unit may
 * see both headers. `NeuralRendering.cpp` owns the class and talks to this
 * façade; this translation unit is the only one that includes the transport
 * headers.
 *
 * The backend owns the D3D11<->D3D12 shared textures, the colour transfer and
 * depth-guide compute passes, and the failure latch that keeps a broken runtime
 * from being retried every frame.
 */
class NeuralRenderingBackend final
{
public:
	/**
	 * @brief One frame of Neural Rendering work.
	 *
	 * @c width and @c height describe the *active* region only. The shared
	 * colour/output textures are allocated at the model raster (the active colour
	 * extent scaled by @c resolutionScaleX/Y) and the guides at the guide extent.
	 * This keeps Feature 18's creation dimensions identical to the raster it
	 * processes and prevents padded/stale source margins from becoming temporal
	 * history.
	 */
	struct FrameInputs
	{
		ID3D11Resource* colorIn = nullptr;                          ///< Scene colour to enhance.
		ID3D11Resource* colorOut = nullptr;                         ///< Distinct destination for the result.
		ID3D11Resource* depth = nullptr;                            ///< Game depth buffer.
		ID3D11ShaderResourceView* depthSRV = nullptr;               ///< SRV over @c depth, used by the guide pass.
		ID3D11ShaderResourceView* materialCategoriesSRV = nullptr;  ///< Packed Masks2 material categories.
		ID3D11Resource* motionVectors = nullptr;                    ///< Motion vectors matching @c depth.
		ID3D11Resource* superResolutionMotionVectors = nullptr;     ///< Processed motion field used by main DLSS.
		std::uint32_t width = 0;                                    ///< Colour/output active region width in pixels.
		std::uint32_t height = 0;                                   ///< Colour/output active region height in pixels.
		std::uint32_t guideWidth = 0;                               ///< Depth/motion-vector active region width (render resolution).
		std::uint32_t guideHeight = 0;                              ///< Depth/motion-vector active region height (render resolution).
		/// Sub-pixel projection offset of @c colorIn in render pixels (Streamline
		/// convention: a scene point at unjittered position u lands at u + offset).
		/// Non-zero only when @c colorIn is the game's jittered render; the
		/// upscaled frame is unjittered and passes zero.
		float jitterOffsetX = 0.0f;
		float jitterOffsetY = 0.0f;
		/// Model raster relative to the colour active region, per axis (0.25..2).
		/// Below one the model runs on a downsampled proxy and only its bounded
		/// edit returns to the full-resolution frame; above one it supersamples.
		float resolutionScaleX = 1.0f;
		float resolutionScaleY = 1.0f;
		float intensity = 0.8f;
		float colorStrength = 1.0f;
		/// Overall weight of the model's edit (0..2); one applies it exactly.
		float transferStrength = 1.0f;
		bool perCategoryStrengths = false;
		std::array<float, 7> categoryColorStrengths{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		std::array<float, 7> categoryTransferStrengths{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		/// Fade the edit across depth silhouettes when the model runs below the
		/// colour resolution; no effect at native scale.
		bool depthAwareResolve = true;
		/// Run the model every other frame and re-apply its previous answer to the
		/// fresh frame in between (experimental; halves the neural cost).
		bool alternateFrames = false;
		float localToneStrength = 1.0f;
		float localStructureStrength = 1.0f;
		float skinStructureStrength = -1.0f;
		std::uint32_t style = 3;
		std::uint32_t outputWidth = 0;   ///< Separate-upscaling output width; zero for ordinary Evaluate().
		std::uint32_t outputHeight = 0;  ///< Separate-upscaling output height.
		std::uint32_t superResolutionQualityMode = 1;
		std::uint32_t superResolutionPreset = 0;
		bool automaticMask = true;
		bool reset = false;  ///< Force a history reset on this frame.
	};

	NeuralRenderingBackend();
	~NeuralRenderingBackend();

	NeuralRenderingBackend(const NeuralRenderingBackend&) = delete;
	NeuralRenderingBackend& operator=(const NeuralRenderingBackend&) = delete;

	/**
	 * @brief Probes for a user-supplied nvngx_dlssnr.dll once and caches the result.
	 * @return True when the Neural Rendering runtime was found and is a supported version.
	 */
	bool IsAvailable();

	/**
	 * @brief Reports whether Feature 18 has produced at least one successful frame.
	 * @return True after a successful Execute; false while latched, uninitialised, or torn down.
	 */
	bool IsFeatureAvailable() const;

	/**
	 * @brief Runs the full Neural Rendering pass for one frame.
	 * @param inputs Resources, active region, and tuning for this frame.
	 * @return True when the result was written to @c inputs.colorOut.
	 */
	bool Evaluate(const FrameInputs& inputs);

	/** @brief Prepare the private, temporally upscaled signed NR residual for this frame. */
	bool PrepareSeparateUpscaling(const FrameInputs& inputs);

	/** @brief Apply the prepared residual to the game's clean display-resolution DLSS output. */
	bool ResolveSeparateUpscaling(ID3D11Resource* cleanColor, ID3D11Resource* colorOut,
		std::uint32_t width, std::uint32_t height);

	/**
	 * @brief Releases shared GPU resources and private NGX feature histories.
	 *
	 * The NGX runtime and interop device remain alive because placement changes can
	 * invoke this on the render thread. Also clears the failure latch for retries.
	 */
	void DestroyResources();

private:
	struct State;
	std::unique_ptr<State> state;
};
