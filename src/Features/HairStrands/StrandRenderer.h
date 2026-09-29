#pragma once

#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "StrandGenerator.h"
#include "StrandStyle.h"

namespace Strands
{
	/** @brief Mirrors SkinnedPoint in HairStrands/Common.hlsli. */
	struct SkinnedPoint
	{
		float3 position;
		float pad0;
		float3 previousPosition;
		float pad1;
		float3 normal;
		float pad2;
	};
	static_assert(sizeof(SkinnedPoint) == 48);

	/** @brief Mirrors cbuffer StrandDraw (b7) in HairStrands/StrandLighting.hlsl. */
	struct alignas(16) StrandDrawCB
	{
		uint32_t pointsPerStrand;
		uint32_t subdivisions;
		float widthScale;
		float minWidthPerDistance;

		float3 eyeDelta;
		float rootWidth;

		float3 previousEyeDelta;
		float tipWidth;

		float waveAmplitude;
		float waveLength;
		float curlRadius;
		float curlLength;

		float curlStart;
		float frizz;
		float flyaways;
		float curlCoherence;
	};
	STATIC_ASSERT_ALIGNAS_16(StrandDrawCB);
	static_assert(sizeof(StrandDrawCB) == 80);

	/** @brief Mirrors GuidePoint in HairStrands/Common.hlsli: one simulated guide point (GPU only). */
	struct GuidePoint
	{
		float4 rotation;
		float3 position;
		float pad0;
		float3 velocity;
		float pad1;
		float3 target;
		float pad2;
		float3 previousPosition;
		float pad3;
		float3 previousTarget;
		float pad4;
	};
	static_assert(sizeof(GuidePoint) == 96);

	inline constexpr uint32_t kMaxColliders = 8;

	/** @brief Mirrors cbuffer SkinCB (b0) in HairStrands/Skinning.hlsli, shared by both strand compute shaders. */
	struct alignas(16) SkinCB
	{
		enum Flags : uint32_t
		{
			kFollow = 1,   // strands follow their simulated guides
			kReset = 2,    // guides restart from their targets
			kCollide = 4,  // guides keep out of the colliders
		};

		uint32_t pointCount;
		uint32_t boneCount;
		uint32_t pointsPerStrand;
		uint32_t guideCount;

		uint32_t headBone;
		uint32_t flags;
		float simWeight;
		float guidance;

		float3 gravity;
		float stepTime;

		float3 eyeShift;
		float rootStiffness;
		float3 previousEyeShift;
		float tipStiffness;

		float3 wind;
		float bendStiffness;

		float velocityKeep;
		float carry;
		float time;
		float teleportDistance;

		uint32_t iterations;
		uint32_t colliderCount;
		uint32_t steps;
		float swingDamping;

		float4 colliders[kMaxColliders * 2];
	};
	STATIC_ASSERT_ALIGNAS_16(SkinCB);
	static_assert(sizeof(SkinCB) == 128 + kMaxColliders * 32);

	/** @brief Global options the renderer reads every frame (owned by the HairStrands feature). */
	struct RenderSettings
	{
		bool autoConvert = true;
		bool playerOnly = false;
		uint32_t maxActors = 6;
		float densityScale = 1.0f;
		float lodStart = 150.0f;
		float lodEnd = 600.0f;
		float minStrandFraction = 0.15f;
		float minPixelWidth = 0.8f;
		float maxWidthScale = 4.0f;
		uint32_t maxSubdivisions = 4;
		uint32_t maxStrandsPerFrame = 200000;

		bool physics = true;
		float physicsDistance = 400.0f;  // simulation fades out over the last quarter of this
		float smpGuidance = 0.35f;       // how much bone (SMP) motion, beyond the head's, moves the targets
		float windStrength = 1.0f;
		bool collision = true;
	};

	/** @brief What the editor and statistics show for one tracked hair. */
	struct InstanceView
	{
		HairKey key;
		StrandStyle style;
		bool converted = false;
		bool authored = false;
		bool edited = false;  // an unsaved editor override is applied
		bool isPlayer = false;
		bool disabledByStyle = false;
		std::string source;  // the style file, empty when converted automatically
		enum class Status
		{
			Cards,  // not converted
			Waiting,
			Generating,
			Ready,
			Failed
		} status = Status::Cards;
		std::string error;  // why generation failed (log text, not translated)
		uint32_t strands = 0;
		uint32_t pointsPerStrand = 0;
		uint32_t activeStrands = 0;
		uint32_t subdivisions = 0;
		float distance = 0.0f;
	};

