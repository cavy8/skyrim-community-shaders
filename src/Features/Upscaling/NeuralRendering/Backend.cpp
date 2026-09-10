#include "Backend.h"

#include "D3D12Interop.h"
#include "Runtime.h"

#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <winrt/base.h>

namespace
{
	/// Shared by EncodeColorCS and DecodeColorCS (register b0).
	struct alignas(16) TransferParams
	{
		float jitterOffset[2]{};  ///< Sub-pixel projection offset of the colour raster, in render pixels.
		float colorStrength = 1.0f;
		float padding = 0.0f;
	};
	static_assert(sizeof(TransferParams) == 16);

	constexpr const wchar_t* kEncodeColorPath = L"Data\\Shaders\\Upscaling\\NeuralRendering\\EncodeColorCS.hlsl";
	constexpr const wchar_t* kDecodeColorPath = L"Data\\Shaders\\Upscaling\\NeuralRendering\\DecodeColorCS.hlsl";
	constexpr const wchar_t* kCopyDepthGuidePath = L"Data\\Shaders\\Upscaling\\NeuralRendering\\CopyDepthGuideCS.hlsl";

	bool GetTextureDesc(ID3D11Resource* resource, D3D11_TEXTURE2D_DESC& desc)
	{
		winrt::com_ptr<ID3D11Texture2D> texture;
		if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(texture.put()))))
			return false;
		texture->GetDesc(&desc);
		return true;
	}

	/**
	 * @brief Builds a single-mip, single-sample shared-texture description from a game resource.
	 *
	 * The supplied active extent deliberately replaces the source allocation
	 * extent. Feature 18 builds internal history for its creation dimensions, so
	 * a render-resolution frame must not masquerade as a padded native frame.
	 */
	D3D11_TEXTURE2D_DESC MakeSharedDesc(const D3D11_TEXTURE2D_DESC& source, DXGI_FORMAT format, UINT bindFlags,
		UINT width, UINT height)
	{
		auto desc = source;
		desc.Width = width;
		desc.Height = height;
		desc.Format = format;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = bindFlags;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;
		return desc;
	}

	bool Matches(const NeuralRendering::SharedTexture& texture, const D3D11_TEXTURE2D_DESC& desc)
	{
		return texture.resource11 && texture.resource12 &&
		       texture.desc.Width == desc.Width && texture.desc.Height == desc.Height &&
		       texture.desc.Format == desc.Format;
	}
}

struct NeuralRenderingBackend::State
{
	NeuralRendering::D3D12Interop interop;

	NeuralRendering::SharedTexture color;
	NeuralRendering::SharedTexture depth;
	NeuralRendering::SharedTexture motionVectors;
	NeuralRendering::SharedTexture output;

	winrt::com_ptr<ID3D11ComputeShader> encodeColorCS;
	winrt::com_ptr<ID3D11ComputeShader> decodeColorCS;
	winrt::com_ptr<ID3D11ComputeShader> copyDepthGuideCS;
	winrt::com_ptr<ID3D11Buffer> transferParamsCB;
	/// Linear clamp sampler for the jitter-compensating resample in both colour passes.
	winrt::com_ptr<ID3D11SamplerState> linearClampSampler;
	bool encodeColorAttempted = false;
	bool decodeColorAttempted = false;
	bool copyDepthGuideAttempted = false;

	/// SRV over the Feature 18 output, consumed by the colour decode pass.
	winrt::com_ptr<ID3D11ShaderResourceView> outputSRV;
	/// SRV over the encoded model input, so the decode pass compares the answer
	/// against the exact proxy the model was given rather than a re-encode.
	winrt::com_ptr<ID3D11ShaderResourceView> colorSRV;
	/// SRV over the caller's colour input, cached against the resource it was created from.
	winrt::com_ptr<ID3D11ShaderResourceView> colorInSRV;
	ID3D11Resource* colorInSRVSource = nullptr;
	/// UAV over the caller's colour destination, cached against the resource it was created from.
	winrt::com_ptr<ID3D11UnorderedAccessView> colorOutUAV;
	ID3D11Resource* colorOutUAVSource = nullptr;
	/// Colour source used on the preceding frame. A change means the model moved
	/// between pre- and post-upscale domains and its temporal history is invalid.
	ID3D11Resource* lastColorInput = nullptr;

	bool probeAttempted = false;
	bool probeSucceeded = false;
	bool failureLatched = false;
	bool featureAvailable = false;
	bool resetPending = true;

