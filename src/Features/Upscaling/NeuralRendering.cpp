#include "NeuralRendering.h"

#include <cmath>
#include <limits>
#include <utility>

#include <Windows.h>
#include <d3d11.h>
#include <winrt/base.h>

#include "../../Globals.h"
#include "../../Utils/D3D.h"

// These declarations are used for their ABI types only. Every NGX function is
// resolved from the already-initialized runtime, so this translation unit never
// links an NGX import or static library.
#include <nvsdk_ngx.h>

namespace
{
	constexpr NVSDK_NGX_Feature kNeuralRenderingFeature = static_cast<NVSDK_NGX_Feature>(18);
	constexpr UINT kNodeMask = 1;
	constexpr float kMaximumResolutionScale = 8.0f;

	constexpr const char* kWidth = "DLSSNR.Width";
	constexpr const char* kHeight = "DLSSNR.Height";
	constexpr const char* kEnabled = "DLSSNR.Enabled";
	constexpr const char* kScalingRatio = "DLSSNR.ScalingRatio";
	constexpr const char* kRenderPreset = "DLSSNR.Hint.Render.Preset";
	constexpr const char* kDepthInverted = "DLSSNR.DepthInverted";
	constexpr const char* kUseAutoMask = "DLSSNR.UseAutoMask";
	constexpr const char* kStyle = "DLSSNR.Style";
	constexpr const char* kIntensity = "DLSSNR.Intensity";
	constexpr const char* kLocalToneStrength = "DLSSNR.LocalToneStrength";
	constexpr const char* kLocalStructureStrength = "DLSSNR.LocalStructureStrength";
	constexpr const char* kSkinStructureStrength = "DLSSNR.SkinStructureStrength";
	constexpr const char* kMVecScaleX = "DLSSNR.MVecScaleX";
	constexpr const char* kMVecScaleY = "DLSSNR.MVecScaleY";
	constexpr const char* kReset = "DLSSNR.Reset";
	constexpr const char* kColor = "DLSSNR.Color";
	constexpr const char* kOutput = "DLSSNR.Output";
	constexpr const char* kDepth = "DLSSNR.Depth";
	constexpr const char* kMVec = "DLSSNR.MVec";

	using AllocateParametersFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
	using DestroyParametersFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
	using GetScratchBufferSizeFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, size_t*);
	using CreateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D11DeviceContext*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
	using EvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D11DeviceContext*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
	using ReleaseFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

	bool IsSuccess(NVSDK_NGX_Result result)
	{
		return NVSDK_NGX_SUCCEED(result);
	}

	bool HasFiniteOptions(const NeuralRendering::Options& options)
	{
		return std::isfinite(options.resolutionScale) && std::isfinite(options.intensity) &&
			std::isfinite(options.localToneStrength) && std::isfinite(options.localStructureStrength) &&
			std::isfinite(options.skinStructureStrength);
	}
}

struct NeuralRendering::Impl
{
	HMODULE runtime = nullptr;
	bool bindAttempted = false;
	bool available = false;
	bool loggedCoreUnavailable = false;
	bool loggedEntryPointsUnavailable = false;
	bool loggedRuntimeUnavailable = false;
	bool loggedInvalidInputs = false;
	bool loggedCreateFailure = false;
	bool loggedEvaluateFailure = false;
	bool loggedScratchFailure = false;

	AllocateParametersFn allocateParameters = nullptr;
	DestroyParametersFn destroyParameters = nullptr;
	GetScratchBufferSizeFn getScratchBufferSize = nullptr;
	CreateFeatureFn createFeature = nullptr;
	EvaluateFeatureFn evaluateFeature = nullptr;
	ReleaseFeatureFn releaseFeature = nullptr;

	NVSDK_NGX_Parameter* parameters = nullptr;
	NVSDK_NGX_Handle* feature = nullptr;
	winrt::com_ptr<ID3D11Buffer> scratch;

	uint32_t width = 0;
	uint32_t height = 0;
	float resolutionScale = 0.0f;
	uint32_t preset = 0;
	uint32_t style = 0;
	bool createFailed = false;
	uint32_t failedWidth = 0;
	uint32_t failedHeight = 0;
	float failedResolutionScale = 0.0f;
	uint32_t failedPreset = 0;
	uint32_t failedStyle = 0;

