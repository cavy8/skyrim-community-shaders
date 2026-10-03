#pragma once

#include <array>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "BodySdf.h"
#include "CardField.h"
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

	/** @brief Mirrors cbuffer CardDraw (b7) in HairStrands/CardLighting.hlsl. */
	struct alignas(16) CardDrawCB
	{
		float3 eyeDelta;  // skinning camera to this pass's camera
		uint32_t boneCount;
		float3 previousEyeDelta;
		float pad0;
		float4 texcoordOffset;  // the material's UV offset (xy) and scale (zw)
	};
	STATIC_ASSERT_ALIGNAS_16(CardDrawCB);
	static_assert(sizeof(CardDrawCB) == 48);

	/** @brief Mirrors GuidePoint in HairStrands/Common.hlsli: one simulated guide point (GPU only). */
	struct GuidePoint
	{
		float4 rotation;
		float3 position;
		float pad0;
		float3 previousPosition;
		float pad1;
		float3 previousPreviousPosition;
		float pad2;
		float3 stepOffset;
		float pad3;
		float3 previousStepOffset;
		float pad4;
		float3 offset;
		float pad5;
		float3 previousOffset;
		float pad6;
	};
	static_assert(sizeof(GuidePoint) == 128);

	inline constexpr uint32_t kMaxColliders = 8;

	/** @brief Mirrors cbuffer SkinCB (b0) in HairStrands/Skinning.hlsli, shared by both strand compute shaders. */
	struct alignas(16) SkinCB
	{
		enum Flags : uint32_t
		{
			kFollow = 1,      // strands follow their simulated guides
			kReset = 2,       // guides restart from their targets
			kCollide = 4,     // guides keep out of the colliders
			kHeadField = 8,   // every strand keeps out of the head field (t4)
			kBodyField = 16,  // every strand keeps off the body's distance field (t5, t6)
			kCardField = 32,  // every strand keeps off the kept cards' field (b1, t7, t8)
		};

		uint32_t pointCount;
		uint32_t boneCount;
		uint32_t pointsPerStrand;
		uint32_t guideCount;

		uint32_t headBone;
		uint32_t flags;
		float simWeight;
		float guidance;

		float3 eyeShift;
		uint32_t steps;

		float3 previousToCurrent;
		float firstStep;

		float stepFraction;
		float displayAlpha;
		float stepTime;
		float teleportDistance;

		// TressFX's simulation settings, in units and per step.
		float damping;
		float localStiffness;
		float globalStiffness;
		float globalRange;

		float gravity;
		float vspCoeff;
		float vspAccelThreshold;
		float clampPositionDelta;

		uint32_t localIterations;
		uint32_t lengthIterations;
		float tipSeparation;
		uint32_t colliderCount;

		float3 headFieldCentre;  // skin space
		float bodyMinClearance;  // units

		float4 wind[4];
		float4 colliders[kMaxColliders * 2];
		// The body's distance field (BodySdf.h), this frame's.
		float4 bodyToGrid[3];  // camera-relative position to grid cells
		float3 bodyGridSize;
		float bodyMaxClearance;  // units
		float3 bodyTexel;
		float bodyTrust;         // units
		float4 bodyRootMove[3];  // a camera-relative point's move with the actor's root over the frame

		uint32_t chainBoneBase;  // palette bones from here on are chain joints (hanging braids)
		uint32_t pad[3];
	};
	STATIC_ASSERT_ALIGNAS_16(SkinCB);
	static_assert(sizeof(SkinCB) == 208 + kMaxColliders * 32 + 144);

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
		bool cardCollision = true;  // strands keep off the cards the hair keeps (braids, ties, buns)
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
		uint32_t bodyActors = 0;          // characters with a collision mesh
		uint32_t bodyTriangles = 0;       // in their collision meshes
		uint32_t bodyFields = 0;          // distance fields built
		uint32_t cardFields = 0;          // kept-card fields built
		uint64_t cardTrianglesDrawn = 0;  // kept as cards (braids, ties, gathered hair), drawn in place of the hidden cards
		uint32_t chainsSimulated = 0;     // hanging braids swinging on chains
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
		/** @brief a_layer's twin (same actor, same mesh counts) if it drew strands this frame or the last, and its geometry (compare only). */
		std::pair<RE::BSGeometry*, Instance*> FindStrandTwin(const Instance& a_layer) const;
		/** @brief True if a_layer's twin (same actor, same mesh counts) drew strands this frame or the last. */
		bool TwinDrawsStrands(const Instance& a_layer) const;
		/**
		 * @brief True if a_layer draws its twin's strands in its own passes: an alpha-tested layer
		 * under blended hair. Blended hair is drawn forward, after the deferred passes, and has no
		 * depth prepass; its alpha-tested layer has both, as alpha-tested hair does.
		 */
		static bool DrawsTwin(const Instance& a_layer, const Instance& a_twin);
		/** @brief True if a_instance drew strands this frame or the last. */
		static bool DrawsStrands(const Instance& a_instance);
		/** @brief The shape that converts a_hosted's hair (the first of its group, same actor, same mesh counts), if seen. */
		Instance* FindGroupHost(const Instance& a_hosted) const;
		/** @brief True if a_hosted's host drew strands this frame or the last. */
		bool HostDrawsStrands(const Instance& a_hosted) const;
		void HideCards(RE::BSRenderPass* a_pass);
		/** @brief The viewport the hidden cards would have used, for the strands drawn in their place. */
		bool GetCardViewport(D3D11_VIEWPORT& o_viewport);
		bool EnsureInstanceBuffers(Instance& a_instance);
		/** @brief The strand shaders for a lighting permutation, compiling them on first request; null until ready. */
		ShaderVariant* GetVariant(uint32_t a_pixelDescriptor);
		/** @brief The strand shaders for a lighting permutation if already compiled, without requesting them. */
		ShaderVariant* FindVariant(uint32_t a_pixelDescriptor);
		/**
		 * @brief The shaders that draw the cards a hair keeps (CardLighting.hlsl) in a lighting
		 * permutation, compiling them on first request; null until ready.
		 */
		ShaderVariant* GetCardVariant(uint32_t a_pixelDescriptor);
		/** @brief The kept cards' depth prepass pixel shader (CardDepth.hlsl), compiling it on first request; null until ready. */
		ID3D11PixelShader* GetCardDepthShader();
		/**
		 * @brief Steps the chains a hair's hanging braids swing on, on the strands' clock, and
		 * appends their joints to this frame's palette rows as bones after the skin instance's.
		 * @param io_palette This frame's skin-to-world rows (absolute translations), a_bones bones.
		 * @return The palette's bone count with the chains.
		 */
		uint32_t SimulateChains(Instance& a_instance, RE::NiSkinInstance* a_skin, std::vector<float4>& io_palette, uint32_t a_bones, const float3& a_eye);
		/** @brief Draws the cards a hair keeps, with the pass's state; a_depthOnly: their alpha-tested depth only. */
		void DrawCards(Instance& a_instance, const D3D11_VIEWPORT* a_viewport, bool a_depthOnly);
		/** @brief The shader once compiled (null until then); requests the compile on first call. a_failure is logged if it fails. */
		winrt::com_ptr<ID3D11ComputeShader> EnsureComputeShader(ComputeShader& a_slot, const wchar_t* a_path, const char* a_entry, const char* a_failure);
		bool EnsureSkinShader();
		/** @brief LOD and skinning, once per rendered frame; true if the hair draws strands this frame. */
		bool PrepareStrands(Instance& a_instance, RE::BSGeometry* a_geometry, RE::NiSkinInstance* a_skin);
		bool UpdateLod(Instance& a_instance, RE::BSGeometry* a_geometry);
		bool Skin(Instance& a_instance, RE::NiSkinInstance* a_skin);

		/** @brief What a simulated hair asks of its actor's body field this frame. */
		struct BodyRequest
		{
			bool wanted = false;  // the actor has a collision mesh (no bone capsules were gathered)
			float3 centre;        // camera-relative: the hair's skull centre (with a head bone)
			float reach = 0.0f;   // units from it the hair can reach; 0 without a head bone
		};

		/**
		 * @brief Fills the simulation part of a_cb for this frame.
		 * @param a_palette This frame's skin-to-world rows, absolute translations.
		 * @param o_body    Whether (and how far round the head) to build the body field.
		 * @return false if the hair is not simulated this frame (plain skinning).
		 */
		bool PrepareSimulation(Instance& a_instance, RE::NiSkinInstance* a_skin, const std::vector<float4>& a_palette, const float3& a_eye, const float3& a_previousEye, SkinCB& o_cb, BodyRequest& o_body);
		/**
		 * @brief Builds a_instance's head field from its actor's head mesh (the Face head part),
		 * once per asset. Leaves it empty, and the head sphere in use, if the actor has no
		 * readable head mesh skinned to the hair's head bone.
		 * @param a_skin The skin instance of the hair being drawn (its head bone's bind pose).
		 */
		void BuildHeadField(Instance& a_instance, RE::BSGeometry* a_geometry, RE::NiSkinInstance* a_skin);
		/**
		 * @brief Keeps a_instance's actor's collision mesh in step with what it wears (BodyCollision::Update).
		 * Until one is built, the bone capsules stand in.
		 */
		void UpdateBody(Instance& a_instance, RE::BSGeometry* a_geometry, RE::NiSkinInstance* a_skin);
		/** @brief BodySdf.cs.hlsl's kernels once compiled (null members until then). */
		BodySdfPrograms GetBodyPrograms();
		/**
		 * @brief The head sphere (without a head field) and, when the hair hangs from a humanoid head and
		 * a_bodyCapsules is set (its actor has no collision mesh), neck, torso and arm capsules.
		 */
		uint32_t GatherColliders(const Instance& a_instance, const BodySkeleton& a_skeleton, const std::vector<float4>& a_palette, uint32_t a_frameBone, const float3& a_eye, bool a_bodyCapsules, float4* o_colliders) const;
		/** @brief Draws the strands with the bound pass state; a_depthOnly draws depth alone, and only if the pass writes depth. */
		void Draw(Instance& a_instance, ShaderVariant& a_variant, const D3D11_VIEWPORT* a_viewport, bool a_depthOnly);
		ID3D11RasterizerState* GetNoCullState(ID3D11RasterizerState* a_current);
		/** @brief The pass's depth state with writes on and an equal test widened to less/greater-equal. */
		ID3D11DepthStencilState* GetStrandDepthState(ID3D11DepthStencilState* a_current, bool a_reversedDepth);
		/** @brief The pass's depth state with an equal test widened to less/greater-equal, writes as the pass has them. */
		ID3D11DepthStencilState* GetCardDepthState(ID3D11DepthStencilState* a_current, bool a_reversedDepth);
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
		std::unordered_map<uint32_t, std::shared_ptr<ShaderVariant>> cardVariants;
		std::shared_ptr<ShaderVariant> cardDepth;  // pixel shader only

		std::mutex computeShaderMutex;
		ComputeShader skinShader;
		ComputeShader simShader;
		ComputeShader bodySkinShader;  // BodySdf.cs.hlsl's kernels
		ComputeShader bodySplatShader;
		ComputeShader bodyFinalizeShader;

		std::unique_ptr<BodyCollision> bodyCollision;
		std::unique_ptr<CardField> cardField;

		// Simulation clock, advanced once per frame: fixed steps shared by all hair (none while
		// paused), where in the frame they end, and how far the frame is past the last one.
		float frameDeltaTime = 0.0f;
		float simAccumulator = 0.0f;  // time since the last step
		uint32_t frameSteps = 0;
		float firstStepFraction = 0.0f;
		float stepFraction = 0.0f;
		float displayAlpha = 0.0f;
		uint64_t simulationStep = 0;          // steps so far, TressFX's frame count for the wind's swell
		std::array<float4, 4> windCorners{};  // the weather's wind as TressFX's four vectors, before a style's response
		uint32_t nextAssetSerial = 0;

		std::unique_ptr<ConstantBuffer> drawCB;
		std::unique_ptr<ConstantBuffer> skinCB;
		std::unique_ptr<ConstantBuffer> cardCB;
		std::unordered_map<ID3D11RasterizerState*, winrt::com_ptr<ID3D11RasterizerState>> noCullStates;
		std::unordered_map<ID3D11DepthStencilState*, winrt::com_ptr<ID3D11DepthStencilState>> strandDepthStates[2];  // by reversed depth
		std::unordered_map<ID3D11DepthStencilState*, winrt::com_ptr<ID3D11DepthStencilState>> cardDepthStates[2];

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
		uint64_t cardTrianglesThisFrame = 0;
		uint32_t chainsThisFrame = 0;
	};
}
