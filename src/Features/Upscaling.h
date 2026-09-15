#pragma once

#include "Feature.h"
#include "Upscaling/DX12SwapChain.h"
#include "Upscaling/FidelityFX.h"
#include "Upscaling/NeuralRendering.h"
#include "Upscaling/RCAS/RCAS.h"
#include "Upscaling/Streamline.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <winrt/base.h>

/**
 * @brief Provides upscaling functionality including DLSS, FSR and TAA.
 *
 * This feature handles various upscaling methods and frame generation technologies
 * to improve performance while maintaining visual quality.
 */
struct Upscaling : Feature
{
private:
	static constexpr std::string_view MOD_ID = "156952";

public:
	// Feature interface
	virtual inline std::string GetName() override { return "Upscaling"; }
	virtual std::string GetDisplayName() override { return T("feature.upscaling.name", "Upscaling"); }
	virtual inline std::string GetShortName() override { return "Upscaling"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual inline bool IsCore() const override { return false; }
	virtual inline std::string_view GetCategory() const override { return FeatureCategories::kDisplay; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.upscaling.description", "Advanced upscaling and frame generation technologies for improved performance"),
			{ T("feature.upscaling.key_feature_1", "DLSS (Deep Learning Super Sampling) support"),
				T("feature.upscaling.key_feature_2", "FSR (FidelityFX Super Resolution) support"),
				T("feature.upscaling.key_feature_3", "TAA (Temporal Anti-Aliasing) support"),
				T("feature.upscaling.key_feature_4", "Frame generation for supported systems") } };
	};

	float2 jitter = { 0, 0 };

	enum class UpscaleMethod
	{
		kNONE,
		kTAA,
		kFSR,
		kDLSS
	};

	struct Settings
	{
		uint upscaleMethod = (uint)UpscaleMethod::kDLSS;
		uint upscaleMethodNoDLSS = (uint)UpscaleMethod::kFSR;
		uint qualityMode = 1;  // Default to Quality (1=Quality, 2=Balanced, 3=Performance, 4=Ultra Performance, 0=Native AA)
		uint frameLimitMode = 1;
		uint frameGenerationMode = 1;
		uint frameGenerationForceEnable = 0;
		bool frameGenerationAllowInMenus = false;
		uint streamlineLogLevel = 0;  // 0=Off, 1=Default, 2=Verbose
		float sharpnessFSR = 0.0f;
		bool sharpnessEnabledDLSS = false;
		float sharpnessDLSS = 0.0f;
		uint presetDLSS = 0;  // 0=Default, 1=J, 2=K, 3=L, 4=M
		bool reflexLowLatencyMode = false;
		bool reflexLowLatencyBoost = false;
		bool reflexUseMarkersToOptimize = false;
		bool reflexUseFPSLimit = false;
		float reflexFPSLimit = 60.0f;
		bool neuralRenderingEnabled = false;
		uint neuralRenderingPlacement = 3;  // 0=Before Upscaling, 1=After Upscaling, 2=Separate Upscaling, 3=Finished Image (default)
		uint neuralRenderingStyle = 0;      // 0=Default, 1=Natural, 2=Cinematic
		float neuralRenderingIntensity = 1.0f;
		float neuralRenderingColorStrength = 1.0f;
		float neuralRenderingLocalToneStrength = 1.0f;
		float neuralRenderingLocalStructureStrength = 1.0f;
		float neuralRenderingSkinStructureStrength = -1.0f;
		bool neuralRenderingAutomaticMask = true;
		uint neuralRenderingResolutionMode = 0;  // 0=Uniform scale, 1=Per-axis (experimental anamorphic) scale
		float neuralRenderingResolutionScale = 1.0f;
		float neuralRenderingResolutionScaleX = 1.0f;
		float neuralRenderingResolutionScaleY = 1.0f;
		float neuralRenderingTransferStrength = 1.0f;
		float neuralRenderingLuminosityStrength = 1.0f;
		float neuralRenderingMaxRatio = 2.0f;  // Two-sided guard on the model/proxy luminance ratio (1/x..x).
		NeuralRendering::CategoryStrengths neuralRenderingEverythingElseStrengths;
		NeuralRendering::CategoryStrengths neuralRenderingSkinStrengths;
		// Hair is the only category that hue-guards its chroma change by default.
		NeuralRendering::CategoryStrengths neuralRenderingHairStrengths{ 1.0f, 1.0f, 1.0f, true };
		NeuralRendering::CategoryStrengths neuralRenderingEyesStrengths;
		NeuralRendering::CategoryStrengths neuralRenderingFoliageStrengths;
		NeuralRendering::CategoryStrengths neuralRenderingLandscapeStrengths;
		NeuralRendering::CategoryStrengths neuralRenderingEquipmentStrengths;
		bool neuralRenderingDepthAwareResolve = true;
		bool neuralRenderingAlternateFrames = false;
		/// Debug view: render each pixel's classified material category as a flat colour instead
		/// of the model's edit. See NeuralRendering::Options::debugCategoryView.
		bool neuralRenderingDebugCategoryView = false;
		bool neuralRenderingRawModelOutput = false;  // Diagnostic: skip the resolve, write Feature 18's answer directly (Finished Image only).
	};