	~Impl()
	{
		DestroyResources();
	}

	bool BindRuntime()
	{
		if (available)
			return available;
		if (bindAttempted && runtime)
			return false;

		bindAttempted = true;
		runtime = GetModuleHandleW(L"_nvngx.dll");
		if (!runtime)
			runtime = GetModuleHandleW(L"nvngx.dll");

		if (!runtime) {
			if (!loggedCoreUnavailable) {
				logger::warn("[NeuralRendering] Streamline has not loaded an NGX core (_nvngx.dll); initialize Streamline before enabling Neural Rendering.");
				loggedCoreUnavailable = true;
			}
			return false;
		}

		allocateParameters = reinterpret_cast<AllocateParametersFn>(GetProcAddress(runtime, "NVSDK_NGX_D3D11_AllocateParameters"));
		destroyParameters = reinterpret_cast<DestroyParametersFn>(GetProcAddress(runtime, "NVSDK_NGX_D3D11_DestroyParameters"));
		getScratchBufferSize = reinterpret_cast<GetScratchBufferSizeFn>(GetProcAddress(runtime, "NVSDK_NGX_D3D11_GetScratchBufferSize"));
		createFeature = reinterpret_cast<CreateFeatureFn>(GetProcAddress(runtime, "NVSDK_NGX_D3D11_CreateFeature"));
		evaluateFeature = reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(runtime, "NVSDK_NGX_D3D11_EvaluateFeature"));
		releaseFeature = reinterpret_cast<ReleaseFeatureFn>(GetProcAddress(runtime, "NVSDK_NGX_D3D11_ReleaseFeature"));

