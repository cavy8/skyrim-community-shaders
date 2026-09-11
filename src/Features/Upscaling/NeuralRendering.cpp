#include "NeuralRendering.h"

#include "NeuralRendering/Backend.h"

// This translation unit deliberately holds nothing but the adapter between the
// public `NeuralRendering` class and `NeuralRenderingBackend`. The transport
// layer lives in a `NeuralRendering` *namespace*, which cannot coexist with a
// `NeuralRendering` *class* in one translation unit, so every transport header
// is confined to NeuralRendering/Backend.cpp.

struct NeuralRendering::Impl
{
	NeuralRenderingBackend backend;
};

namespace
{
	NeuralRenderingBackend::FrameInputs MakeFrameInputs(ID3D11Resource* colorIn, ID3D11Resource* colorOut,
		ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV, ID3D11Resource* motionVectors,
		uint32_t width, uint32_t height, const NeuralRendering::Options& options)
	{
		NeuralRenderingBackend::FrameInputs inputs;
		inputs.colorIn = colorIn;
		inputs.colorOut = colorOut;
		inputs.depth = depth;
		inputs.depthSRV = depthSRV;
		inputs.motionVectors = motionVectors;
		inputs.width = width;
		inputs.height = height;
		inputs.guideWidth = options.guideWidth ? options.guideWidth : width;
		inputs.guideHeight = options.guideHeight ? options.guideHeight : height;
		inputs.jitterOffsetX = options.jitterOffsetX;
		inputs.jitterOffsetY = options.jitterOffsetY;
		inputs.resolutionScaleX = options.resolutionScaleX;
		inputs.resolutionScaleY = options.resolutionScaleY;
		inputs.intensity = options.intensity;
		inputs.colorStrength = options.colorStrength;
		inputs.transferStrength = options.transferStrength;
		inputs.depthAwareResolve = options.depthAwareResolve;
		inputs.alternateFrames = options.alternateFrames;
		inputs.localToneStrength = options.localToneStrength;
		inputs.localStructureStrength = options.localStructureStrength;
		inputs.skinStructureStrength = options.skinStructureStrength;
		inputs.style = options.style;
		inputs.superResolutionQualityMode = options.superResolutionQualityMode;
		inputs.superResolutionPreset = options.superResolutionPreset;
		inputs.automaticMask = options.automaticMask;
		inputs.reset = options.reset;
		return inputs;
	}
}

NeuralRendering::NeuralRendering() :
	impl(std::make_unique<Impl>())
{}

NeuralRendering::~NeuralRendering() = default;

bool NeuralRendering::IsAvailable() const
{
	return impl->backend.IsAvailable();
}

bool NeuralRendering::IsFeatureAvailable() const
{
	return impl->backend.IsFeatureAvailable();
}

bool NeuralRendering::Evaluate(ID3D11Resource* colorIn, ID3D11Resource* colorOut,
	ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV,
	ID3D11Resource* motionVectors,
	uint32_t width, uint32_t height, const Options& options)
{
	// Each placement is 1:1 in colour/output space. After-upscale colour is
	// display-resolution while the depth and motion guides retain the render
	// resolution used by DLSS SR, so their extents are carried independently.
	return impl->backend.Evaluate(MakeFrameInputs(colorIn, colorOut, depth, depthSRV,
		motionVectors, width, height, options));
}

bool NeuralRendering::PrepareSeparateUpscaling(ID3D11Resource* colorIn, ID3D11Resource* editedColor,
	ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV,
	ID3D11Resource* motionVectors, ID3D11Resource* superResolutionMotionVectors,
	uint32_t width, uint32_t height,
	uint32_t outputWidth, uint32_t outputHeight, const Options& options)
{
	auto inputs = MakeFrameInputs(colorIn, editedColor, depth, depthSRV, motionVectors, width, height, options);
	inputs.superResolutionMotionVectors = superResolutionMotionVectors;
	inputs.outputWidth = outputWidth;
	inputs.outputHeight = outputHeight;
	return impl->backend.PrepareSeparateUpscaling(inputs);
}

bool NeuralRendering::ResolveSeparateUpscaling(ID3D11Resource* cleanColor, ID3D11Resource* colorOut,
	uint32_t width, uint32_t height)
{
	return impl->backend.ResolveSeparateUpscaling(cleanColor, colorOut, width, height);
}

void NeuralRendering::DestroyResources()
{
	impl->backend.DestroyResources();
}
