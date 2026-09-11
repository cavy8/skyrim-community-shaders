#include "Runtime.h"

#include "../../../Utils/FileSystem.h"
#include "../../../Utils/Format.h"
#include "../../../Utils/WinApi.h"
#include "../Streamline.h"

#include <Windows.h>
#include <Psapi.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <vector>

#include <nvsdk_ngx_helpers.h>

namespace NeuralRendering
{
	namespace
	{
		constexpr wchar_t kRuntimeName[] = L"nvngx_dlssnr.dll";
		constexpr auto kFeatureDlssNr = static_cast<NVSDK_NGX_Feature>(18);
		constexpr std::array<const char*, 5> kRequiredExports{
			"NVSDK_NGX_D3D12_Init_Ext",
			"NVSDK_NGX_D3D12_CreateFeature",
			"NVSDK_NGX_D3D12_EvaluateFeature",
			"NVSDK_NGX_D3D12_ReleaseFeature",
			"NVSDK_NGX_D3D12_Shutdown1",
		};

		using GetUnsignedValue = unsigned int(NVSDK_CONV*)();
		using InitD3D12WithProjectId = NVSDK_NGX_Result(NVSDK_CONV*)(const char*, NVSDK_NGX_EngineType,
			const char*, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
		using InitD3D12WithApplicationId = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*,
			ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
		using ShutdownD3D12 = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
		using AllocateParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
		using DestroyParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
		using CreateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
		using EvaluateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
		using ReleaseFeature = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
		using GetModuleFileNameWFunction = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);

		GetModuleFileNameWFunction g_originalGetModuleFileNameW = nullptr;
		HMODULE g_callerModule = nullptr;
		std::wstring g_spoofedRuntimePath;
		std::uint32_t g_proxyHits = 0;