	Settings settings;

	struct JitterCB
	{
		float2 jitter;
		float useWideKernel;
		float pad0;
	};

	struct UpscalingDataCB
	{
		float2 trueSamplingDim;
		float2 pad0;
	};

	ConstantBuffer* jitterCB = nullptr;
	ConstantBuffer* upscalingDataCB = nullptr;

	// Runtime state
	bool isWindowed = false;
	bool lowRefreshRate = false;
	bool fidelityFXMissing = false;
	bool d3d12SwapChainActive = false;

	// Timing and scaling
	double refreshRate = 0.0f;
	float2 resolutionScale = { 1.0f, 1.0f };
	LARGE_INTEGER qpf;

	// FG FPS Measurement for Overlay
	bool IsFrameGenerationDx12PathActive() const;
	bool IsFrameGenerationActive() const;
	bool ShouldUseFrameGenerationThisFrame() const;
	float GetFrameGenerationFrameTime() const;
	bool IsUpscalingActive() const;

	// Feature interface overrides
	virtual void DrawSettings() override;
	virtual void SaveSettings(json& o_json) override;
	virtual void LoadSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void DataLoaded() override;

	/** @brief Draws the upstream upscaling/frame-generation/Reflex settings tab. */
	void DrawUpscalingSettings();
	/** @brief Draws the DLSS Neural Rendering settings tab. */
	void DrawNeuralRenderingSettings();

	/**
	 * @brief Installs Direct3D-related hooks for device and factory creation.
	 *
	 * Loads FidelityFX support and patches the import address table (IAT) to redirect D3D11 device and DXGI factory creation functions to custom hook implementations.
	**/
	virtual void Load() override;
	virtual void PostPostLoad() override;
	virtual void SetupResources() override;

	UpscaleMethod GetUpscaleMethod() const;

	/**
	 * @brief Flags the render pass geometry's actor ownership for Neural Rendering categories.
	 *
	 * Hooked onto BSLightingShader::SetupGeometry, for every lighting draw that writes Masks2
	 * (the deferred pass and the forward draws between RestoreNeuralRenderingCategories and
	 * FinishNeuralRenderingCategoryCapture). Sets
	 * State::ExtraShaderDescriptors::IsHumanoidActor when the geometry's owning reference is an
	 * actor whose race carries the ActorTypeNPC keyword; Lighting.hlsl maps that flag to
	 * NeuralRenderingCategories::Equipment for everything the skin, hair and eye permutations
	 * did not already claim. Sets State::ExtraShaderDescriptors::IsHair when the geometry is
	 * part of one of the actor's hair or facial-hair head parts (the hairlines, braids and
	 * strands authored with a shader type other than hair tint), or when its material is
	 * hair tint or carries the hair soft-lighting flag (wigs worn as equipment); Lighting.hlsl
	 * maps that to NeuralRenderingCategories::Hair ahead of the technique-derived category.
	 * @param a_pass The render pass being set up.
	 */
	void BSLightingShader_SetupNeuralCategory(RE::BSRenderPass* a_pass);

	void CheckResources(UpscaleMethod a_upscalemethod);
	void CreateUpscalingTextureResources(UpscaleMethod a_upscalemethod);
	void DestroyUpscalingTextureResources(UpscaleMethod a_upscalemethod);

	winrt::com_ptr<ID3D11ComputeShader> encodeTexturesCS[4];  // One for each UpscaleMethod (kNONE, kTAA, kFSR, kDLSS)
	ID3D11ComputeShader* GetEncodeTexturesCS();