	/// Colour/output and guide (depth+motion) regions NGX last saw. A change in
	/// either (dynamic resolution, or the Before/After placement toggle switching
	/// the colour input between render- and display-res) invalidates temporal
	/// history rather than smearing it into the new domain.
	std::uint32_t lastActiveWidth = 0;
	std::uint32_t lastActiveHeight = 0;
	std::uint32_t lastGuideWidth = 0;
	std::uint32_t lastGuideHeight = 0;

	bool loggedProbeFailure = false;
	bool loggedInvalidInputs = false;
	bool loggedShaderFailure = false;
	bool loggedViewFailure = false;

	~State()
	{
		// Deliberately does not go through Destroy(): the Runtime singleton is a
		// function-local static first touched during gameplay, so at process exit it
		// is destroyed before this object and calling Instance() here would
		// resurrect it. The interop device tears itself down in its own destructor.
		interop.WaitForIdle();
		ReleaseGpuResources();
	}

	bool Available()
	{
		if (!probeAttempted) {
			probeAttempted = true;
			probeSucceeded = NeuralRendering::Runtime::Instance().Probe();
			if (!probeSucceeded && !loggedProbeFailure) {
				loggedProbeFailure = true;
				logger::warn("[NeuralRendering] Runtime unavailable (status={} detail={}); install a compatible nvngx_dlssnr.dll to enable Neural Rendering.",
					NeuralRendering::ToString(NeuralRendering::Runtime::Instance().Status()),
					NeuralRendering::Runtime::Instance().Detail());
			}
		}
		return probeSucceeded;
	}

	bool LatchFailure(const char* operation, HRESULT error)
	{
		failureLatched = true;
		featureAvailable = false;
		logger::error("[NeuralRendering] {} failed hr/ngx=0x{:08X} status={} detail={}",
			operation, static_cast<std::uint32_t>(error),
			NeuralRendering::ToString(NeuralRendering::Runtime::Instance().Status()),
			NeuralRendering::Runtime::Instance().Detail());
		return false;
	}