	struct RenderStats
	{
		uint32_t trackedHair = 0;
		uint32_t convertedHair = 0;
		uint32_t drawnHair = 0;
		uint32_t assets = 0;
		uint32_t pendingJobs = 0;
		uint64_t strandsDrawn = 0;
		uint64_t gpuBytes = 0;
		uint32_t simulatedHair = 0;
		uint64_t guidesSimulated = 0;
	};

	/**
	 * Owns everything the strands need at runtime: the hair it has seen, the generated
	 * assets (shared by every actor wearing the same hair and style), the per-actor skinned
	 * buffers, the strand Lighting shader variants and the draw injection.
	 *
	 * Game objects are only ever read inside the Lighting and Utility shaders' SetupGeometry
	 * and RestoreGeometry for the pass being drawn, when they are guaranteed alive; everything
	 * cached between frames is keyed by pointer but never dereferenced outside those calls.
	 */
	class StrandRenderer
	{
	public:
		explicit StrandRenderer(StyleLibrary& a_library);
		~StrandRenderer();

		/** @brief Once per frame before the world renders: job completion, eviction, budget. */
		void BeginFrame(const RenderSettings& a_settings);

		/** @brief After the game's BSLightingShader::SetupGeometry: skins strands, hides the cards they replace. */
		void OnSetupGeometry(RE::BSRenderPass* a_pass);
		/** @brief Before the game's BSLightingShader::RestoreGeometry: draws the pass's strands. */
		void OnRestoreGeometry(RE::BSRenderPass* a_pass);
		/**
		 * @brief After the game's BSUtilityShader::SetupGeometry: skins the strands and hides the
		 * cards they replace; passes that write depth (the depth prepass) get the strands' depth,
		 * so the shadow mask and other screen-space passes see the strands. Shadow maps keep the cards.
		 */
		void OnUtilitySetupGeometry(RE::BSRenderPass* a_pass);
		/**
		 * @brief After the game's BSEffectShader::SetupGeometry: hides effect shaders drawn over
		 * hair that draws strands (magic effect membranes, overlays such as dirt and blood). They
		 * follow the cards' geometry, which the strands replace.
		 */
		void OnEffectSetupGeometry(RE::BSRenderPass* a_pass);

		/** @brief Drops every compiled shader so edited HLSL is recompiled. */
		void ClearShaders();
		/** @brief Re-resolves every hair's style (style files reloaded or settings changed). */
		void InvalidateStyles();
		/** @brief Frees all GPU and CPU state (feature turned off). */
		void Reset();
		/** @brief Forgets which geometry is hair so it is classified again (keeps generated assets). */
		void ForgetInstances();

		std::vector<InstanceView> GetInstances() const;
		RenderStats GetStats() const { return stats; }

		/** @brief Applies an unsaved style to every actor wearing this hair (nullopt clears it). */
		void SetStyleOverride(const HairKey& a_key, std::optional<StrandStyle> a_style);
		std::optional<StrandStyle> GetStyleOverride(const HairKey& a_key) const;

	private:
		struct Asset;
		struct Instance;
		struct ShaderVariant;

		/** @brief One compute shader, compiled on first request. */
		struct ComputeShader
		{
			winrt::com_ptr<ID3D11ComputeShader> shader;
			bool requested = false;
			bool failed = false;
			uint32_t generation = 0;  // bumped by ClearShaders, so a stale compile is dropped
		};