	winrt::com_ptr<ID3D11PixelShader> depthRefractionUpscalePS;
	ID3D11PixelShader* GetDepthRefractionUpscalePS();

	winrt::com_ptr<ID3D11PixelShader> underwaterMaskUpscalePS;
	ID3D11PixelShader* GetUnderwaterMaskUpscalePS();

	winrt::com_ptr<ID3D11VertexShader> upscaleVS;
	ID3D11VertexShader* GetUpscaleVS();

	winrt::com_ptr<ID3D11DepthStencilState> upscaleDepthStencilState;
	winrt::com_ptr<ID3D11BlendState> upscaleBlendState;
	winrt::com_ptr<ID3D11RasterizerState> upscaleRasterizerState;

	// Helper: Create a Texture2D matching source format at a given size
	static eastl::unique_ptr<Texture2D> CreateTextureFromSource(ID3D11Resource* src, uint32_t width, uint32_t height,
		bool copyBindFlags = false, bool createSRV = false, bool createUAV = false, const char* name = nullptr);

	void ConfigureTAA();
	void ConfigureUpscaling(RE::BSGraphics::State* a_state);
	void Upscale();

	/**
	 * @brief Returns a Finished Image output texture whose size, format and sample count match
	 * @p a_targetDesc, (re)creating it when they change.
	 * @return nullptr (logged once per format) when the target cannot back a UAV write plus a
	 * CopyResource back into it, e.g. an sRGB, typeless or multisampled target.
	 */
	Texture2D* EnsureNeuralRenderingFinishedImageTexture(const D3D11_TEXTURE2D_DESC& a_targetDesc);

	/**
	 * @brief Snapshots Finished Image's render-resolution guides for this frame.
	 *
	 * Called from PerformUpscaling() before UpscaleDepth(): copies kMAIN depth while it is still
	 * on the same render-resolution, jittered raster as kMOTION_VECTOR and the material-category
	 * snapshot, and records that raster's extent. A no-op unless Finished Image is active on DLSS.
	 */
	void CaptureNeuralRenderingFinishedImageGuides();

	/**
	 * @brief Captures what the vanilla tonemap pass just applied, for the pre-tonemap placements' proxy.
	 *
	 * Called from the HDRTonemapBlendCinematic hook right after the vanilla pass ran. Reads
	 * ISHDR's Param / Cinematic / Tint constants from the pass's pixel constant group and the
	 * adaptation texture (AvgTex) the pass sampled at pixel-shader slot 2. Only valid while the
	 * vanilla tonemap owns the frame; pass null (or another owner) to invalidate the capture.
	 * Consumed one frame later by MakeNeuralRenderingDisplayTransform().
	 * @param a_param The pass's shader parameters, or null when the vanilla pass did not run.
	 */
	void CaptureNeuralRenderingDisplayTransform(RE::ImageSpaceShaderParam* a_param);

	/**
	 * @brief Builds the display transform the scene-linear proxy should replicate this frame.
	 *
	 * Combines the last vanilla capture (exposure and grading, when the vanilla tonemap owns the
	 * frame) with Post Processing's Histogram Auto Exposure (when active). Identity when neither
	 * applies, e.g. under Effects11, which keeps the previous plain-Reinhard proxy.
	 */
	NeuralRendering::DisplayTransform MakeNeuralRenderingDisplayTransform() const;

