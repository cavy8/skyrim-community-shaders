#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace NeuralRenderingNGX
{
	/**
	 * @brief Feature 18 DLSSNR.* tuning parameters, latched at creation and supplied by Neural Rendering
	 * settings.
	 */
	struct Tuning
	{
		float intensity = 1.0f;
		float localToneStrength = 1.0f;
		float localStructureStrength = 1.0f;
		float skinStructureStrength = -1.0f;
		std::uint32_t style = 3;
		bool useAutoMask = true;
		bool uiCorrection = false;

		/// Every field here is latched at feature-create time (see Execute()), so
		/// this is how Execute() notices a slider changed and rebuilds the feature.
		friend bool operator==(const Tuning&, const Tuning&) = default;
	};

	/** @brief Result of recording the private DLSS-SR residual pass. */
	enum class SuperResolutionResult
	{
		Failed,
		Created,
		Evaluated,
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
	 * @brief Direct D3D12 binding to NGX Feature 18. Each entry point temporarily patches the caller-path
	 * import to satisfy the signed nvngx.dll gate.
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
		 * @brief Load the supported 310.8.x runtime series and its identity exports. On success Status() is
		 * Ready.
		 *
		 * @param explicitPath Optional DLL path, or a directory containing the DLL.
		 *                     When empty the Streamline plugin folders under Data are searched.
		 * @return True when the runtime was loaded and all required exports are present.
		 */
		bool Probe(const std::filesystem::path& explicitPath = {});

		/**
		 * @brief Initialize NGX and allocate parameters from the resident core. Probe if needed; reinitialize
		 * when the device changes.
		 *
		 * @param device The D3D12 device the feature will be evaluated on.
		 * @param dataPath Writable directory for NGX logs/caches; a temporary folder is used when empty.
		 * @return True when Status() is RuntimeStatus::Initialized.
		 */
		bool Initialize(ID3D12Device* device, const std::filesystem::path& dataPath = {});

		/**
		 * @brief Create or reuse a feature for the output allocation and evaluate active subrects. The caller
		 * must drain in-flight commands before a size change. All resources must belong to the initialized
		 * D3D12 device.
		 *
		 * @param commandList Command list to record the evaluation into.
		 * @param color Input colour buffer.
		 * @param depth Input depth buffer.
		 * @param motionVectors Input motion vector buffer.
		 * @param output Output colour buffer.
		 * @param colorWidth Valid (written) region of the colour and output buffers - render
		 *                   resolution when Neural Rendering runs before the upscaler, display
		 *                   resolution when it runs after.
		 * @param colorHeight Valid region height of the colour and output buffers.
		 * @param guideWidth Valid region of the depth and motion-vector buffers. These come from
		 *                   the game's render pass and are at render (dynamic) resolution in both
		 *                   placements.
		 * @param guideHeight Valid region height of the depth and motion-vector buffers.
		 * @param outputWidth Allocation width of the colour/output buffers; the feature is created
		 *                    for this and never rebuilt for a smaller valid region.
		 * @param outputHeight Allocation height of the colour/output buffers.
		 * @param motionVectorScaleX Factor turning a motion-vector texel into pixels of @p guideWidth.
		 *                           For Skyrim's normalised vectors this is @p guideWidth itself; it
		 *                           must NOT also fold in the render/display ratio, which the subrects
		 *                           already carry.
		 * @param motionVectorScaleY Motion-vector Y scale, analogous to @p motionVectorScaleX.
		 * @param tuning Feature tuning parameters.
		 * @param reset True to discard temporal history this frame.
		 * @param depthInverted True when @p depth is Reverse Z (near = 1, far = 0).
		 * @return True when the evaluation succeeded.
		 */
		bool Execute(ID3D12GraphicsCommandList* commandList,
			ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motionVectors, ID3D12Resource* output,
			std::uint32_t colorWidth, std::uint32_t colorHeight,
			std::uint32_t guideWidth, std::uint32_t guideHeight,
			std::uint32_t outputWidth, std::uint32_t outputHeight,
			float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning, bool reset,
			bool depthInverted);

		/**
		 * @brief Create/evaluate private DLSS-SR for the residual carrier. Creation occupies a separate
		 * submission before evaluation. Uses LDR, unit exposure, and no sharpening; depthInverted is fixed at
		 * creation.
		 */
		SuperResolutionResult ExecuteSuperResolution(ID3D12GraphicsCommandList* commandList,
			ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motionVectors,
			ID3D12Resource* exposure, ID3D12Resource* output,
			std::uint32_t inputWidth, std::uint32_t inputHeight,
			std::uint32_t outputWidth, std::uint32_t outputHeight,
			float jitterOffsetX, float jitterOffsetY,
			float motionVectorScaleX, float motionVectorScaleY,
			float frameTimeDeltaMilliseconds,
			std::uint32_t qualityMode, std::uint32_t preset, bool reset, bool depthInverted);

		/**
		 * @brief Release the NGX feature handle and clear the cached extents.
		 *
		 * The next Execute() recreates the feature. The successful-frame counter is reset.
		 * Safe to call when no feature has been created.
		 */
		void ResetFeature();

		/** @brief Release only the private DLSS-SR feature and its cached creation state. */
		void ResetSuperResolutionFeature();

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

		/**
		 * @brief Log support, minimum GPU architecture, and OS requirements once. The query does not report
		 * accepted creation flags.
		 */
		void LogFeatureRequirements(ID3D12Device* device);
		void* module_ = nullptr;
		void* parameters_ = nullptr;
		void* featureHandle_ = nullptr;
		void* superResolutionParameters_ = nullptr;
		void* superResolutionFeatureHandle_ = nullptr;
		// Creation extents; per-frame active regions are supplied as NGX subrects.
		std::uint32_t featureOutputWidth_ = 0;
		std::uint32_t featureOutputHeight_ = 0;
		std::uint32_t superResolutionInputWidth_ = 0;
		std::uint32_t superResolutionInputHeight_ = 0;
		std::uint32_t superResolutionOutputWidth_ = 0;
		std::uint32_t superResolutionOutputHeight_ = 0;
		std::uint32_t superResolutionQualityMode_ = 0;
		std::uint32_t superResolutionPreset_ = 0;
		ID3D12Device* device_ = nullptr;
		RuntimeStatus status_ = RuntimeStatus::NotProbed;
		std::filesystem::path path_;
		std::string version_;
		std::string detail_;
		bool hasFeatureRequirements_ = false;
		bool featureRequirementsLogged_ = false;
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
