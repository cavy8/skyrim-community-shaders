#pragma once

#include <cstdint>
#include <memory>

struct ID3D11Resource;

/**
 * @brief Experimental direct NGX backend for DLSS 5 Neural Rendering.
 *
 * This class uses the NGX instance initialized by Streamline. It neither
 * initializes nor shuts down NGX, and requires a user-supplied
 * nvngx_dlssnr.dll in Streamline's plugin directory.
 */
class NeuralRendering final
{
public:
	/** @brief Settings passed to the Neural Rendering feature. */
	struct Options
	{
		float resolutionScale = 1.0f;
		uint32_t preset = 0;
		uint32_t style = 0;
		float intensity = 1.0f;
		float localToneStrength = 1.0f;
		float localStructureStrength = 1.0f;
		float skinStructureStrength = -1.0f;
		bool automaticMask = false;
		bool reset = false;
	};

	NeuralRendering();
	~NeuralRendering();

	NeuralRendering(const NeuralRendering&) = delete;
	NeuralRendering& operator=(const NeuralRendering&) = delete;

	/**
	 * @brief Checks whether the NGX core and required D3D11 exports can be bound.
	 * @return True when the core entry points are present; nvngx_dlssnr.dll support is confirmed only by Evaluate.
	 */
	bool IsAvailable() const;

	/**
	 * @brief Checks whether Feature 18 was successfully created for the current resource configuration.
	 * @return True only after a successful Evaluate-created Feature 18 instance; false does not imply the NGX core is absent.
	 */
	bool IsFeatureAvailable() const;

	/**
	 * @brief Executes Neural Rendering on the current D3D11 immediate context.
	 * @param colorIn Input color resource.
	 * @param colorOut Distinct output resource receiving the neural-rendered image.
	 * @param depth Depth resource.
	 * @param motionVectors Motion-vector resource.
	 * @param width Output width in pixels.
	 * @param height Output height in pixels.
	 * @param options Neural Rendering settings.
	 * @return True when NGX successfully evaluates the feature.
	 */
	bool Evaluate(ID3D11Resource* colorIn, ID3D11Resource* colorOut,
		ID3D11Resource* depth, ID3D11Resource* motionVectors,
		uint32_t width, uint32_t height, const Options& options);

	/**
	 * @brief Releases the NGX feature, parameter map, and scratch resource.
	 *
	 * This does not shut down NGX because Streamline owns its lifetime.
	 */
	void DestroyResources();

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