	// D3D11 textures
	Texture2D* reactiveMaskTexture = nullptr;
	Texture2D* transparencyCompositionMaskTexture = nullptr;
	Texture2D* motionVectorCopyTexture = nullptr;
	Texture2D* sharpenerTexture = nullptr;
	Texture2D* neuralRenderingTexture = nullptr;
	/**
	 * Finished Image Neural Rendering output. Allocated lazily to match the tonemap pass's
	 * output target (kFRAMEBUFFER, or HDR Display's float16 redirect of it) exactly - not kMAIN,
	 * whose format differs, which would make the copy back a silent D3D11 no-op.
	 */
	Texture2D* neuralRenderingFinishedImageTexture = nullptr;
	/** Last target format rejected for Finished Image, so the warning logs once rather than per frame. */
	DXGI_FORMAT neuralRenderingFinishedImageRejectedFormat = DXGI_FORMAT_UNKNOWN;
	/**
	 * kMAIN depth copied by CaptureNeuralRenderingFinishedImageGuides() before UpscaleDepth()
	 * expands it to display resolution. Motion vectors and materialCategoriesSnapshot are never
	 * expanded, so Finished Image needs depth on that same render-resolution, jittered raster.
	 */
	Texture2D* neuralRenderingFinishedImageDepthSnapshot = nullptr;
	/** Render-resolution extent of the captured guides (dynamic resolution is locked off afterwards). */
	uint32_t neuralRenderingFinishedImageGuideWidth = 0;
	uint32_t neuralRenderingFinishedImageGuideHeight = 0;
	/** Set by the capture, consumed by the next Finished Image evaluation - one evaluation per upscaled frame. */
	bool neuralRenderingFinishedImageGuidesReady = false;
	/** Last vanilla tonemap pass inputs captured by CaptureNeuralRenderingDisplayTransform(). */
	struct NeuralRenderingDisplayCapture
	{
		bool valid = false;
		winrt::com_ptr<ID3D11ShaderResourceView> adaptationSRV;  ///< ISHDR AvgTex (x adapted, y target luminance).
		float param[4]{};                                        ///< ISHDR Param.
		float cinematic[4]{};                                    ///< ISHDR Cinematic.
		float tint[4]{};                                         ///< ISHDR Tint.
	} neuralRenderingDisplayCapture;
	/** The first successful capture is logged once so its values can be checked against the game's imagespace. */
	bool neuralRenderingDisplayCaptureLogged = false;
	/**
	 * Masks2's packed material categories: copied right after opaque geometry,
	 * before blended decals can alpha-blend into it and corrupt the category
	 * bits (CaptureNeuralRenderingCategories), then refreshed once the forward
	 * lighting draws have added theirs (FinishNeuralRenderingCategoryCapture).
	 * This is what every Neural Rendering evaluation call reads instead of the
	 * live Masks2.
	 */
	Texture2D* materialCategoriesSnapshot = nullptr;
	/** Resolved once in DataLoaded(); identifies humanoid races for the Equipment category. */
	RE::BGSKeyword* actorTypeNPCKeyword = nullptr;
	bool neuralRenderingResultValid = false;
	bool neuralRenderingResourcesActive = false;
	bool neuralRenderingResetThisFrame = false;
	uint neuralRenderingActivePlacement = UINT_MAX;

	virtual void ClearShaderCache() override;

	// Static instances instead of singletons
	static inline Streamline streamline;
	static inline FidelityFX fidelityFX;  ///< Only for frame generation
	static inline DX12SwapChain dx12SwapChain;
	static inline RCAS rcas;  ///< Standalone RCAS sharpening for DLSS
	static inline NeuralRendering neuralRendering;

	winrt::com_ptr<ID3D11PixelShader> copyDepthToSharedBufferPS;

	float projectionPosScaleX = 0.0f;
	float projectionPosScaleY = 0.0f;

	float dynamicResolutionWidthRatio = 1.0f;
	float dynamicResolutionHeightRatio = 1.0f;

	bool previousUpscalingWasActive = false;
	bool depthUpscaleUseWideKernel = false;

	/**
	 * Set by MenuOpenCloseEventHandler when LoadingMenu closes (cell/worldspace transitions,
	 * initial load). Consumed at the start of Upscale() to reset the DLSS SR and
	 * Neural Rendering temporal histories on the same frame.
	 */
	std::atomic<bool> pendingDLSSReset{ false };

	/**
	 * Set by the Neural Rendering comparison hotkey / menu button. Serviced by
	 * Main_PostProcessing, which drives neuralRenderingCompareStep over four frames
	 * (Neural Rendering forced off, then on, each with a warm-up frame) and grabs the
	 * off/on frames at the end of post-processing - before any HUD or menu is drawn -
	 * into Data/DLSS 5 Screenshots/.
	 */
	std::atomic<bool> neuralRenderingComparePending{ false };
	// Render-thread only. 0 = idle; 1-4 = comparison capture frame (see ServiceNeuralRenderingComparison).
	int neuralRenderingCompareStep = 0;
	bool neuralRenderingCompareUserSetting = false;
	std::string neuralRenderingCompareStamp;

	/**
	 * @brief Builds the placement-independent Neural Rendering options from the current settings.
	 *
	 * Callers still set the guide extent and jitter offset, which depend on where
	 * in the frame the pass runs.
	 */
	NeuralRendering::Options MakeNeuralRenderingOptions() const;