		Instance* FindOrCreateInstance(RE::BSRenderPass* a_pass, RE::BSGeometry* a_geometry);
		void Classify(Instance& a_instance, RE::BSRenderPass* a_pass, RE::BSGeometry* a_geometry);
		void ResolveStyle(Instance& a_instance);
		std::shared_ptr<Asset> RequestAsset(Instance& a_instance, RE::BSRenderPass* a_pass, RE::BSGeometry* a_geometry);
		/** @brief True if a_layer's twin (same actor, same mesh counts) drew strands this frame or the last. */
		bool TwinDrawsStrands(const Instance& a_layer) const;
		/** @brief True if a_instance drew strands this frame or the last. */
		static bool DrawsStrands(const Instance& a_instance);
		void HideCards(RE::BSRenderPass* a_pass);
		/** @brief The viewport the hidden cards would have used, for the strands drawn in their place. */
		bool GetCardViewport(D3D11_VIEWPORT& o_viewport);
		bool EnsureInstanceBuffers(Instance& a_instance);
		/** @brief The strand shaders for a lighting permutation, compiling them on first request; null until ready. */
		ShaderVariant* GetVariant(uint32_t a_pixelDescriptor);
		/** @brief The strand shaders for a lighting permutation if already compiled, without requesting them. */
		ShaderVariant* FindVariant(uint32_t a_pixelDescriptor);
		/** @brief The shader once compiled (null until then); requests the compile on first call. a_failure is logged if it fails. */
		winrt::com_ptr<ID3D11ComputeShader> EnsureComputeShader(ComputeShader& a_slot, const wchar_t* a_path, const char* a_failure);
		bool EnsureSkinShader();
		/** @brief LOD and skinning, once per rendered frame; true if the hair draws strands this frame. */
		bool PrepareStrands(Instance& a_instance, RE::BSGeometry* a_geometry, RE::NiSkinInstance* a_skin);
		bool UpdateLod(Instance& a_instance, RE::BSGeometry* a_geometry);
		bool Skin(Instance& a_instance, RE::NiSkinInstance* a_skin);
		/**
		 * @brief Fills the simulation part of a_cb for this frame.
		 * @param a_palette This frame's skin-to-world rows, absolute translations.
		 * @return false if the hair is not simulated this frame (plain skinning).
		 */
		bool PrepareSimulation(Instance& a_instance, RE::NiSkinInstance* a_skin, const std::vector<float4>& a_palette, const float3& a_eye, const float3& a_previousEye, SkinCB& o_cb);
		/** @brief The head sphere and, when the hair hangs from a humanoid head, neck, torso and arm capsules. */
		uint32_t GatherColliders(const Instance& a_instance, RE::NiSkinInstance* a_skin, const std::vector<float4>& a_palette, uint32_t a_frameBone, const float3& a_eye, float4* o_colliders) const;
		/** @brief Draws the strands with the bound pass state; a_depthOnly draws depth alone, and only if the pass writes depth. */
		void Draw(Instance& a_instance, ShaderVariant& a_variant, const D3D11_VIEWPORT* a_viewport, bool a_depthOnly);
		ID3D11RasterizerState* GetNoCullState(ID3D11RasterizerState* a_current);
		/** @brief The pass's depth state with writes on and an equal test widened to less/greater-equal. */
		ID3D11DepthStencilState* GetStrandDepthState(ID3D11DepthStencilState* a_current, bool a_reversedDepth);
		void RestoreHiddenViewport();

		StyleLibrary& library;
		RenderSettings settings;
		uint32_t frame = 0;
		uint32_t libraryGeneration = 0;

		std::unordered_map<RE::BSGeometry*, std::unique_ptr<Instance>> instances;
		std::unordered_map<std::string, std::shared_ptr<Asset>> assets;
		std::unordered_map<std::string, StrandStyle> overrides;  // by HairKey::ToString()
		std::vector<std::shared_ptr<Asset>> jobQueue;            // waiting for a free worker

		std::mutex variantMutex;
		std::unordered_map<uint32_t, std::shared_ptr<ShaderVariant>> variants;

		std::mutex computeShaderMutex;
		ComputeShader skinShader;
		ComputeShader simShader;

		// Simulation clock: frame time (0 while paused) and the weather's wind, read once per frame.
		float frameDeltaTime = 0.0f;
		float simulationTime = 0.0f;
		float3 frameWind;
		uint32_t nextAssetSerial = 0;

		std::unique_ptr<ConstantBuffer> drawCB;
		std::unique_ptr<ConstantBuffer> skinCB;
		std::unordered_map<ID3D11RasterizerState*, winrt::com_ptr<ID3D11RasterizerState>> noCullStates;
		std::unordered_map<ID3D11DepthStencilState*, winrt::com_ptr<ID3D11DepthStencilState>> strandDepthStates[2];  // by reversed depth

		// The pass between OnSetupGeometry and OnRestoreGeometry.
		RE::BSRenderPass* currentPass = nullptr;
		Instance* currentInstance = nullptr;
		ShaderVariant* currentVariant = nullptr;
		bool currentDepthOnly = false;  // a Utility pass: the strands' depth, if it writes depth
		bool cardsHidden = false;
		D3D11_VIEWPORT savedViewport{};
		bool loggedViewportMiss = false;
		std::unordered_set<uint32_t> loggedDepthPasses;  // Utility descriptors that drew strand depth

		RenderStats stats;
		uint64_t strandsThisFrame = 0;
		uint32_t drawnThisFrame = 0;
		uint32_t simulatedThisFrame = 0;
		uint64_t guidesThisFrame = 0;
	};
}
