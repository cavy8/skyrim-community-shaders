#include "Backend.h"

#include "D3D12Interop.h"
#include "Runtime.h"

#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iterator>
#include <utility>
#include <vector>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <winrt/base.h>

#include <algorithm>
#include <initializer_list>

namespace
{
	/// Shared by every transfer pass (register b0); mirrored by NeuralRendering/TransferParams.hlsli.
	struct alignas(16) TransferParams
	{
		float jitterOffset[2]{};  ///< Sub-pixel projection offset of the colour raster, in render pixels.
		float colorStrength = 1.0f;
		float transferStrength = 1.0f;        ///< Overall edit weight; one reproduces the model's change exactly.
		std::uint32_t activeSize[2]{};        ///< Colour/output active region, in colour texels.
		std::uint32_t workSize[2]{};          ///< Model raster; the shared colour/output textures are this size.
		std::uint32_t guideSize[2]{};         ///< Depth guide active region, in guide texels.
		std::uint32_t depthAwareResolve = 0;  ///< Non-zero: fade the edit across depth silhouettes in the decode.
		std::uint32_t staleAnswer = 0;        ///< Non-zero: the decode reprojects the previous frame's answer.
		std::uint32_t hueGuardMask = 0;       ///< Bit i set: category i (NeuralRendering::MaterialCategory) hue-guards its chroma change.
		float guideJitterOffset[2]{};         ///< Projection offset of the guide rasters relative to the colour raster, in guide texels.
		std::uint32_t colorDomain = 0;        ///< NeuralRendering::ColorDomain: how the colour input is encoded.
		float categoryColorStrengths[8]{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		float categoryTransferStrengths[8]{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		float categoryLuminosityStrengths[8]{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		// Display transform of the scene-linear proxy (ColorTransfer.hlsli, MakeNeuralDisplayTransform).
		float displayParam[4]{};                              ///< x vanilla grading on/off, y ISHDR Param.y, z ISHDR Param.z.
		float displayCinematic[4]{ 1.0f, 0.0f, 1.0f, 1.0f };  ///< ISHDR Cinematic.
		float displayTint[4]{ 1.0f, 1.0f, 1.0f, 0.0f };       ///< ISHDR Tint.
		float displayExposure[4]{ 0.0f, 0.18f, 0.0f, 1.0f };  ///< x Post Processing exposure on/off, y scale, zw range.
		/// Multiplier on the smooth half of the model's luminance change. This is the slot the
		/// single Luminosity Strength used to occupy, and with Detail equal to it the maths is
		/// identical, so an upgraded config resolves to exactly the same edit.
		float broadLuminosity = 1.0f;
		std::uint32_t debugCategoryView = 0;  ///< Non-zero: the decode renders the classified category, not the model's edit.
		float maxRatio = 2.0f;                ///< Two-sided guard on the model/proxy luminance ratio (1/maxRatio..maxRatio).
		std::uint32_t rawModelOutput = 0;     ///< Non-zero: the decode writes Feature 18's answer directly (Finished Image diagnostic).
		float highlightWhite = 0.0f;          ///< Display gamma: display peak for the HDR highlight shoulder; 0 = none.
		float wipePosition = -1.0f;           ///< Split-screen comparison split (fraction of the width); negative = off.
		std::uint32_t proxyCurve = 0;         ///< NeuralRendering::ProxyCurve: how the scene-linear proxy is built.
		std::uint32_t debugFlags = 0;         ///< kNeuralDebug* bits (ColorTransfer.hlsli).
		/// x Detail Luminosity, y band radius in model texels, z band data present, w spare.
		float bandParams[4]{ 1.0f, 8.0f, 0.0f, 0.0f };
	};
	static_assert(sizeof(TransferParams) == 272);

	/// kNeuralDebug* in ColorTransfer.hlsli; keep the two in sync.
	constexpr std::uint32_t kDebugFlagGuardClamp = 1u << 0;
	constexpr std::uint32_t kDebugFlagBroadBand = 1u << 1;
	constexpr std::uint32_t kDebugFlagDetailBand = 1u << 2;
	constexpr std::uint32_t kDebugFlagStats = 1u << 4;

	/// DebugStats slots, matching DecodeColorCS's RWStructuredBuffer<uint>.
	constexpr std::uint32_t kDebugStatClampedSamples = 0;
	constexpr std::uint32_t kDebugStatSamples = 1;
	constexpr std::uint32_t kDebugStatPeakBits = 2;
	constexpr std::uint32_t kDebugStatCount = 4;
	/// Frames the staged readback trails the GPU by, so a Map never waits on it.
	constexpr std::size_t kDebugReadbackFrames = 3;

	constexpr float kMinimumResolutionScale = 0.25f;
	/// Native. Supersampling the model (scale above one) was removed: it cost the
	/// square of the scale for no visible gain once the edit is applied as a ratio
	/// to the untouched full-resolution frame.
	constexpr float kMaximumResolutionScale = 1.0f;
	/// Feature 18 is not created below this per-axis extent.
	constexpr std::uint32_t kMinimumModelExtent = 64;
	/// Frames a changed model raster must stay stable before the shared textures
	/// and the NGX feature are rebuilt for it. Rebuilding drains the interop queue,
	/// so applying every intermediate value of a slider drag would hitch per frame.
	constexpr std::uint32_t kModelRasterDebounceFrames = 12;
	/// Frames a changed tuning value must stay stable before Feature 18 is torn
	/// down and recreated for it (see SettleTuning). Rebuilding drains the interop
	/// queue, so latching every intermediate value of a slider drag would hitch
	/// per frame; matches kModelRasterDebounceFrames's reasoning exactly.
	constexpr std::uint32_t kTuningDebounceFrames = 12;
	/// Sent to the shader in place of a real Max Ratio when the ratio guard is
	/// off (NeuralRendering::Options::ratioGuardEnabled false). ResolveNeuralColor
	/// only ever uses this as clamp(ratio, 1/x, x); a value this large makes that
	/// clamp a no-op for any luminance ratio the model could plausibly produce,
	/// without the shader needing a separate enabled flag.
	constexpr float kNeuralRatioGuardDisabledValue = 1.0e6f;

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

	constexpr const wchar_t* kEncodeColorPath = L"Data\\Shaders\\NeuralRendering\\EncodeColorCS.hlsl";
	constexpr const wchar_t* kDecodeColorPath = L"Data\\Shaders\\NeuralRendering\\DecodeColorCS.hlsl";
	constexpr const wchar_t* kPrepareToneDataPath = L"Data\\Shaders\\NeuralRendering\\PrepareToneDataCS.hlsl";
	constexpr const wchar_t* kFilterToneDataPath = L"Data\\Shaders\\NeuralRendering\\FilterToneDataCS.hlsl";
	constexpr const wchar_t* kCopyDepthGuidePath = L"Data\\Shaders\\NeuralRendering\\CopyDepthGuideCS.hlsl";
	constexpr const wchar_t* kEncodeResidualPath = L"Data\\Shaders\\NeuralRendering\\EncodeResidualCS.hlsl";
	constexpr const wchar_t* kApplyResidualPath = L"Data\\Shaders\\NeuralRendering\\ApplyResidualCS.hlsl";

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

	bool Matches(const NeuralRenderingNGX::SharedTexture& texture, const D3D11_TEXTURE2D_DESC& desc)
	{
		return texture.resource11 && texture.resource12 &&
		       texture.desc.Width == desc.Width && texture.desc.Height == desc.Height &&
		       texture.desc.Format == desc.Format;
	}
}

struct NeuralRenderingBackend::State
{
	NeuralRenderingNGX::D3D12Interop interop;

	NeuralRenderingNGX::SharedTexture color;
	NeuralRenderingNGX::SharedTexture depth;
	NeuralRenderingNGX::SharedTexture motionVectors;
	NeuralRenderingNGX::SharedTexture output;
	NeuralRenderingNGX::SharedTexture residualInput;
	NeuralRenderingNGX::SharedTexture residualOutput;
	NeuralRenderingNGX::SharedTexture residualExposure;

	winrt::com_ptr<ID3D11ComputeShader> encodeColorCS;
	winrt::com_ptr<ID3D11ComputeShader> decodeColorCS;
	winrt::com_ptr<ID3D11ComputeShader> copyDepthGuideCS;
	winrt::com_ptr<ID3D11ComputeShader> encodeResidualCS;
	winrt::com_ptr<ID3D11ComputeShader> applyResidualCS;
	winrt::com_ptr<ID3D11ComputeShader> prepareToneDataCS;
	winrt::com_ptr<ID3D11ComputeShader> filterToneDataHorizontalCS;
	winrt::com_ptr<ID3D11ComputeShader> filterToneDataVerticalCS;
	winrt::com_ptr<ID3D11Buffer> transferParamsCB;
	/// Linear clamp sampler for the jitter-compensating resample in both colour passes.
	winrt::com_ptr<ID3D11SamplerState> linearClampSampler;
	bool encodeColorAttempted = false;
	bool decodeColorAttempted = false;
	bool copyDepthGuideAttempted = false;
	bool encodeResidualAttempted = false;
	bool applyResidualAttempted = false;
	bool prepareToneDataAttempted = false;
	bool filterToneDataHorizontalAttempted = false;
	bool filterToneDataVerticalAttempted = false;

	/**
	 * Band split scratch, at the model raster and private to D3D11 (never shared with D3D12).
	 * `toneData` holds (log2 proxy luminance, the edit in stops) from PrepareToneDataCS; the
	 * horizontal filter writes `toneScratch` and the vertical one writes back into `toneData`,
	 * which is what the decode then samples. Two textures are enough because no pass ever
	 * reads the target it is writing.
	 */
	winrt::com_ptr<ID3D11Texture2D> toneData;
	winrt::com_ptr<ID3D11ShaderResourceView> toneDataSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> toneDataUAV;
	winrt::com_ptr<ID3D11Texture2D> toneScratch;
	winrt::com_ptr<ID3D11ShaderResourceView> toneScratchSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> toneScratchUAV;
	std::uint32_t toneWidth = 0;
	std::uint32_t toneHeight = 0;
	/// True once the band passes have written data for the live model raster; cleared whenever
	/// the textures are (re)built, so the first decode after a resize never reads noise.
	bool toneDataValid = false;
	bool loggedToneFailure = false;

	/**
	 * Debug readback (Model Contract probe): DecodeColorCS accumulates a clamp count, a sample
	 * count and a peak into `debugStats`, which is copied into a small ring of staging buffers
	 * and mapped kDebugReadbackFrames later, so the CPU never waits on the GPU.
	 */
	winrt::com_ptr<ID3D11Buffer> debugStats;
	winrt::com_ptr<ID3D11UnorderedAccessView> debugStatsUAV;
	std::array<winrt::com_ptr<ID3D11Buffer>, kDebugReadbackFrames> debugStaging;
	std::array<bool, kDebugReadbackFrames> debugStagingPending{};
	std::size_t debugStagingSlot = 0;
	NeuralRenderingBackend::DebugReadback debugReadback{};

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

	/// Tuning last requested and how many consecutive frames it has been asked
	/// for (see SettleTuning), versus the tuning actually latched into the live
	/// Feature 18 handle. DLSSNR.Intensity/Style/LocalToneStrength/
	/// LocalStructureStrength/SkinStructureStrength/UseAutoMask only take effect
	/// at feature creation (see Runtime::Execute), so a settled change here has
	/// to force a recreate rather than just flow through to the next Execute().
	NeuralRenderingNGX::Tuning requestedTuning{};
	NeuralRenderingNGX::Tuning appliedTuning{};
	std::uint32_t requestedTuningStableFrames = 0;
	bool tuningInitialized = false;

	/// Counts Run() calls; odd frames are skipped in alternating-frame mode.
	std::uint64_t evaluateFrameIndex = 0;
	/// evaluateFrameIndex of the last frame Feature 18 actually ran on (zero: none since
	/// the resources were built). Tells an evaluation how many frames of motion the
	/// model's temporal history has to bridge.
	std::uint64_t lastEvaluatedFrameIndex = 0;

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
			probeSucceeded = NeuralRenderingNGX::Runtime::Instance().Probe();
			if (!probeSucceeded && !loggedProbeFailure) {
				loggedProbeFailure = true;
				logger::warn("[NeuralRendering] Runtime unavailable (status={} detail={}); install a compatible nvngx_dlssnr.dll to enable Neural Rendering.",
					NeuralRenderingNGX::ToString(NeuralRenderingNGX::Runtime::Instance().Status()),
					NeuralRenderingNGX::Runtime::Instance().Detail());
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
			NeuralRenderingNGX::ToString(NeuralRenderingNGX::Runtime::Instance().Status()),
			NeuralRenderingNGX::Runtime::Instance().Detail());
		return false;
	}

	ID3D11ComputeShader* GetShader(winrt::com_ptr<ID3D11ComputeShader>& slot, bool& attempted,
		const wchar_t* path, const char* label,
		const std::vector<std::pair<const char*, const char*>>& defines = {})
	{
		if (!attempted) {
			attempted = true;
			slot.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(path, defines, "cs_5_0")));
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
		auto& runtime = NeuralRenderingNGX::Runtime::Instance();
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
	 * @brief Debounces a Feature-18 tuning change and reports when it should be latched in.
	 *
	 * DLSSNR.Intensity/Style/LocalToneStrength/LocalStructureStrength/
	 * SkinStructureStrength/UseAutoMask only take effect when Feature 18 is
	 * (re)created (see Runtime::Execute), so applying every intermediate value of
	 * a slider drag would tear the feature down and rebuild it - and drain the
	 * interop queue to do it safely - every frame. Instead the request is tracked
	 * the same way SettleModelRaster tracks a resolution-scale drag: once it has
	 * been stable for kTuningDebounceFrames and actually differs from what is
	 * latched into the live handle, the caller is told to recreate.
	 *
	 * @return True exactly once per settled change; the caller must then drain
	 *         the interop queue, call Runtime::ResetFeature(), and update
	 *         appliedTuning - never release the handle without draining first.
	 */
	bool SettleTuning(const NeuralRenderingNGX::Tuning& desired)
	{
		if (!(desired == requestedTuning)) {
			requestedTuning = desired;
			requestedTuningStableFrames = 0;
		} else if (requestedTuningStableFrames < kTuningDebounceFrames) {
			++requestedTuningStableFrames;
		}
		if (!tuningInitialized) {
			tuningInitialized = true;
			appliedTuning = requestedTuning;
			return false;
		}
		if (requestedTuningStableFrames >= kTuningDebounceFrames && !(requestedTuning == appliedTuning)) {
			appliedTuning = requestedTuning;
			return true;
		}
		return false;
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
		NeuralRenderingNGX::Runtime::Instance().ResetFeature();
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
		NeuralRenderingNGX::Runtime::Instance().ResetSuperResolutionFeature();
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

	/**
	 * @brief Allocates the two band-split scratch textures at the model raster.
	 *
	 * Private to D3D11: nothing here is shared with D3D12, so these are ordinary textures
	 * rather than interop allocations. A failure is logged once and turns the split off for
	 * the session rather than failing the frame - the resolve falls back to the unsplit edit,
	 * which is what equal Broad/Detail values produce anyway.
	 *
	 * @return False when the textures are unavailable; the caller must then leave the band
	 *         data flagged absent.
	 */
	bool EnsureToneResources(std::uint32_t modelWidth, std::uint32_t modelHeight)
	{
		if (toneData && toneScratch && toneWidth == modelWidth && toneHeight == modelHeight)
			return true;
		if (loggedToneFailure)
			return false;

		toneData = nullptr;
		toneDataSRV = nullptr;
		toneDataUAV = nullptr;
		toneScratch = nullptr;
		toneScratchSRV = nullptr;
		toneScratchUAV = nullptr;
		toneWidth = 0;
		toneHeight = 0;
		toneDataValid = false;
		if (!modelWidth || !modelHeight)
			return false;

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = modelWidth;
		desc.Height = modelHeight;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		// Half precision resolves about 0.01 stops, far finer than the edit it carries.
		desc.Format = DXGI_FORMAT_R16G16_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		auto* device = globals::d3d::device;
		const auto create = [&](winrt::com_ptr<ID3D11Texture2D>& texture,
								winrt::com_ptr<ID3D11ShaderResourceView>& srv,
								winrt::com_ptr<ID3D11UnorderedAccessView>& uav, const char* name) {
			if (FAILED(device->CreateTexture2D(&desc, nullptr, texture.put())))
				return false;
			Util::SetResourceName(texture.get(), "NeuralRendering::%s", name);
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = desc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			if (FAILED(device->CreateShaderResourceView(texture.get(), &srvDesc, srv.put())))
				return false;
			Util::SetResourceName(srv.get(), "NeuralRendering::%s SRV", name);
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = desc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			if (FAILED(device->CreateUnorderedAccessView(texture.get(), &uavDesc, uav.put())))
				return false;
			Util::SetResourceName(uav.get(), "NeuralRendering::%s UAV", name);
			return true;
		};

		if (!create(toneData, toneDataSRV, toneDataUAV, "ToneData") ||
			!create(toneScratch, toneScratchSRV, toneScratchUAV, "ToneScratch")) {
			loggedToneFailure = true;
			logger::warn("[NeuralRendering] Broad/Detail Luminosity disabled: band textures could not be created at {}x{}",
				modelWidth, modelHeight);
			toneData = nullptr;
			toneDataSRV = nullptr;
			toneDataUAV = nullptr;
			toneScratch = nullptr;
			toneScratchSRV = nullptr;
			toneScratchUAV = nullptr;
			return false;
		}

		toneWidth = modelWidth;
		toneHeight = modelHeight;
		return true;
	}

	/** @brief Allocates the debug statistics buffer and its readback ring on first use. */
	bool EnsureDebugStats()
	{
		if (debugStats && debugStatsUAV)
			return true;

		auto* device = globals::d3d::device;
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = kDebugStatCount * sizeof(std::uint32_t);
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = sizeof(std::uint32_t);
		if (FAILED(device->CreateBuffer(&desc, nullptr, debugStats.put()))) {
			debugStats = nullptr;
			return false;
		}
		Util::SetResourceName(debugStats.get(), "NeuralRendering::DebugStats");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.NumElements = kDebugStatCount;
		if (FAILED(device->CreateUnorderedAccessView(debugStats.get(), &uavDesc, debugStatsUAV.put()))) {
			debugStats = nullptr;
			debugStatsUAV = nullptr;
			return false;
		}
		Util::SetResourceName(debugStatsUAV.get(), "NeuralRendering::DebugStats UAV");

		D3D11_BUFFER_DESC stagingDesc{};
		stagingDesc.ByteWidth = desc.ByteWidth;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (auto& staging : debugStaging) {
			if (FAILED(device->CreateBuffer(&stagingDesc, nullptr, staging.put()))) {
				debugStats = nullptr;
				debugStatsUAV = nullptr;
				for (auto& slot : debugStaging)
					slot = nullptr;
				return false;
			}
			Util::SetResourceName(staging.get(), "NeuralRendering::DebugStats Readback");
		}
		debugStagingPending.fill(false);
		debugStagingSlot = 0;
		return true;
	}

	/**
	 * @brief Reads the oldest queued statistics copy and queues this frame's.
	 *
	 * The ring is kDebugReadbackFrames deep, so the slot being mapped was written that many
	 * frames ago and the map never blocks; D3D11_MAP_FLAG_DO_NOT_WAIT covers the case where it
	 * would anyway. The counters are cleared afterwards, before the decode that fills them.
	 */
	void ServiceDebugReadback(ID3D11DeviceContext* context, bool enabled)
	{
		if (!enabled) {
			debugReadback = {};
			debugStagingPending.fill(false);
			return;
		}
		if (!EnsureDebugStats())
			return;

		auto& slot = debugStaging[debugStagingSlot];
		if (debugStagingPending[debugStagingSlot]) {
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(slot.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)) && mapped.pData) {
				const auto* values = static_cast<const std::uint32_t*>(mapped.pData);
				const auto samples = values[kDebugStatSamples];
				debugReadback.guardClampedPercent = samples ?
				                                        100.0f * static_cast<float>(values[kDebugStatClampedSamples]) / static_cast<float>(samples) :
				                                        0.0f;
				debugReadback.modelPeakLuminance = std::bit_cast<float>(values[kDebugStatPeakBits]);
				debugReadback.valid = samples != 0;
				context->Unmap(slot.get(), 0);
				debugStagingPending[debugStagingSlot] = false;
			}
		}

		// Take this frame's counters before clearing them for the decode that follows.
		context->CopyResource(slot.get(), debugStats.get());
		debugStagingPending[debugStagingSlot] = true;
		debugStagingSlot = (debugStagingSlot + 1) % kDebugReadbackFrames;

		const UINT clearValues[4]{ 0, 0, 0, 0 };
		context->ClearUnorderedAccessViewUint(debugStatsUAV.get(), clearValues);
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

	/// Most SRVs any transfer pass binds (DecodeColorCS: t0-t8).
	static constexpr std::size_t kMaxTransferSources = 9;
	/// The colour destination, plus the decode's optional debug-stats buffer.
	static constexpr std::size_t kMaxTransferDestinations = 2;

	/**
	 * Runs one compute pass over the given extent with @p sources bound from t0 upwards, and
	 * unbinds everything afterwards.
	 *
	 * @param destination u0. @param statistics u1, or null when the pass writes no statistics.
	 */
	static void DispatchTransfer(ID3D11DeviceContext* context, ID3D11ComputeShader* shader,
		std::initializer_list<ID3D11ShaderResourceView*> sources,
		ID3D11UnorderedAccessView* destination,
		ID3D11Buffer* constants, ID3D11SamplerState* sampler,
		std::uint32_t width, std::uint32_t height,
		ID3D11UnorderedAccessView* statistics = nullptr)
	{
		ID3D11ShaderResourceView* boundSources[kMaxTransferSources]{};
		std::copy_n(sources.begin(), std::min(sources.size(), kMaxTransferSources), boundSources);
		ID3D11UnorderedAccessView* boundDestinations[kMaxTransferDestinations]{ destination, statistics };
		context->CSSetShader(shader, nullptr, 0);
		context->CSSetShaderResources(0, static_cast<UINT>(kMaxTransferSources), boundSources);
		context->CSSetUnorderedAccessViews(0, static_cast<UINT>(kMaxTransferDestinations), boundDestinations, nullptr);
		context->CSSetConstantBuffers(0, 1, &constants);
		context->CSSetSamplers(0, 1, &sampler);
		context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

		ID3D11ShaderResourceView* nullSRVs[kMaxTransferSources]{};
		ID3D11UnorderedAccessView* nullUAVs[kMaxTransferDestinations]{};
		ID3D11Buffer* nullCB = nullptr;
		ID3D11SamplerState* nullSampler = nullptr;
		context->CSSetShaderResources(0, static_cast<UINT>(std::size(nullSRVs)), nullSRVs);
		context->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::size(nullUAVs)), nullUAVs, nullptr);
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
		                    std::isfinite(inputs.transferStrength) && std::isfinite(inputs.broadLuminosity) &&
		                    std::isfinite(inputs.detailLuminosity) && std::isfinite(inputs.bandRadius) &&
		                    std::isfinite(inputs.maxRatio) && std::isfinite(inputs.highlightWhite) &&
		                    std::isfinite(inputs.wipePosition) &&
		                    std::isfinite(inputs.jitterOffsetX) && std::isfinite(inputs.jitterOffsetY) &&
		                    std::isfinite(inputs.resolutionScaleX) && std::isfinite(inputs.resolutionScaleY) &&
		                    std::isfinite(inputs.localToneStrength) &&
		                    std::isfinite(inputs.localStructureStrength) && std::isfinite(inputs.skinStructureStrength) &&
		                    std::ranges::all_of(inputs.categoryColorStrengths, [](float value) { return std::isfinite(value); }) &&
		                    std::ranges::all_of(inputs.categoryTransferStrengths, [](float value) { return std::isfinite(value); }) &&
		                    std::ranges::all_of(inputs.categoryLuminosityStrengths, [](float value) { return std::isfinite(value); }) &&
		                    std::ranges::all_of(inputs.display.param, [](float value) { return std::isfinite(value); }) &&
		                    std::ranges::all_of(inputs.display.cinematic, [](float value) { return std::isfinite(value); }) &&
		                    std::ranges::all_of(inputs.display.tint, [](float value) { return std::isfinite(value); }) &&
		                    std::isfinite(inputs.display.postProcessExposureScale) &&
		                    std::ranges::all_of(inputs.display.postProcessAdaptationRange, [](float value) { return std::isfinite(value); });
		// Per-category hue guard needs the material category on every pixel, so unlike the
		// old opt-in per-category strengths this guide is now unconditionally required.
		if (inputs.colorIn && inputs.colorOut && inputs.depth && inputs.depthSRV && inputs.motionVectors &&
			inputs.materialCategoriesSRV && distinct && finite && inputs.width && inputs.height)
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
	 *
	 * @param motionFrames Frames elapsed since the model's previous evaluation; the
	 *        one-frame game motion vectors are scaled by it (see Run).
	 */
	bool EvaluateModel(const FrameInputs& inputs, ID3D11DeviceContext* context,
		ID3D11ComputeShader* encodeShader, ID3D11ComputeShader* guideShader, ID3D11ShaderResourceView* colorInView,
		std::uint32_t modelWidth, std::uint32_t modelHeight, std::uint32_t guideWidth, std::uint32_t guideHeight,
		float motionFrames)
	{
		// (b) Colour moves through compute passes rather than CopyResource. The
		// encode resamples the frame onto the unjittered pixel grid at the model
		// raster so the model sees a stable framing; the decode later samples its
		// answer back at each original pixel's jittered position at the active
		// extent (see ColorTransfer.hlsli).
		// The two adaptation inputs drive the proxy's display transform; either may be null.
		DispatchTransfer(context, encodeShader,
			{ colorInView, inputs.display.vanillaAdaptationSRV, inputs.display.postProcessAdaptationSRV },
			color.uav11.Get(), transferParamsCB.get(), linearClampSampler.get(), modelWidth, modelHeight);
		DispatchTransfer(context, guideShader, { inputs.depthSRV }, depth.uav11.Get(),
			nullptr, nullptr, guideWidth, guideHeight);

		// A held frame (Frame Hold) is the same image every evaluation, so it has no motion:
		// hand the model zero vectors rather than the live frame's, which describe a scene
		// that has moved on.
		if (inputs.staticMotion) {
			const float zeroMotion[4]{};
			context->ClearUnorderedAccessViewFloat(motionVectors.uav11.Get(), zeroMotion);
		} else {
			const D3D11_BOX motionBox{ 0, 0, 0, guideWidth, guideHeight, 1 };
			context->CopySubresourceRegion(motionVectors.resource11.Get(), 0, 0, 0, 0, inputs.motionVectors, 0, &motionBox);
		}

		// Run() has already settled and (if it changed) recreated the feature for this
		// frame's tuning via SettleTuning; appliedTuning is exactly what should be
		// latched into a create and restated into an evaluate, per Runtime::Execute.
		const NeuralRenderingNGX::Tuning& tuning = appliedTuning;

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
		// model scale is deliberately not folded in (see neural-rendering.md). The
		// game's vectors describe one frame of motion, but the model's history is
		// from its previous evaluation, which alternating-frame mode leaves two frames
		// back, so the scale also carries the frames elapsed (constant-velocity
		// extrapolation of this frame's motion).
		const bool executed = NeuralRenderingNGX::Runtime::Instance().Execute(commandList,
			color.resource12.Get(), depth.resource12.Get(), motionVectors.resource12.Get(), output.resource12.Get(),
			modelWidth, modelHeight, guideWidth, guideHeight, output.desc.Width, output.desc.Height,
			static_cast<float>(guideWidth) * motionFrames, static_cast<float>(guideHeight) * motionFrames,
			tuning, inputs.reset || resetPending, inputs.depthInverted);

		for (auto& barrier : barriers)
			std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);