	/**
	 * @brief Evaluates Finished Image Neural Rendering on another feature's composited output.
	 *
	 * Called (via ApplyNeuralRenderingFinishedImage()) after the frame's tonemap has run -
	 * Effects11's, Post Processing's own, or vanilla ISHDR's, whichever owned it that frame - so
	 * the colour this sees is a genuinely finished, display-referred frame rather than the
	 * linear HDR scene colour the Before/After Upscaling placements have to approximate with a
	 * Reinhard proxy. The guides are not on @p a_colorIn's display-resolution, unjittered grid:
	 * motion vectors and the category snapshot stay at render resolution with the frame's TAA
	 * jitter, so depth is the matching snapshot CaptureNeuralRenderingFinishedImageGuides() took
	 * before UpscaleDepth() expanded kMAIN depth, and the guide extent and guide jitter are set
	 * exactly as the After Upscaling placement sets them. The guides are consumed on use, so
	 * the model runs at most once per upscaled frame. The active resolution is read from the same authoritative
	 * globals::game::graphicsState->screenWidth/Height every other Neural Rendering call site
	 * uses - not @p a_colorIn's own GetDesc(), which can legitimately be a larger,
	 * differently-padded allocation than the frame's active region.
	 *
	 * Fails closed: returns false and leaves @p a_colorIn untouched whenever Finished Image is
	 * not the active placement, the backend is unavailable, or a required resource/guide is
	 * missing.
	 *
	 * @param a_colorIn Caller's finished colour for this frame; must be shader-readable.
	 * @param a_colorInSRV SRV over @p a_colorIn.
	 * @param a_colorOut Receives the edited frame; must be UAV-capable and distinct from
	 * @p a_colorIn. Use a texture matching @p a_colorIn's own format so the caller can copy it back.
	 * @return True when the evaluation ran and @p a_colorOut was written.
	 */
	bool EvaluateNeuralRenderingFinishedImage(ID3D11Texture2D* a_colorIn, ID3D11ShaderResourceView* a_colorInSRV,
		ID3D11Texture2D* a_colorOut);

	/**
	 * @brief Applies Finished Image Neural Rendering in place to a game render target.
	 *
	 * Called from PostProcessingExtensions::Main_HDRTonemapBlendCinematic_Render (Hooks.cpp), at
	 * every point that hands the frame's tonemapped colour onward - right after
	 * State::HandlePostProcessing() when Effects11 owns the tonemap, and right after the vanilla
	 * tonemap/passthrough call otherwise (which covers Post Processing owning the tonemap too:
	 * by then the vanilla call has already taken its passthrough branch over Post Processing's
	 * result). Unlike the Before/After/Separate Upscaling placements, Finished Image therefore
	 * does not depend on any one feature owning the tonemap.
	 *
	 * @p a_target is the tonemap pass's output - kFRAMEBUFFER (UNORM in SDR), or HDR Display's
	 * float16 texture while it redirects that slot - never kMAIN, so the edit is written to
	 * neuralRenderingFinishedImageTexture, sized and formatted to match @p a_target, rather
	 * than the kMAIN-format neuralRenderingTexture the other placements use.
	 *
	 * Delegates to EvaluateNeuralRenderingFinishedImage() and, on success, copies the edited
	 * result back into @p a_target so every caller downstream (HUD, menu, Present) sees it
	 * without needing to know Neural Rendering ran. A no-op whenever that call fails closed, or
	 * while a main menu/loading screen is open.
	 *
	 * @param a_target Game render target holding this frame's finished colour.
	 */
	void ApplyNeuralRenderingFinishedImage(RE::RENDER_TARGET a_target);

	/**
	 * @brief Snapshots Masks2's packed material categories before blended decals can touch it.
	 *
	 * Masks2 is deliberately blendable (vertex AO fades under translucent decals), but the
	 * material category packed into its low bits (NeuralRenderingCategories::Pack) is a discrete
	 * value - alpha-blending it produces a meaningless bit pattern, not "the nearer category".
	 * Called from Deferred's blended-decals hook, after opaque geometry but before decals draw.
	 * First of three steps; see RestoreNeuralRenderingCategories() and
	 * FinishNeuralRenderingCategoryCapture() for the forward-stage continuation.
	 */
	void CaptureNeuralRenderingCategories();

