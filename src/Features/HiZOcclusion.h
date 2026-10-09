#pragma once

#include "OverlayFeature.h"

#include <array>
#include <atomic>
#include <unordered_map>

#include <RE/N/NiSmartPointer.h>

// Operational states for HiZ occlusion system
enum class HiZStatus : uint8_t
{
	Init,              // Initial state, not yet set up
	CompilingShaders,  // Shader compilation in progress
	ShadersReady,      // Shaders compiled successfully
	ResourcesReady,    // All resources initialized and valid
	Running,           // Actively processing occlusion tests
	ValidationFailed,  // Resource validation failed
	Error              // Error state - check statusMessage for details
};

// Convert HiZStatus to display string
inline const char* HiZStatusToString(HiZStatus status)
{
	switch (status) {
	case HiZStatus::Init:
		return "Initializing";
	case HiZStatus::CompilingShaders:
		return "Compiling Shaders";
	case HiZStatus::ShadersReady:
		return "Shaders Ready";
	case HiZStatus::ResourcesReady:
		return "Resources Ready";
	case HiZStatus::Running:
		return "Running";
	case HiZStatus::ValidationFailed:
		return "Validation Failed";
	case HiZStatus::Error:
		return "Error";
	default:
		return "Unknown";
	}
}

struct HiZOcclusion : OverlayFeature
{
	virtual ~HiZOcclusion();