		// The DLSSNR snippet resolves its parameter allocator from whichever NGX core
		// module is already resident in the process (loaded by Streamline), never from
		// the snippet DLL itself.
		HMODULE FindNgxCoreModule()
		{
			std::array<HMODULE, 1024> modules{};
			DWORD bytesNeeded = 0;
			if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(sizeof(modules)), &bytesNeeded))
				return nullptr;
			const std::size_t count = std::min<std::size_t>(modules.size(), bytesNeeded / sizeof(HMODULE));
			for (std::size_t index = 0; index < count; ++index) {
				if (GetProcAddress(modules[index], "NVSDK_NGX_D3D12_AllocateParameters") &&
					GetProcAddress(modules[index], "NVSDK_NGX_D3D12_DestroyParameters"))
					return modules[index];
			}
			return nullptr;
		}

		DWORD WINAPI SignedRuntimeGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size)
		{
			if (module == g_callerModule && filename && size && !g_spoofedRuntimePath.empty()) {
				++g_proxyHits;
				const DWORD length = static_cast<DWORD>(g_spoofedRuntimePath.size());
				const DWORD copyLength = std::min(length, size - 1);
				std::memcpy(filename, g_spoofedRuntimePath.data(), copyLength * sizeof(wchar_t));
				filename[copyLength] = L'\0';
				return copyLength < length ? size : length;
			}
			return g_originalGetModuleFileNameW ? g_originalGetModuleFileNameW(module, filename, size) : 0;
		}

		/**
		 * @brief Patches nvngx_dlssnr.dll's import of GetModuleFileNameW for the scope's lifetime.
		 *
		 * Feature 18 gates itself on its caller's module path and refuses to run unless that
		 * path is NVIDIA's signed nvngx.dll. Every NGX entry point must therefore be invoked
		 * with this scope alive; the original import is restored on destruction.
		 */
		class SignedRuntimePathScope
		{
		public:
			SignedRuntimePathScope(HMODULE runtime, const std::filesystem::path& ngxPath)
			{
				if (!runtime)
					return;
				GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(&SignedRuntimeGetModuleFileNameW), &g_callerModule);
				g_spoofedRuntimePath = ngxPath.wstring();
				g_proxyHits = 0;

				auto* base = reinterpret_cast<std::byte*>(runtime);
				auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
				if (dos->e_magic != IMAGE_DOS_SIGNATURE)
					return;
				auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
				if (nt->Signature != IMAGE_NT_SIGNATURE)
					return;
				const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
				if (!imports.VirtualAddress)
					return;

				auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
				for (; descriptor->Name; ++descriptor) {
					if (!descriptor->OriginalFirstThunk)
						continue;
					auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
					auto* functions = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
					for (; names->u1.AddressOfData; ++names, ++functions) {
						if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
							continue;
						auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
						if (std::strcmp(reinterpret_cast<const char*>(import->Name), "GetModuleFileNameW"))
							continue;
						slot_ = reinterpret_cast<void**>(&functions->u1.Function);
						g_originalGetModuleFileNameW = reinterpret_cast<GetModuleFileNameWFunction>(*slot_);
						DWORD oldProtection = 0;
						if (VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &oldProtection)) {
							*slot_ = reinterpret_cast<void*>(&SignedRuntimeGetModuleFileNameW);
							DWORD ignored = 0;
							VirtualProtect(slot_, sizeof(*slot_), oldProtection, &ignored);
							FlushInstructionCache(GetCurrentProcess(), slot_, sizeof(*slot_));
							installed_ = true;
						}
						return;
					}
				}
			}

			~SignedRuntimePathScope()
			{
				if (installed_ && slot_) {
					DWORD oldProtection = 0;
					if (VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &oldProtection)) {
						*slot_ = reinterpret_cast<void*>(g_originalGetModuleFileNameW);
						DWORD ignored = 0;
						VirtualProtect(slot_, sizeof(*slot_), oldProtection, &ignored);
						FlushInstructionCache(GetCurrentProcess(), slot_, sizeof(*slot_));
					}
				}
				g_spoofedRuntimePath.clear();
				g_callerModule = nullptr;
				g_originalGetModuleFileNameW = nullptr;
			}

			SignedRuntimePathScope(const SignedRuntimePathScope&) = delete;
			SignedRuntimePathScope& operator=(const SignedRuntimePathScope&) = delete;

			[[nodiscard]] bool IsInstalled() const { return installed_; }
			[[nodiscard]] std::uint32_t Hits() const { return g_proxyHits; }

		private:
			void** slot_ = nullptr;
			bool installed_ = false;
		};

		std::filesystem::path ResolveRuntimePath(const std::filesystem::path& explicitPath)
		{
			if (!explicitPath.empty())
				return std::filesystem::is_directory(explicitPath) ? explicitPath / kRuntimeName : explicitPath;
			const auto dataPath = Util::PathHelpers::GetDataPath();
			const std::array candidates{
				dataPath / L"Shaders/Upscaling/Streamline" / kRuntimeName,
				dataPath / L"Shaders/Upscaling/StreamlineDX12" / kRuntimeName,
			};
			for (const auto& candidate : candidates) {
				std::error_code error;
				if (std::filesystem::is_regular_file(candidate, error))
					return candidate;
			}
			return {};
		}

		NVSDK_NGX_PerfQuality_Value ToPerfQuality(std::uint32_t qualityMode)
		{
			switch (qualityMode) {
			case 0: return NVSDK_NGX_PerfQuality_Value_DLAA;
			case 2: return NVSDK_NGX_PerfQuality_Value_Balanced;
			case 3: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
			case 4: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
			default: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
			}
		}

		NVSDK_NGX_DLSS_Hint_Render_Preset ToRenderPreset(std::uint32_t preset)
		{
			switch (preset) {
			case 1: return NVSDK_NGX_DLSS_Hint_Render_Preset_J;
			case 2: return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
			case 3: return NVSDK_NGX_DLSS_Hint_Render_Preset_L;
			case 4: return NVSDK_NGX_DLSS_Hint_Render_Preset_M;
			default: return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
			}
		}

		void SetRenderPreset(NVSDK_NGX_Parameter* parameters, std::uint32_t preset)
		{
			if (!parameters || preset == 0)
				return;
			const auto value = static_cast<unsigned int>(ToRenderPreset(preset));
			parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, value);
			parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, value);
			parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, value);
			parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, value);
			parameters->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, value);
		}
	}

	Runtime& Runtime::Instance()
	{
		static Runtime instance;
		return instance;
	}

	Runtime::~Runtime() { Shutdown(); }

	bool Runtime::Probe(const std::filesystem::path& explicitPath)
	{
		Shutdown();
		path_ = ResolveRuntimePath(explicitPath);
		if (path_.empty()) {
			status_ = RuntimeStatus::NotFound;
			detail_ = "nvngx_dlssnr.dll was not found";
			return false;
		}
		const auto version = Util::GetDllVersion(path_.wstring());
		if (!version) {
			status_ = RuntimeStatus::VersionUnavailable;
			detail_ = "DLL version resource is unavailable";
			return false;
		}
		version_ = Util::GetFormattedVersion(*version);
		if (version->major() != 310 || version->minor() != 8) {
			status_ = RuntimeStatus::UnsupportedVersion;
			detail_ = std::format("expected DLSSNR 310.8.x, found {}", version_);
			return false;
		}
		module_ = LoadLibraryExW(path_.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
		if (!module_) {
			status_ = RuntimeStatus::LoadFailed;
			detail_ = std::format("LoadLibraryExW failed with {}", GetLastError());
			return false;
		}
		for (const char* exportName : kRequiredExports) {
			if (!GetProcAddress(static_cast<HMODULE>(module_), exportName)) {
				status_ = RuntimeStatus::MissingExport;
				detail_ = std::format("missing export {}", exportName);
				FreeLibrary(static_cast<HMODULE>(module_));
				module_ = nullptr;
				return false;
			}
		}
		auto getAppId = reinterpret_cast<GetUnsignedValue>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_GetApplicationId"));
		auto getApi = reinterpret_cast<GetUnsignedValue>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_GetAPIVersion"));
		if (!getAppId || !getApi) {
			status_ = RuntimeStatus::MissingExport;
			detail_ = "signed runtime identity exports are missing";
			FreeLibrary(static_cast<HMODULE>(module_));
			module_ = nullptr;
			return false;
		}
		applicationId_ = getAppId();
		apiVersion_ = getApi();
		status_ = RuntimeStatus::Ready;
		return true;
	}

	bool Runtime::Initialize(ID3D12Device* device, const std::filesystem::path& dataPath)
	{
		if (!device)
			return false;
		if (status_ == RuntimeStatus::Initialized && device_ != device)
			Shutdown();
		if (!module_ && !Probe())
			return false;
		if (status_ == RuntimeStatus::Initialized && device_ == device)
			return true;

		std::filesystem::path writablePath = dataPath;
		if (writablePath.empty()) {
			wchar_t tempPath[MAX_PATH]{};
			GetTempPathW(MAX_PATH, tempPath);
			writablePath = std::filesystem::path(tempPath) / L"OpenShaders-NGX";
		}
		std::error_code error;
		std::filesystem::create_directories(writablePath, error);
		HMODULE core = FindNgxCoreModule();
		if (!core) {
			status_ = RuntimeStatus::CoreUnavailable;
			detail_ = "NGX core module was not found before D3D12 initialization";
			return false;
		}

		// The NR snippet's Init_Ext accepts a parameter block, not the feature-search
		// metadata used by the driver core. Initializing only through that entry point
		// lets Feature 18 run but leaves this private D3D12 device unable to locate the
		// DLSS-SR snippet. Mirror Streamline/the OptiScaler experiment: initialize the
		// core directly with the same project identity and the directory containing
		// nvngx_dlss.dll.
		std::error_code absoluteError;
		auto featureDirectory = std::filesystem::absolute(path_.parent_path(), absoluteError);
		if (absoluteError)
			featureDirectory = path_.parent_path();
		const std::wstring featureDirectoryString = featureDirectory.wstring();
		const wchar_t* featurePaths[]{ featureDirectoryString.c_str() };
		NVSDK_NGX_FeatureCommonInfo featureInfo{};
		featureInfo.PathListInfo.Path = featurePaths;
		featureInfo.PathListInfo.Length = static_cast<unsigned int>(std::size(featurePaths));

		auto initializeWithProjectId = reinterpret_cast<InitD3D12WithProjectId>(
			GetProcAddress(core, "NVSDK_NGX_D3D12_Init_with_ProjectID"));
		auto initializeWithApplicationId = reinterpret_cast<InitD3D12WithApplicationId>(
			GetProcAddress(core, "NVSDK_NGX_D3D12_Init_Ext"));
		if (initializeWithProjectId) {
			ngxResult_ = static_cast<std::uint32_t>(initializeWithProjectId(Streamline::ProjectId,
				NVSDK_NGX_ENGINE_TYPE_CUSTOM, Streamline::EngineVersion, writablePath.c_str(), device,
				NVSDK_NGX_Version_API, &featureInfo));
		} else if (initializeWithApplicationId) {
			ngxResult_ = static_cast<std::uint32_t>(initializeWithApplicationId(applicationId_,
				writablePath.c_str(), device, NVSDK_NGX_Version_API, &featureInfo));
		} else {
			status_ = RuntimeStatus::InitializationFailed;
			detail_ = "NGX core is missing its D3D12 initialization entry points";
			return false;
		}
		if (ngxResult_ != NVSDK_NGX_Result_Success) {
			status_ = RuntimeStatus::InitializationFailed;
			detail_ = std::format("NGX core D3D12 init failed 0x{:08X} featurePath={}",
				ngxResult_, featureDirectory.string());
			return false;
		}
		logger::info("[DLSSNR] NGX core D3D12 initialized featurePath={}", featureDirectory.string());
		device_ = device;
		device_->AddRef();

		auto allocate = reinterpret_cast<AllocateParameters>(GetProcAddress(core, "NVSDK_NGX_D3D12_AllocateParameters"));
		NVSDK_NGX_Parameter* parameters = nullptr;
		ngxResult_ = static_cast<std::uint32_t>(allocate(&parameters));
		if (ngxResult_ != NVSDK_NGX_Result_Success || !parameters) {
			const auto allocationResult = ngxResult_;
			Shutdown();
			status_ = RuntimeStatus::ParameterAllocationFailed;
			ngxResult_ = allocationResult;
			detail_ = std::format("NGX parameter allocation failed 0x{:08X}", allocationResult);
			return false;
		}
		parameters_ = parameters;
		status_ = RuntimeStatus::Initialized;
		return true;
	}

	bool Runtime::Execute(ID3D12GraphicsCommandList* commandList,
		ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motionVectors, ID3D12Resource* output,
		std::uint32_t colorWidth, std::uint32_t colorHeight,
		std::uint32_t guideWidth, std::uint32_t guideHeight,
		std::uint32_t outputWidth, std::uint32_t outputHeight,
		float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning, bool reset)
	{
		if (status_ != RuntimeStatus::Initialized || !commandList || !color || !depth || !motionVectors || !output)
			return false;
		auto* parameters = static_cast<NVSDK_NGX_Parameter*>(parameters_);
		auto create = reinterpret_cast<CreateFeature>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_D3D12_CreateFeature"));
		auto evaluate = reinterpret_cast<EvaluateFeature>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_D3D12_EvaluateFeature"));
		auto release = reinterpret_cast<ReleaseFeature>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_D3D12_ReleaseFeature"));
		SignedRuntimePathScope scope(static_cast<HMODULE>(module_), path_.parent_path() / L"nvngx.dll");
		if (!scope.IsInstalled())
			return false;

		// The feature is built for the active colour raster. Before-upscale operation
		// therefore creates it at render resolution and after-upscale operation at
		// display resolution. When that raster changes the backend has already
		// drained the interop queue via EnsureResources(), so no in-flight command
		// list still references the handle being released.
		const bool dimensionsChanged = featureOutputWidth_ != outputWidth || featureOutputHeight_ != outputHeight;
		if (featureHandle_ && dimensionsChanged) {
			release(static_cast<NVSDK_NGX_Handle*>(featureHandle_));
			featureHandle_ = nullptr;
		}

		if (!featureHandle_) {
			parameters->Reset();
			parameters->Set("DLSSNR.Enabled", 1u);
			parameters->Set("DLSSNR.Width", outputWidth);
			parameters->Set("DLSSNR.Height", outputHeight);
			parameters->Set("CreationNodeMask", 1u);
			parameters->Set("VisibilityNodeMask", 1u);

			// The model latches its tuning at feature-create time; the same names
			// written only at evaluate are read by nothing. They are set again in
			// the evaluate block below purely so a future shared-parameter-block
			// path stays correct, but this is the write that takes effect. A tuning
			// change therefore needs the feature rebuilt - today that means toggling
			// Neural Rendering off and on; a debounced in-place rebuild is a follow-up.
			parameters->Set("DLSSNR.Hint.Render.Preset", 0u);
			parameters->Set("DLSSNR.Intensity", tuning.intensity);
			parameters->Set("DLSSNR.Style", tuning.style);
			parameters->Set("DLSSNR.LocalToneStrength", tuning.localToneStrength);
			parameters->Set("DLSSNR.LocalStructureStrength", tuning.localStructureStrength);
			parameters->Set("DLSSNR.SkinStructureStrength", tuning.skinStructureStrength);
			parameters->Set("DLSSNR.UseAutoMask", tuning.useAutoMask ? 1u : 0u);
			parameters->Set("DLSSNR.UICorrection", tuning.uiCorrection ? 1u : 0u);
			NVSDK_NGX_Handle* handle = nullptr;
			ngxResult_ = static_cast<std::uint32_t>(create(commandList, kFeatureDlssNr, parameters, &handle));
			if (ngxResult_ != NVSDK_NGX_Result_Success || !handle) {
				detail_ = std::format("Feature 18 create failed result=0x{:08X} proxyHits={}", ngxResult_, scope.Hits());
				return false;
			}
			featureHandle_ = handle;
			featureOutputWidth_ = outputWidth;
			featureOutputHeight_ = outputHeight;
			reset = true;
		}

		parameters->Reset();
		parameters->Set("DLSSNR.Color", color);
		parameters->Set("DLSSNR.Depth", depth);
		parameters->Set("DLSSNR.MVec", motionVectors);
		parameters->Set("DLSSNR.Output", output);
		// Reset() clears the parameter block. Feature 18 reads these dimensions
		// during evaluation too, so restate the complete per-frame contract rather
		// than relying on values written when the handle was created.
		parameters->Set("DLSSNR.Width", colorWidth);
		parameters->Set("DLSSNR.Height", colorHeight);
		// Colour and output share the display-referred region; depth and motion vectors
		// carry the game's render-resolution region. Each resource states its own valid
		// extent so the model can bridge the two - this is also why the motion-vector
		// scale below must not fold in the resolution ratio a second time.
		parameters->Set("DLSSNR.ColorSubrectBaseX", 0u);
		parameters->Set("DLSSNR.ColorSubrectBaseY", 0u);
		parameters->Set("DLSSNR.ColorSubrectWidth", colorWidth);
		parameters->Set("DLSSNR.ColorSubrectHeight", colorHeight);
		parameters->Set("DLSSNR.DepthSubrectBaseX", 0u);
		parameters->Set("DLSSNR.DepthSubrectBaseY", 0u);
		parameters->Set("DLSSNR.DepthSubrectWidth", guideWidth);
		parameters->Set("DLSSNR.DepthSubrectHeight", guideHeight);
		parameters->Set("DLSSNR.MVecSubrectBaseX", 0u);
		parameters->Set("DLSSNR.MVecSubrectBaseY", 0u);
		parameters->Set("DLSSNR.MVecSubrectWidth", guideWidth);
		parameters->Set("DLSSNR.MVecSubrectHeight", guideHeight);
		parameters->Set("DLSSNR.OutputSubrectBaseX", 0u);
		parameters->Set("DLSSNR.OutputSubrectBaseY", 0u);
		parameters->Set("DLSSNR.OutputSubrectWidth", colorWidth);
		parameters->Set("DLSSNR.OutputSubrectHeight", colorHeight);
		parameters->Set("DLSSNR.MVecScaleX", motionVectorScaleX);
		parameters->Set("DLSSNR.MVecScaleY", motionVectorScaleY);
		parameters->Set("DLSSNR.DepthInverted", 0u);
		parameters->Set("DLSSNR.Enabled", 1u);
		parameters->Set("DLSSNR.Reset", reset ? 1u : 0u);
		parameters->Set("DLSSNR.Intensity", tuning.intensity);
		parameters->Set("DLSSNR.LocalToneStrength", tuning.localToneStrength);
		parameters->Set("DLSSNR.LocalStructureStrength", tuning.localStructureStrength);
		parameters->Set("DLSSNR.SkinStructureStrength", tuning.skinStructureStrength);
		parameters->Set("DLSSNR.UseAutoMask", tuning.useAutoMask ? 1u : 0u);
		parameters->Set("DLSSNR.Style", tuning.style);
		parameters->Set("DLSSNR.UICorrection", tuning.uiCorrection ? 1u : 0u);
		ngxResult_ = static_cast<std::uint32_t>(evaluate(commandList,
			static_cast<NVSDK_NGX_Handle*>(featureHandle_), parameters, nullptr));
		if (ngxResult_ != NVSDK_NGX_Result_Success) {
			detail_ = std::format("Feature 18 evaluate failed result=0x{:08X}", ngxResult_);
			return false;
		}
		++successfulFrames_;
		return true;
	}

	SuperResolutionResult Runtime::ExecuteSuperResolution(ID3D12GraphicsCommandList* commandList,
		ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motionVectors,
		ID3D12Resource* exposure, ID3D12Resource* output,
		std::uint32_t inputWidth, std::uint32_t inputHeight,
		std::uint32_t outputWidth, std::uint32_t outputHeight,
		float jitterOffsetX, float jitterOffsetY,
		float motionVectorScaleX, float motionVectorScaleY,
		float frameTimeDeltaMilliseconds,
		std::uint32_t qualityMode, std::uint32_t preset, bool reset)
	{
		if (status_ != RuntimeStatus::Initialized || !commandList || !color || !depth ||
			!motionVectors || !exposure || !output || !inputWidth || !inputHeight ||
			!outputWidth || !outputHeight)
			return SuperResolutionResult::Failed;

		HMODULE core = FindNgxCoreModule();
		if (!core) {
			detail_ = "NGX core module for private DLSS SR was not found";
			return SuperResolutionResult::Failed;
		}
		auto allocate = reinterpret_cast<AllocateParameters>(GetProcAddress(core, "NVSDK_NGX_D3D12_AllocateParameters"));
		auto create = reinterpret_cast<CreateFeature>(GetProcAddress(core, "NVSDK_NGX_D3D12_CreateFeature"));
		auto evaluate = reinterpret_cast<EvaluateFeature>(GetProcAddress(core, "NVSDK_NGX_D3D12_EvaluateFeature"));
		auto release = reinterpret_cast<ReleaseFeature>(GetProcAddress(core, "NVSDK_NGX_D3D12_ReleaseFeature"));
		if (!allocate || !create || !evaluate || !release) {
			detail_ = "NGX core is missing a private DLSS SR entry point";
			return SuperResolutionResult::Failed;
		}

		const bool creationChanged =
			superResolutionInputWidth_ != inputWidth || superResolutionInputHeight_ != inputHeight ||
			superResolutionOutputWidth_ != outputWidth || superResolutionOutputHeight_ != outputHeight ||
			superResolutionQualityMode_ != qualityMode || superResolutionPreset_ != preset;
		if (superResolutionFeatureHandle_ && creationChanged) {
			detail_ = "private DLSS SR creation settings changed without a fenced reset";
			return SuperResolutionResult::Failed;
		}

		if (!superResolutionParameters_) {
			NVSDK_NGX_Parameter* parameters = nullptr;
			ngxResult_ = static_cast<std::uint32_t>(allocate(&parameters));
			if (ngxResult_ != NVSDK_NGX_Result_Success || !parameters) {
				detail_ = std::format("private DLSS SR parameter allocation failed 0x{:08X}", ngxResult_);
				return SuperResolutionResult::Failed;
			}
			superResolutionParameters_ = parameters;
		}

		auto* parameters = static_cast<NVSDK_NGX_Parameter*>(superResolutionParameters_);
		if (!superResolutionFeatureHandle_) {
			parameters->Reset();
			parameters->Set(NVSDK_NGX_Parameter_Width, inputWidth);
			parameters->Set(NVSDK_NGX_Parameter_Height, inputHeight);
			parameters->Set(NVSDK_NGX_Parameter_OutWidth, outputWidth);
			parameters->Set(NVSDK_NGX_Parameter_OutHeight, outputHeight);
			parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
			parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
			parameters->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(ToPerfQuality(qualityMode)));
			parameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
				static_cast<unsigned int>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
			SetRenderPreset(parameters, preset);

			NVSDK_NGX_Handle* handle = nullptr;
			ngxResult_ = static_cast<std::uint32_t>(create(commandList,
				NVSDK_NGX_Feature_SuperSampling, parameters, &handle));
			if (ngxResult_ != NVSDK_NGX_Result_Success || !handle) {
				detail_ = std::format("private DLSS SR create failed 0x{:08X}", ngxResult_);
				return SuperResolutionResult::Failed;
			}
			superResolutionFeatureHandle_ = handle;
			superResolutionInputWidth_ = inputWidth;
			superResolutionInputHeight_ = inputHeight;
			superResolutionOutputWidth_ = outputWidth;
			superResolutionOutputHeight_ = outputHeight;
			superResolutionQualityMode_ = qualityMode;
			superResolutionPreset_ = preset;
			return SuperResolutionResult::Created;
		}

		parameters->Reset();
		parameters->Set(NVSDK_NGX_Parameter_Color, color);
		parameters->Set(NVSDK_NGX_Parameter_Output, output);
		parameters->Set(NVSDK_NGX_Parameter_Depth, depth);
		parameters->Set(NVSDK_NGX_Parameter_MotionVectors, motionVectors);
		parameters->Set(NVSDK_NGX_Parameter_ExposureTexture, exposure);
		parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, inputWidth);
		parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, inputHeight);
		parameters->Set(NVSDK_NGX_Parameter_Reset, reset ? 1u : 0u);
		parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, jitterOffsetX);
		parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, jitterOffsetY);
		parameters->Set(NVSDK_NGX_Parameter_MV_Scale_X, motionVectorScaleX);
		parameters->Set(NVSDK_NGX_Parameter_MV_Scale_Y, motionVectorScaleY);
		parameters->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,
			std::clamp(std::isfinite(frameTimeDeltaMilliseconds) ? frameTimeDeltaMilliseconds : 16.6667f,
				1.0f, 1000.0f));
		parameters->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
		parameters->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
		parameters->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);

		ngxResult_ = static_cast<std::uint32_t>(evaluate(commandList,
			static_cast<NVSDK_NGX_Handle*>(superResolutionFeatureHandle_), parameters, nullptr));
		if (ngxResult_ != NVSDK_NGX_Result_Success) {
			detail_ = std::format("private DLSS SR evaluate failed 0x{:08X}", ngxResult_);
			return SuperResolutionResult::Failed;
		}
		return SuperResolutionResult::Evaluated;
	}

	void Runtime::ResetSuperResolutionFeature()
	{
		HMODULE core = FindNgxCoreModule();
		auto release = core ? reinterpret_cast<ReleaseFeature>(GetProcAddress(core, "NVSDK_NGX_D3D12_ReleaseFeature")) : nullptr;
		if (superResolutionFeatureHandle_ && release) {
			const auto result = release(static_cast<NVSDK_NGX_Handle*>(superResolutionFeatureHandle_));
			if (result != NVSDK_NGX_Result_Success)
				logger::warn("[DLSSNR] Private DLSS SR release failed result=0x{:08X}", static_cast<std::uint32_t>(result));
		}
		superResolutionFeatureHandle_ = nullptr;
		superResolutionInputWidth_ = superResolutionInputHeight_ = 0;
		superResolutionOutputWidth_ = superResolutionOutputHeight_ = 0;
		superResolutionQualityMode_ = superResolutionPreset_ = 0;
	}

	void Runtime::ResetFeature()
	{
		ResetSuperResolutionFeature();
		if (!module_)
			return;
		SignedRuntimePathScope scope(static_cast<HMODULE>(module_), path_.parent_path() / L"nvngx.dll");
		auto release = reinterpret_cast<ReleaseFeature>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_D3D12_ReleaseFeature"));
		if (featureHandle_ && release) {
			const auto result = release(static_cast<NVSDK_NGX_Handle*>(featureHandle_));
			if (result != NVSDK_NGX_Result_Success)
				logger::warn("[DLSSNR] Feature 18 release failed result=0x{:08X} proxyInstalled={}",
					static_cast<std::uint32_t>(result), scope.IsInstalled());
		}
		featureHandle_ = nullptr;
		featureOutputWidth_ = featureOutputHeight_ = 0;
		successfulFrames_ = 0;
	}

	void Runtime::Shutdown()
	{
		if (device_ && module_) {
			ResetFeature();
			HMODULE core = FindNgxCoreModule();
			if (core) {
				auto destroy = reinterpret_cast<DestroyParameters>(GetProcAddress(core, "NVSDK_NGX_D3D12_DestroyParameters"));
				if (destroy && parameters_) destroy(static_cast<NVSDK_NGX_Parameter*>(parameters_));
				if (destroy && superResolutionParameters_)
					destroy(static_cast<NVSDK_NGX_Parameter*>(superResolutionParameters_));
			}
			parameters_ = nullptr;
			superResolutionParameters_ = nullptr;
			auto shutdown = core ? reinterpret_cast<ShutdownD3D12>(GetProcAddress(core, "NVSDK_NGX_D3D12_Shutdown1")) : nullptr;
			if (shutdown) {
				const auto result = shutdown(device_);
				if (result != NVSDK_NGX_Result_Success)
					logger::warn("[DLSSNR] NGX core shutdown failed result=0x{:08X}",
						static_cast<std::uint32_t>(result));
			}
			device_->Release();
			device_ = nullptr;
		}
		if (module_) FreeLibrary(static_cast<HMODULE>(module_));
		module_ = nullptr;
		status_ = RuntimeStatus::NotProbed;
		path_.clear();
		version_.clear();
		detail_.clear();
		ngxResult_ = applicationId_ = apiVersion_ = 0;
		successfulFrames_ = 0;
	}

	const char* ToString(RuntimeStatus status)
	{
		switch (status) {
		case RuntimeStatus::NotProbed: return "not-probed";
		case RuntimeStatus::NotFound: return "not-found";
		case RuntimeStatus::VersionUnavailable: return "version-unavailable";
		case RuntimeStatus::UnsupportedVersion: return "unsupported-version";
		case RuntimeStatus::LoadFailed: return "load-failed";
		case RuntimeStatus::MissingExport: return "missing-export";
		case RuntimeStatus::Ready: return "ready";
		case RuntimeStatus::InitializationFailed: return "initialization-failed";
		case RuntimeStatus::CoreUnavailable: return "core-unavailable";
		case RuntimeStatus::ParameterAllocationFailed: return "parameter-allocation-failed";
		case RuntimeStatus::Initialized: return "initialized";
		}
		return "unknown";
	}
}