		available = allocateParameters && destroyParameters && getScratchBufferSize && createFeature && evaluateFeature && releaseFeature;
		if (!available && !loggedEntryPointsUnavailable) {
			logger::warn("[NeuralRendering] Required D3D11 NGX exports are missing; update the NVIDIA driver/Streamline runtime.");
			loggedEntryPointsUnavailable = true;
		}
		return available;
	}

	void ClearCreateFailureLatch()
	{
		createFailed = false;
		failedWidth = 0;
		failedHeight = 0;
		failedResolutionScale = 0.0f;
		failedPreset = 0;
		failedStyle = 0;
	}

	void LatchCreateFailure(uint32_t newWidth, uint32_t newHeight, const Options& options)
	{
		createFailed = true;
		failedWidth = newWidth;
		failedHeight = newHeight;
		failedResolutionScale = options.resolutionScale;
		failedPreset = options.preset;
		failedStyle = options.style;
	}

	bool HasLatchedCreateFailure(uint32_t newWidth, uint32_t newHeight, const Options& options) const
	{
		return createFailed && failedWidth == newWidth && failedHeight == newHeight &&
			failedResolutionScale == options.resolutionScale && failedPreset == options.preset && failedStyle == options.style;
	}

	void DestroyResources(bool clearCreateFailureLatch = true)
	{
		if (feature && releaseFeature)
			releaseFeature(feature);
		feature = nullptr;
		scratch = nullptr;

		if (parameters && destroyParameters)
			destroyParameters(parameters);
		parameters = nullptr;

		width = 0;
		height = 0;
		resolutionScale = 0.0f;
		preset = 0;
		style = 0;
		if (clearCreateFailureLatch)
			ClearCreateFailureLatch();
	}

	bool AllocateParameterMap()
	{
		if (parameters)
			return true;

		const NVSDK_NGX_Result result = allocateParameters(&parameters);
		if (IsSuccess(result) && parameters)
			return true;

		parameters = nullptr;
		if (!loggedRuntimeUnavailable) {
			logger::warn("[NeuralRendering] NGX D3D11 runtime is not ready (NGX result 0x{:08X}); ensure Streamline has initialized NGX before enabling Neural Rendering.", static_cast<uint32_t>(result));
			loggedRuntimeUnavailable = true;
		}
		return false;
	}

	void SetCreateParameters(uint32_t newWidth, uint32_t newHeight, const Options& options)
	{
		parameters->Reset();
		parameters->Set(kWidth, newWidth);
		parameters->Set(kHeight, newHeight);
		parameters->Set(kEnabled, 1u);
		parameters->Set(kScalingRatio, options.resolutionScale);
		parameters->Set(kRenderPreset, options.preset);
		parameters->Set(kDepthInverted, 0u);
		parameters->Set(kUseAutoMask, options.automaticMask ? 1u : 0u);
		parameters->Set(kStyle, options.style);
		parameters->Set(kIntensity, options.intensity);
		parameters->Set(kLocalToneStrength, options.localToneStrength);
		parameters->Set(kLocalStructureStrength, options.localStructureStrength);
		parameters->Set(kSkinStructureStrength, options.skinStructureStrength);
		parameters->Set(kMVecScaleX, 1.0f);
		parameters->Set(kMVecScaleY, 1.0f);
		parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, kNodeMask);
		parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, kNodeMask);
	}

	bool CreateScratchBuffer(ID3D11Device* device, size_t sizeInBytes)
	{
		if (sizeInBytes == 0)
			return true;
		if (sizeInBytes > std::numeric_limits<UINT>::max()) {
			if (!loggedScratchFailure) {
				logger::warn("[NeuralRendering] NGX requested an unsupported scratch-buffer size; Neural Rendering is disabled.");
				loggedScratchFailure = true;
			}
			return false;
		}

		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = static_cast<UINT>(sizeInBytes);
		desc.Usage = D3D11_USAGE_DEFAULT;
		winrt::com_ptr<ID3D11Buffer> newScratch;
		if (FAILED(device->CreateBuffer(&desc, nullptr, newScratch.put()))) {
			if (!loggedScratchFailure) {
				logger::warn("[NeuralRendering] Failed to allocate NGX scratch buffer; Neural Rendering is disabled.");
				loggedScratchFailure = true;
			}
			return false;
		}

		Util::SetResourceName(newScratch.get(), "NeuralRendering::NGXScratch");
		scratch = std::move(newScratch);
		parameters->Set(NVSDK_NGX_Parameter_Scratch, static_cast<ID3D11Resource*>(scratch.get()));
		parameters->Set(NVSDK_NGX_Parameter_Scratch_SizeInBytes, static_cast<unsigned long long>(sizeInBytes));
		return true;
	}

	bool Create(ID3D11Device* device, ID3D11DeviceContext* context,
		uint32_t newWidth, uint32_t newHeight, const Options& options)
	{
		SetCreateParameters(newWidth, newHeight, options);

		size_t scratchSize = 0;
		const NVSDK_NGX_Result scratchResult = getScratchBufferSize(kNeuralRenderingFeature, parameters, &scratchSize);
		if (!IsSuccess(scratchResult)) {
			if (!loggedScratchFailure) {
				logger::warn("[NeuralRendering] NGX could not determine scratch requirements (NGX result 0x{:08X}); verify nvngx_dlssnr.dll matches the driver.", static_cast<uint32_t>(scratchResult));
				loggedScratchFailure = true;
			}
			LatchCreateFailure(newWidth, newHeight, options);
			return false;
		}
		if (!CreateScratchBuffer(device, scratchSize)) {
			LatchCreateFailure(newWidth, newHeight, options);
			return false;
		}

		const NVSDK_NGX_Result result = createFeature(context, kNeuralRenderingFeature, parameters, &feature);
		if (!IsSuccess(result) || !feature) {
			feature = nullptr;
			if (!loggedCreateFailure) {
				logger::warn("[NeuralRendering] Feature 18 creation failed (NGX result 0x{:08X}); install a compatible user-supplied nvngx_dlssnr.dll in Streamline's plugin directory.", static_cast<uint32_t>(result));
				loggedCreateFailure = true;
			}
			LatchCreateFailure(newWidth, newHeight, options);
			return false;
		}

		width = newWidth;
		height = newHeight;
		resolutionScale = options.resolutionScale;
		preset = options.preset;
		style = options.style;
		return true;
	}

	bool RequiresRecreate(uint32_t newWidth, uint32_t newHeight, const Options& options) const
	{
		return !feature || width != newWidth || height != newHeight ||
			resolutionScale != options.resolutionScale || preset != options.preset || style != options.style;
	}

	bool Evaluate(ID3D11Resource* colorIn, ID3D11Resource* colorOut,
		ID3D11Resource* depth, ID3D11Resource* motionVectors,
		uint32_t newWidth, uint32_t newHeight, const Options& options)
	{
		if (!BindRuntime() || !globals::d3d::device || !globals::d3d::context)
			return false;
		if (!colorIn || !colorOut || !depth || !motionVectors ||
			colorIn == colorOut || colorIn == depth || colorIn == motionVectors ||
			colorOut == depth || colorOut == motionVectors || depth == motionVectors ||
			!newWidth || !newHeight || !HasFiniteOptions(options) ||
			options.resolutionScale <= 0.0f || options.resolutionScale > kMaximumResolutionScale) {
			if (!loggedInvalidInputs) {
				logger::warn("[NeuralRendering] Invalid or aliased D3D11 resources/settings; Neural Rendering evaluation was skipped.");
				loggedInvalidInputs = true;
			}
			return false;
		}
		if (!AllocateParameterMap())
			return false;
		if (HasLatchedCreateFailure(newWidth, newHeight, options))
			return false;
		if (createFailed)
			ClearCreateFailureLatch();

		if (RequiresRecreate(newWidth, newHeight, options)) {
			DestroyResources(false);
			if (!AllocateParameterMap() || !Create(globals::d3d::device, globals::d3d::context, newWidth, newHeight, options))
				return false;
		}

		parameters->Set(kWidth, newWidth);
		parameters->Set(kHeight, newHeight);
		parameters->Set(kEnabled, 1u);
		parameters->Set(kScalingRatio, options.resolutionScale);
		parameters->Set(kRenderPreset, options.preset);
		parameters->Set(kDepthInverted, 0u);
		parameters->Set(kUseAutoMask, options.automaticMask ? 1u : 0u);
		parameters->Set(kStyle, options.style);
		parameters->Set(kIntensity, options.intensity);
		parameters->Set(kLocalToneStrength, options.localToneStrength);
		parameters->Set(kLocalStructureStrength, options.localStructureStrength);
		parameters->Set(kSkinStructureStrength, options.skinStructureStrength);
		parameters->Set(kMVecScaleX, 1.0f);
		parameters->Set(kMVecScaleY, 1.0f);
		parameters->Set(kReset, options.reset ? 1u : 0u);
		parameters->Set(kColor, colorIn);
		parameters->Set(kOutput, colorOut);
		parameters->Set(kDepth, depth);
		parameters->Set(kMVec, motionVectors);
		parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, kNodeMask);
		parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, kNodeMask);

		const NVSDK_NGX_Result result = evaluateFeature(globals::d3d::context, feature, parameters, nullptr);
		if (IsSuccess(result))
			return true;

		if (!loggedEvaluateFailure) {
			logger::warn("[NeuralRendering] Feature 18 evaluation failed (NGX result 0x{:08X}); verify resource formats and the installed nvngx_dlssnr.dll.", static_cast<uint32_t>(result));
			loggedEvaluateFailure = true;
		}
		return false;
	}
};

NeuralRendering::NeuralRendering() :
	impl(std::make_unique<Impl>())
{}

NeuralRendering::~NeuralRendering() = default;

bool NeuralRendering::IsAvailable() const
{
	return impl->BindRuntime();
}

bool NeuralRendering::IsFeatureAvailable() const
{
	return impl->feature != nullptr;
}

bool NeuralRendering::Evaluate(ID3D11Resource* colorIn, ID3D11Resource* colorOut,
	ID3D11Resource* depth, ID3D11Resource* motionVectors,
	uint32_t width, uint32_t height, const Options& options)
{
	return impl->Evaluate(colorIn, colorOut, depth, motionVectors, width, height, options);
}

void NeuralRendering::DestroyResources()
{
	impl->DestroyResources();
}
