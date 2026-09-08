#pragma once

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
	 * textures are allocated at the native extents of the supplied resources and
	 * the active region is expressed to NGX through its subrect parameters, so
	 * dynamic resolution does not reallocate anything.
	 */
	struct FrameInputs
	{
		ID3D11Resource* colorIn = nullptr;             ///< Scene colour to enhance.
		ID3D11Resource* colorOut = nullptr;            ///< Distinct destination for the result.
		ID3D11Resource* depth = nullptr;               ///< Game depth buffer.
		ID3D11ShaderResourceView* depthSRV = nullptr;  ///< SRV over @c depth, used by the guide pass.
		ID3D11Resource* motionVectors = nullptr;       ///< Motion vectors matching @c depth.
		std::uint32_t width = 0;                       ///< Active region width in pixels.
		std::uint32_t height = 0;                      ///< Active region height in pixels.
		float intensity = 0.8f;
		float localToneStrength = 0.75f;
		float localStructureStrength = 0.9f;
		float skinStructureStrength = 0.9f;
		std::uint32_t style = 3;
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

	/**
	 * @brief Releases every GPU resource, the NGX feature, the runtime, and the interop device.
	 *
	 * Also clears the failure latch, so toggling Neural Rendering off and on is
	 * the supported way to retry after a hard failure.
	 */
	void DestroyResources();

private:
	struct State;
	std::unique_ptr<State> state;
};
