#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace NeuralRendering
{
	/**
	 * @brief User-tunable knobs forwarded to the DLSS Neural Rendering feature.
	 *
	 * These map directly onto the undocumented @c DLSSNR.* NGX parameters consumed
	 * by Feature 18. Defaults match NVIDIA's reference tuning.
	 */
	struct Tuning
	{
		float intensity = 0.8f;
		float localToneStrength = 0.75f;
		float localStructureStrength = 0.9f;
		float skinStructureStrength = 0.9f;
		std::uint32_t style = 3;
		bool useAutoMask = true;
		bool uiCorrection = false;
	};

	/** @brief Lifecycle/diagnostic state of the nvngx_dlssnr.dll runtime. */
	enum class RuntimeStatus
	{
		NotProbed,
		NotFound,
		VersionUnavailable,
		UnsupportedVersion,
		LoadFailed,
		MissingExport,
		Ready,
		InitializationFailed,
		CoreUnavailable,
		ParameterAllocationFailed,
		Initialized,
	};

	/**
	 * @brief Direct D3D12 binding to NVIDIA's DLSS Neural Rendering runtime (NGX Feature 18).
	 *
	 * @c nvngx_dlssnr.dll only exposes a D3D12 ABI and refuses to operate unless its
	 * caller's module path resolves to NVIDIA's signed @c nvngx.dll. Every entry point
	 * is therefore invoked underneath an import-address-table hook on
	 * @c GetModuleFileNameW installed in @c nvngx_dlssnr.dll's own import table, which
	 * reports the signed path for the duration of the call and is removed immediately
	 * afterwards.
	 *
	 * This repository evaluates at most one Neural Rendering pass per frame, so a
	 * single NGX feature handle is kept.
	 */
	class Runtime
	{
	public:
		/** @brief Access the process-wide runtime singleton. */
		static Runtime& Instance();
		~Runtime();
		Runtime(const Runtime&) = delete;
		Runtime& operator=(const Runtime&) = delete;

		/**
		 * @brief Locate, version-gate and load nvngx_dlssnr.dll.
		 *
		 * Only the 310.8.x runtime series is accepted; other versions do not honour
		 * the parameter contract used by Execute(). On success the signed runtime's
		 * application id and NGX API version are read from its identity exports and
		 * Status() becomes RuntimeStatus::Ready.
		 *
		 * @param explicitPath Optional DLL path, or a directory containing the DLL.
		 *                     When empty the Streamline plugin folders under Data are searched.
		 * @return True when the runtime was loaded and all required exports are present.
		 */
		bool Probe(const std::filesystem::path& explicitPath = {});

		/**
		 * @brief Initialize NGX against a D3D12 device and allocate the parameter block.
		 *
		 * Probes automatically when the runtime is not loaded yet. Re-initializes when
		 * called with a different device. The parameter allocator is resolved from the
		 * already-loaded NGX core module rather than from the snippet DLL.
		 *
		 * @param device The D3D12 device the feature will be evaluated on.
		 * @param dataPath Writable directory for NGX logs/caches; a temporary folder is used when empty.
		 * @return True when Status() is RuntimeStatus::Initialized.
		 */
		bool Initialize(ID3D12Device* device, const std::filesystem::path& dataPath = {});

		/**
		 * @brief Create (if needed) and evaluate the Neural Rendering feature.
		 *
		 * The feature handle is built for the output extents and reused across frames.
		 * A smaller input (render) region is passed through the NGX subrects, so
		 * dynamic resolution and the Before/After placement toggle do not recreate it;
		 * only an output-extent change does, and the caller must have drained any
		 * command list still referencing the handle before that happens.
		 * All resources must be D3D12 resources visible to the device passed to Initialize().
		 *
		 * @param commandList Command list to record the evaluation into.
		 * @param color Input colour buffer.
		 * @param depth Input depth buffer.
		 * @param motionVectors Input motion vector buffer.
		 * @param output Output colour buffer.
		 * @param inputWidth Guide (render) resolution width.
		 * @param inputHeight Guide (render) resolution height.
		 * @param outputWidth Colour/output resolution width.
		 * @param outputHeight Colour/output resolution height.
		 * @param motionVectorScaleX Motion vector X scale applied by the runtime.
		 * @param motionVectorScaleY Motion vector Y scale applied by the runtime.
		 * @param tuning Feature tuning parameters.
		 * @param reset True to discard temporal history this frame.
		 * @return True when the evaluation succeeded.
		 */
		bool Execute(ID3D12GraphicsCommandList* commandList,
			ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motionVectors, ID3D12Resource* output,
			std::uint32_t inputWidth, std::uint32_t inputHeight, std::uint32_t outputWidth, std::uint32_t outputHeight,
			float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning, bool reset);

		/**
		 * @brief Release the NGX feature handle and clear the cached extents.
		 *
		 * The next Execute() recreates the feature. The successful-frame counter is reset.
		 * Safe to call when no feature has been created.
		 */
		void ResetFeature();

		/** @brief Release the feature, parameters, NGX instance and the loaded module. */
		void Shutdown();

		/** @brief Current lifecycle/diagnostic state. */
		[[nodiscard]] RuntimeStatus Status() const { return status_; }
		/** @brief Formatted file version of the loaded nvngx_dlssnr.dll. */
		[[nodiscard]] const std::string& Version() const { return version_; }
		/** @brief Human-readable detail for the most recent failure. */
		[[nodiscard]] const std::string& Detail() const { return detail_; }
		/** @brief Raw NVSDK_NGX_Result of the most recent NGX call. */
		[[nodiscard]] std::uint32_t NgxResult() const { return ngxResult_; }
		/** @brief Application id reported by the signed runtime's identity export. */
		[[nodiscard]] std::uint32_t ApplicationId() const { return applicationId_; }
		/** @brief NGX API version reported by the signed runtime's identity export. */
		[[nodiscard]] std::uint32_t ApiVersion() const { return apiVersion_; }
		/** @brief Number of successful evaluations since the last feature reset. */
		[[nodiscard]] std::uint64_t SuccessfulFrames() const { return successfulFrames_; }

	private:
		Runtime() = default;
		void* module_ = nullptr;
		void* parameters_ = nullptr;
		void* featureHandle_ = nullptr;
		// Output (native) extents the feature was built for. The active render
		// region is passed per frame as an NGX subrect, not baked into the handle,
		// so dynamic resolution and the Before/After placement toggle never rebuild.
		std::uint32_t featureOutputWidth_ = 0;
		std::uint32_t featureOutputHeight_ = 0;
		ID3D12Device* device_ = nullptr;
		RuntimeStatus status_ = RuntimeStatus::NotProbed;
		std::filesystem::path path_;
		std::string version_;
		std::string detail_;
		std::uint32_t ngxResult_ = 0;
		std::uint32_t applicationId_ = 0;
		std::uint32_t apiVersion_ = 0;
		std::uint64_t successfulFrames_ = 0;
	};

	/**
	 * @brief Convert a RuntimeStatus to a stable lowercase identifier for logs and UI.
	 * @param status The status to describe.
	 * @return A static string such as "initialized" or "unsupported-version".
	 */
	const char* ToString(RuntimeStatus status);
}