	virtual inline std::string GetName() override { return "HiZ Occlusion Culling"; }
	virtual inline std::string GetShortName() override { return "HiZOcclusion"; }
	virtual inline std::string_view GetShaderDefineName() override { return "HIZ_OCCLUSION"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kDisplay; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.hiz_occlusion.description", "Implements Hierarchical-Z (Hi-Z) occlusion culling to improve rendering performance by testing object visibility against a GPU-generated depth pyramid before expensive rendering operations."),
			{ T("feature.hiz_occlusion.key_feature_1", "GPU-accelerated occlusion testing using hierarchical depth buffer"),
				T("feature.hiz_occlusion.key_feature_2", "Reduces CPU overhead by culling invisible objects before draw calls"),
				T("feature.hiz_occlusion.key_feature_3", "Conservative depth testing with configurable bias for robustness"),
				T("feature.hiz_occlusion.key_feature_4", "Debug visualization tools for occlusion bounds and culling statistics"),
				T("feature.hiz_occlusion.key_feature_5", "Triple-buffered async readback for optimal CPU-GPU synchronization"),
				T("feature.hiz_occlusion.key_feature_6", "Uses the full float depth precision of Reverse Z for distant occluders") }
		};
	}

	virtual bool IsCore() const override { return false; }

	// Preserve Feature base-class contract; implementation will call InitShaders()
	virtual void SetupResources() override;
	// Explicit shader initialization entry point
	void InitShaders();
	// Ensure/create Hi-Z texture + per-mip SRVs/UAVs sized to current depth
	bool InitHiZResources();
	// Unbind all compute stage resources used by Hi-Z passes
	void UnbindD3DResources();
	virtual void Prepass() override;
	virtual void Reset() override;

	bool wasEnabled = false;

	void CreateDebugBuffer();
	void ReleaseDebugBuffer();

	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void ClearShaderCache() override;

	// OverlayFeature interface
	virtual void DrawOverlay() override;
	virtual bool IsOverlayVisible() const override;

	uint32_t currentFrame = 0;

	HiZStatus status = HiZStatus::Init;  // Current operational state
	std::string statusMessage;           // Detailed error/info message for debugging
	bool resourcesSetup = false;         // whether resources have been initialized
	bool resourcesValid = false;         // whether current resources are valid and safe to use

	// Per-frame tracking to verify build/draw order and availability
	uint32_t lastBuiltFrame = 0;           // frame index when the pyramid was last built
	bool builtThisFrame = false;           // true if EarlyPrepass() successfully built the pyramid this frame
	bool preserveResourcesForUI = true;    // prevent Reset() from destroying resources needed for debug viewer
	uint32_t resourceCreationFrame = 0;    // frame when resources were last created
	bool vectorsReserved = false;          // true after initial vector capacity reservation
	bool skipValidationThisFrame = false;  // skip resource validation to prevent crashes during compilation
	bool overlayUpdatedThisFrame = false;  // tracks whether bounds overlay was refreshed this frame
	bool unexpectedDepthFormatLogged = false;
	bool debugShaderCompileAttempted = false;

	struct Settings
	{
		// Debug viewer settings
		bool enableHiZViewer = false;  // show Hi-Z mip viewer window
		uint32_t hizViewerMip = 0;     // selected mip level to view
		float hizViewerScale = 1.0f;   // scale factor for display size
		bool debugMode = false;        // enable debug mode for Hi-Z testing

		// Hi-Z culling settings
		bool enableHiZCulling = true;     // enable Hi-Z occlusion culling
		float conservativeBias = 0.001f;  // relative view-depth tolerance for conservative testing (0.01 = 1%)
		bool showCullingStats = false;    // show Hi-Z culling statistics in UI

		// Bounds overlay viewer (draw tested bounds and closest point)
		bool enableBoundsViewer = false;  // enable per-object bounds debug overlay
		uint32_t boundsMaxObjects = 256;  // maximum objects to draw outlines for (subsampled)

		// Individual toggles for each early-out reason color
		bool showVisTestPassed = true;
		bool showVisInsideBounds = true;
		bool showVisInvalidRadius = true;
		bool showCulledFrustum = true;
		bool showCulledNoEarlyOut = true;

		// Per-result-type culling toggles (disable to diagnose false positives)
		bool cullNoEarlyOut = true;  // cull depth-test-failed objects (red)

		uint32_t consecutiveOccludedThreshold = 1;  // 1-100, cull after N consecutive occluded tests
		float shadowSweepDistance = 4096.0f;
		float guardAngle = 10.0f;
		float motionMarginFrames = 3.0f;

		std::array<bool, 30> cullRenderMode = { true };  // Whether each culling type should be allowed
	};

	Settings settings;

	// Hi-Z pyramid resources
	ID3D11Texture2D* hiZTexture = nullptr;                  // R32_FLOAT, full mip chain
	ID3D11ShaderResourceView* hiZSRV = nullptr;             // SRV over all mips
	std::vector<ID3D11ShaderResourceView*> hiZSRVsPerMip;   // SRV per mip slice (MostDetailedMip=i, MipLevels=1)
	std::vector<ID3D11UnorderedAccessView*> hiZUAVs;        // UAV per mip slice
	uint32_t hiZWidth = 0, hiZHeight = 0, hiZMipCount = 0;  // cached dims

	// Shaders for building the Hi-Z pyramid
	ID3D11ComputeShader* hiZBuildLevel0CS = nullptr;  // depth -> mip 0
	ID3D11ComputeShader* hiZDownsampleCS = nullptr;   // mip n -> mip n+1 (farthest-depth reduction)
	ID3D11ComputeShader* hiZTestCS = nullptr;         // GPU-based occlusion testing (production, no debug overlay)
	ID3D11ComputeShader* hiZTestCSDebug = nullptr;    // GPU-based occlusion testing (with debug overlay)

	// GPU culling resources
	ID3D11Buffer* geometryBoundsBuffer = nullptr;  // Input: geometry bounding spheres
	ID3D11ShaderResourceView* geometryBoundsSRV = nullptr;
	ID3D11Buffer* visibilityResultsBuffer = nullptr;  // Output: visibility results
	ID3D11UnorderedAccessView* visibilityResultsUAV = nullptr;
	ID3D11Buffer* debugResultsBuffer = nullptr;  // Staging buffer for CPU readback
	ID3D11UnorderedAccessView* debugResultsUAV = nullptr;
	ID3D11Buffer* hiZTestParamsBuffer = nullptr;  // Constant buffer for test parameters

	ID3D11Buffer* visibilityReadbackBuffer = nullptr;
	uint32_t readbackFrameIndex = 0;

	ID3D11SamplerState* hiZSampler = nullptr;  // Sampler for Hi-Z sampling in CS
	uint32_t maxGeometryCount = 16384;         // Maximum geometry objects per frame

	// GPU timestamp queries for accurate timing (triple-buffered for async readback)
	static const uint32_t GPU_TIMING_BUFFER_COUNT = 3;
	struct GPUTimingQueries
	{
		ID3D11Query* disjointQuery = nullptr;
		ID3D11Query* beginTimestamp = nullptr;
		ID3D11Query* endTimestamp = nullptr;
		bool pending = false;
	};
	GPUTimingQueries gpuTimingQueries[GPU_TIMING_BUFFER_COUNT];
	uint32_t gpuTimingWriteIndex = 0;
	uint32_t gpuTimingReadIndex = 0;

	// Bounds debug overlay resources (RGBA8, size = HiZ base dimensions)
	ID3D11Texture2D* boundsOverlayTex = nullptr;
	ID3D11ShaderResourceView* boundsOverlaySRV = nullptr;
	ID3D11UnorderedAccessView* boundsOverlayUAV = nullptr;
	uint32_t boundsOverlayW = 0, boundsOverlayH = 0;

	// Overlay resource management
	bool SetupBoundsOverlayResources(uint32_t width, uint32_t height);
	void ReleaseBoundsOverlayResources();
	void ClearBoundsOverlay();

	// Hi-Z occlusion culling functionality
	bool SetupGPUCullingResources();

	void DispatchComputeShader();
	void ProcessVisibilityResults(uint32_t bufferIndex);

	ID3D11ShaderResourceView* GetSourceDepthSRV() const;

	// Accessors for culling step
	inline ID3D11ShaderResourceView* GetHiZSRV() const { return hiZSRV; }
	inline uint32_t GetHiZMipCount() const { return hiZMipCount; }

	enum PassKind : uint8_t
	{
		kCameraPass = 1 << 0,
		kSunShadowPass = 1 << 1
	};

	static uint8_t GetPassKind(uint32_t renderMode);
	static const char* GetRenderModeName(uint32_t renderMode);
	static bool IsMainViewRegistration(uint32_t renderMode, const RE::BSGraphics::BSShaderAccumulator* accum);
	void CaptureSunShadowDirection(const RE::NiCamera* camera);

	std::array<std::atomic<float>, 3> sunShadowDirection{};
	std::atomic<bool> sunShadowDirectionCaptured = false;

	// Check if a given geometry was determined to be occluded
	bool IsGeometryOccluded(RE::BSGeometry* geometry) const;
	bool IsShadowCasterOccluded(RE::BSGeometry* geometry) const;

	bool IsPlayerAttachedGeometry(RE::BSGeometry* geometry, RE::TESObjectREFR* refr) const;

	RE::TESObjectREFR* grabbedReference = nullptr;

	// Debugging result struct (x = object depth, y = max scene depth)

	// -3 = Not culled: Test passed
	// -2 = Not culled: Inside bounds
	// -1 = Not culled: Invalid Radius
	//  0 = Default value
	//  1 = Culled: Frustum
	//  2 = Culled: No early out
	struct OcclusionResult
	{
		uint32_t result;
	};

	// Comprehensive debug data from GPU (matches shader DebugData struct)
	struct DebugData
	{
		DirectX::XMFLOAT4 centerWS_radius;     // xyz=centerWS, w=radius
		DirectX::XMFLOAT4 centerRel_objDepth;  // xyz=centerWSCameraRelative, w=objDepth
		float sceneDepth;
		uint32_t earlyOutReason;
		DirectX::XMFLOAT2 padding;  // Total: 48 bytes
	};

	// Culling statistics
	struct CullingStats
	{
		uint32_t totalTested = 0;
		uint32_t frameIndex = 0;
		uint32_t geometryListSize = 0;

		// Test result statistics
		uint32_t visTestPassed = 0;
		uint32_t visInsideBounds = 0;
		uint32_t visInvalidRadius = 0;
		uint32_t defaultValue = 0;
		uint32_t culledFrustum = 0;
		uint32_t culledNoEarlyOut = 0;

		// Early culling at scene traversal (prevents all downstream CPU work)
		std::atomic<uint32_t> earlyCulledCount = 0;  // geometry culled at AppendVirtual before batching

		// Async readback tracking
		uint32_t lastResultFrame = 0;  // frame when results were last updated
		uint32_t staleFrameCount = 0;  // frames since last new result (0 = fresh data)

		// Timing statistics (in milliseconds)
		float resourceSetupDurationMS = 0.0f;
		float recreateDurationMS = 0.0f;
		float gpuCullingTimeMs = 0.0f;
		float hiZBuildTimeMs = 0.0f;
		float readbackTimeMs = 0.0f;
		float copyTimeMs = 0.0f;
		float mapTimeMs = 0.0f;
		float unmapTimeMs = 0.0f;
		float copyDataTimeMs = 0.0f;

		std::atomic<uint32_t> accumRegisterCalls{ 0 };
		std::array<std::atomic<uint32_t>, 30> renderModeCalls{};

		void Reset()
		{
			totalTested = 0;
			frameIndex = 0;
			geometryListSize = 0;
			visTestPassed = 0;
			visInsideBounds = 0;
			visInvalidRadius = 0;
			defaultValue = 0;
			culledFrustum = 0;
			culledNoEarlyOut = 0;

			earlyCulledCount.store(0, std::memory_order_relaxed);

			resourceSetupDurationMS = 0.0f;
			recreateDurationMS = 0.0f;
			gpuCullingTimeMs = 0.0f;
			hiZBuildTimeMs = 0.0f;
			readbackTimeMs = 0.0f;
			copyTimeMs = 0.0f;
			mapTimeMs = 0.0f;
			unmapTimeMs = 0.0f;
			copyDataTimeMs = 0.0f;

			accumRegisterCalls.store(0, std::memory_order_relaxed);
			for (auto& callCount : renderModeCalls) {
				callCount.store(0, std::memory_order_relaxed);
			}
		}
	};
	CullingStats stats;

	struct PendingRecord
	{
		RE::BSGeometry* geometry;
		uint8_t passes;
	};

	struct TestEntry
	{
		DirectX::XMFLOAT4 bounds;
		DirectX::XMFLOAT4 sweep;
	};

	struct SnapshotEntry
	{
		RE::NiPointer<RE::BSGeometry> geometry;
		PassKind pass;
	};

	struct OcclusionTracker
	{
		std::unordered_map<RE::BSGeometry*, RE::NiPointer<RE::BSGeometry>> occluded;
		std::unordered_map<RE::BSGeometry*, uint32_t> counts;
	};

	struct AsyncReadbackState
	{
		static const int BUFFER_COUNT = 3;  // Triple buffering to handle GPU latency
		ID3D11Buffer* stagingBuffers[BUFFER_COUNT] = {};
		ID3D11Query* completionQueries[BUFFER_COUNT] = {};  // Event queries to detect GPU completion
		D3D11_MAPPED_SUBRESOURCE mappedData[BUFFER_COUNT] = {};
		bool hasPendingRead[BUFFER_COUNT] = {};
		uint32_t pendingFrameIndex[BUFFER_COUNT] = {};
		std::vector<SnapshotEntry> geometrySnapshots[BUFFER_COUNT];  // Geometry tested in each buffer
		uint32_t geometryCount[BUFFER_COUNT] = {};                   // Number of geometry in each buffer
		uint32_t writeIndex = 0;                                     // Next buffer to write GPU results to
		uint32_t readIndex = 0;                                      // Next buffer to try reading from
		uint32_t numPendingReads = 0;                                // Track how many buffers have pending reads
		DirectX::XMFLOAT3 cameraForward[BUFFER_COUNT] = {};
	};
	AsyncReadbackState readbackState;

	// Thread-local vector to hold geometry collected by the current worker thread
	inline static thread_local std::vector<PendingRecord> localPendingGeometry;

	// Tracking active thread-local vectors
	std::vector<std::vector<PendingRecord>*> allThreadVectors;
	std::mutex threadVectorsMutex;

	// Shared state for async pipeline
	uint32_t numGeometry = 0;                            // Number of test entries in current batch
	std::vector<SnapshotEntry> pendingGeometrySnapshot;  // Snapshot for current dispatch

	// Geometry batch for GPU culling
	std::vector<RE::NiPointer<RE::BSGeometry>> pendingGeometry;
	std::vector<uint8_t> pendingPasses;
	std::unordered_map<RE::BSGeometry*, size_t> pendingGeometryIndex;
	std::vector<TestEntry> geometryBounds;

	DirectX::XMFLOAT3 lastEyePosition = {};
	DirectX::XMFLOAT3 lastCameraForward = { 0.0f, 1.0f, 0.0f };
	DirectX::XMFLOAT3 currentCameraForward = { 0.0f, 1.0f, 0.0f };
	DirectX::XMFLOAT3 appliedShadowForward = { 0.0f, 1.0f, 0.0f };
	bool hasCameraHistory = false;
	float frameRotation = 0.0f;
	float motionMargin = 0.0f;

	// Occlusion flag bit - uses unused bit 30 in NiAVObject::flags for O(1) lookup
	// This avoids hash set lookups in hot paths (early culling hooks)
	static constexpr uint32_t kOccludedFlag = 1u << 30;
	static constexpr float kCameraCutDistance = 1000.0f;
	static constexpr float kCameraCutAngle = 30.0f;

	OcclusionTracker cameraOcclusion;
	OcclusionTracker shadowOcclusion;

	static void SetOccludedFlag(RE::BSGeometry* geometry, bool occluded);
	void ClearOcclusionState();
	void ClearShadowOcclusionState();
	void UpdateCameraMotion();
	bool IsReadbackReady(uint32_t bufferIndex);
	void ReleaseReadbackSlot(uint32_t bufferIndex);

	void ExecuteVisibilityTests();
	void ConsolidatePendingGeometry();

	struct HiZSettings
	{
		DirectX::XMFLOAT4 hiZParams;            // 16 (mipCount, conservativeBias, geometryCount, debugMode)
		DirectX::XMFLOAT4 overlaySettings;      // 16 (overlayEnabled, maxObjectsToDraw, unused, unused)
		DirectX::XMFLOAT4 overlayColorToggles;  // 16 (8 bits per toggle: behind|invalid|centerOff|camInside|invalidDepth|nearestOff|visible|occluded)

		DirectX::XMFLOAT3 cameraWorldPos;  // 12
		float motionMargin = 0.0f;         //  4 -> 16

		DirectX::XMFLOAT2 bufferDim;  //  8
		DirectX::XMFLOAT2 guardBand;  //  8 -> 16
	};
	static_assert(sizeof(HiZSettings) % 16 == 0, "HiZSettings must be 16B-sized");
	static_assert(alignof(HiZSettings) <= 16, "HiZSettings alignment should not exceed 16");
};