		if (!interop.EndD3D12())
			return LatchFailure("EndD3D12", interop.LastError());
		if (!executed)
			return LatchFailure("Feature 18 execution", static_cast<HRESULT>(NeuralRenderingNGX::Runtime::Instance().NgxResult()));
		return true;
	}

	bool Run(const FrameInputs& inputs, ID3D11Device* device, ID3D11DeviceContext* context)
	{
		++evaluateFrameIndex;
		if (!interop.IsInitialized() && !InitializeInterop(device, context))
			return false;
		if (NeuralRenderingNGX::Runtime::Instance().Status() != NeuralRenderingNGX::RuntimeStatus::Initialized &&
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

		// DLSSNR.Intensity/Style/LocalToneStrength/LocalStructureStrength/
		// SkinStructureStrength/UseAutoMask are latched at Feature 18 creation and
		// do nothing written at evaluate (see Runtime::Execute). SettleTuning
		// debounces a changed value the same way SettleModelRaster debounces a
		// resolution-scale drag, then this forces a recreate through the same
		// GPU-idle path EnsureResources uses for a raster change - never a bare
		// release() while the interop queue might still reference the handle.
		NeuralRenderingNGX::Tuning desiredTuning;
		desiredTuning.intensity = inputs.intensity;
		desiredTuning.localToneStrength = inputs.localToneStrength;
		desiredTuning.localStructureStrength = inputs.localStructureStrength;
		desiredTuning.skinStructureStrength = inputs.skinStructureStrength;
		desiredTuning.style = inputs.style;
		desiredTuning.useAutoMask = inputs.automaticMask;
		desiredTuning.uiCorrection = false;  // Cav's Unity Shaders never runs Neural Rendering after the UI composite.
		desiredTuning.modelContract = inputs.modelContract <= 2u ? inputs.modelContract : 0u;
		if (SettleTuning(desiredTuning)) {
			if (!interop.WaitForIdle())
				return LatchFailure("tuning change", interop.LastError());
			NeuralRenderingNGX::Runtime::Instance().ResetFeature();
			separateResetPending = true;
			separateResidualReady = false;
			resetPending = true;
		}

		// Alternating frames (the proxy's experimental "VRNR"): run the model every
		// other frame and, in between, re-apply its previous answer to the fresh
		// frame through the decode alone, reprojected through the game's motion
		// vectors. The shared colour/output textures keep the previous proxy/answer
		// pair, which D3D11 already waited on when that frame's D3D12 work was
		// submitted. The first frame after a history reset, a raster change or a
		// failure always evaluates, and so does every frame when there is no
		// motion-vector view to reproject through.
		const bool skipFrame = inputs.alternateFrames && inputs.motionVectorsSRV && !inputs.staticMotion &&
		                       featureAvailable && !resetPending && !inputs.reset && (evaluateFrameIndex % 2) == 1;

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
		// The guides are the game's jittered render targets whatever the colour
		// raster is, so the decode offsets its guide lookups by this before
		// reading them. Zero (the colour is jittered alike) everywhere but after
		// the upscaler. The same one-pixel sanity bound applies; it also rejects
		// a NaN, which fails the comparison.
		transferParams.guideJitterOffset[0] = std::abs(inputs.guideJitterOffsetX) <= 1.0f ? inputs.guideJitterOffsetX : 0.0f;
		transferParams.guideJitterOffset[1] = std::abs(inputs.guideJitterOffsetY) <= 1.0f ? inputs.guideJitterOffsetY : 0.0f;
		transferParams.colorStrength = std::clamp(inputs.colorStrength, 0.0f, 2.0f);
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
		transferParams.staleAnswer = skipFrame ? 1u : 0u;
		// Only 0 (scene linear) and 1 (display gamma) exist; anything else falls back to the
		// original scene-linear behaviour rather than an undefined shader branch.
		transferParams.colorDomain = inputs.colorDomain <= 2u ? inputs.colorDomain : 0u;
		transferParams.proxyCurve = inputs.proxyCurve <= 3u ? inputs.proxyCurve : 0u;
		// Display transform of the scene-linear proxy. A stage whose GPU input is missing is
		// switched off here rather than left to read an unbound slot.
		const auto& display = inputs.display;
		transferParams.displayParam[0] = display.vanillaGrading && display.vanillaAdaptationSRV ? 1.0f : 0.0f;
		transferParams.displayParam[1] = display.param[1];
		transferParams.displayParam[2] = display.param[2];
		transferParams.displayParam[3] = 0.0f;
		std::copy_n(display.cinematic, 4, transferParams.displayCinematic);
		std::copy_n(display.tint, 4, transferParams.displayTint);
		transferParams.displayExposure[0] = display.postProcessExposure && display.postProcessAdaptationSRV ? 1.0f : 0.0f;
		transferParams.displayExposure[1] = display.postProcessExposureScale;
		transferParams.displayExposure[2] = display.postProcessAdaptationRange[0];
		transferParams.displayExposure[3] = display.postProcessAdaptationRange[1];
		// The band split only exists while the two strengths differ; equal values take the
		// single-exponent path in the resolve, so the extra passes and textures are skipped.
		transferParams.broadLuminosity = std::clamp(inputs.broadLuminosity, 0.0f, 2.0f);
		transferParams.bandParams[0] = std::clamp(inputs.detailLuminosity, 0.0f, 2.0f);
		transferParams.bandParams[1] = std::clamp(inputs.bandRadius, 2.0f, 32.0f);
		const bool bandsSeparated = std::abs(transferParams.broadLuminosity - transferParams.bandParams[0]) > 1e-4f;
		const bool toneReady = bandsSeparated && EnsureToneResources(modelWidth, modelHeight) &&
		                       GetShader(prepareToneDataCS, prepareToneDataAttempted, kPrepareToneDataPath, "PrepareToneDataCS") &&
		                       GetShader(filterToneDataHorizontalCS, filterToneDataHorizontalAttempted,
								   kFilterToneDataPath, "FilterToneDataCS") &&
		                       GetShader(filterToneDataVerticalCS, filterToneDataVerticalAttempted,
								   kFilterToneDataPath, "FilterToneDataCS Vertical", { { "VERTICAL", "1" } });
		if (!bandsSeparated)
			toneDataValid = false;
		// The decode may only read the band textures once a pass has actually filled them for
		// this raster; a skipped (alternating) frame keeps the previous evaluation's data.
		transferParams.bandParams[2] = toneReady && (toneDataValid || !skipFrame) ? 1.0f : 0.0f;

		std::uint32_t debugFlags = 0;
		if (inputs.debugGuardClamp)
			debugFlags |= kDebugFlagGuardClamp;
		// Only meaningful while the bands are genuinely separated, and mutually exclusive.
		if (transferParams.bandParams[2] > 0.5f && inputs.debugBroadBand)
			debugFlags |= kDebugFlagBroadBand;
		else if (transferParams.bandParams[2] > 0.5f && inputs.debugDetailBand)
			debugFlags |= kDebugFlagDetailBand;
		const bool collectStats = inputs.measureModelPeak || inputs.debugGuardClamp;
		ServiceDebugReadback(context, collectStats);
		if (collectStats && debugStatsUAV)
			debugFlags |= kDebugFlagStats;
		transferParams.debugFlags = debugFlags;
		// The guard is two-sided (1/maxRatio..maxRatio) and only meaningful at or
		// above one; a stale or misconfigured value below that would otherwise
		// invert into a guard tighter than the floor it is supposed to raise.
		// Disabled (the default - see NeuralRendering::Options::ratioGuardEnabled):
		// send a value large enough that the shader's clamp never actually binds,
		// so a correct large light/dark swing (e.g. a shadow edit) is never capped.
		transferParams.maxRatio = inputs.ratioGuardEnabled ? std::clamp(inputs.maxRatio, 1.0f, 8.0f) : kNeuralRatioGuardDisabledValue;
		std::uint32_t hueGuardMask = 0;
		for (std::size_t index = 0; index < inputs.categoryColorStrengths.size(); ++index) {
			transferParams.categoryColorStrengths[index] = std::clamp(inputs.categoryColorStrengths[index], 0.0f, 2.0f);
			transferParams.categoryTransferStrengths[index] = std::clamp(inputs.categoryTransferStrengths[index], 0.0f, 2.0f);
			transferParams.categoryLuminosityStrengths[index] = std::clamp(inputs.categoryLuminosityStrengths[index], 0.0f, 2.0f);
			if (inputs.categoryHueGuard[index])
				hueGuardMask |= (1u << index);
		}
		transferParams.hueGuardMask = hueGuardMask;
		transferParams.debugCategoryView = inputs.debugCategoryView ? 1u : 0u;
		transferParams.rawModelOutput = inputs.rawModelOutput ? 1u : 0u;
		// Only meaningful for a finished frame on an HDR target; the shader ignores
		// anything at or below one (no headroom) and the scene-linear domain.
		transferParams.highlightWhite = transferParams.colorDomain == 1u ? std::clamp(inputs.highlightWhite, 0.0f, 100.0f) : 0.0f;
		transferParams.wipePosition = inputs.wipePosition >= 0.0f ? std::min(inputs.wipePosition, 1.0f) : -1.0f;
		context->UpdateSubresource(transferParamsCB.get(), 0, nullptr, &transferParams, 0, 0);

		if (!skipFrame) {
			// Only alternating-frame mode leaves a gap between evaluations; a reset
			// discards the history the scale would describe, so it keeps one frame.
			// Anything past two frames (Run not called while a menu paused the
			// game) is not a skip and is not extrapolated.
			const bool historyValid = !resetPending && !inputs.reset && lastEvaluatedFrameIndex != 0;
			const bool bridgedSkip = inputs.alternateFrames && historyValid &&
			                         evaluateFrameIndex - lastEvaluatedFrameIndex == 2;
			if (!EvaluateModel(inputs, context, encodeShader, guideShader, colorInView,
					modelWidth, modelHeight, guideWidth, guideHeight, bridgedSkip ? 2.0f : 1.0f))
				return false;
			lastEvaluatedFrameIndex = evaluateFrameIndex;

			// Split the answer's luminance edit into its smooth and detail halves, once per
			// evaluation and at the model raster, while the proxy and the answer are the pair
			// the resolve is about to use. PrepareToneDataCS writes (log proxy luminance,
			// edit); the two filter passes blur only the edit, along one axis each, and the
			// vertical one lands back in toneData for the decode to sample.
			if (transferParams.bandParams[2] > 0.5f) {
				globals::state->BeginPerfEvent("NeuralRendering::ToneBands");
				DispatchTransfer(context, prepareToneDataCS.get(), { colorSRV.get(), outputSRV.get() },
					toneDataUAV.get(), transferParamsCB.get(), linearClampSampler.get(), modelWidth, modelHeight);
				DispatchTransfer(context, filterToneDataHorizontalCS.get(), { toneDataSRV.get() },
					toneScratchUAV.get(), transferParamsCB.get(), linearClampSampler.get(), modelWidth, modelHeight);
				DispatchTransfer(context, filterToneDataVerticalCS.get(), { toneScratchSRV.get() },
					toneDataUAV.get(), transferParamsCB.get(), linearClampSampler.get(), modelWidth, modelHeight);
				globals::state->EndPerfEvent();
				toneDataValid = true;
			}
		}

		// Re-anchor the model's bounded luminance to the untouched source, then
		// restore its chromaticity through the independently controlled colour pass.
		// The edit is measured against the exact proxy the model received, sampled
		// at the same (jitter-compensated) position. No inverse tonemap or temporal
		// colour accumulator is involved. The game depth rides along as the
		// silhouette guide for the depth-aware resolve, and the motion vectors
		// reproject a stale answer.
		DispatchTransfer(context, decodeShader,
			{ outputSRV.get(), colorInView, colorSRV.get(), inputs.depthSRV, inputs.materialCategoriesSRV,
				inputs.display.vanillaAdaptationSRV, inputs.display.postProcessAdaptationSRV, inputs.motionVectorsSRV,
				transferParams.bandParams[2] > 0.5f ? toneDataSRV.get() : nullptr },
			colorOutView, transferParamsCB.get(), linearClampSampler.get(), colorWidth, colorHeight,
			(transferParams.debugFlags & kDebugFlagStats) != 0 ? debugStatsUAV.get() : nullptr);

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
			NeuralRenderingNGX::Runtime::Instance().ResetSuperResolutionFeature();
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
		DispatchTransfer(context, encodeShader, { originalView, editedView },
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

		const auto result = NeuralRenderingNGX::Runtime::Instance().ExecuteSuperResolution(commandList,
			residualInput.resource12.Get(), depth.resource12.Get(), motionVectors.resource12.Get(),
			residualExposure.resource12.Get(), residualOutput.resource12.Get(),
			inputs.width, inputs.height, inputs.outputWidth, inputs.outputHeight,
			inputs.jitterOffsetX, inputs.jitterOffsetY,
			1.0f, 1.0f,
			globals::game::deltaTime ? *globals::game::deltaTime * 1000.0f : 16.6667f,
			inputs.superResolutionQualityMode, inputs.superResolutionPreset,
			inputs.reset || separateResetPending, inputs.depthInverted);

		for (auto& barrier : barriers)
			std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
		const bool submitted = interop.EndD3D12();
		restoreTargets();
		if (!submitted)
			return LatchFailure("separate residual EndD3D12", interop.LastError());
		if (result == NeuralRenderingNGX::SuperResolutionResult::Failed)
			return LatchFailure("private DLSS SR execution",
				static_cast<HRESULT>(NeuralRenderingNGX::Runtime::Instance().NgxResult()));
		if (result == NeuralRenderingNGX::SuperResolutionResult::Created) {
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
		DispatchTransfer(context, applyShader, { cleanView, residualOutputSRV.get() },
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
		prepareToneDataCS = nullptr;
		filterToneDataHorizontalCS = nullptr;
		filterToneDataVerticalCS = nullptr;
		transferParamsCB = nullptr;
		linearClampSampler = nullptr;
		encodeColorAttempted = false;
		decodeColorAttempted = false;
		copyDepthGuideAttempted = false;
		encodeResidualAttempted = false;
		applyResidualAttempted = false;
		prepareToneDataAttempted = false;
		filterToneDataHorizontalAttempted = false;
		filterToneDataVerticalAttempted = false;

		toneData = nullptr;
		toneDataSRV = nullptr;
		toneDataUAV = nullptr;
		toneScratch = nullptr;
		toneScratchSRV = nullptr;
		toneScratchUAV = nullptr;
		toneWidth = 0;
		toneHeight = 0;
		toneDataValid = false;
		loggedToneFailure = false;

		debugStats = nullptr;
		debugStatsUAV = nullptr;
		for (auto& staging : debugStaging)
			staging = nullptr;
		debugStagingPending.fill(false);
		debugStagingSlot = 0;
		debugReadback = {};

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
		lastEvaluatedFrameIndex = 0;

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
		NeuralRenderingNGX::Runtime::Instance().ResetFeature();
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

NeuralRenderingBackend::DebugReadback NeuralRenderingBackend::GetDebugReadback() const
{
	return state->debugReadback;
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
