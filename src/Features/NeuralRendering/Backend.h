#pragma once

#include <array>
#include <cstdint>
#include <memory>

struct ID3D11Resource;
struct ID3D11ShaderResourceView;

/**
 * @brief D3D11-facing driver for the DLSS Neural Rendering (NGX Feature 18) transport layer.
 *
 * The Neural Rendering feature (`NeuralRendering.cpp`) talks only to this
 * façade; Backend.cpp is the only translation unit that includes the transport
 * headers (`NeuralRenderingNGX::Runtime`, `NeuralRenderingNGX::D3D12Interop`),
 * which keeps the NGX, D3D12 and interop details out of the feature.
 *
 * The backend owns the D3D11<->D3D12 shared textures, the colour transfer and
 * depth-guide compute passes, and the failure latch that keeps a broken runtime
 * from being retried every frame.
 */
class NeuralRenderingBackend final
{
public:
	/**
	 * @brief Display transform the pre-tonemap placements' proxy replicates, so the model
	 *        sees the frame the way the user will (see ColorTransfer.hlsli, NeuralDisplayTransform).
	 *
	 * Default-constructed it is the identity: no exposure and the plain hue-preserving Reinhard
	 * proxy. Ignored for the display-gamma colour domain, which is already a finished frame.
	 */
	struct DisplayTransform
	{
		bool vanillaGrading = false;  ///< The vanilla tonemap owns the frame and the ISHDR constants below are valid.
		float param[4]{};             ///< ISHDR Param: y white point, z Hejl-Burgess-Dawson.
		float cinematic[4]{};         ///< ISHDR Cinematic: x saturation, z contrast, w brightness.
		float tint[4]{};              ///< ISHDR Tint: xyz colour, w amount.
		/// ISHDR's AvgTex from the previous tonemap pass: x adapted luminance, y target luminance.
		ID3D11ShaderResourceView* vanillaAdaptationSRV = nullptr;
		bool postProcessExposure = false;  ///< Post Processing's Histogram Auto Exposure is active downstream.
		/// Post Processing's adapted-luminance buffer (a single float).
		ID3D11ShaderResourceView* postProcessAdaptationSRV = nullptr;
		float postProcessExposureScale = 0.18f;               ///< 0.18 * exp2(exposure compensation).
		float postProcessAdaptationRange[2]{ 0.0f, 1.0f };  ///< Linear clamp range of the adapted luminance.
	};

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
		/// Sub-pixel projection offset of the guide rasters (@c depth,
		/// @c motionVectors and @c materialCategoriesSRV) in guide texels, same
		/// convention. The guides are always the game's jittered render targets,
		/// so this is zero whenever @c colorIn is that same jittered raster. After
		/// the upscaler the colour is the resolved, unjittered frame while the
		/// guides still carry the frame's jitter, and the decode must undo it
		/// before reading them.
		float guideJitterOffsetX = 0.0f;
		float guideJitterOffsetY = 0.0f;
		/// Model raster relative to the colour active region, per axis (0.25..2).
		/// Below one the model runs on a downsampled proxy and only its bounded
		/// edit returns to the full-resolution frame; above one it supersamples.
		float resolutionScaleX = 1.0f;
		float resolutionScaleY = 1.0f;
		/// How @c colorIn is encoded: 0 = linear open-ended HDR scene colour, 1 = finished
		/// gamma-2.2 display-referred frame (NeuralRendering::ColorDomain).
		std::uint32_t colorDomain = 0;
		/// Display transform the scene-linear proxy replicates (identity by default).
		DisplayTransform display{};
		float intensity = 0.8f;
		float colorStrength = 1.0f;
		/// Overall weight of the model's edit (0..2); one applies it exactly.
		float transferStrength = 1.0f;
		/// Additional multiplier on the model's luminance change alone; see
		/// NeuralRendering::Options::luminosityStrength.
		float luminosityStrength = 1.0f;
		/// Two-sided guard (1/maxRatio..maxRatio) on the model/proxy luminance ratio;
		/// see NeuralRendering::Options::maxRatio.
		float maxRatio = 2.0f;
		/// Whether maxRatio is applied at all; see NeuralRendering::Options::ratioGuardEnabled.
		bool ratioGuardEnabled = false;
		std::array<float, 7> categoryColorStrengths{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		std::array<float, 7> categoryTransferStrengths{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		std::array<float, 7> categoryLuminosityStrengths{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		/// Per-category hue guard (see ColorTransfer.hlsli, ResolveNeuralColor). Indexed by
		/// NeuralRendering::MaterialCategory; only Hair (index 2) defaults on.
		std::array<bool, 7> categoryHueGuard{ false, false, true, false, false, false, false };
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
		/// Debug view: the decode renders each pixel's classified material category as a flat
		/// colour instead of blending the model's edit; see NeuralRendering::Options::debugCategoryView.
		bool debugCategoryView = false;
		/// Diagnostic: write Feature 18's answer directly, bypassing the resolve
		/// entirely; see NeuralRendering::Options::rawModelOutput. Only honoured
		/// in the display-gamma colour domain (Finished Image).
		bool rawModelOutput = false;
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
