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
	inputs.intensity = options.intensity;
	inputs.localToneStrength = options.localToneStrength;
	inputs.localStructureStrength = options.localStructureStrength;
	inputs.skinStructureStrength = options.skinStructureStrength;
	inputs.style = options.style;
	inputs.automaticMask = options.automaticMask;
	inputs.reset = options.reset;

	// Neural Rendering runs strictly 1:1: the depth guide is a texel-for-texel
	// copy, so guide and colour extents must agree. The runtime owns the
	// render-preset hint.
	return impl->backend.Evaluate(inputs);
}

void NeuralRendering::DestroyResources()
{
	impl->backend.DestroyResources();
}