	/**
	 * @brief Hands the pre-decal category snapshot back to Masks2 once the deferred composite
	 * has consumed the decal-blended vertex AO, and arms the forward category capture.
	 *
	 * Alpha-blended lighting geometry (hair strands, hairline scalps, translucent clothing) is
	 * sorted and drawn forward, after Deferred::EndDeferred, where Masks2 is normally unbound.
	 * Those draws are what a hair category most often lives in, so BSBatchRenderer_RenderPassImmediately
	 * binds Masks2 back for exactly those draws and they write their category on top of the
	 * restored opaque snapshot. Restoring rather than writing over the live target keeps the
	 * decals' AO contribution to the composite untouched. Called from Deferred::EndDeferred
	 * after DeferredPasses().
	 */
	void RestoreNeuralRenderingCategories();

	/**
	 * @brief Re-snapshots Masks2 after the forward lighting draws and disarms the forward capture.
	 *
	 * Called at the start of Main_PostProcessing, after world and first-person geometry and
	 * before any Neural Rendering evaluation reads the snapshot.
	 */
	void FinishNeuralRenderingCategoryCapture();

	/** True between RestoreNeuralRenderingCategories() and FinishNeuralRenderingCategoryCapture(). */
	bool neuralRenderingForwardCaptureActive = false;

	/** @brief Requests a Neural Rendering on/off comparison screenshot pair; captured over the next few rendered frames. */
	void RequestNeuralRenderingComparisonCapture();
	/**
	 * @brief Drives the two-frame Neural Rendering comparison capture from Main_PostProcessing.
	 * @param a_upscaleMethod The upscale method resolved for this frame.
	 * @param a_framePhaseStart True when called before the frame's upscaling pass (to force the
	 *        Neural Rendering state), false when called after compositing (to queue the screenshot).
	 */
	void ServiceNeuralRenderingComparison(UpscaleMethod a_upscaleMethod, bool a_framePhaseStart);

	void CopySharedD3D12Resources();
	void PostDisplay();
	void PerformUpscaling();
	void UpscaleDepth();

	/**
	 * @brief Applies RCAS sharpening to the main render target after DLSS upscaling.
	 *
	 * Runs in HDR space before tonemapping. Only called when DLSS is active and sharpness > 0.
	 */
	void ApplySharpening();

	static void TimerSleepQPC(int64_t targetQPC);

	void FrameLimiter();

	static double GetRefreshRate(HWND a_window);

	// Unified interface methods - external code should use these instead of direct access
	void LoadUpscalingSDKs();  // Loads all SDKs at once
	HANDLE GetFrameLatencyWaitableObject() const;
	float GetFrameTime() const;

	// Backend interface methods
	bool IsBackendInitialized() const;
	void CheckBackendFeatures(IDXGIAdapter* adapter);
	void UpgradeBackendInterface(void** ppInterface);
	void SetBackendD3DDevice(ID3D11Device* device);
	void PostBackendDevice();

	// Module availability methods
	bool HasFrameGenModule() const;

	// Proxy interface methods
	void SetProxyD3D11Device(ID3D11Device* device);
	void SetProxyD3D11DeviceContext(ID3D11DeviceContext* context);
	void CreateProxySwapChain(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc);
	void CreateProxyInterop();
	IDXGISwapChain* GetProxySwapChain();

	using BlurResources = DX12SwapChain::BlurResources;

	// Get all D3D11 resources needed for background blur when D3D12 swap chain is active
	BlurResources GetBlurResources() const;

private:
	struct Main_UpdateJitter
	{
		static void thunk(RE::BSGraphics::State* a_state);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct MenuManagerDrawInterfaceStartHook
	{
		static void thunk(int64_t a1);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Main_PostProcessing
	{
		static void thunk(RE::ImageSpaceManager* a_this, uint32_t a3, RE::RENDER_TARGET a_target, void* a_4, bool a_5);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	 * Binds Masks2 to SV_Target7 around each forward (post-deferred) lighting draw so it can
	 * write its Neural Rendering category; see RestoreNeuralRenderingCategories().
	 */
	struct BSBatchRenderer_RenderPassImmediately
	{
		static void thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Main_RenderPrecipitation
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSFaceGenManager_UpdatePendingCustomizationTextures
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;
		static bool Register();
	};
};
