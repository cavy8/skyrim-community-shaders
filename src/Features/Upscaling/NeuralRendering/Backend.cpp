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
		float transferStrength = 1.0f;        ///< Overall edit weight; one reproduces the model's change exactly.
		std::uint32_t activeSize[2]{};        ///< Colour/output active region, in colour texels.
		std::uint32_t workSize[2]{};          ///< Model raster; the shared colour/output textures are this size.
		std::uint32_t guideSize[2]{};         ///< Depth guide active region, in guide texels.
		std::uint32_t depthAwareResolve = 0;  ///< Non-zero: fade the edit across depth silhouettes in the decode.
		std::uint32_t skipFrame = 0;          ///< Non-zero: the model did not run; the decode re-applies its stale answer.
	};
	static_assert(sizeof(TransferParams) == 48);

	constexpr float kMinimumResolutionScale = 0.25f;
	constexpr float kMaximumResolutionScale = 2.0f;
	/// Feature 18 is not created below this per-axis extent.
	constexpr std::uint32_t kMinimumModelExtent = 64;
	/// Frames a changed model raster must stay stable before the shared textures
	/// and the NGX feature are rebuilt for it. Rebuilding drains the interop queue,
	/// so applying every intermediate value of a slider drag would hitch per frame.
	constexpr std::uint32_t kModelRasterDebounceFrames = 12;

	/**
	 * @brief Model raster extent for one axis.
	 *
	 * Scale one is exact so the native path is untouched. Otherwise the active
	 * extent is scaled, rounded to an even texel count and floored at
	 * kMinimumModelExtent, matching the raster the DLSSNR-Cost-Scaler proxy builds.
	 */
	std::uint32_t ScaledExtent(std::uint32_t active, float scale)
	{
		scale = std::clamp(scale, kMinimumResolutionScale, kMaximumResolutionScale);
		if (std::abs(scale - 1.0f) < 0.005f)
			return active;
		const auto scaled = static_cast<std::uint32_t>(std::lround(static_cast<double>(active) * scale)) & ~1u;
		return std::max(scaled, kMinimumModelExtent);
	}

	constexpr const wchar_t* kEncodeColorPath = L"Data\\Shaders\\Upscaling\\NeuralRendering\\EncodeColorCS.hlsl";
	constexpr const wchar_t* kDecodeColorPath = L"Data\\Shaders\\Upscaling\\NeuralRendering\\DecodeColorCS.hlsl";
	constexpr const wchar_t* kCopyDepthGuidePath = L"Data\\Shaders\\Upscaling\\NeuralRendering\\CopyDepthGuideCS.hlsl";
	constexpr const wchar_t* kEncodeResidualPath = L"Data\\Shaders\\Upscaling\\NeuralRendering\\EncodeResidualCS.hlsl";
	constexpr const wchar_t* kApplyResidualPath = L"Data\\Shaders\\Upscaling\\NeuralRendering\\ApplyResidualCS.hlsl";

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
	NeuralRendering::SharedTexture residualInput;
	NeuralRendering::SharedTexture residualOutput;
	NeuralRendering::SharedTexture residualExposure;

	winrt::com_ptr<ID3D11ComputeShader> encodeColorCS;
	winrt::com_ptr<ID3D11ComputeShader> decodeColorCS;
	winrt::com_ptr<ID3D11ComputeShader> copyDepthGuideCS;
	winrt::com_ptr<ID3D11ComputeShader> encodeResidualCS;
	winrt::com_ptr<ID3D11ComputeShader> applyResidualCS;
	winrt::com_ptr<ID3D11Buffer> transferParamsCB;
	/// Linear clamp sampler for the jitter-compensating resample in both colour passes.
	winrt::com_ptr<ID3D11SamplerState> linearClampSampler;
	bool encodeColorAttempted = false;
	bool decodeColorAttempted = false;
	bool copyDepthGuideAttempted = false;
	bool encodeResidualAttempted = false;
	bool applyResidualAttempted = false;

	/// SRV over the Feature 18 output, consumed by the colour decode pass.
	winrt::com_ptr<ID3D11ShaderResourceView> outputSRV;
	/// SRV over the encoded model input, so the decode pass compares the answer
	/// against the exact proxy the model was given rather than a re-encode.
	winrt::com_ptr<ID3D11ShaderResourceView> colorSRV;
	/// SRV over the private DLSS-SR output carrier, consumed after the game's main SR pass.
	winrt::com_ptr<ID3D11ShaderResourceView> residualOutputSRV;
	/// SRV over the render-resolution NR result used to form the signed carrier.
	winrt::com_ptr<ID3D11ShaderResourceView> editedColorSRV;
	ID3D11Resource* editedColorSRVSource = nullptr;
	/// SRV over the caller's colour input, cached against the resource it was created from.
	winrt::com_ptr<ID3D11ShaderResourceView> colorInSRV;
	ID3D11Resource* colorInSRVSource = nullptr;
	/// SRV over the game's clean main-SR result. This is kept separate from colorInSRV
	/// because Separate Upscaling reads both resources every frame.
	winrt::com_ptr<ID3D11ShaderResourceView> cleanColorSRV;
	ID3D11Resource* cleanColorSRVSource = nullptr;
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
	bool separateResetPending = true;
	bool separateResidualReady = false;
	std::uint32_t separateOutputWidth = 0;
	std::uint32_t separateOutputHeight = 0;
	std::uint32_t separateQualityMode = UINT_MAX;
	std::uint32_t separatePreset = UINT_MAX;

	/// Colour/output and guide (depth+motion) regions NGX last saw. A change in
	/// either (dynamic resolution, or the Before/After placement toggle switching
	/// the colour input between render- and display-res) invalidates temporal
	/// history rather than smearing it into the new domain.
	std::uint32_t lastActiveWidth = 0;
	std::uint32_t lastActiveHeight = 0;
	std::uint32_t lastGuideWidth = 0;
	std::uint32_t lastGuideHeight = 0;
	std::uint32_t lastModelWidth = 0;
	std::uint32_t lastModelHeight = 0;

	/// Model raster the caller most recently asked for and how many consecutive
	/// frames it has been asked for; see SettleModelRaster.
	std::uint32_t requestedModelWidth = 0;
	std::uint32_t requestedModelHeight = 0;
	std::uint32_t requestedModelStableFrames = 0;

	/// Counts Run() calls; odd frames are skipped in alternating-frame mode.
	std::uint64_t evaluateFrameIndex = 0;

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
	 * @brief Debounces model-raster changes so a slider drag does not rebuild Feature 18 every frame.
	 *
	 * A new raster is adopted immediately when nothing is allocated yet or the
	 * colour active region itself changed (EnsureResources rebuilds then anyway).
	 * Otherwise the current allocation is kept until the request has been stable
	 * for kModelRasterDebounceFrames.
	 *
	 * @return The model raster to run this frame.
	 */
	std::pair<std::uint32_t, std::uint32_t> SettleModelRaster(std::uint32_t desiredWidth, std::uint32_t desiredHeight,
		std::uint32_t activeWidth, std::uint32_t activeHeight)
	{
		if (desiredWidth != requestedModelWidth || desiredHeight != requestedModelHeight) {
			requestedModelWidth = desiredWidth;
			requestedModelHeight = desiredHeight;
			requestedModelStableFrames = 0;
		} else if (requestedModelStableFrames < kModelRasterDebounceFrames) {
			++requestedModelStableFrames;
		}
		const bool allocated = color.resource11 && output.resource11;
		const bool activeUnchanged = activeWidth == lastActiveWidth && activeHeight == lastActiveHeight;
		if (allocated && activeUnchanged && requestedModelStableFrames < kModelRasterDebounceFrames)
			return { color.desc.Width, color.desc.Height };
		return { desiredWidth, desiredHeight };
	}

	/**
	 * @brief Creates or validates compact shared textures at the model and guide extents.
	 * @param modelWidth Model raster width the colour/output textures are allocated at.
	 * @param modelHeight Model raster height.
	 * @return False only when the descriptions cannot be read or a shared texture cannot be created.
	 */
	bool EnsureResources(const FrameInputs& inputs, std::uint32_t modelWidth, std::uint32_t modelHeight)
	{
		D3D11_TEXTURE2D_DESC colorSource{};
		D3D11_TEXTURE2D_DESC depthSource{};
		D3D11_TEXTURE2D_DESC motionSource{};
		if (!GetTextureDesc(inputs.colorIn, colorSource) || !GetTextureDesc(inputs.depth, depthSource) ||
			!GetTextureDesc(inputs.motionVectors, motionSource))
			return false;

		constexpr UINT sharedFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		const auto colorDesc = MakeSharedDesc(colorSource, colorSource.Format, sharedFlags, modelWidth, modelHeight);
		const auto outputDesc = MakeSharedDesc(colorSource, colorSource.Format, sharedFlags, modelWidth, modelHeight);
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
		separateResetPending = true;
		separateResidualReady = false;
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
		logger::info("[NeuralRendering] Shared resources allocated model={}x{} (active {}x{}) depth={}x{} motion={}x{}",
			colorDesc.Width, colorDesc.Height, inputs.width, inputs.height,
			depthDesc.Width, depthDesc.Height, motionDesc.Width, motionDesc.Height);
		return true;
	}

	/** @brief Allocate the signed carrier, its private-SR output, and fixed unit exposure. */
	bool EnsureSeparateResources(const FrameInputs& inputs)
	{
		D3D11_TEXTURE2D_DESC colorSource{};
		if (!GetTextureDesc(inputs.colorIn, colorSource) || !inputs.outputWidth || !inputs.outputHeight)
			return false;

		constexpr UINT sharedFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		const auto inputDesc = MakeSharedDesc(colorSource, DXGI_FORMAT_R16G16B16A16_FLOAT,
			sharedFlags, inputs.width, inputs.height);
		const auto outputDesc = MakeSharedDesc(colorSource, DXGI_FORMAT_R16G16B16A16_FLOAT,
			sharedFlags, inputs.outputWidth, inputs.outputHeight);
		const auto exposureDesc = MakeSharedDesc(colorSource, DXGI_FORMAT_R32_FLOAT, sharedFlags, 1, 1);
		if (Matches(residualInput, inputDesc) && Matches(residualOutput, outputDesc) &&
			Matches(residualExposure, exposureDesc) && residualOutputSRV)
			return true;

		if (!interop.WaitForIdle())
			return false;
		NeuralRendering::Runtime::Instance().ResetSuperResolutionFeature();
		residualInput = {};
		residualOutput = {};
		residualExposure = {};
		residualOutputSRV = nullptr;
		separateResidualReady = false;

		if (!interop.CreateSharedTexture(inputDesc, residualInput, "NeuralRendering::ResidualInput") ||
			!interop.CreateSharedTexture(outputDesc, residualOutput, "NeuralRendering::ResidualOutput") ||
			!interop.CreateSharedTexture(exposureDesc, residualExposure, "NeuralRendering::ResidualExposure"))
			return false;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = outputDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		if (FAILED(globals::d3d::device->CreateShaderResourceView(
				residualOutput.resource11.Get(), &srvDesc, residualOutputSRV.put())))
			return false;
		Util::SetResourceName(residualOutputSRV.get(), "NeuralRendering::ResidualOutput SRV");

		const float unitExposure[4]{ 1.0f, 0.0f, 0.0f, 0.0f };
		globals::d3d::context->ClearUnorderedAccessViewFloat(residualExposure.uav11.Get(), unitExposure);
		separateResetPending = true;
		separateOutputWidth = inputs.outputWidth;
		separateOutputHeight = inputs.outputHeight;
		logger::info("[NeuralRendering] Separate residual resources allocated {}x{} -> {}x{}",
			inputs.width, inputs.height, inputs.outputWidth, inputs.outputHeight);
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

	ID3D11ShaderResourceView* GetEditedColorSRV(ID3D11Device* device, ID3D11Resource* resource)
	{
		if (editedColorSRV && editedColorSRVSource == resource)
			return editedColorSRV.get();
		editedColorSRV = nullptr;
		editedColorSRVSource = nullptr;

		D3D11_TEXTURE2D_DESC desc{};
		if (!GetTextureDesc(resource, desc))
			return nullptr;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		if (FAILED(device->CreateShaderResourceView(resource, &srvDesc, editedColorSRV.put())))
			return nullptr;
		Util::SetResourceName(editedColorSRV.get(), "NeuralRendering::EditedColor SRV");
		editedColorSRVSource = resource;
		return editedColorSRV.get();
	}

	ID3D11ShaderResourceView* GetCleanColorSRV(ID3D11Device* device, ID3D11Resource* resource)
	{
		if (cleanColorSRV && cleanColorSRVSource == resource)
			return cleanColorSRV.get();
		cleanColorSRV = nullptr;
		cleanColorSRVSource = nullptr;

		D3D11_TEXTURE2D_DESC desc{};
		if (!GetTextureDesc(resource, desc))
			return nullptr;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		if (FAILED(device->CreateShaderResourceView(resource, &srvDesc, cleanColorSRV.put())))
			return nullptr;
		Util::SetResourceName(cleanColorSRV.get(), "NeuralRendering::CleanColor SRV");
		cleanColorSRVSource = resource;
		return cleanColorSRV.get();
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

	/// Runs an up-to-four-SRV/single-UAV compute pass over the given extent and unbinds afterwards.
	static void DispatchTransfer(ID3D11DeviceContext* context, ID3D11ComputeShader* shader,
		ID3D11ShaderResourceView* source, ID3D11ShaderResourceView* secondarySource,
		ID3D11ShaderResourceView* tertiarySource, ID3D11ShaderResourceView* quaternarySource,
		ID3D11UnorderedAccessView* destination,
		ID3D11Buffer* constants, ID3D11SamplerState* sampler,
		std::uint32_t width, std::uint32_t height)
	{
		context->CSSetShader(shader, nullptr, 0);
		ID3D11ShaderResourceView* sources[4]{ source, secondarySource, tertiarySource, quaternarySource };
		context->CSSetShaderResources(0, static_cast<UINT>(std::size(sources)), sources);
		context->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
		context->CSSetConstantBuffers(0, 1, &constants);
		context->CSSetSamplers(0, 1, &sampler);
		context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

		ID3D11ShaderResourceView* nullSRVs[4]{};
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
		                    std::isfinite(inputs.transferStrength) &&
		                    std::isfinite(inputs.jitterOffsetX) && std::isfinite(inputs.jitterOffsetY) &&
		                    std::isfinite(inputs.resolutionScaleX) && std::isfinite(inputs.resolutionScaleY) &&
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

	/**
	 * @brief Encodes the frame and guides into the shared textures and runs Feature 18 on them.
	 *
	 * Everything the model needs for one evaluation: the colour encode at the
	 * model raster, the depth-guide copy, the motion-vector copy, and the D3D12
	 * submission. Failures latch. The caller decodes the answer afterwards.
	 */
	bool EvaluateModel(const FrameInputs& inputs, ID3D11DeviceContext* context,
		ID3D11ComputeShader* encodeShader, ID3D11ComputeShader* guideShader, ID3D11ShaderResourceView* colorInView,
		std::uint32_t modelWidth, std::uint32_t modelHeight, std::uint32_t guideWidth, std::uint32_t guideHeight)
	{
		// (b) Colour moves through compute passes rather than CopyResource. The
		// encode resamples the frame onto the unjittered pixel grid at the model
		// raster so the model sees a stable framing; the decode later samples its
		// answer back at each original pixel's jittered position at the active
		// extent (see ColorTransfer.hlsli).
		DispatchTransfer(context, encodeShader, colorInView, nullptr, nullptr, nullptr, color.uav11.Get(),
			transferParamsCB.get(), linearClampSampler.get(), modelWidth, modelHeight);
		DispatchTransfer(context, guideShader, inputs.depthSRV, nullptr, nullptr, nullptr, depth.uav11.Get(),
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

		// Feature/output extents match the compact model raster. A raster change
		// rebuilds only after EnsureResources drains the interop queue. The
		// motion-vector scale is the guide resolution because Skyrim stores vectors
		// as normalized UV displacement; the NGX scale converts them to guide pixels
		// and the model bridges guide and colour rasters from the subrects, so the
		// model scale is deliberately not folded in (see neural-rendering.md). In
		// alternating-frame mode the vectors still describe one frame of motion
		// although two elapsed since the last evaluation; this matches the proxy.
		const bool executed = NeuralRendering::Runtime::Instance().Execute(commandList,
			color.resource12.Get(), depth.resource12.Get(), motionVectors.resource12.Get(), output.resource12.Get(),
			modelWidth, modelHeight, guideWidth, guideHeight, output.desc.Width, output.desc.Height,
			static_cast<float>(guideWidth), static_cast<float>(guideHeight),
			tuning, inputs.reset || resetPending);

		for (auto& barrier : barriers)
			std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);

		if (!interop.EndD3D12())
			return LatchFailure("EndD3D12", interop.LastError());
		if (!executed)
			return LatchFailure("Feature 18 execution", static_cast<HRESULT>(NeuralRendering::Runtime::Instance().NgxResult()));
		return true;
	}

	bool Run(const FrameInputs& inputs, ID3D11Device* device, ID3D11DeviceContext* context)
	{
		++evaluateFrameIndex;
		if (!interop.IsInitialized() && !InitializeInterop(device, context))
			return false;
		if (NeuralRendering::Runtime::Instance().Status() != NeuralRendering::RuntimeStatus::Initialized &&
			!InitializeRuntime())
			return false;
		// The colour/output region is the caller's active extent: dynamic resolution
		// renders into the top-left of natively sized game targets, and both colour
		// passes clamp against the real allocation. The model raster is that extent
		// scaled per axis; the shared colour/output textures are compact at the
		// model raster, the depth+motion guides at the guide extent. The two stay
		// independent because post-upscale colour is display sized.
		const std::uint32_t colorWidth = inputs.width;
		const std::uint32_t colorHeight = inputs.height;
		const std::uint32_t desiredModelWidth = ScaledExtent(colorWidth, inputs.resolutionScaleX);
		const std::uint32_t desiredModelHeight = ScaledExtent(colorHeight, inputs.resolutionScaleY);
		const auto [modelWidth, modelHeight] = SettleModelRaster(desiredModelWidth, desiredModelHeight, colorWidth, colorHeight);

		if (!EnsureResources(inputs, modelWidth, modelHeight))
			return LatchFailure("shared resource creation", interop.LastError());

		const std::uint32_t guideSrcWidth = inputs.guideWidth ? inputs.guideWidth : inputs.width;
		const std::uint32_t guideSrcHeight = inputs.guideHeight ? inputs.guideHeight : inputs.height;
		const std::uint32_t guideWidth = std::min({ guideSrcWidth, depth.desc.Width, motionVectors.desc.Width });
		const std::uint32_t guideHeight = std::min({ guideSrcHeight, depth.desc.Height, motionVectors.desc.Height });
		if (!colorWidth || !colorHeight || !guideWidth || !guideHeight || !modelWidth || !modelHeight)
			return false;

		if (colorWidth != lastActiveWidth || colorHeight != lastActiveHeight ||
			guideWidth != lastGuideWidth || guideHeight != lastGuideHeight ||
			modelWidth != lastModelWidth || modelHeight != lastModelHeight) {
			resetPending = true;
			lastActiveWidth = colorWidth;
			lastActiveHeight = colorHeight;
			lastGuideWidth = guideWidth;
			lastGuideHeight = guideHeight;
			lastModelWidth = modelWidth;
			lastModelHeight = modelHeight;
		}
		if (inputs.colorIn != lastColorInput) {
			resetPending = true;
			lastColorInput = inputs.colorIn;
		}

		// Alternating frames (the proxy's experimental "VRNR"): run the model every
		// other frame and, in between, re-apply its previous answer to the fresh
		// frame through the decode alone. The shared colour/output textures keep
		// the previous proxy/answer pair, which D3D11 already waited on when that
		// frame's D3D12 work was submitted. The first frame after a history reset,
		// a raster change or a failure always evaluates.
		const bool skipFrame = inputs.alternateFrames && featureAvailable && !resetPending && !inputs.reset &&
		                       (evaluateFrameIndex % 2) == 1;

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
		transferParams.transferStrength = std::clamp(inputs.transferStrength, 0.0f, 2.0f);
		transferParams.activeSize[0] = colorWidth;
		transferParams.activeSize[1] = colorHeight;
		transferParams.workSize[0] = modelWidth;
		transferParams.workSize[1] = modelHeight;
		transferParams.guideSize[0] = guideWidth;
		transferParams.guideSize[1] = guideHeight;
		// Silhouette fading only addresses the bilinear upsample of a reduced-
		// resolution edit; at native scale there is nothing to bleed, so leave the
		// resolve exactly as before (the proxy likewise bypasses at 1.0).
		const bool modelBelowNative = modelWidth < colorWidth || modelHeight < colorHeight;
		transferParams.depthAwareResolve = inputs.depthAwareResolve && modelBelowNative ? 1u : 0u;
		transferParams.skipFrame = skipFrame ? 1u : 0u;
		context->UpdateSubresource(transferParamsCB.get(), 0, nullptr, &transferParams, 0, 0);

		if (!skipFrame && !EvaluateModel(inputs, context, encodeShader, guideShader, colorInView,
							  modelWidth, modelHeight, guideWidth, guideHeight))
			return false;

		// Re-anchor the model's bounded luminance to the untouched source, then
		// restore its chromaticity through the independently controlled colour pass.
		// The edit is measured against the exact proxy the model received, sampled
		// at the same (jitter-compensated) position. No inverse tonemap or temporal
		// colour accumulator is involved. The game depth rides along as the
		// silhouette guide for the depth-aware resolve.
		DispatchTransfer(context, decodeShader, outputSRV.get(), colorInView, colorSRV.get(), inputs.depthSRV,
			colorOutView, transferParamsCB.get(), linearClampSampler.get(), colorWidth, colorHeight);

		resetPending = false;
		featureAvailable = true;
		return true;
	}

	bool PrepareSeparate(const FrameInputs& inputs)
	{
		separateResidualReady = false;
		if (!inputs.outputWidth || !inputs.outputHeight || inputs.outputWidth < inputs.width ||
			inputs.outputHeight < inputs.height || !inputs.superResolutionMotionVectors)
			return false;

		// First produce the normal matched-residual NR result at render resolution.
		// This writes only the caller-owned scratch texture; the game's main colour
		// remains untouched and is therefore what its regular DLSS history sees.
		if (!Evaluate(inputs))
			return false;

		auto* device = globals::d3d::device;
		auto* context = globals::d3d::context;
		if (!device || !context)
			return false;

		ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* savedDSV = nullptr;
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
		context->OMSetRenderTargets(0, nullptr, nullptr);

		const auto restoreTargets = [&]() {
			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
			for (auto*& rtv : savedRTVs) {
				if (rtv)
					rtv->Release();
			}
			if (savedDSV)
				savedDSV->Release();
		};

		if (!EnsureSeparateResources(inputs)) {
			restoreTargets();
			return LatchFailure("separate residual resource creation", interop.LastError());
		}
		if (separateQualityMode != inputs.superResolutionQualityMode ||
			separatePreset != inputs.superResolutionPreset) {
			// NGX feature handles may still be referenced by earlier command lists.
			// Drain before releasing/recreating the private history on a live setting change.
			if (!interop.WaitForIdle()) {
				restoreTargets();
				return LatchFailure("private DLSS SR settings rebuild", interop.LastError());
			}
			NeuralRendering::Runtime::Instance().ResetSuperResolutionFeature();
			separateQualityMode = inputs.superResolutionQualityMode;
			separatePreset = inputs.superResolutionPreset;
			separateResetPending = true;
		}

		auto* encodeShader = GetShader(encodeResidualCS, encodeResidualAttempted,
			kEncodeResidualPath, "EncodeResidualCS");
		auto* originalView = GetColorInSRV(device, inputs.colorIn);
		auto* editedView = GetEditedColorSRV(device, inputs.colorOut);
		if (!encodeShader || !originalView || !editedView || !residualInput.uav11) {
			restoreTargets();
			return false;
		}

		// Encode d=(NR-original) into 0.5 + 0.5*d/(1+abs(d)). The carrier is
		// deliberately independent from both scene colour histories.
		DispatchTransfer(context, encodeShader, originalView, editedView, nullptr, nullptr,
			residualInput.uav11.Get(), nullptr, nullptr, inputs.width, inputs.height);

		// Feature 18 intentionally used the raw Skyrim vectors above. The private SR
		// history instead mirrors the game's DLSS contract: the depth-dilated motion
		// field produced by EncodeTexturesCS, expressed with an NGX scale of one.
		const D3D11_BOX motionBox{ 0, 0, 0, inputs.guideWidth, inputs.guideHeight, 1 };
		context->CopySubresourceRegion(motionVectors.resource11.Get(), 0, 0, 0, 0,
			inputs.superResolutionMotionVectors, 0, &motionBox);

		ID3D12GraphicsCommandList* commandList = nullptr;
		if (!interop.BeginD3D12(&commandList) || !commandList) {
			restoreTargets();
			return LatchFailure("separate residual BeginD3D12", interop.LastError());
		}

		ID3D12Resource* resources[5]{
			residualInput.resource12.Get(), depth.resource12.Get(), motionVectors.resource12.Get(),
			residualExposure.resource12.Get(), residualOutput.resource12.Get()
		};
		D3D12_RESOURCE_BARRIER barriers[5]{};
		for (std::size_t index = 0; index < std::size(barriers); ++index) {
			barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[index].Transition.pResource = resources[index];
			barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
			barriers[index].Transition.StateAfter = index == 4 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS :
			                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		}
		commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);

		const auto result = NeuralRendering::Runtime::Instance().ExecuteSuperResolution(commandList,
			residualInput.resource12.Get(), depth.resource12.Get(), motionVectors.resource12.Get(),
			residualExposure.resource12.Get(), residualOutput.resource12.Get(),
			inputs.width, inputs.height, inputs.outputWidth, inputs.outputHeight,
			inputs.jitterOffsetX, inputs.jitterOffsetY,
			1.0f, 1.0f,
			globals::game::deltaTime ? *globals::game::deltaTime * 1000.0f : 16.6667f,
			inputs.superResolutionQualityMode, inputs.superResolutionPreset,
			inputs.reset || separateResetPending);

		for (auto& barrier : barriers)
			std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
		const bool submitted = interop.EndD3D12();
		restoreTargets();
		if (!submitted)
			return LatchFailure("separate residual EndD3D12", interop.LastError());
		if (result == NeuralRendering::SuperResolutionResult::Failed)
			return LatchFailure("private DLSS SR execution",
				static_cast<HRESULT>(NeuralRendering::Runtime::Instance().NgxResult()));
		if (result == NeuralRendering::SuperResolutionResult::Created) {
			logger::info("[NeuralRendering] Private DLSS SR created; clean main-SR frame retained during initialization");
			return false;
		}

		separateResetPending = false;
		separateResidualReady = true;
		return true;
	}

	bool ResolveSeparate(ID3D11Resource* cleanColor, ID3D11Resource* colorOut,
		std::uint32_t width, std::uint32_t height)
	{
		if (!separateResidualReady || !cleanColor || !colorOut || cleanColor == colorOut ||
			width != separateOutputWidth || height != separateOutputHeight || !residualOutputSRV)
			return false;
		separateResidualReady = false;  // The residual belongs to exactly one main-SR result.

		auto* device = globals::d3d::device;
		auto* context = globals::d3d::context;
		if (!device || !context)
			return false;
		auto* applyShader = GetShader(applyResidualCS, applyResidualAttempted,
			kApplyResidualPath, "ApplyResidualCS");
		auto* cleanView = GetCleanColorSRV(device, cleanColor);
		auto* outputView = GetColorOutUAV(device, colorOut);
		if (!applyShader || !cleanView || !outputView)
			return false;

		ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* savedDSV = nullptr;
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
		context->OMSetRenderTargets(0, nullptr, nullptr);
		DispatchTransfer(context, applyShader, cleanView, residualOutputSRV.get(), nullptr, nullptr,
			outputView, nullptr, nullptr, width, height);
		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
		for (auto*& rtv : savedRTVs) {
			if (rtv)
				rtv->Release();
		}
		if (savedDSV)
			savedDSV->Release();
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
		residualInput = {};
		residualOutput = {};
		residualExposure = {};

		outputSRV = nullptr;
		colorSRV = nullptr;
		residualOutputSRV = nullptr;
		editedColorSRV = nullptr;
		editedColorSRVSource = nullptr;
		colorInSRV = nullptr;
		colorInSRVSource = nullptr;
		cleanColorSRV = nullptr;
		cleanColorSRVSource = nullptr;
		colorOutUAV = nullptr;
		colorOutUAVSource = nullptr;
		lastColorInput = nullptr;

		encodeColorCS = nullptr;
		decodeColorCS = nullptr;
		copyDepthGuideCS = nullptr;
		encodeResidualCS = nullptr;
		applyResidualCS = nullptr;
		transferParamsCB = nullptr;
		linearClampSampler = nullptr;
		encodeColorAttempted = false;
		decodeColorAttempted = false;
		copyDepthGuideAttempted = false;
		encodeResidualAttempted = false;
		applyResidualAttempted = false;

		failureLatched = false;
		featureAvailable = false;
		resetPending = true;
		separateResetPending = true;
		separateResidualReady = false;
		separateOutputWidth = 0;
		separateOutputHeight = 0;
		separateQualityMode = UINT_MAX;
		separatePreset = UINT_MAX;
		lastActiveWidth = 0;
		lastActiveHeight = 0;
		lastGuideWidth = 0;
		lastGuideHeight = 0;
		lastModelWidth = 0;
		lastModelHeight = 0;
		requestedModelWidth = 0;
		requestedModelHeight = 0;
		requestedModelStableFrames = 0;
		evaluateFrameIndex = 0;

		loggedInvalidInputs = false;
		loggedShaderFailure = false;
		loggedViewFailure = false;
	}

	void Destroy()
	{
		interop.WaitForIdle();
		// Placement/upscaler changes happen while Streamline's process-wide NGX core
		// is live. Shutting down our D3D12 NGX instance here can enter the shared
		// core while the game is rendering (and has been observed to fault inside
		// NVSDK_NGX_D3D12_Shutdown1). Retire only the two private feature histories;
		// the runtime and interop device remain valid for the next placement.
		NeuralRendering::Runtime::Instance().ResetFeature();
		ReleaseGpuResources();
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

bool NeuralRenderingBackend::PrepareSeparateUpscaling(const FrameInputs& inputs)
{
	return state->PrepareSeparate(inputs);
}

bool NeuralRenderingBackend::ResolveSeparateUpscaling(ID3D11Resource* cleanColor, ID3D11Resource* colorOut,
	std::uint32_t width, std::uint32_t height)
{
	return state->ResolveSeparate(cleanColor, colorOut, width, height);
}

void NeuralRenderingBackend::DestroyResources()
{
	state->Destroy();
}