	ID3D11ComputeShader* GetShader(winrt::com_ptr<ID3D11ComputeShader>& slot, bool& attempted,
		const wchar_t* path, const char* label)
	{
		if (!attempted) {
			attempted = true;
			slot.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(path, {}, "cs_5_0")));
			if (slot)
				Util::SetResourceName(slot.get(), "NeuralRendering::%s", label);
			else if (!loggedShaderFailure) {
				loggedShaderFailure = true;
				logger::error("[NeuralRendering] Failed to compile {}; Neural Rendering is disabled.", label);
			}
		}
		return slot.get();
	}

	bool InitializeInterop(ID3D11Device* device, ID3D11DeviceContext* context)
	{
		winrt::com_ptr<IDXGIDevice> dxgiDevice;
		winrt::com_ptr<IDXGIAdapter> adapter;
		HRESULT result = device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));
		if (SUCCEEDED(result))
			result = dxgiDevice->GetAdapter(adapter.put());
		if (FAILED(result) || !interop.Initialize(adapter.get(), device, context))
			return LatchFailure("D3D12 interop initialization", FAILED(result) ? result : interop.LastError());
		return true;
	}

	bool InitializeRuntime()
	{
		auto& runtime = NeuralRendering::Runtime::Instance();
		if (!runtime.Probe() || !runtime.Initialize(interop.Device()))
			return LatchFailure("runtime initialization", static_cast<HRESULT>(runtime.NgxResult()));
		logger::info("[NeuralRendering] Runtime initialized version={} appId=0x{:08X} api=0x{:X}",
			runtime.Version(), runtime.ApplicationId(), runtime.ApiVersion());
		return true;
	}

	/**
	 * @brief Creates or validates compact shared textures at the active input extents.
	 * @return False only when the descriptions cannot be read or a shared texture cannot be created.
	 */
	bool EnsureResources(const FrameInputs& inputs)
	{
		D3D11_TEXTURE2D_DESC colorSource{};
		D3D11_TEXTURE2D_DESC depthSource{};
		D3D11_TEXTURE2D_DESC motionSource{};
		if (!GetTextureDesc(inputs.colorIn, colorSource) || !GetTextureDesc(inputs.depth, depthSource) ||
			!GetTextureDesc(inputs.motionVectors, motionSource))
			return false;

		constexpr UINT sharedFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		const auto colorDesc = MakeSharedDesc(colorSource, colorSource.Format, sharedFlags, inputs.width, inputs.height);
		const auto outputDesc = MakeSharedDesc(colorSource, colorSource.Format, sharedFlags, inputs.width, inputs.height);
		const auto depthDesc = MakeSharedDesc(depthSource, DXGI_FORMAT_R32_FLOAT, sharedFlags,
			inputs.guideWidth, inputs.guideHeight);
		const auto motionDesc = MakeSharedDesc(motionSource, motionSource.Format, sharedFlags,
			inputs.guideWidth, inputs.guideHeight);

		if (Matches(color, colorDesc) && Matches(output, outputDesc) &&
			Matches(depth, depthDesc) && Matches(motionVectors, motionDesc) && outputSRV && colorSRV)
			return true;

		// Active extents changed (a resolution, placement, or display-mode change). Everything
		// downstream of the allocation - including the NGX feature handle - is stale.
		if (!interop.WaitForIdle())
			return false;
		NeuralRendering::Runtime::Instance().ResetFeature();
		color = {};
		depth = {};
		motionVectors = {};
		output = {};
		outputSRV = nullptr;
		colorSRV = nullptr;

		if (!interop.CreateSharedTexture(colorDesc, color, "NeuralRendering::Color") ||
			!interop.CreateSharedTexture(outputDesc, output, "NeuralRendering::Output") ||
			!interop.CreateSharedTexture(depthDesc, depth, "NeuralRendering::DepthGuide") ||
			!interop.CreateSharedTexture(motionDesc, motionVectors, "NeuralRendering::MotionVectors"))
			return false;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = outputDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		if (FAILED(globals::d3d::device->CreateShaderResourceView(output.resource11.Get(), &srvDesc, outputSRV.put())))
			return false;
		Util::SetResourceName(outputSRV.get(), "NeuralRendering::Output SRV");
		srvDesc.Format = colorDesc.Format;
		if (FAILED(globals::d3d::device->CreateShaderResourceView(color.resource11.Get(), &srvDesc, colorSRV.put())))
			return false;
		Util::SetResourceName(colorSRV.get(), "NeuralRendering::Color SRV");

		resetPending = true;
		logger::info("[NeuralRendering] Shared resources allocated colour={}x{} depth={}x{} motion={}x{}",
			colorDesc.Width, colorDesc.Height, depthDesc.Width, depthDesc.Height, motionDesc.Width, motionDesc.Height);
		return true;
	}

	ID3D11ShaderResourceView* GetColorInSRV(ID3D11Device* device, ID3D11Resource* resource)
	{
		if (colorInSRV && colorInSRVSource == resource)
			return colorInSRV.get();

		colorInSRV = nullptr;
		colorInSRVSource = nullptr;

		D3D11_TEXTURE2D_DESC desc{};
		if (!GetTextureDesc(resource, desc))
			return nullptr;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		if (FAILED(device->CreateShaderResourceView(resource, &srvDesc, colorInSRV.put()))) {
			colorInSRV = nullptr;
			if (!loggedViewFailure) {
				loggedViewFailure = true;
				logger::error("[NeuralRendering] Could not create a shader resource view over the colour input (format={}); Neural Rendering is disabled.", static_cast<int>(desc.Format));
			}
			return nullptr;
		}
		Util::SetResourceName(colorInSRV.get(), "NeuralRendering::ColorIn SRV");
		colorInSRVSource = resource;
		return colorInSRV.get();
	}

	ID3D11UnorderedAccessView* GetColorOutUAV(ID3D11Device* device, ID3D11Resource* resource)
	{
		if (colorOutUAV && colorOutUAVSource == resource)
			return colorOutUAV.get();

		colorOutUAV = nullptr;
		colorOutUAVSource = nullptr;

		D3D11_TEXTURE2D_DESC desc{};
		if (!GetTextureDesc(resource, desc))
			return nullptr;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = desc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;
		if (FAILED(device->CreateUnorderedAccessView(resource, &uavDesc, colorOutUAV.put()))) {
			colorOutUAV = nullptr;
			if (!loggedViewFailure) {
				loggedViewFailure = true;
				logger::error("[NeuralRendering] Could not create an unordered access view over the colour output (format={}); Neural Rendering is disabled.", static_cast<int>(desc.Format));
			}
			return nullptr;
		}
		Util::SetResourceName(colorOutUAV.get(), "NeuralRendering::ColorOut UAV");
		colorOutUAVSource = resource;
		return colorOutUAV.get();
	}

	/// Runs an up-to-three-SRV/single-UAV compute pass over the active region and unbinds afterwards.
	static void DispatchTransfer(ID3D11DeviceContext* context, ID3D11ComputeShader* shader,
		ID3D11ShaderResourceView* source, ID3D11ShaderResourceView* secondarySource,
		ID3D11ShaderResourceView* tertiarySource, ID3D11UnorderedAccessView* destination,
		ID3D11Buffer* constants, ID3D11SamplerState* sampler,
		std::uint32_t width, std::uint32_t height)
	{
		context->CSSetShader(shader, nullptr, 0);
		ID3D11ShaderResourceView* sources[3]{ source, secondarySource, tertiarySource };
		context->CSSetShaderResources(0, static_cast<UINT>(std::size(sources)), sources);
		context->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
		context->CSSetConstantBuffers(0, 1, &constants);
		context->CSSetSamplers(0, 1, &sampler);
		context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

		ID3D11ShaderResourceView* nullSRVs[3]{};
		ID3D11UnorderedAccessView* nullUAV = nullptr;
		ID3D11Buffer* nullCB = nullptr;
		ID3D11SamplerState* nullSampler = nullptr;
		context->CSSetShaderResources(0, static_cast<UINT>(std::size(nullSRVs)), nullSRVs);
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		context->CSSetConstantBuffers(0, 1, &nullCB);
		context->CSSetSamplers(0, 1, &nullSampler);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	bool ValidateInputs(const FrameInputs& inputs)
	{
		const bool distinct = inputs.colorIn != inputs.colorOut && inputs.colorIn != inputs.depth &&
		                      inputs.colorIn != inputs.motionVectors && inputs.colorOut != inputs.depth &&
		                      inputs.colorOut != inputs.motionVectors && inputs.depth != inputs.motionVectors;
		const bool finite = std::isfinite(inputs.intensity) && std::isfinite(inputs.colorStrength) &&
		                    std::isfinite(inputs.jitterOffsetX) && std::isfinite(inputs.jitterOffsetY) &&
		                    std::isfinite(inputs.localToneStrength) &&
		                    std::isfinite(inputs.localStructureStrength) && std::isfinite(inputs.skinStructureStrength);
		if (inputs.colorIn && inputs.colorOut && inputs.depth && inputs.depthSRV && inputs.motionVectors &&
			distinct && finite && inputs.width && inputs.height)
			return true;

		if (!loggedInvalidInputs) {
			loggedInvalidInputs = true;
			logger::warn("[NeuralRendering] Invalid or aliased D3D11 resources/settings; Neural Rendering evaluation was skipped.");
		}
		return false;
	}

	bool Run(const FrameInputs& inputs, ID3D11Device* device, ID3D11DeviceContext* context)
	{
		if (!interop.IsInitialized() && !InitializeInterop(device, context))
			return false;
		if (NeuralRendering::Runtime::Instance().Status() != NeuralRendering::RuntimeStatus::Initialized &&
			!InitializeRuntime())
			return false;
		if (!EnsureResources(inputs))
			return LatchFailure("shared resource creation", interop.LastError());

		// Dynamic resolution renders into the top-left of natively sized game
		// targets. Shared resources are compact, but keep these clamps as a final
		// guard against malformed active extents. Colour/output and depth+motion
		// regions remain independent because post-upscale colour is display sized.
		const std::uint32_t colorWidth = std::min({ inputs.width, color.desc.Width, output.desc.Width });
		const std::uint32_t colorHeight = std::min({ inputs.height, color.desc.Height, output.desc.Height });
		const std::uint32_t guideSrcWidth = inputs.guideWidth ? inputs.guideWidth : inputs.width;
		const std::uint32_t guideSrcHeight = inputs.guideHeight ? inputs.guideHeight : inputs.height;
		const std::uint32_t guideWidth = std::min({ guideSrcWidth, depth.desc.Width, motionVectors.desc.Width });
		const std::uint32_t guideHeight = std::min({ guideSrcHeight, depth.desc.Height, motionVectors.desc.Height });
		if (!colorWidth || !colorHeight || !guideWidth || !guideHeight)
			return false;

		if (colorWidth != lastActiveWidth || colorHeight != lastActiveHeight ||
			guideWidth != lastGuideWidth || guideHeight != lastGuideHeight) {
			resetPending = true;
			lastActiveWidth = colorWidth;
			lastActiveHeight = colorHeight;
			lastGuideWidth = guideWidth;
			lastGuideHeight = guideHeight;
		}
		if (inputs.colorIn != lastColorInput) {
			resetPending = true;
			lastColorInput = inputs.colorIn;
		}

		auto* encodeShader = GetShader(encodeColorCS, encodeColorAttempted, kEncodeColorPath, "EncodeColorCS");
		auto* decodeShader = GetShader(decodeColorCS, decodeColorAttempted, kDecodeColorPath, "DecodeColorCS");
		auto* guideShader = GetShader(copyDepthGuideCS, copyDepthGuideAttempted, kCopyDepthGuidePath, "CopyDepthGuideCS");
		auto* colorInView = GetColorInSRV(device, inputs.colorIn);
		auto* colorOutView = GetColorOutUAV(device, inputs.colorOut);
		if (!encodeShader || !decodeShader || !guideShader || !colorInView || !colorOutView ||
			!color.uav11 || !depth.uav11 || !outputSRV || !colorSRV)
			return false;
		if (!transferParamsCB) {
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = sizeof(TransferParams);
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			const auto result = device->CreateBuffer(&desc, nullptr, transferParamsCB.put());
			if (FAILED(result))
				return LatchFailure("transfer constant-buffer creation", result);
			Util::SetResourceName(transferParamsCB.get(), "NeuralRendering::TransferParams");
		}
		if (!linearClampSampler) {
			D3D11_SAMPLER_DESC desc{};
			desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			desc.MaxLOD = D3D11_FLOAT32_MAX;
			const auto result = device->CreateSamplerState(&desc, linearClampSampler.put());
			if (FAILED(result))
				return LatchFailure("transfer sampler creation", result);
			Util::SetResourceName(linearClampSampler.get(), "NeuralRendering::LinearClampSampler");
		}
		// The jitter offset is only meaningful when the colour raster is the game's
		// jittered render; the caller passes zero for the unjittered upscaled frame.
		// Anything beyond a pixel is not a TAA jitter and is treated as none.
		TransferParams transferParams;
		transferParams.jitterOffset[0] = std::abs(inputs.jitterOffsetX) <= 1.0f ? inputs.jitterOffsetX : 0.0f;
		transferParams.jitterOffset[1] = std::abs(inputs.jitterOffsetY) <= 1.0f ? inputs.jitterOffsetY : 0.0f;
		transferParams.colorStrength = std::clamp(inputs.colorStrength, 0.0f, 1.0f);
		context->UpdateSubresource(transferParamsCB.get(), 0, nullptr, &transferParams, 0, 0);

		// (b) Colour moves through compute passes rather than CopyResource. The
		// encode resamples the frame onto the unjittered pixel grid so the model
		// sees a stable framing; the decode later samples its answer back at each
		// original pixel's jittered position (see ColorTransfer.hlsli).
		DispatchTransfer(context, encodeShader, colorInView, nullptr, nullptr, color.uav11.Get(),
			transferParamsCB.get(), linearClampSampler.get(), colorWidth, colorHeight);
		DispatchTransfer(context, guideShader, inputs.depthSRV, nullptr, nullptr, depth.uav11.Get(),
			nullptr, nullptr, guideWidth, guideHeight);

		const D3D11_BOX motionBox{ 0, 0, 0, guideWidth, guideHeight, 1 };
		context->CopySubresourceRegion(motionVectors.resource11.Get(), 0, 0, 0, 0, inputs.motionVectors, 0, &motionBox);

		NeuralRendering::Tuning tuning;
		tuning.intensity = inputs.intensity;
		tuning.localToneStrength = inputs.localToneStrength;
		tuning.localStructureStrength = inputs.localStructureStrength;
		tuning.skinStructureStrength = inputs.skinStructureStrength;
		tuning.style = inputs.style;
		tuning.useAutoMask = inputs.automaticMask;
		tuning.uiCorrection = false;  // Community Shaders never runs Neural Rendering after the UI composite.

		ID3D12GraphicsCommandList* commandList = nullptr;
		if (!interop.BeginD3D12(&commandList) || !commandList)
			return LatchFailure("BeginD3D12", interop.LastError());

		ID3D12Resource* resources[4]{
			color.resource12.Get(), depth.resource12.Get(),
			motionVectors.resource12.Get(), output.resource12.Get()
		};
		D3D12_RESOURCE_BARRIER barriers[4]{};
		for (std::size_t index = 0; index < std::size(barriers); ++index) {
			barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[index].Transition.pResource = resources[index];
			barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
			barriers[index].Transition.StateAfter = index == 3 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS :
			                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		}
		commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);

		// Feature/output extents match the compact active colour raster. A raster
		// change rebuilds only after EnsureResources drains the interop queue. The
		// motion-vector scale is the guide resolution because Skyrim stores vectors
		// as normalized UV displacement; the NGX scale converts them to pixels.
		const bool executed = NeuralRendering::Runtime::Instance().Execute(commandList,
			color.resource12.Get(), depth.resource12.Get(), motionVectors.resource12.Get(), output.resource12.Get(),
			colorWidth, colorHeight, guideWidth, guideHeight, output.desc.Width, output.desc.Height,
			static_cast<float>(guideWidth), static_cast<float>(guideHeight),
			tuning, inputs.reset || resetPending);

		for (auto& barrier : barriers)
			std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);

		if (!interop.EndD3D12())
			return LatchFailure("EndD3D12", interop.LastError());
		if (!executed)
			return LatchFailure("Feature 18 execution", static_cast<HRESULT>(NeuralRendering::Runtime::Instance().NgxResult()));

		// Re-anchor the model's bounded luminance to the untouched source, then
		// restore its chromaticity through the independently controlled colour pass.
		// The edit is measured against the exact proxy the model received, sampled
		// at the same (jitter-compensated) position. No inverse tonemap or temporal
		// colour accumulator is involved.
		DispatchTransfer(context, decodeShader, outputSRV.get(), colorInView, colorSRV.get(), colorOutView,
			transferParamsCB.get(), linearClampSampler.get(), colorWidth, colorHeight);

		resetPending = false;
		featureAvailable = true;
		return true;
	}

	bool Evaluate(const FrameInputs& inputs)
	{
		ZoneScoped;
		if (failureLatched)
			return false;

		auto* device = globals::d3d::device;
		auto* context = globals::d3d::context;
		if (!device || !context)
			return false;
		if (!ValidateInputs(inputs) || !Available())
			return false;

		TracyD3D11Zone(globals::state->tracyCtx, "Neural Rendering");

		// Callers may still have render targets bound; a UAV write to a bound
		// resource would be silently dropped, so unbind and restore around the pass.
		ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* savedDSV = nullptr;
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
		context->OMSetRenderTargets(0, nullptr, nullptr);

		const bool succeeded = Run(inputs, device, context);

		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
		for (auto*& rtv : savedRTVs) {
			if (rtv)
				rtv->Release();
		}
		if (savedDSV)
			savedDSV->Release();

		return succeeded;
	}

	/// Releases everything this object owns directly, leaving the runtime and interop alone.
	void ReleaseGpuResources()
	{
		color = {};
		depth = {};
		motionVectors = {};
		output = {};

		outputSRV = nullptr;
		colorSRV = nullptr;
		colorInSRV = nullptr;
		colorInSRVSource = nullptr;
		colorOutUAV = nullptr;
		colorOutUAVSource = nullptr;
		lastColorInput = nullptr;

		encodeColorCS = nullptr;
		decodeColorCS = nullptr;
		copyDepthGuideCS = nullptr;
		transferParamsCB = nullptr;
		linearClampSampler = nullptr;
		encodeColorAttempted = false;
		decodeColorAttempted = false;
		copyDepthGuideAttempted = false;

		failureLatched = false;
		featureAvailable = false;
		resetPending = true;
		lastActiveWidth = 0;
		lastActiveHeight = 0;
		lastGuideWidth = 0;
		lastGuideHeight = 0;

		loggedInvalidInputs = false;
		loggedShaderFailure = false;
		loggedViewFailure = false;
	}

	void Destroy()
	{
		interop.WaitForIdle();
		NeuralRendering::Runtime::Instance().Shutdown();
		ReleaseGpuResources();
		interop.Shutdown();
	}
};

NeuralRenderingBackend::NeuralRenderingBackend() :
	state(std::make_unique<State>())
{}

NeuralRenderingBackend::~NeuralRenderingBackend() = default;

bool NeuralRenderingBackend::IsAvailable()
{
	return state->Available();
}

bool NeuralRenderingBackend::IsFeatureAvailable() const
{
	return state->featureAvailable;
}

bool NeuralRenderingBackend::Evaluate(const FrameInputs& inputs)
{
	return state->Evaluate(inputs);
}

void NeuralRenderingBackend::DestroyResources()
{
	state->Destroy();
}
